#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "dual_ble_protocol.h"

/* Portable, bounded BLE observation evidence. No radio, filesystem, allocation,
 * ESP-IDF types, native struct serialization, or active-target eligibility.
 * This is a saved snapshot, not a callback stream or a live scanner bank. */
enum {
    CERBERUS_BLE_ARCHIVE_LEGACY_VERSION = 1,
    CERBERUS_BLE_ARCHIVE_V2_VERSION = 2,
    CERBERUS_BLE_ARCHIVE_V3_VERSION = 3,
    CERBERUS_BLE_ARCHIVE_V4_VERSION = 4,
    CERBERUS_BLE_ARCHIVE_V5_VERSION = 5,
    CERBERUS_BLE_ARCHIVE_VERSION = 6,
    CERBERUS_BLE_ARCHIVE_MAX_ROWS_PER_RECEIVER = 32,
    CERBERUS_BLE_ARCHIVE_MAX_ROWS = 64,
    CERBERUS_BLE_ARCHIVE_NAME_MAX = 32,
    CERBERUS_BLE_ARCHIVE_UUID_MAX = 16,
    CERBERUS_BLE_ARCHIVE_ADV_MAX = 31,
    CERBERUS_BLE_ARCHIVE_HEADER_SIZE = 32,
    CERBERUS_BLE_ARCHIVE_LEGACY_ROW_SIZE = 128,
    CERBERUS_BLE_ARCHIVE_LEGACY_MAX_SIZE = 4128,
    CERBERUS_BLE_ARCHIVE_V2_ROW_SIZE = 144,
    CERBERUS_BLE_ARCHIVE_V2_MAX_SIZE = 9248,
    CERBERUS_BLE_ARCHIVE_V3_ROW_SIZE = 208,
    CERBERUS_BLE_ARCHIVE_V3_MAX_SIZE = 13344,
    CERBERUS_BLE_ARCHIVE_V4_ROW_SIZE = 216,
    CERBERUS_BLE_ARCHIVE_V4_MAX_SIZE = 13856,
    CERBERUS_BLE_ARCHIVE_V5_ROW_SIZE = 264,
    CERBERUS_BLE_ARCHIVE_V5_MAX_SIZE = 16928,
    CERBERUS_BLE_ARCHIVE_ROW_SIZE = 432,
    CERBERUS_BLE_ARCHIVE_MAX_SIZE = 27680,
    CERBERUS_BLE_ARCHIVE_RSSI_UNAVAILABLE = 127,
};
enum {
    CERBERUS_BLE_ARCHIVE_RECEIVER_LEGACY = 0,
    CERBERUS_BLE_ARCHIVE_RECEIVER_S3 = 1,
    CERBERUS_BLE_ARCHIVE_RECEIVER_C5 = 2,
};
enum {
    CERBERUS_BLE_ARCHIVE_RSSI_SWEEP_PEAK = 0,
    CERBERUS_BLE_ARCHIVE_RSSI_LATEST_ADV = 1,
};
enum {
    CERBERUS_BLE_ARCHIVE_RESPONSE_UNKNOWN = 0,
    CERBERUS_BLE_ARCHIVE_RESPONSE_NONE = 1,
    CERBERUS_BLE_ARCHIVE_RESPONSE_PRESENT = 2,
};

