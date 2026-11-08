#include "wardrive_rssi_filter.h"

#include <string.h>

_Static_assert(sizeof(cerberus_wardrive_rssi_filter_t) <= 5U * 1024U,
    "Capture filtering must remain a small optional PSRAM allocation");
_Static_assert((CERBERUS_WARDRIVE_RSSI_SETS & (CERBERUS_WARDRIVE_RSSI_SETS - 1U)) == 0U,
    "RSSI set count must be a power of two");

static bool valid(const cerberus_ward_log_identity_t *identity)
{
    return identity && (identity->type == 1U || identity->type == 2U) &&
        identity->rssi != INT8_MIN && identity->rssi <= 0;
}

static unsigned set_index(const cerberus_ward_log_identity_t *identity)
{
    uint32_t hash = 2166136261U;
    for (unsigned i = 0; i < sizeof(identity->address); ++i)
        hash = (hash ^ identity->address[i]) * 16777619U;
    hash = (hash ^ identity->type) * 16777619U;
    hash ^= hash >> 16U;
    return hash & (CERBERUS_WARDRIVE_RSSI_SETS - 1U);
}

static bool same(const cerberus_wardrive_rssi_entry_t *entry,
    const cerberus_ward_log_identity_t *identity)
{
    return entry->type == identity->type &&
        memcmp(entry->address, identity->address, sizeof(entry->address)) == 0;
}

bool cerberus_wardrive_rssi_needs_append(const cerberus_wardrive_rssi_filter_t *filter,
    const cerberus_ward_log_identity_t *identity)
{
    if (!filter || !valid(identity)) return true;
    const cerberus_wardrive_rssi_entry_t *set = filter->entries[set_index(identity)];
    for (unsigned i = 0; i < CERBERUS_WARDRIVE_RSSI_WAYS; ++i) {
        if (same(&set[i], identity))
            return identity->rssi < set[i].weakest || identity->rssi > set[i].strongest;
    }
    return true;
}

void cerberus_wardrive_rssi_record_append(cerberus_wardrive_rssi_filter_t *filter,
    const cerberus_ward_log_identity_t *identity)
{
    if (!filter || !valid(identity)) return;
    unsigned slot = set_index(identity), empty = CERBERUS_WARDRIVE_RSSI_WAYS;
    cerberus_wardrive_rssi_entry_t *set = filter->entries[slot];
    for (unsigned i = 0; i < CERBERUS_WARDRIVE_RSSI_WAYS; ++i) {
        if (same(&set[i], identity)) {
            if (identity->rssi < set[i].weakest) set[i].weakest = identity->rssi;
            if (identity->rssi > set[i].strongest) set[i].strongest = identity->rssi;
            return;
        }
        if (!set[i].type && empty == CERBERUS_WARDRIVE_RSSI_WAYS) empty = i;
    }
    if (empty == CERBERUS_WARDRIVE_RSSI_WAYS) {
        empty = filter->next[slot];
        filter->next[slot] = (uint8_t)((empty + 1U) % CERBERUS_WARDRIVE_RSSI_WAYS);
    }
    memcpy(set[empty].address, identity->address, sizeof(set[empty].address));
    set[empty].type = identity->type;
    set[empty].weakest = set[empty].strongest = identity->rssi;
}
