#include "mesh_locate.h"
#include <string.h>

static bool channel_valid(uint8_t c) { return c >= 11U && c <= 26U; }
static bool target_valid(const mesh_observation_address_key_t *k)
{
    return k && channel_valid(k->pan.channel) && (k->pan.known || !k->pan.value) &&
        (k->mode == MESH_MAC_ADDRESS_EXT64 ||
         (k->mode == MESH_MAC_ADDRESS_SHORT16 && k->value < UINT64_C(0xfffe)));
}
static bool terminal(uint8_t state)
{ return state == CERBERUS_MESH_LOCATE_COMPLETE || state == CERBERUS_MESH_LOCATE_ERROR; }
static uint64_t later(uint64_t now, uint64_t delay)
{ return UINT64_MAX - now < delay ? UINT64_MAX : now + delay; }
static uint64_t little(const uint8_t *p, size_t n)
{
    uint64_t v=0U;
    for (size_t i=0U;i<n;++i) v|=(uint64_t)p[i]<<(8U*i);
    return v;
}
static bool overlap(const void *a,size_t an,const void *b,size_t bn)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return x<=y ? y-x<an : x-y<bn;
}
static void invalidate_age(mesh_locate_t *s)
{ s->anchor_valid=false; s->snapshot_anchor_us=s->snapshot_age_us=0U; }

bool mesh_locate_control_valid(const cerberus_mesh_locate_control_t *r,
                               uint8_t command,uint64_t session)
{
    if (!r || command<CERBERUS_MESH_LOCATE_COMMAND_INFO || command>CERBERUS_MESH_LOCATE_COMMAND_RELEASE ||
        command==CERBERUS_MESH_LOCATE_COMMAND_READ || !r->session_id || r->reserved ||
        (command!=CERBERUS_MESH_LOCATE_COMMAND_INFO && r->session_id!=session)) return false;
    if (!(r->flags&CERBERUS_MESH_LOCATE_BOUND)) {
        if (command==CERBERUS_MESH_LOCATE_COMMAND_INFO)
            return !r->sequence && !r->error_code && !r->channel && r->version==1U &&
                r->projection_bytes==CERBERUS_MESH_LOCATE_HEADER_BYTES &&
                ((!r->flags && r->state==CERBERUS_MESH_LOCATE_IDLE) ||
                 (r->flags==CERBERUS_MESH_LOCATE_HELD && r->state==CERBERUS_MESH_LOCATE_QUEUED));
        return !r->sequence && !r->state && !r->flags && !r->channel && !r->version &&
            !r->projection_bytes && (r->error_code || command==CERBERUS_MESH_LOCATE_COMMAND_RELEASE);
    }
    if (!(r->flags&CERBERUS_MESH_LOCATE_HELD) || (r->flags&~0x7fU) || r->version!=1U ||
        r->state<CERBERUS_MESH_LOCATE_QUEUED || r->state>CERBERUS_MESH_LOCATE_ERROR ||
        r->projection_bytes<CERBERUS_MESH_LOCATE_HEADER_BYTES ||
        r->projection_bytes>CERBERUS_MESH_LOCATE_PROJECTION_BYTES ||
        (r->channel && !channel_valid(r->channel)) || (terminal(r->state) && r->channel) ||
        ((r->flags&CERBERUS_MESH_LOCATE_RX_ACTIVE) &&
         (r->state!=CERBERUS_MESH_LOCATE_RECEIVING || !r->channel))) return false;
    if (command==CERBERUS_MESH_LOCATE_COMMAND_PEEK)
        return r->sequence>CERBERUS_MESH_LOCATE_PIN_BASE && (r->flags&CERBERUS_MESH_LOCATE_DATA_READY);
    return r->sequence<=CERBERUS_MESH_LOCATE_SEQUENCE_MAX;
}

