#include "wifi_sae_analysis.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const uint8_t ap[6] = {2, 1, 2, 3, 4, 5};
static const uint8_t sta[6] = {4, 1, 2, 3, 4, 5};
static cerberus_wifi_sae_table_t table;
static cerberus_wifi_sae_observation_t meta;
static uint8_t frame[4097];

static void reset(void)
{
    assert(cerberus_wifi_sae_init(&table, 6U, true, ap, NULL));
    meta = (cerberus_wifi_sae_observation_t){.channel = 6U, .rssi = -51};
}
static size_t auth(uint16_t status, uint16_t group, size_t body, bool from_ap)
{
    assert(body <= sizeof(frame) - 30U);
    memset(frame, 0, sizeof(frame)); frame[0] = 0xb0U;
    memcpy(frame + 4U, from_ap ? sta : ap, 6U);
    memcpy(frame + 10U, from_ap ? ap : sta, 6U);
    memcpy(frame + 16U, ap, 6U);
    frame[22] = 0x30U; frame[23] = 0x12U;
    frame[24] = 3U; frame[26] = 1U;
    frame[28] = (uint8_t)status; frame[29] = (uint8_t)(status >> 8U);
    if (body) frame[30] = (uint8_t)group;
    if (body > 1U) frame[31] = (uint8_t)(group >> 8U);
    return 30U + body;
}
static int feed(size_t length)
{
    ++meta.sequence; meta.observed_us += 100U;
    return cerberus_wifi_sae_observe(&table, &meta, frame, length);
}
static void fixed_fields(void)
{
    reset();
    assert(feed(auth(1U, 0U, 0U, true)) == 0);
    assert(table.summary.other_status == 1U && !table.records[0].group_present);
    assert(!table.records[0].flags && table.records[0].frame_length == 30U);
    assert(table.records[0].mac_sequence == 0x123U);
    assert(feed(auth(77U, 65535U, 2U, true)) == 0);
    assert(table.records[0].group_present && table.records[0].group == 65535U);
    assert(table.summary.group_rejections == 1U && table.summary.unknown_groups == 1U);
    assert(feed(auth(0U, 19U, 98U, false)) == 1);
    assert(table.records[1].flags == CERBERUS_WIFI_SAE_KNOWN_GROUP);
    assert(table.records[1].frame_length == 128U && table.records[1].body_length == 98U);
    assert(!memcmp(table.records[1].transmitter, sta, 6U));
    assert(!memcmp(table.records[0].transmitter, ap, 6U));
    assert(feed(auth(126U, 20U, 146U, false)) == 1);
    assert(table.records[1].frame_length == 176U && table.summary.h2e == 1U);
    assert(feed(auth(127U, 21U, 200U, false)) == 1);
    assert(table.records[1].frame_length == 230U && table.summary.sae_pk == 1U);
    assert(feed(auth(0U, 19U, 97U, false)) == 1);
    assert(table.records[1].flags & CERBERUS_WIFI_SAE_SHORT_BODY);
    assert(feed(auth(0U, 19U, 1U, false)) == 1);
    assert(!table.records[1].group_present && table.records[1].group == 0U);
    assert(feed(auth(0U, 99U, 2U, false)) == 1);
    assert(table.records[1].group_present && table.records[1].flags == CERBERUS_WIFI_SAE_OPAQUE_BODY);
    assert(feed(auth(76U, 19U, 0U, true)) == 0);
    assert(table.records[0].flags & CERBERUS_WIFI_SAE_SHORT_BODY);
    assert(feed(auth(76U, 19U, 2U, true)) == 0);
    assert(table.records[0].group_present && (table.records[0].flags & CERBERUS_WIFI_SAE_SHORT_BODY));
    assert(feed(auth(76U, 19U, 34U, true)) == 0);
    assert(table.records[0].flags & CERBERUS_WIFI_SAE_OPAQUE_BODY);
    assert(feed(auth(76U, 19U, 37U, true)) == 0);
    frame[32] = 255U; frame[33] = 33U; frame[34] = 93U;
    assert(feed(67U) == 0); /* A container-shaped token stays opaque. */
    assert(table.records[0].body_length == 37U && (table.records[0].flags & CERBERUS_WIFI_SAE_OPAQUE_BODY));
    assert(feed(auth(0x014dU, 19U, 2U, true)) == 0);
    assert(table.records[0].status == 0x014dU && !table.records[0].group_present);
    assert(feed(auth(123U, 19U, 2U, true)) == 0 && !table.records[0].group_present);
}

