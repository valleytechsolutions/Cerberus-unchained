#pragma once
#include "ble_family_wire.h"
#include "dual_ble_scan.h"
#include "ble_response.h"

/* Historical publication proof, never a current radio lease. Generation0 is
 * valid after wrap; boot+epoch must be nonzero and index must be in the bank.
 * Full-callback proof, when present, supplies same-original family facts;
 * otherwise retained raw bytes alone supply them. Cached peer name, UUID, manufacturer,
 * RSSI, count and time fields cannot qualify or corroborate a hint.
 * Known receive metadata is required. A full report longer than31 without
 * full-callback proof remains unclassifiable because its suffix was not retained. Incomplete
 * EXT reports and responses never acquire this envelope. Nonlegacy SIDff is
 * allowed by the shared validator: one-original format hints do not claim a
 * particular advertising set or S3-style matched response. A later partial
 * callback may be absent from this retained-original view; no current/latest
 * callback or freshness claim follows from classification. */
static inline bool cerberus_dual_ble_family_classify(const cerberus_dual_ble_row_t *row,
    const cerberus_dual_ble_provenance_t *provenance,
    cerberus_dual_ble_family_evidence_t *out)
{
    if (!out) return false;
    if (!row)
        return cerberus_dual_ble_family_unavailable(out, CERBERUS_DUAL_BLE_FAMILY_INVALID_SOURCE);
    if (!provenance || !provenance->boot_id || !provenance->epoch ||
        provenance->index >= CERBERUS_DUAL_BLE_CAPACITY)
        return cerberus_dual_ble_family_unavailable(out, CERBERUS_DUAL_BLE_FAMILY_NO_PROVENANCE);
    if (row->address_type > 3U || row->event_type > 3U ||
        row->raw_length > CERBERUS_DUAL_BLE_RAW_BYTES ||
        (row->raw_flags & ~(CERBERUS_BLE_ADVERTISEMENT_TRUNCATED | CERBERUS_BLE_ADVERTISEMENT_MALFORMED)) ||
        !cerberus_dual_ble_receive_metadata_valid(&row->receive,
            row->raw_length, row->event_type, row->raw_flags))
        return cerberus_dual_ble_family_unavailable(out, CERBERUS_DUAL_BLE_FAMILY_INVALID_SOURCE);
    const cerberus_dual_ble_family_wire_t absent = {0};
    if (memcmp(&row->family, &absent, sizeof(absent))) {
        if (!cerberus_dual_ble_family_wire_decode(&row->family, &row->receive,
                row->raw_length, row->event_type, row->raw_flags,
                row->address, row->address_type, out))
            return cerberus_dual_ble_family_unavailable(out, CERBERUS_DUAL_BLE_FAMILY_INVALID_SOURCE);
        return out->available;
    }
    if (row->receive.source_kind == CERBERUS_DUAL_BLE_RECEIVE_UNKNOWN)
        return cerberus_dual_ble_family_unavailable(out, CERBERUS_DUAL_BLE_FAMILY_RECEIVE_UNKNOWN);
    if ((row->raw_flags & CERBERUS_BLE_ADVERTISEMENT_TRUNCATED) ||
        row->receive.callback_length > CERBERUS_DUAL_BLE_RAW_BYTES)
        return cerberus_dual_ble_family_unavailable(out, CERBERUS_DUAL_BLE_FAMILY_TRUNCATED);
    if ((row->raw_flags & CERBERUS_BLE_ADVERTISEMENT_MALFORMED) ||
        row->receive.ad_validation != CERBERUS_DUAL_BLE_AD_VALID)
        return cerberus_dual_ble_family_unavailable(out, CERBERUS_DUAL_BLE_FAMILY_MALFORMED);
    if (row->receive.callback_length != row->raw_length)
        return cerberus_dual_ble_family_unavailable(out, CERBERUS_DUAL_BLE_FAMILY_INVALID_SOURCE);
    return cerberus_dual_ble_family_parse_ad(row->raw, row->raw_length,
        row->address, row->address_type, out);
}

/* Validate the response against its frozen original, never against current
 * scanner fields or aggregate peer hints. Generic acquisition carries no
 * original family proof; focused acquisition carries its selected code. */
static inline bool cerberus_dual_ble_response_row_valid(const cerberus_dual_ble_row_t *row)
{
    if (!row) return false;
    uint8_t selected = 0U;
    const cerberus_dual_ble_family_wire_t empty = {0};
    if (memcmp(&row->family, &empty, sizeof(empty))) {
        cerberus_dual_ble_family_evidence_t evidence;
        if (!cerberus_dual_ble_family_wire_decode(&row->family, &row->receive,
                row->raw_length, row->event_type, row->raw_flags,
                row->address, row->address_type, &evidence)) return false;
        selected = row->family.family_code;
    }
    return cerberus_dual_ble_response_source_valid(&row->response, &row->receive,
        row->raw_length, row->event_type, row->raw_flags, row->address,
        row->address_type, selected);
}

static inline bool cerberus_dual_ble_response_present(const cerberus_dual_ble_row_t *row,
    const cerberus_dual_ble_provenance_t *proof)
{
    return proof && proof->boot_id && proof->epoch && proof->index < CERBERUS_DUAL_BLE_CAPACITY &&
        row && row->response.state == CERBERUS_DUAL_BLE_RESPONSE_PRESENT &&
        cerberus_dual_ble_response_row_valid(row);
}

/* This reports response-only facts. It never merges name/UUID fields across
 * originals and responses, and grants no connection or sound eligibility. */
static inline bool cerberus_dual_ble_response_classify(const cerberus_dual_ble_row_t *row,
    const cerberus_dual_ble_provenance_t *proof, cerberus_dual_ble_family_evidence_t *out)
{
    if (!out) return false;
    if (!proof || !proof->boot_id || !proof->epoch || proof->index >= CERBERUS_DUAL_BLE_CAPACITY)
        return cerberus_dual_ble_family_unavailable(out, CERBERUS_DUAL_BLE_FAMILY_NO_PROVENANCE);
    if (!cerberus_dual_ble_response_present(row, proof) ||
        !cerberus_dual_ble_response_family(&row->response, row->address, row->address_type, out))
        return cerberus_dual_ble_family_unavailable(out, CERBERUS_DUAL_BLE_FAMILY_INVALID_SOURCE);
    return out->available;
}

/* Conservative response age only: the caller supplies its current clock.
 * Both sources retain FIRST BEGIN's original anchor. Unknown/backward/overflow
 * values fail without changing output; no exact C5-to-S3 clock is invented. */
static inline bool cerberus_dual_ble_response_age(const cerberus_dual_ble_row_t *row,
    uint64_t now_ms, uint32_t *out)
{
    if (!out || !row || row->response.state != CERBERUS_DUAL_BLE_RESPONSE_PRESENT ||
        !(row->response.flags & CERBERUS_DUAL_BLE_RESPONSE_AGE_VALID) ||
        !cerberus_dual_ble_response_row_valid(row)) return false;
    uint32_t age = cerberus_dual_ble_age_upper(row->response.age_ms, row->request_started_ms, now_ms);
    if (age == UINT32_MAX) return false;
    *out = age; return true;
}
