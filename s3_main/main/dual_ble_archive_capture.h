#pragma once

#include "ble_archive.h"
#include "dual_ble_scan.h"
#include "ble_family_wire.h"
#include "ble_response.h"
#include <string.h>

/* Pure copy from a frozen C5 publication. Source identity must travel with the
 * selected/painted row: never reconstruct it from a later scan's START/status.
 * The raw ADV is evidence, including malformed/truncated bytes. Peer metadata
 * remains independent; frozen full-family proof is copied without inferring
 * absent evidence, freshness or active eligibility.
 * No RF/SPI/clock/allocation. Invalid input leaves the output unchanged. */
static inline bool cerberus_dual_ble_archive_capture_row(cerberus_ble_archive_row_t *out,
    const cerberus_dual_ble_row_t *source,
    const cerberus_dual_ble_provenance_t *provenance, uint64_t saved_uptime_ms)
{
    if (!out || !source || !provenance || !provenance->boot_id || !provenance->epoch ||
        provenance->index >= CERBERUS_DUAL_BLE_CAPACITY || source->address_type > 3U ||
        (source->uuid_length != 0U && source->uuid_length != 2U &&
         source->uuid_length != 4U && source->uuid_length != 16U) ||
        source->raw_length > CERBERUS_DUAL_BLE_RAW_BYTES || source->event_type > 4U ||
        (source->flags & ~CERBERUS_BLE_RESULT_CONNECTABLE) ||
        (source->raw_flags & ~(CERBERUS_BLE_ADVERTISEMENT_TRUNCATED |
            CERBERUS_BLE_ADVERTISEMENT_MALFORMED)) ||
        (source->timing_flags & ~(CERBERUS_DUAL_BLE_SEEN | CERBERUS_DUAL_BLE_RSSI_VALID)) ||
        ((source->timing_flags & CERBERUS_DUAL_BLE_RSSI_VALID) &&
            (source->rssi < -127 || source->rssi > 20)) ||
        !cerberus_dual_ble_receive_metadata_valid(&source->receive,
            source->raw_length, source->event_type, source->raw_flags)) return false;
    const cerberus_dual_ble_family_wire_t absent = {0};
    if (memcmp(&source->family, &absent, sizeof(absent))) {
        cerberus_dual_ble_family_evidence_t evidence;
        if (!cerberus_dual_ble_family_wire_decode(&source->family, &source->receive,
                source->raw_length, source->event_type, source->raw_flags,
                source->address, source->address_type, &evidence)) return false;
    }
    if (!cerberus_dual_ble_response_source_valid(&source->response, &source->receive,
            source->raw_length, source->event_type, source->raw_flags,
            source->address, source->address_type,
            source->family.version ? source->family.family_code : 0U)) return false;
    if (source->response.state == CERBERUS_DUAL_BLE_RESPONSE_PRESENT &&
        (source->response.flags & CERBERUS_DUAL_BLE_RESPONSE_AGE_VALID) &&
        (source->timing_flags & CERBERUS_DUAL_BLE_SEEN)) {
        if (source->callback_age_ms == UINT32_MAX || source->response.age_ms > source->callback_age_ms)
            return false;
        const uint32_t difference = source->callback_age_ms - source->response.age_ms;
        if (difference < source->response.interval_us / 1000U ||
            difference > (source->response.interval_us + 999U) / 1000U) return false;
    }
    const char *end = memchr(source->name, 0, sizeof(source->name));
    if (!end) return false;

    cerberus_ble_archive_row_t row = {0};
    row.receiver = CERBERUS_BLE_ARCHIVE_RECEIVER_C5;
    row.rssi_kind = CERBERUS_BLE_ARCHIVE_RSSI_LATEST_ADV;
    memcpy(row.address, source->address, sizeof(row.address));
    row.address_type = source->address_type;
    memcpy(row.name, source->name, (size_t)(end - source->name));
    row.rssi_valid = (source->timing_flags & CERBERUS_DUAL_BLE_RSSI_VALID) != 0U;
    row.rssi = row.rssi_valid ? source->rssi : CERBERUS_BLE_ARCHIVE_RSSI_UNAVAILABLE;
    row.manufacturer_id = source->manufacturer_id;
    row.peer_flags = source->flags;
    row.uuid_length = source->uuid_length;
    memcpy(row.uuid, source->uuid, row.uuid_length);
    row.advertisement_length = source->raw_length;
    memcpy(row.advertisement, source->raw, row.advertisement_length);
    row.advertisement_event_type = source->event_type;
    row.advertisement_flags = source->raw_flags;
    /* Frozen same-original metadata; UNKNOWN from older peers stays UNKNOWN.
     * Never infer receive provenance from current scanner state or raw hints. */
    row.receive = source->receive;
    row.family = source->family;
    row.c5_response = source->response;
    row.reports_seen = source->reports;
    row.generation = provenance->generation;
    row.index = provenance->index;
    row.receiver_boot_id = provenance->boot_id;
    row.scan_epoch = provenance->epoch;
    row.scan_epoch_valid = true;
    /* C5 and S3 clocks are distinct. Retain only an upper age bound at save;
     * exact S3 source/copy timestamps remain explicitly unknown and zero.
     * A C5 observation can predate S3 boot, so age may exceed saved uptime. */
    if ((source->timing_flags & CERBERUS_DUAL_BLE_SEEN) &&
        source->callback_age_ms != UINT32_MAX && saved_uptime_ms >= source->request_started_ms) {
        uint64_t elapsed = saved_uptime_ms - source->request_started_ms;
        if (elapsed <= UINT64_MAX - source->callback_age_ms) {
            row.age_upper_at_save_ms = elapsed + source->callback_age_ms;
            row.age_upper_valid = true;
        }
    }
    if (source->response.state == CERBERUS_DUAL_BLE_RESPONSE_PRESENT &&
        (source->response.flags & CERBERUS_DUAL_BLE_RESPONSE_AGE_VALID) &&
        saved_uptime_ms >= source->request_started_ms) {
        const uint64_t elapsed = saved_uptime_ms - source->request_started_ms;
        if (elapsed <= UINT64_MAX - source->response.age_ms) {
            row.c5_response_age_upper_at_save_ms = elapsed + source->response.age_ms;
            row.c5_response_age_upper_valid = true;
        }
    }
    *out = row;
    return true;
}
