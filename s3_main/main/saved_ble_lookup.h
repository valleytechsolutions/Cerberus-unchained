#pragma once

#include "dual_ble_scan.h"
#include "s3_ble_discovery.h"
#include "s3_ble_discovery_session.h"

#define CERBERUS_SAVED_BLE_CANDIDATES 8U
#define CERBERUS_SAVED_BLE_FRESH_MS 5000U

/* A saved address is a search selector, never a current measurement or an
 * authenticated identity. Sources and actual observed address types remain
 * separate. These copies grant no raw-advertisement or connection authority. */
typedef struct {
    cerberus_c5_ble_result_t peer;
    uint64_t run_id, boot_id, epoch, anchor_ms;
    uint32_t age_at_anchor_ms, age_ms;
    uint8_t source; /* 1: S3 sweep-peak RSSI; 2: C5 original-ADV RSSI. */
    bool fresh;
} cerberus_saved_ble_lookup_candidate_t;

typedef struct {
    bool attached, receiving;
    int32_t error;
    uint32_t sweeps;
} cerberus_saved_ble_lookup_source_t;

typedef struct {
    bool enabled, stopping;
    uint8_t count;
    cerberus_saved_ble_lookup_source_t source[2];
} cerberus_saved_ble_lookup_view_t;

/* UI-owned optional PSRAM store. Reuses the existing bounded reader and local
 * discovery controller; only four native staging rows, no second inventory. */
typedef struct {
    cerberus_dual_ble_scan_t c5;
    cerberus_ble_discovery_session_t s3;
    cerberus_s3_ble_discovery_record_t staging[4];
    cerberus_saved_ble_lookup_candidate_t candidates[CERBERUS_SAVED_BLE_CANDIDATES];
    uint64_t run_id, last_tick_ms, s3_epoch, s3_revision;
    uint32_t c5_snapshots;
    uint8_t address[6], seen_mask;
    int32_t c5_begin_error;
    bool enabled, have_clock;
} cerberus_saved_ble_lookup_t;

void cerberus_saved_ble_lookup_init(cerberus_saved_ble_lookup_t *, const cerberus_dual_ble_ops_t *);
/* Caller drains other owners before Start. Start is explicit and clears prior
 * candidates; it cannot interrupt cleanup or adopt an existing attachment. */
bool cerberus_saved_ble_lookup_start(cerberus_saved_ble_lookup_t *, const uint8_t address[6]);
void cerberus_saved_ble_lookup_stop(cerberus_saved_ble_lookup_t *);
void cerberus_saved_ble_lookup_tick(cerberus_saved_ble_lookup_t *, uint64_t now_ms);
bool cerberus_saved_ble_lookup_released(const cerberus_saved_ble_lookup_t *);
void cerberus_saved_ble_lookup_view(const cerberus_saved_ble_lookup_t *, uint64_t now_ms,
    cerberus_saved_ble_lookup_view_t *);
/* Compact stable source/type order. Copies and paints never refresh age. */
bool cerberus_saved_ble_lookup_candidate(const cerberus_saved_ble_lookup_t *, uint8_t index,
    uint64_t now_ms, cerberus_saved_ble_lookup_candidate_t *);
/* Validate the exact painted candidate before requesting Stop and handing
 * its typed address to Fox Hunt. Wait for released() before opening Fox Hunt. */
bool cerberus_saved_ble_lookup_accepts(const cerberus_saved_ble_lookup_t *,
    const cerberus_saved_ble_lookup_candidate_t *, uint64_t now_ms);
