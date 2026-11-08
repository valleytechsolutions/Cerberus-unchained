#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    CERBERUS_WIFI_READOUT_IDLE = 0,
    CERBERUS_WIFI_READOUT_READING,
    CERBERUS_WIFI_READOUT_COMPLETE,
    CERBERUS_WIFI_READOUT_FAILED,
    CERBERUS_WIFI_READOUT_CANCELLED,
} cerberus_wifi_readout_outcome_t;

typedef enum {
    CERBERUS_WIFI_READOUT_CANCEL_NONE = 0,
    CERBERUS_WIFI_READOUT_CANCEL_STOP,
    CERBERUS_WIFI_READOUT_CANCEL_PAGE,
    CERBERUS_WIFI_READOUT_CANCEL_SLEEP,
    CERBERUS_WIFI_READOUT_CANCEL_SUPERSEDED,
    CERBERUS_WIFI_READOUT_CANCEL_RESET,
} cerberus_wifi_readout_cancel_reason_t;

/* Local S3 aggregates only. No result/PSRAM pointers, addresses, SSIDs or RF
 * times. Elapsed covers accepted readout begin through commit/failure/cancel,
 * excluding RF scan duration and screen presentation. Max step is wall time
 * in a whole readout tick (including its bounded batch of transfer substeps),
 * while max gap is the interval between tick starts. Neither
 * is physical touch latency. Phase is the UI's numeric transfer phase. */
typedef struct {
    uint64_t sampled_us, elapsed_us, max_step_us, max_tick_gap_us;
    uint32_t attempts, completed, failed, cancelled, step_calls;
    int32_t error;
    uint16_t rows_total, rows_copied;
    uint8_t state, phase, cancel_reason;
    bool timing_valid, counters_saturated;
} cerberus_wifi_readout_diagnostics_t;

/* One UI task owns begin/step/finish. They allocate nothing and perform no
 * I/O. Only the published value copy is protected by a short critical section;
 * console readers never inspect the UI-owned model or its transfer storage.
 * Counters saturate. A clock rollback/negative sample invalidates and clears
 * all timing measurements until begin; counts and outcome remain available.
 * Begin before allocation, so allocation failure is a measurable attempt. */
void cerberus_wifi_readout_diagnostics_begin(uint16_t rows_total, uint8_t phase,
    int64_t now_us);
/* Call once after a measured tick, even if finish ran inside that tick. In
 * that case max-step/gap are updated but the terminal elapsed time is retained.
 * Do not report idle ticks or steps from a superseded attempt. */
void cerberus_wifi_readout_diagnostics_step(uint8_t phase, uint16_t rows_copied,
    int64_t started_us, int64_t finished_us);
/* First terminal outcome wins. Preserve the phase before cancellation clears
 * transfer storage. Cancel reasons apply only to CANCELLED; use NONE otherwise.
 * Keep rows_copied <= rows_total. Terminal measurements persist until begin. */
void cerberus_wifi_readout_diagnostics_finish(cerberus_wifi_readout_outcome_t outcome,
    cerberus_wifi_readout_cancel_reason_t cancel_reason, int32_t error,
    uint8_t phase, uint16_t rows_copied, int64_t now_us);
void cerberus_wifi_readout_diagnostics_get(cerberus_wifi_readout_diagnostics_t *snapshot);
