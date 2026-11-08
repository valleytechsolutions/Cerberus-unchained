#include "wardrive_upload_settings.h"
#include "nvs.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
static char console_output[512];
static void console_printf(const char *format,...)
{va_list args;va_start(args,format);int n=vsnprintf(console_output,sizeof(console_output),format,args);va_end(args);assert(n>0&&(size_t)n<sizeof(console_output));}
#include "upload_profile_impl.h"

static unsigned calls, closes, fail_at;
static bool exists, pending, erase_pending, opened;
static nvs_open_mode_t mode;
static uint8_t stored[512], staged[512];
static size_t stored_size = 512;
static bool fault(void) { return ++calls == fail_at; }
esp_err_t nvs_open(const char *name, nvs_open_mode_t requested, nvs_handle_t *handle)
{
    assert(!strcmp(name, "ward_upload") && !opened);
    if (fault()) return ESP_FAIL;
    if (requested == NVS_READONLY && !exists) return ESP_ERR_NVS_NOT_FOUND;
    mode = requested; opened = true; *handle = 7; return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *data, size_t *length)
{
    assert(opened && handle == 7 && !strcmp(key, "config"));
    if (fault()) return ESP_FAIL;
    if (!exists) return ESP_ERR_NVS_NOT_FOUND;
    if (*length < stored_size) { *length = stored_size; return ESP_ERR_NVS_INVALID_LENGTH; }
    *length = stored_size; memcpy(data, stored, stored_size); return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t length)
{
    assert(opened && mode == NVS_READWRITE && handle == 7 && !strcmp(key, "config") && length == 512);
    if (fault()) return ESP_FAIL;
    memcpy(staged, data, length); pending = true; return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    assert(opened && mode == NVS_READWRITE && handle == 7 && !strcmp(key, "config"));
    if (fault()) return ESP_FAIL;
    if (!exists) return ESP_ERR_NVS_NOT_FOUND;
    erase_pending = true; return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(opened && mode == NVS_READWRITE && handle == 7);
    if (fault()) return ESP_FAIL;
    if (pending) { memcpy(stored, staged, 512); exists = true; stored_size = 512; }
    if (erase_pending) { memset(stored, 0, 512); exists = false; }
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle)
{ assert(opened && handle == 7); opened = pending = erase_pending = false; ++closes; }

static bool zero(const void *data, size_t n)
{ const unsigned char *p = data; while (n--) if (*p++) return false; return true; }
static void reset_calls(unsigned failure) { assert(!opened); calls = closes = 0; fail_at = failure; }

