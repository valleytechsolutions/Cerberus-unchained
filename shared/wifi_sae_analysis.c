#include "wifi_sae_analysis.h"
#include <string.h>

static uint16_t le16(const uint8_t *p)
{ return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8U); }

static bool unicast(const uint8_t address[6])
{
    static const uint8_t zero[6];
    return address && !(address[0] & 1U) && memcmp(address, zero, 6U);
}

static void count(cerberus_wifi_sae_table_t *table, uint32_t *value)
{
    if (*value == UINT32_MAX) table->summary.saturated = true;
    else ++*value;
}

bool cerberus_wifi_sae_init(cerberus_wifi_sae_table_t *table, uint8_t channel,
    bool filter_bssid, const uint8_t bssid[6], const cerberus_wifi_capture_plan_t *plan)
{
    if (!table) return false;
    memset(table, 0, sizeof(*table));
    if (plan ? (channel || filter_bssid || !cerberus_wifi_capture_plan_valid(plan)) :
        (!cerberus_wifi_capture_plan_channel_bit(channel) || (filter_bssid && !unicast(bssid))))
        return false;
    table->planned = plan != NULL;
    if (plan) table->plan = *plan;
    table->channel = channel;
    table->filter_bssid = filter_bssid;
    if (filter_bssid) memcpy(table->bssid, bssid, 6U);
    table->initialized = true;
    return true;
}

static unsigned prime_length(uint16_t group)
{
    switch (group) {
    case 19U: return 32U;
    case 20U: return 48U;
    case 21U: return 66U;
    default: return 0U;
    }
}

/* Fixed fields are public packet claims. These length checks deliberately do
 * not validate a scalar, curve point, token, password identifier or handshake.
 * In particular, legacy tokens can precede the scalar while H2E extensions
 * follow the element. Extra bytes alone cannot identify either layout. */
static void body_shape(cerberus_wifi_sae_record_t *record, const uint8_t *body)
{
    bool commit = record->status == 0U || record->status == 126U || record->status == 127U;
    bool group_field = commit || record->status == 76U || record->status == 77U;
    if (!group_field) {
        if (record->body_length) record->flags |= CERBERUS_WIFI_SAE_OPAQUE_BODY;
        return;
    }
    if (record->body_length < 2U) {
        record->flags |= CERBERUS_WIFI_SAE_SHORT_BODY;
        return;
    }
    record->group_present = true;
    record->group = le16(body);
    unsigned prime = prime_length(record->group);
    if (prime) record->flags |= CERBERUS_WIFI_SAE_KNOWN_GROUP;
    if (commit) {
        if (!prime) record->flags |= CERBERUS_WIFI_SAE_OPAQUE_BODY;
        else {
            unsigned minimum = 2U + 3U * prime;
            if (record->body_length < minimum) record->flags |= CERBERUS_WIFI_SAE_SHORT_BODY;
            else if (record->body_length > minimum) record->flags |= CERBERUS_WIFI_SAE_OPAQUE_BODY;
        }
    } else if (record->status == 76U) {
        if (record->body_length == 2U) record->flags |= CERBERUS_WIFI_SAE_SHORT_BODY;
        else record->flags |= CERBERUS_WIFI_SAE_OPAQUE_BODY;
    } else if (record->body_length > 2U) record->flags |= CERBERUS_WIFI_SAE_OPAQUE_BODY;
}

static void status_count(cerberus_wifi_sae_table_t *table,
    cerberus_wifi_sae_record_t *record, uint16_t status)
{
    uint32_t *summary, *peer = NULL;
    switch (status) {
    case 0U: summary = &table->summary.commits; if (record) peer = &record->commits; break;
    case 126U: summary = &table->summary.h2e; if (record) peer = &record->h2e; break;
    case 127U: summary = &table->summary.sae_pk; if (record) peer = &record->sae_pk; break;
    case 76U: summary = &table->summary.token_requests; if (record) peer = &record->token_requests; break;
    case 77U: summary = &table->summary.group_rejections; if (record) peer = &record->group_rejections; break;
    default: summary = &table->summary.other_status; if (record) peer = &record->other_status; break;
    }
    if (record) count(table, peer);
    else count(table, summary);
}

