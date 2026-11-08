#include "wifi_eapol_analysis.h"
#include "wifi_beacon_analysis.h"
#include <string.h>

typedef struct {
    uint8_t bssid[6], station[6], nonce[32], descriptor, version, phase, mic_length;
    uint64_t replay;
    uint16_t pmkids;
    bool group, encrypted, unknown_layout;
} key_observation_t;

enum { IGNORE, OTHER, MALFORMED, KEY, SCOPE };
static uint16_t be16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] << 8U | p[1]); }
static uint16_t le16(const uint8_t *p) { return (uint16_t)((uint16_t)p[1] << 8U | p[0]); }
static uint64_t be64(const uint8_t *p)
{
    uint64_t value = 0U;
    for (unsigned i = 0; i < 8U; ++i) value = (value << 8U) | p[i];
    return value;
}
static bool zero(const uint8_t *p, size_t length)
{
    for (size_t i = 0; i < length; ++i) if (p[i]) return false;
    return true;
}
static bool unicast(const uint8_t *p) { return !(p[0] & 1U) && !zero(p, 6U); }
static bool scope_matches(const cerberus_wifi_eapol_table_t *t,
    const uint8_t bssid[6], uint8_t channel)
{
    if (t->planned) return cerberus_wifi_capture_plan_matches(&t->plan, bssid, channel);
    return channel == t->channel && (!t->filter_bssid || !memcmp(t->bssid, bssid, 6U));
}
static void add(cerberus_wifi_eapol_table_t *t, uint32_t *value, uint32_t count)
{
    if (UINT32_MAX - *value < count) { *value = UINT32_MAX; t->summary.saturated = true; }
    else *value += count;
}

/* Key data is a bounded IE stream only when transmitted in the clear. A
 * recognized element before a malformed tail does not make the frame valid. */
static bool pmkid_elements(const uint8_t *data, size_t length, uint16_t *count)
{
    *count = 0U;
    /* Locate zero padding once. Rechecking the whole remaining suffix at each
     * zero-length IE would make a long sequence of such IEs quadratic. */
    size_t nonzero_end = length;
    while (nonzero_end && !data[nonzero_end - 1U]) --nonzero_end;
    while (length) {
        if (!nonzero_end) return true;
        if (length < 2U || (size_t)data[1] + 2U > length) return false;
        size_t size = (size_t)data[1] + 2U;
        if (data[0] == 221U && data[1] == 0U) return nonzero_end <= 2U;
        if (data[0] == 221U && data[1] >= 4U && !memcmp(data + 2U, "\x00\x0f\xac\x04", 4U)) {
            if (data[1] != 20U) return false;
            ++*count;
        }
        data += size;
        length -= size;
        nonzero_end = nonzero_end > size ? nonzero_end - size : 0U;
    }
    return true;
}

