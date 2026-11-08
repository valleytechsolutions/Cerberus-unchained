#include "wifi_ap_survey_session.h"
#include "wifi_ap_survey_model.h"
#include "wifi_capture_plan.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"line %u: %s\n",__LINE__,#x); exit(1); } } while (0)
enum { REMOTE_NOT_FOUND=-1001, REMOTE_INVALID=-1002, REMOTE_TIMEOUT=-1003, TRANSPORT_LOST=-1004 };

typedef struct {
    cerberus_ap_survey_table_t live;
    cerberus_ap_survey_snapshot_t pinned;
    cerberus_ap_survey_start_t accepted;
    uint64_t epoch, lease_us, retention_us, anchor_us;
    uint64_t last_operation_us, max_read_gap_us, previous_read_us;
    unsigned commands[8], starts_accepted, snapshots, read_steps, cancels, published;
    unsigned step, stop_ticks, stop_delay, read_fail_step, malformed_images;
    unsigned lost_command, lost_count;
    int32_t warning, terminal_error;
    bool bound, held, stop, release, foreign, lost_before, bad_control, stall_read;
    bool skip_lease, mutate_foreign_after_lost_start, reject_publish;
} receiver_t;

static receiver_t receivers[2];
static cerberus_ap_survey_session_t session;
static cerberus_ap_survey_model_t model;
static cerberus_ap_survey_session_ops_t ops;
static uint64_t now_us, tick_number;
static unsigned operations, total_operations, publications;
static uint8_t last_source, last_command;
static bool fail_bad_publish;

static void operation(unsigned slot, uint8_t command)
{
    ++operations; ++total_operations;
    CHECK(operations==1);
    last_source=(uint8_t)(slot+1U); last_command=command;
    receivers[slot].last_operation_us=now_us;
}

static void observe(unsigned slot)
{
    receiver_t *r=&receivers[slot];
    if(!r->held||r->stop||!r->live.accepting)return;
    uint8_t frame[42]={0};frame[0]=0x80;memset(frame+4,255,6);
    uint8_t address[6]={2,1,2,3,4,(uint8_t)(slot+1U)};
    memcpy(frame+10,address,6);memcpy(frame+16,address,6);
    frame[34]=0x10;frame[36]=0;frame[37]=4;
    memcpy(frame+38,slot?"C5AP":"S3AP",4);
    cerberus_ap_survey_decoded_t decoded;
    CHECK(cerberus_ap_survey_decode(frame,sizeof(frame),slot?36:1,-40-(int)slot,
        now_us,&decoded)==CERBERUS_AP_SURVEY_AP);
    CHECK(cerberus_ap_survey_commit(&r->live,r->accepted.session_id,1,slot?36:1,now_us,&decoded));
}

static void advance_backend(unsigned slot)
{
    receiver_t *r=&receivers[slot];
    if(!r->bound)return;
    if(r->held&&!r->stop&&!r->skip_lease&&now_us>=r->lease_us){
        r->stop=true;r->terminal_error=REMOTE_TIMEOUT;r->stop_ticks=0;
        (void)cerberus_ap_survey_disarm(&r->live,r->accepted.session_id,1,now_us);
    }
    if(r->held&&r->stop){
        if(r->stop_ticks)--r->stop_ticks;
        else {r->held=false;r->retention_us=now_us+UINT64_C(30000000);}
    }
    if(!r->held&&(r->release||now_us>=r->retention_us))r->bound=false;
    observe(slot);
}

