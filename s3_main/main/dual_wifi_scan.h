#pragma once

#include "c5_spi_link.h"
#include "wifi_archive.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CERBERUS_DUAL_WIFI_CAPACITY 64U
enum { CERBERUS_DUAL_WIFI_SOURCE_S3 = 1U, CERBERUS_DUAL_WIFI_SOURCE_C5 = 2U };
typedef enum {
    CERBERUS_DUAL_WIFI_IDLE,
    CERBERUS_DUAL_WIFI_RUNNING,
    CERBERUS_DUAL_WIFI_READING,
    CERBERUS_DUAL_WIFI_STOPPING,
    CERBERUS_DUAL_WIFI_COMPLETE,
    CERBERUS_DUAL_WIFI_ERROR,
} cerberus_dual_wifi_state_t;

typedef struct {
    cerberus_c5_wifi_result_t ap;
    cerberus_s3_wifi_observation_t observation;
    uint32_t source_generation;
    uint32_t received_ms; /* Local result receipt, not a beacon/source timestamp. */
    uint8_t source;
    uint8_t source_index;
} cerberus_dual_wifi_row_t;

typedef struct {
    cerberus_wifi_archive_publication_t publication;
    uint8_t count;
    cerberus_dual_wifi_row_t rows[CERBERUS_DUAL_WIFI_CAPACITY];
} cerberus_dual_wifi_snapshot_t;

typedef struct {
    uint32_t generation;
    uint32_t elapsed_ms;
    uint32_t completed_ms;
    esp_err_t error;
    esp_err_t s3_error;
    esp_err_t c5_error;
    uint16_t s3_total;
    uint16_t c5_total;
    uint8_t state;
    uint8_t count; /* Published retained bank; stays unchanged during retry/Stop. */
    uint8_t s3_count;
    uint8_t c5_count;
    bool split;
    bool fallback;
    bool cleanup_pending;
    bool rebind_ready;
    bool rebinding;
    bool rebind_cached; /* Validating an existing completed bank; no new RF. */
    uint8_t rebind_source; /* Selected row's receiver; zero for an ordinary scan. */
} cerberus_dual_wifi_status_t;

typedef struct {
    int64_t received_us; /* Original full-bank local receipt, never RF time. */
    uint64_t session_id;
    uint8_t band; /* CERBERUS_WIFI_SCAN_BAND_* coverage of this C5 bank. */
    bool reused;
} cerberus_dual_wifi_handoff_t;

/* One UI consumer. start/stop schedule work; call step regularly even after
 * leaving a page until cleanup_pending clears. No added coordinator task.
 * Split uses local S3 only when its Wi-Fi and BLE owners are available. A
 * clean local refusal falls back to the owned C5 all-band scan. Firmware<61
 * fails before RF so the caller can keep the legacy scan route. */
esp_err_t cerberus_dual_wifi_scan_start(void);
void cerberus_dual_wifi_scan_step(void);
void cerberus_dual_wifi_scan_stop(void);
void cerberus_dual_wifi_scan_get_status(cerberus_dual_wifi_status_t *out);
esp_err_t cerberus_dual_wifi_scan_get_row(uint8_t index, cerberus_dual_wifi_row_t *out);
/* UI-task-only coherent completed publication copy, no radio/clock/allocation.
 * Old publication survives retry, Stop, errors and AP rebind. No complete bank
 * means NOT_FOUND. Failure leaves output unchanged; no internal alias allowed. */
esp_err_t cerberus_dual_wifi_scan_copy_snapshot(cerberus_dual_wifi_snapshot_t *out);
/* Direct masked historical capture avoids another full-bank scratch. Caller
 * supplies the sole save-clock sample; UINT64_MAX selects every retained row.
 * UI task only, no radio/clock/allocation, failure leaves output unchanged. */
esp_err_t cerberus_dual_wifi_scan_capture_archive(int64_t now_us, uint64_t selected_rows,
    cerberus_wifi_archive_t *out);

/* A C5-origin row in the eligible completed ordinary bank can be rebound using
 * owned-status / expected-proof / owned-status validation without new RF.
 * Other rows use a fresh receive-only C5 all-band scan. The retained64 view
 * remains unchanged. A match requires exact BSSID, channel, security and SSID;
 * cached reuse additionally requires the original source generation/index.
 * copy_legacy_bank performs one bounded owned-status fence before export;
 * the receiving AP workflow must retain/validate its proof and preserve the
 * mandatory handoff receipt time and band. Failed runtime validation does not
 * silently start RF or stop a foreign session. */
esp_err_t cerberus_dual_wifi_scan_begin_rebind(const cerberus_dual_wifi_row_t *target);
esp_err_t cerberus_dual_wifi_scan_copy_legacy_bank(cerberus_c5_wifi_result_t *rows,
    cerberus_s3_wifi_observation_t *observations, size_t capacity,
    cerberus_wifi_scan_status_payload_t *status, cerberus_c5_wifi_relation_bank_t *proof,
    uint8_t *selected_index, cerberus_dual_wifi_handoff_t *handoff);

/* UI task only: publish local aggregates after the UI tick, including
 * hidden cleanup. The console must use diagnostics_get instead. */
void cerberus_dual_wifi_scan_publish_diagnostics(int32_t ui_error);
