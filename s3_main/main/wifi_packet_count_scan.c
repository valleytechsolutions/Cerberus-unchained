#include "wifi_packet_count_scan.h"
#include <limits.h>
#include <string.h>

static uint64_t later(uint64_t now, uint64_t delay)
{ return UINT64_MAX - now < delay ? UINT64_MAX : now + delay; }
static uint64_t get(const uint8_t *p, unsigned n)
{ return cerberus_packet_count_get(p, n); }
static bool terminal(uint8_t state)
{ return state == CERBERUS_PACKET_COUNT_COMPLETE || state == CERBERUS_PACKET_COUNT_ERROR; }
static uint16_t configured(unsigned count)
{ return count == 16U ? UINT16_MAX : (uint16_t)((1U << count) - 1U); }
static bool source_released(const wifi_packet_count_source_t *s)
{ return !s->request.session_id && s->phase == WIFI_COUNT_IDLE && !s->record_bound; }
static bool target_same(const cerberus_wifi_packet_count_target_t *a,
    const cerberus_wifi_packet_count_target_t *b)
{ return a->role == b->role && !memcmp(a->mac, b->mac, sizeof(a->mac)); }
static uint64_t add(uint64_t a, uint64_t b, bool *saturated)
{ if (UINT64_MAX - a < b) { *saturated = true; return UINT64_MAX; } return a + b; }
static void row_add(wifi_packet_count_row_totals_t *out,
    const cerberus_wifi_packet_count_row_t *r, bool *saturated)
{
#define SUM(field) out->field = add(out->field, r->field, saturated)
    SUM(observations); SUM(transmitted); SUM(addressed); SUM(bssid_context);
    SUM(management); SUM(data); SUM(retry); SUM(protected_frames); SUM(fragmented);
#undef SUM
    *saturated |= (r->flags & CERBERUS_WIFI_PACKET_COUNT_ROW_SATURATED) != 0;
}
static void commit(wifi_packet_count_source_t *s)
{
    if (s->committed || !s->final_valid || !s->published.frozen) return;
    wifi_packet_count_totals_t *t = &s->completed;
    const cerberus_wifi_packet_count_summary_t *v = &s->published.summary;
    for (unsigned i = 0; i < v->count; ++i) row_add(&t->rows[i], &s->published.rows[i], &t->saturated);
    for (unsigned i = 0; i < CERBERUS_WIFI_PACKET_COUNT_CHANNELS; ++i)
        t->channel_window_us[i] = add(t->channel_window_us[i], s->published.channel_window_us[i], &t->saturated);
#define SUM(field) t->field = add(t->field, v->field, &t->saturated)
    SUM(callbacks); SUM(matched_frames); SUM(invalid_metadata); SUM(malformed_frames);
    SUM(unsupported_frames); SUM(clock_dropped_frames); SUM(outside_window_frames);
    SUM(valid_frames); SUM(channel_failures);
#undef SUM
    t->raw_dropped_full=add(t->raw_dropped_full,s->published.recording.dropped_full,&t->saturated);
    t->raw_dropped_other=add(t->raw_dropped_other,s->published.recording.dropped_other,&t->saturated);
    t->saturated |= (v->flags & CERBERUS_WIFI_PACKET_COUNT_SATURATED) != 0;
    if (t->segments < UINT32_MAX) ++t->segments; else t->saturated = true;
    s->committed = true;
}
void wifi_packet_count_scan_init(wifi_packet_count_scan_t *s)
{ if (s) memset(s, 0, sizeof(*s)); }
bool wifi_packet_count_scan_released(const wifi_packet_count_scan_t *s)
{
    return !s || (!s->requested && !s->retuning && source_released(&s->sources[0]) &&
        source_released(&s->sources[1]));
}
esp_err_t wifi_packet_count_scan_add(wifi_packet_count_scan_t *s,
    const wifi_packet_count_selection_t *v)
{
    if (!s || !v || !cerberus_wifi_packet_count_target_valid(&v->target) ||
        !memchr(v->label, 0, sizeof(v->label))) return ESP_ERR_INVALID_ARG;
    if (!wifi_packet_count_scan_released(s) || s->segments) return ESP_ERR_INVALID_STATE;
    for (unsigned i = 0; i < s->count; ++i)
        if (target_same(&s->selected[i].target, &v->target)) return ESP_OK;
    if (s->count == CERBERUS_WIFI_PACKET_COUNT_LIMIT) return ESP_ERR_NO_MEM;
    s->selected[s->count++] = *v;
    return ESP_OK;
}
esp_err_t wifi_packet_count_scan_remove(wifi_packet_count_scan_t *s, unsigned i)
{
    if (!s || i >= s->count) return ESP_ERR_INVALID_ARG;
    if (!wifi_packet_count_scan_released(s) || s->segments) return ESP_ERR_INVALID_STATE;
    --s->count;
    memmove(s->selected + i, s->selected + i + 1U, (s->count - i) * sizeof(s->selected[0]));
    memset(&s->selected[s->count], 0, sizeof(s->selected[0]));
    return ESP_OK;
}
uint64_t wifi_packet_count_scan_plan(const wifi_packet_count_scan_t *s, unsigned mode, uint8_t channel)
{
    if (!s || !s->count) return 0;
    if (mode == WIFI_PACKET_COUNT_FIXED) return cerberus_wifi_packet_count_channel_bit(channel);
    if (mode == WIFI_PACKET_COUNT_ALL) return (UINT64_C(1) << CERBERUS_WIFI_PACKET_COUNT_CHANNELS) - 1U;
    if (mode != WIFI_PACKET_COUNT_SELECTED) return 0;
    uint64_t mask = 0;
    for (unsigned i = 0; i < s->count; ++i)
        mask |= cerberus_wifi_packet_count_channel_bit(s->selected[i].target.channel_hint);
    return mask;
}
static void arm_source(wifi_packet_count_scan_t *s, unsigned source, uint64_t mask)
{
    wifi_packet_count_source_t *v = &s->sources[source];
    commit(v);
    /* Keep only cumulative totals from earlier exact frozen segments. */
    memset(&v->published, 0, offsetof(wifi_packet_count_source_t, completed));
    memset(&v->before, 0, sizeof(*v) - offsetof(wifi_packet_count_source_t, before));
    v->request.channel_mask = mask; v->request.target_count = s->count;
    v->request.dwell_ms = s->dwell_ms; v->request.record = s->recording;
    v->phase = mask ? WIFI_COUNT_INFO : WIFI_COUNT_IDLE;
}
static void arm(wifi_packet_count_scan_t *s, uint64_t mask)
{
    unsigned total = 0, low = 0;
    for (unsigned i = 0; i < CERBERUS_WIFI_PACKET_COUNT_CHANNELS; ++i) if (mask & (UINT64_C(1) << i)) {
        ++total; if (cerberus_wifi_packet_count_channel(i) <= 14U) ++low;
    }
    unsigned wanted = total > 1U ? (total + 1U) / 2U : 0U;
    if (wanted > low) wanted = low;
    uint64_t local = 0;
    for (unsigned i = 0; i < 14U && wanted; ++i) if (mask & (UINT64_C(1) << i)) {
        local |= UINT64_C(1) << i; --wanted;
    }
    arm_source(s, 0, local); arm_source(s, 1, mask & ~local);
    s->requested_channels = mask; s->requested = true; s->frozen = false;
    s->closing = s->fallback = false; s->s3_admission_error = ESP_OK;
    s->error = ESP_OK; s->next_source = 0;
    if (s->segments != UINT32_MAX) ++s->segments;
}
esp_err_t wifi_packet_count_scan_begin(wifi_packet_count_scan_t *s,
    uint64_t mask, uint16_t dwell, bool recording)
{
    if (!s || !s->count || !cerberus_wifi_packet_count_mask_valid(mask) ||
        dwell < CERBERUS_WIFI_PACKET_COUNT_DWELL_MIN_MS ||
        dwell > CERBERUS_WIFI_PACKET_COUNT_DWELL_MAX_MS) return ESP_ERR_INVALID_ARG;
    if (!wifi_packet_count_scan_released(s) || s->segments == UINT32_MAX) return ESP_ERR_INVALID_STATE;
    for (unsigned i = 0; i < 2U; ++i)
        if (s->sources[i].start_attempted && !s->sources[i].final_valid) return ESP_ERR_INVALID_STATE;
    s->dwell_ms = dwell; s->recording = recording; arm(s, mask);
    return ESP_OK;
}
void wifi_packet_count_scan_stop(wifi_packet_count_scan_t *s)
{
    if (!s) return;
    s->requested = s->retuning = false;
    for (unsigned i = 0; i < 2U; ++i) {
        wifi_packet_count_source_t *v = &s->sources[i]; v->stop = true; v->next_action_ms = 0;
        if (!v->attempted) { v->phase = WIFI_COUNT_IDLE; v->request.session_id = 0; }
    }
}
void wifi_packet_count_scan_close(wifi_packet_count_scan_t *s)
{
    if (!s) return;
    wifi_packet_count_scan_stop(s); s->closing = true;
    for (unsigned i = 0; i < 2U; ++i) s->sources[i].close = true;
}
esp_err_t wifi_packet_count_scan_retune(wifi_packet_count_scan_t *s, uint64_t mask)
{
    if (!s || !cerberus_wifi_packet_count_mask_valid(mask)) return ESP_ERR_INVALID_ARG;
    if (!s->requested || s->retuning || s->closing || s->segments == UINT32_MAX)
        return ESP_ERR_INVALID_STATE;
    wifi_packet_count_scan_stop(s); s->pending_channels = mask; s->retuning = true;
    return ESP_OK;
}
static void fault(wifi_packet_count_source_t *v, esp_err_t error, uint64_t now)
{
    v->error = error; v->faulted = v->stop = true; v->final_valid = false;
    v->published.frozen = false; v->next_action_ms = later(now, WIFI_PACKET_COUNT_RETRY_MS);
}
static bool control_valid(const cerberus_packet_count_control_t *r, uint8_t cmd, uint64_t session)
{
    if (!r || (cmd != CERBERUS_PACKET_COUNT_COMMAND_INFO && r->session_id != session)) return false;
    if (!(r->flags & CERBERUS_PACKET_COUNT_BOUND)) {
        if (cmd == CERBERUS_PACKET_COUNT_COMMAND_INFO)
            return !r->sequence && !r->configured && !r->count && !r->channel &&
                ((!r->flags && r->state == CERBERUS_PACKET_COUNT_IDLE) ||
                 (r->flags == CERBERUS_PACKET_COUNT_HELD && r->state == CERBERUS_PACKET_COUNT_QUEUED));
        return !r->sequence && !r->configured && !r->state && !r->flags && !r->channel &&
            !r->count && (r->error_code || cmd == CERBERUS_PACKET_COUNT_COMMAND_RELEASE);
    }
    if (r->state < CERBERUS_PACKET_COUNT_STAGING || r->state > CERBERUS_PACKET_COUNT_ERROR ||
        r->count > CERBERUS_PACKET_COUNT_TARGETS || (r->configured & ~configured(r->count)) ||
        (r->channel && !cerberus_wifi_packet_count_channel_bit(r->channel)) ||
        ((r->flags & CERBERUS_PACKET_COUNT_RX_ACTIVE) &&
            (!(r->flags & CERBERUS_PACKET_COUNT_HELD) || r->state != CERBERUS_PACKET_COUNT_RUNNING || !r->channel))) return false;
    return cmd != CERBERUS_PACKET_COUNT_COMMAND_PEEK ||
        ((r->sequence > CERBERUS_PACKET_COUNT_PIN_BASE && (r->sequence & 1U)) || (!r->sequence && r->error_code));
}
static bool accept(wifi_packet_count_source_t *v, const cerberus_packet_count_control_t *r,
    uint8_t cmd, uint64_t now)
{
    if (!control_valid(r, cmd, v->request.session_id)) { fault(v, ESP_ERR_INVALID_RESPONSE, now); return false; }
    if (!(r->flags & CERBERUS_PACKET_COUNT_BOUND)) {
        bool release = cmd == CERBERUS_PACKET_COUNT_COMMAND_RELEASE && !r->error_code;
        v->clean_rejection = !v->accepted && !release;
        v->published.frozen = release && v->final_valid && !v->faulted && !v->close;
        if (!release) { v->error = r->error_code; v->final_valid = false; }
        v->request.session_id = 0; v->phase = WIFI_COUNT_IDLE; v->have_control = false;
        return false;
    }
    if (r->count != v->request.target_count ||
        (r->channel && !(v->request.channel_mask & cerberus_wifi_packet_count_channel_bit(r->channel))) ||
        (cmd != CERBERUS_PACKET_COUNT_COMMAND_PEEK && r->sequence < v->revision_floor)) {
        fault(v, ESP_ERR_INVALID_RESPONSE, now); return false;
    }
    v->accepted = v->have_control = true; v->session = r->session_id; v->remote = *r;
    if (cmd != CERBERUS_PACKET_COUNT_COMMAND_PEEK) v->revision_floor = r->sequence;
    if (cmd == CERBERUS_PACKET_COUNT_COMMAND_STATUS || cmd == CERBERUS_PACKET_COUNT_COMMAND_BEGIN ||
        cmd == CERBERUS_PACKET_COUNT_COMMAND_START) v->last_status_ms = now;
    if (cmd == CERBERUS_PACKET_COUNT_COMMAND_STATUS) v->progress_due = true;
    if (r->flags&CERBERUS_PACKET_COUNT_CLOCK_INVALID) {
        v->clock_invalid=true;
        v->published.row_anchor_valid=v->staging.row_anchor_valid=0;
    }
    if (!v->faulted) v->error = r->error_code;
    return true;
}
static esp_err_t exchange(const wifi_packet_count_transport_t *t, unsigned source, uint8_t cmd,
    const void *q, size_t qn, void *r, size_t rn)
{ memset(r, 0, rn); return t->exchange(t->context, source, cmd, q, qn, r, rn); }
static unsigned cost(unsigned source, uint8_t command)
{
    return source == WIFI_PACKET_COUNT_SOURCE_C5 &&
        (command == CERBERUS_PACKET_COUNT_COMMAND_INFO || command == CERBERUS_PACKET_COUNT_COMMAND_BEGIN) ? 2U : 1U;
}
static bool summary_decode(wifi_packet_count_source_t *v, const uint8_t *p)
{
    wifi_packet_count_snapshot_t *s = &v->staging;
    cerberus_wifi_packet_count_summary_t *r = &s->summary;
    if (p[0] != 1U || p[1] < CERBERUS_PACKET_COUNT_QUEUED || p[1] > CERBERUS_PACKET_COUNT_ERROR ||
        p[3] != v->request.record || p[8] != v->request.target_count || p[9] > 2U ||
        get(p + 10, 2) || get(p + 126, 2) || get(p + 124, 2) != configured(p[8])) return false;
    memset(r, 0, sizeof(*r));
    r->flags = (uint16_t)get(p + 4, 2); r->dwell_ms = (uint16_t)get(p + 6, 2); r->count = p[8];
    r->revision = (uint32_t)get(p + 16, 4);
    if (r->flags & ~63U || r->dwell_ms != v->request.dwell_ms || r->revision < v->projection_floor) return false;
    r->callbacks = (uint32_t)get(p + 20, 4); r->invalid_metadata = (uint32_t)get(p + 24, 4);
    r->malformed_frames = (uint32_t)get(p + 28, 4); r->unsupported_frames = (uint32_t)get(p + 32, 4);
    r->clock_dropped_frames = (uint32_t)get(p + 36, 4); r->outside_window_frames = (uint32_t)get(p + 40, 4);
    r->valid_frames = (uint32_t)get(p + 44, 4); r->matched_frames = (uint32_t)get(p + 48, 4);
    r->channel_failures = (uint32_t)get(p + 52, 4);
    r->started_source_us = get(p + 56, 8); r->stopped_source_us = get(p + 64, 8);
    r->copied_source_us = get(p + 72, 8); r->requested_channels = get(p + 80, 8);
    r->attempted_channels = get(p + 88, 8); r->successful_channels = get(p + 96, 8);
    if (r->requested_channels != v->request.channel_mask ||
        (r->attempted_channels & ~r->requested_channels) ||
        (r->successful_channels & ~r->attempted_channels) || r->matched_frames > r->valid_frames ||
        r->valid_frames > r->callbacks ||
        (!(r->flags & CERBERUS_WIFI_PACKET_COUNT_STARTED_TIME) && r->started_source_us) ||
        (!(r->flags & CERBERUS_WIFI_PACKET_COUNT_STOPPED_TIME) && r->stopped_source_us) ||
        (!(r->flags & CERBERUS_WIFI_PACKET_COUNT_COPIED_TIME) && r->copied_source_us) ||
        ((r->flags & 5U) == 5U && r->copied_source_us < r->started_source_us) ||
        ((r->flags & 6U) == 6U && r->copied_source_us < r->stopped_source_us)) return false;
    uint64_t partition=(uint64_t)r->invalid_metadata+r->malformed_frames+r->unsupported_frames+
        r->clock_dropped_frames+r->outside_window_frames+r->valid_frames;
    if (!(r->flags&CERBERUS_WIFI_PACKET_COUNT_SATURATED) && partition!=r->callbacks) return false;
    if ((r->flags&CERBERUS_WIFI_PACKET_COUNT_STOPPED) && !terminal(p[1])) return false;
    if (terminal(p[1]) && (p[2] || !(r->flags&CERBERUS_WIFI_PACKET_COUNT_STOPPED))) return false;
    s->state = p[1]; s->channel = p[2]; s->source_error = (int32_t)get(p + 12, 4);
    s->recording = (cerberus_packet_count_record_stats_t){.accepted=(uint32_t)get(p + 104,4),
        .acked=(uint32_t)get(p + 108,4), .dropped_full=(uint32_t)get(p + 112,4),
        .dropped_other=(uint32_t)get(p + 116,4), .last_sequence=(uint32_t)get(p + 120,4), .pending=p[9]};
    if (!v->request.record && (s->recording.accepted || s->recording.acked || s->recording.dropped_full ||
        s->recording.dropped_other || s->recording.last_sequence || s->recording.pending)) return false;
    return !s->channel || (v->request.channel_mask & cerberus_wifi_packet_count_channel_bit(s->channel));
}
static bool row_decode(wifi_packet_count_scan_t *scan, wifi_packet_count_source_t *v)
{
    const uint8_t *p = v->scratch;
    cerberus_wifi_packet_count_row_t *r = &v->staging.rows[v->index];
    memset(r,0,sizeof(*r)); memcpy(&r->target,p,8);
    if (memcmp(&r->target,&scan->selected[v->index].target,8) || p[71] || get(p+94,2) ||
        (p[70]&~15U) || (get(p+92,2)&~63U) || get(p+88,4)<v->before.revision || get(p+88,4)<v->projection_floor) return false;
    r->observations=(uint32_t)get(p+8,4); r->transmitted=(uint32_t)get(p+12,4);
    r->addressed=(uint32_t)get(p+16,4); r->bssid_context=(uint32_t)get(p+20,4);
    r->management=(uint32_t)get(p+24,4); r->data=(uint32_t)get(p+28,4);
    r->retry=(uint32_t)get(p+32,4); r->protected_frames=(uint32_t)get(p+36,4); r->fragmented=(uint32_t)get(p+40,4);
    r->first_us=get(p+44,8); r->last_us=get(p+52,8); r->last_tx_us=get(p+60,8);
    r->rssi=(int8_t)p[68]; r->last_channel=p[69]; r->flags=p[70];
    uint64_t copied=get(p+72,8); uint16_t flags=(uint16_t)get(p+92,2);
    if ((!(r->flags&CERBERUS_WIFI_PACKET_COUNT_ROW_SATURATED) &&
            ((uint64_t)r->management+r->data != r->observations ||
             (uint64_t)r->transmitted+r->addressed < r->observations)) ||
        (!!r->observations != !!(r->flags&CERBERUS_WIFI_PACKET_COUNT_ROW_TIME)) ||
        (!!r->transmitted != !!(r->flags&CERBERUS_WIFI_PACKET_COUNT_ROW_TX_TIME)) ||
        ((r->flags&CERBERUS_WIFI_PACKET_COUNT_ROW_RSSI) && (r->rssi>0 || r->rssi < -127)) ||
        (!!r->observations != !!r->last_channel) ||
        r->transmitted>r->observations || r->addressed>r->observations ||
        r->management>r->observations || r->data>r->observations || r->retry>r->observations ||
        r->protected_frames>r->observations || r->fragmented>r->observations ||
        (!(r->flags&1U)&&(r->first_us||r->last_us)) || (!(r->flags&2U)&&r->last_tx_us) ||
        (!(r->flags&4U)&&r->rssi) || ((r->flags&4U)&&!r->transmitted) ||
        ((r->flags&1U)&&r->first_us>r->last_us) ||
        ((flags&4U)&&((r->flags&1U)&&r->last_us>copied)) ||
        ((flags&4U)&&((r->flags&2U)&&r->last_tx_us>copied)) ||
        (!(flags&4U)&&copied) || (r->last_channel&&!(v->request.channel_mask&cerberus_wifi_packet_count_channel_bit(r->last_channel)))) return false;
    if (v->cycle_final && (get(p+88,4)!=v->before.revision || !(flags&CERBERUS_WIFI_PACKET_COUNT_STOPPED))) return false;
    v->staging.row_copied_us[v->index]=copied; v->staging.row_anchor_us[v->index]=v->read_anchor_us;
    if (!v->clock_invalid && v->anchor_valid && (flags&4U) && !(flags&CERBERUS_WIFI_PACKET_COUNT_CLOCK_INVALID))
        v->staging.row_anchor_valid|=(uint16_t)(1U<<v->index);
    return true;
}
static bool summary_same(const cerberus_wifi_packet_count_summary_t *a,
    const cerberus_wifi_packet_count_summary_t *b)
{
    cerberus_wifi_packet_count_summary_t x=*a,y=*b;
    x.copied_source_us=y.copied_source_us=0;
    return !memcmp(&x,&y,sizeof(x));
}
static void cycle(wifi_packet_count_source_t *v)
{
    memset(&v->staging,0,sizeof(v->staging)); v->staging.session=v->session;
    v->kind=CERBERUS_PACKET_COUNT_READ_SUMMARY; v->index=0; v->after_summary=false;
    v->cycle_final=terminal(v->remote.state)&&!(v->remote.flags&CERBERUS_PACKET_COUNT_HELD);
    v->phase=WIFI_COUNT_PEEK;
}
static bool projection_done(wifi_packet_count_scan_t *s,wifi_packet_count_source_t *v)
{
    if (v->kind==CERBERUS_PACKET_COUNT_READ_SUMMARY) {
        if (!summary_decode(v,v->scratch)) return false;
        if (!v->after_summary) {
            v->before=v->staging.summary;
            if (v->cycle_final&&(!(v->before.flags&CERBERUS_WIFI_PACKET_COUNT_STOPPED)||!terminal(v->staging.state))) return false;
            v->kind=CERBERUS_PACKET_COUNT_READ_ROW; v->index=0;
        } else {
            if (v->cycle_final&&!summary_same(&v->before,&v->staging.summary)) return false;
            if (!v->cycle_final&&terminal(v->staging.state)) { v->phase=WIFI_COUNT_POLL; return true; }
            v->phase=WIFI_COUNT_FINAL_POLL; return true;
        }
    } else if (v->kind==CERBERUS_PACKET_COUNT_READ_ROW) {
        if (!row_decode(s,v)) return false;
        if (++v->index==s->count) { v->kind=CERBERUS_PACKET_COUNT_READ_CHANNEL; v->index=0; }
    } else {
        const uint8_t *p=v->scratch;
        if (p[0]!=cerberus_wifi_packet_count_channel(v->index)||(p[1]&~63U)||get(p+2,2)||
            get(p+4,4)<v->before.revision || get(p+4,4)<v->projection_floor ||
            (v->cycle_final&&(get(p+4,4)!=v->before.revision||!(p[1]&CERBERUS_WIFI_PACKET_COUNT_STOPPED)))) return false;
        v->staging.channel_window_us[v->index]=get(p+8,8); ++v->index;
    }
    if (v->kind==CERBERUS_PACKET_COUNT_READ_CHANNEL) {
        while(v->index<CERBERUS_WIFI_PACKET_COUNT_CHANNELS&&!(v->request.channel_mask&(UINT64_C(1)<<v->index))) ++v->index;
        if (v->index==CERBERUS_WIFI_PACKET_COUNT_CHANNELS) {
            v->kind=CERBERUS_PACKET_COUNT_READ_SUMMARY; v->index=0; v->after_summary=true;
        }
    }
    v->phase=WIFI_COUNT_PEEK; return true;
}
static unsigned step(wifi_packet_count_scan_t *scan,unsigned source,uint64_t now,
    const wifi_packet_count_transport_t *t)
{
    wifi_packet_count_source_t *v=&scan->sources[source];
    if (now<v->next_action_ms) return 0;
    if (v->phase==WIFI_COUNT_IDLE) {
        if (v->record_bound && t->record_drained && t->record_drained(t->context,source,v->session)) v->record_bound=false;
        return 0;
    }
    cerberus_packet_count_control_t r={0}; esp_err_t error;
    cerberus_packet_count_request_t q={v->request.session_id};
    uint8_t cmd=0; const void *request=&q; size_t request_size=sizeof(q);
    cerberus_packet_count_target_t target={0}; cerberus_packet_count_peek_t peek={0};
    if (v->attempted&&v->stop&&(!v->have_control||(v->remote.flags&CERBERUS_PACKET_COUNT_HELD))) cmd=CERBERUS_PACKET_COUNT_COMMAND_STOP;
    else if (v->release_attempted) cmd=CERBERUS_PACKET_COUNT_COMMAND_RELEASE;
    else if (v->attempted&&(!v->have_control||
        (now-v->last_status_ms>=WIFI_PACKET_COUNT_KEEPALIVE_MS&&
         (!v->progress_due||now-v->last_status_ms>=WIFI_PACKET_COUNT_PROGRESS_MAX_MS)))) cmd=CERBERUS_PACKET_COUNT_COMMAND_STATUS;
    else if (v->close&&v->attempted) cmd=CERBERUS_PACKET_COUNT_COMMAND_RELEASE;
    else if (v->faulted) return 0; /* Close explicitly releases an invalid final projection. */
    else switch(v->phase) {
    case WIFI_COUNT_INFO: cmd=CERBERUS_PACKET_COUNT_COMMAND_INFO; request=NULL; request_size=0; break;
    case WIFI_COUNT_BEGIN: cmd=CERBERUS_PACKET_COUNT_COMMAND_BEGIN;request=&v->request;request_size=sizeof(v->request);v->attempted=true;break;
    case WIFI_COUNT_TARGET:
        cmd=CERBERUS_PACKET_COUNT_COMMAND_TARGET;target.session_id=q.session_id;target.index=v->upload_index;
        target.role=scan->selected[target.index].target.role;target.channel_hint=scan->selected[target.index].target.channel_hint;
        memcpy(target.mac,scan->selected[target.index].target.mac,6);request=&target;request_size=sizeof(target);break;
    case WIFI_COUNT_START: cmd=CERBERUS_PACKET_COUNT_COMMAND_START;v->start_attempted=true;break;
    case WIFI_COUNT_POLL: case WIFI_COUNT_FINAL_POLL: cmd=CERBERUS_PACKET_COUNT_COMMAND_STATUS;break;
    case WIFI_COUNT_PEEK:
        cmd=CERBERUS_PACKET_COUNT_COMMAND_PEEK;peek.session_id=q.session_id;peek.kind=v->kind;peek.index=v->index;
        request=&peek;request_size=sizeof(peek);break;
    case WIFI_COUNT_READ: {
        cerberus_packet_count_read_t read={q.session_id,v->pin,v->part};cerberus_packet_count_chunk_t chunk;
        v->progress_due=false;
        error=exchange(t,source,CERBERUS_PACKET_COUNT_COMMAND_READ,&read,sizeof(read),&chunk,sizeof(chunk));
        if(error!=ESP_OK){v->error=error;v->next_action_ms=later(now,WIFI_PACKET_COUNT_RETRY_MS);return 1;}
        if(chunk.session_id!=q.session_id||chunk.pin!=v->pin||chunk.part!=v->part){fault(v,ESP_ERR_INVALID_RESPONSE,now);return 1;}
        unsigned length=v->kind==CERBERUS_PACKET_COUNT_READ_SUMMARY?128U:v->kind==CERBERUS_PACKET_COUNT_READ_ROW?96U:16U;
        memcpy(v->scratch+v->part*8U,chunk.data,8U);
        if(++v->part*8U==length&&!projection_done(scan,v))fault(v,ESP_ERR_INVALID_RESPONSE,now);
        return 1;
    }
    case WIFI_COUNT_RELEASE: cmd=CERBERUS_PACKET_COUNT_COMMAND_RELEASE;break;
    default: fault(v,ESP_ERR_INVALID_STATE,now);return 0;
    }
    if(cmd==CERBERUS_PACKET_COUNT_COMMAND_RELEASE&&v->record_bound) {
        if (!t->record_drained||!t->record_drained(t->context,source,v->session)) {
            v->next_action_ms=later(now,50U);return 0;
        }
        v->record_bound=false;
    }
    if (cmd==CERBERUS_PACKET_COUNT_COMMAND_RELEASE) v->release_attempted=true;
    /* A slow STATUS can consume a whole UI tick. Grant one bounded phase
     * operation after a validated reply, not an extra blocking exchange.
     * Consume the credit even on failure; uncertain BEGIN/START and retry
     * backoff still require their existing status/cleanup paths. */
    v->progress_due=false;
    uint8_t phase=v->phase;
    int64_t anchor=t->clock_us(t->context);
    error=exchange(t,source,cmd,request,request_size,&r,sizeof(r));
    if(error!=ESP_OK){
        v->error=error;v->next_action_ms=later(now,WIFI_PACKET_COUNT_RETRY_MS);
        if(cmd==CERBERUS_PACKET_COUNT_COMMAND_BEGIN||cmd==CERBERUS_PACKET_COUNT_COMMAND_START)v->have_control=false;
        return cost(source,cmd);
    }
    if(cmd==CERBERUS_PACKET_COUNT_COMMAND_INFO){
        if(!control_valid(&r,cmd,0)){fault(v,ESP_ERR_INVALID_RESPONSE,now);return cost(source,cmd);}
        if(r.flags||r.error_code||r.session_id==UINT64_MAX){
            v->error=r.error_code?r.error_code:ESP_ERR_INVALID_STATE;v->clean_rejection=true;v->phase=WIFI_COUNT_IDLE;return cost(source,cmd);
        }
        v->request.session_id=r.session_id+1U;v->phase=WIFI_COUNT_BEGIN;return cost(source,cmd);
    }
    if(!accept(v,&r,cmd,now))return cost(source,cmd);
    if(v->request.record&&!v->record_bound&&!v->close&&!v->stop){
        error=t->record_bind?t->record_bind(t->context,source,v->session):ESP_ERR_NOT_SUPPORTED;
        if(error!=ESP_OK){v->error=error;v->stop=v->close=true;return cost(source,cmd);}
        v->record_bound=true;
    }
    if(cmd==CERBERUS_PACKET_COUNT_COMMAND_STOP){v->phase=WIFI_COUNT_POLL;v->next_action_ms=later(now,250U);return 1;}
    if(cmd==CERBERUS_PACKET_COUNT_COMMAND_RELEASE){v->next_action_ms=later(now,WIFI_PACKET_COUNT_RETRY_MS);return 1;}
    if(cmd==CERBERUS_PACKET_COUNT_COMMAND_PEEK){
        if (!r.sequence) {
            if(r.error_code==ESP_ERR_NOT_FINISHED)v->next_action_ms=later(now,WIFI_PACKET_COUNT_RETRY_MS);
            else fault(v,r.error_code,now);
            return 1;
        }
        /* STATUS can advance the live model while these immutable parts are
         * transferred. This pin is fenced by the floor known when accepted. */
        v->projection_floor=v->revision_floor;
        v->pin=r.sequence;v->part=0;memset(v->scratch,0,sizeof(v->scratch));
        v->read_anchor_us=anchor<0?0U:(uint64_t)anchor;v->anchor_valid=anchor>=0;v->phase=WIFI_COUNT_READ;return 1;
    }
    if(cmd==CERBERUS_PACKET_COUNT_COMMAND_STATUS&&phase!=WIFI_COUNT_POLL&&phase!=WIFI_COUNT_FINAL_POLL){
        /* Lost BEGIN/START replies recover the accepted exact session. */
        if(phase==WIFI_COUNT_BEGIN)v->phase=WIFI_COUNT_TARGET;
        if(phase==WIFI_COUNT_START&&r.state!=CERBERUS_PACKET_COUNT_STAGING)v->phase=WIFI_COUNT_POLL;
        return 1;
    }
    if(cmd==CERBERUS_PACKET_COUNT_COMMAND_BEGIN){
        if(r.error_code || r.state!=CERBERUS_PACKET_COUNT_STAGING){v->stop=true;v->phase=WIFI_COUNT_POLL;return cost(source,cmd);}
        v->phase=WIFI_COUNT_TARGET;return cost(source,cmd);}
    if(cmd==CERBERUS_PACKET_COUNT_COMMAND_TARGET){
        if(r.error_code||!(r.configured&(1U<<v->upload_index))){fault(v,ESP_ERR_INVALID_RESPONSE,now);return 1;}
        if(++v->upload_index==scan->count)v->phase=WIFI_COUNT_START;
        return 1;
    }
    if(cmd==CERBERUS_PACKET_COUNT_COMMAND_START){v->phase=WIFI_COUNT_POLL;return cost(source,cmd);}
    if(phase==WIFI_COUNT_FINAL_POLL){
        if(v->cycle_final&&(r.sequence!=v->staging.summary.revision||!terminal(r.state)||(r.flags&CERBERUS_PACKET_COUNT_HELD))){fault(v,ESP_ERR_INVALID_RESPONSE,now);return 1;}
        if(v->clock_invalid)v->staging.row_anchor_valid=0;
        v->staging.valid=true;v->staging.frozen=false;
        v->published=v->staging;v->final_valid=v->cycle_final;
        v->phase=v->cycle_final?WIFI_COUNT_RELEASE:WIFI_COUNT_POLL;
        v->next_cycle_ms=later(now,WIFI_PACKET_COUNT_REFRESH_MS);return 1;
    }
    if(r.state==CERBERUS_PACKET_COUNT_STAGING){
        if(v->stop){v->next_action_ms=later(now,250U);return 1;}
        v->phase=v->upload_index<scan->count?WIFI_COUNT_TARGET:WIFI_COUNT_START;return 1;
    }
    if(terminal(r.state)&&!(r.flags&(CERBERUS_PACKET_COUNT_HELD|CERBERUS_PACKET_COUNT_DATA_READY)))v->phase=WIFI_COUNT_RELEASE;
    else if((r.flags&CERBERUS_PACKET_COUNT_DATA_READY)&&(!(r.flags&CERBERUS_PACKET_COUNT_HELD)||now>=v->next_cycle_ms))cycle(v);
    else v->next_action_ms=later(now,250U);
    return 1;
}
void wifi_packet_count_scan_tick(wifi_packet_count_scan_t *s,uint64_t now,
    const wifi_packet_count_transport_t *t)
{
    if(!s||!t||!t->exchange||!t->clock_us)return;
    int64_t start=t->clock_us(t->context);
    if(start<0||(s->last_tick_ms&&now<s->last_tick_ms)){
        wifi_packet_count_scan_stop(s);s->error=ESP_ERR_INVALID_STATE;
        for(unsigned i=0;i<2;++i){
            s->sources[i].clock_invalid=true;
            s->sources[i].published.row_anchor_valid=s->sources[i].staging.row_anchor_valid=0;
        }
    }
    s->last_tick_ms=now;
    unsigned calls=0;
    for(unsigned tries=0;tries<8U&&calls<WIFI_PACKET_COUNT_TICK_CALLS;++tries){
        int64_t current=t->clock_us(t->context);
        if(calls&&(current<start||current-start>=(int64_t)WIFI_PACKET_COUNT_TICK_BUDGET_US))break;
        wifi_packet_count_source_t *local=&s->sources[0],*remote=&s->sources[1];
        if(local->clean_rejection&&remote->phase==WIFI_COUNT_INFO&&!remote->attempted&&!s->closing){
            s->s3_admission_error=local->error;s->fallback=true;
            remote->request.channel_mask=s->requested_channels;local->clean_rejection=false;
        }
        unsigned source=s->next_source++%2U;
        /* Admit S3 before C5. Any ambiguity retains the original disjoint plan. */
        if(source==1U&&!remote->attempted&&local->request.channel_mask&&!source_released(local)&&
            (!local->have_control||!local->start_attempted||local->remote.state==CERBERUS_PACKET_COUNT_STAGING))continue;
        /* Reserve the maximum wire cost of a possible capability refresh. */
        if(source==1U&&calls+2U>WIFI_PACKET_COUNT_TICK_CALLS&&
            (s->sources[source].phase==WIFI_COUNT_INFO||s->sources[source].phase==WIFI_COUNT_BEGIN))continue;
        calls+=step(s,source,now,t);
    }
    bool released=source_released(&s->sources[0])&&source_released(&s->sources[1]);
    if(released){
        bool valid=true,any=false;
        for(unsigned i=0;i<2U;++i){
            any|=s->sources[i].published.frozen;
            if(s->sources[i].start_attempted&&!s->sources[i].published.frozen)valid=false;
        }
        valid&=any;
        if(s->retuning&&valid&&!s->closing){
            uint64_t mask=s->pending_channels;s->retuning=false;
            if(s->interruptions<UINT32_MAX)++s->interruptions;
            arm(s,mask);
        }else{ s->requested=false;s->retuning=false;s->frozen=valid&&!s->closing; }
    }
}
bool wifi_packet_count_scan_row(const wifi_packet_count_scan_t *s,unsigned index,
    wifi_packet_count_row_totals_t *out,bool *saturated)
{
    if(!s||!out||!saturated||index>=s->count)return false;
    memset(out,0,sizeof(*out));*saturated=false;
    for(unsigned source=0;source<2U;++source){const wifi_packet_count_source_t *v=&s->sources[source];
        const wifi_packet_count_row_totals_t *r=&v->completed.rows[index];
#define SUM(field) out->field=add(out->field,r->field,saturated)
        SUM(observations);SUM(transmitted);SUM(addressed);SUM(bssid_context);SUM(management);
        SUM(data);SUM(retry);SUM(protected_frames);SUM(fragmented);
#undef SUM
        *saturated|=v->completed.saturated;
        if(v->published.valid&&!v->committed)row_add(out,&v->published.rows[index],saturated);
    }
    return true;
}
bool wifi_packet_count_scan_signal_age(const wifi_packet_count_scan_t *s,unsigned source,
    unsigned index,int64_t now,uint64_t *age)
{
    if(!s||!age||source>=2U||index>=s->count||now<0||s->sources[source].clock_invalid)return false;
    const wifi_packet_count_snapshot_t *v=&s->sources[source].published;
    const cerberus_wifi_packet_count_row_t *r=&v->rows[index];
    if(!v->valid||!(v->row_anchor_valid&(1U<<index))||(r->flags&6U)!=6U||
        (uint64_t)now<v->row_anchor_us[index]||v->row_copied_us[index]<r->last_tx_us)return false;
    uint64_t a=v->row_copied_us[index]-r->last_tx_us,b=(uint64_t)now-v->row_anchor_us[index];
    if(UINT64_MAX-a<b)return false;
    *age=a+b;return true;
}
