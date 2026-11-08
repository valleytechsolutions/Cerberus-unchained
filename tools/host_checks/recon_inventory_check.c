#ifdef NDEBUG
#undef NDEBUG
#endif
#include "recon_inventory.h"
#include "sentinel_watch_detect.h"
#include <assert.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static unsigned checks;
#define CHECK(condition) do { ++checks; assert(condition); } while (0)
#define SECOND UINT64_C(1000000)
#define TTL CERBERUS_RECON_INVENTORY_EXPIRE_US

static cerberus_recon_record_t row(uint8_t radio, unsigned identity, uint64_t now)
{
    cerberus_recon_record_t r = {
        .consumed_us = now, .radio = radio, .generation = 255,
        .address = {2, 3, 4, 5, (uint8_t)(identity >> 8), (uint8_t)identity},
        .rssi_valid = true, .rssi = -55,
    };
    if (radio == CERBERUS_RECON_RADIO_WIFI) {
        r.channel = 6;
        r.channel_valid = true;
    } else r.address_type = 1;
    return r;
}

static cerberus_recon_inventory_entry_t *lookup(cerberus_recon_inventory_t *s,
    const cerberus_recon_record_t *r)
{
    for (size_t i = 0; i < CERBERUS_RECON_INVENTORY_CAPACITY; ++i)
        if (s->entries[i].occupied && s->entries[i].record.radio == r->radio &&
            s->entries[i].record.address_type == r->address_type &&
            !memcmp(s->entries[i].record.address, r->address, sizeof(r->address)))
            return &s->entries[i];
    return NULL;
}

static void invariant(const cerberus_recon_inventory_t *s)
{
    uint8_t actual[2] = {0};
    for (size_t i = 0; i < CERBERUS_RECON_INVENTORY_CAPACITY; ++i) {
        const cerberus_recon_inventory_entry_t *e = &s->entries[i];
        if (!e->occupied) continue;
        CHECK(e->record.radio == i / CERBERUS_RECON_INVENTORY_PER_RADIO);
        ++actual[e->record.radio];
        CHECK(e->revision && e->revision <= s->revision);
        CHECK(e->first_consumed_us <= e->last_consumed_us);
        CHECK(e->age_anchor_us <= e->last_consumed_us);
        CHECK(e->last_consumed_us <= s->clock_us);
        for (size_t j = 0; j < i; ++j) {
            const cerberus_recon_inventory_entry_t *other = &s->entries[j];
            if (!other->occupied || other->record.radio != e->record.radio) continue;
            CHECK(other->record.address_type != e->record.address_type ||
                memcmp(other->record.address, e->record.address, 6));
        }
    }
    CHECK(actual[0] == s->count[0] && actual[1] == s->count[1]);
    CHECK(actual[0] <= 32 && actual[1] <= 32);
    CHECK(s->history_count <= 64);
    cerberus_recon_inventory_bucket_t history[64];
    CHECK(cerberus_recon_inventory_buckets(s, history, 64) == s->history_count);
    for (size_t i = 1; i < s->history_count; ++i)
        CHECK(history[i].second == history[i - 1].second + 1);
}

static void commit(cerberus_recon_inventory_t *s, uint64_t pub,
    const cerberus_recon_record_t *r)
{
    CHECK(cerberus_recon_inventory_begin(s, r->radio, pub, r->consumed_us));
    CHECK(cerberus_recon_inventory_offer(s, r));
    CHECK(cerberus_recon_inventory_end(s, r->consumed_us));
    invariant(s);
}

static void same_rows(const cerberus_recon_inventory_t *a,
    const cerberus_recon_inventory_t *b)
{
    CHECK(!memcmp(a->entries, b->entries, sizeof(a->entries)));
    CHECK(!memcmp(a->history, b->history, sizeof(a->history)));
    CHECK(!memcmp(a->count, b->count, sizeof(a->count)));
    CHECK(a->revision == b->revision);
    CHECK(a->history_count == b->history_count && a->history_head == b->history_head);
    CHECK(a->counters.accepted == b->counters.accepted);
    CHECK(a->counters.added == b->counters.added);
    CHECK(a->counters.expired == b->counters.expired);
    CHECK(a->counters.replaced == b->counters.replaced);
}

