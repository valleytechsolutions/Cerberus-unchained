#pragma once

/* Passive AP/client header evidence. No ESP, allocation or RF operations.
 * Callers serialize commits, channel accounting and snapshot copies. Decode
 * outside the receive lock, then revalidate the original admission token. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "wifi_relation_observation.h"

#define CERBERUS_AP_SURVEY_AP_CAPACITY 32U
#define CERBERUS_AP_SURVEY_RELATION_CAPACITY 32U
#define CERBERUS_AP_SURVEY_DWELL_MS 250U
#define CERBERUS_AP_SURVEY_MAX_FRAME 2304U

enum {
    CERBERUS_AP_SURVEY_AP_RSSI_VALID = 1U,
    CERBERUS_AP_SURVEY_AP_BEACON = 2U,
    CERBERUS_AP_SURVEY_AP_PROBE_RESPONSE = 4U,
};
enum {
    CERBERUS_AP_SURVEY_SNAPSHOT_RECEIVING = 1U,
    CERBERUS_AP_SURVEY_SNAPSHOT_SATURATED = 2U,
    CERBERUS_AP_SURVEY_SNAPSHOT_EXHAUSTED = 4U,
};
enum {
    CERBERUS_AP_SURVEY_REJECTED,
    CERBERUS_AP_SURVEY_AP,
    CERBERUS_AP_SURVEY_RELATION,
};

typedef struct __attribute__((packed)) {
    uint8_t bssid[6], ssid[32], ssid_length, channel;
    int8_t rssi;
    uint8_t flags;
    uint16_t capabilities; /* Advertised bits, not an inferred auth mode. */
    uint32_t incarnation, frames;
    uint64_t first_callback_us, last_callback_us;
} cerberus_ap_survey_ap_t;

typedef struct __attribute__((packed)) {
    cerberus_wifi_relation_row_t row;
    uint32_t incarnation;
} cerberus_ap_survey_relation_t;

typedef struct __attribute__((packed)) {
    uint64_t session_id, copied_source_us;
    uint64_t requested_channels, effective_channels, attempted_channels, applied_channels;
    uint64_t receive_us;
    uint32_t revision, ap_frames, relation_frames, rejected_frames, stale_frames;
    uint32_t ap_replacements, relation_replacements, received_frames, saturations;
    uint32_t completed_passes, tune_failures;
    uint8_t ap_count, relation_count, channel, flags;
    cerberus_ap_survey_ap_t aps[CERBERUS_AP_SURVEY_AP_CAPACITY];
    cerberus_ap_survey_relation_t relations[CERBERUS_AP_SURVEY_RELATION_CAPACITY];
} cerberus_ap_survey_snapshot_t;

typedef struct {
    uint8_t kind;
    cerberus_ap_survey_ap_t ap;
    cerberus_wifi_relation_row_t relation;
} cerberus_ap_survey_decoded_t;

typedef struct {
    cerberus_ap_survey_snapshot_t live;
    uint64_t armed_us, latest_callback_us, arm_token, control_us;
    uint32_t incarnation_counter;
    bool accepting;
} cerberus_ap_survey_table_t;

/* Exact BSSID+RX-channel and BSSID+STA+RX-channel keys. Full tables replace
 * the least recently observed row, with a new non-reused incarnation and an
 * explicit replacement count. An unchanged key never moves its slot. */
bool cerberus_ap_survey_begin(cerberus_ap_survey_table_t *, uint64_t session_id,
    uint64_t requested_channels, uint64_t effective_channels);
bool cerberus_ap_survey_arm(cerberus_ap_survey_table_t *, uint64_t session_id,
    uint64_t arm_token, uint8_t channel, uint64_t source_us);
bool cerberus_ap_survey_disarm(cerberus_ap_survey_table_t *, uint64_t session_id,
    uint64_t arm_token, uint64_t source_us);
bool cerberus_ap_survey_note_channel(cerberus_ap_survey_table_t *, uint8_t channel,
    bool applied);
void cerberus_ap_survey_note_pass(cerberus_ap_survey_table_t *);

/* Returns REJECTED for unsupported or malformed frames; output is cleared.
 * Input excludes FCS and must already have passed native RX-error checks. */
uint8_t cerberus_ap_survey_decode(const uint8_t *, size_t, uint8_t channel,
    int rssi, uint64_t callback_us, cerberus_ap_survey_decoded_t *);
bool cerberus_ap_survey_commit(cerberus_ap_survey_table_t *, uint64_t session_id,
    uint64_t arm_token, uint8_t channel, uint64_t callback_us,
    const cerberus_ap_survey_decoded_t *);
/* Bounded copy of internally committed state; validate the copied image after
 * releasing the receive lock, never do quadratic identity checks in it. */
bool cerberus_ap_survey_snapshot(const cerberus_ap_survey_table_t *, uint64_t source_us,
    cerberus_ap_survey_snapshot_t *);
bool cerberus_ap_survey_ap_valid(const cerberus_ap_survey_ap_t *, uint64_t source_us);
bool cerberus_ap_survey_snapshot_valid(const cerberus_ap_survey_snapshot_t *);

#ifdef __cplusplus
static_assert(sizeof(cerberus_ap_survey_ap_t) == 68U, "survey AP ABI");
static_assert(sizeof(cerberus_ap_survey_relation_t) == 48U, "survey relation ABI");
static_assert(sizeof(cerberus_ap_survey_snapshot_t) == 3816U, "survey snapshot ABI");
#else
_Static_assert(sizeof(cerberus_ap_survey_ap_t) == 68U, "survey AP ABI");
_Static_assert(sizeof(cerberus_ap_survey_relation_t) == 48U, "survey relation ABI");
_Static_assert(sizeof(cerberus_ap_survey_snapshot_t) == 3816U, "survey snapshot ABI");
#endif
