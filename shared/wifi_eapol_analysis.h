#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "wifi_capture_plan.h"

#define CERBERUS_WIFI_EAPOL_MAX_PEERS 16U
#define CERBERUS_WIFI_EAPOL_MAX_CONTEXTS 16U
#define CERBERUS_WIFI_EAPOL_FRAME_MAX 4096U
#define CERBERUS_WIFI_EAPOL_EXCHANGE_US UINT64_C(15000000)

enum {
    CERBERUS_WIFI_EAPOL_NONE, CERBERUS_WIFI_EAPOL_M1,
    CERBERUS_WIFI_EAPOL_M2, CERBERUS_WIFI_EAPOL_M3, CERBERUS_WIFI_EAPOL_M4,
};

typedef struct {
    uint64_t observed_us; /* Exact C5 callback clock; never S3 delivery time. */
    uint32_t sequence;
    uint8_t channel;
    int8_t rssi;
} cerberus_wifi_eapol_observation_t;

typedef struct {
    uint32_t received, keys, other, malformed, ignored, phase[4], group_keys;
    uint32_t pmkid_frames, pmkid_elements, encrypted_key_data, unknown_layout;
    uint32_t exchanges, retries, sequence_rejected, clock_rejected, scope_rejected;
    uint32_t peer_replaced, peer_dropped;
    uint32_t beacons, beacon_malformed, beacon_dropped;
    uint8_t count, context_count;
    bool saturated;
} cerberus_wifi_eapol_summary_t;

/* Receive evidence only. An exchange is a consistent observed sequence, not
 * MIC verification, successful authentication, or proof of physical identity. */
typedef struct {
    uint8_t bssid[6], station[6];
    uint32_t phase[4], keys, pmkid_frames, pmkid_elements, exchanges, retries, gaps;
    uint64_t first_us, last_us;
    uint32_t sequence, first_sequence;
    uint8_t channel, last_phase;
    int8_t rssi;
    bool rssi_valid;
} cerberus_wifi_eapol_record_t;

/* Independent unauthenticated beacon context. These bytes are not a scan
 * result, selected target, negotiated security suite or station observation. */
typedef struct {
    uint8_t bssid[6], transmitter[6], ssid[32];
    uint8_t ssid_length, channel, advertised_channel;
    uint16_t capabilities;
    int8_t rssi;
    bool rssi_valid, hidden_like;
    uint32_t frames, sequence, first_sequence;
    uint64_t first_us, last_us;
} cerberus_wifi_eapol_context_t;

typedef struct {
    int8_t peer_index, context_index; /* -1 unless a new observation updates it. */
} cerberus_wifi_eapol_result_t;

/* Private correlation state; no retained MIC, PMKID, key material or payload.
 * Publicly transmitted nonces are retained only to avoid combining exchanges. */
typedef struct {
    uint8_t anonce[32], snonce[32];
    uint64_t replay_m1, replay_m3, started_us;
    uint8_t stage, descriptor, version, mic_length, channel;
} cerberus_wifi_eapol_exchange_t;

typedef struct {
    cerberus_wifi_eapol_summary_t summary;
    cerberus_wifi_eapol_record_t records[CERBERUS_WIFI_EAPOL_MAX_PEERS];
    cerberus_wifi_eapol_exchange_t exchange[CERBERUS_WIFI_EAPOL_MAX_PEERS];
    cerberus_wifi_eapol_context_t contexts[CERBERUS_WIFI_EAPOL_MAX_CONTEXTS];
    cerberus_wifi_capture_plan_t plan;
    uint8_t bssid[6], channel;
    uint64_t last_us;
    uint32_t last_sequence;
    bool initialized, have_time, planned, filter_bssid;
} cerberus_wifi_eapol_table_t;

/* No allocation, radio, clock or filesystem calls. The plan is copied. Fixed
 * scopes use one canonical channel and optionally one infrastructure BSSID;
 * planned scopes use channel=0, filter_bssid=false and a 2000 ms EAPOL plan. */
bool cerberus_wifi_eapol_init_options(cerberus_wifi_eapol_table_t *, uint8_t channel,
    bool filter_bssid, const uint8_t bssid[6], const cerberus_wifi_capture_plan_t *plan);
/* Compatibility wrapper for a fixed selected AP. */
bool cerberus_wifi_eapol_init(cerberus_wifi_eapol_table_t *, const uint8_t bssid[6], uint8_t channel);
/* Input is a complete CRC-verified raw 802.11 frame without FCS, at most
 * CERBERUS_WIFI_EAPOL_FRAME_MAX bytes. The caller
 * proves its capture generation before this call. Returns a row index only
 * for a new, accepted key observation; -1 otherwise. Summary counters can
 * change for malformed/other traffic without admitting a station. Duplicate
 * capture sequence never updates source or delivery age. RF retries with a
 * new sequence are new observations, but not a new exchange. */
int cerberus_wifi_eapol_observe(cerberus_wifi_eapol_table_t *,
    const cerberus_wifi_eapol_observation_t *, const uint8_t *frame, size_t length);
cerberus_wifi_eapol_result_t cerberus_wifi_eapol_observe_full(cerberus_wifi_eapol_table_t *,
    const cerberus_wifi_eapol_observation_t *, const uint8_t *frame, size_t length);
bool cerberus_wifi_eapol_copy(const cerberus_wifi_eapol_table_t *, uint8_t index,
    cerberus_wifi_eapol_record_t *);
bool cerberus_wifi_eapol_context_copy(const cerberus_wifi_eapol_table_t *, uint8_t index,
    cerberus_wifi_eapol_context_t *);
