#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "wifi_packet_count_scan.h"

static unsigned checks;
#define CHECK(x) do{++checks;assert(x);}while(0)
typedef struct {
    cerberus_wifi_packet_count_table_t table;
    cerberus_wifi_packet_count_target_t targets[16];
    cerberus_packet_count_begin_t begin;
    uint8_t bytes[128],state;
    uint64_t latest,session,deadline;
    uint32_t pin;
    uint16_t configured;
    unsigned starts,stops,releases,reads,begin_calls;
    bool held, reject, lost_begin, lost_start, hold_stop, corrupt, bind_fail, drain_ready;
    bool clock_invalid, peek_busy, begin_error, lost_release, peek_exhausted, expired, corrupt_status;
} fake_source_t;
typedef struct {
    fake_source_t source[2];int64_t clock;unsigned calls,binds,drains,delay,operations;
    uint8_t last_command,fail_command;unsigned failures;bool enforce_lease;
} fake_t;
static fake_t fake;
static wifi_packet_count_scan_t scan;
static int64_t clock_us(void *p){return ((fake_t*)p)->clock;}
static uint16_t mask(unsigned n){return n==16U?UINT16_MAX:(uint16_t)((1U<<n)-1U);}
static uint8_t first_channel(uint64_t channels)
{for(unsigned i=0;i<42U;++i)if(channels&(UINT64_C(1)<<i))return cerberus_wifi_packet_count_channel(i);return 0;}
static void reply(fake_source_t *f,uint64_t requested,cerberus_packet_count_control_t *r)
{
    memset(r,0,sizeof(*r));r->session_id=requested;
    if(!f->session||f->session!=requested){r->error_code=ESP_ERR_NOT_FOUND;return;}
    r->state=f->state;r->flags=CERBERUS_PACKET_COUNT_BOUND|(f->held?CERBERUS_PACKET_COUNT_HELD:0);
    r->count=f->begin.target_count;r->configured=f->configured;r->sequence=f->table.summary.revision;
    if(f->table.summary.count)r->flags|=CERBERUS_PACKET_COUNT_DATA_READY;
    if(f->clock_invalid)r->flags|=CERBERUS_PACKET_COUNT_CLOCK_INVALID;
    if(f->begin_error)r->error_code=ESP_FAIL;
    if(f->expired)r->error_code=ESP_ERR_TIMEOUT;
    if(f->state==CERBERUS_PACKET_COUNT_RUNNING){r->flags|=CERBERUS_PACKET_COUNT_RX_ACTIVE;r->channel=f->table.window_channel;}
    if(f->begin.record)r->flags|=CERBERUS_PACKET_COUNT_RECORDING;
}
static void observe(fake_source_t *f,int64_t now)
{
    uint8_t frame[24]={0x08,0x01};
    memcpy(frame+4,f->targets[0].mac,6);
    memcpy(frame+10,f->targets[f->begin.target_count>1U?1U:0U].mac,6);
    memcpy(frame+16,f->targets[0].mac,6);
    CHECK(cerberus_wifi_packet_count_observe(&f->table,frame,sizeof(frame),CERBERUS_WIFI_PACKET_COUNT_DATA,
        true,f->table.window_channel,-45,now));
}
static esp_err_t exchange(void *context,unsigned source,uint8_t cmd,
    const void *q,size_t qn,void *out,size_t rn)
{
    fake_t *p=context;fake_source_t *f=&p->source[source];
    p->calls+=source==1U&&(cmd==161U||cmd==162U)?2U:1U;p->clock+=p->delay;
    ++p->operations;p->last_command=cmd;
    CHECK(source<2U);memset(out,0,rn);
    /* Model only the native lease boundary: expiry precedes renewal, and a
     * stopped exact context is retained for reads/release without rearming. */
    if(p->enforce_lease&&f->held&&f->deadline&&(uint64_t)p->clock>=f->deadline){
        if(f->table.summary.count)cerberus_wifi_packet_count_finish(&f->table,p->clock);
        f->held=false;f->state=CERBERUS_PACKET_COUNT_ERROR;f->expired=true;
    }
    if(p->failures&&cmd==p->fail_command){--p->failures;return ESP_ERR_TIMEOUT;}
    if(cmd==CERBERUS_PACKET_COUNT_COMMAND_READ){
        CHECK(qn==sizeof(cerberus_packet_count_read_t)&&rn==sizeof(cerberus_packet_count_chunk_t));
        const cerberus_packet_count_read_t *r=q;cerberus_packet_count_chunk_t *v=out;
        v->session_id=r->session_id;v->part=r->part;
        if(r->session_id!=f->session||r->pin!=f->pin||r->part>=16U)return ESP_OK;
        v->session_id=f->session;v->pin=f->pin;v->part=r->part;memcpy(v->data,f->bytes+r->part*8U,8);
        if(f->corrupt){v->session_id++;f->corrupt=false;}++f->reads;return ESP_OK;
    }
    CHECK(rn==sizeof(cerberus_packet_count_control_t));cerberus_packet_count_control_t *r=out;
    if(cmd==CERBERUS_PACKET_COUNT_COMMAND_INFO){CHECK(!qn);r->session_id=f->latest;return ESP_OK;}
    CHECK(qn>=8U);uint64_t session;memcpy(&session,q,8);
    if(cmd==CERBERUS_PACKET_COUNT_COMMAND_BEGIN){
        CHECK(qn==sizeof(f->begin));++f->begin_calls;r->session_id=session;
        if(f->reject){r->error_code=ESP_ERR_NO_MEM;return ESP_OK;}
        CHECK(!f->session&&session==f->latest+1U);f->latest=f->session=session;
        f->begin=*(const cerberus_packet_count_begin_t*)q;f->held=true;f->state=CERBERUS_PACKET_COUNT_STAGING;
        f->deadline=(uint64_t)p->clock+(uint64_t)CERBERUS_PACKET_COUNT_LEASE_MS*1000U;
        memset(&f->table,0,sizeof(f->table));f->configured=0;
        reply(f,session,r);if(f->lost_begin){f->lost_begin=false;return ESP_ERR_TIMEOUT;}return ESP_OK;
    }
    if(session!=f->session){
        if(cmd==CERBERUS_PACKET_COUNT_COMMAND_RELEASE&&session==f->latest&&!f->session){r->session_id=session;return ESP_OK;}
        reply(f,session,r);return ESP_OK;
    }
    if(cmd==CERBERUS_PACKET_COUNT_COMMAND_TARGET){
        const cerberus_packet_count_target_t *t=q;CHECK(qn==sizeof(*t)&&t->index<f->begin.target_count);
        f->targets[t->index]=(cerberus_wifi_packet_count_target_t){.role=t->role,.channel_hint=t->channel_hint};
        memcpy(f->targets[t->index].mac,t->mac,6);f->configured|=(uint16_t)(1U<<t->index);
    }else if(cmd==CERBERUS_PACKET_COUNT_COMMAND_START){
        CHECK(f->state==CERBERUS_PACKET_COUNT_STAGING&&f->configured==mask(f->begin.target_count));++f->starts;
        CHECK(cerberus_wifi_packet_count_init(&f->table,f->targets,f->begin.target_count,f->begin.channel_mask,f->begin.dwell_ms));
        uint8_t channel=first_channel(f->begin.channel_mask);
        cerberus_wifi_packet_count_attempt(&f->table,channel,true);
        CHECK(cerberus_wifi_packet_count_window_begin(&f->table,channel,p->clock));
        f->state=CERBERUS_PACKET_COUNT_RUNNING;observe(f,p->clock);
        reply(f,session,r);if(f->lost_start){f->lost_start=false;return ESP_ERR_TIMEOUT;}return ESP_OK;
    }else if(cmd==CERBERUS_PACKET_COUNT_COMMAND_STOP){
        ++f->stops;
        if(!f->hold_stop){if(f->state>=CERBERUS_PACKET_COUNT_RUNNING)cerberus_wifi_packet_count_finish(&f->table,p->clock);
            f->held=false;f->state=CERBERUS_PACKET_COUNT_COMPLETE;}
    }else if(cmd==CERBERUS_PACKET_COUNT_COMMAND_RELEASE){
        CHECK(!f->held);++f->releases;f->session=0;r->session_id=session;
        if(f->lost_release){f->lost_release=false;return ESP_ERR_TIMEOUT;}return ESP_OK;
    }else if(cmd==CERBERUS_PACKET_COUNT_COMMAND_PEEK){
        const cerberus_packet_count_peek_t *v=q;CHECK(qn==sizeof(*v));memset(f->bytes,0,sizeof(f->bytes));
        if(f->peek_busy||f->peek_exhausted){reply(f,session,r);r->sequence=0;
            r->error_code=f->peek_busy?ESP_ERR_NOT_FINISHED:ESP_ERR_INVALID_STATE;f->peek_busy=false;return ESP_OK;}
        cerberus_packet_count_record_stats_t raw={0};bool okay=false;
        if(v->kind==0)okay=cerberus_packet_count_encode_summary(&f->table,p->clock,f->state,
            f->table.window_channel,f->begin.record,0,f->configured,&raw,f->bytes);
        else if(v->kind==1)okay=cerberus_packet_count_encode_row(&f->table,v->index,p->clock,f->bytes);
        else if(v->kind==2)okay=cerberus_packet_count_encode_channel(&f->table,v->index,p->clock,f->bytes);
        CHECK(okay);reply(f,session,r);f->pin+=2U;r->sequence=f->pin;return ESP_OK;
    }else {CHECK(cmd==CERBERUS_PACKET_COUNT_COMMAND_STATUS);
        if(f->held&&!f->expired)f->deadline=(uint64_t)p->clock+(uint64_t)CERBERUS_PACKET_COUNT_LEASE_MS*1000U;}
    reply(f,session,r);
    if(cmd==CERBERUS_PACKET_COUNT_COMMAND_STATUS&&f->corrupt_status){++r->session_id;f->corrupt_status=false;}
    return ESP_OK;
}
static esp_err_t record_bind(void *p,unsigned source,uint64_t session)
{fake_t *f=p;CHECK(session==f->source[source].session);++f->binds;return f->source[source].bind_fail?ESP_FAIL:ESP_OK;}
static bool record_drained(void *p,unsigned source,uint64_t session)
{fake_t *f=p;CHECK(session==f->source[source].latest&&(!f->source[source].session||!f->source[source].held));++f->drains;return f->source[source].drain_ready;}
static const wifi_packet_count_transport_t transport={exchange,clock_us,record_bind,record_drained,&fake};
static void tick(void)
{fake.calls=0;fake.clock+=20000;wifi_packet_count_scan_tick(&scan,(uint64_t)fake.clock/1000U,&transport);CHECK(fake.calls<=4U);}
static void ticks(unsigned n){while(n--)tick();}
static void slow_tick(void)
{fake.calls=0;fake.operations=0;fake.clock+=1200000;wifi_packet_count_scan_tick(&scan,(uint64_t)fake.clock/1000U,&transport);CHECK(fake.calls<=2U&&fake.operations<=1U);}
static void until_released(void)
{for(unsigned i=0;i<2000U&&!wifi_packet_count_scan_released(&scan);++i)tick();CHECK(wifi_packet_count_scan_released(&scan));}
static void reset(void)
{
    memset(&fake,0,sizeof(fake));fake.clock=1000000;
    for(unsigned i=0;i<2U;++i){fake.source[i].latest=100U+i*100U;fake.source[i].pin=0x7fffffffU;fake.source[i].drain_ready=true;}
    wifi_packet_count_scan_init(&scan);
    for(unsigned i=0;i<2U;++i){wifi_packet_count_selection_t s={.target={.role=(uint8_t)(i+1U),.channel_hint=(uint8_t)(i?6:1),.mac={2,0,0,0,0,(uint8_t)(i+1U)}},.discovery_source=1,.discovery_publication=22};
        snprintf(s.label,sizeof(s.label),"TARGET%u",i);CHECK(wifi_packet_count_scan_add(&scan,&s)==ESP_OK);}
}
static void basic(void)
{
    reset();wifi_packet_count_selection_t duplicate=scan.selected[0];duplicate.target.channel_hint=11;
    CHECK(wifi_packet_count_scan_add(&scan,&duplicate)==ESP_OK&&scan.count==2U&&scan.selected[0].target.channel_hint==1U);
    CHECK(wifi_packet_count_scan_plan(&scan,WIFI_PACKET_COUNT_SELECTED,0)==0x21U);
    CHECK(wifi_packet_count_scan_begin(&scan,1U,100U,false)==ESP_OK);ticks(160);
    CHECK(!fake.source[0].starts&&fake.source[1].starts==1U);
    CHECK(scan.sources[1].published.valid&&!scan.frozen);
    wifi_packet_count_row_totals_t t;bool sat;
    CHECK(wifi_packet_count_scan_row(&scan,0,&t,&sat)&&t.observations==1&&t.addressed==1&&!t.transmitted&&!sat);
    CHECK(wifi_packet_count_scan_row(&scan,1,&t,&sat)&&t.observations==1&&t.transmitted==1&&!t.addressed);
    uint64_t age;CHECK(!wifi_packet_count_scan_signal_age(&scan,1,0,fake.clock,&age));
    CHECK(wifi_packet_count_scan_signal_age(&scan,1,1,fake.clock,&age));
    wifi_packet_count_scan_stop(&scan);until_released();CHECK(scan.frozen&&fake.source[1].releases==1U);
    CHECK(scan.sources[1].published.summary.matched_frames==1U);
    ticks(50);CHECK(fake.source[1].starts==1U);
}
static void dual_and_retune(void)
{
    reset();uint64_t channels=wifi_packet_count_scan_plan(&scan,WIFI_PACKET_COUNT_ALL,0);
    CHECK(wifi_packet_count_scan_begin(&scan,channels,100,false)==ESP_OK);ticks(400);
    CHECK(fake.source[0].starts==1&&fake.source[1].starts==1);
    CHECK(!(fake.source[0].begin.channel_mask&fake.source[1].begin.channel_mask));
    CHECK((fake.source[0].begin.channel_mask|fake.source[1].begin.channel_mask)==channels);
    CHECK(!(fake.source[0].begin.channel_mask>>14U));
    wifi_packet_count_row_totals_t t;bool sat;
    CHECK(wifi_packet_count_scan_row(&scan,0,&t,&sat)&&t.observations==2U);
    CHECK(wifi_packet_count_scan_retune(&scan,cerberus_wifi_packet_count_channel_bit(11))==ESP_OK);
    ticks(800);CHECK(scan.interruptions==1U&&scan.segments==2U&&fake.source[1].starts==2U);
    CHECK(wifi_packet_count_scan_row(&scan,0,&t,&sat)&&t.observations==3U);
    wifi_packet_count_scan_stop(&scan);until_released();CHECK(scan.frozen);
}
static void uncertainty(void)
{
    reset();fake.source[0].reject=true;
    CHECK(wifi_packet_count_scan_begin(&scan,0x21,100,false)==ESP_OK);ticks(250);
    CHECK(scan.fallback&&scan.s3_admission_error==ESP_ERR_NO_MEM&&fake.source[1].begin.channel_mask==0x21);
    wifi_packet_count_scan_close(&scan);until_released();
    reset();fake.source[0].lost_begin=fake.source[0].lost_start=true;
    CHECK(wifi_packet_count_scan_begin(&scan,0x21,100,false)==ESP_OK);ticks(300);
    CHECK(!scan.fallback&&fake.source[0].begin_calls==1&&fake.source[0].starts==1&&fake.source[1].starts==1);
    fake.source[0].hold_stop=true;wifi_packet_count_scan_stop(&scan);ticks(700);
    CHECK(!wifi_packet_count_scan_released(&scan)&&!fake.source[0].releases&&fake.source[1].releases==1);
    fake.source[0].hold_stop=false;until_released();CHECK(scan.frozen);
    reset();CHECK(wifi_packet_count_scan_begin(&scan,1,100,false)==ESP_OK);ticks(20);
    fake.source[1].corrupt=true;ticks(200);CHECK(scan.sources[1].faulted&&!wifi_packet_count_scan_released(&scan));
    wifi_packet_count_scan_close(&scan);until_released();CHECK(!scan.frozen);
}
static void recording(void)
{
    reset();fake.source[1].drain_ready=false;
    CHECK(wifi_packet_count_scan_begin(&scan,1,100,true)==ESP_OK);ticks(150);CHECK(fake.binds==1);
    wifi_packet_count_scan_stop(&scan);ticks(250);CHECK(!wifi_packet_count_scan_released(&scan)&&fake.drains&&!fake.source[1].releases);
    fake.source[1].drain_ready=true;until_released();CHECK(scan.frozen);
    reset();fake.source[1].bind_fail=true;
    CHECK(wifi_packet_count_scan_begin(&scan,1,100,true)==ESP_OK);ticks(150);
    CHECK(!fake.source[1].starts&&wifi_packet_count_scan_released(&scan)&&!scan.frozen);
    reset();CHECK(wifi_packet_count_scan_begin(&scan,1,100,false)==ESP_OK);wifi_packet_count_scan_close(&scan);until_released();CHECK(!fake.calls&&!fake.source[1].begin_calls);
}
static void lifecycle_edges(void)
{
    reset();CHECK(wifi_packet_count_scan_begin(&scan,1,100,true)==ESP_OK);ticks(150);
    fake.source[1].drain_ready=false;fake.source[1].session=0;ticks(100);
    CHECK(!scan.sources[1].request.session_id&&scan.sources[1].record_bound&&!wifi_packet_count_scan_released(&scan)&&fake.drains);
    fake.source[1].drain_ready=true;until_released();CHECK(!scan.frozen&&!fake.source[1].releases);
    reset();fake.source[1].peek_busy=true;CHECK(wifi_packet_count_scan_begin(&scan,1,100,false)==ESP_OK);ticks(160);
    CHECK(!scan.sources[1].faulted&&scan.sources[1].published.valid);
    uint64_t age;CHECK(wifi_packet_count_scan_signal_age(&scan,1,1,fake.clock,&age));
    fake.source[1].clock_invalid=true;
    for(unsigned i=0;i<60U&&scan.sources[1].published.row_anchor_valid;++i)tick();
    CHECK(!wifi_packet_count_scan_signal_age(&scan,1,1,fake.clock,&age));
    wifi_packet_count_scan_close(&scan);until_released();
    reset();CHECK(wifi_packet_count_scan_begin(&scan,1,100,false)==ESP_OK);ticks(15);
    fake.source[1].table.summary.revision=0x80000020U;ticks(200);
    CHECK(!scan.sources[1].faulted&&scan.sources[1].published.summary.revision>=0x80000020U);
    wifi_packet_count_scan_stop(&scan);until_released();CHECK(scan.frozen);
    reset();fake.source[0].begin_error=true;CHECK(wifi_packet_count_scan_begin(&scan,0x21U,100,false)==ESP_OK);ticks(200);
    CHECK(!scan.fallback&&!fake.source[0].starts&&fake.source[1].starts==1U&&fake.source[1].begin.channel_mask==0x20U);
    wifi_packet_count_scan_close(&scan);until_released();
    reset();CHECK(wifi_packet_count_scan_begin(&scan,1,100,false)==ESP_OK);ticks(150);
    fake.source[1].lost_release=true;wifi_packet_count_scan_stop(&scan);until_released();CHECK(scan.frozen&&fake.source[1].releases==1U);
    reset();fake.source[1].peek_exhausted=true;CHECK(wifi_packet_count_scan_begin(&scan,1,100,false)==ESP_OK);ticks(100);
    CHECK(scan.sources[1].faulted&&!wifi_packet_count_scan_released(&scan));
    wifi_packet_count_scan_close(&scan);until_released();
    reset();fake.delay=9000;CHECK(wifi_packet_count_scan_begin(&scan,0x21U,100,false)==ESP_OK);
    for(unsigned i=0;i<10U;++i){tick();CHECK(fake.calls<=2U);}
    wifi_packet_count_scan_close(&scan);until_released();
}
static void until_pin(unsigned kind,unsigned index)
{
    fake.delay=9000; /* One transport operation per tick exposes pin boundaries. */
    for(unsigned i=0;i<1000U;++i){
        wifi_packet_count_source_t *v=&scan.sources[1];
        if(v->phase==WIFI_COUNT_READ&&v->kind==kind&&v->index==index&&v->part==0U)return;
        tick();
    }
    CHECK(false);
}
static void pinned_interleavings(void)
{
    reset();CHECK(wifi_packet_count_scan_begin(&scan,1,100,false)==ESP_OK);
    until_pin(CERBERUS_PACKET_COUNT_READ_SUMMARY,0);
    uint32_t pinned=scan.sources[1].projection_floor;
    observe(&fake.source[1],fake.clock);fake.clock+=1100000;fake.delay=9000;tick();
    CHECK(scan.sources[1].revision_floor>pinned&&scan.sources[1].projection_floor==pinned);
    fake.delay=0;ticks(200);CHECK(!scan.sources[1].faulted&&scan.sources[1].published.valid);
    wifi_packet_count_scan_stop(&scan);until_released();CHECK(scan.frozen);
    reset();CHECK(wifi_packet_count_scan_begin(&scan,1,100,false)==ESP_OK);
    until_pin(CERBERUS_PACKET_COUNT_READ_SUMMARY,0);
    CHECK(scan.sources[1].projection_floor>0U);
    cerberus_packet_count_put(fake.source[1].bytes+16U,scan.sources[1].projection_floor-1U,4U);
    ticks(100);CHECK(scan.sources[1].faulted);wifi_packet_count_scan_close(&scan);until_released();
    reset();CHECK(wifi_packet_count_scan_begin(&scan,1,100,false)==ESP_OK);ticks(150);
    uint64_t age;CHECK(wifi_packet_count_scan_signal_age(&scan,1,1,fake.clock,&age));
    until_pin(CERBERUS_PACKET_COUNT_READ_ROW,1);
    fake.source[1].clock_invalid=true;fake.clock+=1100000;fake.delay=9000;tick();
    CHECK(scan.sources[1].clock_invalid&&!scan.sources[1].published.row_anchor_valid&&!scan.sources[1].staging.row_anchor_valid);
    fake.delay=0;ticks(200);CHECK(scan.sources[1].published.valid&&!scan.sources[1].published.row_anchor_valid);
    CHECK(!wifi_packet_count_scan_signal_age(&scan,1,1,fake.clock,&age));
    /* An older projection or a later inconsistent clear cannot restore age. */
    fake.source[1].clock_invalid=false;ticks(150);CHECK(!wifi_packet_count_scan_signal_age(&scan,1,1,fake.clock,&age));
    wifi_packet_count_scan_close(&scan);until_released();
    reset();CHECK(wifi_packet_count_scan_begin(&scan,1,100,false)==ESP_OK);ticks(150);
    int64_t prior=fake.clock;fake.clock-=100000;tick();CHECK(scan.sources[1].clock_invalid);
    fake.clock=prior+200000;until_released();CHECK(!wifi_packet_count_scan_signal_age(&scan,1,1,fake.clock,&age));
}
static void slow_tick_progress(void)
{
    reset();fake.delay=9000;fake.enforce_lease=true;
    CHECK(wifi_packet_count_scan_remove(&scan,1)==ESP_OK);
    CHECK(wifi_packet_count_scan_begin(&scan,1,100,false)==ESP_OK);
    for(unsigned i=0;i<20U&&!fake.source[1].starts;++i)slow_tick();
    CHECK(fake.source[1].starts==1U);
    for(unsigned i=0;i<200U&&!scan.sources[1].published.valid;++i)slow_tick();
    CHECK(scan.sources[1].published.valid&&!scan.sources[1].faulted);
    CHECK(!fake.source[1].expired);
    CHECK(scan.sources[1].published.rows[0].observations==1U);
    wifi_packet_count_scan_stop(&scan);
    for(unsigned i=0;i<200U&&!wifi_packet_count_scan_released(&scan);++i)slow_tick();
    CHECK(wifi_packet_count_scan_released(&scan)&&scan.frozen);

    reset();fake.delay=9000;fake.enforce_lease=true;
    CHECK(wifi_packet_count_scan_begin(&scan,0x21U,100,false)==ESP_OK);
    for(unsigned i=0;i<450U&&(!scan.sources[0].published.valid||!scan.sources[1].published.valid);++i)slow_tick();
    CHECK(scan.sources[0].published.valid&&scan.sources[1].published.valid);
    CHECK(fake.source[0].starts==1U&&fake.source[1].starts==1U);
    CHECK(!fake.source[0].expired&&!fake.source[1].expired);
    wifi_packet_count_scan_close(&scan);until_released();
}
static void slow_tick_safety(void)
{
    reset();fake.delay=9000;fake.enforce_lease=true;
    fake.source[1].lost_begin=fake.source[1].lost_start=true;
    CHECK(wifi_packet_count_scan_begin(&scan,1,100,false)==ESP_OK);
    for(unsigned i=0;i<250U&&!scan.sources[1].published.valid;++i)slow_tick();
    CHECK(scan.sources[1].published.valid&&!scan.sources[1].faulted&&!fake.source[1].expired);
    CHECK(fake.source[1].begin_calls==1U&&fake.source[1].starts==1U);
    wifi_packet_count_scan_close(&scan);slow_tick();
    CHECK(fake.last_command==CERBERUS_PACKET_COUNT_COMMAND_STOP);
    slow_tick();CHECK(fake.last_command==CERBERUS_PACKET_COUNT_COMMAND_STATUS);
    slow_tick();CHECK(fake.last_command==CERBERUS_PACKET_COUNT_COMMAND_RELEASE);
    CHECK(wifi_packet_count_scan_released(&scan));

    reset();CHECK(wifi_packet_count_scan_begin(&scan,1,100,false)==ESP_OK);
    until_pin(CERBERUS_PACKET_COUNT_READ_SUMMARY,0);fake.enforce_lease=true;
    wifi_packet_count_source_t *v=&scan.sources[1];
    slow_tick();CHECK(fake.last_command==CERBERUS_PACKET_COUNT_COMMAND_STATUS&&v->progress_due);
    uint16_t part=v->part;unsigned reads=fake.source[1].reads;
    fake.clock+=(int64_t)WIFI_PACKET_COUNT_PROGRESS_MAX_MS*1000;
    slow_tick();CHECK(fake.last_command==CERBERUS_PACKET_COUNT_COMMAND_STATUS&&v->part==part);
    CHECK(!fake.source[1].expired&&fake.source[1].reads==reads);

    /* A failed progress attempt consumes its credit and preserves backoff.
     * A failed STATUS must never grant credit or permit a read past it. */
    fake.fail_command=CERBERUS_PACKET_COUNT_COMMAND_READ;fake.failures=1;
    slow_tick();CHECK(fake.last_command==CERBERUS_PACKET_COUNT_COMMAND_READ&&!v->progress_due&&v->part==part);
    tick();CHECK(!fake.calls&&v->part==part);
    fake.fail_command=CERBERUS_PACKET_COUNT_COMMAND_STATUS;fake.failures=1;
    slow_tick();CHECK(fake.last_command==CERBERUS_PACKET_COUNT_COMMAND_STATUS&&!v->progress_due&&v->part==part);
    tick();CHECK(!fake.calls&&v->part==part);
    slow_tick();CHECK(fake.last_command==CERBERUS_PACKET_COUNT_COMMAND_STATUS&&v->progress_due);
    slow_tick();CHECK(fake.last_command==CERBERUS_PACKET_COUNT_COMMAND_READ&&v->part==part+1U);

    slow_tick();CHECK(fake.last_command==CERBERUS_PACKET_COUNT_COMMAND_STATUS&&v->progress_due);
    part=v->part;fake.clock+=(int64_t)CERBERUS_PACKET_COUNT_LEASE_MS*1000;
    slow_tick();CHECK(fake.last_command==CERBERUS_PACKET_COUNT_COMMAND_STATUS&&v->part==part);
    CHECK(fake.source[1].expired&&!fake.source[1].held&&v->error==ESP_ERR_TIMEOUT);
    CHECK(fake.source[1].starts==1U);
    wifi_packet_count_scan_close(&scan);slow_tick();
    CHECK(fake.last_command==CERBERUS_PACKET_COUNT_COMMAND_RELEASE&&wifi_packet_count_scan_released(&scan));

    /* Stop has priority even when an exact STATUS granted progress credit. */
    reset();CHECK(wifi_packet_count_scan_begin(&scan,1,100,false)==ESP_OK);
    until_pin(CERBERUS_PACKET_COUNT_READ_SUMMARY,0);slow_tick();CHECK(scan.sources[1].progress_due);
    wifi_packet_count_scan_stop(&scan);slow_tick();CHECK(fake.last_command==CERBERUS_PACKET_COUNT_COMMAND_STOP);
    wifi_packet_count_scan_close(&scan);slow_tick();CHECK(fake.last_command==CERBERUS_PACKET_COUNT_COMMAND_STATUS);
    slow_tick();CHECK(fake.last_command==CERBERUS_PACKET_COUNT_COMMAND_RELEASE);
    CHECK(wifi_packet_count_scan_released(&scan));

    /* Progress credit requires an exact accepted STATUS, and never turns an
     * earlier reply into authority over a replacement native session. */
    reset();CHECK(wifi_packet_count_scan_begin(&scan,1,100,false)==ESP_OK);
    until_pin(CERBERUS_PACKET_COUNT_READ_SUMMARY,0);fake.source[1].corrupt_status=true;
    part=scan.sources[1].part;slow_tick();
    CHECK(fake.last_command==CERBERUS_PACKET_COUNT_COMMAND_STATUS&&scan.sources[1].faulted);
    CHECK(!scan.sources[1].progress_due&&scan.sources[1].part==part);
    wifi_packet_count_scan_close(&scan);until_released();

    reset();CHECK(wifi_packet_count_scan_begin(&scan,1,100,false)==ESP_OK);
    until_pin(CERBERUS_PACKET_COUNT_READ_SUMMARY,0);slow_tick();CHECK(scan.sources[1].progress_due);
    ++fake.source[1].session;++fake.source[1].latest;part=scan.sources[1].part;
    slow_tick();CHECK(fake.last_command==CERBERUS_PACKET_COUNT_COMMAND_READ&&scan.sources[1].faulted);
    CHECK(!scan.sources[1].progress_due&&scan.sources[1].part==part&&fake.source[1].held);
    wifi_packet_count_scan_close(&scan);slow_tick();
    CHECK(wifi_packet_count_scan_released(&scan)&&fake.source[1].held&&!fake.source[1].releases);
}
int main(void)
{basic();dual_and_retune();uncertainty();recording();lifecycle_edges();pinned_interleavings();slow_tick_progress();slow_tick_safety();printf("Packet Count dual controller: %u checks PASS; workspace %zu bytes\n",checks,sizeof(scan));return 0;}
