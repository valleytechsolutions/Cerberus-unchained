/* Actual controller + actual model/parser, deterministic transport boundary. */
#include "mesh_scan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(c) do { ++checks; if (!(c)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c); exit(1); } } while (0)
typedef struct { uint32_t sequence; uint8_t bytes[139]; } row_t;
typedef struct {
    uint64_t epoch;
    cerberus_mesh_observation_start_t request;
    row_t rows[16];
    uint32_t last_sequence, acked, callbacks, drops, invalid;
    unsigned head,count,starts,stops,releases,reads,acks,statuses,peeks,wire_calls;
    unsigned stop_hold,release_hold,summary_fault,record_fault;
    uint8_t bad_control_count,extra_flags;
    int64_t clock,step_us;
    esp_err_t source_error;
    uint8_t last_command;
    bool bound,running,legacy,old_peer,lose_start,lose_ack,lose_release,timeout_read;
    bool wrong_session,wrong_part,wrong_sequence,bad_tail,read_error_bad_tail;
    bool invalid_unbound,start_reject,no_start_time,clock_backwards;
    bool summary_pinned,ack_retiring,final_channel_bad;
    uint8_t summary[128];
} peer_t;
static mesh_scan_t scan;
static peer_t peer;

static void le(uint8_t *p,uint64_t value,size_t count)
{ for (size_t i=0;i<count;++i) p[i]=(uint8_t)(value>>(8U*i)); }
static void reset(void)
{ memset(&peer,0,sizeof(peer)); peer.epoch=100U; mesh_scan_init(&scan); }
static void add_row(unsigned kind)
{
    ++peer.callbacks;
    if (peer.count==16U) { ++peer.drops; return; }
    row_t *r=&peer.rows[(peer.head+peer.count)%16U]; memset(r,0,sizeof(*r));
    r->sequence=++peer.last_sequence;
    le(r->bytes,1000U+10U*r->sequence,8U); r->bytes[8]=11U; r->bytes[9]=(uint8_t)-45;
    r->bytes[10]=200U; r->bytes[11]=9U; r->bytes[12]=7U;
    const uint8_t mac[]={0x41,0x88,7,0x34,0x12,0x44,0x33,0x22,0x11};
    memcpy(r->bytes+14U,mac,sizeof(mac));
    if (kind==1U) r->bytes[14]=0x44U; /* Unsupported frame type, retained raw. */
    if (kind==2U) { r->bytes[11]=2U; memset(r->bytes+16U,0,123U); } /* Truncated header. */
    ++peer.count;
}
static void control_reply(cerberus_mesh_observation_control_t *r,uint64_t session)
{
    memset(r,0,sizeof(*r)); r->session_id=session;
    if (!peer.bound || session!=peer.epoch) {
        r->error_code=ESP_ERR_NOT_FOUND;
        if (peer.invalid_unbound) r->capacity=16U;
        return;
    }
    r->sequence=peer.last_sequence; r->error_code=peer.source_error;
    r->state=peer.running?CERBERUS_MESH_OBSERVATION_RECEIVING:
        peer.stop_hold?CERBERUS_MESH_OBSERVATION_STOPPING:
        peer.source_error?CERBERUS_MESH_OBSERVATION_ERROR:CERBERUS_MESH_OBSERVATION_COMPLETE;
    r->flags=CERBERUS_MESH_OBSERVATION_BOUND|CERBERUS_MESH_OBSERVATION_HELD|
        CERBERUS_MESH_OBSERVATION_DATA_READY|peer.extra_flags;
    if (peer.running) r->flags|=CERBERUS_MESH_OBSERVATION_RX_ACTIVE;
    r->count=peer.bad_control_count?peer.bad_control_count:(uint8_t)peer.count;
    r->capacity=16U; r->channel=peer.running?11U:0U; r->version=1U;
}
static void make_summary(void)
{
    uint8_t *p=peer.summary; memset(p,0,128U);
    p[0]=1U; p[1]=CERBERUS_MESH_OBSERVATION_STOPPED|CERBERUS_MESH_OBSERVATION_STOP_TIME_VALID|
        CERBERUS_MESH_OBSERVATION_COPY_TIME_VALID;
    if (!peer.no_start_time) p[1]|=CERBERUS_MESH_OBSERVATION_START_TIME_VALID;
    if (peer.extra_flags&CERBERUS_MESH_OBSERVATION_EXHAUSTED) p[1]|=CERBERUS_MESH_OBSERVATION_SEQUENCE_EXHAUSTED;
    p[2]=peer.request.mode; le(p+4U,peer.request.channel_mask,2U);
    le(p+6U,peer.no_start_time?0U:1U,2U); le(p+8U,peer.request.dwell_ms,2U);
    le(p+12U,peer.no_start_time?0U:500U,8U); le(p+20U,1000000U,8U); le(p+28U,1100000U,8U);
    le(p+36U,peer.callbacks,4U); le(p+40U,peer.invalid,4U); le(p+44U,peer.drops,4U);
    le(p+48U,peer.last_sequence,4U); le(p+52U,peer.acked,4U); le(p+56U,peer.last_sequence,4U);
    if (!peer.no_start_time) le(p+60U,100U,4U);
    le(p+124U,(uint32_t)peer.source_error,4U);
    switch (peer.summary_fault) {
    case 1:p[0]=2U;break; case 2:p[1]|=0x80U;break; case 3:p[10]=1U;break;
    case 4:p[2]^=1U;break; case 5:p[4]^=2U;break; case 6:p[8]^=1U;break;
    case 7:p[6]|=2U;break; case 8:p[64]=1U;break; case 9:p[3]=11U;break;
    case 10:p[52]^=1U;break; case 11:p[48]^=1U;break; case 12:p[36]^=1U;break;
    case 13:p[1]&=(uint8_t)~CERBERUS_MESH_OBSERVATION_STOP_TIME_VALID;break;
    case 14:le(p+12U,2000000U,8U);break; case 15:le(p+28U,999U,8U);break;
    case 16:le(p+124U,ESP_FAIL,4U);break;
    case 17:p[1]|=CERBERUS_MESH_OBSERVATION_COUNTERS_SATURATED;break;
    case 18:le(p+36U,UINT32_MAX,4U);break;
    case 19:le(p+20U,999U,8U);break;
    case 20:p[1]&=(uint8_t)~CERBERUS_MESH_OBSERVATION_STOPPED;break;
    default:break;
    }
}
static void mutate_record(uint8_t *p,unsigned fault)
{
    switch (fault) {
    case 1:p[13]=1U;break;case 2:p[11]=1U;break;case 3:p[11]=126U;break;
    case 4:p[8]=10U;break;case 5:p[8]=12U;break;case 6:p[12]|=8U;break;
    case 7:p[12]&=(uint8_t)~MESH_OBSERVATION_TIME_VALID;break;
    case 8:p[12]&=(uint8_t)~MESH_OBSERVATION_RSSI_VALID;break;
    case 9:p[12]&=(uint8_t)~MESH_OBSERVATION_LQI_VALID;break;
    case 10:p[138]=1U;break;default:break;
    }
}
static esp_err_t exchange(void *context,uint8_t command,const void *request,size_t request_size,
                           void *reply,size_t reply_size)
{
    CHECK(context==&peer && reply_size==22U);
    peer.last_command=command;
    peer.wire_calls+=(command==CERBERUS_MESH_OBSERVATION_COMMAND_INFO ||
        command==CERBERUS_MESH_OBSERVATION_COMMAND_START)?2U:1U;
    peer.clock+=peer.step_us;
    if (peer.clock_backwards) peer.clock-=100000U;
    memset(reply,0,reply_size);
    cerberus_mesh_observation_control_t *c=reply;
    if (command==CERBERUS_MESH_OBSERVATION_COMMAND_INFO) {
        CHECK(request_size==0U && !request);
        if (peer.old_peer) return ESP_ERR_NOT_SUPPORTED;
        if (peer.bound) control_reply(c,peer.epoch);
        else { c->session_id=peer.epoch;c->capacity=16U;c->version=1U;
            if (peer.legacy) { c->flags=CERBERUS_MESH_OBSERVATION_HELD;c->state=CERBERUS_MESH_OBSERVATION_QUEUED; } }
        return ESP_OK;
    }
    uint64_t session=0; CHECK(request!=NULL && request_size>=8U); memcpy(&session,request,8U);
    if (command==CERBERUS_MESH_OBSERVATION_COMMAND_START) {
        CHECK(request_size==sizeof(peer.request)); ++peer.starts;
        if (peer.start_reject || session!=peer.epoch+1U || peer.bound) {
            c->session_id=session;c->error_code=ESP_ERR_INVALID_STATE;return ESP_OK;
        }
        peer.epoch=session;peer.bound=true;peer.running=true;memcpy(&peer.request,request,sizeof(peer.request));
        control_reply(c,session);
        if (peer.lose_start) { peer.lose_start=false;return ESP_ERR_TIMEOUT; }
        return ESP_OK;
    }
    if (command==CERBERUS_MESH_OBSERVATION_COMMAND_READ) {
        CHECK(request_size==sizeof(cerberus_mesh_observation_read_t)); ++peer.reads;
        const cerberus_mesh_observation_read_t *r=request;
        cerberus_mesh_observation_chunk_t *chunk=reply;
        chunk->session_id=session;chunk->part=r->part;
        if (peer.timeout_read) { peer.timeout_read=false;return ESP_ERR_TIMEOUT; }
        bool summary=r->kind==CERBERUS_MESH_OBSERVATION_SUMMARY;
        if (!peer.bound || session!=peer.epoch ||
            (summary?(!peer.summary_pinned || r->sequence!=UINT32_C(0x80000001)):
            (!peer.count || r->sequence!=peer.rows[peer.head].sequence))) {
            le(chunk->data,ESP_ERR_NOT_FOUND,4U);
            if (peer.read_error_bad_tail) chunk->data[8]=1U;
            return ESP_OK;
        }
        const uint8_t *bytes=summary?peer.summary:peer.rows[peer.head].bytes;
        size_t size=summary?128U:139U,offset=(size_t)r->part*9U;
        CHECK(offset<size); size_t amount=size-offset;if(amount>9U)amount=9U;
        chunk->sequence=r->sequence; memcpy(chunk->data,bytes+offset,amount);
        if (peer.wrong_session) chunk->session_id^=1U;
        if (peer.wrong_part) ++chunk->part;
        if (peer.wrong_sequence) chunk->sequence^=UINT32_C(0x80000000);
        if (peer.bad_tail && amount<9U) chunk->data[8]=1U;
        return ESP_OK;
    }
    control_reply(c,session);
    if (command==CERBERUS_MESH_OBSERVATION_COMMAND_RELEASE) {
        CHECK(request_size==8U);++peer.releases;
        if (session==peer.epoch && !peer.bound) { memset(c,0,sizeof(*c));c->session_id=session;return ESP_OK; }
        if (!peer.bound || session!=peer.epoch) return ESP_OK;
        if (peer.release_hold) { --peer.release_hold;c->flags&=(uint8_t)~CERBERUS_MESH_OBSERVATION_DATA_READY;return ESP_OK; }
        peer.bound=false;peer.running=false;peer.count=0U;
        memset(c,0,sizeof(*c));c->session_id=session;
        if (peer.lose_release) { peer.lose_release=false;return ESP_ERR_TIMEOUT; }
        return ESP_OK;
    }
    if (!peer.bound || session!=peer.epoch) return ESP_OK;
    if (command==CERBERUS_MESH_OBSERVATION_COMMAND_STATUS) {
        CHECK(request_size==8U);++peer.statuses;
        if (peer.final_channel_bad && peer.summary_pinned) c->channel=11U;
        return ESP_OK;
    }
    if (command==CERBERUS_MESH_OBSERVATION_COMMAND_STOP) {
        CHECK(request_size==8U);++peer.stops;peer.running=false;
        if (peer.stop_hold) --peer.stop_hold;
        control_reply(c,session);return ESP_OK;
    }
    if (command==CERBERUS_MESH_OBSERVATION_COMMAND_PEEK) {
        CHECK(request_size==sizeof(cerberus_mesh_observation_peek_t));++peer.peeks;
        const cerberus_mesh_observation_peek_t *r=request;
        if (r->kind==CERBERUS_MESH_OBSERVATION_SUMMARY) {
            CHECK(!peer.running && !peer.count);make_summary();peer.summary_pinned=true;c->sequence=UINT32_C(0x80000001);
        } else { CHECK(r->kind==CERBERUS_MESH_OBSERVATION_ROW);c->sequence=peer.count?peer.rows[peer.head].sequence:0U; }
        return ESP_OK;
    }
    if (command==CERBERUS_MESH_OBSERVATION_COMMAND_ACK) {
        CHECK(request_size==sizeof(cerberus_mesh_observation_ack_t));++peer.acks;
        const cerberus_mesh_observation_ack_t *r=request;
        if (peer.ack_retiring) {
            c->flags&=(uint8_t)~CERBERUS_MESH_OBSERVATION_DATA_READY;c->error_code=ESP_ERR_NOT_FOUND;
            return ESP_OK;
        }
        if (r->sequence!=peer.acked) {
            CHECK(peer.count && peer.rows[peer.head].sequence==r->sequence);
            peer.head=(peer.head+1U)%16U;--peer.count;peer.acked=r->sequence;
        }
        control_reply(c,session);c->sequence=r->sequence;
        if (peer.lose_ack) { peer.lose_ack=false;return ESP_ERR_TIMEOUT; }
        return ESP_OK;
    }
    CHECK(false);return ESP_FAIL;
}
static int64_t clock_us(void *context) { CHECK(context==&peer);return peer.clock; }
static const mesh_scan_transport_t transport={.exchange=exchange,.clock_us=clock_us,.context=&peer};
static void tick(uint64_t now)
{
    unsigned before=peer.wire_calls;
    mesh_scan_tick(&scan,now,&transport);
    CHECK(peer.wire_calls-before<=MESH_SCAN_TICK_CALLS);
}
static void begin(void)
{
    CHECK(mesh_scan_begin(&scan,CERBERUS_MESH_OBSERVATION_FIXED,1U,100U)==ESP_OK);
    tick(0U);CHECK(scan.accepted && peer.starts==1U && scan.model.session==peer.epoch);
}
static void until_phase(unsigned phase,uint64_t *now)
{
    for (unsigned i=0;i<2000U && scan.phase!=phase && !scan.faulted;++i) { *now+=25U;tick(*now); }
    CHECK(scan.phase==phase);
}
static void finish(uint64_t *now)
{
    for (unsigned i=0;i<4000U && !mesh_scan_released(&scan) && !scan.faulted;++i) { *now+=25U;tick(*now); }
    CHECK(mesh_scan_released(&scan));
}
static void abandon(uint64_t *now)
{
    mesh_scan_close(&scan);
    for (unsigned i=0;i<2000U && !mesh_scan_released(&scan);++i) { *now+=25U;tick(*now); }
    CHECK(mesh_scan_released(&scan) && !scan.frozen && !peer.bound);
}

