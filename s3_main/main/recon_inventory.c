#include "recon_inventory.h"
#include <limits.h>
#include <string.h>

static void increment(uint32_t *value)
{
    if (*value != UINT32_MAX) ++*value;
}

static bool same_identity(const cerberus_recon_record_t *a,
    const cerberus_recon_record_t *b)
{
    return a->radio == b->radio && a->address_type == b->address_type &&
        memcmp(a->address, b->address, sizeof(a->address)) == 0;
}

static bool source_anchor(const cerberus_recon_record_t *r, uint64_t *anchor,
    uint64_t *offset, bool *available)
{
    *available = (r->source_flags & CERBERUS_RECON_SOURCE_TIME_VALID) != 0;
    *anchor = r->consumed_us;
    *offset = 0U;
    if (!*available) return true;
    if (r->source_kind == CERBERUS_RECON_SOURCE_C5_AGE_MS) {
        if (!(r->source_flags & CERBERUS_RECON_SOURCE_ANCHOR_VALID) ||
            r->source_anchor_us > r->consumed_us) return false;
        /* Independent chip clocks are never subtracted. C5 age includes
         * conservative transport/rounding uncertainty at S3 request start. */
        *anchor = r->source_anchor_us;
        *offset = r->source_time * 1000U;
    } else if (r->source_kind == CERBERUS_RECON_SOURCE_S3_CALLBACK_US) {
        *anchor = r->source_time;
    } else if (r->source_kind == CERBERUS_RECON_SOURCE_S3_ADV_MS32 ||
               r->source_kind == CERBERUS_RECON_SOURCE_S3_RESPONSE_MS32) {
        /* Lift only the unambiguous past half of the low32 millisecond clock
         * into the caller's full uptime domain; never guess across half-wrap. */
        uint64_t consumed_ms = r->consumed_us / 1000U;
        uint32_t age_ms = (uint32_t)consumed_ms - (uint32_t)r->source_time;
        if (age_ms > INT32_MAX || age_ms > consumed_ms) return false;
        *anchor = (consumed_ms - age_ms) * 1000U;
    } else return false;
    return *anchor <= r->consumed_us;
}

static bool payload_valid(const cerberus_recon_record_t *r)
{
    if (!r) return false;
    cerberus_recon_record_t copy = *r;
    /* The inventory also works with recording OFF. These journal-assigned
     * values have no meaning here and are not manufactured in stored rows. */
    copy.session_id = 1U;
    copy.sequence = 1U;
    return cerberus_recon_record_valid(&copy);
}

static size_t first_slot(uint8_t radio)
{
    return (size_t)radio * CERBERUS_RECON_INVENTORY_PER_RADIO;
}

static cerberus_recon_inventory_entry_t *find_entry(
    cerberus_recon_inventory_t *s, const cerberus_recon_record_t *r)
{
    size_t begin = first_slot(r->radio);
    for (size_t i = begin; i < begin + CERBERUS_RECON_INVENTORY_PER_RADIO; ++i)
        if (s->entries[i].occupied && same_identity(&s->entries[i].record, r))
            return &s->entries[i];
    return NULL;
}

void cerberus_recon_inventory_init(cerberus_recon_inventory_t *s, uint64_t now)
{
    if (!s) return;
    memset(s, 0, sizeof(*s));
    s->clock_us = now;
    s->initialized = true;
}

static void advance_history(cerberus_recon_inventory_t *s, uint64_t now)
{
    if (!s->history_count) return;
    uint64_t second = now / UINT64_C(1000000);
    uint64_t previous = s->history[s->history_head].second;
    if (second <= previous) return;
    uint64_t difference = second - previous;
    if (difference >= CERBERUS_RECON_INVENTORY_BUCKETS) {
        memset(s->history, 0, sizeof(s->history));
        for (size_t i = 0; i < CERBERUS_RECON_INVENTORY_BUCKETS; ++i)
            s->history[i].second = second - (CERBERUS_RECON_INVENTORY_BUCKETS - 1U) + i;
        s->history_head = CERBERUS_RECON_INVENTORY_BUCKETS - 1U;
        s->history_count = CERBERUS_RECON_INVENTORY_BUCKETS;
        return;
    }
    for (uint64_t i = 1; i <= difference; ++i) {
        s->history_head = (uint8_t)((s->history_head + 1U) % CERBERUS_RECON_INVENTORY_BUCKETS);
        s->history[s->history_head] = (cerberus_recon_inventory_bucket_t){
            .second = previous + i};
        if (s->history_count < CERBERUS_RECON_INVENTORY_BUCKETS) ++s->history_count;
    }
}

bool cerberus_recon_inventory_tick(cerberus_recon_inventory_t *s, uint64_t now)
{
    if (!s || !s->initialized) return false;
    if (now < s->clock_us) { increment(&s->counters.invalid); return false; }
    advance_history(s, now);
    s->clock_us = now;
    return true;
}

