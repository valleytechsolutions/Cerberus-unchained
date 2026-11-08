/* Actual production controller and parser; only the transport/clock is modeled. */
#include "mesh_locate.h"
#include "mesh_locate_fixture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(c) do { ++checks; if (!(c)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c); exit(1); } } while (0)
typedef struct {
    uint64_t epoch;
    cerberus_mesh_locate_start_t request;
    cerberus_mesh_locate_projection_t live,pinned;
    uint32_t pin;
    uint16_t pin_size;
    unsigned calls,starts,stops,statuses,peeks,reads,releases,stop_hold,release_hold;
    unsigned corrupt,read_fault,control_fault,final_fault,fault_part;
    int64_t clock,cost;
    uint64_t copy;
    bool bound,running,old_peer,lost_start,lost_read,lost_release,reboot_start;
    bool malformed_unbound,legacy,invalid_clock,backward_clock,read_remote_error;
} peer_t;
static peer_t peer;
static mesh_locate_t locate;
static uint64_t now_ms;
static mesh_observation_address_key_t key;
static void le(uint8_t *p,uint64_t value,size_t n)
{ for(size_t i=0;i<n;++i) p[i]=(uint8_t)(value>>(8U*i)); }
static void reset(void)
{
    memset(&peer,0,sizeof(peer)); peer.epoch=100U; peer.clock=1000000; peer.copy=2000000U;
    peer.pin=CERBERUS_MESH_LOCATE_PIN_BASE; now_ms=0U; mesh_locate_init(&locate);
    key=(mesh_observation_address_key_t){.value=0x1122U,.mode=2U,.pan={.value=0x1234U,.channel=11U,.known=true}};
}
static void row(uint32_t seq,unsigned payload)
{
    cerberus_mesh_locate_projection_t *p=&peer.live;
    p->match_sequence=p->matches=seq; p->callbacks=seq;
    p->last_match_us=500000U+seq; p->rssi=-45; p->lqi=200U;
    p->flags|=CERBERUS_MESH_LOCATE_MATCH_TIME_VALID|CERBERUS_MESH_LOCATE_RSSI_VALID|CERBERUS_MESH_LOCATE_LQI_VALID;
    memset(p->mac,0,sizeof(p->mac));
    if (key.mode==2U) {
        const uint8_t raw[]={0x41,0x88,7,0x34,0x12,0x44,0x33,0x22,0x11};
        memcpy(p->mac,raw,sizeof(raw)); le(p->mac+3U,key.pan.value,2U); le(p->mac+7U,key.value,2U);
        p->mac_length=(uint8_t)(9U+payload);
    } else {
        /* 2015 EXT/EXT compressed: both PANs unknown, full source retained. */
        const uint8_t raw[]={0x41,0xec,7}; memcpy(p->mac,raw,sizeof(raw));
        le(p->mac+3U,UINT64_C(0x8877665544332211),8U); le(p->mac+11U,key.value,8U);
        p->mac_length=(uint8_t)(19U+payload);
    }
    for (size_t i=(key.mode==2U?9U:19U);i<p->mac_length;++i) p->mac[i]=(uint8_t)i;
}
static uint8_t flags(const cerberus_mesh_locate_projection_t *p)
{
    uint8_t f=CERBERUS_MESH_LOCATE_BOUND|CERBERUS_MESH_LOCATE_HELD|CERBERUS_MESH_LOCATE_DATA_READY;
    if (p->state==CERBERUS_MESH_LOCATE_RECEIVING) f|=CERBERUS_MESH_LOCATE_RX_ACTIVE;
    if (p->flags&CERBERUS_MESH_LOCATE_PROJECTION_CLOCK_INVALID) f|=CERBERUS_MESH_LOCATE_CLOCK_INVALID;
    if (p->flags&CERBERUS_MESH_LOCATE_COUNTERS_SATURATED) f|=CERBERUS_MESH_LOCATE_SATURATED;
    if (p->flags&CERBERUS_MESH_LOCATE_SEQUENCE_EXHAUSTED) f|=CERBERUS_MESH_LOCATE_EXHAUSTED;
    return f;
}
static void state(void)
{
    peer.live.state=peer.running?CERBERUS_MESH_LOCATE_RECEIVING:peer.stop_hold?CERBERUS_MESH_LOCATE_STOPPING:
        peer.live.error_code?CERBERUS_MESH_LOCATE_ERROR:CERBERUS_MESH_LOCATE_COMPLETE;
    peer.live.channel=peer.running?key.pan.channel:0U;
    if (!peer.running && !peer.stop_hold) peer.live.flags|=CERBERUS_MESH_LOCATE_STOPPED;
    else peer.live.flags&=(uint8_t)~CERBERUS_MESH_LOCATE_STOPPED;
}
static void reply(cerberus_mesh_locate_control_t *r,uint64_t session)
{
    memset(r,0,sizeof(*r)); r->session_id=session;
    if (!peer.bound || session!=peer.epoch) {
        r->error_code=ESP_ERR_NOT_FOUND;
        if (peer.malformed_unbound) r->version=1U;
        return;
    }
    state(); r->state=peer.live.state; r->flags=flags(&peer.live); r->channel=peer.live.channel;
    r->sequence=peer.live.match_sequence; r->version=1U; r->projection_bytes=(uint8_t)(72U+peer.live.mac_length);
    r->error_code=peer.live.error_code;
    switch(peer.control_fault) {
    case 1:r->reserved=1U;break;case 2:r->state=6U;break;case 3:r->flags|=0x80U;break;
    case 4:r->session_id++;break;case 5:r->projection_bytes=71U;break;case 6:r->projection_bytes=198U;break;
    case 7:r->version=2U;break;case 8:r->channel=12U;break;case 9:r->sequence=0x80000000U;break;
    default:break;
    }
}
static void corrupt_projection(cerberus_mesh_locate_projection_t *p)
{
    switch(peer.corrupt) {
    case 1:p->version=2U;break;case 2:p->reserved0=1U;break;case 3:p->reserved1=1U;break;
    case 4:p->address^=UINT64_C(1)<<48U;break;case 5:p->pan_known=2U;break;case 6:p->pan++;break;
    case 7:p->address_mode=1U;break;case 8:p->channel=12U;break;case 9:p->state=6U;break;
    case 10:p->flags^=CERBERUS_MESH_LOCATE_STOPPED;break;case 11:p->matches++;break;
    case 12:p->callbacks++;break;case 13:p->flags&=(uint8_t)~CERBERUS_MESH_LOCATE_COPY_TIME_VALID;break;
    case 14:p->flags&=(uint8_t)~CERBERUS_MESH_LOCATE_MATCH_TIME_VALID;break;
    case 15:p->flags&=(uint8_t)~CERBERUS_MESH_LOCATE_RSSI_VALID;break;
    case 16:p->flags&=(uint8_t)~CERBERUS_MESH_LOCATE_LQI_VALID;break;
    case 17:p->last_match_us=p->copied_us+1U;break;
    case 18:p->flags|=CERBERUS_MESH_LOCATE_COUNTERS_SATURATED;break;
    case 19:p->mac[0]=0x44U;break; /* Unsupported frame type. */
    case 20:p->mac[1]=0U;break; /* No source address. */
    case 21:p->mac[7]^=1U;break;case 22:p->mac[3]^=1U;break;
    case 23:p->mac_length=1U;break;case 24:p->match_sequence=0x80000000U;break;
    case 25:p->error_code=ESP_FAIL;break;case 26:p->mac_length=126U;break;
    case 27:p->match_sequence=p->matches=1U;p->callbacks=1U;break;
    default:break;
    }
}
static void bytes(const cerberus_mesh_locate_projection_t *p,uint8_t out[197])
{
    memset(out,0,197U); out[0]=p->version;out[1]=p->flags;out[2]=p->state;out[3]=p->channel;
    out[4]=p->address_mode;out[5]=p->pan_known;out[6]=p->mac_length;out[7]=p->reserved0;
    le(out+8U,p->pan,2U);out[10]=(uint8_t)p->rssi;out[11]=p->lqi;
    le(out+12U,p->match_sequence,4U);le(out+16U,p->address,8U);le(out+24U,p->copied_us,8U);le(out+32U,p->last_match_us,8U);
    le(out+40U,p->callbacks,4U);le(out+44U,p->invalid_metadata,4U);le(out+48U,p->malformed,4U);
    le(out+52U,p->unsupported,4U);le(out+56U,p->filtered_nonmatch,4U);le(out+60U,p->matches,4U);
    le(out+64U,(uint32_t)p->error_code,4U);le(out+68U,p->reserved1,4U);memcpy(out+72U,p->mac,125U);
}
static esp_err_t exchange(void *ctx,uint8_t cmd,const void *request,size_t n,void *out,size_t outn)
{
    CHECK(ctx==&peer);CHECK(outn==22U);++peer.calls;peer.clock+=peer.cost;
    if (cmd==154U || cmd==155U) ++peer.calls;
    if (peer.old_peer && (cmd==154U || cmd==155U)) return ESP_ERR_NOT_SUPPORTED;
    cerberus_mesh_locate_control_t *r=out;
    if (cmd==154U) {
        CHECK(!n && !request);memset(r,0,sizeof(*r));r->session_id=peer.epoch;r->version=1U;r->projection_bytes=72U;
        if(peer.legacy){r->flags=CERBERUS_MESH_LOCATE_HELD;r->state=CERBERUS_MESH_LOCATE_QUEUED;}
        return ESP_OK;
    }
    if(cmd==155U){
        CHECK(n==22U);++peer.starts;const cerberus_mesh_locate_start_t *q=request;
        if(peer.reboot_start){peer.epoch+=100U;peer.reboot_start=false;}
        if(q->session_id!=peer.epoch+1U){reply(r,q->session_id);return ESP_OK;}
        peer.request=*q;peer.epoch=q->session_id;peer.bound=peer.running=true;
        memset(&peer.live,0,sizeof(peer.live));peer.live.version=1U;peer.live.address=q->address;
        peer.live.address_mode=q->address_mode;peer.live.pan_known=q->pan_known;peer.live.pan=q->pan;
        reply(r,q->session_id);
        if(peer.lost_start){peer.lost_start=false;return ESP_ERR_TIMEOUT;}return ESP_OK;
    }
    if(cmd==159U){
        CHECK(n==13U);++peer.reads;const cerberus_mesh_locate_read_t *q=request;
        cerberus_mesh_locate_chunk_t *chunk=out;memset(chunk,0,sizeof(*chunk));
        chunk->session_id=q->session_id;chunk->part=q->part;
        if(peer.lost_read){peer.lost_read=false;return ESP_ERR_TIMEOUT;}
        if(!peer.bound || q->session_id!=peer.epoch || q->pin!=peer.pin || peer.read_remote_error){
            le(chunk->data,ESP_ERR_NOT_FOUND,4U);if(peer.read_fault==6U)chunk->data[8]=1U;return ESP_OK;
        }
        uint8_t data[197];bytes(&peer.pinned,data);size_t offset=(size_t)q->part*9U;
        size_t length=peer.pin_size;
        CHECK(offset<length);size_t amount=length-offset;if(amount>9U)amount=9U;
        chunk->pin=q->pin;memcpy(chunk->data,data+offset,amount);
        switch(!peer.fault_part || peer.fault_part==q->part+1U?peer.read_fault:0U){
        case 1:chunk->session_id++;break;case 2:chunk->pin++;break;case 3:chunk->part++;break;
        case 4:if(amount<9U)chunk->data[8]=1U;break;case 5:chunk->pin=0U;memset(chunk->data,0,9U);break;
        default:break;
        }return ESP_OK;
    }
    CHECK(n==8U);uint64_t session=((const cerberus_mesh_locate_request_t*)request)->session_id;
    if(cmd==156U){++peer.statuses;reply(r,session);if(peer.final_fault && locate.phase==MESH_LOCATE_FINAL_STATUS)r->sequence++;return ESP_OK;}
    if(cmd==157U){++peer.stops;if(peer.bound && session==peer.epoch){peer.running=false;if(peer.stop_hold)--peer.stop_hold;}reply(r,session);return ESP_OK;}
    if(cmd==160U){
        ++peer.releases;if(peer.bound && session==peer.epoch){if(peer.release_hold)--peer.release_hold;else peer.bound=false;}
        reply(r,session);if(!peer.bound){r->error_code=ESP_OK;if(peer.malformed_unbound)r->version=1U;}
        if(peer.lost_release){peer.lost_release=false;return ESP_ERR_TIMEOUT;}return ESP_OK;
    }
    CHECK(cmd==158U);++peer.peeks;reply(r,session);if(!peer.bound || session!=peer.epoch)return ESP_OK;
    peer.pinned=peer.live;peer.pinned.copied_us=peer.copy;peer.pinned.flags|=CERBERUS_MESH_LOCATE_COPY_TIME_VALID;
    if(peer.pinned.flags&CERBERUS_MESH_LOCATE_PROJECTION_CLOCK_INVALID){peer.pinned.copied_us=0U;peer.pinned.flags&=(uint8_t)~CERBERUS_MESH_LOCATE_COPY_TIME_VALID;}
    peer.pin_size=r->projection_bytes;corrupt_projection(&peer.pinned);r->sequence=++peer.pin;
    /* Controls stay actual/canonical while injected payloads can contradict them. */
    return ESP_OK;
}
static int64_t clock_us(void *ctx)
{ CHECK(ctx==&peer);if(peer.invalid_clock)return -1;if(peer.backward_clock)return peer.clock-2000000;return peer.clock; }
static mesh_scan_transport_t transport={.exchange=exchange,.clock_us=clock_us,.context=&peer};
static void tick(void)
{
    unsigned before=peer.calls;mesh_locate_tick(&locate,now_ms,&transport);
    CHECK(peer.calls-before<=4U);now_ms+=100U;peer.clock+=100000;
}
static void ticks(unsigned n){for(unsigned i=0;i<n;++i)tick();}
static void begin(void){CHECK(mesh_locate_begin(&locate,&key,50U)==ESP_OK);tick();CHECK(peer.starts==1U);}
static void sample(uint32_t seq,unsigned payload)
{ row(seq,payload);for(unsigned i=0;i<100U && (!locate.has_snapshot || locate.snapshot.match_sequence!=seq);++i)tick();CHECK(locate.has_snapshot && locate.snapshot.match_sequence==seq && !locate.faulted); }
static void finish(void)
{
    mesh_locate_stop(&locate);for(unsigned i=0;i<200U && !mesh_locate_released(&locate);++i)tick();
    CHECK(mesh_locate_released(&locate));CHECK(locate.frozen);CHECK(mesh_locate_frozen_valid(&locate));CHECK(!peer.bound);
}
static void abandon(void)
{
    peer.control_fault=0U;peer.malformed_unbound=false;peer.read_fault=0U;peer.corrupt=0U;
    mesh_locate_close(&locate);for(unsigned i=0;i<100U && !mesh_locate_released(&locate);++i)tick();
    CHECK(mesh_locate_released(&locate));CHECK(!locate.frozen);CHECK(!peer.bound);
}
static void basic(void)
{
    reset();CHECK(mesh_locate_released(&locate));CHECK(mesh_locate_begin(&locate,&key,0U)==ESP_ERR_INVALID_ARG);
    CHECK(mesh_locate_begin(&locate,&key,50U)==ESP_OK);mesh_locate_stop(&locate);tick();CHECK(!peer.calls);
    CHECK(mesh_locate_released(&locate));begin();CHECK(mesh_locate_begin(&locate,&key,50U)==ESP_ERR_INVALID_STATE);
    sample(1U,0U);CHECK(locate.samples==1U && locate.history_count==1U);unsigned read=peer.reads,peek=peer.peeks;
    ticks(20U);CHECK(peer.reads==read && peer.peeks==peek);uint64_t age=0U;
    CHECK(mesh_locate_age_upper(&locate,peer.clock,&age));CHECK(age>=peer.copy-peer.live.last_match_us);
    finish();CHECK(locate.session==101U && locate.origin_session==50U);CHECK(locate.samples==1U);
    mesh_locate_t saved=locate;CHECK(mesh_locate_begin(&locate,&key,0U)!=ESP_OK);CHECK(!memcmp(&saved,&locate,sizeof(saved)));
    CHECK(mesh_locate_begin(&locate,&key,50U)==ESP_OK);CHECK(!locate.has_snapshot && !locate.history_count && !locate.frozen);
    mesh_locate_close(&locate);CHECK(mesh_locate_released(&locate));
    reset();begin();ticks(10U);CHECK(locate.has_snapshot && !locate.snapshot.match_sequence);finish();CHECK(!locate.samples);
    reset();key.mode=3U;key.value=UINT64_C(0xfedcba9876543210);key.pan.known=false;key.pan.value=0U;
    begin();sample(1U,106U);CHECK(locate.snapshot.mac_length==125U);finish();
    reset();key.value=0xfffeU;CHECK(mesh_locate_begin(&locate,&key,50U)!=ESP_OK);
    key.value=0xffffU;CHECK(mesh_locate_begin(&locate,&key,50U)!=ESP_OK);key.value=0x10000U;CHECK(mesh_locate_begin(&locate,&key,50U)!=ESP_OK);
    reset();key.pan.known=false;CHECK(mesh_locate_begin(&locate,&key,50U)!=ESP_OK);
    reset();key.pan.value=0U;begin();sample(1U,0U);finish();
}
static void lifecycle(void)
{
    reset();peer.old_peer=true;CHECK(mesh_locate_begin(&locate,&key,50U)==ESP_OK);ticks(5U);
    CHECK(mesh_locate_released(&locate)&&!peer.starts&&!peer.stops&&locate.error==ESP_ERR_NOT_SUPPORTED);
    reset();peer.legacy=true;CHECK(mesh_locate_begin(&locate,&key,50U)==ESP_OK);tick();CHECK(mesh_locate_released(&locate)&&!peer.starts);
    reset();peer.lost_start=true;begin();CHECK(!locate.accepted && peer.bound);sample(4U,0U);CHECK(peer.starts==1U && locate.coalesced_matches==3U);
    peer.stop_hold=3U;peer.release_hold=3U;peer.lost_release=true;finish();CHECK(peer.stops==3U&&peer.releases>=4U);
    reset();peer.reboot_start=true;begin();ticks(10U);CHECK(mesh_locate_released(&locate)&&!peer.bound&&!locate.has_snapshot);
    reset();peer.lost_start=true;begin();mesh_locate_stop(&locate);finish();CHECK(peer.starts==1U&&peer.stops==1U);
    reset();begin();sample(1U,0U);peer.bound=false;peer.epoch+=100U;ticks(10U);CHECK(mesh_locate_released(&locate)&&!locate.frozen&&locate.has_snapshot);
    reset();begin();row(1U,116U);ticks(1U);CHECK(locate.phase==MESH_LOCATE_READ);unsigned read=peer.reads;
    mesh_locate_stop(&locate);tick();CHECK(peer.stops==1U);CHECK(peer.reads-read<=2U);finish();
    reset();begin();sample(1U,0U);peer.live.error_code=ESP_FAIL;peer.running=false;finish();CHECK(locate.error==ESP_FAIL);
    reset();begin();sample(1U,0U);peer.final_fault=1U;mesh_locate_stop(&locate);ticks(30U);CHECK(locate.faulted&&!locate.frozen&&peer.bound);abandon();
    reset();begin();sample(1U,0U);peer.bound=false;peer.malformed_unbound=true;ticks(10U);CHECK(locate.faulted&&locate.request.session_id);abandon();
    reset();begin();row(1U,116U);ticks(1U);peer.lost_read=true;uint64_t anchor=locate.pending_anchor_us;ticks(30U);
    CHECK(locate.has_snapshot&&locate.snapshot_anchor_us==anchor&&!locate.faulted);abandon();
}
static void corruption(void)
{
    for(unsigned c=1U;c<=26U;++c){
        reset();begin();row(1U,1U);peer.corrupt=c;ticks(30U);
        CHECK(locate.faulted && !locate.has_snapshot && !locate.frozen);abandon();
    }
    for(unsigned c=1U;c<=9U;++c){
        reset();begin();peer.control_fault=c;ticks(10U);CHECK(locate.faulted && locate.request.session_id);abandon();
    }
    for(unsigned c=1U;c<=6U;++c){
        reset();begin();row(1U,1U);peer.read_fault=c;peer.read_remote_error=c==6U;ticks(30U);
        CHECK(locate.faulted && !locate.has_snapshot);abandon();
    }
    reset();begin();sample(1U,0U);peer.live.rssi--;mesh_locate_stop(&locate);ticks(30U);CHECK(locate.faulted&&locate.samples==1U);abandon();
    reset();begin();sample(1U,0U);row(2U,0U);peer.live.last_match_us=1U;ticks(30U);CHECK(locate.faulted&&locate.samples==1U);abandon();
    reset();begin();row(2U,0U);peer.corrupt=27U;ticks(30U);CHECK(locate.faulted&&!locate.has_snapshot);abandon();
    for(unsigned part=1U;part<=22U;++part){
        reset();begin();row(1U,116U);peer.read_fault=2U;peer.fault_part=part;ticks(30U);
        CHECK(locate.faulted&&!locate.has_snapshot&&!locate.history_count);abandon();
    }
    for(unsigned field=0U;field<6U;++field){
        reset();begin();sample(1U,1U);cerberus_mesh_locate_projection_t prior=locate.snapshot;
        if(field==0U)peer.live.last_match_us++;
        if(field==1U)peer.live.mac[2]++;
        if(field==2U)peer.live.mac[9]++;
        if(field==3U)peer.live.lqi++;
        if(field==4U){peer.live.flags&=(uint8_t)~CERBERUS_MESH_LOCATE_RSSI_VALID;peer.live.rssi=0;}
        if(field==5U){peer.live.flags&=(uint8_t)~CERBERUS_MESH_LOCATE_MATCH_TIME_VALID;peer.live.last_match_us=0U;}
        mesh_locate_stop(&locate);ticks(30U);CHECK(locate.faulted&&locate.samples==1U);
        CHECK(!memcmp(&prior,&locate.snapshot,sizeof(prior)));abandon();
    }
}
static void history_and_age(void)
{
    reset();begin();sample(3U,0U);
    for(uint32_t i=4U;i<=43U;++i){peer.copy+=1000U;sample(i,0U);}
    CHECK(locate.samples==41U && locate.coalesced_matches==2U && locate.history_count==32U);
    mesh_locate_sample_t h;
    for(uint8_t i=0U;i<32U;++i){CHECK(mesh_locate_history_get(&locate,i,&h));CHECK(h.sequence==12U+i);}
    CHECK(!mesh_locate_history_get(&locate,32U,&h));CHECK(!mesh_locate_history_get(&locate,0U,&locate.history[0]));
    uint64_t before;CHECK(mesh_locate_age_upper(&locate,peer.clock,&before));finish();
    uint64_t after;CHECK(mesh_locate_age_upper(&locate,peer.clock,&after));CHECK(after>=before);CHECK(locate.samples==41U);
    mesh_locate_t saved=locate;
    for(size_t offset=0U;offset<sizeof(locate);++offset){
        /* No arbitrary-memory trust: specifically corrupt every byte of the validated raw witness. */
        if(offset>=offsetof(mesh_locate_t,snapshot)+72U && offset<offsetof(mesh_locate_t,snapshot)+81U){
            locate=saved;((uint8_t*)&locate)[offset]^=0x80U;
            if(offset==offsetof(mesh_locate_t,snapshot)+79U)CHECK(!mesh_locate_frozen_valid(&locate));
        }
    }
    locate=saved;locate.history[locate.history_head].sequence=0U;CHECK(!mesh_locate_frozen_valid(&locate));
    locate=saved;locate.coalesced_matches++;CHECK(!mesh_locate_frozen_valid(&locate));
    locate=saved;locate.request.session_id=101U;CHECK(!mesh_locate_frozen_valid(&locate));
    locate=saved;locate.snapshot_age_us=UINT64_MAX;CHECK(!mesh_locate_age_upper(&locate,peer.clock,&after));CHECK(!after);
    locate=saved;CHECK(!mesh_locate_age_upper(&locate,-1,&after));CHECK(!mesh_locate_age_upper(&locate,0,&after));
    CHECK(!mesh_locate_age_upper(&locate,peer.clock,&locate.snapshot_age_us));
    reset();begin();peer.invalid_clock=true;sample(1U,0U);CHECK(!mesh_locate_age_upper(&locate,peer.clock,&after));finish();
    reset();begin();sample(1U,0U);peer.backward_clock=true;tick();CHECK(locate.clock_failed);CHECK(!mesh_locate_age_upper(&locate,peer.clock,&after));abandon();
    reset();begin();row(1U,0U);peer.live.flags|=CERBERUS_MESH_LOCATE_PROJECTION_CLOCK_INVALID;sample(1U,0U);
    CHECK(!mesh_locate_age_upper(&locate,peer.clock,&after));finish();
    reset();begin();sample(1U,0U);unsigned peeks=peer.peeks;
    peer.live.flags|=CERBERUS_MESH_LOCATE_PROJECTION_CLOCK_INVALID;peer.running=false;peer.stop_hold=20U;
    ticks(5U);CHECK(peer.peeks==peeks);CHECK(!locate.anchor_valid);
    CHECK(!mesh_locate_age_upper(&locate,peer.clock,&after));CHECK(!locate.snapshot_anchor_us&&!locate.snapshot_age_us);
    peer.stop_hold=0U;abandon();
}
static void bounds(void)
{
    for(unsigned payload=0U;payload<=116U;++payload){
        reset();begin();sample(1U,payload);CHECK(peer.reads==(81U+payload+8U)/9U);finish();
    }
    reset();begin();row(CERBERUS_MESH_LOCATE_SEQUENCE_MAX,0U);peer.live.flags|=CERBERUS_MESH_LOCATE_SEQUENCE_EXHAUSTED;
    peer.copy=peer.live.last_match_us+1000U;
    peer.live.callbacks=UINT32_MAX;peer.live.filtered_nonmatch=UINT32_MAX-CERBERUS_MESH_LOCATE_SEQUENCE_MAX;
    peer.live.flags|=CERBERUS_MESH_LOCATE_COUNTERS_SATURATED;ticks(20U);CHECK(locate.has_snapshot&&!locate.faulted);
    /* row() refreshes counters, so reestablish a saturated valid terminal partition. */
    peer.live.callbacks=UINT32_MAX;peer.live.filtered_nonmatch=UINT32_MAX-CERBERUS_MESH_LOCATE_SEQUENCE_MAX;
    finish();CHECK(locate.samples==1U);
    reset();peer.cost=8000;CHECK(mesh_locate_begin(&locate,&key,50U)==ESP_OK);tick();CHECK(!peer.starts&&peer.calls==2U);
    tick();CHECK(peer.starts==1U&&peer.calls==4U);abandon();
    for(unsigned count=0U;count<=40U;++count){
        CHECK(mesh_locate_test_fill(&locate,count));CHECK(locate.samples==count);CHECK(mesh_locate_frozen_valid(&locate));
        CHECK(mesh_locate_test_fill_options(&locate,count,3U,ESP_FAIL));
        CHECK(locate.error==ESP_FAIL && locate.coalesced_matches==2U*count);
        mesh_locate_t saved=locate;
        if(count){
            CHECK(locate.anchor_valid);locate.snapshot_anchor_us=UINT64_MAX;CHECK(!mesh_locate_frozen_valid(&locate));
            locate=saved;locate.snapshot_age_us=0U;CHECK(!mesh_locate_frozen_valid(&locate));
            locate=saved;locate.samples=0U;locate.history_count=0U;locate.history_head=0U;
            locate.coalesced_matches=locate.snapshot.match_sequence;CHECK(!mesh_locate_frozen_valid(&locate));
        }
    }
    printf("mesh Locate controller PASS %u checks; state=%zu history=%zu bytes\n",checks,sizeof(locate),sizeof(locate.history));
}
int main(void){basic();lifecycle();corruption();history_and_age();bounds();return 0;}
