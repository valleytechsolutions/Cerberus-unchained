#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Complementary observer. Only explicit response modes solicit scan responses
 * on1M; all older modes stay passive. No mode here authorizes a connection,
 * pairing, advertising or GATT operation. Times are C5 callback ages. */
#define CERBERUS_DUAL_BLE_LEGACY_VERSION 1U
#define CERBERUS_DUAL_BLE_VERSION 2U
#define CERBERUS_DUAL_BLE_MIN_FIRMWARE 61U
#define CERBERUS_DUAL_BLE_SNAPSHOT_MIN_FIRMWARE 62U
#define CERBERUS_DUAL_BLE_INVENTORY_MIN_FIRMWARE 63U
#define CERBERUS_DUAL_BLE_METADATA_MIN_FIRMWARE 64U
#define CERBERUS_DUAL_BLE_ANALYZER_MIN_FIRMWARE 65U
#define CERBERUS_DUAL_BLE_FAMILY_MIN_FIRMWARE 66U
#define CERBERUS_DUAL_BLE_RESPONSE_MIN_FIRMWARE 67U
#define CERBERUS_DUAL_BLE_COMPACT_RESPONSE_MIN_FIRMWARE 68U
#define CERBERUS_DUAL_BLE_REACQUIRE_MIN_FIRMWARE 84U
#define CERBERUS_DUAL_BLE_ANY_OBSERVED_TYPE 0xffU
#define CERBERUS_DUAL_BLE_CAPACITY 32U
#define CERBERUS_DUAL_BLE_RAW_BYTES 31U
#define CERBERUS_DUAL_BLE_STALE_MS 5000U
#define CERBERUS_DUAL_BLE_ORPHAN_MS 12000U
enum { CERBERUS_DUAL_BLE_DISCOVERY, CERBERUS_DUAL_BLE_TRACK, CERBERUS_DUAL_BLE_ANALYZER, CERBERUS_DUAL_BLE_FAMILY,
       CERBERUS_DUAL_BLE_RESPONSE_DISCOVERY, CERBERUS_DUAL_BLE_RESPONSE_FAMILY,
       CERBERUS_DUAL_BLE_REACQUIRE };

static inline bool cerberus_dual_ble_response_mode(uint8_t mode)
{ return mode == CERBERUS_DUAL_BLE_RESPONSE_DISCOVERY || mode == CERBERUS_DUAL_BLE_RESPONSE_FAMILY; }
static inline bool cerberus_dual_ble_family_mode(uint8_t mode)
{ return mode == CERBERUS_DUAL_BLE_FAMILY || mode == CERBERUS_DUAL_BLE_RESPONSE_FAMILY; }
static inline bool cerberus_dual_ble_snapshot_mode(uint8_t mode)
{ return mode == CERBERUS_DUAL_BLE_DISCOVERY || cerberus_dual_ble_family_mode(mode) ||
    cerberus_dual_ble_response_mode(mode) || mode == CERBERUS_DUAL_BLE_REACQUIRE; }

enum { CERBERUS_DUAL_BLE_ATTACHED = 1U, CERBERUS_DUAL_BLE_OWNED = 2U,
       CERBERUS_DUAL_BLE_EXTENDED = 4U, CERBERUS_DUAL_BLE_READY = 8U,
       CERBERUS_DUAL_BLE_SNAPSHOTS = 16U };
enum { CERBERUS_DUAL_BLE_SEEN = 1U, CERBERUS_DUAL_BLE_RSSI_VALID = 2U };
enum { CERBERUS_DUAL_BLE_SNAPSHOT_RUNNING = 1U, CERBERUS_DUAL_BLE_SNAPSHOT_COMPLETE = 2U,
       CERBERUS_DUAL_BLE_SNAPSHOT_EXTENDED = 4U };
enum { CERBERUS_DUAL_BLE_SNAPSHOT_OK, CERBERUS_DUAL_BLE_SNAPSHOT_BUSY,
       CERBERUS_DUAL_BLE_SNAPSHOT_STALE, CERBERUS_DUAL_BLE_SNAPSHOT_RANGE,
       CERBERUS_DUAL_BLE_SNAPSHOT_INVALID };
