#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CERBERUS_UPLOAD_CONFIG_WIRE_BYTES 512U
typedef enum { CERBERUS_UPLOAD_WIGLE, CERBERUS_UPLOAD_WDGWARS,
    CERBERUS_UPLOAD_WARDRIFT, CERBERUS_UPLOAD_SERVICE_COUNT } cerberus_upload_service_t;
typedef enum { CERBERUS_UPLOAD_FIELD_NONE, CERBERUS_UPLOAD_FIELD_SSID,
    CERBERUS_UPLOAD_FIELD_PASSWORD, CERBERUS_UPLOAD_FIELD_WIGLE_NAME,
    CERBERUS_UPLOAD_FIELD_WIGLE_TOKEN, CERBERUS_UPLOAD_FIELD_WDGWARS_TOKEN,
    CERBERUS_UPLOAD_FIELD_WARDRIFT_TOKEN, CERBERUS_UPLOAD_FIELD_DESTINATIONS } cerberus_upload_field_t;
typedef struct {
    char ssid[33], password[65], wigle_name[65], wigle_token[129];
    char wdgwars_token[65], wardrift_token[129];
    uint8_t destinations;
} cerberus_upload_config_t;

/* Empty credentials are valid unconfigured settings, never ready to upload.
 * The codec is canonical, versioned and corruption-checked, not encrypted.
 * No function logs a value. Callers clear scratch copies after use. Invalid
 * decode clears output except overlapping ranges, which remain unchanged. */
cerberus_upload_field_t cerberus_upload_config_invalid(const cerberus_upload_config_t *config);
bool cerberus_upload_config_wifi_ready(const cerberus_upload_config_t *config);
bool cerberus_upload_config_service_ready(const cerberus_upload_config_t *config,
    cerberus_upload_service_t service);
bool cerberus_upload_config_encode(const cerberus_upload_config_t *config,
    uint8_t out[CERBERUS_UPLOAD_CONFIG_WIRE_BYTES]);
bool cerberus_upload_config_decode(const uint8_t *bytes, size_t length,
    cerberus_upload_config_t *out);
void cerberus_upload_config_clear(void *bytes, size_t length);
const char *cerberus_upload_field_name(cerberus_upload_field_t field);
const char *cerberus_upload_service_name(cerberus_upload_service_t service);
