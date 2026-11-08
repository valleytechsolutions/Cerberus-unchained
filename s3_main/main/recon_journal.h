#pragma once

/* Completed-sweep observation journal. It records retained receiver rows, not
 * every RF callback or authenticated physical devices. No I/O, allocation,
 * RTOS or radio operations. The owner serializes all access. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CERBERUS_RECON_JOURNAL_CAPACITY 64U
#define CERBERUS_RECON_NAME_MAX_LENGTH 32U
#define CERBERUS_RECON_RULE_MAX_LENGTH 31U
#define CERBERUS_RECON_CSV_CAPACITY 768U
#define CERBERUS_RECON_MANIFEST_CAPACITY 2048U

typedef enum {
    CERBERUS_RECON_RADIO_WIFI = 0,
    CERBERUS_RECON_RADIO_BLE = 1,
} cerberus_recon_radio_t;

typedef enum {
    CERBERUS_RECON_SOURCE_NONE = 0,
    CERBERUS_RECON_SOURCE_S3_ADV_MS32,
    CERBERUS_RECON_SOURCE_S3_RESPONSE_MS32,
    CERBERUS_RECON_SOURCE_C5_AGE_MS,
    CERBERUS_RECON_SOURCE_S3_CALLBACK_US,
} cerberus_recon_source_t;

enum {
    CERBERUS_RECON_SOURCE_TIME_VALID = 1U << 0,
    CERBERUS_RECON_SOURCE_SEQUENCE_VALID = 1U << 1,
    CERBERUS_RECON_SOURCE_ANCHOR_VALID = 1U << 2,
};

enum {
    CERBERUS_RECON_LIMIT_NAME_TRUNCATED = 1U << 0,
    CERBERUS_RECON_LIMIT_RAW_TRUNCATED = 1U << 1,
    CERBERUS_RECON_LIMIT_RETAINED_CAPACITY = 1U << 2,
    CERBERUS_RECON_LIMIT_REPORTS_REJECTED = 1U << 3,
    CERBERUS_RECON_LIMIT_SOURCE_UNAVAILABLE = 1U << 4,
};

typedef struct {
    /* Assigned by the service. Session IDs are nonzero boot-lifetime IDs;
     * sequence is nonzero and strictly increases across offers in a session.
     * consumed_us is S3 consumption time, never substituted for RF time. */
    uint64_t session_id, sequence, consumed_us;
    uint64_t source_time, source_anchor_us;
    uint32_t source_sequence, source_limits;
    /* Wi-Fi BSSID network order; BLE native NimBLE little-endian order.
     * CSV renders both in the conventional colon-delimited display order. */
    uint8_t address[6];
    uint8_t radio, address_type, generation, channel;
    int8_t rssi;
    uint8_t source_kind, source_flags;
    bool channel_valid, rssi_valid;
    /* Name is exactly the received bytes available to the adapter, with an
     * explicit length. Prior SDK/wire truncation cannot be reconstructed. */
    uint8_t name[CERBERUS_RECON_NAME_MAX_LENGTH], name_length;
    char rule[CERBERUS_RECON_RULE_MAX_LENGTH + 1U];
    uint8_t category, confidence, mode;
    uint8_t alert_state;
    bool classified, ibeacon;
} cerberus_recon_record_t;

typedef enum {
    CERBERUS_RECON_ALERT_NOT_EVALUATED = 0,
    CERBERUS_RECON_ALERT_NOT_QUEUED,
    CERBERUS_RECON_ALERT_QUEUED,
} cerberus_recon_alert_state_t;

typedef struct {
    uint32_t accepted, written, dropped, invalid, abandoned;
    /* Retained journal rows, including repeated identities; independently
     * saturating lower bounds at UINT32_MAX. iBeacon is the BLE row's hint
     * flag, not a separate row, full payload, device or RF-event counter.
     * Accepted includes queued/abandoned rows; written requires commit. */
    uint32_t accepted_wifi_rows, accepted_ble_rows, accepted_ibeacon_rows;
    uint32_t written_wifi_rows, written_ble_rows, written_ibeacon_rows;
} cerberus_recon_counters_t;

typedef struct {
    cerberus_recon_record_t rows[CERBERUS_RECON_JOURNAL_CAPACITY];
    uint64_t session_id, started_us, last_sequence, last_consumed_us;
    cerberus_recon_counters_t counters;
    uint16_t head, count;
} cerberus_recon_journal_t;

typedef enum {
    CERBERUS_RECON_MANIFEST_ACTIVE = 0,
    CERBERUS_RECON_MANIFEST_COMPLETE,
    CERBERUS_RECON_MANIFEST_ERROR,
} cerberus_recon_manifest_state_t;

typedef struct {
    uint64_t session_id, started_us, ended_us, last_sequence;
    char session_name[7]; /* Durable R00001..R99999 folder; ID is boot-local. */
    cerberus_recon_counters_t counters;
    uint16_t queued;
    cerberus_recon_manifest_state_t state;
    int32_t error_code;
} cerberus_recon_summary_t;

/* init is an explicit new-session reset; the owner must finish/abandon the old
 * session first. Invalid ID/arguments leave the object unchanged. */
bool cerberus_recon_journal_init(cerberus_recon_journal_t *journal,
    uint64_t session_id, uint64_t started_us);
bool cerberus_recon_record_valid(const cerberus_recon_record_t *record);
/* False increments invalid for bad input/ordering or dropped for a full ring.
 * A valid full-ring offer still advances sequence/time admission fences.
 * Existing rows never change. Repeated identities across sweeps are retained. */
bool cerberus_recon_journal_offer(cerberus_recon_journal_t *journal,
    const cerberus_recon_record_t *record);
/* Borrowed head remains valid only until the next mutation. A worker should
 * copy it under its own lock before I/O. commit follows a full successful row
 * append only; a partial/failed append must abandon/fail, never retry blindly. */
const cerberus_recon_record_t *cerberus_recon_journal_peek(
    const cerberus_recon_journal_t *journal);
bool cerberus_recon_journal_commit(cerberus_recon_journal_t *journal,
    uint64_t expected_sequence);
void cerberus_recon_journal_abandon(cerberus_recon_journal_t *journal);

/* ASCII CSV uses CRLF and a fixed schema. The quoted name begins "bytes:";
 * backslash and nonprinting/high bytes are escaped reversibly, quotes use CSV
 * doubling. Thus received text cannot add records or spreadsheet formulas.
 * Source units/domain are explicit and RSSI scope is independently labelled.
 * All formatters return bytes excluding NUL; 0 means invalid/insufficient room
 * and clears out[0] when possible. Output may not overlap input objects. */
const char *cerberus_recon_csv_header(void);
size_t cerberus_recon_row_format(const cerberus_recon_record_t *record,
    char *out, size_t capacity);
/* Complete requires queued==0, abandoned==0 and error_code==0. Error requires a nonzero
 * error code. Ending time is S3 uptime, not UTC; no location is fabricated.
 * Counter saturation is reported as a lower bound, not exact totals. Radio
 * sums/subset relationships must remain consistent even after saturation.
 * JSON row_counter_version=1 adds typed counts to the existing v1 schema;
 * the CSV schema and existing manifest fields retain their meaning. */
size_t cerberus_recon_manifest_format(const cerberus_recon_summary_t *summary,
    char *out, size_t capacity);
