#pragma once

#include "c5_spi_link.h" /* Existing normalized BLE inventory and raw packet shapes. */
#include "s3_ble_snapshot.h"
#include "s3_ble_tracker.h"
#include "ble_analyzer.h"
#include "watch_receive_timing.h"
#include "s3_ble_wardrive.h"
#include "s3_ble_mac_monitor.h"
#include "s3_ble_discovery.h"

/* Local S3 observer. Every sweep is bounded to eight seconds; the caller must
 * consume a completed generation before starting the next one. The default
 * remains passive for Watch, Wardriving, Fox Hunt, and Analyzer. Internally an
 * exact host claim token gates callbacks and release; the public owner label
 * never authorizes cleanup. Source epochs remain valid after a deliberate
 * completed-sweep release, while a replaced held claim invalidates its work. */
esp_err_t cerberus_s3_ble_scanner_start(cerberus_ble_scan_status_payload_t *status);
/* Active 1M scan for the device list. Scan responses can add display fields
 * and separately proven passive hints to an exact same-sweep original report;
 * they never change that advertisement's raw bytes/time or active eligibility. */
esp_err_t cerberus_s3_ble_scanner_start_active(cerberus_ble_scan_status_payload_t *status);
esp_err_t cerberus_s3_ble_scanner_stop(cerberus_ble_scan_status_payload_t *status);
esp_err_t cerberus_s3_ble_scanner_get_status(cerberus_ble_scan_status_payload_t *status);
/* Reusable explicit passive lease for Recon. epoch is the never-reused full
 * capture identity; callback and host epochs also gate its native ownership.
 * Source clocks are S3 uptime, not a replacement for any row observation time. */
typedef struct {
    uint64_t epoch, started_ms, source_now_ms;
    cerberus_ble_scan_status_payload_t scan;
    uint32_t scan_epoch;
    unsigned host_epoch;
    int32_t error;
    bool attached, owned, receiving, stopping;
} cerberus_s3_ble_passive_state_t;

/* 0 attaches a new finite passive sweep. Exact prior epoch rearms only after
 * its prior RF/queues released. A rejected valid-token rearm returns the old
 * attachment; non-OK never means a new sweep was accepted.
 * Non-OK + attached output may describe the prior completed lease or an
 * acquisition whose cleanup still owns resources: retain its token until
 * release/retry. Empty failure output claims no NEW
 * accepted/held lease; it never authorizes adoption of a global scanner. */
esp_err_t cerberus_s3_ble_scanner_passive_start(uint64_t expected_previous_epoch,
    cerberus_s3_ble_passive_state_t *state);
/* Matching faulted/reset attachments still return ESP_OK plus their error and
 * exact owned state; wrong/absent tokens return NOT_FOUND with cleared output. */
esp_err_t cerberus_s3_ble_scanner_passive_get_state(uint64_t epoch,
    cerberus_s3_ble_passive_state_t *state);
esp_err_t cerberus_s3_ble_scanner_passive_stop(uint64_t epoch,
    cerberus_s3_ble_passive_state_t *state);
esp_err_t cerberus_s3_ble_scanner_passive_release(uint64_t epoch);
/* COMPLETE-only scoped copies. Wrong tokens return NOT_FOUND; invalid data,
 * reset/replacement or clocks return INVALID_STATE. Outputs clear together on
 * failure. No allocation/RF and no private bank; each operation holds one lock. */
esp_err_t cerberus_s3_ble_scanner_passive_copy_row(uint64_t epoch, uint8_t index,
    cerberus_s3_ble_discovery_record_t *row, cerberus_s3_ble_passive_state_t *state);
esp_err_t cerberus_s3_ble_scanner_passive_get_watch_status(uint64_t epoch,
    cerberus_watch_scan_status_payload_t *status);
esp_err_t cerberus_s3_ble_scanner_passive_get_watch_result(uint64_t epoch, uint8_t index,
    cerberus_watch_observation_t *result);
