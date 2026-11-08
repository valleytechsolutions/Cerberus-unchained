#pragma once

#include "wifi_capture_plan.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CERBERUS_WIFI_FRAME_MAX_EVENTS 16U
#define CERBERUS_WIFI_FRAME_MAX_LENGTH 4096U

typedef struct {
    uint64_t observed_us; /* C5 callback clock, not UTC or S3 time. */
    uint32_t sequence;
    uint8_t channel;
    int8_t rssi;
} cerberus_wifi_frame_observation_t;

typedef struct {
    uint32_t received, management, data, control, other, malformed;
    uint32_t beacons, probe_requests, probe_responses, deauth, disassoc;
    uint32_t eapol_headers, retries, scope_rejected, sequence_rejected, clock_rejected;
    uint32_t event_dropped, event_replaced;
    uint64_t bytes, first_us, last_us;
    uint32_t sequence;
    int8_t rssi, min_rssi, max_rssi;
    uint8_t channel, count;
    bool rssi_valid, extrema_valid, saturated;
} cerberus_wifi_frame_summary_t;

/* Bounded public header evidence, not a device identity or attack attribution.
 * A tuple includes channel, subtype and reason visibility; matching RF retries
 * are new observations. A new tuple at capacity replaces the least recently
 * observed slot. first_sequence distinguishes each slot admission. No frame
 * payload, key material or tokens are retained. */
typedef struct {
    uint8_t bssid[6], transmitter[6], receiver[6];
    uint8_t subtype, channel;
    uint16_t reason;
    int8_t rssi;
    bool rssi_valid, reason_known, protected_frame, fragmented;
    uint32_t frames, retries, first_sequence, sequence;
    uint64_t first_us, last_us;
} cerberus_wifi_frame_record_t;

typedef struct {
    bool delivered;
    int8_t event_index; /* -1 unless this observation updates a retained row. */
} cerberus_wifi_frame_result_t;

typedef struct {
    cerberus_wifi_frame_summary_t summary;
    cerberus_wifi_frame_record_t records[CERBERUS_WIFI_FRAME_MAX_EVENTS];
    cerberus_wifi_capture_plan_t plan;
    uint8_t bssid[6], channel, mode_mask;
    uint64_t last_us;
    uint32_t last_sequence;
    bool initialized, have_time, planned, filter_bssid;
} cerberus_wifi_frame_model_t;

/* Copies its complete scope. Exact RAW, PROBE or DEAUTH mode only. PROBE
 * scopes are channel-only, including plans derived from selected AP channels. */
bool cerberus_wifi_frame_init(cerberus_wifi_frame_model_t *, uint8_t channel,
    uint8_t mode_mask, bool filter_bssid, const uint8_t bssid[6],
    const cerberus_wifi_capture_plan_t *plan);
/* Complete CRC-verified frame, without FCS. Caller proves generation. Replayed
 * transport sequences and source-clock regressions never refresh delivery. */
cerberus_wifi_frame_result_t cerberus_wifi_frame_observe(cerberus_wifi_frame_model_t *,
    const cerberus_wifi_frame_observation_t *, const uint8_t *, size_t);
bool cerberus_wifi_frame_copy(const cerberus_wifi_frame_model_t *, uint8_t index,
    cerberus_wifi_frame_record_t *);