typedef struct {
    /* NimBLE/native byte order, least significant address octet first. The
     * address type is part of identity: 0 public, 1 random, 2/3 identity types. */
    uint8_t address[6], address_type;
    /* A bounded received peer C-string projection, not the complete on-air
     * name: prior peer conversion can lose bytes after an embedded NUL.
     * Non-ASCII/control bytes remain intact here; rendering must sanitize. */
    char name[CERBERUS_BLE_ARCHIVE_NAME_MAX + 1];
    int8_t rssi;
    bool rssi_valid; /* Semantics are explicit in rssi_kind below. */
    uint16_t manufacturer_id;
    uint8_t peer_flags;
    uint8_t uuid_length, uuid[CERBERUS_BLE_ARCHIVE_UUID_MAX];
    /* Original retained ADV only. Malformed/truncated/unknown bytes and flag
     * bits remain evidence. A matched response is stored separately below. */
    uint8_t advertisement_length, advertisement[CERBERUS_BLE_ARCHIVE_ADV_MAX];
    uint8_t advertisement_event_type, advertisement_flags;
    uint16_t reports_seen;
    uint8_t generation, index;
    uint64_t scan_epoch;
    bool scan_epoch_valid;
    /* Historical S3 uptime from the SAME boot as saved_uptime_ms. The copy
     * ceiling bounds the retained source; it is not a packet timestamp.
     * Unknown source time is explicit, never inferred from generation/index.
     * Invalid source/epoch fields must be zero; invalid RSSI must be 127. */
    uint64_t original_source_uptime_ms, copy_ceiling_ms;
    bool source_time_valid;
    /* LEGACY is v1's unspecified receiver; never infer a new S3 source label.
     * C5 carries latest original-ADV RSSI, its committed boot/epoch identity,
     * and an optional conservative age at SAVE. Its exact S3 time fields must
     * be zero/invalid. That upper bound may exceed saved S3 uptime. S3/LEGACY
     * use peak RSSI and must leave C5 bound/boot fields zero/invalid. */
    uint8_t receiver, rssi_kind;
    uint64_t age_upper_at_save_ms, receiver_boot_id;
    bool age_upper_valid;
    /* UNKNOWN includes old files, C5/LEGACY and unavailable correlation proof.
     * NONE means this frozen S3 original has no admitted matched response.
     * PRESENT is bound to this row's typed address/generation and the original
     * sequence/time below. Original/response bytes and source clocks never
     * replace each other. Non-PRESENT fields are all zero. */
    uint8_t response_state, response_length, response_original_length;
    uint8_t response_event_type, response_flags, response_sid;
    bool response_sid_valid, response_time_valid;
    uint32_t response_original_sequence, response_sequence;
    uint32_t response_original_observed_ms, response_observed_ms;
    uint64_t response_source_uptime_ms;
    uint8_t response[CERBERUS_BLE_ARCHIVE_ADV_MAX];
    /* Same-original C5 callback envelope, separate from aggregate peer hints.
     * Known values must match this row's raw length/event/flags. S3, LEGACY
     * and all pre-v4 files retain canonical allzero UNKNOWN; neither a saved
     * peer boot/epoch nor a raw prefix reconstructs absent receive metadata. */
    cerberus_dual_ble_receive_metadata_t receive;
    /* Optional frozen C5 full-callback family evidence, canonical wire48.
     * Allzero means absent (including every pre-v5 file); no raw/peer fields
     * reconstruct it. Nonzero evidence must validate against THIS typed row,
     * receive envelope and boot/epoch source proof. S3/LEGACY require allzero.
     * A canonical unavailable/nonmatching proof preserves dequalification;
     * it must not fall back to an earlier or short-preview positive hint. */
    cerberus_dual_ble_family_wire_t family;
    /* v6: separate C5 response domain. The older S3 response fields above
     * remain UNKNOWN for C5. Pre-v6/S3/LEGACY have canonical allzero C5
     * response evidence. A response never replaces original bytes/time/RSSI.
     * This is local callback correlation, not an authenticated air exchange. */
    cerberus_dual_ble_response_wire_t c5_response;
    uint64_t c5_response_age_upper_at_save_ms;
    bool c5_response_age_upper_valid;
} cerberus_ble_archive_row_t;

typedef struct {
    uint64_t saved_uptime_ms;
    uint8_t count; /* 1..64, at most32/source; duplicate receiver+typed addresses rejected. */
    cerberus_ble_archive_row_t rows[CERBERUS_BLE_ARCHIVE_MAX_ROWS];
} cerberus_ble_archive_t;

bool cerberus_ble_archive_validate(const cerberus_ble_archive_t *document);

/* Encode v6: header32 + count*row432. Decode also accepts v5 row264,
 * v4 row216, v3 row208, v2 row144
 * and v1 row128/count<=32, mapping v1's unspecified source to LEGACY. V3
 * response evidence remains intact. Pre-v3 rows have UNKNOWN response;
 * all pre-v4 rows have UNKNOWN receive metadata. Every pre-v5 row has absent
 * allzero full-family proof, never fabricated provenance. Pre-v6 rows have
 * allzero UNKNOWN C5 response evidence and unknown response age bounds.
 * Little-endian integers, zero padding
 * and reserved bytes. CRC-32/ISO-HDLC covers ALL encoded bytes, treating the
 * header CRC field as zero. This detects corruption, not authenticity.
 * Encode failure leaves output bytes unchanged and sets *written=0 when given
 * unless written itself illegally overlaps the input or output buffer.
 * Decode failure leaves the entire previous document unchanged. No scratch
 * document or heap is required: a complete preflight precedes publication.
 * Input and output memory must not overlap (overlap is rejected). The caller
 * must keep input immutable throughout either operation. */
bool cerberus_ble_archive_encode(const cerberus_ble_archive_t *document,
    uint8_t *output, size_t capacity, size_t *written);
bool cerberus_ble_archive_decode(const uint8_t *input, size_t length,
    cerberus_ble_archive_t *document);

/* Historical age at SAVE only. Never compare a loaded document with this
 * boot's clock or present it as current freshness. Zero age is valid.
 * Failure leaves *age_ms unchanged. */
bool cerberus_ble_archive_age_at_save(const cerberus_ble_archive_t *document,
    size_t index, uint64_t *age_ms);
/* C5 conservative age at SAVE, never an exact packet timestamp. Exact-age
 * helper above rejects C5; this helper rejects S3/LEGACY and unknown bounds.
 * Neither helper uses the loading boot's clock. Failure leaves output intact. */
bool cerberus_ble_archive_age_upper_at_save(const cerberus_ble_archive_t *document,
    size_t index, uint64_t *age_ms);
/* Exact historical age of a PRESENT S3 response with a valid independent
 * source time. Rejects unavailable response/time; leaves output untouched. */
bool cerberus_ble_archive_response_age_at_save(const cerberus_ble_archive_t *document,
    size_t index, uint64_t *age_ms);
/* Independent conservative C5 response age at SAVE; never subtract from S3
 * uptime to invent a timestamp. Rejects unknown bounds and other receivers. */
bool cerberus_ble_archive_c5_response_age_upper_at_save(const cerberus_ble_archive_t *document,
    size_t index, uint64_t *age_ms);
