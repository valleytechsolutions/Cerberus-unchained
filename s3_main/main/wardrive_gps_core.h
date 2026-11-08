#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CERBERUS_WARDRIVE_GPS_LINE_CAPACITY 128U
#define CERBERUS_WARDRIVE_GPS_FIX_MAX_AGE_MS 5000U
#define CERBERUS_WARDRIVE_GPS_SENTENCE_MAX_AGE_MS 10000U

typedef enum {
    CERBERUS_WARDRIVE_GPS_ABSENT = 0,
    CERBERUS_WARDRIVE_GPS_NO_FIX,
    CERBERUS_WARDRIVE_GPS_STALE,
    CERBERUS_WARDRIVE_GPS_FRESH,
} cerberus_wardrive_gps_state_t;

/* Parser outcome is explicit even when diagnostic counters have saturated.
 * RMC/GGA identify an accepted sentence, not a guarantee of a navigation fix. */
typedef enum {
    CERBERUS_WARDRIVE_GPS_LINE_OTHER = 0,
    CERBERUS_WARDRIVE_GPS_LINE_RMC,
    CERBERUS_WARDRIVE_GPS_LINE_GGA,
    CERBERUS_WARDRIVE_GPS_LINE_FIELDS_INVALID,
    CERBERUS_WARDRIVE_GPS_LINE_BAD_CHECKSUM,
    CERBERUS_WARDRIVE_GPS_LINE_FRAME_INVALID,
    CERBERUS_WARDRIVE_GPS_LINE_OVERFLOW,
    CERBERUS_WARDRIVE_GPS_LINE_CONTROL,
} cerberus_wardrive_gps_line_result_t;

typedef enum {
    CERBERUS_WARDRIVE_GPS_TERMINATION_LF = 1,
    CERBERUS_WARDRIVE_GPS_TERMINATION_CRLF = 2,
} cerberus_wardrive_gps_termination_t;

/* Synchronous, borrowed payload; copy it before returning if it is needed
 * later. The observer must not re-enter or mutate this core while it runs.
 * payload_complete is raw capture completeness, not parser validity.
 * The payload excludes the explicit LF/CRLF terminator and is not a C string. */
typedef void (*cerberus_wardrive_gps_line_observer_t)(void *context,
    const uint8_t *payload, uint8_t retained_length, uint32_t total_length,
    uint64_t received_ms, cerberus_wardrive_gps_line_result_t result,
    cerberus_wardrive_gps_termination_t termination, bool payload_complete);

/* Pure parser state. The UART/PPS adapter owns synchronization when feeding it
 * from a task. No hardware or ESP-IDF dependency is required for host tests. */
typedef struct {
    char line[CERBERUS_WARDRIVE_GPS_LINE_CAPACITY];
    uint8_t line_length;
    bool discarding_line;
    bool pending_cr;
    bool line_control;
    bool line_overflow;
    uint32_t line_total_length;
    bool sentence_seen;
    bool fix_live;
    bool utc_seen;
    bool altitude_valid;
    bool gga_seen;
    bool satellites_valid;
    bool hdop_valid;
    uint64_t last_sentence_ms;
    uint64_t last_fix_ms;
    uint64_t last_utc_ms;
    uint64_t last_gga_ms;
    double latitude;
    double longitude;
    float altitude_m;
    float hdop;
    uint8_t satellites;
    char utc[25]; /* YYYY-MM-DDTHH:MM:SS.mmmZ from a valid RMC date/time. */
    char utc_second[7];
    char gga_second[7];
    bool gga_time_valid;
    bool gga_fix_valid; /* A qualified position from the timed GGA epoch. */
    bool rmc_time_valid;
    bool rmc_fix_valid;
    uint16_t gga_millis,utc_millis;
    uint32_t valid_sentences;
    uint32_t bad_checksums;
    uint32_t malformed_sentences;
    uint32_t overflow_lines;
    uint32_t fix_updates;
} cerberus_wardrive_gps_core_t;

typedef struct {
    cerberus_wardrive_gps_state_t state;
    uint32_t sentence_age_ms;
    uint32_t fix_age_ms;
    uint32_t utc_age_ms; /* UINT32_MAX until a checksum-validated RMC time exists. */
    uint32_t gga_age_ms; /* Independent of RMC coordinates/UTC; UINT32_MAX if unknown. */
    bool coordinates_valid;
    double latitude;
    double longitude;
    bool altitude_valid;
    float altitude_m;
    float hdop;
    uint8_t satellites;
    bool satellites_valid;
    bool hdop_valid;
    bool utc_valid;
    bool utc_from_rmc;
    char utc[25];
    uint32_t valid_sentences;
    uint32_t bad_checksums;
    uint32_t malformed_sentences;
    uint32_t overflow_lines;
    uint32_t fix_updates;
} cerberus_wardrive_gps_snapshot_t;

void cerberus_wardrive_gps_core_reset(cerberus_wardrive_gps_core_t *core);

/* Feed arbitrary UART chunks. A line with an invalid/missing checksum, an
 * embedded control byte, or more than 127 bytes is discarded. */
void cerberus_wardrive_gps_core_feed(cerberus_wardrive_gps_core_t *core,
    const uint8_t *bytes, size_t count, uint64_t monotonic_ms);

/* Same sole assembler as core_feed. At each nonempty terminated boundary the
 * optional observer receives the original first127 payload bytes, a saturating
 * total payload length and explicit parse/discard outcome. No extra buffer or
 * callback is created for an unterminated partial line. */
void cerberus_wardrive_gps_core_feed_observed(cerberus_wardrive_gps_core_t *core,
    const uint8_t *bytes, size_t count, uint64_t monotonic_ms,
    cerberus_wardrive_gps_line_observer_t observer, void *context);

/* For complete NMEA lines without CR/LF. Returns true only when a strict
 * checksum passes; recognized sentences may still have malformed fields. */
bool cerberus_wardrive_gps_core_ingest_line(cerberus_wardrive_gps_core_t *core,
    const char *line, size_t length, uint64_t monotonic_ms);

/* Coordinates and UTC are zero/empty unless the fix is fresh. UTC is separately
 * valid only when a checksum-checked RMC gave a valid date and time. */
void cerberus_wardrive_gps_core_snapshot(const cerberus_wardrive_gps_core_t *core,
    uint64_t monotonic_ms, cerberus_wardrive_gps_snapshot_t *out);
