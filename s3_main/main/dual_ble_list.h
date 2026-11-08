#pragma once

/* Presentation-only indexes into one C5 bank. No allocation, radio calls,
 * cross-receiver identity merge, or mutation of source evidence. */
#include "dual_ble_scan.h"
#include "ble_rssi.h"
#include "dual_ble_family.h"
#include <string.h>

#define CERBERUS_DUAL_BLE_RECENT_MS UINT32_C(30000)
typedef enum {
    CERBERUS_DUAL_BLE_SORT_SIGNAL = 0,
    CERBERUS_DUAL_BLE_SORT_NAME,
    CERBERUS_DUAL_BLE_SORT_SCAN,
    CERBERUS_DUAL_BLE_SORT_COUNT
} cerberus_dual_ble_sort_t;

/* Original display name has priority. A fallback is sourced only from the
 * separately validated matched response; callers label it RSP NAME. */
static inline bool cerberus_dual_ble_list_response_name(const cerberus_dual_ble_row_t *row)
{
    return row && !row->name[0] && row->response.state == CERBERUS_DUAL_BLE_RESPONSE_PRESENT &&
        row->response.name_length && cerberus_dual_ble_response_row_valid(row);
}

static inline void cerberus_dual_ble_list_label(const cerberus_dual_ble_row_t *row,
    char out[CERBERUS_BLE_NAME_MAX_LENGTH + 1U])
{
    size_t n = 0U;
    if (row && row->name[0]) {
        while (n < CERBERUS_BLE_NAME_MAX_LENGTH && row->name[n]) {
            unsigned char value = (unsigned char)row->name[n];
            out[n++] = value >= 0x20U && value <= 0x7eU ? (char)value : '?';
        }
    } else if (cerberus_dual_ble_list_response_name(row)) {
        while (n < row->response.name_length && n < CERBERUS_BLE_NAME_MAX_LENGTH) {
            uint8_t value = row->response.name[n];
            out[n++] = value >= 0x20U && value <= 0x7eU ? (char)value : '?';
        }
    } else {
        memcpy(out, "<UNNAMED>", 9U); n = 9U;
    }
    out[n] = '\0';
}

static inline bool cerberus_dual_ble_list_rssi_valid(const cerberus_dual_ble_row_t *row)
{
    return row && (row->timing_flags & (CERBERUS_DUAL_BLE_SEEN | CERBERUS_DUAL_BLE_RSSI_VALID)) ==
        (CERBERUS_DUAL_BLE_SEEN | CERBERUS_DUAL_BLE_RSSI_VALID) &&
        cerberus_ble_rssi_valid(row->rssi);
}

static inline bool cerberus_dual_ble_list_age(const cerberus_dual_ble_row_t *row,
    uint64_t now_ms, uint32_t *age_ms)
{
    if (!row || !age_ms || !(row->timing_flags & CERBERUS_DUAL_BLE_SEEN)) return false;
    uint32_t age = cerberus_dual_ble_age_upper(row->callback_age_ms, row->request_started_ms, now_ms);
    if (age == UINT32_MAX) return false; /* Unknown, rewound or saturated clock. */
    *age_ms = age;
    return true;
}

static inline int cerberus_dual_ble_list_name_compare(const cerberus_dual_ble_row_t *a,
    const cerberus_dual_ble_row_t *b)
{
    char left[CERBERUS_BLE_NAME_MAX_LENGTH + 1U], right[CERBERUS_BLE_NAME_MAX_LENGTH + 1U];
    cerberus_dual_ble_list_label(a, left); cerberus_dual_ble_list_label(b, right);
    for (size_t i = 0U; i <= CERBERUS_BLE_NAME_MAX_LENGTH; ++i) {
        unsigned char x = (unsigned char)left[i], y = (unsigned char)right[i];
        if (x >= 'A' && x <= 'Z') x = (unsigned char)(x + ('a' - 'A'));
        if (y >= 'A' && y <= 'Z') y = (unsigned char)(y + ('a' - 'A'));
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
    return 0;
}

static inline bool cerberus_dual_ble_list_before(const cerberus_dual_ble_row_t *a,
    const cerberus_dual_ble_row_t *b, cerberus_dual_ble_sort_t sort)
{
    if (sort == CERBERUS_DUAL_BLE_SORT_SCAN) return false;
    if (sort == CERBERUS_DUAL_BLE_SORT_SIGNAL) {
        bool av = cerberus_dual_ble_list_rssi_valid(a), bv = cerberus_dual_ble_list_rssi_valid(b);
        if (av != bv) return av;
        if (av && a->rssi != b->rssi) return a->rssi > b->rssi;
    }
    return cerberus_dual_ble_list_name_compare(a, b) < 0;
}

/* Stable insertion into at most 32 indexes. Rebuilding at each UI paint also
 * expires recent rows without relying on a new sweep. A conservative upper
 * bound <=30s proves recency; larger/unknown bounds do not prove it. */
static inline uint8_t cerberus_dual_ble_list_order(const cerberus_dual_ble_row_t *rows,
    uint8_t count, cerberus_dual_ble_sort_t sort, bool recent_only, uint64_t now_ms,
    uint8_t indexes[CERBERUS_DUAL_BLE_CAPACITY])
{
    if (!indexes || (count && !rows) || count > CERBERUS_DUAL_BLE_CAPACITY ||
        (unsigned)sort >= CERBERUS_DUAL_BLE_SORT_COUNT) return 0U;
    uint8_t visible = 0U;
    for (uint8_t i = 0U; i < count; ++i) {
        uint32_t age;
        if (recent_only && (!cerberus_dual_ble_list_age(&rows[i], now_ms, &age) ||
            age > CERBERUS_DUAL_BLE_RECENT_MS)) continue;
        uint8_t at = visible;
        while (at && cerberus_dual_ble_list_before(&rows[i], &rows[indexes[at - 1U]], sort)) {
            indexes[at] = indexes[at - 1U]; --at;
        }
        indexes[at] = i; ++visible;
    }
    return visible;
}