static void reply_control(receiver_t *r,uint64_t requested,bool info,
    int32_t error,cerberus_ap_survey_control_t *out)
{
    memset(out,0,sizeof(*out));out->session_id=info?r->epoch:requested;out->error_code=error;
    if(info&&r->foreign){out->state=CERBERUS_AP_SURVEY_QUEUED;out->flags=CERBERUS_AP_SURVEY_HELD;return;}
    if(!r->bound||(!info&&requested!=r->accepted.session_id))return;
    out->session_id=r->accepted.session_id;
    out->flags=CERBERUS_AP_SURVEY_BOUND|CERBERUS_AP_SURVEY_DATA_READY;
    if(r->held)out->flags|=CERBERUS_AP_SURVEY_HELD;
    if(r->live.accepting)out->flags|=CERBERUS_AP_SURVEY_RECEIVING;
    out->state=r->held?(r->stop?CERBERUS_AP_SURVEY_STOPPING:CERBERUS_AP_SURVEY_RUNNING):
        (r->terminal_error?CERBERUS_AP_SURVEY_ERROR:CERBERUS_AP_SURVEY_COMPLETE);
    out->revision=r->live.live.revision;out->ap_count=r->live.live.ap_count;
    out->relation_count=r->live.live.relation_count;
    out->channel=r->live.accepting?(r->live.live.channel):0;
    if(!error)out->error_code=r->terminal_error?r->terminal_error:r->warning;
}

static int32_t exchange(void *context,uint8_t source,uint8_t command,
    const void *request,size_t length,cerberus_ap_survey_control_t *out)
{
    CHECK(context==&model&&source>=1&&source<=2);
    CHECK(length==cerberus_ap_survey_request_bytes(command));
    CHECK((length&&!request)==false);
    unsigned slot=source-1U;receiver_t *r=&receivers[slot];
    operation(slot,command);++r->commands[command-CERBERUS_AP_SURVEY_COMMAND_INFO];
    uint64_t requested=0;if(length)memcpy(&requested,request,8);
    bool lose=r->lost_count&&r->lost_command==command;
    if(lose)--r->lost_count;
    if(lose&&r->lost_before){memset(out,0,sizeof(*out));return TRANSPORT_LOST;}
    int32_t error=0;
    if(command==CERBERUS_AP_SURVEY_COMMAND_START){
        cerberus_ap_survey_start_t q;memcpy(&q,request,sizeof(q));
        if(r->bound&&q.session_id==r->accepted.session_id){
            if(memcmp(&q,&r->accepted,sizeof(q)))error=REMOTE_INVALID;
        }else if(r->foreign||r->bound||r->epoch==UINT64_MAX||q.session_id!=r->epoch+1U||!cerberus_ap_survey_start_valid(&q)){
            error=REMOTE_INVALID;
        }else{
            r->epoch=q.session_id;r->accepted=q;r->bound=r->held=true;r->stop=r->release=false;
            r->terminal_error=0;r->stop_ticks=0;r->lease_us=now_us+UINT64_C(10000000);
            CHECK(cerberus_ap_survey_begin(&r->live,q.session_id,q.channel_mask,q.channel_mask));
            CHECK(cerberus_ap_survey_note_channel(&r->live,slot?36:1,true));
            CHECK(cerberus_ap_survey_arm(&r->live,q.session_id,1,slot?36:1,now_us));
            ++r->starts_accepted;observe(slot);
        }
    }else if(command!=CERBERUS_AP_SURVEY_COMMAND_INFO){
        if(!r->bound||requested!=r->accepted.session_id){
            error=command==CERBERUS_AP_SURVEY_COMMAND_RELEASE&&requested==r->epoch?0:REMOTE_NOT_FOUND;
        }else if(command==CERBERUS_AP_SURVEY_COMMAND_STOP||command==CERBERUS_AP_SURVEY_COMMAND_RELEASE){
            if(!r->stop){r->stop=true;r->stop_ticks=r->stop_delay;
                CHECK(cerberus_ap_survey_disarm(&r->live,requested,1,now_us));}
            if(command==CERBERUS_AP_SURVEY_COMMAND_RELEASE)r->release=true;
        }else if(command==CERBERUS_AP_SURVEY_COMMAND_RENEW&&r->held&&!r->stop){
            r->lease_us=now_us+UINT64_C(10000000);
        }
        if(r->bound&&!r->held&&!r->release)r->retention_us=now_us+UINT64_C(30000000);
    }
    reply_control(r,requested,command==CERBERUS_AP_SURVEY_COMMAND_INFO,error,out);
    CHECK(cerberus_ap_survey_control_valid(out,command==CERBERUS_AP_SURVEY_COMMAND_INFO,requested));
    if(lose){
        if(command==CERBERUS_AP_SURVEY_COMMAND_START&&r->mutate_foreign_after_lost_start){
            r->bound=r->held=false;r->foreign=true;++r->epoch;
        }
        memset(out,0,sizeof(*out));return TRANSPORT_LOST;
    }
    if(r->bad_control&&command==CERBERUS_AP_SURVEY_COMMAND_STATUS){out->reserved=1;r->bad_control=false;}
    return 0;
}