static void ordinary_and_empty(void)
{
    for (unsigned count=0;count<=16U;++count) {
        reset();begin();for(unsigned i=0;i<count;++i)add_row(i%3U);
        if(count==16U) { add_row(0U);add_row(0U);peer.invalid=3U;peer.callbacks+=3U; }
        unsigned before=peer.wire_calls;mesh_scan_stop(&scan);CHECK(before==peer.wire_calls);
        uint64_t now=0U;finish(&now);
        CHECK(scan.frozen && scan.final_valid && scan.model.counters.records==count);
        CHECK(scan.final_summary.enqueued==count && scan.final_summary.acked==count);
        CHECK(scan.model.counters.unsupported==(count+1U)/3U);
        CHECK(scan.model.counters.malformed==count/3U);
        CHECK(peer.reads==count*16U+15U && peer.starts==1U);
        CHECK(scan.final_summary.queue_drops==(count==16U?2U:0U));
    }
    reset();begin();peer.no_start_time=true;peer.running=false;
    uint64_t now=0U;finish(&now);
    CHECK(scan.frozen && !(scan.final_summary.flags&CERBERUS_MESH_OBSERVATION_START_TIME_VALID));
    reset();begin();add_row(0U);peer.source_error=ESP_FAIL;
    mesh_scan_stop(&scan);now=0U;finish(&now);
    CHECK(scan.frozen && scan.error==ESP_FAIL && scan.final_summary.error_code==ESP_FAIL);
    reset();begin();peer.extra_flags=CERBERUS_MESH_OBSERVATION_EXHAUSTED;
    mesh_scan_stop(&scan);now=0U;finish(&now);CHECK(scan.frozen); /* Summary-pin exhaustion flag is not a row gap. */
}

