#include "sentinel_baseline.h"

#include "settings.h"

#include <string.h>

esp_err_t sentinel_baseline_compare(sentinel_baseline_t *summary,
    const cerberus_c5_wifi_result_t *rows, size_t count, uint8_t generation,
    bool settings_available)
{
    if (summary == NULL) return ESP_ERR_INVALID_ARG;
    memset(summary, 0, sizeof(*summary));
    summary->checked = true;
    if (count > CERBERUS_WIFI_SCAN_MAX_RESULTS) {
        summary->error = ESP_ERR_INVALID_RESPONSE;
        return summary->error;
    }
    if (count && rows == NULL) {
        summary->error = ESP_ERR_INVALID_ARG;
        return summary->error;
    }
    if (!settings_available) {
        summary->error = ESP_ERR_INVALID_STATE;
        return summary->error;
    }
    for (size_t index = 0; index < count; ++index) {
        const cerberus_c5_wifi_result_t *record = &rows[index];
        if (!memchr(record->ssid, '\0', sizeof(record->ssid)) ||
            !record->channel || record->security > CERBERUS_WIFI_SECURITY_OTHER) {
            summary->error = ESP_ERR_INVALID_RESPONSE;
            return summary->error;
        }
    }
    sentinel_baseline_t candidate = *summary;
    candidate.generation = generation;
    candidate.saved = cerberus_settings_wifi_trusted_count();
    for (size_t index = 0; index < count; ++index) {
        const cerberus_c5_wifi_result_t *record = &rows[index];
        switch (cerberus_settings_wifi_trust_state(record->ssid, record->bssid, record->security)) {
        case CERBERUS_WIFI_TRUST_EXACT: ++candidate.exact; break;
        case CERBERUS_WIFI_TRUST_MISMATCH: ++candidate.review; break;
        default: ++candidate.other; break;
        }
        ++candidate.compared;
    }
    candidate.error = ESP_OK;
    *summary = candidate;
    return ESP_OK;
}