bool cerberus_recon_inventory_begin(cerberus_recon_inventory_t *s,
    uint8_t radio, uint64_t publication, uint64_t now)
{
    if (!s || !s->initialized) return false;
    if (s->batch_active || radio > CERBERUS_RECON_RADIO_BLE || !publication ||
        publication <= s->publication[radio] || now < s->clock_us) {
        increment(&s->counters.invalid);
        return false;
    }
    s->publication[radio] = publication;
    s->batch_radio = radio;
    s->batch_active = true;
    s->batch_failed = false;
    s->staged_count = 0U;
    s->batch_started_us = s->batch_last_us = now;
    s->clock_us = now;
    return true;
}

bool cerberus_recon_inventory_offer(cerberus_recon_inventory_t *s,
    const cerberus_recon_record_t *r)
{
    if (!s || !s->initialized) return false;
    if (s->batch_failed) return false;
    uint64_t anchor = 0U, offset = 0U;
    bool available = false;
    bool valid = s->batch_active && payload_valid(r) &&
        r->radio == s->batch_radio && r->consumed_us >= s->batch_last_us &&
        r->consumed_us >= s->clock_us &&
        s->staged_count < CERBERUS_RECON_INVENTORY_PER_RADIO &&
        source_anchor(r, &anchor, &offset, &available);
    if (valid) {
        if (s->staged_count && r->generation != s->staged[0].generation) valid = false;
        for (size_t i = 0; i < s->staged_count; ++i)
            if (same_identity(r, &s->staged[i])) valid = false;
        cerberus_recon_inventory_entry_t *old = find_entry(s, r);
        if (old && available && r->source_kind != CERBERUS_RECON_SOURCE_C5_AGE_MS &&
            old->age_basis == CERBERUS_RECON_INVENTORY_AGE_SOURCE &&
            anchor < old->age_anchor_us) valid = false;
    }
    if (!valid) {
        increment(&s->counters.invalid);
        if (s->batch_active) s->batch_failed = true;
        return false;
    }
    s->staged[s->staged_count++] = *r;
    s->batch_last_us = r->consumed_us;
    s->clock_us = r->consumed_us;
    return true;
}

void cerberus_recon_inventory_abort(cerberus_recon_inventory_t *s)
{
    if (!s) return;
    memset(s->staged, 0, sizeof(s->staged));
    s->staged_count = 0U;
    s->batch_active = s->batch_failed = false;
}

static void expire_entry(cerberus_recon_inventory_t *s,
    cerberus_recon_inventory_entry_t *entry)
{
    --s->count[entry->record.radio];
    memset(entry, 0, sizeof(*entry));
    increment(&s->counters.expired);
    increment(&s->history[s->history_head].expired);
}