static void lost_replies(void)
{
    reset();peer.lose_start=true;
    CHECK(mesh_scan_begin(&scan,0U,1U,100U)==ESP_OK);tick(0U);
    CHECK(peer.bound && !scan.accepted && scan.start_attempted && scan.request.session_id==peer.epoch);
    tick(699U);CHECK(peer.starts==1U && peer.statuses==0U);
    tick(700U);CHECK(scan.accepted && peer.statuses==1U && peer.starts==1U);
    add_row(0U);peer.timeout_read=true;peer.lose_ack=true;
    uint64_t now=700U;until_phase(MESH_SCAN_ACK,&now);
    CHECK(scan.model.counters.records==1U);
    tick(now+25U);now+=25U;
    CHECK(peer.acked==1U && scan.acked_sequence==0U && scan.phase==MESH_SCAN_ACK);
    unsigned records=scan.model.counters.records;mesh_scan_stop(&scan);
    peer.release_hold=2U;peer.lose_release=true;
    for(unsigned i=0;i<2000U && !scan.final_valid;++i){now+=25U;tick(now);}
    CHECK(scan.final_valid && !scan.frozen && scan.model.counters.records==records);
    finish(&now);
    CHECK(scan.frozen && peer.releases>=4U && peer.acks>=2U && peer.starts==1U);
    CHECK(scan.model.counters.replayed==0U && scan.model.counters.records==1U);
}