static void empty(cerberus_recon_inventory_t *s, uint8_t radio,
    uint64_t pub, uint64_t now)
{
    CHECK(cerberus_recon_inventory_begin(s, radio, pub, now));
    CHECK(cerberus_recon_inventory_end(s, now));
    invariant(s);
}

static void basic_identity_and_copies(void)
{
    cerberus_recon_inventory_t s;
    cerberus_recon_inventory_init(&s, 0);
    CHECK(!s.history_count);
    CHECK(cerberus_recon_inventory_tick(&s, 10 * SECOND));
    CHECK(!s.history_count); /* No invented time samples before first sweep. */
    cerberus_recon_record_t a = row(1, 1, 10 * SECOND);
    const uint8_t raw[] = {0, 0xff, ',', '"', '\n', '\\', 0x7f};
    memcpy(a.name, raw, sizeof(raw)); a.name_length = sizeof(raw);
    a.ibeacon = true;
    a.classified = true;
    a.category = SENTINEL_WATCH_BEACON;
    a.confidence = SENTINEL_WATCH_CONFIDENCE_LOW;
    strcpy(a.rule, "beacon.ibeacon");
    a.mode = 1;
    a.alert_state = CERBERUS_RECON_ALERT_NOT_QUEUED;
    commit(&s, 1, &a);
    CHECK(s.count[1] == 1 && s.counters.added == 1 && s.counters.accepted == 1);
    cerberus_recon_inventory_entry_t *e = lookup(&s, &a);
    CHECK(e && !memcmp(&e->record, &a, sizeof(a)));
    CHECK(!e->record.session_id && !e->record.sequence);
    CHECK(e->age_basis == CERBERUS_RECON_INVENTORY_AGE_SWEEP);
    CHECK(e->age_source_kind == CERBERUS_RECON_SOURCE_NONE);
    cerberus_recon_inventory_entry_t frozen = *e;
    memset(a.name, 0x11, sizeof(a.name));
    CHECK(!memcmp(e->record.name, raw, sizeof(raw)));
    a.consumed_us += SECOND;
    a.generation = 0; a.rssi = -80;
    commit(&s, 2, &a);
    CHECK(e->first_consumed_us == 10 * SECOND && e->last_consumed_us == 11 * SECOND);
    CHECK(e->revision != frozen.revision && frozen.record.rssi == -55);
    CHECK(s.counters.added == 1 && s.counters.accepted == 2);
    cerberus_recon_record_t b = a;
    b.address_type = 0; b.consumed_us++;
    commit(&s, 3, &b);
    CHECK(s.count[1] == 2);
    cerberus_recon_record_t w = row(0, 1, b.consumed_us + 1);
    commit(&s, 1, &w);
    CHECK(s.count[0] == 1 && s.count[1] == 2);
    uint8_t indexes[64];
    memset(indexes, 0xaa, sizeof(indexes));
    CHECK(cerberus_recon_inventory_indexes(&s, CERBERUS_RECON_INVENTORY_ALL, indexes, 2) == 3);
    CHECK(s.entries[indexes[0]].record.radio == 0);
    CHECK(s.entries[indexes[1]].record.address_type == 0);
    CHECK(indexes[2] == 0xaa);
    CHECK(cerberus_recon_inventory_indexes(&s, 1, NULL, 0) == 2);
    CHECK(cerberus_recon_inventory_indexes(&s, 0, indexes, 64) == 1);
    uint64_t age = 99;
    CHECK(cerberus_recon_inventory_entry_age(&frozen, 11 * SECOND, &age));
    CHECK(age == SECOND);
    CHECK(!cerberus_recon_inventory_entry_age(&frozen, 1, &age) && age == SECOND);
}

