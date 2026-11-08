#pragma once

#include "ble_archive.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Marauder's MAC/payload JSON is an interchange record, not a live BLE source.
 * It has no address type, receiver, clock, epoch, RSSI or ADV/RSP provenance.
 * These are Cerberus resource bounds, not BLE or upstream format maxima. */
enum {
    CERBERUS_AIRTAG_JSON_MAX_RECORDS = 64,
    CERBERUS_AIRTAG_JSON_PAYLOAD_CAPACITY = 8192,
    CERBERUS_AIRTAG_JSON_MAX_BYTES = 49152,
    CERBERUS_AIRTAG_JSON_MAX_DEPTH = 12,
};
typedef enum {
    CERBERUS_AIRTAG_JSON_OK = 0,
    CERBERUS_AIRTAG_JSON_INVALID,
    CERBERUS_AIRTAG_JSON_LIMIT,
    CERBERUS_AIRTAG_JSON_NO_ELIGIBLE,
} cerberus_airtag_json_result_t;
typedef struct {
    uint8_t address[6]; /* Native least-significant octet first; type UNKNOWN. */
    uint16_t offset, length;
} cerberus_airtag_json_record_t;
typedef struct {
    uint16_t payload_used;
    uint8_t count;
    cerberus_airtag_json_record_t records[CERBERUS_AIRTAG_JSON_MAX_RECORDS];
    uint8_t payload[CERBERUS_AIRTAG_JSON_PAYLOAD_CAPACITY];
} cerberus_airtag_json_t;
typedef struct {
    /* Packet candidates, not devices: each original and each PRESENT response
     * is considered independently in row / ADV-then-RSP order (at most128).
     * eligible: complete raw Find My hint; filtered: complete raw without it;
     * incomplete: unavailable/unproven/truncated/malformed raw or source. */
    uint16_t eligible, filtered, incomplete;
} cerberus_airtag_json_export_stats_t;

/* Records preserve order and duplicate MACs, including empty payloads. Payload
 * slices are contiguous in record order; absent slots/tail are not evidence.
 * No heap, clock, filesystem, transport, or active-target operation. */
cerberus_airtag_json_result_t cerberus_airtag_json_validate(const cerberus_airtag_json_t *document);
/* Strict JSON/schema and exact declared payload_size. Unknown object members
 * are structurally parsed with bounded nesting. Duplicate schema fields are
 * rejected; duplicate unknown extension names are ignored after validation.
 * Input immutable, no overlap.
 * Decode failure leaves the entire destination unchanged (preflight first). */
cerberus_airtag_json_result_t cerberus_airtag_json_decode(const uint8_t *input,
    size_t length, cerberus_airtag_json_t *document);
/* Canonical root array, uppercase colon MAC, spaced lowercase 0xXX payload.
 * Failure leaves output bytes unchanged; *written=0 unless it aliases input,
 * output or document. Empty array is valid. Capacity never implies truncation. */
cerberus_airtag_json_result_t cerberus_airtag_json_encode(const cerberus_airtag_json_t *document,
    uint8_t *output, size_t capacity, size_t *written);
/* Export separately proven complete native S3/C5 ADV and PRESENT responses.
 * Full decoded hints never reconstruct missing raw bytes. Historical imports
 * and LEGACY rows are not source proof. Stats are published after valid input
 * even on LIMIT/NO_ELIGIBLE; document changes only on OK. Interchange drops
 * native receiver/type/provenance and retains duplicate MAC packet records. */
cerberus_airtag_json_result_t cerberus_airtag_json_from_archive(
    const cerberus_ble_archive_t *archive, cerberus_airtag_json_t *document,
    cerberus_airtag_json_export_stats_t *stats);