static void stop_close_and_identity(void)
{
    reset();CHECK(mesh_scan_begin(&scan,0U,1U,100U)==ESP_OK);mesh_scan_stop(&scan);tick(0U);
    CHECK(mesh_scan_released(&scan) && !peer.wire_calls);
    reset();peer.step_us=8000;CHECK(mesh_scan_begin(&scan,0U,1U,100U)==ESP_OK);tick(0U);
    CHECK(scan.phase==MESH_SCAN_START && peer.wire_calls==2U);mesh_scan_stop(&scan);tick(1U);
    CHECK(!peer.starts && mesh_scan_released(&scan));
    reset();begin();add_row(0U);uint64_t now=0U;until_phase(MESH_SCAN_READ_ROW,&now);
    CHECK(scan.part<16U);mesh_scan_stop(&scan);unsigned before=peer.stops;tick(now+25U);now+=25U;
    CHECK(peer.stops==before+1U);finish(&now);CHECK(scan.frozen && scan.model.counters.records==1U);
    reset();begin();add_row(0U);now=0U;until_phase(MESH_SCAN_READ_ROW,&now);
    mesh_scan_close(&scan);finish(&now);CHECK(!scan.frozen && scan.model.counters.records==0U && !peer.count);
    reset();peer.lose_start=true;CHECK(mesh_scan_begin(&scan,0U,1U,100U)==ESP_OK);tick(0U);
    mesh_scan_close(&scan);now=0U;finish(&now);CHECK(!scan.frozen && peer.stops==1U && peer.starts==1U);
    reset();begin();add_row(0U);now=0U;until_phase(MESH_SCAN_READ_ROW,&now);
    peer.bound=false;peer.running=false;peer.epoch+=100U;
    for(unsigned i=0;i<100U && !mesh_scan_released(&scan);++i){now+=100U;tick(now);}
    CHECK(mesh_scan_released(&scan) && !scan.frozen && scan.error==ESP_ERR_NOT_FOUND);
    CHECK(!peer.stops && !peer.releases); /* No command adopts the replacement identity. */
    reset();begin();peer.invalid_unbound=true;peer.bound=false;peer.epoch+=100U;
    tick(1000U);CHECK(scan.faulted && scan.request.session_id && !scan.frozen);
    peer.invalid_unbound=false;now=1000U;
    for(unsigned i=0;i<100U && !mesh_scan_released(&scan);++i){now+=100U;tick(now);}
    CHECK(mesh_scan_released(&scan));
}

