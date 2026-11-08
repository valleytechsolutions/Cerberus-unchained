#pragma once
#include "wardrive_upload_config.h"
#include "esp_err.h"

/* NVS must already be initialized by normal device startup. These functions
 * never initialize/erase NVS, connect a radio, upload, or log credential bytes.
 * Missing settings load as empty. Existing flash configuration determines
 * at-rest protection; this module does not configure encryption/eFuses. */
esp_err_t cerberus_upload_settings_load(cerberus_upload_config_t *out);
esp_err_t cerberus_upload_settings_save(const cerberus_upload_config_t *config);
esp_err_t cerberus_upload_settings_forget(void);
