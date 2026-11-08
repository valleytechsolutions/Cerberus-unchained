#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "wifi_ap_survey.h"

/* Local counterpart of the passive AP survey protocol. Requests and replies
 * have the same shapes as C5, including the 54-byte snapshot chunk. The S3
 * accepts only 2.4 GHz plans. A bound HELD reply retains cleanup ownership;
 * neither a timeout nor an error grants permission to start another owner. */
esp_err_t cerberus_s3_wifi_ap_survey_exchange(uint8_t command,
    const void *request, size_t request_size, void *reply, size_t reply_size);
bool cerberus_s3_wifi_ap_survey_active(void);
/* Direct local copy of the live bank. The caller supplies a separate optional
 * PSRAM destination. It is cleared on failure; a read never renews RF lease. */
esp_err_t cerberus_s3_wifi_ap_survey_snapshot(uint64_t session,
    cerberus_ap_survey_snapshot_t *out);
