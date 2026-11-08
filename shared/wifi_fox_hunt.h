#pragma once

#include "cerberus_protocol.h"

#define CERBERUS_WIFI_FOX_HUNT_MIN_FIRMWARE 83U
#define CERBERUS_WIFI_FOX_HUNT_LOST_MS 1500U
#define CERBERUS_WIFI_FOX_HUNT_DWELL_MS 250U
#define CERBERUS_WIFI_FOX_HUNT_BAND_2G 1U
#define CERBERUS_WIFI_FOX_HUNT_BAND_5G 2U
#define CERBERUS_WIFI_FOX_HUNT_BAND_BOTH 3U
#define CERBERUS_WIFI_FOX_HUNT_BOUND 1U
#define CERBERUS_WIFI_FOX_HUNT_HELD 2U
#define CERBERUS_WIFI_FOX_HUNT_CANCELLED 4U
#define CERBERUS_WIFI_FOX_HUNT_RECEIVING 8U
#define CERBERUS_WIFI_FOX_HUNT_SEARCHING 16U
#define CERBERUS_WIFI_FOX_HUNT_CHANNELS_SKIPPED 32U
#define CERBERUS_WIFI_FOX_HUNT_SWEEPS_SATURATED 64U
#define CERBERUS_WIFI_FOX_HUNT_RSSI_UNKNOWN (-128)

typedef struct __attribute__((packed)) {
    uint64_t session_id;
    uint8_t target[6], initial_channel, search_band;
} cerberus_wifi_fox_hunt_start_t;

/* Session IDs share the Monitor/Analyzer epoch. INFO with a different owner
 * returns only the epoch, HELD and INVALID_STATE. Unbound Status/Stop echoes
 * the requested ID with an error; no other fields grant authority.
 * State uses CERBERUS_WIFI_PACKET_MONITOR_{IDLE,RUNNING,ERROR}.
 * current_channel is zero while RX is not admitted. A successful target
 * callback starts a 1500 ms hold; initial reception gets the same grace.
 * Search uses >=250 ms successful-channel dwells. CHANNELS_SKIPPED makes a
 * nonfatal SDK tuning error explicit; an ERROR state is always terminal.
 * sweeps counts completed search passes and saturates rather than wrapping. */
typedef struct __attribute__((packed)) {
    uint64_t session_id;
    int32_t error_code;
    uint32_t latest_sequence;
    uint16_t sweeps;
    uint8_t state, current_channel, flags, last_target_channel;
} cerberus_wifi_fox_hunt_status_t;

/* Latest successful valid-RSSI callback whose transmitter address matched.
 * sequence counts these callbacks, never wraps, and is not a packet rate.
 * observed_ms is the low 32 bits of C5 callback uptime (not S3 time or UTC).
 * age_ms is ceil(elapsed microseconds / 1000); UINT32_MAX means unknown age.
 * A bound session with no observation returns sequence=observed_ms=channel=0,
 * RSSI_UNKNOWN and age_ms=UINT32_MAX. Cached scan RSSI is never substituted. */
typedef struct __attribute__((packed)) {
    uint64_t session_id;
    uint32_t sequence, age_ms, observed_ms;
    int8_t rssi;
    uint8_t channel;
} cerberus_wifi_fox_hunt_signal_t;

/* SIGNAL failures use this shorter payload under the original command/token. */
typedef struct __attribute__((packed)) {
    uint64_t session_id;
    int32_t error_code;
} cerberus_wifi_fox_hunt_signal_error_t;

static inline bool cerberus_wifi_fox_hunt_channel_valid(uint8_t channel)
{
    return (channel >= 1U && channel <= 14U) ||
        (channel >= 36U && channel <= 64U && channel % 4U == 0U) ||
        (channel >= 100U && channel <= 144U && channel % 4U == 0U) ||
        (channel >= 149U && channel <= 177U && (channel - 149U) % 4U == 0U);
}

static inline bool cerberus_wifi_fox_hunt_start_valid(const cerberus_wifi_fox_hunt_start_t *request)
{
    if (!request || !request->session_id || !request->search_band ||
        (request->search_band & ~CERBERUS_WIFI_FOX_HUNT_BAND_BOTH) ||
        !cerberus_wifi_fox_hunt_channel_valid(request->initial_channel) ||
        !(request->search_band & (request->initial_channel <= 14U ?
            CERBERUS_WIFI_FOX_HUNT_BAND_2G : CERBERUS_WIFI_FOX_HUNT_BAND_5G)) ||
        (request->target[0] & 1U)) return false;
    uint8_t any = 0U;
    for (size_t i = 0; i < sizeof(request->target); ++i) any |= request->target[i];
    return any != 0U;
}

_Static_assert(sizeof(cerberus_wifi_fox_hunt_start_t) == 16U, "Fox Hunt start wire size");
_Static_assert(sizeof(cerberus_wifi_fox_hunt_status_t) == 22U, "Fox Hunt status wire size");
_Static_assert(sizeof(cerberus_wifi_fox_hunt_signal_t) == 22U, "Fox Hunt signal wire size");
_Static_assert(sizeof(cerberus_wifi_fox_hunt_signal_error_t) == 12U, "Fox Hunt error wire size");