static void corruption(void)
{
    for(unsigned fault=1U;fault<=10U;++fault){
        reset();begin();add_row(0U);mutate_record(peer.rows[0].bytes,fault);
        uint64_t now=0U;for(unsigned i=0;i<200U && !scan.faulted;++i){now+=25U;tick(now);}
        CHECK(scan.faulted && !scan.frozen && !peer.acks && scan.model.counters.records==0U);
        abandon(&now);
    }
    for(unsigned fault=1U;fault<=20U;++fault){
        reset();begin();add_row(0U);peer.summary_fault=fault;mesh_scan_stop(&scan);
        uint64_t now=0U;for(unsigned i=0;i<500U && !scan.faulted;++i){now+=25U;tick(now);}
        CHECK(scan.faulted && !scan.frozen && scan.model.counters.records==1U);
        abandon(&now);
    }
    for(unsigned fault=0U;fault<4U;++fault){
        reset();begin();add_row(0U);
        peer.wrong_session=fault==0U;peer.wrong_part=fault==1U;peer.wrong_sequence=fault==2U;peer.bad_tail=fault==3U;
        uint64_t now=0U;for(unsigned i=0;i<200U && !scan.faulted;++i){now+=25U;tick(now);}
        CHECK(scan.faulted && !peer.acks && scan.request.session_id==peer.epoch);
        abandon(&now);
    }
    reset();begin();add_row(0U);add_row(0U);le(peer.rows[1].bytes,900U,8U);
    uint64_t now=0U;for(unsigned i=0;i<500U && !scan.faulted;++i){now+=25U;tick(now);}
    CHECK(scan.faulted && scan.model.counters.records==1U && peer.acked==1U);abandon(&now);
    for(unsigned count=33U;count<=255U;count+=222U){
        reset();begin();peer.bad_control_count=(uint8_t)count;tick(1000U);
        CHECK(scan.faulted && scan.request.session_id==peer.epoch && !scan.frozen);
        peer.bad_control_count=0U;now=1000U;abandon(&now);
    }
    reset();begin();add_row(0U);peer.ack_retiring=true;now=0U;
    for(unsigned i=0;i<300U && !scan.faulted;++i){now+=25U;tick(now);}
    CHECK(scan.faulted && scan.model.counters.records==1U && scan.acked_sequence==0U && peer.count==1U);
    peer.ack_retiring=false;abandon(&now);
    reset();begin();peer.final_channel_bad=true;mesh_scan_stop(&scan);now=0U;
    for(unsigned i=0;i<300U && !scan.faulted;++i){now+=25U;tick(now);}
    CHECK(scan.faulted && !scan.frozen);peer.final_channel_bad=false;abandon(&now);
}

