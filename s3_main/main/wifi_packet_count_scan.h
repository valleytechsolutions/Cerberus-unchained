#pragma once
/* Original selected-target presentation controller. No allocation, tasks or
 * radio calls outside tick; all producer access uses exact session replies. */
#include "esp_err.h"
#include "wifi_packet_count.h"
#include "wifi_packet_count_protocol.h"

#define WIFI_PACKET_COUNT_TICK_CALLS 4U
#define WIFI_PACKET_COUNT_TICK_BUDGET_US 8000U
#define WIFI_PACKET_COUNT_RETRY_MS 700U
#define WIFI_PACKET_COUNT_KEEPALIVE_MS 1000U
/* One operation may precede a due keepalive only with a recent exact STATUS.
 * Keep at least half the native lease for delayed transport/worker scheduling. */
#define WIFI_PACKET_COUNT_PROGRESS_MAX_MS (CERBERUS_PACKET_COUNT_LEASE_MS / 2U)
#define WIFI_PACKET_COUNT_REFRESH_MS 500U
enum { WIFI_PACKET_COUNT_SOURCE_S3, WIFI_PACKET_COUNT_SOURCE_C5, WIFI_PACKET_COUNT_SOURCES };
enum { WIFI_PACKET_COUNT_FIXED, WIFI_PACKET_COUNT_SELECTED, WIFI_PACKET_COUNT_ALL };

typedef struct {
    cerberus_wifi_packet_count_target_t target;
    uint8_t discovery_source, bssid[6];
    uint64_t discovery_publication;
    char label[33]; /* Frozen discovery label, never matching authority. */
} wifi_packet_count_selection_t;

typedef struct {
    esp_err_t (*exchange)(void *context, unsigned source, uint8_t command,
        const void *request, size_t request_size, void *reply, size_t reply_size);
    int64_t (*clock_us)(void *context);
    esp_err_t (*record_bind)(void *context, unsigned source, uint64_t session);
    bool (*record_drained)(void *context, unsigned source, uint64_t session);
    void *context;
} wifi_packet_count_transport_t;

typedef struct {
    uint64_t observations, transmitted, addressed, bssid_context;
    uint64_t management, data, retry, protected_frames, fragmented;
} wifi_packet_count_row_totals_t;

typedef struct {
    wifi_packet_count_row_totals_t rows[CERBERUS_WIFI_PACKET_COUNT_LIMIT];
    uint64_t channel_window_us[CERBERUS_WIFI_PACKET_COUNT_CHANNELS];
    uint64_t callbacks, matched_frames, invalid_metadata, malformed_frames;
    uint64_t unsupported_frames, clock_dropped_frames, outside_window_frames;
    uint64_t valid_frames, channel_failures, raw_dropped_full, raw_dropped_other;
    uint32_t segments;
    bool saturated;
} wifi_packet_count_totals_t;

typedef struct {
    cerberus_wifi_packet_count_summary_t summary;
    cerberus_wifi_packet_count_row_t rows[CERBERUS_WIFI_PACKET_COUNT_LIMIT];
    uint64_t channel_window_us[CERBERUS_WIFI_PACKET_COUNT_CHANNELS];
    uint64_t row_copied_us[CERBERUS_WIFI_PACKET_COUNT_LIMIT];
    uint64_t row_anchor_us[CERBERUS_WIFI_PACKET_COUNT_LIMIT];
    uint16_t row_anchor_valid;
    cerberus_packet_count_record_stats_t recording;
    uint64_t session, request_anchor_us;
    esp_err_t source_error;
    uint8_t state, channel;
    bool valid, frozen, anchor_valid;
} wifi_packet_count_snapshot_t;

enum {
    WIFI_COUNT_IDLE, WIFI_COUNT_INFO, WIFI_COUNT_BEGIN, WIFI_COUNT_TARGET,
    WIFI_COUNT_START, WIFI_COUNT_POLL, WIFI_COUNT_PEEK, WIFI_COUNT_READ,
    WIFI_COUNT_FINAL_POLL, WIFI_COUNT_RELEASE
};
typedef struct {
    wifi_packet_count_snapshot_t published, staging;
    wifi_packet_count_totals_t completed;
    cerberus_wifi_packet_count_summary_t before;
    cerberus_packet_count_begin_t request;
    cerberus_packet_count_control_t remote;
    uint8_t scratch[CERBERUS_PACKET_COUNT_SUMMARY_BYTES];
    uint64_t session, next_action_ms, last_status_ms, next_cycle_ms;
    uint64_t read_anchor_us;
    uint32_t pin, revision_floor, projection_floor;
    esp_err_t error;
    uint16_t part;
    uint8_t phase, upload_index, kind, index;
    bool attempted, accepted, start_attempted, have_control, stop, close;
    bool faulted, final_valid, committed, clean_rejection, anchor_valid;
    bool cycle_final, after_summary;
    bool record_bound, release_attempted, clock_invalid;
    bool progress_due;
} wifi_packet_count_source_t;

/* Keep this bounded workspace in optional PSRAM. A manual retune freezes and
 * accumulates both prior segments before arming the next disjoint plan. */
typedef struct {
    wifi_packet_count_selection_t selected[CERBERUS_WIFI_PACKET_COUNT_LIMIT];
    wifi_packet_count_source_t sources[WIFI_PACKET_COUNT_SOURCES];
    uint64_t requested_channels, pending_channels, last_tick_ms;
    uint32_t segments, interruptions;
    esp_err_t error, s3_admission_error;
    uint16_t dwell_ms;
    uint8_t count, next_source;
    bool requested, closing, retuning, frozen, fallback, recording;
} wifi_packet_count_scan_t;

void wifi_packet_count_scan_init(wifi_packet_count_scan_t *scan);
/* Duplicate role+MAC preserves the original provenance; no silent replacement. */
esp_err_t wifi_packet_count_scan_add(wifi_packet_count_scan_t *scan,
    const wifi_packet_count_selection_t *selection);
esp_err_t wifi_packet_count_scan_remove(wifi_packet_count_scan_t *scan, unsigned index);
uint64_t wifi_packet_count_scan_plan(const wifi_packet_count_scan_t *scan,
    unsigned mode, uint8_t fixed_channel);
bool wifi_packet_count_scan_released(const wifi_packet_count_scan_t *scan);
esp_err_t wifi_packet_count_scan_begin(wifi_packet_count_scan_t *scan,
    uint64_t channel_mask, uint16_t dwell_ms, bool recording);
esp_err_t wifi_packet_count_scan_retune(wifi_packet_count_scan_t *scan, uint64_t channel_mask);
void wifi_packet_count_scan_stop(wifi_packet_count_scan_t *scan);
void wifi_packet_count_scan_close(wifi_packet_count_scan_t *scan);
void wifi_packet_count_scan_tick(wifi_packet_count_scan_t *scan, uint64_t now_ms,
    const wifi_packet_count_transport_t *transport);
/* Totals combine prior fenced segments with the latest sampled current one.
 * No clock subtraction across receivers and no packets-per-second claim. */
bool wifi_packet_count_scan_row(const wifi_packet_count_scan_t *scan, unsigned index,
    wifi_packet_count_row_totals_t *totals, bool *saturated);
bool wifi_packet_count_scan_signal_age(const wifi_packet_count_scan_t *scan,
    unsigned source, unsigned index, int64_t now_us, uint64_t *upper_us);