#define CERBERUS_DUAL_BLE_SNAPSHOT_CHUNK_BYTES 10U
#define CERBERUS_DUAL_BLE_SNAPSHOT_ROW_BYTES 102U
#define CERBERUS_DUAL_BLE_SNAPSHOT_CHUNKS 11U
#define CERBERUS_DUAL_BLE_SNAPSHOT_METADATA_ROW_BYTES 110U
#define CERBERUS_DUAL_BLE_SNAPSHOT_METADATA_CHUNKS 11U
#define CERBERUS_DUAL_BLE_SNAPSHOT_FAMILY_ROW_BYTES 158U
#define CERBERUS_DUAL_BLE_SNAPSHOT_FAMILY_CHUNKS 16U
#define CERBERUS_DUAL_BLE_SNAPSHOT_RESPONSE_ROW_BYTES 310U
#define CERBERUS_DUAL_BLE_SNAPSHOT_RESPONSE_CHUNKS 31U

enum { CERBERUS_DUAL_BLE_RECEIVE_UNKNOWN, CERBERUS_DUAL_BLE_RECEIVE_LEGACY,
       CERBERUS_DUAL_BLE_RECEIVE_EXTENDED };
enum { CERBERUS_DUAL_BLE_AD_UNKNOWN, CERBERUS_DUAL_BLE_AD_VALID,
       CERBERUS_DUAL_BLE_AD_INVALID };
/* Provenance of the SAME retained original as raw/RSSI/age, never peer hints.
 * callback_length is the delivered SDK report length, not a reconstructed
 * advertising train. UNKNOWN is canonical allzero. Legacy has no controller
 * data-status/SID/PHY observation: status/SID=0xff, PHY/properties=0.
 * Known EXT metadata describes complete originals only. A preview boundary
 * may mark raw MALFORMED despite independently valid full AD layout. */
typedef struct __attribute__((packed)) {
    uint8_t source_kind, properties, callback_length, sid;
    uint8_t primary_phy, secondary_phy, data_status, ad_validation;
} cerberus_dual_ble_receive_metadata_t;

static inline bool cerberus_dual_ble_receive_metadata_valid(
    const cerberus_dual_ble_receive_metadata_t *m,
    uint8_t raw_length, uint8_t event_type, uint8_t raw_flags)
{
    if (!m) return false;
    if (m->source_kind == CERBERUS_DUAL_BLE_RECEIVE_UNKNOWN)
        return !m->properties && !m->callback_length && !m->sid &&
            !m->primary_phy && !m->secondary_phy && !m->data_status && !m->ad_validation;
    /* Raw flag bit0 is preview truncation; bit1 is existing raw malformed.
     * Do not depend on the larger protocol header or reinterpret bit1 here. */
    const uint8_t preview = m->callback_length < CERBERUS_DUAL_BLE_RAW_BYTES ?
        m->callback_length : CERBERUS_DUAL_BLE_RAW_BYTES;
    if (event_type > 3U || raw_length != preview || (raw_flags & ~3U) ||
        ((raw_flags & 1U) != 0U) != (m->callback_length > CERBERUS_DUAL_BLE_RAW_BYTES) ||
        (m->ad_validation != CERBERUS_DUAL_BLE_AD_VALID &&
         m->ad_validation != CERBERUS_DUAL_BLE_AD_INVALID)) return false;
    if (m->source_kind == CERBERUS_DUAL_BLE_RECEIVE_LEGACY)
        return !m->properties && m->callback_length <= CERBERUS_DUAL_BLE_RAW_BYTES &&
            m->sid == 0xffU && !m->primary_phy && !m->secondary_phy && m->data_status == 0xffU;
    if (m->source_kind != CERBERUS_DUAL_BLE_RECEIVE_EXTENDED ||
        (m->properties & ~0x1fU) || (m->properties & 0x08U) || m->data_status ||
        (m->sid > 15U && m->sid != 0xffU) ||
        (m->primary_phy != 1U && m->primary_phy != 3U) || m->secondary_phy > 3U) return false;
    if (m->properties & 0x10U) {
        if (m->callback_length > CERBERUS_DUAL_BLE_RAW_BYTES || m->sid != 0xffU ||
            m->primary_phy != 1U || m->secondary_phy) return false;
        return (m->properties == 0x10U && event_type == 3U) ||
            (m->properties == 0x12U && event_type == 2U) ||
            (m->properties == 0x13U && event_type == 0U) ||
            (m->properties == 0x15U && event_type == 1U);
    }
    return event_type == ((m->properties & 1U) ? 0U : 3U);
}