static void atomic_failure_and_publications(void)
{
    cerberus_recon_inventory_t s, before;
    cerberus_recon_inventory_init(&s, 0);
    cerberus_recon_record_t a = row(1, 1, SECOND);
    commit(&s, UINT64_C(0x100000001), &a);
    before = s;
    CHECK(!cerberus_recon_inventory_begin(&s, 1, 1, SECOND));
    CHECK(!cerberus_recon_inventory_begin(&s, 1, UINT64_C(0x100000001), SECOND));
    CHECK(!cerberus_recon_inventory_begin(&s, 1, 0, SECOND));
    same_rows(&s, &before);
    CHECK(cerberus_recon_inventory_begin(&s, 1, UINT64_C(0x100000002), 2 * SECOND));
    a.consumed_us = 2 * SECOND; a.address[5] = 2;
    CHECK(cerberus_recon_inventory_offer(&s, &a));
    same_rows(&s, &before); /* Staging remains invisible. */
    CHECK(!cerberus_recon_inventory_offer(&s, &a)); /* Same typed identity. */
    a.address[5] = 3;
    CHECK(!cerberus_recon_inventory_offer(&s, &a));
    CHECK(!cerberus_recon_inventory_end(&s, 2 * SECOND));
    same_rows(&s, &before);
    CHECK(!s.batch_active && !s.staged_count);
    CHECK(!cerberus_recon_inventory_begin(&s, 1, UINT64_C(0x100000002), 2 * SECOND));
    CHECK(cerberus_recon_inventory_begin(&s, 1, UINT64_C(0x100000003), 2 * SECOND));
    CHECK(cerberus_recon_inventory_offer(&s, &a));
    cerberus_recon_inventory_abort(&s);
    same_rows(&s, &before);
    CHECK(!cerberus_recon_inventory_begin(&s, 1, UINT64_C(0x100000004), 2 * SECOND - 1));
    CHECK(!cerberus_recon_inventory_begin(&s, 1, UINT64_C(0x100000003), 2 * SECOND));
    CHECK(cerberus_recon_inventory_begin(&s, 1, UINT64_C(0x100000004), 2 * SECOND));
    CHECK(!cerberus_recon_inventory_begin(&s, 0, 1, 2 * SECOND));
    CHECK(cerberus_recon_inventory_offer(&s, &a));
    CHECK(!cerberus_recon_inventory_end(&s, 2 * SECOND - 1));
    same_rows(&s, &before);
    CHECK(cerberus_recon_inventory_begin(&s, 1, UINT64_C(0x100000005), 2 * SECOND));
    a.radio = 0;
    CHECK(!cerberus_recon_inventory_offer(&s, &a));
    CHECK(!cerberus_recon_inventory_end(&s, 2 * SECOND));
    same_rows(&s, &before);
    CHECK(cerberus_recon_inventory_begin(&s, 1, UINT64_C(0x100000006), 2 * SECOND));
    CHECK(!cerberus_recon_inventory_offer(&s, NULL));
    CHECK(!cerberus_recon_inventory_end(&s, 2 * SECOND));
    same_rows(&s, &before);
    CHECK(!cerberus_recon_inventory_tick(&s, SECOND - 1));
    same_rows(&s, &before);
    a = row(1, 4, 2 * SECOND);
    commit(&s, UINT64_C(0x100000007), &a); /* Explicit fresh publication recovers. */
    CHECK(s.count[1] == 2 && s.entries[32].record.address[5] == 1);
    before = s;
    CHECK(cerberus_recon_inventory_begin(&s, 1, UINT64_C(0x100000008), a.consumed_us));
    CHECK(cerberus_recon_inventory_offer(&s, &a));
    ++a.address[5]; ++a.generation;
    CHECK(!cerberus_recon_inventory_offer(&s, &a));
    CHECK(!cerberus_recon_inventory_end(&s, a.consumed_us));
    same_rows(&s, &before);
}