static unsigned parse(const cerberus_wifi_eapol_table_t *t, const uint8_t *frame,
    size_t length, uint8_t channel, key_observation_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!frame || length < 24U || length > CERBERUS_WIFI_EAPOL_FRAME_MAX) return MALFORMED;
    uint16_t fc = le16(frame);
    unsigned subtype = (fc >> 4U) & 15U, direction = (fc >> 8U) & 3U;
    if ((fc & 0x000fU) != 0x0008U || (subtype & 4U) || direction < 1U || direction > 2U)
        return IGNORE;
    const uint8_t *ap = frame + (direction == 1U ? 4U : 10U);
    const uint8_t *station = frame + (direction == 1U ? 10U : 4U);
    if (!unicast(ap) || !unicast(station) || !memcmp(ap, station, 6U)) return IGNORE;
    if (!scope_matches(t, ap, channel)) return SCOPE;
    if ((fc & (0x0400U | 0x4000U)) || (le16(frame + 22U) & 15U)) return MALFORMED;
    size_t header = 24U;
    if (subtype & 8U) {
        if (length < 26U || (frame[24] & 0x80U)) return MALFORMED;
        header += 2U;
        if (fc & 0x8000U) header += 4U;
    } else if (fc & 0x8000U) return IGNORE;
    if (length < header + 8U) return MALFORMED;
    if (memcmp(frame + header, "\xaa\xaa\x03\x00\x00\x00\x88\x8e", 8U)) return IGNORE;
    const uint8_t *eapol = frame + header + 8U;
    size_t available = length - header - 8U;
    if (available < 4U || !eapol[0] || eapol[0] > 3U) return MALFORMED;
    size_t body_length = be16(eapol + 2U);
    if (body_length > available - 4U) return MALFORMED;
    if (eapol[1] != 3U) return OTHER;
    const uint8_t *key = eapol + 4U;
    if (!body_length) return MALFORMED;
    if (key[0] != 2U && key[0] != 254U) return OTHER;
    if (body_length < 95U) return MALFORMED;
    uint16_t info = be16(key + 1U);
    out->descriptor = key[0]; out->version = (uint8_t)(info & 7U);
    if (out->version > 3U || (info & 0xc000U)) return OTHER;
    out->group = !(info & 0x0008U);
    out->encrypted = (info & 0x1000U) != 0U;
    memcpy(out->bssid, ap, 6U);
    memcpy(out->station, station, 6U);
    memcpy(out->nonce, key + 13U, 32U);
    out->replay = be64(key + 5U);

    /* Version0 delegates MIC size to the negotiated AKM. Without negotiated
     * context, accept only a unique exact-length 16/24-byte layout. Ambiguous
     * layouts keep phase-pattern evidence but cannot complete an exchange or
     * yield PMKID presence. Other descriptor versions have a 16-byte MIC. */
    bool layout16 = be16(key + 93U) == body_length - 95U;
    bool layout24 = body_length >= 103U && be16(key + 101U) == body_length - 103U;
    if (out->version) {
        if (!layout16) return MALFORMED;
        out->mic_length = 16U;
    } else if (layout16 != layout24) out->mic_length = layout16 ? 16U : 24U;
    else {
        if (!layout16) return MALFORMED;
        out->unknown_layout = true;
    }
    size_t data_length = out->mic_length ? body_length - 79U - out->mic_length : 0U;
    if (out->descriptor == 2U && out->mic_length && !out->encrypted &&
        !pmkid_elements(key + 79U + out->mic_length, data_length, &out->pmkids)) return MALFORMED;

    bool ack = (info & 0x0080U) != 0U, mic = (info & 0x0100U) != 0U;
    bool secure = (info & 0x0200U) != 0U, install = (info & 0x0040U) != 0U;
    bool nonce = !zero(out->nonce, 32U);
    bool mic_shape = !out->mic_length || (mic != zero(key + 77U, out->mic_length));
    if (!out->group && !(info & (0x0030U | 0x0400U | 0x0800U | 0x2000U)) && mic_shape) {
        if (direction == 2U && ack && !mic && !secure && !install && !out->encrypted && nonce)
            out->phase = CERBERUS_WIFI_EAPOL_M1;
        else if (direction == 1U && !ack && mic && !secure && !install && !out->encrypted && nonce)
            out->phase = CERBERUS_WIFI_EAPOL_M2;
        else if (direction == 2U && ack && mic && install && nonce && (secure || out->descriptor == 254U))
            out->phase = CERBERUS_WIFI_EAPOL_M3;
        else if (direction == 1U && !ack && mic && !install && !out->encrypted && !nonce &&
            out->mic_length && !data_length && (secure || out->descriptor == 254U))
            out->phase = CERBERUS_WIFI_EAPOL_M4;
    }
    return KEY;
}