typedef struct __attribute__((packed)) {
    uint64_t boot_id;
    uint32_t source_now_ms;
    uint32_t next_job_id; /* C5-issued, never reused during this boot. Zero: exhausted. */
    uint8_t version, capacity, raw_bytes, flags, reserved[2];
} cerberus_dual_ble_info_t;
typedef struct __attribute__((packed)) {
    uint64_t boot_id;
    uint32_t job_id;
    /* REACQUIRE is passive address-only admission. ANY_OBSERVED_TYPE requests
     * separately observed types0..3; it is never a connection address type.
     * Saved payloads/types grant no current identity or action permission. */
    uint8_t address[6], address_type, mode, version, reserved; /* FAMILY: selected code1..6; other modes:0. */
} cerberus_dual_ble_start_t;
typedef struct __attribute__((packed)) {
    uint64_t boot_id;
    uint32_t job_id;
} cerberus_dual_ble_binding_t;
typedef struct __attribute__((packed)) {
    uint64_t epoch;
    uint32_t job_id;
    int32_t error_code;
    uint8_t state, generation, count, flags, version, reserved;
} cerberus_dual_ble_status_t;
typedef struct __attribute__((packed)) {
    cerberus_dual_ble_binding_t binding;
    uint8_t index, reserved;
} cerberus_dual_ble_row_request_t;
typedef struct __attribute__((packed)) {
    uint64_t epoch;
    uint32_t job_id, age_ms;
    uint8_t index, generation;
    int8_t latest_rssi;
    uint8_t flags;
    uint16_t reports;
} cerberus_dual_ble_row_time_t;
typedef struct __attribute__((packed)) {
    uint64_t epoch;
    uint32_t job_id, age_ms, reports;
    int8_t rssi;
    uint8_t flags;
} cerberus_dual_ble_track_t;

enum { CERBERUS_DUAL_BLE_ANALYZER_VALID = 1U, CERBERUS_DUAL_BLE_ANALYZER_BUSY = 2U,
       CERBERUS_DUAL_BLE_ANALYZER_SATURATED = 4U, CERBERUS_DUAL_BLE_ANALYZER_FINAL = 8U,
       CERBERUS_DUAL_BLE_ANALYZER_RSSI_VALID = 16U, CERBERUS_DUAL_BLE_ANALYZER_CLOCK_INVALID = 32U };
/* Firmware65: request is binding12. Counts typed original callbacks, including
 * partial EXT reports; not packets, devices, classified ADs or inventory rows.
 * elapsed_ms is C5 time since the actual scan-start boundary, frozen at finish.
 * Saturated counts are lower bounds. Rate intervals must not cross full epochs.
 * Invalid replies have zero counts/time and RSSI127. BUSY/stale have epoch0;
 * pending-start has bound epoch/flags0; bad clock has only CLOCK_INVALID.
 * Valid latest-callback RSSI is separate from earlier valid RSSI observations. */
typedef struct __attribute__((packed)) {
    uint64_t epoch;
    uint32_t job_id, reports, elapsed_ms;
    uint8_t flags;
    int8_t rssi;
} cerberus_dual_ble_analyzer_sample_t;

/* v2: a per-job strict request counter makes lost BEGIN replies idempotent.
 * C5-issued handles never repeat during a boot and map to boot/job/full epoch.
 * A successful next BEGIN replaces the sole frozen bank; reads never do. */
typedef struct __attribute__((packed)) {
    cerberus_dual_ble_binding_t binding;
    uint64_t request_id;
} cerberus_dual_ble_snapshot_begin_t;
typedef struct __attribute__((packed)) {
    cerberus_dual_ble_binding_t binding;
    uint64_t handle;
} cerberus_dual_ble_snapshot_binding_t;
typedef struct __attribute__((packed)) {
    cerberus_dual_ble_binding_t binding;
    uint64_t handle;
    uint8_t index, chunk;
} cerberus_dual_ble_snapshot_read_t;
typedef struct __attribute__((packed)) {
    uint64_t boot_id, handle;
    int32_t error_code;
    uint8_t count, flags;
} cerberus_dual_ble_snapshot_status_t;

