#pragma once

/* Presentation-only references into existing, UI-owned committed banks.
 * No allocation, transport, source mutation, or identity merge. References are
 * ephemeral: the UI must copy the painted row and its full provenance before
 * either source can publish again, and select that copy rather than this slot.
 * This helper never authorizes an action or validates packet evidence. */
#include "s3_ble_discovery_inventory.h"
#include "dual_ble_list.h"

#define CERBERUS_BLE_DISCOVERY_LIST_CAPACITY 64U
#define CERBERUS_BLE_DISCOVERY_LIST_RECENT_MS UINT64_C(30000)

typedef enum {
    CERBERUS_BLE_DISCOVERY_LIST_ALL = 0,
    CERBERUS_BLE_DISCOVERY_LIST_S3,
    CERBERUS_BLE_DISCOVERY_LIST_C5,
    CERBERUS_BLE_DISCOVERY_LIST_SOURCE_COUNT,
} cerberus_ble_discovery_list_source_t;

typedef enum {
    CERBERUS_BLE_DISCOVERY_LIST_SIGNAL = 0,
    CERBERUS_BLE_DISCOVERY_LIST_NAME,
    CERBERUS_BLE_DISCOVERY_LIST_SOURCE_ORDER,
    CERBERUS_BLE_DISCOVERY_LIST_SORT_COUNT,
} cerberus_ble_discovery_list_sort_t;

typedef struct {
    uint8_t source, index;
} cerberus_ble_discovery_list_ref_t;

_Static_assert(CERBERUS_BLE_SCAN_MAX_RESULTS + CERBERUS_DUAL_BLE_CAPACITY <=
    CERBERUS_BLE_DISCOVERY_LIST_CAPACITY, "receiver banks exceed reference capacity");
_Static_assert(sizeof(cerberus_ble_discovery_list_ref_t) == 2U,
    "discovery reference must remain two bytes");

typedef enum {
    CERBERUS_BLE_DISCOVERY_LIST_AGE_UNKNOWN = 0,
    CERBERUS_BLE_DISCOVERY_LIST_AGE_EXACT,
    CERBERUS_BLE_DISCOVERY_LIST_AGE_UPPER,
} cerberus_ble_discovery_list_age_kind_t;

typedef struct {
    uint64_t ms;
    cerberus_ble_discovery_list_age_kind_t kind;
} cerberus_ble_discovery_list_age_t;

/* NULL means an absent receiver. A non-NULL S3 inventory must be initialized;
 * its family is the caller's selection, never inferred from C5 observations. */
static inline bool cerberus_ble_discovery_list_inputs_valid(
    const cerberus_s3_ble_discovery_inventory_t *s3,
    const cerberus_dual_ble_row_t *c5, uint8_t c5_count)
{
    return (!s3 || (s3->initialized && s3->count <= CERBERUS_BLE_SCAN_MAX_RESULTS)) &&
        c5_count <= CERBERUS_DUAL_BLE_CAPACITY && (!c5_count || c5);
}

static inline bool cerberus_ble_discovery_list_ref_valid(
    const cerberus_s3_ble_discovery_inventory_t *s3,
    const cerberus_dual_ble_row_t *c5, uint8_t c5_count,
    cerberus_ble_discovery_list_ref_t ref)
{
    if (!cerberus_ble_discovery_list_inputs_valid(s3, c5, c5_count)) return false;
    return (ref.source == CERBERUS_BLE_DISCOVERY_LIST_S3 && s3 && ref.index < s3->count) ||
        (ref.source == CERBERUS_BLE_DISCOVERY_LIST_C5 && ref.index < c5_count);
}

/* Exact S3 original age and conservative C5 original-age upper bound remain
 * different meanings. Unknown, rewound, or saturated C5 clocks return UNKNOWN;
 * a known upper bound above the recent window does not establish staleness. */
static inline cerberus_ble_discovery_list_age_t cerberus_ble_discovery_list_age(
    const cerberus_s3_ble_discovery_inventory_t *s3,
    const cerberus_dual_ble_row_t *c5, uint8_t c5_count,
    cerberus_ble_discovery_list_ref_t ref, uint64_t now_ms)
{
    cerberus_ble_discovery_list_age_t age = {0U, CERBERUS_BLE_DISCOVERY_LIST_AGE_UNKNOWN};
    if (!cerberus_ble_discovery_list_ref_valid(s3, c5, c5_count, ref)) return age;
    if (ref.source == CERBERUS_BLE_DISCOVERY_LIST_S3) {
        if (cerberus_s3_ble_discovery_entry_age_ms(&s3->entries[ref.index], now_ms, &age.ms))
            age.kind = CERBERUS_BLE_DISCOVERY_LIST_AGE_EXACT;
    } else {
        uint32_t upper;
        if (cerberus_dual_ble_list_age(&c5[ref.index], now_ms, &upper)) {
            age.ms = upper;
            age.kind = CERBERUS_BLE_DISCOVERY_LIST_AGE_UPPER;
        }
    }
    return age;
}

