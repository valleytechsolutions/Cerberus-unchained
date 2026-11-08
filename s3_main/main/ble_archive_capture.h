#pragma once

#include "ble_archive.h"
#include "c5_spi_link.h"
#include "ble_rssi.h"
#include "s3_ble_discovery.h"
#include <string.h>

/* Copy an already frozen observation. This helper never queries the scanner,
 * repairs identities, refreshes source time or imports an active target. */
static inline bool cerberus_ble_archive_capture_row(cerberus_ble_archive_row_t *out,
    const cerberus_c5_ble_result_t *peer, const cerberus_c5_ble_advertisement_t *raw,
    uint8_t generation, uint8_t index, uint64_t epoch, bool epoch_valid,
    uint64_t original_ms, uint64_t ceiling_ms, bool source_valid)
{
    if (!out || !peer || !raw || peer->address_type > 3U ||
        raw->address_type != peer->address_type || memcmp(raw->address, peer->address, 6U) ||
        raw->length > CERBERUS_BLE_ARCHIVE_ADV_MAX ||
        (peer->uuid_length != 0U && peer->uuid_length != 2U &&
         peer->uuid_length != 4U && peer->uuid_length != 16U) ||
        (epoch_valid && !epoch) || (source_valid && original_ms > ceiling_ms)) return false;
    const char *end = memchr(peer->name, 0, sizeof(peer->name));
    if (!end) return false;
    cerberus_ble_archive_row_t row = {0};
    row.receiver = CERBERUS_BLE_ARCHIVE_RECEIVER_S3;
    row.rssi_kind = CERBERUS_BLE_ARCHIVE_RSSI_SWEEP_PEAK;
    memcpy(row.address, peer->address, sizeof(row.address));
    row.address_type = peer->address_type;
    memcpy(row.name, peer->name, (size_t)(end - peer->name));
    row.rssi_valid = cerberus_ble_rssi_valid(peer->rssi);
    row.rssi = row.rssi_valid ? peer->rssi : CERBERUS_BLE_ARCHIVE_RSSI_UNAVAILABLE;
    row.manufacturer_id = peer->manufacturer_id;
    row.peer_flags = peer->flags;
    row.uuid_length = peer->uuid_length;
    memcpy(row.uuid, peer->uuid, row.uuid_length);
    row.advertisement_length = raw->length;
    memcpy(row.advertisement, raw->data, raw->length);
    row.advertisement_event_type = raw->event_type;
    row.advertisement_flags = raw->flags;
    row.reports_seen = raw->reports_seen;
    row.generation = generation;
    row.index = index;
    row.scan_epoch_valid = epoch_valid;
    row.scan_epoch = epoch_valid ? epoch : 0U;
    row.source_time_valid = source_valid;
    row.original_source_uptime_ms = source_valid ? original_ms : 0U;
    row.copy_ceiling_ms = source_valid ? ceiling_ms : 0U;
    *out = row;
    return true;
}

/* Attach only the response bound to this frozen original. Missing clocks,
 * epochs or source proof leave UNKNOWN; NONE means no admitted response for
 * the proven original, not that no response existed over the air. The base
 * advertisement and its historical time are never replaced or refreshed. */
static inline bool cerberus_ble_archive_capture_response(cerberus_ble_archive_row_t *out,
    const cerberus_s3_ble_focus_t *focus, const cerberus_c5_ble_advertisement_t *raw,
    uint32_t observed_ms)
{
    if (!out || !focus || !raw || out->receiver != CERBERUS_BLE_ARCHIVE_RECEIVER_S3 ||
        out->address_type != raw->address_type || memcmp(out->address, raw->address, 6U) ||
        raw->length > sizeof(raw->data) || out->advertisement_length != raw->length ||
        out->advertisement_event_type != raw->event_type ||
        out->advertisement_flags != raw->flags || out->reports_seen != raw->reports_seen ||
        memcmp(out->advertisement, raw->data, raw->length)) return false;
    cerberus_ble_archive_row_t row = *out;
    row.response_state = CERBERUS_BLE_ARCHIVE_RESPONSE_UNKNOWN;
    row.response_length = row.response_original_length = row.response_event_type = 0U;
    row.response_flags = row.response_sid = 0U;
    row.response_sid_valid = row.response_time_valid = false;
    row.response_original_sequence = row.response_sequence = 0U;
    row.response_original_observed_ms = row.response_observed_ms = 0U;
    row.response_source_uptime_ms = 0U;
    memset(row.response, 0, sizeof(row.response));
    uint64_t source_ms = 0U;
    const cerberus_s3_ble_response_state_t state = row.scan_epoch_valid && row.scan_epoch &&
        row.source_time_valid ? cerberus_s3_ble_discovery_response(focus, raw,
            row.generation, observed_ms, row.original_source_uptime_ms,
            row.copy_ceiling_ms, &source_ms) : CERBERUS_S3_BLE_RESPONSE_UNKNOWN;
    if (state == CERBERUS_S3_BLE_RESPONSE_NONE)
        row.response_state = CERBERUS_BLE_ARCHIVE_RESPONSE_NONE;
    else if (state == CERBERUS_S3_BLE_RESPONSE_PRESENT) {
        const cerberus_s3_ble_discovery_evidence_t *e = &focus->discovery;
        row.response_state = CERBERUS_BLE_ARCHIVE_RESPONSE_PRESENT;
        row.response_length = e->scan_response.length;
        row.response_original_length = e->scan_response.original_length;
        row.response_event_type = e->scan_response.event_type;
        row.response_flags = e->scan_response.flags;
        row.response_sid_valid = (e->flags & CERBERUS_S3_BLE_EVIDENCE_SID_VALID) != 0U;
        row.response_sid = row.response_sid_valid ? e->sid : 0U;
        row.response_original_sequence = e->advertisement_sequence;
        row.response_sequence = e->scan_response_sequence;
        row.response_original_observed_ms = e->advertisement_observed_ms;
        row.response_observed_ms = e->scan_response_observed_ms;
        row.response_time_valid = true;
        row.response_source_uptime_ms = source_ms;
        memcpy(row.response, e->scan_response.data, row.response_length);
    }
    *out = row;
    return true;
}