static int32_t snapshot_step(void *context,uint8_t source,uint64_t id,
    cerberus_ap_survey_snapshot_t *out,bool *complete,uint64_t *anchor,uint64_t *receipt)
{
    CHECK(context==&model&&source>=1&&source<=2);
    unsigned slot=source-1U;receiver_t *r=&receivers[slot];operation(slot,CERBERUS_AP_SURVEY_COMMAND_SNAPSHOT_READ);
    *complete=false;++r->read_steps;
    if(!r->bound||id!=r->accepted.session_id||r->release)return REMOTE_NOT_FOUND;
    if(!r->held)r->retention_us=now_us+UINT64_C(30000000);
    if(r->previous_read_us&&r->step&&now_us-r->previous_read_us>r->max_read_gap_us)
        r->max_read_gap_us=now_us-r->previous_read_us;
    r->previous_read_us=now_us;
    ++r->step;
    if(r->read_fail_step&&r->step==r->read_fail_step){r->read_fail_step=0;return TRANSPORT_LOST;}
    if(r->stall_read)return 0;
    if(slot==0||r->step==2){
        r->anchor_us=now_us;CHECK(cerberus_ap_survey_snapshot(&r->live,now_us,&r->pinned));
        CHECK(cerberus_ap_survey_snapshot_valid(&r->pinned));++r->snapshots;
    }
    if(slot==0){*out=r->pinned;*complete=true;}
    else if(r->step>=3&&r->step<=98){
        size_t offset=(r->step-3U)*40U,take=sizeof(*out)-offset;if(take>40U)take=40U;
        memcpy((uint8_t *)out+offset,(const uint8_t *)&r->pinned+offset,take);
    }else if(r->step==99)*complete=true;
    if(*complete){
        *anchor=r->anchor_us;*receipt=now_us;
        if(r->malformed_images){--r->malformed_images;out->aps[0].ssid_length=33;}
    }
    return 0;
}

static void snapshot_cancel(void *context,uint8_t source)
{
    CHECK(context==&model&&source>=1&&source<=2);
    receiver_t *r=&receivers[source-1U];++r->cancels;r->step=0;r->previous_read_us=0;
}
static bool publish(void *context,uint8_t source,uint64_t run,uint64_t anchor,
    uint64_t receipt,const cerberus_ap_survey_snapshot_t *snapshot)
{
    CHECK(context==&model&&source>=1&&source<=2);
    CHECK(cerberus_ap_survey_snapshot_valid(snapshot));
    receiver_t *r=&receivers[source-1U];
    if(r->reject_publish){r->reject_publish=false;return false;}
    cerberus_ap_survey_model_result_t result=cerberus_ap_survey_model_publish(&model,source,run,anchor,receipt,snapshot);
    if(fail_bad_publish)CHECK(result==CERBERUS_AP_SURVEY_MODEL_APPLIED||result==CERBERUS_AP_SURVEY_MODEL_DUPLICATE);
    if(result!=CERBERUS_AP_SURVEY_MODEL_APPLIED&&result!=CERBERUS_AP_SURVEY_MODEL_DUPLICATE)return false;
    ++r->published;++publications;return true;
}
static void unavailable(void *context,uint8_t source,bool value)
{CHECK(context==&model);cerberus_ap_survey_model_unavailable(&model,source,value);}

