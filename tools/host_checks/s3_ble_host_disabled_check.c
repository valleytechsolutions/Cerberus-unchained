#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
enum { ESP_OK, ESP_ERR_NOT_SUPPORTED, ESP_ERR_INVALID_ARG };
#include "../../s3_main/main/s3_ble_host.c"
int main(void)
{
    cerberus_s3_ble_memory_diagnostics_t diagnostics;
    memset(&diagnostics, 0xa5, sizeof(diagnostics));
    cerberus_s3_ble_host_memory_diagnostics(&diagnostics);
    const unsigned char *bytes = (const unsigned char *)&diagnostics;
    for (size_t i = 0; i < sizeof(diagnostics); ++i) assert(bytes[i] == 0);
    cerberus_s3_ble_host_memory_diagnostics(NULL);
    assert(cerberus_s3_ble_host_owner() == CERBERUS_S3_BLE_OWNER_NONE);
    assert(!cerberus_s3_ble_host_initialized() && !cerberus_s3_ble_host_synced());
    assert(cerberus_s3_ble_host_epoch() == 0);
    cerberus_s3_ble_host_lease_t lease;
    memset(&lease,0xa5,sizeof(lease));
    assert(cerberus_s3_ble_host_claim(CERBERUS_S3_BLE_OWNER_SCANNER,&lease) == ESP_ERR_NOT_SUPPORTED);
    const cerberus_s3_ble_host_lease_t zero={0};
    assert(!memcmp(&lease,&zero,sizeof(lease)) && !cerberus_s3_ble_host_lease_valid(&lease));
    assert(!cerberus_s3_ble_host_lease_valid(NULL));
    assert(cerberus_s3_ble_host_claim(CERBERUS_S3_BLE_OWNER_SCANNER,NULL)==ESP_ERR_INVALID_ARG);
    assert(cerberus_s3_ble_host_release(&lease) == ESP_ERR_NOT_SUPPORTED);
    puts("Disabled BLE host: no SDK/PSRAM dependency, deterministic empty diagnostics passed.");
    return 0;
}