/* The value remains S3 sweep peak or C5 latest original-ADV RSSI. Sorting by
 * signal compares those reported numbers; it does not normalize radio gain,
 * equate sampling windows, or assert a current/live measurement. */
static inline bool cerberus_ble_discovery_list_rssi(
    const cerberus_s3_ble_discovery_inventory_t *s3,
    const cerberus_dual_ble_row_t *c5, uint8_t c5_count,
    cerberus_ble_discovery_list_ref_t ref, int8_t *out)
{
    if (!out || !cerberus_ble_discovery_list_ref_valid(s3, c5, c5_count, ref)) return false;
    int8_t value;
    if (ref.source == CERBERUS_BLE_DISCOVERY_LIST_S3) {
        value = s3->entries[ref.index].record.peer.rssi;
        if (!cerberus_ble_rssi_valid(value)) return false;
    } else {
        if (!cerberus_dual_ble_list_rssi_valid(&c5[ref.index])) return false;
        value = c5[ref.index].rssi;
    }
    *out = value;
    return true;
}

/* A common bounded display label for both receivers: sanitized original name
 * or the exact address. Name bytes and typed identity in the source stay intact.
 * Returns full display length; capacity zero is a count-only call. Invalid
 * arguments leave output untouched. The output must not alias source storage. */
static inline size_t cerberus_ble_discovery_list_label(
    const cerberus_s3_ble_discovery_inventory_t *s3,
    const cerberus_dual_ble_row_t *c5, uint8_t c5_count,
    cerberus_ble_discovery_list_ref_t ref, char *out, size_t capacity)
{
    if ((!out && capacity) || !cerberus_ble_discovery_list_ref_valid(s3, c5, c5_count, ref))
        return 0U;
    const char *name;
    char response_name[CERBERUS_BLE_NAME_MAX_LENGTH + 1U];
    const uint8_t *address;
    if (ref.source == CERBERUS_BLE_DISCOVERY_LIST_S3) {
        name = s3->entries[ref.index].record.peer.name;
        address = s3->entries[ref.index].record.peer.address;
    } else {
        name = c5[ref.index].name;
        if (cerberus_dual_ble_list_response_name(&c5[ref.index])) {
            cerberus_dual_ble_list_label(&c5[ref.index], response_name); name = response_name;
        }
        address = c5[ref.index].address;
    }
    size_t length = 0U;
    if (name[0]) {
        while (length < CERBERUS_BLE_NAME_MAX_LENGTH && name[length]) {
            unsigned char value = (unsigned char)name[length];
            if (length + 1U < capacity)
                out[length] = value >= 0x20U && value <= 0x7eU ? (char)value : '?';
            ++length;
        }
    } else {
        static const char hex[] = "0123456789ABCDEF";
        for (size_t i = 0U; i < 6U; ++i) {
            uint8_t value = address[5U - i];
            if (length + 1U < capacity) out[length] = hex[value >> 4U];
            ++length;
            if (length + 1U < capacity) out[length] = hex[value & 15U];
            ++length;
            if (i != 5U) {
                if (length + 1U < capacity) out[length] = ':';
                ++length;
            }
        }
    }
    if (capacity) out[length < capacity ? length : capacity - 1U] = '\0';
    return length;
}

static inline bool cerberus_ble_discovery_list_before(
    const cerberus_s3_ble_discovery_inventory_t *s3,
    const cerberus_dual_ble_row_t *c5, uint8_t c5_count,
    cerberus_ble_discovery_list_ref_t left, cerberus_ble_discovery_list_ref_t right,
    cerberus_ble_discovery_list_sort_t sort)
{
    if (sort == CERBERUS_BLE_DISCOVERY_LIST_SOURCE_ORDER) return false;
    if (sort == CERBERUS_BLE_DISCOVERY_LIST_SIGNAL) {
        int8_t a = 0, b = 0;
        bool av = cerberus_ble_discovery_list_rssi(s3, c5, c5_count, left, &a);
        bool bv = cerberus_ble_discovery_list_rssi(s3, c5, c5_count, right, &b);
        if (av != bv) return av;
        if (av && a != b) return a > b;
    }
    char a[CERBERUS_BLE_NAME_MAX_LENGTH + 1U], b[CERBERUS_BLE_NAME_MAX_LENGTH + 1U];
    size_t al = cerberus_ble_discovery_list_label(s3, c5, c5_count, left, a, sizeof(a));
    size_t bl = cerberus_ble_discovery_list_label(s3, c5, c5_count, right, b, sizeof(b));
    size_t length = al < bl ? al : bl;
    for (size_t i = 0U; i < length; ++i) {
        unsigned char x = cerberus_s3_ble_list_fold((unsigned char)a[i]);
        unsigned char y = cerberus_s3_ble_list_fold((unsigned char)b[i]);
        if (x != y) return x < y;
    }
    return al < bl;
}