static void reset(void)
{
    memset(receivers,0,sizeof(receivers));
    receivers[0].epoch=UINT64_C(0x1234567800000001);receivers[1].epoch=UINT64_C(0x8765432100000001);
    receivers[0].stop_delay=receivers[1].stop_delay=2;
    now_us=UINT64_C(1000000);tick_number=0;operations=total_operations=publications=0;
    last_source=last_command=0;fail_bad_publish=true;
    cerberus_ap_survey_session_init(&session);cerberus_ap_survey_model_init(&model);
    ops=(cerberus_ap_survey_session_ops_t){.context=&model,.exchange=exchange,
        .snapshot_step=snapshot_step,.snapshot_cancel=snapshot_cancel,.publish=publish,.unavailable=unavailable};
}
static void tick(uint64_t elapsed)
{
    now_us+=elapsed;++tick_number;operations=0;last_source=last_command=0;
    advance_backend(0);advance_backend(1);
    cerberus_ap_survey_session_tick(&session,now_us,&ops);
    CHECK(operations<=1);
}
static void ticks(unsigned n)
{for(unsigned i=0;i<n;++i)tick(20000);}
static void start_run(uint64_t run)
{
    CHECK(cerberus_ap_survey_session_start(&session,run,1,UINT64_C(1)<<14));
    CHECK(total_operations==0||run>1);
}
static void until_published(unsigned a,unsigned b)
{
    unsigned count=0;
    while(receivers[0].published<a||receivers[1].published<b){CHECK(++count<2500);tick(20000);}
}
static void until_released(void)
{
    unsigned count=0;
    while(!cerberus_ap_survey_session_released(&session)){CHECK(++count<2500);tick(20000);}
    CHECK(!receivers[0].bound&&!receivers[1].bound);
}

static void normal_progress_and_model(void)
{
    reset();ticks(10);CHECK(!total_operations&&cerberus_ap_survey_session_released(&session));
    start_run(1);CHECK(!total_operations);until_published(3,2);
    CHECK(receivers[0].starts_accepted==1&&receivers[1].starts_accepted==1);
    CHECK(receivers[0].published>receivers[1].published&&receivers[1].read_steps>=99);
    CHECK(receivers[1].max_read_gap_us<250000&&receivers[0].commands[3]&&receivers[1].commands[3]);
    CHECK(model.banks[0].published&&model.banks[1].published&&!model.banks[0].unavailable&&!model.banks[1].unavailable);
    cerberus_ap_survey_page_t page;
    CHECK(cerberus_ap_survey_model_page(&model,CERBERUS_AP_SURVEY_AP,NULL,0,&page)&&page.copied==2);
    CHECK(cerberus_ap_survey_model_select(&model,&page.rows[0]));
    cerberus_ap_survey_record_t frozen=model.selected.frozen;
    unsigned previous=receivers[0].published;until_published(previous+1,receivers[1].published);
    CHECK(!memcmp(&frozen,&model.selected.frozen,sizeof(frozen))&&model.selected.present);
    CHECK(model.selected.latest.evidence.ap.frames>frozen.evidence.ap.frames);
    cerberus_ap_survey_session_stop(&session);until_released();
    uint64_t previous_ids[2]={receivers[0].epoch,receivers[1].epoch};
    CHECK(!cerberus_ap_survey_session_start(&session,1,1,UINT64_C(1)<<14));
    start_run(2);ticks(6);
    CHECK(receivers[0].epoch==previous_ids[0]+1&&receivers[1].epoch==previous_ids[1]+1);
    CHECK(receivers[0].starts_accepted==2&&receivers[1].starts_accepted==2);
    cerberus_ap_survey_session_close(&session);until_released();
}