static void expiry_and_radio_isolation(void)
{
    cerberus_recon_inventory_t s;
    cerberus_recon_inventory_init(&s, 0);
    cerberus_recon_record_t b = row(1, 1, SECOND), w = row(0, 1, SECOND);
    commit(&s, 1, &b); commit(&s, 1, &w);
    CHECK(cerberus_recon_inventory_tick(&s, SECOND + TTL + 99 * SECOND));
    CHECK(s.count[0] == 1 && s.count[1] == 1 && !s.counters.expired);
    /* A completed empty Wi-Fi bank cannot expire the BLE partition. */
    empty(&s, 0, 2, SECOND + TTL + 99 * SECOND);
    CHECK(!s.count[0] && s.count[1] == 1 && s.counters.expired == 1);
    b.consumed_us = SECOND + TTL + 100 * SECOND;
    commit(&s, 2, &b); /* Refresh first on resume: no false expire/re-add. */
    CHECK(s.count[1] == 1 && s.counters.added == 2 && s.counters.expired == 1);
    empty(&s, 1, 3, b.consumed_us + TTL - 1);
    CHECK(s.count[1] == 1);
    empty(&s, 1, 4, b.consumed_us + TTL);
    CHECK(!s.count[1] && s.counters.expired == 2);
    b.consumed_us += TTL;
    commit(&s, 5, &b);
    CHECK(s.count[1] == 1 && s.counters.added == 3);
    CHECK(lookup(&s, &b)->first_consumed_us == b.consumed_us);
}

