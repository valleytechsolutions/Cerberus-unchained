#include "wifi_frame_analysis.h"
#include "wifi_capture_filter.h"
#include "cerberus_protocol.h"
#include <string.h>

static void add(cerberus_wifi_frame_summary_t *s, uint32_t *value)
{
    if (*value == UINT32_MAX) s->saturated = true;
    else { ++*value; if (*value == UINT32_MAX) s->saturated = true; }
}

static bool valid_mode(uint8_t mode)
{
    return mode == CERBERUS_WIFI_CAPTURE_MODE_RAW ||
        mode == CERBERUS_WIFI_CAPTURE_MODE_PROBE || mode == CERBERUS_WIFI_CAPTURE_MODE_DEAUTH;
}

bool cerberus_wifi_frame_init(cerberus_wifi_frame_model_t *m, uint8_t channel,
    uint8_t mode, bool filter_bssid, const uint8_t bssid[6],
    const cerberus_wifi_capture_plan_t *plan)
{
    static const uint8_t zero[6];
    if (!m || !valid_mode(mode)) return false;
    if (plan) {
        if (channel || filter_bssid || !cerberus_wifi_capture_plan_valid(plan) ||
            !cerberus_wifi_capture_plan_mode_valid(mode, plan->dwell_ms) ||
            (mode == CERBERUS_WIFI_CAPTURE_MODE_PROBE && plan->target_count)) return false;
    } else if (!cerberus_wifi_capture_plan_channel_bit(channel) ||
        (filter_bssid && (!bssid || (bssid[0] & 1U) || !memcmp(bssid, zero, 6U) ||
            mode == CERBERUS_WIFI_CAPTURE_MODE_PROBE))) return false;
    memset(m, 0, sizeof(*m));
    m->initialized = true; m->channel = channel; m->mode_mask = mode;
    m->planned = plan != NULL; m->filter_bssid = filter_bssid;
    if (plan) m->plan = *plan;
    if (filter_bssid) memcpy(m->bssid, bssid, 6U);
    return true;
}

static bool same_event(const cerberus_wifi_frame_record_t *a,
    const cerberus_wifi_frame_record_t *b)
{
    return !memcmp(a->bssid, b->bssid, 6U) && !memcmp(a->transmitter, b->transmitter, 6U) &&
        !memcmp(a->receiver, b->receiver, 6U) && a->channel == b->channel &&
        a->subtype == b->subtype && a->reason_known == b->reason_known &&
        a->reason == b->reason && a->protected_frame == b->protected_frame &&
        a->fragmented == b->fragmented;
}

static int record_event(cerberus_wifi_frame_model_t *m,
    const cerberus_wifi_frame_observation_t *o, const uint8_t *f, size_t length)
{
    cerberus_wifi_frame_record_t next = {0};
    memcpy(next.receiver, f + 4U, 6U); memcpy(next.transmitter, f + 10U, 6U);
    memcpy(next.bssid, f + 16U, 6U);
    next.subtype = f[0] >> 4U; next.channel = o->channel;
    next.protected_frame = (f[1] & 0x40U) != 0U;
    next.fragmented = (f[1] & 4U) != 0U || (f[22] & 15U) != 0U;
    next.reason_known = !next.protected_frame && !next.fragmented && !(f[1] & 0x80U) && length >= 26U;
    if (next.reason_known) next.reason = (uint16_t)f[24] | (uint16_t)f[25] << 8U;
    unsigned slot = m->summary.count;
    for (unsigned i = 0U; i < m->summary.count; ++i)
        if (same_event(&m->records[i], &next)) { slot = i; break; }
    if (slot == m->summary.count) {
        if (slot == CERBERUS_WIFI_FRAME_MAX_EVENTS) {
            slot = 0U;
            for (unsigned i = 1U; i < CERBERUS_WIFI_FRAME_MAX_EVENTS; ++i)
                if (m->records[i].sequence < m->records[slot].sequence) slot = i;
            add(&m->summary, &m->summary.event_replaced);
        } else ++m->summary.count;
        m->records[slot] = next;
        m->records[slot].first_sequence = o->sequence;
        m->records[slot].first_us = o->observed_us;
    }
    cerberus_wifi_frame_record_t *row = &m->records[slot];
    add(&m->summary, &row->frames);
    if (f[1] & 8U) add(&m->summary, &row->retries);
    row->sequence = o->sequence; row->last_us = o->observed_us;
    row->rssi = o->rssi; row->rssi_valid = o->rssi >= -127 && o->rssi <= 0;
    return (int)slot;
}

