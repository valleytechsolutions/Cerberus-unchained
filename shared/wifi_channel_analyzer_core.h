#pragma once

#include "wifi_channel_analyzer.h"
#include "wifi_packet_monitor_core.h"
#include "wifi_scan_observation.h"

typedef struct __attribute__((packed)) {
    uint32_t sequence, frames, window_us;
    uint8_t channel, flags;
} cerberus_wifi_channel_analyzer_window_t;

typedef struct {
    cerberus_wifi_channel_analyzer_window_t samples[CERBERUS_WIFI_CHANNEL_ANALYZER_SAMPLES];
    uint32_t latest_sequence;
    uint8_t stored_count;
} cerberus_wifi_channel_analyzer_ring_t;

static inline bool cerberus_wifi_channel_analyzer_push(cerberus_wifi_channel_analyzer_ring_t *ring,
    uint32_t frames, uint32_t window_us, uint8_t channel, uint8_t flags)
{
    if (!ring || !window_us || !cerberus_wifi_packet_monitor_channel_valid(channel) ||
        (flags & ~CERBERUS_WIFI_CHANNEL_ANALYZER_SATURATED) || ring->latest_sequence==UINT32_MAX)
        return false;
    uint32_t sequence=++ring->latest_sequence;
    ring->samples[sequence % CERBERUS_WIFI_CHANNEL_ANALYZER_SAMPLES]=
        (cerberus_wifi_channel_analyzer_window_t){sequence,frames,window_us,channel,flags};
    if (ring->stored_count<CERBERUS_WIFI_CHANNEL_ANALYZER_SAMPLES) ++ring->stored_count;
    return true;
}

static inline bool cerberus_wifi_channel_analyzer_get(const cerberus_wifi_channel_analyzer_ring_t *ring,
    uint32_t sequence, cerberus_wifi_channel_analyzer_window_t *out)
{
    if (!ring || !out || !sequence || sequence>ring->latest_sequence ||
        ring->latest_sequence-sequence>=ring->stored_count) return false;
    *out=ring->samples[sequence % CERBERUS_WIFI_CHANNEL_ANALYZER_SAMPLES];
    return out->sequence==sequence;
}

static inline bool cerberus_wifi_channel_analyzer_beacon(const uint8_t *frame, size_t bytes,
    uint8_t channel, int rssi, cerberus_wifi_channel_analyzer_hint_t *out)
{
    cerberus_wifi_observation_frame_t parsed;
    if (!out || !frame || !bytes || frame[0]!=0x80U || rssi < -127 || rssi > 20 ||
        !cerberus_wifi_observation_decode(frame,bytes,channel,&parsed)) return false;
    cerberus_wifi_channel_analyzer_hint_t hint={0};
    memcpy(hint.bssid,parsed.key.bssid,sizeof(hint.bssid));
    memcpy(hint.ssid,parsed.key.ssid,parsed.key.ssid_length);
    hint.ssid_length=parsed.key.ssid_length;
    hint.rssi=(int8_t)rssi;hint.channel=channel;hint.flags=CERBERUS_WIFI_CHANNEL_ANALYZER_HINT_VALID;
    *out=hint;
    return true;
}

_Static_assert(sizeof(cerberus_wifi_channel_analyzer_window_t)==14U,"Analyzer window size");