bool cerberus_wifi_eapol_init_options(cerberus_wifi_eapol_table_t *t,
    uint8_t channel, bool filter_bssid, const uint8_t bssid[6],
    const cerberus_wifi_capture_plan_t *plan)
{
    if (!t) return false;
    memset(t, 0, sizeof(*t));
    if (plan) {
        if (channel || filter_bssid || !cerberus_wifi_capture_plan_valid(plan) ||
            plan->dwell_ms != CERBERUS_WIFI_CAPTURE_PLAN_EAPOL_DWELL_MS) return false;
        t->plan = *plan; t->planned = true;
    } else {
        if (!cerberus_wifi_capture_plan_channel_bit(channel) ||
            (filter_bssid && (!bssid || !unicast(bssid)))) return false;
        if (filter_bssid) memcpy(t->bssid, bssid, 6U);
        t->channel = channel; t->filter_bssid = filter_bssid;
    }
    t->initialized = true;
    return true;
}

bool cerberus_wifi_eapol_init(cerberus_wifi_eapol_table_t *t, const uint8_t bssid[6], uint8_t channel)
{ return cerberus_wifi_eapol_init_options(t, channel, true, bssid, NULL); }

static int slot(cerberus_wifi_eapol_table_t *t, const uint8_t bssid[6],
    const uint8_t station[6], uint64_t now, uint32_t sequence)
{
    for (uint8_t i = 0; i < t->summary.count; ++i)
        if (!memcmp(t->records[i].bssid, bssid, 6U) &&
            !memcmp(t->records[i].station, station, 6U)) return i;
    int index = t->summary.count;
    if (index < (int)CERBERUS_WIFI_EAPOL_MAX_PEERS) ++t->summary.count;
    else {
        index = -1;
        for (uint8_t i = 0; i < t->summary.count; ++i) {
            const cerberus_wifi_eapol_exchange_t *e = &t->exchange[i];
            bool pending = e->stage > 0U && e->stage < 4U && now >= e->started_us &&
                now - e->started_us <= CERBERUS_WIFI_EAPOL_EXCHANGE_US;
            if (!pending && (index < 0 || t->records[i].last_us < t->records[index].last_us)) index = i;
        }
        if (index < 0) { add(t, &t->summary.peer_dropped, 1U); return -1; }
        add(t, &t->summary.peer_replaced, 1U);
    }
    memset(&t->records[index], 0, sizeof(t->records[index]));
    memset(&t->exchange[index], 0, sizeof(t->exchange[index]));
    memcpy(t->records[index].bssid, bssid, 6U);
    memcpy(t->records[index].station, station, 6U); t->records[index].first_us = now;
    t->records[index].first_sequence = sequence;
    return index;
}