/* Optional firmware63 extension; INFO v2 and its flags remain unchanged for
 * older hosts. Request uses snapshot_binding: boot/job/full-epoch mapping is
 * validated through the frozen handle. Counters describe this RF sweep at
 * FIRST BEGIN, not distinct devices and not cumulative session totals.
 * Dropped counts typed complete-original callbacks for unretained addresses
 * rejected at a full bank; stale epochs, responses, partial extended reports
 * and invalid address types are excluded. FAMILY also excludes filtered-out
 * unrelated originals. Response modes also count qualified pending-original
 * promotions driven by a matched response when they replace/drop a slot;
 * these are admission events, not physical devices or original-report counts.
 * Counters saturate at UINT32_MAX. */
enum { CERBERUS_DUAL_BLE_INVENTORY_REPLACEMENT = 1U };
typedef struct __attribute__((packed)) {
    uint64_t handle;
    uint32_t replaced, dropped;
    int32_t error_code;
    uint8_t count, flags;
} cerberus_dual_ble_snapshot_inventory_t;
typedef struct __attribute__((packed)) {
    uint64_t handle;
    uint8_t index, chunk, byte_count, error;
    uint8_t data[CERBERUS_DUAL_BLE_SNAPSHOT_CHUNK_BYTES];
} cerberus_dual_ble_snapshot_chunk_t;
/* Peer name/UUID/manufacturer are accumulated peer hints. Raw/RSSI/time are
 * one latest original ADV observation; these are not synthetic packet fields.
 * age_ms is frozen at the first BEGIN. The S3 must keep that request's clock
 * anchor across retries, rather than treating an idempotent reply as newer. */
typedef struct __attribute__((packed)) {
    uint8_t address[6], address_type;
    int8_t latest_rssi;
    uint8_t name_length, uuid_length, name[32], uuid[16];
    uint16_t manufacturer_id;
    uint8_t peer_flags, raw[31], raw_length, event_type, raw_flags;
    uint16_t reports;
    uint32_t age_ms;
    uint8_t timing_flags;
} cerberus_dual_ble_snapshot_row_t;

/* Firmware64 opt-in READ_METADATA only. Existing READ retains102 bytes and
 * its two-byte final chunk. This virtual110-byte projection uses the same11
 * chunks: the final chunk contains the old two bytes followed by metadata8. */
typedef struct __attribute__((packed)) {
    cerberus_dual_ble_snapshot_row_t row;
    cerberus_dual_ble_receive_metadata_t receive;
} cerberus_dual_ble_snapshot_metadata_row_t;

/* Firmware66 FAMILY-only evidence from the same full complete callback.
 * Byte fields are canonical, multibyte tuple fields explicitly little endian.
 * Version0 is allzero/absent for older and ordinary scan rows. */
typedef struct __attribute__((packed)) {
    uint8_t version, family_code, state, categories, hc_name, ibeacon_result, flags;
    uint8_t findmy_bits, findmy_uuid_widths, flipper_bits, flipper_flags;
    uint8_t flock_facts, flock_accessory_contexts;
    uint8_t flock_raven_3100_contexts, flock_raven_3200_contexts;
    uint8_t meta_pairs[10], ibeacon_uuid[16];
    uint8_t ibeacon_major_le[2], ibeacon_minor_le[2];
    int8_t ibeacon_power;
    uint8_t reserved[2];
} cerberus_dual_ble_family_wire_t;

typedef struct __attribute__((packed)) {
    cerberus_dual_ble_snapshot_metadata_row_t metadata;
    cerberus_dual_ble_family_wire_t family;
} cerberus_dual_ble_snapshot_family_row_t;

enum { CERBERUS_DUAL_BLE_RESPONSE_UNKNOWN, CERBERUS_DUAL_BLE_RESPONSE_NONE,
       CERBERUS_DUAL_BLE_RESPONSE_PRESENT };
enum { CERBERUS_DUAL_BLE_RESPONSE_AGE_VALID = 1U, CERBERUS_DUAL_BLE_RESPONSE_RSSI_VALID = 2U,
       CERBERUS_DUAL_BLE_RESPONSE_NAME_COMPLETE = 4U, CERBERUS_DUAL_BLE_RESPONSE_NAME_TRUNCATED = 8U };