static void stop_partial_and_close(void)
{
    reset();start_run(1);
    while(receivers[1].step<20)tick(20000);
    unsigned c5_snapshots=receivers[1].snapshots,c5_published=receivers[1].published;
    cerberus_ap_survey_session_stop(&session);until_released();
    CHECK(receivers[1].snapshots==c5_snapshots+1&&receivers[1].published==c5_published+1);
    CHECK(!(model.banks[1].snapshot.flags&CERBERUS_AP_SURVEY_SNAPSHOT_RECEIVING));
    CHECK(receivers[1].cancels>=2);
    reset();start_run(1);while(receivers[1].step<20)tick(20000);
    c5_snapshots=receivers[1].snapshots;c5_published=receivers[1].published;
    unsigned s3_published=receivers[0].published;
    cerberus_ap_survey_session_close(&session);until_released();
    CHECK(receivers[1].snapshots==c5_snapshots&&receivers[1].published==c5_published&&receivers[0].published==s3_published);
    CHECK(receivers[0].starts_accepted==1&&receivers[1].starts_accepted==1);
    reset();start_run(1);tick(20000);CHECK(!receivers[0].starts_accepted);
    cerberus_ap_survey_session_close(&session);tick(20000);
    CHECK(cerberus_ap_survey_session_released(&session)&&!receivers[0].starts_accepted&&!receivers[1].starts_accepted);
}

static void lost_commands(void)
{
    const uint8_t commands[]={CERBERUS_AP_SURVEY_COMMAND_START,CERBERUS_AP_SURVEY_COMMAND_STATUS,
        CERBERUS_AP_SURVEY_COMMAND_STOP,CERBERUS_AP_SURVEY_COMMAND_RELEASE};
    for(unsigned i=0;i<sizeof(commands);++i){
        reset();receiver_t *r=&receivers[1];r->lost_command=commands[i];r->lost_count=1;
        start_run(1);until_published(1,1);
        CHECK(r->starts_accepted==1&&r->commands[1]==1);
        if(commands[i]==CERBERUS_AP_SURVEY_COMMAND_STATUS)CHECK(!r->lost_count);
        cerberus_ap_survey_session_stop(&session);until_released();
        CHECK(!r->lost_count&&r->starts_accepted==1);
    }
    reset();receivers[1].lost_command=CERBERUS_AP_SURVEY_COMMAND_START;
    receivers[1].lost_count=1;receivers[1].lost_before=true;
    start_run(1);until_published(1,1);
    CHECK(receivers[1].commands[1]==2&&receivers[1].starts_accepted==1);
    cerberus_ap_survey_session_close(&session);until_released();
    reset();receivers[1].lost_command=CERBERUS_AP_SURVEY_COMMAND_START;
    receivers[1].lost_count=1;receivers[1].mutate_foreign_after_lost_start=true;
    start_run(1);ticks(100);
    CHECK(receivers[1].foreign&&receivers[1].commands[4]==0&&receivers[1].commands[5]==0);
    CHECK(session.radios[1].unavailable&&!session.radios[1].request.session_id&&receivers[0].published>0);
    cerberus_ap_survey_session_close(&session);until_released();
}

static void backoff_and_renew_priority(void)
{
    reset();start_run(1);until_published(1,1);
    receivers[1].lost_command=CERBERUS_AP_SURVEY_COMMAND_STATUS;receivers[1].lost_count=1;
    while(receivers[1].lost_count)tick(20000);
    uint64_t failed_at=now_us;unsigned controls=receivers[1].commands[2],reads=receivers[1].read_steps;
    uint32_t s3_before=receivers[0].live.live.received_frames;
    ticks(12);
    CHECK(now_us-failed_at==240000&&receivers[1].commands[2]==controls&&receivers[1].read_steps==reads);
    CHECK(receivers[0].live.live.received_frames>s3_before);
    ticks(3);CHECK(receivers[1].commands[2]>controls);
    receivers[1].bad_control=true;
    while(receivers[1].bad_control)tick(20000);
    uint64_t owned=session.radios[1].request.session_id;
    CHECK(owned==receivers[1].accepted.session_id&&session.radios[1].error==CERBERUS_AP_SURVEY_SESSION_INVALID);
    controls=receivers[1].commands[2];ticks(12);
    CHECK(session.radios[1].request.session_id==owned&&receivers[1].commands[2]==controls);
    ticks(3);CHECK(receivers[1].commands[2]>controls&&session.radios[1].request.session_id==owned);
    session.radios[0].next_renew_us=session.radios[1].next_renew_us=now_us;
    session.radios[0].next_action_us=session.radios[1].next_action_us=now_us;
    tick(20000);CHECK(last_command==CERBERUS_AP_SURVEY_COMMAND_RENEW);
    uint8_t first=last_source;tick(20000);CHECK(last_command==CERBERUS_AP_SURVEY_COMMAND_RENEW&&last_source!=first);
    cerberus_ap_survey_session_close(&session);until_released();
}