cerberus_wifi_frame_result_t cerberus_wifi_frame_observe(cerberus_wifi_frame_model_t *m,
    const cerberus_wifi_frame_observation_t *o, const uint8_t *f, size_t length)
{
    cerberus_wifi_frame_result_t result = {.event_index = -1};
    if (!m || !m->initialized || !o) return result;
    cerberus_wifi_frame_summary_t *s = &m->summary;
    if (!o->sequence || o->sequence <= m->last_sequence) {
        add(s, &s->sequence_rejected); return result;
    }
    if (m->planned ? !(m->plan.channel_mask & cerberus_wifi_capture_plan_channel_bit(o->channel)) :
        o->channel != m->channel) { add(s, &s->scope_rejected); return result; }
    if (o->observed_us > UINT64_C(0xffffffffffff) || (m->have_time && o->observed_us < m->last_us)) {
        add(s, &s->clock_rejected); return result;
    }
    m->last_sequence = o->sequence; m->last_us = o->observed_us; m->have_time = true;
    const uint8_t *bssid = cerberus_wifi_capture_filter_bssid(f, length);
    if ((m->filter_bssid && (!bssid || memcmp(m->bssid, bssid, 6U))) ||
        (m->planned && m->plan.target_count &&
            !cerberus_wifi_capture_plan_matches(&m->plan, bssid, o->channel))) {
        add(s, &s->scope_rejected); return result;
    }
    if (m->mode_mask != CERBERUS_WIFI_CAPTURE_MODE_RAW &&
        !cerberus_wifi_capture_filter_match(m->mode_mask, false, NULL, f, length)) {
        add(s, &s->scope_rejected); return result;
    }
    result.delivered = true;
    if (!s->received) s->first_us = o->observed_us;
    add(s, &s->received);
    if (length > UINT64_MAX - s->bytes) { s->bytes = UINT64_MAX; s->saturated = true; }
    else s->bytes += length;
    s->last_us = o->observed_us; s->sequence = o->sequence; s->channel = o->channel;
    s->rssi = o->rssi; s->rssi_valid = o->rssi >= -127 && o->rssi <= 0;
    if (s->rssi_valid) {
        if (!s->extrema_valid || o->rssi < s->min_rssi) s->min_rssi = o->rssi;
        if (!s->extrema_valid || o->rssi > s->max_rssi) s->max_rssi = o->rssi;
        s->extrema_valid = true;
    }
    if (!f || length < 2U || length > CERBERUS_WIFI_FRAME_MAX_LENGTH || (f[0] & 3U)) {
        add(s, &s->malformed); return result;
    }
    unsigned type = (f[0] >> 2U) & 3U, subtype = f[0] >> 4U;
    if (type == 3U) { add(s, &s->other); return result; }
    size_t header = type == 1U ? ((subtype == 12U || subtype == 13U) ? 10U : 16U) : 24U;
    bool qos = type == 2U && (subtype & 8U);
    if (type == 2U) {
        if ((f[1] & 3U) == 3U) header += 6U;
        if (qos) header += 2U;
        if (qos && (f[1] & 0x80U)) header += 4U;
    } else if (type == 0U && (f[1] & 0x80U)) header += 4U;
    if (length < header || (type == 0U && (f[1] & 3U))) {
        add(s, &s->malformed); return result;
    }
    if (f[1] & 8U) add(s, &s->retries);
    if (type == 1U) { add(s, &s->control); return result; }
    if (type == 0U) {
        add(s, &s->management);
        if (subtype == 8U) { if (length >= header + 12U) add(s, &s->beacons); else add(s, &s->malformed); }
        if (subtype == 4U) add(s, &s->probe_requests);
        if (subtype == 5U) { if (length >= header + 12U) add(s, &s->probe_responses); else add(s, &s->malformed); }
        if (subtype == 10U || subtype == 12U) {
            add(s, subtype == 12U ? &s->deauth : &s->disassoc);
            if (!(f[1] & 0x44U) && !(f[22] & 15U) && length < header + 2U) add(s, &s->malformed);
            result.event_index = (int8_t)record_event(m, o, f, length);
        }
        return result;
    }
    add(s, &s->data);
    /* Only an unfragmented, clear single-MSDU payload establishes LLC/EAPOL.
     * Null-data and A-MSDU layouts are deliberately not decoded here. */
    if ((subtype & 4U) || (f[1] & 0x44U) || (f[22] & 15U)) return result;
    size_t qos_offset = 24U + ((f[1] & 3U) == 3U ? 6U : 0U);
    if (qos && (f[qos_offset] & 0x80U)) return result;
    static const uint8_t llc[8] = {0xaa, 0xaa, 3, 0, 0, 0, 0x88, 0x8e};
    if (length < header + 8U || memcmp(f + header, llc, sizeof(llc))) return result;
    if (length < header + 12U) { add(s, &s->malformed); return result; }
    const uint8_t *e = f + header + 8U;
    size_t body = (size_t)e[2] << 8U | e[3];
    if (e[0] < 1U || e[0] > 3U || body > length - header - 12U) add(s, &s->malformed);
    else add(s, &s->eapol_headers);
    return result;
}

bool cerberus_wifi_frame_copy(const cerberus_wifi_frame_model_t *m, uint8_t index,
    cerberus_wifi_frame_record_t *out)
{
    if (out) memset(out, 0, sizeof(*out));
    if (!m || !m->initialized || !out || index >= m->summary.count) return false;
    *out = m->records[index]; return true;
}
