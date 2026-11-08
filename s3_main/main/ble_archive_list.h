#pragma once

/* Historical presentation only: indexes into one immutable, already decoded
 * archive. No row bank, allocation, clocks, IO, radio work, identity merge or
 * active-target eligibility. The codec owns full document validation. */
#include "ble_archive.h"
#include "ble_rssi.h"

#define CERBERUS_BLE_ARCHIVE_LIST_CAPACITY 64U

typedef enum {
    CERBERUS_BLE_ARCHIVE_LIST_ALL = 0,
    CERBERUS_BLE_ARCHIVE_LIST_S3,
    CERBERUS_BLE_ARCHIVE_LIST_C5,
    CERBERUS_BLE_ARCHIVE_LIST_LEGACY,
    CERBERUS_BLE_ARCHIVE_LIST_SOURCE_COUNT,
} cerberus_ble_archive_list_source_t;

typedef enum {
    CERBERUS_BLE_ARCHIVE_LIST_FILE = 0,
    CERBERUS_BLE_ARCHIVE_LIST_NAME,
    CERBERUS_BLE_ARCHIVE_LIST_SIGNAL,
    CERBERUS_BLE_ARCHIVE_LIST_SORT_COUNT,
} cerberus_ble_archive_list_sort_t;

_Static_assert(CERBERUS_BLE_ARCHIVE_MAX_ROWS <= CERBERUS_BLE_ARCHIVE_LIST_CAPACITY,
    "archive rows exceed presentation index capacity");
_Static_assert(sizeof(uint8_t) * CERBERUS_BLE_ARCHIVE_LIST_CAPACITY == 64U,
    "archive list workspace must remain 64 bytes");

/* A bounded common display label, never an address-derived privacy decision.
 * Returns full label length; capacity zero is a count-only call. Invalid args
 * leave output untouched. Output must not overlap the source row. */
static inline size_t cerberus_ble_archive_list_label(const cerberus_ble_archive_row_t *row,
    char *out, size_t capacity)
{
    if (!row || (!out && capacity)) return 0U;
    const char *name = row->name[0] ? row->name : "[NO RETAINED NAME]";
    size_t length = 0U;
    while (length < CERBERUS_BLE_ARCHIVE_NAME_MAX && name[length]) {
        unsigned char value = (unsigned char)name[length];
        if (length + 1U < capacity)
            out[length] = value >= 0x20U && value <= 0x7eU ? (char)value : '.';
        ++length;
    }
    if (capacity) out[length < capacity ? length : capacity - 1U] = '\0';
    return length;
}

static inline bool cerberus_ble_archive_list_rssi_valid(const cerberus_ble_archive_row_t *row)
{
    return row && row->rssi_valid && cerberus_ble_rssi_valid(row->rssi);
}

static inline bool cerberus_ble_archive_list_source_matches(uint8_t receiver,
    cerberus_ble_archive_list_source_t source)
{
    return source == CERBERUS_BLE_ARCHIVE_LIST_ALL ||
        (source == CERBERUS_BLE_ARCHIVE_LIST_S3 && receiver == CERBERUS_BLE_ARCHIVE_RECEIVER_S3) ||
        (source == CERBERUS_BLE_ARCHIVE_LIST_C5 && receiver == CERBERUS_BLE_ARCHIVE_RECEIVER_C5) ||
        (source == CERBERUS_BLE_ARCHIVE_LIST_LEGACY && receiver == CERBERUS_BLE_ARCHIVE_RECEIVER_LEGACY);
}

static inline bool cerberus_ble_archive_list_before(const cerberus_ble_archive_row_t *left,
    const cerberus_ble_archive_row_t *right, cerberus_ble_archive_list_sort_t sort)
{
    if (sort == CERBERUS_BLE_ARCHIVE_LIST_FILE) return false;
    if (sort == CERBERUS_BLE_ARCHIVE_LIST_SIGNAL) {
        bool lv = cerberus_ble_archive_list_rssi_valid(left);
        bool rv = cerberus_ble_archive_list_rssi_valid(right);
        if (lv != rv) return lv;
        if (lv && left->rssi != right->rssi) return left->rssi > right->rssi;
    }
    char a[CERBERUS_BLE_ARCHIVE_NAME_MAX + 1U], b[CERBERUS_BLE_ARCHIVE_NAME_MAX + 1U];
    size_t al = cerberus_ble_archive_list_label(left, a, sizeof(a));
    size_t bl = cerberus_ble_archive_list_label(right, b, sizeof(b));
    size_t length = al < bl ? al : bl;
    for (size_t i = 0U; i < length; ++i) {
        unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x >= 'A' && x <= 'Z') x = (unsigned char)(x + ('a' - 'A'));
        if (y >= 'A' && y <= 'Z') y = (unsigned char)(y + ('a' - 'A'));
        if (x != y) return x < y;
    }
    return al < bl;
}

/* Caller provides 64 output bytes, nonoverlapping with the document. File
 * order preserves exact stored order after source filtering; equal complete
 * sort keys retain that order. Signal compares stored valid numbers only:
 * S3/LEGACY peak and C5 latest-ADV semantics/times stay unchanged and distinct.
 * No source+typed-address or physical-device deduplication occurs. Invalid
 * bounded arguments return zero without touching output; unused slots remain
 * untouched. Count zero is an empty view, not a newly valid archive file. */
static inline uint8_t cerberus_ble_archive_list_order(const cerberus_ble_archive_t *document,
    cerberus_ble_archive_list_source_t source, cerberus_ble_archive_list_sort_t sort,
    uint8_t indexes[CERBERUS_BLE_ARCHIVE_LIST_CAPACITY])
{
    if (!document || !indexes || document->count > CERBERUS_BLE_ARCHIVE_MAX_ROWS ||
        (unsigned)source >= CERBERUS_BLE_ARCHIVE_LIST_SOURCE_COUNT ||
        (unsigned)sort >= CERBERUS_BLE_ARCHIVE_LIST_SORT_COUNT) return 0U;
    /* Cheap domain preflight before any output writes; an unknown receiver
     * must never be rendered or reinterpreted as a LEGACY observation. */
    for (uint8_t i = 0U; i < document->count; ++i)
        if (document->rows[i].receiver > CERBERUS_BLE_ARCHIVE_RECEIVER_C5) return 0U;
    uint8_t visible = 0U;
    for (uint8_t i = 0U; i < document->count; ++i) {
        if (!cerberus_ble_archive_list_source_matches(document->rows[i].receiver, source)) continue;
        uint8_t at = visible;
        while (at && cerberus_ble_archive_list_before(&document->rows[i],
                &document->rows[indexes[at - 1U]], sort)) {
            indexes[at] = indexes[at - 1U];
            --at;
        }
        indexes[at] = i;
        ++visible;
    }
    return visible;
}