static void sources(void)
{
    cerberus_recon_inventory_t s, before;
    cerberus_recon_inventory_init(&s, 0);
    cerberus_recon_record_t b = row(1, 1, 10 * SECOND + 999);
    b.source_kind = CERBERUS_RECON_SOURCE_S3_ADV_MS32;
    b.source_flags = CERBERUS_RECON_SOURCE_TIME_VALID | CERBERUS_RECON_SOURCE_SEQUENCE_VALID;
    b.source_time = 9000; b.source_sequence = 9;
    commit(&s, 1, &b);
    cerberus_recon_inventory_entry_t *e = lookup(&s, &b);
    CHECK(e->age_anchor_us == 9 * SECOND && e->age_basis == CERBERUS_RECON_INVENTORY_AGE_SOURCE);
    CHECK(e->age_source_kind == CERBERUS_RECON_SOURCE_S3_ADV_MS32);
    uint64_t age;
    CHECK(cerberus_recon_inventory_entry_age(e, b.consumed_us, &age) && age == SECOND + 999);
    CHECK(!cerberus_recon_inventory_entry_age(e, 9 * SECOND + 500000, &age));
    CHECK(age == SECOND + 999); /* Source-before-query is insufficient after rollback. */
    b.consumed_us = 20 * SECOND;
    b.source_flags = 0; b.source_time = 0; b.source_sequence = 0;
    b.source_kind = CERBERUS_RECON_SOURCE_NONE;
    commit(&s, 2, &b);
    CHECK(e->last_consumed_us == 20 * SECOND && e->age_anchor_us == 9 * SECOND);
    CHECK(e->record.source_kind == CERBERUS_RECON_SOURCE_NONE);
    CHECK(e->age_source_kind == CERBERUS_RECON_SOURCE_S3_ADV_MS32);
    empty(&s, 1, 3, 9 * SECOND + TTL);
    CHECK(!s.count[1]); /* Unavailable source never refreshed genuine age. */

    cerberus_recon_inventory_init(&s, 0);
    b = row(1, 2, 10 * SECOND);
    b.source_kind = CERBERUS_RECON_SOURCE_S3_CALLBACK_US;
    b.source_flags = CERBERUS_RECON_SOURCE_TIME_VALID;
    b.source_time = 9 * SECOND + 42;
    commit(&s, 1, &b);
    CHECK(lookup(&s, &b)->age_anchor_us == 9 * SECOND + 42);
    before = s;
    b.consumed_us = 11 * SECOND;
    b.source_time = 9 * SECOND + 41;
    CHECK(cerberus_recon_inventory_begin(&s, 1, 2, b.consumed_us));
    CHECK(!cerberus_recon_inventory_offer(&s, &b));
    CHECK(!cerberus_recon_inventory_end(&s, b.consumed_us));
    same_rows(&s, &before);
    b.source_time = b.consumed_us + 1;
    CHECK(cerberus_recon_inventory_begin(&s, 1, 3, b.consumed_us));
    CHECK(!cerberus_recon_inventory_offer(&s, &b));
    CHECK(!cerberus_recon_inventory_end(&s, b.consumed_us));
    same_rows(&s, &before);

    cerberus_recon_inventory_init(&s, 0);
    cerberus_recon_record_t w = row(0, 1, 10 * SECOND);
    w.source_kind = CERBERUS_RECON_SOURCE_C5_AGE_MS;
    w.source_flags = CERBERUS_RECON_SOURCE_TIME_VALID | CERBERUS_RECON_SOURCE_ANCHOR_VALID;
    w.source_time = 250; w.source_anchor_us = 9 * SECOND + 500;
    commit(&s, 1, &w);
    CHECK(lookup(&s, &w)->age_anchor_us == 9000500);
    CHECK(lookup(&s, &w)->age_offset_us == 250000);
    CHECK(cerberus_recon_inventory_entry_age(lookup(&s, &w), w.consumed_us, &age));
    CHECK(age == 1249500);
    /* The C5 source may predate S3 boot. This is an age upper bound at a
     * reference, not an impossible negative reconstructed S3 timestamp. */
    w.source_time = 9001;
    commit(&s, 2, &w);
    CHECK(cerberus_recon_inventory_entry_age(lookup(&s, &w), w.consumed_us, &age));
    CHECK(age == 10000500);
    /* Conservative bounds can move due to latency/rounding; do not reject a
     * valid later request because its inferred instant would move backward. */
    w.consumed_us += SECOND;
    w.source_anchor_us += SECOND;
    w.source_time = 10002;
    commit(&s, 3, &w);
    CHECK(lookup(&s, &w)->age_offset_us == 10002000);
    before = s;
    w.source_time = 0; w.source_anchor_us = w.consumed_us + 1;
    CHECK(cerberus_recon_inventory_begin(&s, 0, 4, w.consumed_us));
    CHECK(!cerberus_recon_inventory_offer(&s, &w));
    CHECK(!cerberus_recon_inventory_end(&s, w.consumed_us));
    same_rows(&s, &before);

    /* Low32-ms wrap retains actual S3 full uptime, including source zero. */
    const uint64_t wrap_ms = UINT64_C(1) << 32;
    for (unsigned kind = CERBERUS_RECON_SOURCE_S3_ADV_MS32;
         kind <= CERBERUS_RECON_SOURCE_S3_RESPONSE_MS32; ++kind) {
        cerberus_recon_inventory_init(&s, 0);
        b = row(1, kind, (wrap_ms + 20) * 1000 + 321);
        b.source_kind = (uint8_t)kind; b.source_flags = CERBERUS_RECON_SOURCE_TIME_VALID;
        b.source_time = UINT32_MAX - 9U;
        commit(&s, 1, &b);
        CHECK(lookup(&s, &b)->age_anchor_us == (wrap_ms - 10) * 1000);
        b.source_time = 0;
        commit(&s, 2, &b);
        CHECK(lookup(&s, &b)->age_anchor_us == wrap_ms * 1000);
        before = s;
        b.source_time = 21;
        CHECK(cerberus_recon_inventory_begin(&s, 1, 3, b.consumed_us));
        CHECK(!cerberus_recon_inventory_offer(&s, &b));
        CHECK(!cerberus_recon_inventory_end(&s, b.consumed_us));
        same_rows(&s, &before);
    }
    cerberus_recon_inventory_init(&s, 0);
    b = row(1, 1, 10 * SECOND);
    b.source_kind = CERBERUS_RECON_SOURCE_S3_ADV_MS32;
    b.source_flags = CERBERUS_RECON_SOURCE_TIME_VALID;
    b.source_time = UINT32_MAX - 1;
    CHECK(cerberus_recon_inventory_begin(&s, 1, 1, b.consumed_us));
    CHECK(!cerberus_recon_inventory_offer(&s, &b)); /* Would precede boot. */
    CHECK(!cerberus_recon_inventory_end(&s, b.consumed_us));
    CHECK(!s.history_count && !s.count[1]);
    /* Old, otherwise valid source is retained nowhere and creates no arrival. */
    b = row(1, 1, TTL);
    b.source_kind = CERBERUS_RECON_SOURCE_S3_ADV_MS32;
    b.source_flags = CERBERUS_RECON_SOURCE_TIME_VALID;
    b.source_time = 0;
    commit(&s, 2, &b);
    CHECK(!s.count[1] && !s.counters.added && !s.counters.expired);
}