static void bounds_and_scope(void)
{
    reset();
    (void)auth(1U, 0U, 0U, true);
    for (size_t length = 0U; length < 30U; ++length) assert(feed(length) == -1);
    assert(table.summary.count == 0U && table.summary.malformed == 30U);
    assert(feed(sizeof(frame)) == -1);
    assert(feed(auth(1U, 0U, 4066U, true)) == 0);
    const uint8_t flags[] = {1U, 2U, 4U, 0x40U, 0x80U};
    for (unsigned i = 0U; i < sizeof(flags); ++i) {
        size_t length = auth(0U, 19U, 98U, true); frame[1] = flags[i]; assert(feed(length) == -1);
    }
    (void)auth(0U, 19U, 98U, true); frame[22] |= 1U; assert(feed(128U) == -1);
    frame[22] &= 0xf0U; frame[0] = 0xb1U; assert(feed(128U) == -1);
    frame[0] = 8U; assert(feed(128U) == -1);
    frame[0] = 0xb0U; frame[25] = 1U; assert(feed(128U) == -1);
    frame[25] = 0U; frame[27] = 1U; assert(feed(128U) == -1);
    frame[27] = 0U; frame[26] = 2U; assert(feed(128U) == -1);
    frame[26] = 1U; frame[10] |= 1U; assert(feed(128U) == -1);
    frame[10] &= 0xfeU; frame[16] ^= 2U; assert(feed(128U) == -1);
    assert(table.summary.scope_rejected == 1U);
    frame[16] ^= 2U; meta.channel = 36U; assert(feed(128U) == -1);
    assert(table.summary.scope_rejected == 2U);

    cerberus_wifi_capture_plan_t plan;
    cerberus_wifi_capture_plan_clear(&plan);
    assert(cerberus_wifi_capture_plan_add_target(&plan, ap, 6U));
    assert(cerberus_wifi_capture_plan_add_target(&plan, ap, 36U));
    assert(cerberus_wifi_sae_init(&table, 0U, false, NULL, &plan));
    plan.channel_mask = 0U; memset(plan.bssids, 0, sizeof(plan.bssids));
    assert(feed(128U) == 0); /* Scope is frozen; it never borrows caller memory. */
    meta.channel = 6U; assert(feed(128U) == 0);
    meta.channel = 11U; assert(feed(128U) == -1);
    assert(table.summary.scope_rejected == 1U);
    assert(!cerberus_wifi_sae_init(&table, 0U, false, NULL, &plan));
    assert(!cerberus_wifi_sae_init(&table, 32U, false, NULL, NULL));
    assert(!cerberus_wifi_sae_init(&table, 6U, true, NULL, NULL));
}

static void identity_time_and_limits(void)
{
    reset(); size_t length = auth(0U, 19U, 98U, false);
    assert(feed(length) == 0);
    cerberus_wifi_sae_record_t frozen = table.records[0];
    assert(cerberus_wifi_sae_observe(&table, &meta, frame, length) == -1);
    assert(table.summary.sequence_rejected == 1U && !memcmp(&frozen, &table.records[0], sizeof(frozen)));
    frame[1] = 8U; assert(feed(length) == 0);
    assert(table.summary.retry_marked == 1U && table.records[0].retry_marked == 1U);
    assert(table.records[0].first_sequence == 1U && table.records[0].sequence == 2U);
    meta.observed_us = 0U; ++meta.sequence;
    assert(cerberus_wifi_sae_observe(&table, &meta, frame, length) == -1);
    assert(table.summary.clock_rejected == 1U && table.records[0].last_us == 200U);
    meta.observed_us = UINT64_C(0x1000000000000); ++meta.sequence;
    assert(cerberus_wifi_sae_observe(&table, &meta, frame, length) == -1);
    meta.observed_us = 500U; meta.rssi = 127; assert(feed(length) == 0);
    assert(!table.records[0].rssi_valid);
    meta.sequence = UINT32_MAX; ++meta.observed_us;
    assert(cerberus_wifi_sae_observe(&table, &meta, frame, length) == 0);
    meta.sequence = 1U; assert(cerberus_wifi_sae_observe(&table, &meta, frame, length) == -1);

    reset();
    for (unsigned i = 0U; i < CERBERUS_WIFI_SAE_MAX_PEERS; ++i) {
        length = auth(1U, 0U, 0U, false); frame[15] = (uint8_t)i;
        assert(feed(length) == (int)i);
    }
    frozen = table.records[0]; frame[15] = 99U;
    assert(feed(length) == -1 && table.summary.peer_dropped == 1U);
    assert(!memcmp(&frozen, &table.records[0], sizeof(frozen)));
    frame[15] = 0U; assert(feed(length) == 0 && table.records[0].observations == 2U);
    table.summary.received = UINT32_MAX; table.summary.observations = UINT32_MAX;
    table.records[0].observations = UINT32_MAX;
    assert(feed(length) == 0 && table.summary.saturated);
    assert(table.summary.received == UINT32_MAX && table.records[0].observations == UINT32_MAX);
    cerberus_wifi_sae_record_t copy;
    memset(&copy, 0x5a, sizeof(copy));
    assert(!cerberus_wifi_sae_copy(&table, 16U, &copy) && !copy.first_sequence);
}

static void no_payload_retention(void)
{
    reset(); size_t length = auth(126U, 19U, 133U, false);
    memset(frame + 32U, 0xa5, length - 32U);
    assert(feed(length) == 0);
    cerberus_wifi_sae_table_t first = table;
    reset(); length = auth(126U, 19U, 133U, false);
    memset(frame + 32U, 0x5a, length - 32U);
    assert(feed(length) == 0);
    assert(!memcmp(&first, &table, sizeof(table)));
}

int main(void)
{
    fixed_fields(); bounds_and_scope(); identity_time_and_limits(); no_payload_retention();
    printf("SAE evidence: fixed fields, conditional groups, body bounds, opaque data, scope/time/identity; %zu-byte table\n", sizeof(table));
    return 0;
}
