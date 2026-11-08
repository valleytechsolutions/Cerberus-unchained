#include "mesh_scan.h"
#include <string.h>

static bool channel_valid(uint8_t channel) { return channel >= 11U && channel <= 26U; }
static bool terminal(const cerberus_mesh_observation_control_t *c)
{
    return (c->state == CERBERUS_MESH_OBSERVATION_COMPLETE ||
            c->state == CERBERUS_MESH_OBSERVATION_ERROR) &&
           !(c->flags & CERBERUS_MESH_OBSERVATION_RX_ACTIVE);
}
static uint64_t later(uint64_t now, uint64_t delay)
{ return UINT64_MAX - now < delay ? UINT64_MAX : now + delay; }
static uint64_t little(const uint8_t *p, size_t size)
{
    uint64_t value = 0;
    for (size_t i = 0; i < size; ++i) value |= (uint64_t)p[i] << (8U * i);
    return value;
}

bool mesh_scan_control_valid(const cerberus_mesh_observation_control_t *c,
                              uint8_t command, uint64_t requested_session)
{
    if (command < CERBERUS_MESH_OBSERVATION_COMMAND_INFO ||
        command > CERBERUS_MESH_OBSERVATION_COMMAND_RELEASE || command == CERBERUS_MESH_OBSERVATION_COMMAND_READ)
        return false;
    if (!c || !c->session_id || (command != CERBERUS_MESH_OBSERVATION_COMMAND_INFO &&
        c->session_id != requested_session)) return false;
    if (!(c->flags & CERBERUS_MESH_OBSERVATION_BOUND)) {
        if (command == CERBERUS_MESH_OBSERVATION_COMMAND_INFO) {
            return c->capacity == CERBERUS_MESH_OBSERVATION_CAPACITY &&
                c->version == CERBERUS_MESH_OBSERVATION_VERSION && !c->sequence &&
                !c->error_code && !c->count && !c->channel &&
                ((!c->flags && c->state == CERBERUS_MESH_OBSERVATION_IDLE) ||
                 (c->flags == CERBERUS_MESH_OBSERVATION_HELD &&
                  c->state == CERBERUS_MESH_OBSERVATION_QUEUED));
        }
        return !c->sequence && !c->state && !c->flags && !c->count && !c->capacity &&
            !c->channel && !c->version &&
            (c->error_code != ESP_OK || command == CERBERUS_MESH_OBSERVATION_COMMAND_RELEASE);
    }
    if (!(c->flags & CERBERUS_MESH_OBSERVATION_HELD) || (c->flags & ~0x7fU) ||
        c->version != CERBERUS_MESH_OBSERVATION_VERSION ||
        c->capacity != CERBERUS_MESH_OBSERVATION_CAPACITY || c->count > c->capacity ||
        c->state < CERBERUS_MESH_OBSERVATION_QUEUED || c->state > CERBERUS_MESH_OBSERVATION_ERROR ||
        (c->channel && !channel_valid(c->channel)) ||
        ((c->flags & CERBERUS_MESH_OBSERVATION_RX_ACTIVE) &&
         (c->state != CERBERUS_MESH_OBSERVATION_RECEIVING || !channel_valid(c->channel)))) return false;
    if (command != CERBERUS_MESH_OBSERVATION_COMMAND_PEEK &&
        c->sequence > CERBERUS_MESH_OBSERVATION_SEQUENCE_MAX) return false;
    return true;
}

void mesh_scan_init(mesh_scan_t *s) { if (s) memset(s,0,sizeof(*s)); }
bool mesh_scan_released(const mesh_scan_t *s)
{ return !s || (!s->request.session_id && !s->requested && s->phase == MESH_SCAN_IDLE); }