static void correlate(cerberus_wifi_eapol_table_t *t, unsigned index, const key_observation_t *key,
    uint64_t now, uint8_t channel)
{
    cerberus_wifi_eapol_exchange_t *e = &t->exchange[index];
    cerberus_wifi_eapol_record_t *r = &t->records[index];
    if (e->stage && e->channel != channel) {
        if (e->stage < 4U) add(t, &r->gaps, 1U);
        memset(e, 0, sizeof(*e));
        if (key->phase != CERBERUS_WIFI_EAPOL_M1) return;
    }
    if (e->stage && now - e->started_us > CERBERUS_WIFI_EAPOL_EXCHANGE_US) {
        if (e->stage < 4U) add(t, &r->gaps, 1U);
        memset(e, 0, sizeof(*e));
    }
    if (!key->phase || !key->mic_length) return;
    bool same = e->stage && key->descriptor == e->descriptor && key->version == e->version &&
        key->mic_length == e->mic_length;
    /* An AP can increase the replay counter when retransmitting M3. Retain
     * the observed exchange and require M4 to answer that latest counter;
     * a repeated completed exchange never increments completion again. */
    if (same && key->phase == 3U && e->stage >= 3U && key->replay > e->replay_m3 &&
        !memcmp(key->nonce, e->anonce, 32U)) {
        e->replay_m3 = key->replay;
        add(t, &r->retries, 1U); add(t, &t->summary.retries, 1U); return;
    }
    bool retry = same && ((key->phase == 1U && key->replay == e->replay_m1 && !memcmp(key->nonce, e->anonce, 32U)) ||
        (key->phase == 2U && e->stage >= 2U && key->replay == e->replay_m1 && !memcmp(key->nonce, e->snonce, 32U)) ||
        (key->phase == 3U && e->stage >= 3U && key->replay == e->replay_m3 && !memcmp(key->nonce, e->anonce, 32U)) ||
        (key->phase == 4U && e->stage == 4U && key->replay == e->replay_m3));
    if (retry) { add(t, &r->retries, 1U); add(t, &t->summary.retries, 1U); return; }
    if (key->phase == 1U) {
        if (e->stage && e->stage < 4U) add(t, &r->gaps, 1U);
        memset(e, 0, sizeof(*e));
        e->stage = 1U; e->started_us = now; e->replay_m1 = key->replay; e->channel = channel;
        e->descriptor = key->descriptor; e->version = key->version; e->mic_length = key->mic_length;
        memcpy(e->anonce, key->nonce, 32U);
    } else if (same && key->phase == 2U && e->stage == 1U && key->replay == e->replay_m1 &&
        memcmp(key->nonce, e->anonce, 32U)) {
        e->stage = 2U; memcpy(e->snonce, key->nonce, 32U);
    } else if (same && key->phase == 3U && e->stage == 2U && key->replay > e->replay_m1 &&
        !memcmp(key->nonce, e->anonce, 32U)) {
        e->stage = 3U; e->replay_m3 = key->replay;
    } else if (same && key->phase == 4U && e->stage == 3U && key->replay == e->replay_m3) {
        e->stage = 4U; add(t, &r->exchanges, 1U); add(t, &t->summary.exchanges, 1U);
    } else { add(t, &r->gaps, 1U); memset(e, 0, sizeof(*e)); }
}

static int observe_beacon(cerberus_wifi_eapol_table_t *t,
    const cerberus_wifi_eapol_observation_t *meta, const uint8_t *frame, size_t length)
{
    if (length >= 24U && !scope_matches(t, frame + 16U, meta->channel)) {
        add(t, &t->summary.scope_rejected, 1U); return -1;
    }
    cerberus_wifi_beacon_observation_t beacon;
    if (!cerberus_wifi_beacon_decode(frame, length, meta->channel, meta->rssi,
            meta->observed_us, &beacon)) {
        add(t, &t->summary.beacon_malformed, 1U); return -1;
    }
    add(t, &t->summary.beacons, 1U);
    unsigned index = 0U;
    while (index < t->summary.context_count &&
        memcmp(t->contexts[index].bssid, beacon.bssid, 6U)) ++index;
    if (index == CERBERUS_WIFI_EAPOL_MAX_CONTEXTS) {
        add(t, &t->summary.beacon_dropped, 1U); return -1;
    }
    cerberus_wifi_eapol_context_t *context = &t->contexts[index];
    if (index == t->summary.context_count) {
        ++t->summary.context_count;
        context->first_sequence = meta->sequence; context->first_us = meta->observed_us;
    }
    memcpy(context->bssid, beacon.bssid, 6U);
    memcpy(context->transmitter, beacon.transmitter, 6U);
    memcpy(context->ssid, beacon.ssid, sizeof(context->ssid));
    context->ssid_length = beacon.ssid_length; context->hidden_like = beacon.hidden_like;
    context->channel = beacon.rx_channel; context->advertised_channel = beacon.advertised_channel;
    context->capabilities = beacon.capabilities; context->rssi = beacon.rssi;
    context->rssi_valid = beacon.rssi >= -127 && beacon.rssi <= 0;
    context->sequence = meta->sequence; context->last_us = meta->observed_us;
    add(t, &context->frames, 1U);
    return (int)index;
}

