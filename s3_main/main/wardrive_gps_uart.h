#pragma once

#include "esp_err.h"
#include "wardrive_gps_core.h"
#include "gps_config.h"

/* LC29H can emit multi-constellation NMEA bursts at 115200 or faster. A 4 KiB
 * UART ring and 20 ms reads bound burst pressure without per-line allocation. */
#define CERBERUS_WARDRIVE_GPS_RX_BUFFER_BYTES 4096U
#define CERBERUS_WARDRIVE_GPS_TASK_STACK_BYTES 3584U
#define CERBERUS_WARDRIVE_GPS_TRACE_CAPACITY 8U
#define CERBERUS_WARDRIVE_GPS_TRACE_PAYLOAD_BYTES (CERBERUS_WARDRIVE_GPS_LINE_CAPACITY - 1U)

enum {
    CERBERUS_WARDRIVE_GPS_TRACE_PAYLOAD_COMPLETE = 1U,
    CERBERUS_WARDRIVE_GPS_TRACE_RECEIPT_VALID = 2U,
    CERBERUS_WARDRIVE_GPS_TRACE_MASKED = 4U,
};

typedef struct {
    uint64_t sequence;
    uint64_t receiver_epoch;
    uint64_t received_ms; /* UART read receipt, not GNSS fix/capture time. */
    uint32_t total_length;
    uint8_t retained_length;
    uint8_t result; /* cerberus_wardrive_gps_line_result_t */
    uint8_t termination; /* cerberus_wardrive_gps_termination_t */
    uint8_t flags;
    uint8_t payload[CERBERUS_WARDRIVE_GPS_TRACE_PAYLOAD_BYTES];
} cerberus_wardrive_gps_trace_row_t;

typedef struct {
    cerberus_wardrive_gps_snapshot_t gps;
    uint64_t receiver_epoch;
    esp_err_t error; /* Current failure while resources remain held. */
    esp_err_t historical_error; /* Last failure after clean release. */
    bool driver_active;
    bool reader_running;
    bool starting;
    bool stopping;
    bool clock_valid;
    cerberus_gps_config_t config;
    uint32_t active_baud, received_bytes, baud_changes;
    bool detecting_baud;
} cerberus_wardrive_gps_uart_state_t;

typedef struct {
    uint64_t observer_token;
    uint64_t receiver_epoch;
    /* Evictions count retained rows replaced by newer rows. Discarded counts
     * overflow/control/frame-invalid input, not all checksum/field failures
     * and not bytes lost before the UART reader. Those outcomes remain per row. */
    uint32_t observed_lines;
    uint32_t evicted_lines;
    uint32_t overflow_lines;
    uint32_t discarded_lines;
    uint8_t count;
    bool capturing;
    bool sequence_exhausted;
    cerberus_wardrive_gps_trace_row_t rows[CERBERUS_WARDRIVE_GPS_TRACE_CAPACITY];
} cerberus_wardrive_gps_trace_snapshot_t;

/* UART2 uses the saved RX pin and baud/preset. Session and screen preview
 * share one reader, preserving the acquired fix across Start. A failed Stop
 * retains ownership for an exact same-owner retry. TX/PPS assignments are
 * stored but neither pin is driven; NMEA supplies position and UTC. */
esp_err_t cerberus_wardrive_gps_uart_start(void);
esp_err_t cerberus_wardrive_gps_uart_stop(void);
/* UI attaches once on entry and releases on leave; session ownership keeps
 * the same receiver/fix alive across Start/Stop. Retry NOT_FINISHED releases. */
esp_err_t cerberus_wardrive_gps_uart_preview(bool enabled);
void cerberus_wardrive_gps_uart_snapshot(
    cerberus_wardrive_gps_snapshot_t *out, esp_err_t *uart_error);

/* Time holdover for TLS after leaving the GPS screen. This does not make an
 * old position eligible for logging, start a reader or retain UART ownership.
 * The sample must come from a valid RMC fix in this boot. */
#define CERBERUS_WARDRIVE_GPS_TIME_HOLDOVER_MS 900000U
typedef struct {
    char utc[25];
    uint64_t received_ms;
} cerberus_wardrive_gps_time_t;
esp_err_t cerberus_wardrive_gps_uart_recent_time(cerberus_wardrive_gps_time_t *out);

/* Value-only local state. No UART operations, allocation or cleanup. */
esp_err_t cerberus_wardrive_gps_uart_get_state(cerberus_wardrive_gps_uart_state_t *out);

/* An explicit observer of an already-running reader. OFF/starting/stopping
 * returns NOT_FINISHED without allocation. Only one observer may attach.
 * The eight-row SPIRAM bank has no internal fallback/per-line allocation.
 * Opening, reading and ending never start or stop UART/radios/logging.
 * Read returns oldest-to-newest values. Masking zeroes every payload and
 * retained_length, including unknown/malformed lines. Stop retains historical
 * rows; a new receiver session clears them without reusing sequence numbers.
 * End requires the exact token and releases only this observer's storage.
 * Observer lock attempts are bounded; End TIMEOUT retains the token/storage
 * for an exact retry. A sequence-exhausted ring is retained, not capturing. */
esp_err_t cerberus_wardrive_gps_uart_trace_begin(uint64_t *observer_token);
esp_err_t cerberus_wardrive_gps_uart_trace_read(uint64_t observer_token,
    bool mask, cerberus_wardrive_gps_trace_snapshot_t *out);
esp_err_t cerberus_wardrive_gps_uart_trace_end(uint64_t observer_token);
