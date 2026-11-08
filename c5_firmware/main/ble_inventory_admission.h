#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Bounded private admission policy, separate from packet evidence. Only the
 * snapshot-based passive discovery session enables replacement. No RF, heap,
 * clocks or SDK dependencies. A slot's rank changes only on a qualifying
 * complete original, never on a malformed report or a backward clock. */
enum { CERBERUS_BLE_INVENTORY_SLOTS = 32U, CERBERUS_BLE_INVENTORY_AD_MAX = 255U };
typedef struct {
    uint32_t rank[CERBERUS_BLE_INVENTORY_SLOTS], sequence, replaced, dropped;
    int64_t clock_highwater_us;
    bool enabled, clock_valid;
} cerberus_ble_inventory_admission_t;

static inline void cerberus_ble_inventory_admission_begin(
    cerberus_ble_inventory_admission_t *s, bool enabled, int64_t started_us)
{
    memset(s, 0, sizeof(*s));
    s->enabled = enabled;
    s->clock_valid = started_us >= 0;
    s->clock_highwater_us = started_us >= 0 ? started_us : 0;
}

/* Validate the whole independent AD container. Unknown complete types are
 * allowed; known UUID/service/manufacturer/flags widths are checked. A zero
 * terminator permits zero padding only. Cached peer fields never complete a
 * malformed container or corroborate a current name. */
static inline bool cerberus_ble_inventory_ad_valid(const uint8_t *data, size_t length)
{
    if ((!data && length) || length > CERBERUS_BLE_INVENTORY_AD_MAX) return false;
    const uint8_t *name = NULL;
    size_t name_length = 0U;
    for (size_t offset = 0U; offset < length;) {
        size_t n = data[offset++];
        if (!n) {
            while (offset < length) if (data[offset++]) return false;
            break;
        }
        if (n > length - offset) return false;
        uint8_t type = data[offset];
        const uint8_t *value = data + offset + 1U;
        size_t value_length = n - 1U;
        if (type >= 0x02U && type <= 0x07U) {
            size_t width = type <= 0x03U ? 2U : type <= 0x05U ? 4U : 16U;
            if (value_length % width) return false;
        } else if ((type == 0xFFU && value_length < 2U) ||
            (type == 0x16U && value_length < 2U) ||
            (type == 0x20U && value_length < 4U) ||
            (type == 0x21U && value_length < 16U) ||
            (type == 0x01U && value_length != 1U)) return false;
        else if (type == 0x09U && value_length) {
            if (name && (name_length != value_length || memcmp(name, value, value_length))) return false;
            name = value; name_length = value_length;
        }
        offset += n;
    }
    return true;
}

/* Caller first rejects stale epochs, invalid typed identities, responses and
 * partial extended reports. It passes current retained count/existing index.
 * Nonfull and existing-row evidence retains the prior admission behavior.
 * Full new-address replacements require independent validity and a monotonic
 * clock. Equal clock values use callback order; exhausted sequence fails
 * closed. Stable slot order breaks equal ranks (including rank0/no qualifier).
 * Return -1 means unretained, otherwise original/new/replaced slot index. */
static inline int cerberus_ble_inventory_admit(cerberus_ble_inventory_admission_t *s,
    uint8_t count, int existing, bool qualifying, int64_t callback_us)
{
    if (count > CERBERUS_BLE_INVENTORY_SLOTS || existing < -1 || existing >= count) return -1;
    bool clock_ok = callback_us >= 0 && (!s->clock_valid || callback_us >= s->clock_highwater_us);
    if (clock_ok) { s->clock_highwater_us = callback_us; s->clock_valid = true; }
    bool ranked = qualifying && clock_ok && s->sequence != UINT32_MAX;
    uint32_t sequence = ranked ? ++s->sequence : 0U;
    if (existing >= 0) {
        if (ranked) s->rank[existing] = sequence;
        return existing;
    }
    if (count < CERBERUS_BLE_INVENTORY_SLOTS) {
        s->rank[count] = sequence;
        return count;
    }
    if (s->enabled && ranked) {
        uint8_t oldest = 0U;
        for (uint8_t i = 1U; i < count; ++i)
            if (s->rank[i] < s->rank[oldest]) oldest = i;
        s->rank[oldest] = sequence;
        if (s->replaced != UINT32_MAX) ++s->replaced;
        return oldest;
    }
    if (s->dropped != UINT32_MAX) ++s->dropped;
    return -1;
}