static void invalid_images_and_unavailable(void)
{
    reset();start_run(1);until_published(1,1);
    cerberus_ap_survey_bank_t saved=model.banks[1];
    receivers[1].malformed_images=1;unsigned published=receivers[1].published;
    while(receivers[1].malformed_images)tick(20000);
    CHECK(receivers[1].published==published&&session.radios[1].read_error==CERBERUS_AP_SURVEY_SESSION_INVALID);
    CHECK(!memcmp(&saved,&model.banks[1],sizeof(saved)));
    until_published(receivers[0].published,published+1);
    cerberus_ap_survey_session_close(&session);until_released();
    reset();receivers[1].foreign=true;start_run(1);ticks(100);
    CHECK(!receivers[1].starts_accepted&&session.radios[1].unavailable&&receivers[0].published>0);
    cerberus_ap_survey_session_close(&session);until_released();
    reset();receivers[1].lost_command=CERBERUS_AP_SURVEY_COMMAND_INFO;receivers[1].lost_count=1;
    start_run(1);ticks(100);CHECK(!receivers[1].starts_accepted&&receivers[0].published>0);
    cerberus_ap_survey_session_close(&session);until_released();
}

static void clock_and_lease_failures(void)
{
    reset();start_run(1);ticks(20);unsigned read_steps=receivers[1].read_steps;
    uint64_t anchor=now_us;operations=0;
    cerberus_ap_survey_session_tick(&session,anchor-1,&ops);
    CHECK(!cerberus_ap_survey_session_running(&session)&&operations<=1);
    until_released();CHECK(receivers[1].read_steps==read_steps);
    reset();start_run(1);ticks(20);
    tick(UINT64_C(11000000));until_released();
    CHECK(receivers[0].terminal_error==REMOTE_TIMEOUT&&receivers[1].terminal_error==REMOTE_TIMEOUT);
    CHECK(receivers[0].starts_accepted==1&&receivers[1].starts_accepted==1);
    reset();start_run(1);receivers[1].stall_read=true;
    unsigned count=0;while(session.radios[1].read_error!=CERBERUS_AP_SURVEY_SESSION_TIMEOUT){CHECK(++count<1500);tick(20000);}
    CHECK(receivers[1].cancels&&receivers[0].published>10&&receivers[1].commands[3]>3);
    cerberus_ap_survey_session_close(&session);until_released();
    reset();receivers[0].warning=receivers[1].warning=REMOTE_INVALID;start_run(1);until_published(1,1);
    CHECK(cerberus_ap_survey_session_running(&session)&&session.radios[0].error==REMOTE_INVALID&&session.radios[1].error==REMOTE_INVALID);
    CHECK(receivers[0].held&&receivers[1].held);cerberus_ap_survey_session_close(&session);until_released();
}

int main(void)
{
    normal_progress_and_model();stop_partial_and_close();lost_commands();
    backoff_and_renew_priority();invalid_images_and_unavailable();clock_and_lease_failures();
    printf("Dual AP survey session PASS %u checks; exact ownership, one operation/tick, independent snapshots, retries, final drain and immutable publication; session %zu bytes\n",checks,sizeof(session));
    return 0;
}