bool cerberus_recon_inventory_end(cerberus_recon_inventory_t *s, uint64_t now)
{
    if (!s || !s->initialized) return false;
    if (!s->batch_active || s->batch_failed || now < s->clock_us ||
        now < s->batch_last_us || UINT64_MAX - s->revision < s->staged_count) {
        if (!s->batch_failed) increment(&s->counters.invalid);
        cerberus_recon_inventory_abort(s);
        return false;
    }
    advance_history(s, now);
    if (!s->history_count) {
        s->history[0].second = now / UINT64_C(1000000);
        s->history_count = 1U;
    }
    /* First update every existing address, then expire the completed radio.
     * This prevents resumed fresh rows being expired before their refresh.
     * New rows are admitted afterwards so missing expired entries free their
     * slots without turning an actual expiry into capacity replacement. */
    bool handled[CERBERUS_RECON_INVENTORY_PER_RADIO] = {false};
    bool refreshed[CERBERUS_RECON_INVENTORY_CAPACITY] = {false};
    for (size_t i = 0; i < s->staged_count; ++i) {
        cerberus_recon_record_t *r = &s->staged[i];
        cerberus_recon_inventory_entry_t *entry = find_entry(s, r);
        if (!entry) continue;
        uint64_t anchor, offset;
        bool available;
        (void)source_anchor(r, &anchor, &offset, &available);
        if (available || entry->age_basis == CERBERUS_RECON_INVENTORY_AGE_SWEEP) {
            entry->age_anchor_us = anchor;
            entry->age_offset_us = offset;
            entry->age_basis = available ? CERBERUS_RECON_INVENTORY_AGE_SOURCE :
                CERBERUS_RECON_INVENTORY_AGE_SWEEP;
            entry->age_source_kind = available ? r->source_kind : CERBERUS_RECON_SOURCE_NONE;
        }
        entry->record = *r;
        entry->last_consumed_us = r->consumed_us;
        entry->revision = ++s->revision;
        refreshed[(size_t)(entry - s->entries)] = true;
        handled[i] = true;
        increment(&s->counters.accepted);
    }
    size_t begin = first_slot(s->batch_radio);
    for (size_t i = begin; i < begin + CERBERUS_RECON_INVENTORY_PER_RADIO; ++i) {
        cerberus_recon_inventory_entry_t *entry = &s->entries[i];
        uint64_t age;
        if (cerberus_recon_inventory_entry_age(entry, now, &age) &&
            age >= CERBERUS_RECON_INVENTORY_EXPIRE_US)
            expire_entry(s, entry);
    }
    for (size_t i = 0; i < s->staged_count; ++i) {
        if (handled[i]) continue;
        cerberus_recon_record_t *r = &s->staged[i];
        uint64_t anchor, offset;
        bool available;
        (void)source_anchor(r, &anchor, &offset, &available);
        /* A newly delivered but already expired observation is not an arrival. */
        if (offset >= CERBERUS_RECON_INVENTORY_EXPIRE_US ||
            now - anchor >= CERBERUS_RECON_INVENTORY_EXPIRE_US - offset) continue;
        size_t slot = CERBERUS_RECON_INVENTORY_CAPACITY;
        for (size_t j = begin; j < begin + CERBERUS_RECON_INVENTORY_PER_RADIO; ++j) {
            if (!s->entries[j].occupied) { slot = j; break; }
            if (!refreshed[j] && (slot == CERBERUS_RECON_INVENTORY_CAPACITY ||
                s->entries[j].last_consumed_us < s->entries[slot].last_consumed_us)) slot = j;
        }
        /* At most 32 distinct rows enter this 32-slot partition. Therefore
         * each new row always has an empty or not-refreshed victim. */
        bool replaced = s->entries[slot].occupied;
        if (replaced) increment(&s->counters.replaced);
        else ++s->count[s->batch_radio];
        s->entries[slot] = (cerberus_recon_inventory_entry_t){
            .record = *r, .first_consumed_us = r->consumed_us,
            .last_consumed_us = r->consumed_us, .age_anchor_us = anchor, .age_offset_us = offset,
            .revision = ++s->revision, .occupied = true,
            .age_basis = available ? CERBERUS_RECON_INVENTORY_AGE_SOURCE : CERBERUS_RECON_INVENTORY_AGE_SWEEP,
            .age_source_kind = available ? r->source_kind : CERBERUS_RECON_SOURCE_NONE};
        refreshed[slot] = true;
        increment(&s->counters.accepted);
        increment(&s->counters.added);
        increment(&s->history[s->history_head].added);
    }
    s->clock_us = now;
    cerberus_recon_inventory_abort(s);
    return true;
}

bool cerberus_recon_inventory_entry_age(const cerberus_recon_inventory_entry_t *e,
    uint64_t now, uint64_t *age)
{
    if (!e || !e->occupied || !age || now < e->age_anchor_us ||
        now < e->last_consumed_us) return false;
    uint64_t elapsed = now - e->age_anchor_us;
    *age = e->age_offset_us > UINT64_MAX - elapsed ? UINT64_MAX : e->age_offset_us + elapsed;
    return true;
}

size_t cerberus_recon_inventory_indexes(const cerberus_recon_inventory_t *s,
    uint8_t filter, uint8_t *indexes, size_t capacity)
{
    if (!s || !s->initialized || filter > CERBERUS_RECON_INVENTORY_ALL ||
        (!indexes && capacity)) return 0U;
    uint8_t order[CERBERUS_RECON_INVENTORY_CAPACITY];
    size_t count = 0U;
    for (size_t i = 0; i < CERBERUS_RECON_INVENTORY_CAPACITY; ++i) {
        if (!s->entries[i].occupied ||
            (filter != CERBERUS_RECON_INVENTORY_ALL && s->entries[i].record.radio != filter)) continue;
        size_t position = count++;
        while (position && s->entries[order[position - 1U]].last_consumed_us <
                s->entries[i].last_consumed_us) {
            order[position] = order[position - 1U];
            --position;
        }
        order[position] = (uint8_t)i;
    }
    size_t copied = count < capacity ? count : capacity;
    if (copied) memcpy(indexes, order, copied);
    return count;
}

size_t cerberus_recon_inventory_buckets(const cerberus_recon_inventory_t *s,
    cerberus_recon_inventory_bucket_t *out, size_t capacity)
{
    if (!s || !s->initialized || (!out && capacity)) return 0U;
    size_t count = s->history_count < capacity ? s->history_count : capacity;
    size_t first = (s->history_head + CERBERUS_RECON_INVENTORY_BUCKETS + 1U -
        s->history_count) % CERBERUS_RECON_INVENTORY_BUCKETS;
    for (size_t i = 0; i < count; ++i)
        out[i] = s->history[(first + i) % CERBERUS_RECON_INVENTORY_BUCKETS];
    return s->history_count;
}