static void new_run_and_budget(void)
{
    reset();begin();add_row(0U);mesh_scan_stop(&scan);uint64_t now=0U;finish(&now);
    uint64_t old=scan.model.session;CHECK(scan.frozen);
    peer.step_us=8000;peer.head=peer.count=peer.last_sequence=peer.acked=peer.callbacks=0U;
    CHECK(mesh_scan_begin(&scan,0U,1U,100U)==ESP_OK);tick(now+25U);now+=25U;
    CHECK(scan.phase==MESH_SCAN_START && scan.model.session==old && scan.frozen);
    peer.start_reject=true;tick(now+25U);now+=25U;
    CHECK(mesh_scan_released(&scan) && scan.model.session==old && scan.frozen);
    peer.start_reject=false;peer.step_us=0;
    CHECK(mesh_scan_begin(&scan,0U,1U,100U)==ESP_OK);tick(now+25U);now+=25U;
    CHECK(scan.accepted && scan.model.session!=old && !scan.frozen && !scan.model.counters.records);
    CHECK(mesh_scan_begin(&scan,0U,1U,100U)==ESP_ERR_INVALID_STATE);abandon(&now);
    reset();peer.old_peer=true;CHECK(mesh_scan_begin(&scan,0U,1U,100U)==ESP_OK);tick(0U);
    CHECK(mesh_scan_released(&scan) && scan.error==ESP_ERR_NOT_SUPPORTED && !peer.starts && !peer.stops);
    reset();peer.legacy=true;CHECK(mesh_scan_begin(&scan,0U,1U,100U)==ESP_OK);tick(0U);
    CHECK(mesh_scan_released(&scan) && !peer.starts && !peer.stops);
    reset();peer.epoch=UINT64_MAX;CHECK(mesh_scan_begin(&scan,0U,1U,100U)==ESP_OK);tick(0U);
    CHECK(mesh_scan_released(&scan) && !peer.starts);
    reset();CHECK(mesh_scan_begin(&scan,0U,3U,100U)==ESP_ERR_INVALID_ARG);
    CHECK(mesh_scan_begin(&scan,1U,0U,100U)==ESP_ERR_INVALID_ARG);
    CHECK(mesh_scan_begin(&scan,1U,3U,99U)==ESP_ERR_INVALID_ARG);
    CHECK(mesh_scan_begin(&scan,1U,3U,2001U)==ESP_ERR_INVALID_ARG);
    reset();peer.clock=-1;CHECK(mesh_scan_begin(&scan,0U,1U,100U)==ESP_OK);tick(0U);CHECK(!peer.wire_calls);
    peer.clock=0;peer.clock_backwards=true;tick(1U);CHECK(peer.wire_calls==2U && !peer.starts);
    peer.clock_backwards=false;mesh_scan_stop(&scan);CHECK(mesh_scan_released(&scan));
    reset();begin();add_row(0U);peer.step_us=8000;unsigned before=peer.wire_calls;tick(25U);
    CHECK(peer.wire_calls-before==1U); /* No second command begins after8ms. */
    peer.step_us=0;mesh_scan_stop(&scan);now=25U;finish(&now);CHECK(scan.frozen);
}

