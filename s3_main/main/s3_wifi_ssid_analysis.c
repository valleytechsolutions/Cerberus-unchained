#include "s3_wifi_ssid_analysis.h"

#include <limits.h>
#include <string.h>

static bool received_length(const cerberus_c5_wifi_result_t *row, size_t *length)
{
    const char *end = memchr(row->ssid, '\0', sizeof(row->ssid));
    if (!end || !row->channel || row->security > CERBERUS_WIFI_SECURITY_OTHER)
        return false;
    *length = (size_t)(end - row->ssid);
    return true;
}

bool cerberus_s3_wifi_ssid_analyze(const cerberus_c5_wifi_result_t *rows,
    size_t count, size_t selected_index, cerberus_s3_wifi_ssid_analysis_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!rows || !count || count > CERBERUS_WIFI_SCAN_MAX_RESULTS || selected_index >= count)
        return false;
    size_t selected_length = 0U;
    const cerberus_c5_wifi_result_t *selected = &rows[selected_index];
    if (!received_length(selected, &selected_length)) return false;
    cerberus_s3_wifi_ssid_analysis_t pending = {0};
    pending.strongest_rssi = INT8_MIN;
    pending.weakest_rssi = INT8_MAX;
    for (size_t i = 0U; i < count; ++i) {
        size_t length = 0U;
        const cerberus_c5_wifi_result_t *row = &rows[i];
        if (!received_length(row, &length)) return false;
        if (!selected_length ? i != selected_index :
            length != selected_length || memcmp(row->ssid, selected->ssid, length))
            continue;
        ++pending.total_bssids;
        pending.security_mask |= (uint16_t)(1U << row->security);
        if (row->channel <= 14U) ++pending.count_24_ghz;
        else ++pending.count_5_ghz;
        if (row->rssi > pending.strongest_rssi) pending.strongest_rssi = row->rssi;
        if (row->rssi < pending.weakest_rssi) pending.weakest_rssi = row->rssi;
        if (memcmp(row->bssid, selected->bssid, sizeof(row->bssid)) &&
            pending.peer_count < sizeof(pending.peers) / sizeof(pending.peers[0]))
            pending.peers[pending.peer_count++] = *row;
    }
    *out = pending;
    return true;
}