esp_err_t cerberus_s3_ble_scanner_passive_get_watch_detailed_result(uint64_t epoch,
    uint8_t index, cerberus_watch_observation_t *result, cerberus_watch_receive_timing_t *timing,
    sentinel_watch_motion_t *motion, cerberus_watch_receive_timing_t *motion_timing);
esp_err_t cerberus_s3_ble_scanner_passive_get_activity(uint64_t epoch,
    cerberus_ble_activity_payload_t *status);
/* Atomic Wardriving bank from an exact accepted passive lease. COMPLETE and
 * released RF/queues are required; the attachment remains until its owner
 * releases or rearms it. Count must equal the bank count and fit capacity<=32.
 * Caller supplies disjoint buffers; NULL rows/evidence are allowed only for
 * zero capacity. All capacity entries and mandatory state clear on failure
 * (clearing is bounded to32 even for invalid capacity). No allocation/RF.
 * Rows, source-specific evidence and the final state share one locked bank. */
esp_err_t cerberus_s3_ble_scanner_passive_copy_wardrive(uint64_t epoch, uint8_t count,
    cerberus_c5_ble_result_t *rows, cerberus_s3_ble_wardrive_evidence_t *evidence,
    size_t capacity, cerberus_s3_ble_passive_state_t *state);
/* Atomic COMPLETE-bank status and local replacement count for this exact
 * generation. Both outputs are mandatory and cleared on failure. Replacement
 * discards retained inventory, while dropped_reports counts rejected reports;
 * neither is an RF packet-loss measurement. No RF or allocation is performed. */
esp_err_t cerberus_s3_ble_scanner_get_inventory_status(uint8_t generation,
    cerberus_ble_scan_status_payload_t *status, uint16_t *replacements);
esp_err_t cerberus_s3_ble_scanner_get_result(uint8_t generation, uint8_t index,
    cerberus_c5_ble_result_t *result);
/* One existing mutex protects the entire completed bank and its evidence.
 * Count must equal stored_count, and count <= capacity <= MAX_RESULTS. All
 * supplied capacity entries are cleared on failure (bounded to MAX_RESULTS
 * even for an invalid capacity). Null buffers are allowed only at capacity 0.
 * This getter never starts RF, allocates storage or changes source times. */
esp_err_t cerberus_s3_ble_scanner_copy_wardrive(uint8_t generation, uint8_t count,
    cerberus_c5_ble_result_t *rows, cerberus_s3_ble_wardrive_evidence_t *evidence,
    size_t capacity);
esp_err_t cerberus_s3_ble_scanner_get_advertisement(uint8_t generation, uint8_t index,
    cerberus_c5_ble_advertisement_t *result);
/* Time of that exact raw ADV, not the time a completed row was opened. */
esp_err_t cerberus_s3_ble_scanner_get_advertisement_time(uint8_t generation,
    uint8_t index, uint32_t *observed_ms);
esp_err_t cerberus_s3_ble_scanner_get_focus(uint8_t generation, uint8_t index,
    cerberus_s3_ble_focus_t *result);
esp_err_t cerberus_s3_ble_scanner_get_watch_status(uint8_t generation,
    cerberus_watch_scan_status_payload_t *status);
esp_err_t cerberus_s3_ble_scanner_get_watch_result(uint8_t generation, uint8_t index,
    cerberus_watch_observation_t *result);
/* One locked completed-row copy with optional qualifying-callback age. The
 * request anchor is sampled before waiting for the lock, never at UI paint. */
esp_err_t cerberus_s3_ble_scanner_get_watch_observed_result(uint8_t generation,
    uint8_t index, cerberus_watch_observation_t *result,
    cerberus_watch_receive_timing_t *timing);
/* Copy the row and its independent source/Location timing under one lock.
 * All outputs are mandatory and cleared together on failure. */
