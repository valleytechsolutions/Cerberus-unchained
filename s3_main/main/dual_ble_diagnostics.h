#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Cached aggregates only: no address, name, advertising bytes, peer pointer,
 * C5 boot identity or selected-device identifier crosses this boundary. */
typedef struct {
    uint32_t reports, age_ms;
    int8_t rssi;
    bool target_matches, seen, fresh;
} cerberus_dual_ble_sample_diagnostics_t;
typedef struct {
    uint64_t sequence, published_ms;
    /* Counters reset at an explicit public discovery/track begin, not at an
     * automatic RF sweep rearm. Publications/first-result latency describe
     * discovery only; neither measures packet count or RF observation time. */
    uint32_t c5_sweeps, c5_snapshots, c5_first_result_ms;
    uint32_t c5_inventory_replaced, c5_inventory_dropped;
    int32_t c5_error, s3_scan_error, s3_track_error, fox_error;
    cerberus_dual_ble_sample_diagnostics_t s3, c5;
    uint8_t version, c5_phase, c5_state, c5_generation, c5_rows, c5_read_index;
    uint8_t s3_owner, s3_state;
    int8_t requested_mode; /* -1 off, 0 discovery, 1 tracking, 2 Analyzer, 3 family. */
    /* Desired UI family, controller's retained request and published bank are
     * separate scopes during drain/rebind. Zero means no focused family; the
     * latter two values do not establish current ownership or freshness. */
    uint8_t requested_family, c5_requested_family, c5_published_family;
    bool available, clock_valid, sequence_saturated, awake, allocated;
    bool enabled, attached, receiving, reading, stopping, released, extended_configured;
    bool c5_has_first_result, c5_live_updates;
    bool c5_inventory_known, c5_inventory_replacement_enabled;
    bool s3_initialized, s3_synced, s3_scan_cached;
    bool s3_discovery_enabled, s3_discovery_attached, s3_discovery_owned;
    bool fox_open, fox_pending, fox_stopping;
} cerberus_dual_ble_diagnostics_t;

_Static_assert(sizeof(cerberus_dual_ble_diagnostics_t) <= 128U, "bounded BLE diagnostic snapshot");

/* Single UI writer. sampled_ms is S3 diagnostic publication uptime, not RF
 * time. The module assigns sequence/clock fields, retains a nondecreasing
 * clock watermark and marks negative/backward clocks invalid. No allocation,
 * peripheral access, scanner calls or lease changes. NULL clears payload. */
void cerberus_dual_ble_diagnostics_publish(const cerberus_dual_ble_diagnostics_t *, int64_t sampled_ms);
/* One fixed-size value copy under a short critical section; NULL is a no-op. */
void cerberus_dual_ble_diagnostics_get(cerberus_dual_ble_diagnostics_t *);