#define CERBERUS_DUAL_BLE_RESPONSE_VERSION 1U
#define CERBERUS_DUAL_BLE_RESPONSE_INTERVAL_US 2000000U
/* Explicit response acquisition only: active1M/passive-coded. This separate
 * source never refreshes the retained original. Sequence/time are local C5
 * callback correlation, not an authenticated exchange or physical identity.
 * UNKNOWN is allzero; NONE is version/state plus the eligible retained original sequence. PRESENT uses one complete
 * matched response, full-container hints and its separate bounded preview.
 * Generic discovery uses family_code0 in the dedicated response domain. */
typedef struct __attribute__((packed)) {
    uint8_t version, state, flags, raw_length, event_type, name_length;
    int8_t rssi;
    uint8_t raw_flags;
    uint64_t original_sequence, response_sequence;
    uint32_t age_ms, interval_us;
    cerberus_dual_ble_receive_metadata_t receive;
    uint8_t raw[31], name[32];
    cerberus_dual_ble_family_wire_t family;
    uint8_t reserved_tail;
} cerberus_dual_ble_response_wire_t;
typedef struct __attribute__((packed)) {
    cerberus_dual_ble_snapshot_family_row_t family;
    cerberus_dual_ble_response_wire_t response;
} cerberus_dual_ble_snapshot_response_row_t;
_Static_assert(sizeof(cerberus_dual_ble_response_wire_t) == 152U, "response sidecar layout");
_Static_assert(sizeof(cerberus_dual_ble_snapshot_response_row_t) == 310U, "response projection layout");

/* Optional firmware68 projection; INFO/version2, response acquisition and the
 * full READ_RESPONSE representation remain unchanged. Bytes0..157 are the
 * same frozen original prefix. Bytes158/159 are this format's version/state,
 * including UNKNOWN (1/0 on wire, canonical allzero sidecar after decoding).
 * NONE retains bytes0..15 of its sidecar, including LE64 original_sequence.
 * PRESENT retains all152 bytes. Omitted bytes are canonical zero, never an
 * inference from a short transfer. This command preserves the frozen handle. */
#define CERBERUS_DUAL_BLE_COMPACT_RESPONSE_VERSION 1U
#define CERBERUS_DUAL_BLE_COMPACT_RESPONSE_UNKNOWN_BYTES 160U
#define CERBERUS_DUAL_BLE_COMPACT_RESPONSE_NONE_BYTES 174U
_Static_assert(offsetof(cerberus_dual_ble_snapshot_response_row_t, response) == 158U,
    "compact response prefix");
_Static_assert(offsetof(cerberus_dual_ble_response_wire_t, original_sequence) == 8U &&
    offsetof(cerberus_dual_ble_response_wire_t, response_sequence) == 16U,
    "compact NONE retains full original sequence");

static inline uint16_t cerberus_dual_ble_compact_response_wire_size(uint8_t version, uint8_t state)
{
    if (version != CERBERUS_DUAL_BLE_COMPACT_RESPONSE_VERSION) return 0U;
    switch (state) {
    case CERBERUS_DUAL_BLE_RESPONSE_UNKNOWN: return CERBERUS_DUAL_BLE_COMPACT_RESPONSE_UNKNOWN_BYTES;
    case CERBERUS_DUAL_BLE_RESPONSE_NONE: return CERBERUS_DUAL_BLE_COMPACT_RESPONSE_NONE_BYTES;
    case CERBERUS_DUAL_BLE_RESPONSE_PRESENT: return CERBERUS_DUAL_BLE_SNAPSHOT_RESPONSE_ROW_BYTES;
    default: return 0U;
    }
}

/* Producer-side canonical omission check over the frozen LE byte sidecar.
 * No row copy or unaligned multibyte read. PRESENT is not shortened; its
 * existing full semantic validator remains mandatory at the receiver. */
static inline uint16_t cerberus_dual_ble_compact_response_size(const uint8_t response[152])
{
    if (!response) return 0U;
    if (response[1] == CERBERUS_DUAL_BLE_RESPONSE_UNKNOWN) {
        for (size_t i = 0U; i < 152U; ++i) if (response[i]) return 0U;
        return CERBERUS_DUAL_BLE_COMPACT_RESPONSE_UNKNOWN_BYTES;
    }
    if (response[0] != CERBERUS_DUAL_BLE_RESPONSE_VERSION) return 0U;
    if (response[1] == CERBERUS_DUAL_BLE_RESPONSE_PRESENT)
        return CERBERUS_DUAL_BLE_SNAPSHOT_RESPONSE_ROW_BYTES;
    if (response[1] != CERBERUS_DUAL_BLE_RESPONSE_NONE) return 0U;
    uint8_t sequence_nonzero = 0U;
    for (size_t i = 2U; i < 152U; ++i) {
        if (i >= 8U && i < 16U) sequence_nonzero |= response[i];
        else if (response[i]) return 0U;
    }
    return sequence_nonzero ? CERBERUS_DUAL_BLE_COMPACT_RESPONSE_NONE_BYTES : 0U;
}