esp_err_t mesh_scan_begin(mesh_scan_t *s, uint8_t mode, uint16_t mask, uint16_t dwell)
{
    if (!s || !mask || mode > CERBERUS_MESH_OBSERVATION_SWEEP || dwell < 100U || dwell > 2000U ||
        (mode == CERBERUS_MESH_OBSERVATION_FIXED && (mask & (uint16_t)(mask - 1U))))
        return ESP_ERR_INVALID_ARG;
    if (!mesh_scan_released(s)) return ESP_ERR_INVALID_STATE;
    s->request = (cerberus_mesh_observation_start_t){.mode=mode,.channel_mask=mask,.dwell_ms=dwell};
    s->requested = true; s->stop_requested = s->close_requested = s->start_attempted = false;
    s->accepted = s->have_control = s->faulted = s->final_valid = false;
    s->callback_time_seen = false; s->observed_mask = 0U;
    s->first_callback_us = s->last_callback_us = 0U;
    s->sequence = s->acked_sequence = s->last_enqueued = 0U;
    s->error = s->fault_error = ESP_OK; s->next_action_ms = s->last_status_ms = s->last_tick_ms = 0U;
    s->part = 0U; s->phase = MESH_SCAN_INFO;
    memset(&s->remote,0,sizeof(s->remote)); memset(s->scratch,0,sizeof(s->scratch));
    return ESP_OK;
}

void mesh_scan_stop(mesh_scan_t *s)
{
    if (!s) return;
    s->requested = false; s->stop_requested = true; s->next_action_ms = 0U;
    if (!s->start_attempted) {
        s->request.session_id = 0U; s->phase = MESH_SCAN_IDLE; s->stop_requested = false;
    }
}
void mesh_scan_close(mesh_scan_t *s)
{
    if (!s) return;
    mesh_scan_stop(s); s->close_requested = true;
}

static void fault(mesh_scan_t *s, esp_err_t error, uint64_t now)
{
    s->faulted = true; s->fault_error = s->error = error;
    s->requested = false; s->stop_requested = true; s->final_valid = false;
    if (s->accepted) s->frozen = false;
    s->next_action_ms = later(now,MESH_SCAN_RETRY_MS);
}

static void retire(mesh_scan_t *s, const cerberus_mesh_observation_control_t *r, bool released)
{
    if (s->accepted) s->frozen = released && s->final_valid && !s->faulted;
    s->error = s->faulted ? s->fault_error : s->frozen ? s->final_summary.error_code : r->error_code;
    s->request.session_id = 0U; s->requested = s->stop_requested = s->close_requested = false;
    s->start_attempted = s->accepted = s->have_control = false;
    s->phase = MESH_SCAN_IDLE; s->part = 0U; s->sequence = 0U;
    s->remote = *r; s->next_action_ms = 0U;
}

static bool accept(mesh_scan_t *s, const cerberus_mesh_observation_control_t *r,
                    uint8_t command, uint64_t now)
{
    if (!mesh_scan_control_valid(r,command,s->request.session_id)) {
        fault(s,ESP_ERR_INVALID_RESPONSE,now); return false;
    }
    if (!(r->flags & CERBERUS_MESH_OBSERVATION_BOUND)) {
        retire(s,r,command == CERBERUS_MESH_OBSERVATION_COMMAND_RELEASE && r->error_code == ESP_OK);
        return false;
    }
    if (r->channel && !(s->request.channel_mask & (uint16_t)(1U << (r->channel - 11U)))) {
        fault(s,ESP_ERR_INVALID_RESPONSE,now); return false;
    }
    if (command != CERBERUS_MESH_OBSERVATION_COMMAND_PEEK &&
        command != CERBERUS_MESH_OBSERVATION_COMMAND_ACK &&
        command != CERBERUS_MESH_OBSERVATION_COMMAND_RELEASE) {
        if (s->accepted && r->sequence < s->last_enqueued) {
            fault(s,ESP_ERR_INVALID_RESPONSE,now); return false;
        }
        s->last_enqueued = r->sequence;
    }
    if (!s->accepted) {
        if (!mesh_observation_init(&s->model,s->request.session_id)) {
            fault(s,ESP_ERR_INVALID_STATE,now); return false;
        }
        memset(&s->final_summary,0,sizeof(s->final_summary));
        s->accepted = true; s->frozen = false;
    }
    s->remote = *r; s->have_control = true;
    if (command == CERBERUS_MESH_OBSERVATION_COMMAND_STATUS ||
        command == CERBERUS_MESH_OBSERVATION_COMMAND_START) s->last_status_ms = now;
    s->error = s->faulted ? s->fault_error : r->error_code;
    if (terminal(r)) s->requested = false;
    return true;
}

