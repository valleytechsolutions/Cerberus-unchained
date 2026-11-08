#undef NDEBUG
#include "wardrive_geofence.h"
#include "wardrive_stream.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
enum { ESP_OK, ESP_FAIL, ESP_ERR_INVALID_STATE, ESP_ERR_INVALID_ARG, ESP_ERR_NVS_NOT_FOUND, ESP_ERR_NOT_SUPPORTED };
static bool settings_ready, exists, mismatch;
static int settings_handle;
static unsigned writes, commits, reads;
static esp_err_t write_error, commit_error, read_error;
static size_t stored_size;
static cerberus_geofence_config_t stored;
static esp_err_t nvs_set_blob(int handle, const char *key, const void *data, size_t size)
{
    assert(handle == settings_handle && !strcmp(key, "geofence_v1") && size == sizeof(stored));
    ++writes;
    if (write_error) return write_error;
    memcpy(&stored, data, size); stored_size = size; exists = true; return ESP_OK;
}
static esp_err_t nvs_commit(int handle)
{ assert(handle == settings_handle); ++commits; return commit_error; }
static esp_err_t nvs_get_blob(int handle, const char *key, void *data, size_t *size)
{
    assert(handle == settings_handle && !strcmp(key, "geofence_v1")); ++reads;
    if (read_error) return read_error;
    if (!exists) return ESP_ERR_NVS_NOT_FOUND;
    assert(*size >= sizeof(stored)); memcpy(data, &stored, sizeof(stored)); *size = stored_size;
    if (mismatch) ((cerberus_geofence_config_t *)data)->areas[0].latitude += 1.0;
    return ESP_OK;
}
#include "geofence_settings_impl.h"

#define CERBERUS_WARDRIVE_STREAM_HOST
typedef struct { unsigned firmware_version, capabilities, radio_state; } cerberus_c5_status_t;
static cerberus_c5_status_t c5_fixture = { .firmware_version = CERBERUS_WD_MIN_FIRMWARE, .capabilities = 3 };
static unsigned radio_reads;
static esp_err_t c5_spi_link_get_status(cerberus_c5_status_t *out)
{ ++radio_reads; *out = c5_fixture; return ESP_OK; }
#include "geofence_admission_impl.h"

int main(void)
{
    cerberus_geofence_config_t got, config = cerberus_geofence_defaults();
    assert(cerberus_settings_geofences_get(&got) == ESP_ERR_INVALID_STATE);
    assert(cerberus_settings_geofences_set(&config) == ESP_ERR_INVALID_STATE && !writes);
    settings_ready = true; load_geofence_settings();
    assert(cerberus_settings_geofences_get(NULL) == ESP_ERR_INVALID_ARG);
    assert(cerberus_settings_geofences_get(&got) == ESP_OK && !memcmp(&got, &config, sizeof(got)));
    config.areas[0].enabled = 1; strcpy(config.areas[0].name, "SYNTHETIC");
    config.areas[0].latitude = 12.0; config.areas[0].longitude = -42.0;
    assert(cerberus_settings_geofences_set(&config) == ESP_OK && writes == 1 && commits == 1);
    assert(cerberus_settings_geofences_get(&got) == ESP_OK && !memcmp(&got, &config, sizeof(got)));
    memset(&geofence_settings, 0, sizeof(geofence_settings)); load_geofence_settings();
    assert(cerberus_settings_geofences_get(&got) == ESP_OK && !memcmp(&got, &config, sizeof(got)));
    config.areas[1].latitude = NAN;
    assert(cerberus_settings_geofences_set(&config) == ESP_ERR_INVALID_ARG && writes == 1);
    assert(cerberus_settings_geofences_get(&got) == ESP_OK); config.areas[1].latitude = 0;
    write_error = ESP_FAIL;
    assert(cerberus_settings_geofences_set(&config) == ESP_FAIL && commits == 1);
    assert(cerberus_settings_geofences_get(&got) == ESP_FAIL); write_error = 0;
    commit_error = ESP_FAIL;
    assert(cerberus_settings_geofences_set(&config) == ESP_FAIL);
    assert(cerberus_settings_geofences_get(&got) == ESP_FAIL); commit_error = 0;
    read_error = ESP_FAIL;
    assert(cerberus_settings_geofences_set(&config) == ESP_FAIL);
    assert(cerberus_settings_geofences_get(&got) == ESP_FAIL); read_error = 0;
    mismatch = true;
    assert(cerberus_settings_geofences_set(&config) == ESP_ERR_INVALID_STATE); mismatch = false;
    assert(cerberus_settings_geofences_set(&config) == ESP_OK);
    stored_size--;
    load_geofence_settings(); assert(cerberus_settings_geofences_get(&got) == ESP_ERR_INVALID_STATE);
    stored_size++; stored.version++;
    load_geofence_settings(); assert(cerberus_settings_geofences_get(&got) == ESP_ERR_INVALID_STATE);
    stored = config; stored.areas[0].longitude = INFINITY;
    load_geofence_settings(); assert(cerberus_settings_geofences_get(&got) == ESP_ERR_INVALID_STATE);
    config = cerberus_geofence_defaults(); assert(cerberus_settings_geofences_set(&config) == ESP_OK);
    load_geofence_settings(); assert(cerberus_settings_geofences_get(&got) == ESP_OK);
    assert(!memcmp(&got, &config, sizeof(got)));
    assert(admission(true) == ESP_OK); /* All areas off retains expansion use. */
    config.areas[0].enabled = 1; strcpy(config.areas[0].name, "SYNTHETIC");
    assert(cerberus_settings_geofences_set(&config) == ESP_OK);
    unsigned before = radio_reads;
    assert(admission(true) == ESP_ERR_NOT_SUPPORTED && radio_reads == before);
    assert(admission(false) == ESP_ERR_NOT_SUPPORTED); /* Base streaming support cannot pause. */
    c5_fixture.firmware_version = CERBERUS_WD_PAUSE_MIN_FIRMWARE;
    assert(admission(false) == ESP_OK);
    geofence_settings_error = ESP_FAIL; before = radio_reads;
    assert(admission(false) == ESP_FAIL && radio_reads == before);
    geofence_settings_error = ESP_OK; geofence_settings.version++;
    assert(admission(false) == ESP_ERR_INVALID_STATE && radio_reads == before);
    printf("Geofence NVS: validation, readback, restart, corrupt records and failure recovery passed (%u reads).\n", reads);
}