/* Fill at most 64 two-byte references, never another payload bank. SOURCE ORDER
 * is S3 inventory index order followed by C5 published index order, not a shared
 * chronology. Signal ties use name; equal full sort keys retain source/index
 * order. Source plus typed address remains distinct: no rows are deduplicated.
 * Each source mask is computed by its owner, preserving family-specific S3
 * history/response time and C5 original upper-bound semantics. This ordering
 * layer never derives classification or treats a receiver mask as identity.
 * Caller provides 64 output slots that do not overlap either source. Invalid
 * arguments return zero without touching output; unused output stays untouched. */
static inline uint8_t cerberus_ble_discovery_list_order_masked(
    const cerberus_s3_ble_discovery_inventory_t *s3,
    const cerberus_dual_ble_row_t *c5, uint8_t c5_count,
    cerberus_ble_discovery_list_source_t source, cerberus_ble_discovery_list_sort_t sort,
    uint32_t s3_visible, uint32_t c5_visible,
    cerberus_ble_discovery_list_ref_t refs[CERBERUS_BLE_DISCOVERY_LIST_CAPACITY])
{
    if (!refs || !cerberus_ble_discovery_list_inputs_valid(s3, c5, c5_count) ||
        (unsigned)source >= CERBERUS_BLE_DISCOVERY_LIST_SOURCE_COUNT ||
        (unsigned)sort >= CERBERUS_BLE_DISCOVERY_LIST_SORT_COUNT) return 0U;
    uint8_t visible = 0U;
    for (uint8_t receiver = CERBERUS_BLE_DISCOVERY_LIST_S3;
            receiver <= CERBERUS_BLE_DISCOVERY_LIST_C5; ++receiver) {
        if (source != CERBERUS_BLE_DISCOVERY_LIST_ALL && source != receiver) continue;
        uint8_t count = receiver == CERBERUS_BLE_DISCOVERY_LIST_S3 ? (s3 ? s3->count : 0U) : c5_count;
        for (uint8_t i = 0U; i < count; ++i) {
            cerberus_ble_discovery_list_ref_t ref = {receiver, i};
            uint32_t visible_mask = receiver == CERBERUS_BLE_DISCOVERY_LIST_S3 ? s3_visible : c5_visible;
            if (!(visible_mask & (UINT32_C(1) << i))) continue;
            uint8_t at = visible;
            while (at && cerberus_ble_discovery_list_before(s3, c5, c5_count, ref, refs[at - 1U], sort)) {
                refs[at] = refs[at - 1U];
                --at;
            }
            refs[at] = ref;
            ++visible;
        }
    }
    return visible;
}

/* Compatibility entrypoint for ordinary original-age filtering. Family views
 * pass their own source-specific masks to the shared ordering implementation. */
static inline uint8_t cerberus_ble_discovery_list_order(
    const cerberus_s3_ble_discovery_inventory_t *s3,
    const cerberus_dual_ble_row_t *c5, uint8_t c5_count,
    cerberus_ble_discovery_list_source_t source, cerberus_ble_discovery_list_sort_t sort,
    bool recent_only, uint64_t now_ms,
    cerberus_ble_discovery_list_ref_t refs[CERBERUS_BLE_DISCOVERY_LIST_CAPACITY])
{
    if (!cerberus_ble_discovery_list_inputs_valid(s3, c5, c5_count)) return 0U;
    uint32_t masks[2] = {UINT32_MAX, UINT32_MAX};
    if (recent_only) {
        masks[0] = masks[1] = 0U;
        for (uint8_t source_index = 0U; source_index < 2U; ++source_index) {
            uint8_t count = source_index ? c5_count : s3 ? s3->count : 0U;
            for (uint8_t index = 0U; index < count; ++index) {
                cerberus_ble_discovery_list_ref_t ref = {(uint8_t)(source_index + 1U), index};
                cerberus_ble_discovery_list_age_t age = cerberus_ble_discovery_list_age(s3, c5, c5_count, ref, now_ms);
                if (age.kind != CERBERUS_BLE_DISCOVERY_LIST_AGE_UNKNOWN &&
                    age.ms <= CERBERUS_BLE_DISCOVERY_LIST_RECENT_MS)
                    masks[source_index] |= UINT32_C(1) << index;
            }
        }
    }
    return cerberus_ble_discovery_list_order_masked(s3, c5, c5_count,
        source, sort, masks[0], masks[1], refs);
}