static void capacity_and_stable_eviction(void)
{
    cerberus_recon_inventory_t s;
    cerberus_recon_inventory_init(&s, 0);
    for (uint8_t radio = 0; radio < 2; ++radio) {
        CHECK(cerberus_recon_inventory_begin(&s, radio, 1, SECOND));
        for (unsigned i = 0; i < 32; ++i) {
            cerberus_recon_record_t r = row(radio, i, SECOND);
            CHECK(cerberus_recon_inventory_offer(&s, &r));
        }
        CHECK(cerberus_recon_inventory_end(&s, SECOND));
    }
    invariant(&s);
    CHECK(s.count[0] == 32 && s.count[1] == 32 && s.counters.added == 64);
    /* Equal timestamps must not repeatedly evict the same freshly added row.
     * Current-batch rows are protected, with deterministic lowest-slot ties. */
    CHECK(cerberus_recon_inventory_begin(&s, 1, 2, SECOND));
    for (unsigned i = 0; i < 32; ++i) {
        cerberus_recon_record_t r = row(1, 100 + i, SECOND);
        CHECK(cerberus_recon_inventory_offer(&s, &r));
    }
    CHECK(cerberus_recon_inventory_end(&s, SECOND));
    CHECK(s.counters.replaced == 32 && !s.counters.expired && s.counters.added == 96);
    for (unsigned i = 0; i < 32; ++i) {
        cerberus_recon_record_t r = row(1, 100 + i, SECOND);
        CHECK(lookup(&s, &r));
        r = row(1, i, SECOND); CHECK(!lookup(&s, &r));
        r = row(0, i, SECOND); CHECK(lookup(&s, &r));
    }
    invariant(&s);
    cerberus_recon_inventory_t before = s;
    CHECK(cerberus_recon_inventory_begin(&s, 1, 3, 2 * SECOND));
    for (unsigned i = 0; i < 32; ++i) {
        cerberus_recon_record_t r = row(1, 200 + i, 2 * SECOND);
        CHECK(cerberus_recon_inventory_offer(&s, &r));
    }
    cerberus_recon_record_t extra = row(1, 232, 2 * SECOND);
    CHECK(!cerberus_recon_inventory_offer(&s, &extra));
    CHECK(!cerberus_recon_inventory_end(&s, 2 * SECOND));
    same_rows(&s, &before);
    /* Refreshed incumbents survive even when they tie with older rows. */
    CHECK(cerberus_recon_inventory_begin(&s, 1, 4, 2 * SECOND));
    cerberus_recon_record_t keep = row(1, 100, 2 * SECOND);
    CHECK(cerberus_recon_inventory_offer(&s, &keep));
    for (unsigned i = 0; i < 31; ++i) {
        cerberus_recon_record_t r = row(1, 300 + i, 2 * SECOND);
        CHECK(cerberus_recon_inventory_offer(&s, &r));
    }
    CHECK(cerberus_recon_inventory_end(&s, 2 * SECOND));
    CHECK(lookup(&s, &keep));
    for (unsigned i = 0; i < 31; ++i) {
        cerberus_recon_record_t r = row(1, 300 + i, SECOND);
        CHECK(lookup(&s, &r));
    }
    invariant(&s);
    /* Expired slots are reclaimed before new admission, not replacements. */
    uint32_t replaced = s.counters.replaced;
    extra = row(1, 600, 2 * SECOND + TTL);
    commit(&s, 5, &extra);
    CHECK(s.count[1] == 1 && s.count[0] == 32);
    CHECK(s.counters.expired == 32 && s.counters.replaced == replaced);
}