static esp_err_t control(mesh_scan_t *s, const mesh_scan_transport_t *t, uint8_t command,
                         cerberus_mesh_observation_control_t *reply)
{
    cerberus_mesh_observation_request_t request = {.session_id=s->request.session_id};
    const void *data = &request; size_t size = sizeof(request);
    if (command == CERBERUS_MESH_OBSERVATION_COMMAND_INFO) { data = NULL; size = 0U; }
    if (command == CERBERUS_MESH_OBSERVATION_COMMAND_START) { data = &s->request; size = sizeof(s->request); }
    memset(reply,0,sizeof(*reply));
    return t->exchange(t->context,command,data,size,reply,sizeof(*reply));
}

static void retry(mesh_scan_t *s, esp_err_t error, uint64_t now)
{ s->error = s->faulted ? s->fault_error : error; s->next_action_ms = later(now,MESH_SCAN_RETRY_MS); }

static esp_err_t read_part(mesh_scan_t *s, const mesh_scan_transport_t *t, bool *done)
{
    bool summary = s->phase == MESH_SCAN_READ_SUMMARY;
    size_t bytes = summary ? CERBERUS_MESH_OBSERVATION_SUMMARY_BYTES : CERBERUS_MESH_OBSERVATION_RECORD_BYTES;
    cerberus_mesh_observation_read_t request = {.session_id=s->request.session_id,.sequence=s->sequence,
        .kind=summary?CERBERUS_MESH_OBSERVATION_SUMMARY:CERBERUS_MESH_OBSERVATION_ROW,.part=s->part};
    cerberus_mesh_observation_chunk_t reply = {0};
    *done = false;
    esp_err_t error = t->exchange(t->context,CERBERUS_MESH_OBSERVATION_COMMAND_READ,
        &request,sizeof(request),&reply,sizeof(reply));
    if (error != ESP_OK) return error;
    if (reply.session_id != request.session_id || reply.part != request.part) return ESP_ERR_INVALID_RESPONSE;
    if (!reply.sequence) {
        for (size_t i = 4U; i < sizeof(reply.data); ++i) if (reply.data[i]) return ESP_ERR_INVALID_RESPONSE;
        int32_t remote = (int32_t)(uint32_t)little(reply.data,4U);
        return remote ? remote : ESP_ERR_INVALID_RESPONSE;
    }
    if (reply.sequence != request.sequence) return ESP_ERR_INVALID_RESPONSE;
    size_t offset = (size_t)s->part * CERBERUS_MESH_OBSERVATION_CHUNK_BYTES;
    if (offset >= bytes) return ESP_ERR_INVALID_RESPONSE;
    size_t amount = bytes - offset;
    if (amount > sizeof(reply.data)) amount = sizeof(reply.data);
    for (size_t i = amount; i < sizeof(reply.data); ++i) if (reply.data[i]) return ESP_ERR_INVALID_RESPONSE;
    memcpy(s->scratch + offset,reply.data,amount);
    ++s->part; *done = offset + amount == bytes;
    return ESP_OK;
}