_Static_assert(sizeof(cerberus_dual_ble_family_wire_t) == 48U, "family evidence layout");
_Static_assert(sizeof(cerberus_dual_ble_snapshot_family_row_t) == 158U, "family snapshot row layout");

_Static_assert(sizeof(cerberus_dual_ble_info_t) == 22U, "dual BLE info frame");
_Static_assert(sizeof(cerberus_dual_ble_start_t) == 22U, "dual BLE start frame");
_Static_assert(sizeof(cerberus_dual_ble_status_t) == 22U, "dual BLE status frame");
_Static_assert(sizeof(cerberus_dual_ble_row_time_t) == 22U, "dual BLE row frame");
_Static_assert(sizeof(cerberus_dual_ble_track_t) == 22U, "dual BLE track frame");
_Static_assert(sizeof(cerberus_dual_ble_analyzer_sample_t) == 22U, "dual BLE analyzer frame");
_Static_assert(sizeof(cerberus_dual_ble_snapshot_begin_t) == 20U, "snapshot begin frame");
_Static_assert(sizeof(cerberus_dual_ble_snapshot_binding_t) == 20U, "snapshot binding frame");
_Static_assert(sizeof(cerberus_dual_ble_snapshot_read_t) == 22U, "snapshot read frame");
_Static_assert(sizeof(cerberus_dual_ble_snapshot_status_t) == 22U, "snapshot status frame");
_Static_assert(sizeof(cerberus_dual_ble_snapshot_inventory_t) == 22U, "snapshot inventory frame");
_Static_assert(sizeof(cerberus_dual_ble_snapshot_chunk_t) == 22U, "snapshot chunk frame");
_Static_assert(sizeof(cerberus_dual_ble_snapshot_row_t) == 102U, "snapshot row layout");
_Static_assert(sizeof(cerberus_dual_ble_receive_metadata_t) == 8U, "receive metadata layout");
_Static_assert(sizeof(cerberus_dual_ble_snapshot_metadata_row_t) == 110U, "metadata snapshot row layout");

static inline bool cerberus_dual_ble_start_valid(const cerberus_dual_ble_start_t *r)
{
    if (!r || !r->boot_id || !r->job_id ||
        (r->version != CERBERUS_DUAL_BLE_VERSION && r->version != CERBERUS_DUAL_BLE_LEGACY_VERSION) ||
        r->mode > CERBERUS_DUAL_BLE_REACQUIRE ||
        (r->mode == CERBERUS_DUAL_BLE_REACQUIRE ?
            r->address_type != CERBERUS_DUAL_BLE_ANY_OBSERVED_TYPE : r->address_type > 3U) ||
        (cerberus_dual_ble_family_mode(r->mode) ? (r->reserved < 1U || r->reserved > 6U) : r->reserved != 0U) ||
        (r->mode >= CERBERUS_DUAL_BLE_ANALYZER && r->version != CERBERUS_DUAL_BLE_VERSION)) return false;
    if (r->mode == CERBERUS_DUAL_BLE_REACQUIRE) {
        uint8_t any = 0U, all = 0xffU;
        for (size_t i = 0; i < 6U; ++i) { any |= r->address[i]; all &= r->address[i]; }
        if (!any || all == 0xffU) return false;
    } else if (r->mode != CERBERUS_DUAL_BLE_TRACK) {
        if (r->address_type) return false;
        for (size_t i = 0; i < 6U; ++i) if (r->address[i]) return false;
    }
    return true;
}

static inline uint32_t cerberus_dual_ble_age_upper(uint32_t age_at_reply,
    uint64_t request_started_ms, uint64_t now_ms)
{
    if (now_ms < request_started_ms || now_ms - request_started_ms > UINT32_MAX ||
        now_ms - request_started_ms > UINT32_MAX - age_at_reply) return UINT32_MAX;
    return age_at_reply + (uint32_t)(now_ms - request_started_ms);
}