int main(int argc, char **argv)
{
    assert(argc == 2);
    cerberus_upload_config_t c = {0}, d;
    assert(cerberus_upload_config_invalid(&c) == CERBERUS_UPLOAD_FIELD_NONE);
    assert(!cerberus_upload_config_wifi_ready(&c));
    assert(!cerberus_upload_config_service_ready(&c, CERBERUS_UPLOAD_WIGLE));
    uint8_t wire[512], bad[512];
    assert(cerberus_upload_config_encode(&c, wire));
    FILE *f = fopen(argv[1], "wb"); assert(f); assert(fwrite(wire, 1, sizeof(wire), f) == sizeof(wire)); assert(fclose(f) == 0);
    strcpy(c.ssid, "Synthetic lab AP"); strcpy(c.password, "fixture-passphrase");
    strcpy(c.wigle_name, "fixture-api-name"); strcpy(c.wigle_token, "fixture-token");
    memset(c.wdgwars_token, 'a', 64); strcpy(c.wardrift_token, "fixture-device-key"); c.destinations = 7;
    assert(cerberus_upload_config_wifi_ready(&c));
    for (unsigned i = 0; i < 3; ++i) assert(cerberus_upload_config_service_ready(&c, (cerberus_upload_service_t)i));
    assert(!cerberus_upload_config_service_ready(&c, (cerberus_upload_service_t)99));
    assert(cerberus_upload_config_encode(&c, wire));
    assert(cerberus_upload_config_decode(wire, sizeof(wire), &d)); assert(!memcmp(&c, &d, sizeof(c)));
    for (unsigned i = 0; i < 512; ++i) for (unsigned bit = 0; bit < 8; ++bit) {
        memcpy(bad, wire, 512); bad[i] ^= (uint8_t)(1U << bit); memset(&d, 0xA5, sizeof(d));
        assert(!cerberus_upload_config_decode(bad, 512, &d)); assert(zero(&d, sizeof(d)));
    }
    for (unsigned n = 0; n < 512; ++n) {
        memset(&d, 0xA5, sizeof(d)); assert(!cerberus_upload_config_decode(wire, n, &d)); assert(zero(&d, sizeof(d)));
    }
    uint8_t alias[1024]; memset(alias, 0xCC, sizeof(alias)); memcpy(alias, wire, 512);
    assert(!cerberus_upload_config_decode(alias, 512, (cerberus_upload_config_t *)(void *)(alias + 1)));
    assert(!memcmp(alias, wire, 512));
    assert(!cerberus_upload_config_encode(&c, (uint8_t *)(void *)&c));
    c.password[63] = 'X'; /* bytes beyond the first NUL must not persist */
    assert(cerberus_upload_config_encode(&c, wire)); assert(cerberus_upload_config_decode(wire, 512, &d)); assert(d.password[63] == 0);
    c = d;
#define BAD_FIELD(field, text, expected) do { d = c; memset(d.field, 0, sizeof(d.field)); strcpy(d.field, text); assert(cerberus_upload_config_invalid(&d) == expected); } while (0)
    BAD_FIELD(password, "short", CERBERUS_UPLOAD_FIELD_PASSWORD);
    BAD_FIELD(wigle_name, "wrong:colon", CERBERUS_UPLOAD_FIELD_WIGLE_NAME);
    BAD_FIELD(wigle_token, "token\r\nHeader: value", CERBERUS_UPLOAD_FIELD_WIGLE_TOKEN);
    BAD_FIELD(wardrift_token, "contains space", CERBERUS_UPLOAD_FIELD_WARDRIFT_TOKEN);
    BAD_FIELD(wdgwars_token, "too-short", CERBERUS_UPLOAD_FIELD_WDGWARS_TOKEN);
    BAD_FIELD(ssid, "control\n", CERBERUS_UPLOAD_FIELD_SSID);
    d = c; memset(d.wdgwars_token, 'G', 64); assert(cerberus_upload_config_invalid(&d) == CERBERUS_UPLOAD_FIELD_WDGWARS_TOKEN);
    d = c; memset(d.wigle_name, 'x', sizeof(d.wigle_name)); assert(cerberus_upload_config_invalid(&d) == CERBERUS_UPLOAD_FIELD_WIGLE_NAME);
    d = c; memset(d.ssid, 'x', sizeof(d.ssid)); assert(cerberus_upload_config_invalid(&d) == CERBERUS_UPLOAD_FIELD_SSID);
    d = c; memset(d.password, 'f', 64); d.password[64] = 0; assert(cerberus_upload_config_invalid(&d) == CERBERUS_UPLOAD_FIELD_NONE);
    d.password[0] = 'z'; assert(cerberus_upload_config_invalid(&d) == CERBERUS_UPLOAD_FIELD_PASSWORD);
    d = c; d.destinations = 8; assert(cerberus_upload_config_invalid(&d) == CERBERUS_UPLOAD_FIELD_DESTINATIONS);
    d = c; memset(d.password, 0, sizeof(d.password)); assert(cerberus_upload_config_wifi_ready(&d)); /* explicit open network */
    memset(&d, 0xCC, sizeof(d)); reset_calls(0);
    assert(cerberus_upload_settings_load(&d) == ESP_OK && zero(&d, sizeof(d)) && !opened);
    assert(cerberus_upload_settings_save(&c) == ESP_OK);
    assert(cerberus_upload_settings_load(&d) == ESP_OK && !memcmp(&c, &d, sizeof(c)));
    print_upload_profile();assert(strstr(console_output,"upload_profile_home_ready=1"));
    assert(!strstr(console_output,c.ssid)&&!strstr(console_output,c.password)&&!strstr(console_output,c.wigle_name)&&
        !strstr(console_output,c.wigle_token)&&!strstr(console_output,c.wdgwars_token)&&!strstr(console_output,c.wardrift_token));
    for (unsigned failure = 1; failure <= 3; ++failure) {
        reset_calls(failure); assert(cerberus_upload_settings_save(&c) != ESP_OK); assert(!opened);
        assert(closes == (failure == 1 ? 0U : 1U));
    }
    for (unsigned failure = 1; failure <= 2; ++failure) {
        reset_calls(failure); memset(&d, 0xCC, sizeof(d)); assert(cerberus_upload_settings_load(&d) != ESP_OK);
        assert(!opened && zero(&d, sizeof(d)));
    }
    reset_calls(0); stored[20] ^= 1; assert(cerberus_upload_settings_load(&d) == ESP_ERR_INVALID_RESPONSE);
    assert(zero(&d, sizeof(d))); stored[20] ^= 1;
    stored_size = 513; assert(cerberus_upload_settings_load(&d) == ESP_ERR_NVS_INVALID_LENGTH); assert(zero(&d, sizeof(d))); stored_size = 512;
    for (unsigned failure = 1; failure <= 3; ++failure) { reset_calls(failure); assert(cerberus_upload_settings_forget() != ESP_OK); assert(!opened); }
    reset_calls(0); assert(cerberus_upload_settings_forget() == ESP_OK); assert(!exists);
    assert(cerberus_upload_settings_forget() == ESP_OK);
    assert(cerberus_upload_settings_load(&d) == ESP_OK && zero(&d, sizeof(d)));
    reset_calls(0); c.destinations = 255;
    assert(cerberus_upload_settings_save(&c) == ESP_ERR_INVALID_ARG && !calls);
    cerberus_upload_config_clear(&c, sizeof(c)); assert(zero(&c, sizeof(c)));
    puts("Upload configuration: wire corruption/limits, secret clearing and actual NVS adapter failure paths passed");
    return 0;
}
