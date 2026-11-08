#pragma once

#include "wifi_capture_plan.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CERBERUS_WIFI_SAE_MAX_PEERS 16U
#define CERBERUS_WIFI_SAE_FRAME_MAX 4096U

enum {
    CERBERUS_WIFI_SAE_SHORT_BODY = 1U,
    CERBERUS_WIFI_SAE_OPAQUE_BODY = 2U,
    CERBERUS_WIFI_SAE_KNOWN_GROUP = 4U,
    CERBERUS_WIFI_SAE_RETRY_MARKED = 8U,
};

typedef struct {
    uint64_t observed_us; /* C5 callback clock; not delivery time or UTC. */
    uint32_t sequence;
    uint8_t channel;
    int8_t rssi;
} cerberus_wifi_sae_observation_t;

typedef struct {
    uint32_t received, observations, commits, h2e, sae_pk, token_requests;
    uint32_t group_rejections, other_status, retry_marked, short_bodies;
    uint32_t opaque_bodies, unknown_groups, malformed, ignored;
    uint32_t sequence_rejected, clock_rejected, scope_rejected, peer_dropped;
    uint8_t count;
    bool saturated;
} cerberus_wifi_sae_summary_t;

/* A structurally observed transaction-1 header, not a validated SAE commit.
 * No scalar, element, token, password identifier or confirm bytes are kept. */
typedef struct {
    uint8_t bssid[6], transmitter[6], receiver[6];
    uint64_t first_us, last_us;
    uint32_t sequence, first_sequence, observations, retry_marked;
    uint32_t commits, h2e, sae_pk, token_requests, group_rejections, other_status;
    uint16_t status, group, frame_length, body_length, mac_sequence;
    uint8_t channel, flags;
    int8_t rssi;
    bool group_present, rssi_valid;
} cerberus_wifi_sae_record_t;

typedef struct {
    cerberus_wifi_sae_summary_t summary;
    cerberus_wifi_sae_record_t records[CERBERUS_WIFI_SAE_MAX_PEERS];
    cerberus_wifi_capture_plan_t plan;
    uint8_t bssid[6], channel;
    uint64_t last_us;
    uint32_t last_sequence;
    bool initialized, planned, filter_bssid, have_time;
} cerberus_wifi_sae_table_t;

/* The caller proves capture generation. Scope is copied and immutable;
 * planned capture has channel zero and no separate single-BSSID filter. */
bool cerberus_wifi_sae_init(cerberus_wifi_sae_table_t *, uint8_t channel,
    bool filter_bssid, const uint8_t bssid[6], const cerberus_wifi_capture_plan_t *plan);
/* Stable admission order, at most 16 address pairs. New RF retries are counted
 * observations; repeated capture sequences never refresh a row. Returns the
 * updated row index, or -1 if only summary counters changed. */
int cerberus_wifi_sae_observe(cerberus_wifi_sae_table_t *,
    const cerberus_wifi_sae_observation_t *, const uint8_t *frame, size_t length);
bool cerberus_wifi_sae_copy(const cerberus_wifi_sae_table_t *, uint8_t index,
    cerberus_wifi_sae_record_t *);
