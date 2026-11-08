#pragma once
#include "probe_sniff_model.h"
#include "esp_err.h"

#define CERBERUS_PROBE_SNIFF_TICK_CALLS 4U
#define CERBERUS_PROBE_SNIFF_TICK_BUDGET_US 8000U
#define CERBERUS_PROBE_SNIFF_REFRESH_MS 500U
#define CERBERUS_PROBE_SNIFF_RETRY_MS 700U
#define CERBERUS_PROBE_SNIFF_KEEPALIVE_MS 1000U

typedef struct {
    esp_err_t (*control)(void *context, uint8_t command,
        const cerberus_probe_sniff_start_t *request, cerberus_probe_sniff_control_t *reply);
    esp_err_t (*read)(void *context, const cerberus_probe_sniff_read_t *request,
        cerberus_probe_sniff_chunk_t *reply);
    int64_t (*clock_us)(void *context);
    void *context;
} cerberus_probe_sniff_transport_t;

enum { CERBERUS_PROBE_SCAN_IDLE, CERBERUS_PROBE_SCAN_INFO, CERBERUS_PROBE_SCAN_START,
    CERBERUS_PROBE_SCAN_BEFORE, CERBERUS_PROBE_SCAN_SUMMARY_BEFORE,
    CERBERUS_PROBE_SCAN_ROW, CERBERUS_PROBE_SCAN_SUMMARY_AFTER,
    CERBERUS_PROBE_SCAN_AFTER };

/* Place in the optional UI PSRAM workspace, never an automatic local. */
typedef struct {
    cerberus_probe_sniff_snapshot_t published, staging;
    cerberus_probe_sniff_summary_t before;
    cerberus_probe_sniff_control_t remote;
    cerberus_probe_sniff_start_t request;
    union {
        cerberus_probe_sniff_summary_t summary;
        cerberus_probe_sniff_projection_t row;
        uint8_t bytes[CERBERUS_PROBE_SNIFF_SUMMARY_BYTES];
    } scratch;
    uint64_t publication, last_status_ms, next_action_ms, next_cycle_ms;
    uint32_t pin;
    esp_err_t error;
    uint8_t phase, row, part;
    bool have_control, requested, stop_requested, close_requested, restart_requested;
    bool cycle_frozen;
    uint8_t restart_mode, restart_channel;
} cerberus_probe_sniff_scan_t;

void cerberus_probe_sniff_scan_init(cerberus_probe_sniff_scan_t *scan);
esp_err_t cerberus_probe_sniff_scan_begin(cerberus_probe_sniff_scan_t *scan,
    uint8_t mode, uint8_t channel);
/* Stop retains and downloads final evidence. Close abandons incomplete reads,
 * cancels the exact session, then releases its data. Both are pure intent. */
void cerberus_probe_sniff_scan_stop(cerberus_probe_sniff_scan_t *scan);
void cerberus_probe_sniff_scan_close(cerberus_probe_sniff_scan_t *scan);
bool cerberus_probe_sniff_scan_released(const cerberus_probe_sniff_scan_t *scan);
/* Maximum4 wire exchanges; INFO/START capability checks each cost2. An
 * elapsed-budget check prevents starting another step, not a hard SDK bound.
 * Stop precedes optional reads; exact STATUS renews through details/save. */
void cerberus_probe_sniff_scan_tick(cerberus_probe_sniff_scan_t *scan,
    uint64_t now_ms, const cerberus_probe_sniff_transport_t *transport);
