#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Native finalized logs only. Interrupted .TMP captures are never upload or
 * bulk-delete candidates. This parser retains no observed identifiers. */
#define CERBERUS_WARD_LOG_NAME_CAPACITY 24U
#define CERBERUS_WARD_LOG_PREFIX "CERBWardrive"
#define CERBERUS_WARD_LOG_LINE_CAPACITY 1024U
#define CERBERUS_WARD_LOG_MAX_BYTES (64U * 1024U * 1024U)
typedef enum { CERBERUS_WARD_LOG_UNKNOWN, CERBERUS_WARD_LOG_WIGLE,
    CERBERUS_WARD_LOG_UNLOCATED } cerberus_ward_log_kind_t;
typedef enum { CERBERUS_WARD_LOG_OK, CERBERUS_WARD_LOG_ARGUMENT,
    CERBERUS_WARD_LOG_LIMIT, CERBERUS_WARD_LOG_ENCODING,
    CERBERUS_WARD_LOG_METADATA, CERBERUS_WARD_LOG_COLUMNS,
    CERBERUS_WARD_LOG_ROW, CERBERUS_WARD_LOG_TRUNCATED,
    CERBERUS_WARD_LOG_EMPTY } cerberus_ward_log_error_t;
typedef struct {
    cerberus_ward_log_kind_t kind;
    cerberus_ward_log_error_t error;
    uint64_t bytes;
    uint32_t lines, rows, wifi_24, wifi_5, ble;
    uint32_t missing_accuracy, missing_altitude, zero_coordinates;
    bool complete, legacy_metadata;
    char first_utc[20], last_utc[20];
} cerberus_ward_log_summary_t;
typedef struct {
    uint8_t address[6];
    uint8_t type; /* 1: WIFI, 2: BLE; never group by SSID or display name. */
    int8_t rssi;
} cerberus_ward_log_identity_t;
typedef struct {
    cerberus_ward_log_summary_t summary;
    cerberus_ward_log_identity_t last_row; /* Last fully validated WiGLE row. */
    size_t used;
    char line[CERBERUS_WARD_LOG_LINE_CAPACITY];
} cerberus_ward_log_parser_t;

cerberus_ward_log_kind_t cerberus_ward_log_name(const char *name);
void cerberus_ward_log_init(cerberus_ward_log_parser_t *parser);
/* Feed accepts arbitrary byte boundaries, including CR/LF and UTF-8 splits.
 * Stop feeding on false. Finish requires two headers, data and a final LF.
 * Native CSV records cannot contain embedded newlines. No row deduplication. */
bool cerberus_ward_log_feed(cerberus_ward_log_parser_t *parser,
    const void *bytes, size_t length);
bool cerberus_ward_log_finish(cerberus_ward_log_parser_t *parser);
const char *cerberus_ward_log_error_name(cerberus_ward_log_error_t error);
