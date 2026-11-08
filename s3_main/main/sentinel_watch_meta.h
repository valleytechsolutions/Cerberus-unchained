#pragma once

/* Project existing native Meta evidence; this is not a detector or parser.
 * The UI must first validate its accepted Recon passive lease and the entire
 * frozen bank (including generation/index/typed identity). Equal epoch values
 * below are an additional stale-copy guard, never radio ownership authority.
 * Only the current original may qualify; no response-only/name/OUI inference.
 * A false result leaves the caller's match unchanged. */
#include "sentinel_watch_detect.h"
#include "s3_ble_snapshot.h"
#include "c5_spi_link.h"
#include <string.h>

static inline bool sentinel_watch_project_meta(
    const cerberus_s3_ble_focus_t *focus,
    const cerberus_c5_ble_advertisement_t *raw,
    const cerberus_c5_ble_result_t *peer, uint8_t generation,
    uint32_t observed_ms, uint64_t captured_epoch, uint64_t accepted_epoch,
    sentinel_watch_match_t *match)
{
    if (!focus || !raw || !peer || !match || !captured_epoch ||
        captured_epoch != accepted_epoch ||
        match->category != SENTINEL_WATCH_NONE ||
        match->confidence != SENTINEL_WATCH_CONFIDENCE_NONE ||
        match->rule_id || match->label || match->evidence ||
        peer->address_type > 3U || raw->address_type != peer->address_type ||
        memcmp(raw->address, peer->address, sizeof(peer->address)) ||
        raw->length > sizeof(raw->data) || raw->event_type > 3U ||
        (raw->flags & ~CERBERUS_BLE_ADVERTISEMENT_TRUNCATED) ||
        ((raw->flags & CERBERUS_BLE_ADVERTISEMENT_TRUNCATED) &&
         raw->length != sizeof(raw->data))) return false;
    const cerberus_s3_ble_discovery_evidence_t *source = &focus->discovery;
    const uint8_t required = CERBERUS_S3_BLE_EVIDENCE_ADV_COMPLETE |
        CERBERUS_S3_BLE_EVIDENCE_ADV_VALID;
    if (!cerberus_s3_ble_response_storage_valid(source) ||
        (source->flags & required) != required || (source->flags & ~0x3fU) ||
        (source->flags & CERBERUS_S3_BLE_EVIDENCE_SID_INVALID) ||
        ((source->flags & CERBERUS_S3_BLE_EVIDENCE_SID_VALID) && source->sid > 15U) ||
        source->generation != generation || source->address_type != peer->address_type ||
        memcmp(source->address, peer->address, sizeof(peer->address)) ||
        !source->advertisement_sequence ||
        source->raw_advertisement_sequence != source->advertisement_sequence ||
        source->advertisement_observed_ms != observed_ms ||
        ((source->flags & CERBERUS_S3_BLE_EVIDENCE_RAW_COMPLETE) != 0U) ==
            ((raw->flags & CERBERUS_BLE_ADVERTISEMENT_TRUNCATED) != 0U) ||
        !(focus->category_bits & CERBERUS_S3_BLE_FOCUS_META_HINT) ||
        focus->meta_suppressed ||
        (source->meta_advertisement.pairs[9] & 0xe0U) ||
        !cerberus_ble_meta_evidence_allowed(&source->meta_advertisement) ||
        cerberus_ble_meta_evidence_blocked(&source->meta_advertisement) ||
        cerberus_ble_meta_evidence_blocked(&source->meta_scan_response)) return false;
    *match = (sentinel_watch_match_t){SENTINEL_WATCH_SMART_GLASSES,
        SENTINEL_WATCH_CONFIDENCE_LOW, "meta.original-id-hint",
        "Meta identifier hint", "Allowed identifier/context in current original / not a model ID"};
    return true;
}