static void graph_and_saturation(void)
{
    cerberus_recon_inventory_t s;
    cerberus_recon_inventory_init(&s, 500 * SECOND);
    CHECK(cerberus_recon_inventory_tick(&s, 550 * SECOND));
    CHECK(!s.history_count);
    empty(&s, 1, 1, 600 * SECOND + 10);
    cerberus_recon_inventory_bucket_t buckets[64];
    CHECK(cerberus_recon_inventory_buckets(&s, buckets, 64) == 1);
    CHECK(buckets[0].second == 600 && !buckets[0].added && !buckets[0].expired);
    cerberus_recon_record_t r = row(1, 1, 600 * SECOND + 999);
    commit(&s, 2, &r);
    r.address[5] = 2; commit(&s, 3, &r);
    CHECK(cerberus_recon_inventory_tick(&s, 602 * SECOND));
    CHECK(cerberus_recon_inventory_buckets(&s, buckets, 64) == 3);
    CHECK(buckets[0].added == 2 && buckets[1].added == 0 && buckets[2].second == 602);
    CHECK(cerberus_recon_inventory_tick(&s, 664 * SECOND));
    CHECK(cerberus_recon_inventory_buckets(&s, buckets, 64) == 64);
    CHECK(buckets[0].second == 601 && buckets[63].second == 664);
    CHECK(cerberus_recon_inventory_tick(&s, UINT64_C(9000000000000)));
    CHECK(cerberus_recon_inventory_buckets(&s, buckets, 64) == 64);
    CHECK(buckets[0].second == UINT64_C(9000000) - 63);
    CHECK(buckets[63].second == UINT64_C(9000000));
    CHECK(!buckets[63].added && s.count[1] == 2 && !s.counters.expired);
    empty(&s, 1, 4, UINT64_C(9000000000000));
    CHECK(!s.count[1] && s.counters.expired == 2);
    CHECK(cerberus_recon_inventory_buckets(&s, buckets, 64) == 64);
    CHECK(buckets[63].expired == 2);
    s.counters.accepted = s.counters.added = s.counters.expired = UINT32_MAX;
    s.counters.replaced = s.counters.invalid = UINT32_MAX;
    s.history[s.history_head].added = s.history[s.history_head].expired = UINT32_MAX;
    r.consumed_us = s.clock_us; r.address[5] = 3;
    commit(&s, 5, &r);
    CHECK(s.counters.added == UINT32_MAX && s.counters.accepted == UINT32_MAX);
    CHECK(s.history[s.history_head].added == UINT32_MAX);
    CHECK(!cerberus_recon_inventory_begin(&s, 1, 5, s.clock_us));
    CHECK(s.counters.invalid == UINT32_MAX);
    empty(&s, 1, 6, s.clock_us + TTL);
    CHECK(s.counters.expired == UINT32_MAX);
    invariant(&s);
}