static bool ingest(mesh_scan_t *s)
{
    const uint8_t *p = s->scratch;
    mesh_observation_input_t input = {0};
    input.session = s->request.session_id; input.sequence = s->sequence;
    input.callback_us = little(p,8U); input.channel = p[8]; input.rssi = (int8_t)p[9];
    input.lqi = p[10]; input.length = p[11]; input.flags = p[12];
    if (p[13] || input.length < 2U || input.length > MESH_MAC_MAX_LENGTH ||
        !channel_valid(input.channel) || !(s->request.channel_mask & (1U << (input.channel - 11U))) ||
        (input.flags & ~7U) || (!(input.flags & MESH_OBSERVATION_TIME_VALID) && input.callback_us) ||
        (!(input.flags & MESH_OBSERVATION_RSSI_VALID) && input.rssi) ||
        (!(input.flags & MESH_OBSERVATION_LQI_VALID) && input.lqi)) return false;
    for (size_t i = input.length; i < MESH_MAC_MAX_LENGTH; ++i) if (p[14U+i]) return false;
    memcpy(input.mac,p+14U,input.length);
    if ((input.flags & MESH_OBSERVATION_TIME_VALID) && s->callback_time_seen &&
        input.callback_us < s->last_callback_us) return false;
    mesh_observation_result_t result = mesh_observation_ingest(&s->model,&input);
    if (result != MESH_OBSERVATION_OK && result != MESH_OBSERVATION_MALFORMED &&
        result != MESH_OBSERVATION_UNSUPPORTED) return false;
    s->observed_mask |= (uint16_t)(1U << (input.channel - 11U));
    if (input.flags & MESH_OBSERVATION_TIME_VALID) {
        if (!s->callback_time_seen) s->first_callback_us = input.callback_us;
        s->last_callback_us = input.callback_us; s->callback_time_seen = true;
    }
    return true;
}

static bool decode_final(mesh_scan_t *s)
{
    const uint8_t *p = s->scratch;
    cerberus_mesh_observation_summary_t r = {0};
    r.version=p[0]; r.flags=p[1]; r.mode=p[2]; r.channel=p[3];
    r.requested_mask=(uint16_t)little(p+4U,2U); r.tuned_mask=(uint16_t)little(p+6U,2U);
    r.dwell_ms=(uint16_t)little(p+8U,2U); r.reserved=(uint16_t)little(p+10U,2U);
    r.started_us=little(p+12U,8U); r.stopped_us=little(p+20U,8U); r.copied_us=little(p+28U,8U);
    r.callbacks=(uint32_t)little(p+36U,4U); r.invalid_callbacks=(uint32_t)little(p+40U,4U);
    r.queue_drops=(uint32_t)little(p+44U,4U); r.enqueued=(uint32_t)little(p+48U,4U);
    r.acked=(uint32_t)little(p+52U,4U); r.last_sequence=(uint32_t)little(p+56U,4U);
    bool saturated = r.callbacks == UINT32_MAX || r.invalid_callbacks == UINT32_MAX ||
        r.queue_drops == UINT32_MAX || r.enqueued == UINT32_MAX || r.acked == UINT32_MAX;
    for (size_t i=0; i<16U; ++i) {
        r.completed_dwell_ms[i]=(uint32_t)little(p+60U+4U*i,4U);
        if (r.completed_dwell_ms[i] && !(r.tuned_mask & (1U << i))) return false;
        saturated |= r.completed_dwell_ms[i] == UINT32_MAX;
    }
    r.error_code=(int32_t)(uint32_t)little(p+124U,4U);
    uint64_t total=(uint64_t)r.invalid_callbacks+r.queue_drops+r.enqueued;
    bool start=(r.flags & CERBERUS_MESH_OBSERVATION_START_TIME_VALID)!=0U;
    bool stop=(r.flags & CERBERUS_MESH_OBSERVATION_STOP_TIME_VALID)!=0U;
    bool copy=(r.flags & CERBERUS_MESH_OBSERVATION_COPY_TIME_VALID)!=0U;
    if (r.version != CERBERUS_MESH_OBSERVATION_VERSION || r.reserved || (r.flags & ~0x7fU) ||
        !(r.flags & CERBERUS_MESH_OBSERVATION_STOPPED) || r.channel || r.mode != s->request.mode ||
        r.requested_mask != s->request.channel_mask || r.dwell_ms != s->request.dwell_ms ||
        (r.tuned_mask & (uint16_t)~r.requested_mask) || (s->observed_mask & (uint16_t)~r.tuned_mask) ||
        (r.callbacks && !r.tuned_mask) ||
        r.enqueued != s->acked_sequence || r.acked != s->acked_sequence ||
        r.last_sequence != s->acked_sequence || s->model.counters.records != s->acked_sequence ||
        r.callbacks != (total > UINT32_MAX ? UINT32_MAX : (uint32_t)total) ||
        saturated != ((r.flags & CERBERUS_MESH_OBSERVATION_COUNTERS_SATURATED)!=0U) ||
        (r.last_sequence == CERBERUS_MESH_OBSERVATION_SEQUENCE_MAX &&
         !(r.flags & CERBERUS_MESH_OBSERVATION_SEQUENCE_EXHAUSTED)) ||
        (!start && r.started_us) || (!stop && r.stopped_us) || (!copy && r.copied_us) ||
        (start && stop && r.started_us > r.stopped_us) ||
        (start && copy && r.started_us > r.copied_us) || (stop && copy && r.stopped_us > r.copied_us) ||
        (s->callback_time_seen && start && s->first_callback_us < r.started_us) ||
        (s->callback_time_seen && stop && s->last_callback_us > r.stopped_us) ||
        (s->callback_time_seen && copy && s->last_callback_us > r.copied_us)) return false;
    s->final_summary=r;
    return true;
}

