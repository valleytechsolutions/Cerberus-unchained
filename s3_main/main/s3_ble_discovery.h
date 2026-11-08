#pragma once

#include "c5_spi_link.h"
#include "s3_ble_snapshot.h"
#include <string.h>

/* Interpret only a frozen same-source copy. Exact original_source_ms must
 * already have been lifted using its producer epoch and full-width copy
 * ceiling; a low32 time by itself is never sufficient. No clock is sampled.
 * UNKNOWN includes stale raw ADV surviving a newer invalid original; NONE
 * means this valid original has no admitted response. PRESENT supplies the
 * response's independent exact source time (including genuine time zero).
 * This helper does not authorize an active action or make evidence recent. */
static inline cerberus_s3_ble_response_state_t cerberus_s3_ble_discovery_response(
    const cerberus_s3_ble_focus_t *focus, const cerberus_c5_ble_advertisement_t *raw,
    uint8_t generation, uint32_t observed_ms, uint64_t original_source_ms,
    uint64_t copy_ceiling_ms, uint64_t *response_source_ms)
{
    if (response_source_ms) *response_source_ms = 0U;
    if (!focus || !raw || !response_source_ms || original_source_ms > copy_ceiling_ms ||
        (uint32_t)original_source_ms != observed_ms || raw->address_type > 3U ||
        raw->length > sizeof(raw->data) || raw->event_type > 3U ||
        (raw->flags & ~CERBERUS_BLE_ADVERTISEMENT_TRUNCATED) ||
        ((raw->flags & CERBERUS_BLE_ADVERTISEMENT_TRUNCATED) &&
         raw->length != sizeof(raw->data))) return CERBERUS_S3_BLE_RESPONSE_UNKNOWN;
    const cerberus_s3_ble_discovery_evidence_t *source = &focus->discovery;
    const uint8_t required = CERBERUS_S3_BLE_EVIDENCE_ADV_COMPLETE |
        CERBERUS_S3_BLE_EVIDENCE_ADV_VALID;
    if (!cerberus_s3_ble_response_storage_valid(source) ||
        (source->flags & required) != required || (source->flags & ~0x3fU) ||
        (source->flags & CERBERUS_S3_BLE_EVIDENCE_SID_INVALID) ||
        ((source->flags & CERBERUS_S3_BLE_EVIDENCE_SID_VALID) && source->sid > 15U) ||
        !source->advertisement_sequence ||
        source->raw_advertisement_sequence != source->advertisement_sequence ||
        source->advertisement_observed_ms != observed_ms ||
        source->generation != generation || source->address_type != raw->address_type ||
        memcmp(source->address, raw->address, sizeof(raw->address)) ||
        ((source->flags & CERBERUS_S3_BLE_EVIDENCE_RAW_COMPLETE) != 0U) ==
            ((raw->flags & CERBERUS_BLE_ADVERTISEMENT_TRUNCATED) != 0U))
        return CERBERUS_S3_BLE_RESPONSE_UNKNOWN;
    if (!(source->flags & CERBERUS_S3_BLE_EVIDENCE_RESPONSE_VALID))
        return CERBERUS_S3_BLE_RESPONSE_NONE;
    uint32_t elapsed = source->scan_response_observed_ms - source->advertisement_observed_ms;
    if (original_source_ms > UINT64_MAX - elapsed || original_source_ms + elapsed > copy_ceiling_ms)
        return CERBERUS_S3_BLE_RESPONSE_UNKNOWN;
    *response_source_ms = original_source_ms + elapsed;
    return CERBERUS_S3_BLE_RESPONSE_PRESENT;
}

/* A local scan identity never aliases the public generation byte. Revision
 * changes only when the producer observes a report or a lifecycle transition;
 * neither copying nor rendering refreshes any source timestamp. */