cerberus_wifi_eapol_result_t cerberus_wifi_eapol_observe_full(cerberus_wifi_eapol_table_t *t,
    const cerberus_wifi_eapol_observation_t *meta, const uint8_t *frame, size_t length)
{
    cerberus_wifi_eapol_result_t result = {-1, -1};
    if (!t || !t->initialized || !meta || !frame) return result;
    if (!meta->sequence || meta->sequence <= t->last_sequence) {
        add(t, &t->summary.sequence_rejected, 1U); return result;
    }
    t->last_sequence = meta->sequence;
    if (t->planned ? !(t->plan.channel_mask & cerberus_wifi_capture_plan_channel_bit(meta->channel)) :
        meta->channel != t->channel) { add(t, &t->summary.scope_rejected, 1U); return result; }
    if (t->have_time && meta->observed_us < t->last_us) {
        add(t, &t->summary.clock_rejected, 1U); return result;
    }
    t->have_time = true; t->last_us = meta->observed_us;
    add(t, &t->summary.received, 1U);
    if (length && frame[0] == 0x80U) {
        result.context_index = (int8_t)observe_beacon(t, meta, frame, length);
        return result;
    }
    key_observation_t key;
    unsigned kind = parse(t, frame, length, meta->channel, &key);
    if (kind != KEY) {
        add(t, kind == IGNORE ? &t->summary.ignored : kind == OTHER ? &t->summary.other :
            kind == SCOPE ? &t->summary.scope_rejected : &t->summary.malformed, 1U);
        return result;
    }
    add(t, &t->summary.keys, 1U);
    if (key.phase) add(t, &t->summary.phase[key.phase - 1U], 1U);
    if (key.group) add(t, &t->summary.group_keys, 1U);
    if (key.encrypted) add(t, &t->summary.encrypted_key_data, 1U);
    if (key.unknown_layout) add(t, &t->summary.unknown_layout, 1U);
    if (key.pmkids) { add(t, &t->summary.pmkid_frames, 1U); add(t, &t->summary.pmkid_elements, key.pmkids); }
    int index = slot(t, key.bssid, key.station, meta->observed_us, meta->sequence);
    if (index < 0) return result;
    cerberus_wifi_eapol_record_t *r = &t->records[index];
    r->last_us = meta->observed_us; r->sequence = meta->sequence; r->channel = meta->channel;
    r->rssi = meta->rssi; r->rssi_valid = meta->rssi >= -127 && meta->rssi <= 0;
    r->last_phase = key.phase; add(t, &r->keys, 1U);
    if (key.phase) add(t, &r->phase[key.phase - 1U], 1U);
    if (key.pmkids) { add(t, &r->pmkid_frames, 1U); add(t, &r->pmkid_elements, key.pmkids); }
    correlate(t, (unsigned)index, &key, meta->observed_us, meta->channel);
    result.peer_index = (int8_t)index; return result;
}

int cerberus_wifi_eapol_observe(cerberus_wifi_eapol_table_t *t,
    const cerberus_wifi_eapol_observation_t *meta, const uint8_t *frame, size_t length)
{ return cerberus_wifi_eapol_observe_full(t, meta, frame, length).peer_index; }

bool cerberus_wifi_eapol_copy(const cerberus_wifi_eapol_table_t *t, uint8_t index,
    cerberus_wifi_eapol_record_t *out)
{
    if (out) memset(out, 0, sizeof(*out));
    if (!t || !out || !t->initialized || index >= t->summary.count || index >= CERBERUS_WIFI_EAPOL_MAX_PEERS)
        return false;
    *out = t->records[index]; return true;
}

bool cerberus_wifi_eapol_context_copy(const cerberus_wifi_eapol_table_t *t, uint8_t index,
    cerberus_wifi_eapol_context_t *out)
{
    if (out) memset(out, 0, sizeof(*out));
    if (!t || !out || !t->initialized || index >= t->summary.context_count ||
        index >= CERBERUS_WIFI_EAPOL_MAX_CONTEXTS) return false;
    *out = t->contexts[index]; return true;
}