static bool final_fence(const mesh_scan_t *s)
{
    const cerberus_mesh_observation_control_t *r=&s->remote;
    uint8_t expected=0U;
    if (s->final_summary.flags & CERBERUS_MESH_OBSERVATION_SUMMARY_CLOCK_INVALID) expected|=CERBERUS_MESH_OBSERVATION_CLOCK_INVALID;
    if (s->final_summary.flags & CERBERUS_MESH_OBSERVATION_COUNTERS_SATURATED) expected|=CERBERUS_MESH_OBSERVATION_SATURATED;
    if (s->final_summary.flags & CERBERUS_MESH_OBSERVATION_SEQUENCE_EXHAUSTED) expected|=CERBERUS_MESH_OBSERVATION_EXHAUSTED;
    return terminal(r) && !r->count && !r->channel && r->sequence == s->acked_sequence &&
        (r->flags & CERBERUS_MESH_OBSERVATION_DATA_READY) &&
        (r->flags & (CERBERUS_MESH_OBSERVATION_CLOCK_INVALID|CERBERUS_MESH_OBSERVATION_SATURATED|
         CERBERUS_MESH_OBSERVATION_EXHAUSTED)) == expected && r->error_code == s->final_summary.error_code;
}

void mesh_scan_tick(mesh_scan_t *s, uint64_t now, const mesh_scan_transport_t *t)
{
    if (!s || !t || !t->exchange || !t->clock_us) return;
    int64_t begun=t->clock_us(t->context);
    if (begun<0) { s->error=ESP_ERR_INVALID_STATE; return; }
    if (now<s->last_tick_ms) s->next_action_ms=0U;
    s->last_tick_ms=now;
    unsigned calls=0U;
    while (calls<MESH_SCAN_TICK_CALLS) {
        int64_t current=t->clock_us(t->context);
        if (current<begun) { s->error=ESP_ERR_INVALID_STATE; return; }
        if ((uint64_t)(current-begun)>=MESH_SCAN_TICK_BUDGET_US || now<s->next_action_ms) return;
        cerberus_mesh_observation_control_t reply;
        esp_err_t error;
        if (s->request.session_id && s->stop_requested && (!s->have_control || !terminal(&s->remote))) {
            ++calls; error=control(s,t,CERBERUS_MESH_OBSERVATION_COMMAND_STOP,&reply);
            if (error!=ESP_OK) { retry(s,error,now); return; }
            if (!accept(s,&reply,CERBERUS_MESH_OBSERVATION_COMMAND_STOP,now)) return;
            if (!terminal(&s->remote)) { s->next_action_ms=later(now,MESH_SCAN_RETRY_MS); return; }
            continue;
        }
        if (s->request.session_id && s->close_requested && s->have_control && terminal(&s->remote))
            s->phase=MESH_SCAN_RELEASE;
        if (s->request.session_id && s->start_attempted && s->phase!=MESH_SCAN_RELEASE &&
            (!s->accepted || now<s->last_status_ms || now-s->last_status_ms>=MESH_SCAN_KEEPALIVE_MS)) {
            if (calls==MESH_SCAN_TICK_CALLS) return;
            ++calls; error=control(s,t,CERBERUS_MESH_OBSERVATION_COMMAND_STATUS,&reply);
            if (error!=ESP_OK) { retry(s,error,now); return; }
            if (!accept(s,&reply,CERBERUS_MESH_OBSERVATION_COMMAND_STATUS,now)) return;
            continue;
        }
        if (s->faulted && !s->close_requested) return;
        if (s->phase==MESH_SCAN_IDLE) return;
        unsigned cost=(s->phase==MESH_SCAN_INFO || s->phase==MESH_SCAN_START)?2U:1U;
        if (calls+cost>MESH_SCAN_TICK_CALLS) return;
        current=t->clock_us(t->context);
        if (current<begun) { s->error=ESP_ERR_INVALID_STATE; return; }
        if ((uint64_t)(current-begun)>=MESH_SCAN_TICK_BUDGET_US) return;
        calls+=cost;
        if (s->phase==MESH_SCAN_INFO) {
            error=control(s,t,CERBERUS_MESH_OBSERVATION_COMMAND_INFO,&reply);
            if (error!=ESP_OK || !mesh_scan_control_valid(&reply,CERBERUS_MESH_OBSERVATION_COMMAND_INFO,0U) ||
                (reply.flags & (CERBERUS_MESH_OBSERVATION_BOUND|CERBERUS_MESH_OBSERVATION_HELD)) ||
                reply.session_id==UINT64_MAX) {
                s->error=error!=ESP_OK?error:ESP_ERR_INVALID_STATE;
                s->requested=false; s->phase=MESH_SCAN_IDLE; return;
            }
            s->request.session_id=reply.session_id+1U; s->phase=MESH_SCAN_START;
        } else if (s->phase==MESH_SCAN_START) {
            s->start_attempted=true;
            error=control(s,t,CERBERUS_MESH_OBSERVATION_COMMAND_START,&reply);
            s->phase=MESH_SCAN_PEEK_ROW;
            if (error!=ESP_OK) { retry(s,error,now); return; }
            if (!accept(s,&reply,CERBERUS_MESH_OBSERVATION_COMMAND_START,now)) return;
        } else if (s->phase==MESH_SCAN_PEEK_ROW || s->phase==MESH_SCAN_PEEK_SUMMARY) {
            bool summary=s->phase==MESH_SCAN_PEEK_SUMMARY;
            cerberus_mesh_observation_peek_t request={.session_id=s->request.session_id,
                .kind=summary?CERBERUS_MESH_OBSERVATION_SUMMARY:CERBERUS_MESH_OBSERVATION_ROW};
            memset(&reply,0,sizeof(reply));
            error=t->exchange(t->context,CERBERUS_MESH_OBSERVATION_COMMAND_PEEK,&request,sizeof(request),&reply,sizeof(reply));
            if (error!=ESP_OK) { retry(s,error,now); return; }
            if (!accept(s,&reply,CERBERUS_MESH_OBSERVATION_COMMAND_PEEK,now)) return;
            if (!(reply.flags & CERBERUS_MESH_OBSERVATION_DATA_READY)) { retry(s,ESP_ERR_NOT_FINISHED,now); return; }
            if (summary) {
                if (!terminal(&reply) || reply.count || reply.sequence<=CERBERUS_MESH_OBSERVATION_SUMMARY_BASE) {
                    fault(s,ESP_ERR_INVALID_RESPONSE,now); return;
                }
                s->phase=MESH_SCAN_READ_SUMMARY;
            } else if (!reply.sequence) {
                if (reply.count) { fault(s,ESP_ERR_INVALID_RESPONSE,now); return; }
                if (terminal(&reply)) s->phase=MESH_SCAN_PEEK_SUMMARY;
                else { s->next_action_ms=later(now,MESH_SCAN_EMPTY_POLL_MS); return; }
                continue;
            } else {
                if (!reply.count || reply.sequence>CERBERUS_MESH_OBSERVATION_SEQUENCE_MAX ||
                    reply.sequence!=s->acked_sequence+1U) { fault(s,ESP_ERR_INVALID_RESPONSE,now); return; }
                s->phase=MESH_SCAN_READ_ROW;
            }
            s->sequence=reply.sequence; s->part=0U; memset(s->scratch,0,sizeof(s->scratch));
        } else if (s->phase==MESH_SCAN_READ_ROW || s->phase==MESH_SCAN_READ_SUMMARY) {
            bool done=false; error=read_part(s,t,&done);
            if (error!=ESP_OK) {
                if (error==ESP_ERR_INVALID_RESPONSE) fault(s,error,now); else retry(s,error,now);
                return;
            }
            if (done) {
                if (s->phase==MESH_SCAN_READ_ROW) {
                    if (!ingest(s)) { fault(s,ESP_ERR_INVALID_RESPONSE,now); return; }
                    s->phase=MESH_SCAN_ACK;
                } else {
                    if (!decode_final(s)) { fault(s,ESP_ERR_INVALID_RESPONSE,now); return; }
                    s->phase=MESH_SCAN_FINAL_STATUS;
                }
            }
        } else if (s->phase==MESH_SCAN_ACK) {
            cerberus_mesh_observation_ack_t request={.session_id=s->request.session_id,.sequence=s->sequence};
            memset(&reply,0,sizeof(reply));
            error=t->exchange(t->context,CERBERUS_MESH_OBSERVATION_COMMAND_ACK,&request,sizeof(request),&reply,sizeof(reply));
            if (error!=ESP_OK) { retry(s,error,now); return; }
            if (!accept(s,&reply,CERBERUS_MESH_OBSERVATION_COMMAND_ACK,now)) return;
            if (!(reply.flags & CERBERUS_MESH_OBSERVATION_DATA_READY) || reply.sequence!=s->sequence) {
                fault(s,ESP_ERR_INVALID_RESPONSE,now); return;
            }
            s->acked_sequence=s->sequence; s->phase=MESH_SCAN_PEEK_ROW;
        } else if (s->phase==MESH_SCAN_FINAL_STATUS) {
            error=control(s,t,CERBERUS_MESH_OBSERVATION_COMMAND_STATUS,&reply);
            if (error!=ESP_OK) { retry(s,error,now); return; }
            if (!accept(s,&reply,CERBERUS_MESH_OBSERVATION_COMMAND_STATUS,now)) return;
            if (!final_fence(s)) { fault(s,ESP_ERR_INVALID_RESPONSE,now); return; }
            s->final_valid=true; s->phase=MESH_SCAN_RELEASE;
        } else if (s->phase==MESH_SCAN_RELEASE) {
            error=control(s,t,CERBERUS_MESH_OBSERVATION_COMMAND_RELEASE,&reply);
            if (error!=ESP_OK) { retry(s,error,now); return; }
            if (!accept(s,&reply,CERBERUS_MESH_OBSERVATION_COMMAND_RELEASE,now)) return;
            s->next_action_ms=later(now,MESH_SCAN_RETRY_MS); return;
        } else { fault(s,ESP_ERR_INVALID_STATE,now); return; }
    }
}
