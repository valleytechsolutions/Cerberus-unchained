#pragma once

#include "esp_err.h"
#include "wifi_fox_hunt.h"

static inline bool wifi_fox_target_valid(const uint8_t target[6])
{
    if (!target || (target[0] & 1U)) return false;
    uint8_t any = 0U;
    for (unsigned i = 0U; i < 6U; ++i) any |= target[i];
    return any != 0U;
}

static inline bool wifi_fox_band_channel_valid(uint8_t band, uint8_t channel)
{
    return band && !(band & ~CERBERUS_WIFI_FOX_HUNT_BAND_BOTH) &&
        cerberus_wifi_fox_hunt_channel_valid(channel) &&
        (band & (channel <= 14U ? CERBERUS_WIFI_FOX_HUNT_BAND_2G : CERBERUS_WIFI_FOX_HUNT_BAND_5G));
}

static inline bool wifi_fox_status_valid(const cerberus_wifi_fox_hunt_status_t *r, bool info, uint64_t session)
{
    if (!r || !r->session_id || (!info && r->session_id != session) || (r->flags & 0x80U)) return false;
    if (!(r->flags & CERBERUS_WIFI_FOX_HUNT_BOUND)) {
        if (r->latest_sequence || r->sweeps || r->state || r->current_channel || r->last_target_channel) return false;
        return info ? ((!r->flags && !r->error_code) ||
            (r->flags == CERBERUS_WIFI_FOX_HUNT_HELD && r->error_code == ESP_ERR_INVALID_STATE)) :
            !r->flags && r->error_code != ESP_OK;
    }
    bool held = (r->flags & CERBERUS_WIFI_FOX_HUNT_HELD) != 0U;
    bool receiving = (r->flags & CERBERUS_WIFI_FOX_HUNT_RECEIVING) != 0U;
    if (r->state > CERBERUS_WIFI_PACKET_MONITOR_ERROR ||
        (r->state == CERBERUS_WIFI_PACKET_MONITOR_RUNNING && !held) ||
        (r->state == CERBERUS_WIFI_PACKET_MONITOR_IDLE && held) ||
        (r->state == CERBERUS_WIFI_PACKET_MONITOR_ERROR && !r->error_code) ||
        (r->state != CERBERUS_WIFI_PACKET_MONITOR_ERROR && r->error_code &&
            !(r->flags & CERBERUS_WIFI_FOX_HUNT_CHANNELS_SKIPPED)) ||
        (receiving && (!held || r->state != CERBERUS_WIFI_PACKET_MONITOR_RUNNING)) ||
        (receiving ? !cerberus_wifi_fox_hunt_channel_valid(r->current_channel) : r->current_channel != 0U) ||
        ((r->flags & CERBERUS_WIFI_FOX_HUNT_SEARCHING) && r->state != CERBERUS_WIFI_PACKET_MONITOR_RUNNING) ||
        ((r->flags & CERBERUS_WIFI_FOX_HUNT_SWEEPS_SATURATED) && r->sweeps != UINT16_MAX)) return false;
    return r->latest_sequence ? cerberus_wifi_fox_hunt_channel_valid(r->last_target_channel) :
        r->last_target_channel == 0U;
}

static inline bool wifi_fox_signal_valid(const cerberus_wifi_fox_hunt_signal_t *r, uint64_t session)
{
    if (!r || !session || r->session_id != session) return false;
    if (!r->sequence) return !r->observed_ms && !r->channel &&
        r->rssi == CERBERUS_WIFI_FOX_HUNT_RSSI_UNKNOWN && r->age_ms == UINT32_MAX;
    return r->rssi >= -127 && r->rssi <= 20 && cerberus_wifi_fox_hunt_channel_valid(r->channel);
}
