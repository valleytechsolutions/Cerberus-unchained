#pragma once

#include "esp_err.h"
#include "wifi_channel_analyzer.h"
#include "wifi_packet_monitor_core.h"

static inline bool wifi_analyzer_status_valid(const cerberus_wifi_channel_analyzer_status_t *r,
    bool info, uint64_t session)
{
    if (!r || !r->session_id || (!info && r->session_id != session) ||
        (r->flags & ~(CERBERUS_WIFI_CHANNEL_ANALYZER_BOUND | CERBERUS_WIFI_CHANNEL_ANALYZER_HELD |
            CERBERUS_WIFI_CHANNEL_ANALYZER_CANCELLED))) return false;
    if (!(r->flags & CERBERUS_WIFI_CHANNEL_ANALYZER_BOUND)) {
        if (r->latest_sequence || r->target_ms || r->state || r->channel || r->stored_count) return false;
        return info ? ((r->flags == 0U && r->error_code == ESP_OK) ||
            (r->flags == CERBERUS_WIFI_CHANNEL_ANALYZER_HELD && r->error_code == ESP_ERR_INVALID_STATE)) :
            r->flags == 0U && r->error_code != ESP_OK;
    }
    return cerberus_wifi_packet_monitor_channel_valid(r->channel) &&
        r->target_ms == CERBERUS_WIFI_CHANNEL_ANALYZER_TARGET_MS &&
        r->stored_count == (r->latest_sequence < CERBERUS_WIFI_CHANNEL_ANALYZER_SAMPLES ?
            r->latest_sequence : CERBERUS_WIFI_CHANNEL_ANALYZER_SAMPLES) &&
        r->state <= CERBERUS_WIFI_PACKET_MONITOR_ERROR &&
        (r->state != CERBERUS_WIFI_PACKET_MONITOR_RUNNING || (r->flags & CERBERUS_WIFI_CHANNEL_ANALYZER_HELD)) &&
        (r->state != CERBERUS_WIFI_PACKET_MONITOR_IDLE || !(r->flags & CERBERUS_WIFI_CHANNEL_ANALYZER_HELD)) &&
        (r->state != CERBERUS_WIFI_PACKET_MONITOR_ERROR || r->error_code != ESP_OK);
}

static inline bool wifi_analyzer_sample_valid(const cerberus_wifi_channel_analyzer_sample_t *r,
    uint64_t session, uint32_t sequence)
{
    return r && session && sequence && r->session_id == session && r->sequence == sequence &&
        cerberus_wifi_packet_monitor_channel_valid(r->channel) &&
        r->window_us >= CERBERUS_WIFI_CHANNEL_ANALYZER_TARGET_MS * 1000U &&
        !(r->flags & ~CERBERUS_WIFI_CHANNEL_ANALYZER_SATURATED) &&
        (!!(r->flags & CERBERUS_WIFI_CHANNEL_ANALYZER_SATURATED) == (r->frames == UINT32_MAX));
}

static inline bool wifi_analyzer_hint_status_valid(const cerberus_wifi_channel_analyzer_hint_status_t *r,
    uint64_t session)
{
    if (!r || !session || r->session_id != session) return false;
    if (r->error_code != ESP_OK)
        return !r->projection && !r->length && !r->flags && r->age_ms == UINT32_MAX;
    return r->projection && r->length == CERBERUS_WIFI_CHANNEL_ANALYZER_HINT_BYTES &&
        r->flags == CERBERUS_WIFI_CHANNEL_ANALYZER_HINT_VALID;
}

static inline bool wifi_analyzer_hint_valid(const cerberus_wifi_channel_analyzer_hint_t *r)
{
    if (!r || r->flags != CERBERUS_WIFI_CHANNEL_ANALYZER_HINT_VALID || r->ssid_length > 32U ||
        !cerberus_wifi_packet_monitor_channel_valid(r->channel) || r->rssi < -127 || r->rssi > 20 ||
        (r->bssid[0] & 1U)) return false;
    uint8_t any = 0U;
    for (unsigned i = 0; i < 6U; ++i) any |= r->bssid[i];
    if (!any) return false;
    for (unsigned i = r->ssid_length; i < 32U; ++i) if (r->ssid[i]) return false;
    return true;
}

static inline bool wifi_analyzer_hint_part_valid(const cerberus_wifi_channel_analyzer_hint_part_t *r,
    uint64_t session, uint32_t projection, uint8_t part)
{
    if (!r || !session || !projection || part >= CERBERUS_WIFI_CHANNEL_ANALYZER_HINT_PARTS ||
        r->session_id != session || r->projection != projection || r->part != part) return false;
    for (unsigned i = 0; i < CERBERUS_WIFI_CHANNEL_ANALYZER_HINT_PART_BYTES; ++i)
        if (part * CERBERUS_WIFI_CHANNEL_ANALYZER_HINT_PART_BYTES + i >= CERBERUS_WIFI_CHANNEL_ANALYZER_HINT_BYTES &&
            r->bytes[i]) return false;
    return true;
}