int cerberus_wifi_sae_observe(cerberus_wifi_sae_table_t *table,
    const cerberus_wifi_sae_observation_t *metadata, const uint8_t *frame, size_t length)
{
    if (!table || !table->initialized || !metadata || !frame) return -1;
    if (!metadata->sequence || metadata->sequence <= table->last_sequence) {
        count(table, &table->summary.sequence_rejected);
        return -1;
    }
    table->last_sequence = metadata->sequence;
    uint64_t bit = cerberus_wifi_capture_plan_channel_bit(metadata->channel);
    if (table->planned ? !(bit & table->plan.channel_mask) : metadata->channel != table->channel) {
        count(table, &table->summary.scope_rejected);
        return -1;
    }
    if (metadata->observed_us > UINT64_C(0xffffffffffff) ||
        (table->have_time && metadata->observed_us < table->last_us)) {
        count(table, &table->summary.clock_rejected);
        return -1;
    }
    table->have_time = true;
    table->last_us = metadata->observed_us;
    count(table, &table->summary.received);
    if (length < 24U || length > CERBERUS_WIFI_SAE_FRAME_MAX) {
        count(table, &table->summary.malformed);
        return -1;
    }
    if (frame[0] != 0xb0U || (frame[1] & 0xc7U) || (frame[22] & 15U)) {
        count(table, &table->summary.ignored);
        return -1;
    }
    if (length < 30U) {
        count(table, &table->summary.malformed);
        return -1;
    }
    if (le16(frame + 24U) != 3U || le16(frame + 26U) != 1U ||
        !unicast(frame + 4U) || !unicast(frame + 10U) || !unicast(frame + 16U) ||
        !memcmp(frame + 4U, frame + 10U, 6U)) {
        count(table, &table->summary.ignored);
        return -1;
    }
    if (table->planned ? !cerberus_wifi_capture_plan_matches(&table->plan,
            frame + 16U, metadata->channel) :
        (table->filter_bssid && memcmp(frame + 16U, table->bssid, 6U))) {
        count(table, &table->summary.scope_rejected);
        return -1;
    }
    cerberus_wifi_sae_record_t observed = {0};
    memcpy(observed.bssid, frame + 16U, 6U);
    memcpy(observed.transmitter, frame + 10U, 6U);
    memcpy(observed.receiver, frame + 4U, 6U);
    observed.status = le16(frame + 28U);
    observed.frame_length = (uint16_t)length;
    observed.body_length = (uint16_t)(length - 30U);
    observed.mac_sequence = le16(frame + 22U) >> 4U;
    observed.channel = metadata->channel;
    observed.rssi = metadata->rssi;
    observed.rssi_valid = metadata->rssi >= -127 && metadata->rssi <= 0;
    if (frame[1] & 8U) observed.flags |= CERBERUS_WIFI_SAE_RETRY_MARKED;
    body_shape(&observed, frame + 30U);
    count(table, &table->summary.observations);
    status_count(table, NULL, observed.status);
    if (observed.flags & CERBERUS_WIFI_SAE_RETRY_MARKED) count(table, &table->summary.retry_marked);
    if (observed.flags & CERBERUS_WIFI_SAE_SHORT_BODY) count(table, &table->summary.short_bodies);
    if (observed.flags & CERBERUS_WIFI_SAE_OPAQUE_BODY) count(table, &table->summary.opaque_bodies);
    if (observed.group_present && !(observed.flags & CERBERUS_WIFI_SAE_KNOWN_GROUP))
        count(table, &table->summary.unknown_groups);

    unsigned index;
    for (index = 0U; index < table->summary.count; ++index) {
        const cerberus_wifi_sae_record_t *row = &table->records[index];
        if (!memcmp(row->bssid, observed.bssid, 6U) &&
            !memcmp(row->transmitter, observed.transmitter, 6U) &&
            !memcmp(row->receiver, observed.receiver, 6U)) break;
    }
    if (index == CERBERUS_WIFI_SAE_MAX_PEERS) {
        count(table, &table->summary.peer_dropped);
        return -1;
    }
    cerberus_wifi_sae_record_t *row = &table->records[index];
    if (index == table->summary.count) {
        ++table->summary.count;
        row->first_us = metadata->observed_us;
        row->first_sequence = metadata->sequence;
    }
    /* Replace only latest public metadata; keep stable identity/counters. */
    observed.first_us = row->first_us;
    observed.first_sequence = row->first_sequence;
    observed.observations = row->observations;
    observed.retry_marked = row->retry_marked;
    observed.commits = row->commits;
    observed.h2e = row->h2e;
    observed.sae_pk = row->sae_pk;
    observed.token_requests = row->token_requests;
    observed.group_rejections = row->group_rejections;
    observed.other_status = row->other_status;
    observed.last_us = metadata->observed_us;
    observed.sequence = metadata->sequence;
    *row = observed;
    count(table, &row->observations);
    if (observed.flags & CERBERUS_WIFI_SAE_RETRY_MARKED) count(table, &row->retry_marked);
    status_count(table, row, observed.status);
    return (int)index;
}

bool cerberus_wifi_sae_copy(const cerberus_wifi_sae_table_t *table, uint8_t index,
    cerberus_wifi_sae_record_t *out)
{
    if (out) memset(out, 0, sizeof(*out));
    if (!table || !out || !table->initialized || index >= table->summary.count ||
        index >= CERBERUS_WIFI_SAE_MAX_PEERS) return false;
    *out = table->records[index];
    return true;
}