void mesh_locate_init(mesh_locate_t *s) { if (s) memset(s,0,sizeof(*s)); }
bool mesh_locate_released(const mesh_locate_t *s)
{ return !s || (!s->request.session_id && !s->requested && s->phase==MESH_LOCATE_IDLE); }
esp_err_t mesh_locate_begin(mesh_locate_t *s,const mesh_observation_address_key_t *key,uint64_t origin)
{
    if (!s || !target_valid(key) || !origin) return ESP_ERR_INVALID_ARG;
    if (!mesh_locate_released(s)) return ESP_ERR_INVALID_STATE;
    mesh_observation_address_key_t saved=*key;
    memset(s,0,sizeof(*s)); s->target=saved; s->origin_session=origin;
    s->request=(cerberus_mesh_locate_start_t){.address=saved.value,.pan=saved.pan.value,
        .channel=saved.pan.channel,.address_mode=saved.mode,.pan_known=saved.pan.known,.version=1U};
    s->requested=true; s->phase=MESH_LOCATE_INFO;
    return ESP_OK;
}
void mesh_locate_stop(mesh_locate_t *s)
{
    if (!s || mesh_locate_released(s)) return;
    if (!s->stop_requested) {
        s->pin=0U; s->part=0U;
        if (s->start_attempted && s->phase!=MESH_LOCATE_RELEASE) s->phase=MESH_LOCATE_POLL;
    }
    s->requested=false; s->stop_requested=true; s->next_action_ms=0U;
    if (!s->start_attempted) {
        s->request.session_id=0U; s->phase=MESH_LOCATE_IDLE; s->stop_requested=false;
    }
}
void mesh_locate_close(mesh_locate_t *s)
{
    if (!s || mesh_locate_released(s)) return;
    mesh_locate_stop(s); s->close_requested=true; s->final_valid=false; s->frozen=false;
}
static void fault(mesh_locate_t *s,esp_err_t error,uint64_t now)
{
    s->faulted=true; s->fault_error=s->error=error; s->final_valid=s->frozen=false;
    mesh_locate_stop(s); s->next_action_ms=later(now,MESH_LOCATE_RETRY_MS);
}
static void retry(mesh_locate_t *s,esp_err_t error,uint64_t now)
{ s->error=s->faulted?s->fault_error:error; s->next_action_ms=later(now,MESH_LOCATE_RETRY_MS); }
static void retire(mesh_locate_t *s,const cerberus_mesh_locate_control_t *r,bool released)
{
    s->frozen=released && s->final_valid && !s->faulted && !s->close_requested;
    s->error=s->faulted?s->fault_error:s->frozen?s->snapshot.error_code:r->error_code;
    s->request.session_id=0U; s->requested=s->stop_requested=s->close_requested=false;
    s->accepted=s->start_attempted=s->have_control=false; s->remote=*r;
    s->phase=MESH_LOCATE_IDLE; s->pin=0U; s->part=0U; s->next_action_ms=0U;
}
static bool accept(mesh_locate_t *s,const cerberus_mesh_locate_control_t *r,uint8_t cmd,uint64_t now)
{
    if (!mesh_locate_control_valid(r,cmd,s->request.session_id)) {
        fault(s,ESP_ERR_INVALID_RESPONSE,now); return false;
    }
    if (!(r->flags&CERBERUS_MESH_LOCATE_BOUND)) {
        retire(s,r,cmd==CERBERUS_MESH_LOCATE_COMMAND_RELEASE && r->error_code==ESP_OK); return false;
    }
    if ((r->channel && r->channel!=s->target.pan.channel) ||
        (cmd!=CERBERUS_MESH_LOCATE_COMMAND_PEEK && r->sequence<s->last_sequence)) {
        fault(s,ESP_ERR_INVALID_RESPONSE,now); return false;
    }
    if (!s->accepted) { s->accepted=true; s->session=s->request.session_id; }
    if (cmd!=CERBERUS_MESH_LOCATE_COMMAND_PEEK) s->last_sequence=r->sequence;
    s->remote=*r; s->have_control=true;
    if (r->flags&CERBERUS_MESH_LOCATE_CLOCK_INVALID) invalidate_age(s);
    if (cmd==CERBERUS_MESH_LOCATE_COMMAND_STATUS || cmd==CERBERUS_MESH_LOCATE_COMMAND_START)
        s->last_status_ms=now;
    s->error=s->faulted?s->fault_error:r->error_code;
    if (terminal(r->state)) s->requested=false;
    return true;
}
static esp_err_t control(mesh_locate_t *s,const mesh_scan_transport_t *t,uint8_t cmd,
                         cerberus_mesh_locate_control_t *r)
{
    cerberus_mesh_locate_request_t req={.session_id=s->request.session_id};
    const void *p=&req; size_t n=sizeof(req);
    if (cmd==CERBERUS_MESH_LOCATE_COMMAND_INFO) { p=NULL; n=0U; }
    else if (cmd==CERBERUS_MESH_LOCATE_COMMAND_START) { p=&s->request; n=sizeof(s->request); }
    memset(r,0,sizeof(*r)); return t->exchange(t->context,cmd,p,n,r,sizeof(*r));
}
static uint8_t control_flags(const cerberus_mesh_locate_projection_t *p)
{
    uint8_t f=0U;
    if (p->flags&CERBERUS_MESH_LOCATE_PROJECTION_CLOCK_INVALID) f|=CERBERUS_MESH_LOCATE_CLOCK_INVALID;
    if (p->flags&CERBERUS_MESH_LOCATE_COUNTERS_SATURATED) f|=CERBERUS_MESH_LOCATE_SATURATED;
    if (p->flags&CERBERUS_MESH_LOCATE_SEQUENCE_EXHAUSTED) f|=CERBERUS_MESH_LOCATE_EXHAUSTED;
    return f;
}
static bool projection_valid(const cerberus_mesh_locate_projection_t *p,const mesh_observation_address_key_t *key)
{
    if (!p || !target_valid(key) || p->version!=1U || p->reserved0 || p->reserved1 ||
        p->state<CERBERUS_MESH_LOCATE_QUEUED || p->state>CERBERUS_MESH_LOCATE_ERROR ||
        p->pan_known>1U || p->address_mode!=key->mode || p->address!=key->value ||
        p->pan_known!=key->pan.known || p->pan!=key->pan.value ||
        ((p->flags&CERBERUS_MESH_LOCATE_STOPPED)!=0U)!=terminal(p->state) ||
        (terminal(p->state)?p->channel!=0U:(p->channel && p->channel!=key->pan.channel)) ||
        (p->state==CERBERUS_MESH_LOCATE_RECEIVING && p->channel!=key->pan.channel) ||
        p->match_sequence>CERBERUS_MESH_LOCATE_SEQUENCE_MAX || p->match_sequence!=p->matches ||
        p->mac_length>CERBERUS_MESH_LOCATE_MAC_BYTES) return false;
    bool copy=(p->flags&CERBERUS_MESH_LOCATE_COPY_TIME_VALID)!=0U;
    bool match=(p->flags&CERBERUS_MESH_LOCATE_MATCH_TIME_VALID)!=0U;
    if ((!copy && p->copied_us) || (!match && p->last_match_us) ||
        (!(p->flags&CERBERUS_MESH_LOCATE_RSSI_VALID) && p->rssi) ||
        (!(p->flags&CERBERUS_MESH_LOCATE_LQI_VALID) && p->lqi) ||
        (copy && match && p->last_match_us>p->copied_us)) return false;
    uint64_t total=(uint64_t)p->invalid_metadata+p->malformed+p->unsupported+p->filtered_nonmatch+p->matches;
    bool sat=p->callbacks==UINT32_MAX || p->invalid_metadata==UINT32_MAX ||
        p->malformed==UINT32_MAX || p->unsupported==UINT32_MAX || p->filtered_nonmatch==UINT32_MAX;
    if (p->callbacks!=(total>UINT32_MAX?UINT32_MAX:(uint32_t)total) ||
        sat!=((p->flags&CERBERUS_MESH_LOCATE_COUNTERS_SATURATED)!=0U) ||
        (p->match_sequence==CERBERUS_MESH_LOCATE_SEQUENCE_MAX &&
         !(p->flags&CERBERUS_MESH_LOCATE_SEQUENCE_EXHAUSTED))) return false;
    for (size_t i=p->mac_length;i<sizeof(p->mac);++i) if (p->mac[i]) return false;
    if (!p->match_sequence)
        return !p->mac_length && !p->last_match_us && !p->rssi && !p->lqi &&
            !(p->flags&(CERBERUS_MESH_LOCATE_MATCH_TIME_VALID|CERBERUS_MESH_LOCATE_RSSI_VALID|CERBERUS_MESH_LOCATE_LQI_VALID));
    mesh_mac_observation_t parsed;
    return p->mac_length>=2U && mesh_mac_parse(p->mac,p->mac_length,&parsed)==MESH_MAC_HEADER_OK &&
        parsed.source.mode==key->mode && parsed.source.value==key->value &&
        !parsed.source.broadcast && !parsed.source.short_sentinel &&
        (parsed.source_pan.origin!=MESH_MAC_PAN_UNKNOWN)==key->pan.known &&
        parsed.source_pan.value==key->pan.value;
}
static void decode(const uint8_t *p,cerberus_mesh_locate_projection_t *r)
{
    memset(r,0,sizeof(*r));
    r->version=p[0]; r->flags=p[1]; r->state=p[2]; r->channel=p[3];
    r->address_mode=p[4]; r->pan_known=p[5]; r->mac_length=p[6]; r->reserved0=p[7];
    r->pan=(uint16_t)little(p+8U,2U); r->rssi=(int8_t)p[10]; r->lqi=p[11];
    r->match_sequence=(uint32_t)little(p+12U,4U); r->address=little(p+16U,8U);
    r->copied_us=little(p+24U,8U); r->last_match_us=little(p+32U,8U);
    r->callbacks=(uint32_t)little(p+40U,4U); r->invalid_metadata=(uint32_t)little(p+44U,4U);
    r->malformed=(uint32_t)little(p+48U,4U); r->unsupported=(uint32_t)little(p+52U,4U);
    r->filtered_nonmatch=(uint32_t)little(p+56U,4U); r->matches=(uint32_t)little(p+60U,4U);
    r->error_code=(int32_t)(uint32_t)little(p+64U,4U); r->reserved1=(uint32_t)little(p+68U,4U);
    memcpy(r->mac,p+72U,sizeof(r->mac));
}
static bool same_sample(const cerberus_mesh_locate_projection_t *a,const cerberus_mesh_locate_projection_t *b)
{
    const uint8_t f=CERBERUS_MESH_LOCATE_MATCH_TIME_VALID|CERBERUS_MESH_LOCATE_RSSI_VALID|CERBERUS_MESH_LOCATE_LQI_VALID;
    return a->match_sequence==b->match_sequence && a->mac_length==b->mac_length &&
        a->last_match_us==b->last_match_us && a->rssi==b->rssi && a->lqi==b->lqi &&
        (a->flags&f)==(b->flags&f) && !memcmp(a->mac,b->mac,sizeof(a->mac));
}
static bool projection_control(const cerberus_mesh_locate_projection_t *p,const cerberus_mesh_locate_control_t *r)
{
    return r->state==p->state && r->channel==p->channel && r->error_code==p->error_code &&
        r->projection_bytes==72U+p->mac_length && (r->flags&CERBERUS_MESH_LOCATE_DATA_READY) &&
        (r->flags&(CERBERUS_MESH_LOCATE_CLOCK_INVALID|CERBERUS_MESH_LOCATE_SATURATED|CERBERUS_MESH_LOCATE_EXHAUSTED))==control_flags(p);
}
static bool publish(mesh_locate_t *s)
{
    cerberus_mesh_locate_projection_t p;
    decode(s->scratch,&p);
    if (!projection_valid(&p,&s->target) || !projection_control(&p,&s->pinned_control) ||
        s->wire_size!=72U+p.mac_length || p.match_sequence<s->pinned_sequence_floor) return false;
    const cerberus_mesh_locate_projection_t *old=&s->snapshot;
    bool duplicate=s->has_snapshot && p.match_sequence==old->match_sequence;
    if (s->has_snapshot && (p.match_sequence<old->match_sequence || p.callbacks<old->callbacks ||
        p.invalid_metadata<old->invalid_metadata || p.malformed<old->malformed ||
        p.unsupported<old->unsupported || p.filtered_nonmatch<old->filtered_nonmatch ||
        (duplicate && !same_sample(&p,old)) ||
        ((p.flags&old->flags&CERBERUS_MESH_LOCATE_COPY_TIME_VALID) && p.copied_us<old->copied_us) ||
        ((p.flags&old->flags&CERBERUS_MESH_LOCATE_MATCH_TIME_VALID) && p.last_match_us<old->last_match_us))) return false;
    bool age=s->pending_anchor_valid && !s->clock_failed &&
        (p.flags&(CERBERUS_MESH_LOCATE_COPY_TIME_VALID|CERBERUS_MESH_LOCATE_MATCH_TIME_VALID))==
            (CERBERUS_MESH_LOCATE_COPY_TIME_VALID|CERBERUS_MESH_LOCATE_MATCH_TIME_VALID) &&
        !(p.flags&CERBERUS_MESH_LOCATE_PROJECTION_CLOCK_INVALID);
    uint64_t bound=age?p.copied_us-p.last_match_us:0U;
    if (age && duplicate && s->anchor_valid) {
        if (s->pending_anchor_us<s->snapshot_anchor_us ||
            UINT64_MAX-s->snapshot_age_us<s->pending_anchor_us-s->snapshot_anchor_us) age=false;
        else {
            uint64_t prior=s->snapshot_age_us+s->pending_anchor_us-s->snapshot_anchor_us;
            if (bound<prior) bound=prior;
        }
    }
    if (p.match_sequence && !duplicate) {
        uint32_t previous=s->has_snapshot?old->match_sequence:0U;
        s->coalesced_matches+=p.match_sequence-previous-1U;
        mesh_locate_sample_t sample={.callback_us=p.last_match_us,.sequence=p.match_sequence,
            .rssi=p.rssi,.lqi=p.lqi,.flags=(uint8_t)(p.flags&14U)};
        s->history[s->history_head]=sample;
        s->history_head=(uint8_t)((s->history_head+1U)%MESH_LOCATE_HISTORY);
        if (s->history_count<MESH_LOCATE_HISTORY) ++s->history_count;
        ++s->samples;
    }
    s->snapshot=p; s->has_snapshot=true; s->anchor_valid=age;
    s->snapshot_age_us=age?bound:0U; s->snapshot_anchor_us=age?s->pending_anchor_us:0U;
    s->error=p.error_code;
    return true;
}
static esp_err_t read_part(mesh_locate_t *s,const mesh_scan_transport_t *t,bool *done)
{
    cerberus_mesh_locate_read_t q={.session_id=s->request.session_id,.pin=s->pin,.part=s->part};
    cerberus_mesh_locate_chunk_t r={0}; *done=false;
    esp_err_t error=t->exchange(t->context,CERBERUS_MESH_LOCATE_COMMAND_READ,&q,sizeof(q),&r,sizeof(r));
    if (error!=ESP_OK) return error;
    if (r.session_id!=q.session_id || r.part!=q.part) return ESP_ERR_INVALID_RESPONSE;
    if (!r.pin) {
        for (size_t i=4U;i<sizeof(r.data);++i) if (r.data[i]) return ESP_ERR_INVALID_RESPONSE;
        error=(int32_t)(uint32_t)little(r.data,4U); return error?error:ESP_ERR_INVALID_RESPONSE;
    }
    size_t offset=(size_t)s->part*9U;
    if (r.pin!=q.pin || offset>=s->wire_size) return ESP_ERR_INVALID_RESPONSE;
    size_t n=s->wire_size-offset; if (n>sizeof(r.data)) n=sizeof(r.data);
    for (size_t i=n;i<sizeof(r.data);++i) if (r.data[i]) return ESP_ERR_INVALID_RESPONSE;
    memcpy(s->scratch+offset,r.data,n); ++s->part; *done=offset+n==s->wire_size;
    return ESP_OK;
}
static bool observe_clock(mesh_locate_t *s,int64_t now)
{
    if (now<0 || (s->clock_seen && (uint64_t)now<s->last_clock_us)) {
        s->clock_failed=true; invalidate_age(s); return false;
    }
    s->last_clock_us=(uint64_t)now; s->clock_seen=true; return !s->clock_failed;
}
void mesh_locate_tick(mesh_locate_t *s,uint64_t now,const mesh_scan_transport_t *t)
{
    if (!s || !t || !t->exchange || !t->clock_us) return;
    int64_t begun=t->clock_us(t->context); (void)observe_clock(s,begun);
    if (now<s->last_tick_ms) { s->clock_failed=true; invalidate_age(s); s->next_action_ms=0U; }
    s->last_tick_ms=now;
    unsigned calls=0U;
    while (calls<MESH_SCAN_TICK_CALLS) {
        int64_t current=t->clock_us(t->context); (void)observe_clock(s,current);
        if ((begun>=0 && current>=begun && (uint64_t)(current-begun)>=MESH_SCAN_TICK_BUDGET_US) ||
            now<s->next_action_ms) return;
        cerberus_mesh_locate_control_t r; esp_err_t error;
        if (s->request.session_id && s->stop_requested && (!s->have_control || !terminal(s->remote.state))) {
            ++calls; error=control(s,t,CERBERUS_MESH_LOCATE_COMMAND_STOP,&r);
            if (error!=ESP_OK) { retry(s,error,now); return; }
            if (!accept(s,&r,CERBERUS_MESH_LOCATE_COMMAND_STOP,now)) return;
            s->pin=0U; s->part=0U; s->phase=MESH_LOCATE_POLL;
            if (!terminal(r.state)) { s->next_action_ms=later(now,MESH_LOCATE_RETRY_MS); return; }
            continue;
        }
        if (s->request.session_id && s->close_requested && s->have_control && terminal(s->remote.state))
            s->phase=MESH_LOCATE_RELEASE;
        if (s->request.session_id && s->start_attempted && s->phase!=MESH_LOCATE_RELEASE &&
            (!s->accepted || now<s->last_status_ms || now-s->last_status_ms>=MESH_LOCATE_KEEPALIVE_MS)) {
            ++calls; error=control(s,t,CERBERUS_MESH_LOCATE_COMMAND_STATUS,&r);
            if (error!=ESP_OK) { retry(s,error,now); return; }
            if (!accept(s,&r,CERBERUS_MESH_LOCATE_COMMAND_STATUS,now)) return;
            continue;
        }
        if (s->faulted && !s->close_requested) return;
        if (s->phase==MESH_LOCATE_IDLE) return;
        unsigned cost=(s->phase==MESH_LOCATE_INFO || s->phase==MESH_LOCATE_START)?2U:1U;
        if (calls+cost>MESH_SCAN_TICK_CALLS) return;
        calls+=cost;
        if (s->phase==MESH_LOCATE_INFO) {
            error=control(s,t,CERBERUS_MESH_LOCATE_COMMAND_INFO,&r);
            if (error!=ESP_OK || !mesh_locate_control_valid(&r,CERBERUS_MESH_LOCATE_COMMAND_INFO,0U) ||
                r.flags || r.session_id==UINT64_MAX) {
                s->error=error!=ESP_OK?error:ESP_ERR_INVALID_STATE;
                s->requested=false; s->phase=MESH_LOCATE_IDLE; return;
            }
            s->request.session_id=r.session_id+1U; s->phase=MESH_LOCATE_START;
        } else if (s->phase==MESH_LOCATE_START) {
            s->start_attempted=true; error=control(s,t,CERBERUS_MESH_LOCATE_COMMAND_START,&r);
            s->phase=MESH_LOCATE_POLL;
            if (error!=ESP_OK) { retry(s,error,now); return; }
            if (!accept(s,&r,CERBERUS_MESH_LOCATE_COMMAND_START,now)) return;
        } else if (s->phase==MESH_LOCATE_POLL) {
            error=control(s,t,CERBERUS_MESH_LOCATE_COMMAND_STATUS,&r);
            if (error!=ESP_OK) { retry(s,error,now); return; }
            if (!accept(s,&r,CERBERUS_MESH_LOCATE_COMMAND_STATUS,now)) return;
            if (!s->has_snapshot || r.sequence!=s->snapshot.match_sequence || terminal(r.state))
                s->phase=MESH_LOCATE_PEEK;
            else { s->next_action_ms=later(now,MESH_LOCATE_POLL_MS); return; }
        } else if (s->phase==MESH_LOCATE_PEEK) {
            int64_t anchor=t->clock_us(t->context);
            s->pending_anchor_valid=observe_clock(s,anchor);
            s->pending_anchor_us=s->pending_anchor_valid?(uint64_t)anchor:0U;
            s->pinned_sequence_floor=s->last_sequence;
            error=control(s,t,CERBERUS_MESH_LOCATE_COMMAND_PEEK,&r);
            if (error!=ESP_OK) { retry(s,error,now); return; }
            if (!accept(s,&r,CERBERUS_MESH_LOCATE_COMMAND_PEEK,now)) return;
            s->pinned_control=r; s->pin=r.sequence; s->wire_size=r.projection_bytes;
            s->part=0U; memset(s->scratch,0,sizeof(s->scratch)); s->phase=MESH_LOCATE_READ;
        } else if (s->phase==MESH_LOCATE_READ) {
            bool done=false; error=read_part(s,t,&done);
            if (error!=ESP_OK) {
                if (error==ESP_ERR_INVALID_RESPONSE) fault(s,error,now); else retry(s,error,now);
                return;
            }
            if (done) {
                if (!publish(s)) { fault(s,ESP_ERR_INVALID_RESPONSE,now); return; }
                if (terminal(s->snapshot.state)) s->phase=MESH_LOCATE_FINAL_STATUS;
                else { s->phase=MESH_LOCATE_POLL; s->next_action_ms=later(now,MESH_LOCATE_POLL_MS); return; }
            }
        } else if (s->phase==MESH_LOCATE_FINAL_STATUS) {
            error=control(s,t,CERBERUS_MESH_LOCATE_COMMAND_STATUS,&r);
            if (error!=ESP_OK) { retry(s,error,now); return; }
            if (!accept(s,&r,CERBERUS_MESH_LOCATE_COMMAND_STATUS,now)) return;
            if (!terminal(r.state) || r.sequence!=s->snapshot.match_sequence ||
                !projection_control(&s->snapshot,&r)) { fault(s,ESP_ERR_INVALID_RESPONSE,now); return; }
            s->final_valid=true; s->phase=MESH_LOCATE_RELEASE;
        } else if (s->phase==MESH_LOCATE_RELEASE) {
            error=control(s,t,CERBERUS_MESH_LOCATE_COMMAND_RELEASE,&r);
            if (error!=ESP_OK) { retry(s,error,now); return; }
            if (!accept(s,&r,CERBERUS_MESH_LOCATE_COMMAND_RELEASE,now)) return;
            s->next_action_ms=later(now,MESH_LOCATE_RETRY_MS); return;
        } else { fault(s,ESP_ERR_INVALID_STATE,now); return; }
    }
}