static void streaming_and_control_shapes(void)
{
    reset();begin();tick(1U);unsigned peeks=peer.peeks,wire=peer.wire_calls;
    tick(250U);CHECK(peer.peeks==peeks && peer.wire_calls==wire);
    tick(251U);CHECK(peer.peeks==peeks+1U); /* Empty poll is250ms after the previous peek. */
    for(unsigned i=0;i<16U;++i)add_row(i%3U);
    uint64_t now=251U;
    for(unsigned i=0;i<1000U && scan.acked_sequence<8U;++i){now+=25U;tick(now);}
    CHECK(scan.acked_sequence>=8U && peer.running && !scan.frozen);
    for(unsigned i=0;i<8U;++i)add_row(i%3U);
    CHECK(peer.last_sequence==24U && peer.count<=16U);
    for(unsigned i=0;i<1500U && scan.acked_sequence<24U;++i){now+=25U;tick(now);}
    CHECK(scan.acked_sequence==24U && scan.model.counters.records==24U && peer.running);
    CHECK(peer.statuses>0U && scan.model.counters.replayed==0U);
    mesh_scan_stop(&scan);finish(&now);CHECK(scan.frozen && scan.final_summary.enqueued==24U);

    reset();peer.step_us=8000;CHECK(mesh_scan_begin(&scan,0U,1U,100U)==ESP_OK);tick(0U);
    uint64_t candidate=scan.request.session_id;CHECK(candidate==101U && !peer.starts);
    peer.epoch=9000U;peer.step_us=0;tick(1U);
    CHECK(mesh_scan_released(&scan) && peer.starts==1U && !peer.bound && !peer.stops);
    CHECK(scan.model.session==0U); /* Actual stale exact-next candidate rejected after reboot. */

    reset();begin();cerberus_mesh_observation_control_t good,bad;
    control_reply(&good,peer.epoch);
    CHECK(mesh_scan_control_valid(&good,CERBERUS_MESH_OBSERVATION_COMMAND_STATUS,peer.epoch));
    for(unsigned n=0;n<9U;++n){
        bad=good;
        switch(n){
        case 0:bad.session_id++;break;case 1:bad.capacity=15U;break;case 2:bad.version=2U;break;
        case 3:bad.count=17U;break;case 4:bad.flags|=0x80U;break;
        case 5:bad.flags&=(uint8_t)~CERBERUS_MESH_OBSERVATION_HELD;break;
        case 6:bad.state=CERBERUS_MESH_OBSERVATION_COMPLETE;break;
        case 7:bad.channel=0U;break;case 8:bad.sequence=UINT32_C(0x80000001);break;
        }
        CHECK(!mesh_scan_control_valid(&bad,CERBERUS_MESH_OBSERVATION_COMMAND_STATUS,peer.epoch));
    }
    memset(&bad,0,sizeof(bad));bad.session_id=peer.epoch;bad.error_code=ESP_ERR_NOT_FOUND;
    CHECK(mesh_scan_control_valid(&bad,CERBERUS_MESH_OBSERVATION_COMMAND_STATUS,peer.epoch));
    bad.error_code=ESP_OK;
    CHECK(!mesh_scan_control_valid(&bad,CERBERUS_MESH_OBSERVATION_COMMAND_STATUS,peer.epoch));
    CHECK(mesh_scan_control_valid(&bad,CERBERUS_MESH_OBSERVATION_COMMAND_RELEASE,peer.epoch));
    CHECK(!mesh_scan_control_valid(&good,CERBERUS_MESH_OBSERVATION_COMMAND_READ,peer.epoch));
    CHECK(!mesh_scan_control_valid(&good,255U,peer.epoch));
    now=0U;abandon(&now);
}

int main(void)
{
    ordinary_and_empty();lost_replies();stop_close_and_identity();corruption();new_run_and_budget();
    streaming_and_control_shapes();
    printf("mesh controller: %u checks PASS; scan=%zu model=%zu bytes\n",checks,sizeof(scan),sizeof(scan.model));
    return 0;
}