static void invalid_inputs_and_boundaries(void)
{
    cerberus_recon_inventory_t s = {0};
    uint8_t indexes[2] = {91, 92};
    cerberus_recon_inventory_bucket_t bucket = {.second = 123};
    uint64_t age = 45;
    CHECK(!cerberus_recon_inventory_begin(NULL, 0, 1, 0));
    CHECK(!cerberus_recon_inventory_tick(NULL, 0));
    CHECK(!cerberus_recon_inventory_offer(NULL, NULL));
    CHECK(!cerberus_recon_inventory_end(NULL, 0));
    cerberus_recon_inventory_abort(NULL); cerberus_recon_inventory_init(NULL, 0);
    CHECK(!cerberus_recon_inventory_begin(&s, 0, 1, 0));
    CHECK(!cerberus_recon_inventory_tick(&s, 0));
    CHECK(!cerberus_recon_inventory_indexes(NULL, 0, indexes, 2));
    CHECK(!cerberus_recon_inventory_buckets(NULL, &bucket, 1));
    CHECK(bucket.second == 123 && indexes[0] == 91);
    CHECK(!cerberus_recon_inventory_entry_age(NULL, 0, &age) && age == 45);
    cerberus_recon_inventory_init(&s, 0);
    CHECK(!cerberus_recon_inventory_indexes(&s, 3, indexes, 2));
    CHECK(!cerberus_recon_inventory_indexes(&s, 0, NULL, 2));
    CHECK(!cerberus_recon_inventory_buckets(&s, NULL, 2));
    CHECK(!cerberus_recon_inventory_entry_age(&s.entries[0], 0, &age));
    CHECK(!cerberus_recon_inventory_begin(&s, 2, 1, 0));
    CHECK(!cerberus_recon_inventory_offer(&s, NULL));
    CHECK(!cerberus_recon_inventory_end(&s, 0));

    for (unsigned length = 0; length <= 33; ++length) {
        cerberus_recon_inventory_init(&s, 0);
        cerberus_recon_record_t r = row(1, 1, 0);
        memset(r.name, 0xff, sizeof(r.name)); r.name_length = (uint8_t)length;
        CHECK(cerberus_recon_inventory_begin(&s, 1, 1, 0));
        CHECK(cerberus_recon_inventory_offer(&s, &r) == (length <= 32));
        CHECK(cerberus_recon_inventory_end(&s, 0) == (length <= 32));
        if (length <= 32) {
            CHECK(lookup(&s, &r)->record.name_length == length);
            CHECK(!memcmp(lookup(&s, &r)->record.name, r.name, 32));
        }
    }
    for (unsigned value = 0; value <= 255; ++value) {
        cerberus_recon_inventory_init(&s, 0);
        cerberus_recon_record_t r = row(1, 1, 0);
        r.rssi = (int8_t)value;
        CHECK(cerberus_recon_inventory_begin(&s, 1, 1, 0));
        bool valid = r.rssi >= -127 && r.rssi <= 20;
        CHECK(cerberus_recon_inventory_offer(&s, &r) == valid);
        CHECK(cerberus_recon_inventory_end(&s, 0) == valid);
    }
    /* UINT64 publication and revision do not silently wrap/reuse identities. */
    cerberus_recon_inventory_init(&s, UINT64_MAX - 10);
    cerberus_recon_record_t r = row(1, 1, UINT64_MAX - 10);
    commit(&s, UINT64_MAX, &r);
    CHECK(!cerberus_recon_inventory_begin(&s, 1, 1, UINT64_MAX));
    CHECK(!cerberus_recon_inventory_begin(&s, 1, UINT64_MAX, UINT64_MAX));
    CHECK(cerberus_recon_inventory_tick(&s, UINT64_MAX));
    CHECK(lookup(&s, &r));
    s.revision = UINT64_MAX;
    r.radio = 0; r.address_type = 0;
    r.consumed_us = UINT64_MAX;
    CHECK(cerberus_recon_inventory_begin(&s, 0, 1, UINT64_MAX));
    CHECK(cerberus_recon_inventory_offer(&s, &r));
    cerberus_recon_inventory_t before = s;
    CHECK(!cerberus_recon_inventory_end(&s, UINT64_MAX));
    same_rows(&s, &before);
}

static void many_sweeps(void)
{
    cerberus_recon_inventory_t s;
    cerberus_recon_inventory_init(&s, 0);
    uint64_t publication[2] = {0};
    for (unsigned batch = 0; batch < 500; ++batch) {
        uint8_t radio = batch % 2;
        uint64_t now = (uint64_t)batch * SECOND;
        CHECK(cerberus_recon_inventory_begin(&s, radio, ++publication[radio], now));
        for (unsigned i = 0; i < 32; ++i) {
            cerberus_recon_record_t r = row(radio, (batch * 7 + i) % 101, now);
            r.generation = (uint8_t)batch;
            r.name[0] = (uint8_t)i; r.name_length = 1;
            CHECK(cerberus_recon_inventory_offer(&s, &r));
        }
        CHECK(cerberus_recon_inventory_end(&s, now));
        for (unsigned i = 0; i < 32; ++i) {
            cerberus_recon_record_t r = row(radio, (batch * 7 + i) % 101, now);
            cerberus_recon_inventory_entry_t *e = lookup(&s, &r);
            CHECK(e && e->record.name[0] == i && e->last_consumed_us == now);
        }
        invariant(&s);
    }
    CHECK(s.counters.accepted == 16000);
    CHECK(s.counters.replaced > 0 && !s.counters.expired);
    CHECK(s.count[0] == 32 && s.count[1] == 32);
}

int main(void)
{
    basic_identity_and_copies();
    atomic_failure_and_publications();
    expiry_and_radio_isolation();
    sources();
    capacity_and_stable_eviction();
    graph_and_saturation();
    invalid_inputs_and_boundaries();
    many_sweeps();
    printf("Recon inventory: %u checks passed; model=%zu entry=%zu bytes.\n",
        checks, sizeof(cerberus_recon_inventory_t), sizeof(cerberus_recon_inventory_entry_t));
    return 0;
}