bool mesh_locate_age_upper(const mesh_locate_t *s,int64_t now,uint64_t *age)
{
    if (!age || (s && overlap(age,sizeof(*age),s,sizeof(*s)))) return false;
    *age=0U;
    if (!s || !s->has_snapshot || !s->snapshot.match_sequence || !s->anchor_valid || s->clock_failed ||
        (s->remote.flags&CERBERUS_MESH_LOCATE_CLOCK_INVALID) ||
        now<0 || (uint64_t)now<s->snapshot_anchor_us ||
        (s->clock_seen && (uint64_t)now<s->last_clock_us) ||
        UINT64_MAX-s->snapshot_age_us<(uint64_t)now-s->snapshot_anchor_us) return false;
    *age=s->snapshot_age_us+(uint64_t)now-s->snapshot_anchor_us; return true;
}
bool mesh_locate_history_get(const mesh_locate_t *s,uint8_t index,mesh_locate_sample_t *out)
{
    if (!s || !out || overlap(out,sizeof(*out),s,sizeof(*s)) ||
        s->history_count>MESH_LOCATE_HISTORY || s->history_head>=MESH_LOCATE_HISTORY || index>=s->history_count) return false;
    *out=s->history[(s->history_head+MESH_LOCATE_HISTORY-s->history_count+index)%MESH_LOCATE_HISTORY];
    return true;
}
bool mesh_locate_frozen_valid(const mesh_locate_t *s)
{
    if (!s || !s->frozen || !s->final_valid || s->faulted || !s->has_snapshot || !s->session || !s->origin_session ||
        !mesh_locate_released(s) || s->accepted || s->have_control || s->start_attempted || s->stop_requested ||
        s->close_requested || s->pin || s->part || !terminal(s->snapshot.state) ||
        !projection_valid(&s->snapshot,&s->target) ||
        !mesh_locate_control_valid(&s->remote,CERBERUS_MESH_LOCATE_COMMAND_RELEASE,s->session) ||
        s->remote.flags || s->remote.error_code || s->error!=s->snapshot.error_code ||
        s->request.address!=s->target.value || s->request.pan!=s->target.pan.value ||
        s->request.address_mode!=s->target.mode || s->request.channel!=s->target.pan.channel ||
        s->request.pan_known!=s->target.pan.known || s->request.version!=1U ||
        s->history_count!=(s->samples>MESH_LOCATE_HISTORY?MESH_LOCATE_HISTORY:s->samples) ||
        s->history_head!=s->samples%MESH_LOCATE_HISTORY || s->samples>s->snapshot.match_sequence ||
        (!!s->samples != !!s->snapshot.match_sequence) ||
        s->coalesced_matches!=s->snapshot.match_sequence-s->samples ||
        s->last_sequence!=s->snapshot.match_sequence ||
        s->pinned_sequence_floor>s->snapshot.match_sequence ||
        !mesh_locate_control_valid(&s->pinned_control,CERBERUS_MESH_LOCATE_COMMAND_PEEK,s->session) ||
        !projection_control(&s->snapshot,&s->pinned_control)) return false;
    if (s->anchor_valid) {
        if (s->clock_failed || !s->clock_seen || !s->snapshot.match_sequence ||
            s->snapshot_anchor_us>INT64_MAX || s->snapshot_anchor_us>s->last_clock_us ||
            (s->snapshot.flags&(CERBERUS_MESH_LOCATE_COPY_TIME_VALID|CERBERUS_MESH_LOCATE_MATCH_TIME_VALID))!=3U ||
            (s->snapshot.flags&CERBERUS_MESH_LOCATE_PROJECTION_CLOCK_INVALID) ||
            s->snapshot_age_us<s->snapshot.copied_us-s->snapshot.last_match_us) return false;
    } else if (s->snapshot_anchor_us || s->snapshot_age_us) return false;
    uint32_t previous=0U; uint64_t time=0U; bool time_seen=false;
    for (uint8_t i=0U;i<s->history_count;++i) {
        mesh_locate_sample_t h;
        if (!mesh_locate_history_get(s,i,&h) || !h.sequence || h.sequence<=previous ||
            h.sequence>s->snapshot.match_sequence || h.reserved || (h.flags&~14U) ||
            (!(h.flags&CERBERUS_MESH_LOCATE_MATCH_TIME_VALID) && h.callback_us) ||
            (!(h.flags&CERBERUS_MESH_LOCATE_RSSI_VALID) && h.rssi) ||
            (!(h.flags&CERBERUS_MESH_LOCATE_LQI_VALID) && h.lqi) ||
            ((h.flags&CERBERUS_MESH_LOCATE_MATCH_TIME_VALID) && time_seen && h.callback_us<time)) return false;
        previous=h.sequence;
        if (h.flags&CERBERUS_MESH_LOCATE_MATCH_TIME_VALID) { time=h.callback_us; time_seen=true; }
        if (i+1U==s->history_count && (h.sequence!=s->snapshot.match_sequence ||
            h.callback_us!=s->snapshot.last_match_us || h.rssi!=s->snapshot.rssi || h.lqi!=s->snapshot.lqi ||
            h.flags!=(s->snapshot.flags&14U))) return false;
    }
    return true;
}
