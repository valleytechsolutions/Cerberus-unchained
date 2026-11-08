#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Portable historical SDK AP observations, not raw beacon/SSID evidence or
 * permission to operate a live radio. No allocation, clock or SDK dependency. */
enum {
    CERBERUS_WIFI_ARCHIVE_VERSION = 1,
    CERBERUS_WIFI_ARCHIVE_MAX_ROWS = 64,
    CERBERUS_WIFI_ARCHIVE_MAX_ROWS_PER_SOURCE = 32,
    CERBERUS_WIFI_ARCHIVE_HEADER_SIZE = 96,
    CERBERUS_WIFI_ARCHIVE_ROW_SIZE = 80,
    CERBERUS_WIFI_ARCHIVE_MAX_SIZE = 5216,
    CERBERUS_WIFI_ARCHIVE_SOURCE_S3 = 1,
    CERBERUS_WIFI_ARCHIVE_SOURCE_C5 = 2,
    CERBERUS_WIFI_ARCHIVE_COVERAGE_NONE = 0,
    CERBERUS_WIFI_ARCHIVE_COVERAGE_2G = 1,
    CERBERUS_WIFI_ARCHIVE_COVERAGE_5G = 2,
    CERBERUS_WIFI_ARCHIVE_COVERAGE_ALL = 3,
};

/* These fields describe the ORIGINAL complete publication even in a subset.
 * Identifiers are historical only; C5 relation epoch is not a boot identity.
 * received_us is the local full-bank receipt, never RF/UTC/GPS time. */
typedef struct {
    uint64_t received_us, c5_session_id, c5_relation_epoch;
    uint32_t generation, elapsed_ms;
    int32_t s3_error, c5_error;
    uint16_t s3_total, c5_total;
    uint8_t s3_count, c5_count, s3_coverage, c5_coverage;
    bool split, fallback;
} cerberus_wifi_archive_publication_t;

typedef struct {
    /* Bounded SDK C-string, not complete binary/on-air SSID. The first NUL
     * terminates identity; its tail is canonical zero. Rendering sanitizes. */
    char ssid[33];
    uint8_t bssid[6], channel, security, source, source_index;
    int8_t rssi;
    bool rssi_valid;
    uint32_t source_generation, received_ms;
    /* C5 matching BSSID/SSID/channel frame presence only. Neither this bound
     * nor the request anchor dates the driver's aggregate RSSI/security.
     * S3 has no such RF timestamp; all frame fields must be zero/false. */
    uint64_t frame_request_started_us, frame_age_upper_at_save_ms;
    uint32_t frame_source_age_ms;
    bool frame_present, frame_age_upper_valid;
} cerberus_wifi_archive_row_t;

typedef struct {
    cerberus_wifi_archive_publication_t publication;
    uint64_t saved_us;
    bool saved_time_valid, subset;
    uint8_t count;
    cerberus_wifi_archive_row_t rows[CERBERUS_WIFI_ARCHIVE_MAX_ROWS];
} cerberus_wifi_archive_t;

/* Canonical source/channel/security/timing/identity validation. Source/index
 * duplicates reject; duplicate AP/SSID observations across sources remain.
 * Unused native rows/padding are ignored and never serialized. */
bool cerberus_wifi_archive_validate(const cerberus_wifi_archive_t *document);
/* Bounded component checks for adapters that preflight with one row scratch;
 * document validation additionally enforces source/index/generation coherence. */
bool cerberus_wifi_archive_validate_publication(const cerberus_wifi_archive_publication_t *publication,
    uint64_t saved_us, bool saved_time_valid, bool subset, uint8_t count);
bool cerberus_wifi_archive_validate_row(const cerberus_wifi_archive_row_t *row,
    const cerberus_wifi_archive_publication_t *publication, uint64_t saved_us, bool saved_time_valid);
/* Explicit LE v1 bytes: magic CWFIARC\0; version/header/row sizes at8/10/12;
 * total size16, CRC20, save/receipt/session/relation64 at24/32/40/48;
 * publication32 fields56..71, totals16 at72/74, source counts/coverage76..79,
 * selected count80, split/fallback/subset/save-clock bits81, reserved82..95.
 * Row: source/index/SSID-length/flags0..3, BSSID4..9, channel10, security11,
 * RSSI12, generation16, receipt32 at20, frame-anchor64 at24, source-age32 at32,
 * saved-upper64 at40, SSID32 at48. Other bytes zero; row flags RSSI/frame/upper.
 * CRC32 covers every byte with its own field zero.
 * Decode and subset preflight before mutation; all failed outputs unchanged.
 * Encode failure leaves bytes unchanged and sets *written=0 when disjoint.
 * All arguments that refer to memory must be nonoverlapping. */
bool cerberus_wifi_archive_encode(const cerberus_wifi_archive_t *document,
    uint8_t *bytes, size_t capacity, size_t *written);
bool cerberus_wifi_archive_decode(const uint8_t *bytes, size_t length,
    cerberus_wifi_archive_t *document);
bool cerberus_wifi_archive_subset(const cerberus_wifi_archive_t *document,
    uint64_t selected_rows, cerberus_wifi_archive_t *out);
/* A mask addresses current document indexes, not source indexes. UINT64_MAX
 * means all rows (also for an empty publication); all other excess bits reject. */