esp_err_t cerberus_s3_ble_scanner_get_watch_detailed_result(uint8_t generation,
    uint8_t index, cerberus_watch_observation_t *result,
    cerberus_watch_receive_timing_t *timing, sentinel_watch_motion_t *motion,
    cerberus_watch_receive_timing_t *motion_timing);
/* One bounded lock copies every field from the same completed bank. The first
 * row may discover its full capture epoch; subsequent rows must require it.
 * Outputs clear together on failure, including unsupported builds. */
esp_err_t cerberus_s3_ble_scanner_get_watch_profiled_result(uint8_t generation,
    uint8_t index, uint64_t expected_capture_epoch, cerberus_watch_observation_t *result,
    cerberus_watch_receive_timing_t *timing, sentinel_watch_motion_t *motion,
    cerberus_watch_receive_timing_t *motion_timing, cerberus_watch_profile_evidence_t *profile);
esp_err_t cerberus_s3_ble_scanner_get_activity(uint8_t generation,
    cerberus_ble_activity_payload_t *status);
/* An exact selected identity is attached separately from its finite passive
 * sweep. Full scan/host epochs authorize callbacks and Stop; generation8 does
 * not. Measurements retain the existing GAP report semantics (ADV/RSP). */
typedef struct {
    cerberus_ble_scan_status_payload_t scan;
    uint32_t scan_epoch;
    unsigned host_epoch;
    int32_t error;
    bool attached, owned, receiving, stopping;
} cerberus_s3_ble_track_state_t;

/* Begin rejects an existing attachment. Absent get_state returns canonical
 * zero/ESP_OK. Reset or replacement remains faulted until release + begin. */
esp_err_t cerberus_s3_ble_scanner_track_begin(const uint8_t address[6],
    uint8_t address_type);
esp_err_t cerberus_s3_ble_scanner_track_start(cerberus_s3_ble_track_state_t *state);
esp_err_t cerberus_s3_ble_scanner_track_get_state(cerberus_s3_ble_track_state_t *state);
esp_err_t cerberus_s3_ble_scanner_track_stop(cerberus_s3_ble_track_state_t *state);
/* Release refuses an owned/queued sweep; end is the safe compatibility wrapper. */
esp_err_t cerberus_s3_ble_scanner_track_release(void);
void cerberus_s3_ble_scanner_track_end(void);
esp_err_t cerberus_s3_ble_scanner_track_status(
    cerberus_s3_ble_track_status_t *status);
/* Analyzer owns a separate attachment over the existing finite passive GAP
 * procedure. The never-reused callback epoch and host epoch are checked under
 * scan_mutex; generation is display metadata, never Stop authorization. */
typedef struct {
    cerberus_ble_scan_status_payload_t scan;
    uint32_t scan_epoch;
    unsigned host_epoch;
    int32_t error;
    bool attached, owned, receiving, stopping;
} cerberus_s3_ble_analyzer_state_t;

/* Attaches the existing optional PSRAM history/shared scanner snapshot; no RF. A second attachment
 * is rejected. Start accepts one passive eight-second sweep; get_state never
 * starts/rearms it. Every output is cleared for invalid/absent input. */
esp_err_t cerberus_s3_ble_scanner_analyzer_begin(void);
esp_err_t cerberus_s3_ble_scanner_analyzer_start(cerberus_s3_ble_analyzer_state_t *state);
esp_err_t cerberus_s3_ble_scanner_analyzer_get_state(cerberus_s3_ble_analyzer_state_t *state);
esp_err_t cerberus_s3_ble_scanner_analyzer_stop(cerberus_s3_ble_analyzer_state_t *state);
/* Refuses to free an owned/queued sweep. Host reset or a proven replacement
 * retires our ownership without stopping or attributing the foreign scan. */
esp_err_t cerberus_s3_ble_scanner_analyzer_release(void);
/* Compatibility wrapper: a failed release retains its attachment safely. */
void cerberus_s3_ble_scanner_analyzer_end(void);
esp_err_t cerberus_s3_ble_scanner_analyzer_status(cerberus_ble_analyzer_view_t *view);
