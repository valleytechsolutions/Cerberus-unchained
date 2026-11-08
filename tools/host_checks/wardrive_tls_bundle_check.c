#include <stdbool.h>
#include "esp_crt_bundle.h"
#include "mbedtls/platform.h"
#include "psa/crypto.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Firmware uses its linked bundle. This test installs a synthetic one first. */
const uint8_t bundle_start[4] __asm__("_binary_x509_crt_bundle_start") = {0};
const uint8_t bundle_end[4] __asm__("_binary_x509_crt_bundle_end") = {0};

static size_t allocations;
static int fail_after = -1;

void *bundle_calloc(size_t count, size_t size)
{
    if (fail_after == 0) return NULL;
    if (fail_after > 0) --fail_after;
    void *result = calloc(count, size);
    if (result) ++allocations;
    return result;
}

void bundle_free(void *pointer)
{
    if (!pointer) return;
    assert(allocations > 0);
    --allocations;
    free(pointer);
}

static int verify(mbedtls_ssl_config *config, mbedtls_x509_crt *chain,
                  const char *hostname, uint32_t *flags)
{
    if (config->MBEDTLS_PRIVATE(f_ca_cb)) {
        return mbedtls_x509_crt_verify_with_ca_cb(chain,
            config->MBEDTLS_PRIVATE(f_ca_cb), config->MBEDTLS_PRIVATE(p_ca_cb),
            &mbedtls_x509_crt_profile_default, hostname, flags,
            config->MBEDTLS_PRIVATE(f_vrfy), config->MBEDTLS_PRIVATE(p_vrfy));
    }
    return mbedtls_x509_crt_verify(chain, config->MBEDTLS_PRIVATE(ca_chain),
        NULL, hostname, flags, config->MBEDTLS_PRIVATE(f_vrfy),
        config->MBEDTLS_PRIVATE(p_vrfy));
}

static void check_file(mbedtls_ssl_config *config, const char *path,
                       const char *hostname, bool accepted, uint32_t reason)
{
    mbedtls_x509_crt chain;
    mbedtls_x509_crt_init(&chain);
    assert(mbedtls_x509_crt_parse_file(&chain, path) == 0);
    uint32_t flags = 0;
    int result = verify(config, &chain, hostname, &flags);
    assert((result == 0) == accepted);
    if (accepted) assert(flags == 0);
    if (reason) assert((flags & reason) == reason);
    mbedtls_x509_crt_free(&chain);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    assert(mbedtls_platform_set_calloc_free(bundle_calloc, bundle_free) == 0);
    assert(psa_crypto_init() == PSA_SUCCESS);
    uint8_t bundle[2048];
    FILE *file = fopen(argv[1], "rb");
    assert(file);
    size_t length = fread(bundle, 1, sizeof(bundle), file);
    assert(feof(file) && length > 4);
    fclose(file);
    assert(esp_crt_bundle_set(bundle, length) == ESP_OK);

    mbedtls_ssl_config config;
    mbedtls_ssl_config_init(&config);
    assert(esp_crt_bundle_attach(&config) == ESP_OK);
#ifdef CHECK_LEGACY
    check_file(&config, "valid.pem", "wardrive.test", false, 0);
    check_file(&config, "direct.pem", "wardrive.test", true, 0);
    puts("Legacy bundle: cross-signed chain rejected; direct chain accepted");
#else
    check_file(&config, "valid.pem", "wardrive.test", true, 0);
    check_file(&config, "direct.pem", "wardrive.test", true, 0);
    check_file(&config, "valid.pem", "wrong.test", false, MBEDTLS_X509_BADCERT_CN_MISMATCH);
    check_file(&config, "expired.pem", "wardrive.test", false, MBEDTLS_X509_BADCERT_EXPIRED);
    check_file(&config, "future.pem", "wardrive.test", false, MBEDTLS_X509_BADCERT_FUTURE);
    check_file(&config, "expired-intermediate.pem", "wardrive.test", false, MBEDTLS_X509_BADCERT_EXPIRED);
    check_file(&config, "future-intermediate.pem", "wardrive.test", false, MBEDTLS_X509_BADCERT_FUTURE);
    check_file(&config, "untrusted.pem", "wardrive.test", false, 0);
    check_file(&config, "tampered.pem", "wardrive.test", false, 0);

    size_t baseline = allocations;
    for (unsigned i = 0; i < 128; ++i) {
        check_file(&config, "valid.pem", "wardrive.test", true, 0);
        assert(allocations == baseline);
    }

    mbedtls_x509_crt chain;
    mbedtls_x509_crt_init(&chain);
    assert(mbedtls_x509_crt_parse_file(&chain, "valid.pem") == 0);
    baseline = allocations;
    for (int i = 0; i < 16; ++i) {
        fail_after = i;
        uint32_t flags = 0;
        int result = verify(&config, &chain, "wardrive.test", &flags);
        fail_after = -1;
        if (result == 0) assert(flags == 0);
        assert(allocations == baseline);
    }
    mbedtls_x509_crt_free(&chain);
    esp_crt_bundle_detach(&config);
    assert(config.MBEDTLS_PRIVATE(f_vrfy) == NULL);
    assert(config.MBEDTLS_PRIVATE(f_ca_cb) == NULL);
    puts("Cross-signed chain accepted; bad names, dates and signatures rejected");
    puts("128 verification cycles and 16 allocation failures released their allocations");
#endif
    mbedtls_ssl_config_free(&config);
    mbedtls_psa_crypto_free();
    assert(allocations == 0);
    return 0;
}
