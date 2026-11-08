#include "wardrive_upload_settings.h"
#include "nvs.h"

#define UPLOAD_NAMESPACE "ward_upload"
#define UPLOAD_KEY "config"

esp_err_t cerberus_upload_settings_load(cerberus_upload_config_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    cerberus_upload_config_clear(out, sizeof(*out));
    nvs_handle_t handle;
    esp_err_t error = nvs_open(UPLOAD_NAMESPACE, NVS_READONLY, &handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (error != ESP_OK) return error;
    uint8_t bytes[CERBERUS_UPLOAD_CONFIG_WIRE_BYTES]; size_t length = sizeof(bytes);
    error = nvs_get_blob(handle, UPLOAD_KEY, bytes, &length);
    nvs_close(handle);
    if (error == ESP_OK && !cerberus_upload_config_decode(bytes, length, out)) error = ESP_ERR_INVALID_RESPONSE;
    if (error == ESP_ERR_NVS_NOT_FOUND) error = ESP_OK;
    cerberus_upload_config_clear(bytes, sizeof(bytes));
    return error;
}

esp_err_t cerberus_upload_settings_save(const cerberus_upload_config_t *config)
{
    uint8_t bytes[CERBERUS_UPLOAD_CONFIG_WIRE_BYTES];
    if (!cerberus_upload_config_encode(config, bytes)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t error = nvs_open(UPLOAD_NAMESPACE, NVS_READWRITE, &handle);
    if (error == ESP_OK) {
        error = nvs_set_blob(handle, UPLOAD_KEY, bytes, sizeof(bytes));
        if (error == ESP_OK) error = nvs_commit(handle);
        nvs_close(handle);
    }
    cerberus_upload_config_clear(bytes, sizeof(bytes));
    return error;
}

esp_err_t cerberus_upload_settings_forget(void)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open(UPLOAD_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) return error;
    error = nvs_erase_key(handle, UPLOAD_KEY);
    if (error == ESP_ERR_NVS_NOT_FOUND) error = ESP_OK;
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    return error;
}