typedef struct {
    uint64_t scan_epoch;
    uint64_t revision;
    uint64_t started_ms;
    uint64_t source_now_ms; /* Same S3 clock; copy anchor, never raw observation time. */
    cerberus_ble_scan_status_payload_t status;
    /* stored_count includes admitted rows whose current family hints cleared;
     * consumers derive matches and retire those typed nonmatching records. */
    cerberus_s3_ble_filter_t family;
    uint16_t replacements;
    uint16_t binding_overflows;
    /* A passive saved-address attachment. The selector has no implied type,
     * payload identity or current device identity. Ordinary lists keep zero. */
    uint8_t target_address[6];
    bool reacquiring;
    bool attached;
    bool owned;
    bool stopping;
} cerberus_s3_ble_discovery_meta_t;

typedef struct {
    cerberus_c5_ble_result_t peer;
    cerberus_c5_ble_advertisement_t advertisement;
    cerberus_s3_ble_focus_t focus;
    uint32_t observed_ms; /* Exact original raw advertisement time; zero valid. */
    uint8_t generation;
    uint8_t index;
} cerberus_s3_ble_discovery_record_t;

typedef struct {
    cerberus_s3_ble_discovery_meta_t meta;
    cerberus_s3_ble_discovery_record_t records[CERBERUS_BLE_SCAN_MAX_RESULTS];
} cerberus_s3_ble_discovery_bank_t;

/* Initial Start requires expected_previous_epoch=0 and no attachment. Exact
 * same-family rearm is allowed only after the previous finite procedure and
 * queued cleanup have released. Rejected starts clear meta (no foreign token).
 * Accepted Start returns its epoch before the asynchronous SDK start executes.
 * A focused attachment owns one reusable PSRAM binding cache until release. */
esp_err_t cerberus_s3_ble_scanner_start_discovery(cerberus_s3_ble_filter_t family,
    uint64_t expected_previous_epoch, cerberus_s3_ble_discovery_meta_t *meta);

/* Passive originals from this exact six-byte selector, retaining separately
 * observed types 0..3. Rearm requires the same selector and attachment mode;
 * no saved metadata enters the receive bank or the Fox Hunt tracker. */
#define CERBERUS_S3_BLE_REACQUIRE_CAPACITY 4U
esp_err_t cerberus_s3_ble_scanner_start_reacquire(const uint8_t address[6],
    uint64_t expected_previous_epoch, cerberus_s3_ble_discovery_meta_t *meta);

/* Scoped read/Stop/release return NOT_FOUND only for authoritative absence of
 * this exact attachment. TIMEOUT means ownership is unknown and must be kept.
 * All outputs clear on failure. Reads never initialize, allocate, or start RF.
 * Copies reject source clocks before any retained evidence and intervals
 * spanning a full32-bit timer revolution; no ambiguous source age is guessed.
 * Active or COMPLETE bank copies include every row under one bounded mutex. */
esp_err_t cerberus_s3_ble_scanner_discovery_status(uint64_t expected_epoch,
    cerberus_s3_ble_discovery_meta_t *meta);
esp_err_t cerberus_s3_ble_scanner_copy_discovery(uint64_t expected_epoch,
    cerberus_s3_ble_discovery_bank_t *bank);
/* Compact atomic copy for a targeted attachment only. The caller supplies
 * four records. Failure clears all four records and meta. */
esp_err_t cerberus_s3_ble_scanner_copy_reacquire(uint64_t expected_epoch,
    cerberus_s3_ble_discovery_record_t records[CERBERUS_S3_BLE_REACQUIRE_CAPACITY],
    cerberus_s3_ble_discovery_meta_t *meta);
esp_err_t cerberus_s3_ble_scanner_stop_discovery(uint64_t expected_epoch,
    cerberus_s3_ble_discovery_meta_t *meta);
esp_err_t cerberus_s3_ble_scanner_release_discovery(uint64_t expected_epoch);

/* Unscoped aggregate diagnostics only; never a control/adoption token. An
 * unattached or never-initialized scanner returns canonical zero/ESP_OK.
 * Attached state uses the same bounded lock and source-clock validation. */
esp_err_t cerberus_s3_ble_scanner_discovery_diagnostics(cerberus_s3_ble_discovery_meta_t *meta);
