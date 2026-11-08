#ifdef NDEBUG
#undef NDEBUG
#endif
#include "wifi_capture_session.h"
#include "c5_spi_link.h"
#include "storage.h"
#include "wifi_pcap.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t milliseconds, stop_after, lease_at;
static int lock_depth;
static TaskFunction_t queued_worker;
static cerberus_wifi_capture_status_payload_t remote;
static cerberus_s3_capture_options_t expected;
static unsigned starts, stops, peeks, reads, acks, begins, appends, finishes, saved, discards;
static unsigned stop_transport_failures, stop_payload_failures, ack_failures;
static unsigned read_failures, finish_timeouts, status_failures;
static unsigned frame_count, frame_index, last_acked, observed, observer_seen[8];
static unsigned allocations, alloc_fail_on, live_allocations, task_creates, stop_on_observed;
static bool storage_open, task_create_failure, lost_start_reply, lost_ack_reply;
static bool lost_start_status, start_rejected, inject_frame_at_stop;
static bool reenter_preflight, stop_during_preflight, wrong_recovery_mode, wrong_recovery_channel;
static bool finish_and_replace_during_create;
static esp_err_t begin_error, append_error, finish_error;
static uint8_t last_block[128];
static size_t last_block_length;
static void *allocated[3];
static size_t allocated_size[3];
static unsigned cases;
static bool plan_supported, plan_bad_reply, hop_bad_reply;
static unsigned plan_stages, plan_targets, stage_failures, target_failures, cancel_target;
static esp_err_t plan_query_error, hop_error;
static cerberus_wifi_capture_plan_status_payload_t staged;
static uint8_t planned_first_channel(void)
{
    for(unsigned i=0;i<CERBERUS_WIFI_CAPTURE_PLAN_CHANNELS;++i)
        if(expected.plan->channel_mask & (UINT64_C(1)<<i))return cerberus_wifi_capture_plan_channel(i);
    assert(0);return 0;
}
static uint32_t all_targets(uint8_t count)
{return count==32U?UINT32_MAX:(UINT32_C(1)<<count)-1U;}


void fake_capture_lock_enter(portMUX_TYPE *lock)
{(void)lock; assert(lock_depth == 0); ++lock_depth;}
void fake_capture_lock_exit(portMUX_TYPE *lock)
{(void)lock; assert(lock_depth == 1); --lock_depth;}

void *heap_caps_malloc(size_t size,unsigned caps)
{
    assert(!lock_depth && caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if(++allocations==alloc_fail_on)return NULL;
    for(unsigned i=0;i<3U;++i)if(!allocated[i]){
        allocated[i]=malloc(size);assert(allocated[i]);
        allocated_size[i]=size;++live_allocations;
        memset(allocated[i],0xa5,size);
        return allocated[i];
    }
    assert(0);return NULL;
}
void heap_caps_free(void *pointer)
{
    assert(!lock_depth && pointer && live_allocations);
    for(unsigned i=0;i<3U;++i)if(allocated[i]==pointer){
        const uint8_t *bytes=pointer;
        for(size_t j=0;j<allocated_size[i];++j)assert(bytes[j]==0U);
        free(pointer);allocated[i]=NULL;--live_allocations;return;
    }
    assert(0);
}
int xTaskCreate(TaskFunction_t fn,const char *name,uint32_t stack,
    void *argument,unsigned priority,void *handle)
{
    (void)argument;(void)priority;(void)handle;
    assert(fn && name && stack == 6144U && !queued_worker && !lock_depth);
    ++task_creates;
    if(task_create_failure)return 0;
    if(finish_and_replace_during_create){
        finish_and_replace_during_create=false;
        fn(NULL); /* The new task finishes before xTaskCreate returns. */
        assert(!live_allocations && !queued_worker);
        assert(cerberus_s3_capture_start_options(&expected)==ESP_OK);
        return pdPASS; /* Shared globals now belong to the replacement. */
    }
    queued_worker=fn;return pdPASS;
}
void vTaskDelay(uint32_t ticks)
{
    assert(!lock_depth);
    milliseconds+=ticks*10U;
    assert(milliseconds<100000U); /* A retained lease must recover in each fixture. */
    if(stop_after && milliseconds>=stop_after){stop_after=0;cerberus_s3_capture_stop();}
}
void vTaskDelete(void *task)
{(void)task;assert(!live_allocations && !storage_open && !lock_depth);}
int64_t esp_timer_get_time(void){return (int64_t)milliseconds*1000LL;}

esp_err_t cerberus_storage_wifi_capture_begin(const uint8_t *header,size_t length)
{
    assert(!lock_depth && expected.save_pcap);
    assert(header && length==CERBERUS_WIFI_PCAP_FILE_HEADER_SIZE);
    assert(header[0]==0x0a && header[1]==0x0d && !storage_open);
    ++begins;
    if(begin_error!=ESP_OK)return begin_error;
    storage_open=true;return ESP_OK;
}
esp_err_t cerberus_storage_wifi_capture_append(const uint8_t *block,size_t length)
{
    assert(!lock_depth && storage_open && block && length>=40U && length<=sizeof(last_block));
    ++appends;
    if(append_error!=ESP_OK)return append_error;
    memcpy(last_block,block,length);last_block_length=length;return ESP_OK;
}
esp_err_t cerberus_storage_wifi_capture_finish(bool keep,
    char *name,size_t capacity,uint64_t *bytes)
{
    assert(!lock_depth && storage_open && name && capacity>=17U && live_allocations==(expected.plan?3U:2U));
    cerberus_s3_capture_status_t state;cerberus_s3_capture_status(&state);
    assert(state.busy && state.state==CERBERUS_S3_CAPTURE_STOPPING);
    ++finishes;
    if(finish_timeouts){
        --finish_timeouts;
        assert(cerberus_s3_capture_start_options(&expected)==ESP_ERR_INVALID_STATE);
        return ESP_ERR_TIMEOUT; /* FILE really remains open. */
    }
    storage_open=false;
    if(finish_error!=ESP_OK)return finish_error;
    if(!keep){++discards;return ESP_OK;}
    assert(bytes);++saved;
    snprintf(name,capacity,"WCAP0001.PCAPNG");
    *bytes=CERBERUS_WIFI_PCAP_FILE_HEADER_SIZE+(uint64_t)last_block_length;
    return ESP_OK;
}

esp_err_t c5_spi_link_wifi_capture_status(cerberus_wifi_capture_status_payload_t *status)
{
    assert(status && !lock_depth);
    if(reenter_preflight){
        reenter_preflight=false;
        cerberus_s3_capture_status_t local;cerberus_s3_capture_status(&local);
        assert(local.busy && local.generation==0U);
        assert(cerberus_s3_capture_start_options(&expected)==ESP_ERR_INVALID_STATE);
        if(stop_during_preflight)cerberus_s3_capture_stop();
    }
    if(status_failures){--status_failures;return ESP_ERR_TIMEOUT;}
    if(lease_at && milliseconds>=lease_at && remote.state==CERBERUS_WIFI_CAPTURE_RUNNING){
        remote.state=CERBERUS_WIFI_CAPTURE_STOPPED;remote.error_code=ESP_ERR_TIMEOUT;
    }
    *status=remote;return ESP_OK;
}
esp_err_t c5_spi_link_wifi_capture_plan_status(cerberus_wifi_capture_plan_status_payload_t *out)
{
    assert(out && !lock_depth);memset(out,0,sizeof(*out));
    if(!plan_supported)return ESP_ERR_NOT_SUPPORTED;
    if(plan_query_error)return plan_query_error;
    *out=staged;return ESP_OK;
}
esp_err_t c5_spi_link_wifi_capture_plan_stage(const cerberus_wifi_capture_plan_request_t *r,
    cerberus_wifi_capture_plan_status_payload_t *out)
{
    assert(!lock_depth && plan_supported && expected.plan && r && out && !r->reserved);
    assert(r->channel_mask==expected.plan->channel_mask && r->target_count==expected.plan->target_count);
    assert(r->dwell_ms==250U);++plan_stages;
    if(staged.generation!=r->generation)staged=(cerberus_wifi_capture_plan_status_payload_t){
        .generation=r->generation,.channel_mask=r->channel_mask,.target_count=r->target_count,
        .state=r->target_count?CERBERUS_WIFI_CAPTURE_PLAN_STAGING:CERBERUS_WIFI_CAPTURE_PLAN_READY};
    *out=staged;if(plan_bad_reply)out->channel_mask^=1U;
    if(stage_failures){--stage_failures;return ESP_ERR_TIMEOUT;}return ESP_OK;
}
esp_err_t c5_spi_link_wifi_capture_plan_target(const cerberus_wifi_capture_target_request_t *r,
    cerberus_wifi_capture_plan_status_payload_t *out)
{
    assert(!lock_depth && r && out && expected.plan && r->generation==staged.generation);
    assert(r->index<staged.target_count && !memcmp(r->bssid,expected.plan->bssids[r->index],6U));
    ++plan_targets;staged.target_present_mask|=UINT32_C(1)<<r->index;
    if(staged.target_present_mask==all_targets(staged.target_count))staged.state=CERBERUS_WIFI_CAPTURE_PLAN_READY;
    *out=staged;if(cancel_target && plan_targets==cancel_target)cerberus_s3_capture_stop();
    if(target_failures){--target_failures;return ESP_ERR_TIMEOUT;}return ESP_OK;
}
esp_err_t c5_spi_link_wifi_capture_hop_status(uint32_t generation,
    cerberus_wifi_capture_hop_status_payload_t *out)
{
    assert(!lock_depth && out && expected.plan);memset(out,0,sizeof(*out));
    if(hop_error)return hop_error;
    *out=(cerberus_wifi_capture_hop_status_payload_t){.generation=generation,
        .attempted_mask=expected.plan->channel_mask,.applied_mask=expected.plan->channel_mask,
        .completed_sweeps=2U};
    if(hop_bad_reply)out->applied_mask|=UINT64_C(1)<<63U;
    return ESP_OK;
}
esp_err_t c5_spi_link_wifi_capture_start(
    const cerberus_wifi_capture_start_request_t *request,
    cerberus_wifi_capture_status_payload_t *status)
{
    assert(!lock_depth && request && status && request->generation>remote.generation);
    cerberus_s3_capture_status_t local;cerberus_s3_capture_status(&local);
    assert(local.busy && local.generation==request->generation);
    assert(request->channel==expected.channel && request->lease_ms==5000U);
    assert(request->mode_mask==expected.mode_mask);
    assert(request->flags==((expected.hopping?CERBERUS_WIFI_CAPTURE_FLAG_HOP:0U)|
        (expected.filter_bssid?CERBERUS_WIFI_CAPTURE_FLAG_BSSID:0U)|
        (expected.plan?CERBERUS_WIFI_CAPTURE_FLAG_PLAN:0U)));
    if(expected.filter_bssid)assert(!memcmp(request->bssid,expected.bssid,6U));
    ++starts;
    if(start_rejected)return ESP_ERR_INVALID_STATE;
    remote.generation=request->generation;
    remote.channel=expected.plan?planned_first_channel():expected.hopping?1U:request->channel;
    remote.mode_mask=request->mode_mask;
    remote.state=CERBERUS_WIFI_CAPTURE_RUNNING;
    remote.pending=(uint8_t)(frame_count>2U?2U:frame_count);
    remote.accepted=frame_count;
    remote.error_code=ESP_OK;
    if(wrong_recovery_mode)remote.mode_mask=CERBERUS_WIFI_CAPTURE_MODE_RAW;
    if(wrong_recovery_channel)remote.channel=11U;
    *status=remote;
    if(lost_start_status)status_failures=1U;
    return lost_start_reply?ESP_ERR_TIMEOUT:ESP_OK;
}
esp_err_t c5_spi_link_wifi_capture_stop(uint32_t generation,
    cerberus_wifi_capture_status_payload_t *status)
{
    assert(status && !lock_depth);++stops;
    if(generation!=remote.generation)return ESP_ERR_INVALID_STATE;
    cerberus_s3_capture_status_t state;cerberus_s3_capture_status(&state);
    assert(state.busy && state.state==CERBERUS_S3_CAPTURE_STOPPING);
    if(stop_transport_failures){--stop_transport_failures;return ESP_ERR_TIMEOUT;}
    if(stop_payload_failures){
        --stop_payload_failures;*status=remote;
        status->state=CERBERUS_WIFI_CAPTURE_ERROR;status->error_code=ESP_FAIL;
        return ESP_OK;
    }
    if(inject_frame_at_stop){
        inject_frame_at_stop=false;frame_count=1U;remote.accepted=1U;remote.pending=1U;
    }
    remote.state=CERBERUS_WIFI_CAPTURE_STOPPED;*status=remote;return ESP_OK;
}
static uint8_t current_channel(void)
{static const uint8_t channels[]={6,36,1};return expected.hopping?channels[frame_index%3U]:expected.channel;}
esp_err_t c5_spi_link_wifi_capture_peek(uint32_t generation,
    cerberus_wifi_capture_peek_payload_t *peek)
{
    assert(generation==remote.generation && peek && !lock_depth);++peeks;
    memset(peek,0,sizeof(*peek));peek->generation=generation;
    if(remote.pending){
        peek->sequence=frame_index+1U;peek->length=42U;
        peek->channel=current_channel();peek->rssi_dbm=(int8_t)(-42-(int)frame_index);
        uint64_t time=UINT64_C(999000000)+peek->sequence*10U;
        for(unsigned i=0;i<6U;++i)peek->monotonic_us_le48[i]=(uint8_t)(time>>(8U*i));
        remote.channel=peek->channel;
    }
    return ESP_OK;
}
esp_err_t c5_spi_link_wifi_capture_read_frame(
    const cerberus_wifi_capture_peek_payload_t *peek,uint8_t *frame,size_t capacity)
{
    assert(!lock_depth && peek && peek->sequence==frame_index+1U &&
        peek->length==42U && frame && capacity>=42U);++reads;
    if(read_failures){--read_failures;return ESP_ERR_INVALID_CRC;}
    memset(frame,0,42U);frame[0]=0x80U;memset(frame+4,0xff,6U);
    const uint8_t transmitter[]={2,1,2,3,4,5};
    memcpy(frame+10,transmitter,6U);memcpy(frame+16,transmitter,6U);
    frame[36]=0U;frame[37]=1U;frame[38]=(uint8_t)('A'+frame_index);
    frame[39]=3U;frame[40]=1U;frame[41]=peek->channel;
    return ESP_OK;
}
uint64_t c5_spi_link_wifi_capture_monotonic_us(const cerberus_wifi_capture_peek_payload_t *peek)
{
    assert(peek);uint64_t time=0U;
    for(unsigned i=0;i<6U;++i)time|=(uint64_t)peek->monotonic_us_le48[i]<<(8U*i);
    return time;
}
esp_err_t c5_spi_link_wifi_capture_ack(uint32_t generation,uint32_t sequence)
{
    assert(!lock_depth && generation==remote.generation && sequence);++acks;
    if(sequence==last_acked)return ESP_OK; /* Applied ACK, lost response. */
    assert(remote.pending && sequence==frame_index+1U);
    if(ack_failures){--ack_failures;return ESP_ERR_TIMEOUT;}
    last_acked=sequence;++frame_index;
    unsigned remaining=frame_count-frame_index;
    remote.pending=(uint8_t)(remaining>2U?2U:remaining);
    if(lost_ack_reply){lost_ack_reply=false;return ESP_ERR_TIMEOUT;}
    return ESP_OK;
}
static void observe(const cerberus_s3_capture_observation_t *meta,
    const uint8_t *frame,size_t length,void *context)
{
    assert(context==&observed && !lock_depth && frame && length==42U);
    assert(meta && meta->generation==remote.generation && meta->sequence<8U);
    assert(observer_seen[meta->sequence]++==0U);
    assert(meta->channel==current_channel() && meta->rssi_dbm==(int8_t)(-42-(int)frame_index));
    assert(meta->observed_us==UINT64_C(999000000)+meta->sequence*10U);
    assert(frame[0]==0x80U && frame[38]==(uint8_t)('A'+frame_index));
    ++observed;
    if(stop_on_observed && observed>=stop_on_observed)cerberus_s3_capture_stop();
}

static void reset_case(void)
{
    assert(!queued_worker && !storage_open && !live_allocations && !lock_depth);
    cerberus_s3_capture_status_t state;cerberus_s3_capture_status(&state);assert(!state.busy);
    plan_supported=plan_bad_reply=hop_bad_reply=false;
    plan_stages=plan_targets=stage_failures=target_failures=cancel_target=0U;
    plan_query_error=hop_error=ESP_OK;memset(&staged,0,sizeof(staged));
    milliseconds=stop_after=lease_at=0U;
    starts=stops=peeks=reads=acks=begins=appends=finishes=saved=discards=0U;
    stop_transport_failures=stop_payload_failures=ack_failures=read_failures=0U;
    finish_timeouts=status_failures=allocations=alloc_fail_on=task_creates=0U;
    frame_index=last_acked=observed=stop_on_observed=0U;frame_count=1U;
    memset(observer_seen,0,sizeof(observer_seen));
    task_create_failure=lost_start_reply=lost_ack_reply=lost_start_status=start_rejected=false;
    inject_frame_at_stop=false;
    reenter_preflight=stop_during_preflight=wrong_recovery_mode=wrong_recovery_channel=false;
    finish_and_replace_during_create=false;
    begin_error=append_error=finish_error=ESP_OK;last_block_length=0U;
    remote.state=CERBERUS_WIFI_CAPTURE_STOPPED;remote.pending=0U;
    remote.error_code=ESP_OK;remote.accepted=0U;
    expected=(cerberus_s3_capture_options_t){.channel=6U,
        .mode_mask=CERBERUS_WIFI_CAPTURE_MODE_BEACON,.observer=observe,.observer_context=&observed};
    ++cases;
}
static cerberus_s3_capture_status_t run_capture(void)
{
    assert(queued_worker && !lock_depth && live_allocations);
    TaskFunction_t fn=queued_worker;queued_worker=NULL;fn(NULL);
    assert(!queued_worker && !storage_open && !lock_depth && !live_allocations);
    cerberus_s3_capture_status_t out;cerberus_s3_capture_status(&out);assert(!out.busy);return out;
}
static cerberus_s3_capture_status_t start_and_run(void)
{
    assert(cerberus_s3_capture_start_options(&expected)==ESP_OK);
    cerberus_s3_capture_status_t status;cerberus_s3_capture_status(&status);
    assert(status.busy && status.state==CERBERUS_S3_CAPTURE_STARTING);
    assert(status.hopping==expected.hopping && status.save_pcap==expected.save_pcap);
    assert(cerberus_s3_capture_start_options(&expected)==ESP_ERR_INVALID_STATE);
    return run_capture();
}

static void planned_options(cerberus_wifi_capture_plan_t *plan)
{
    plan_supported=true;expected.channel=0U;expected.hopping=true;
    expected.mode_mask=CERBERUS_WIFI_CAPTURE_MODE_SAE_COMMIT;expected.plan=plan;
    stop_after=100U;
}
static void plan_cases(void)
{
    cerberus_wifi_capture_plan_t plan, frozen;
    cerberus_s3_capture_status_t state;
    const uint8_t ap[6]={2,1,2,3,4,5};
    cerberus_wifi_capture_plan_clear(&plan);
    assert(cerberus_wifi_capture_plan_add_target(&plan,ap,6U));
    assert(cerberus_wifi_capture_plan_add_target(&plan,ap,36U));
    assert(plan.target_count==1U && cerberus_wifi_capture_plan_valid(&plan));
    reset_case();planned_options(&plan);plan_supported=false;
    assert(cerberus_s3_capture_start_options(&expected)==ESP_ERR_NOT_SUPPORTED);
    assert(!allocations && !starts);

    reset_case();planned_options(&plan);frozen=plan;staged.generation=10000U;
    uint32_t reserved=0U;
    assert(cerberus_s3_capture_start_options_generation(&expected,&reserved)==ESP_OK && reserved>10000U);
    memset(&plan,0,sizeof(plan));expected.plan=&frozen;
    state=run_capture();assert(state.state==CERBERUS_S3_CAPTURE_COMPLETE && state.planned &&
        state.hop_valid && state.attempted_mask==frozen.channel_mask && state.completed_sweeps==2U);
    assert(observed==1U && plan_stages==1U && plan_targets==1U && allocations==2U);
    plan=frozen;

    reset_case();planned_options(&plan);stage_failures=1U;target_failures=1U;
    expected.save_pcap=true;state=start_and_run();
    assert(state.state==CERBERUS_S3_CAPTURE_SAVED && plan_stages==2U && plan_targets==2U && observed==1U);
    assert(saved==1U && allocations==3U);

    reset_case();planned_options(&plan);stage_failures=3U;expected.save_pcap=true;
    state=start_and_run();assert(state.state==CERBERUS_S3_CAPTURE_ERROR && !starts && !stops && discards==1U);
    reset_case();planned_options(&plan);plan_bad_reply=true;
    state=start_and_run();assert(state.state==CERBERUS_S3_CAPTURE_ERROR && state.error_code==ESP_ERR_INVALID_RESPONSE && !starts);
    reset_case();planned_options(&plan);target_failures=3U;
    state=start_and_run();assert(state.state==CERBERUS_S3_CAPTURE_ERROR && !starts && plan_targets==3U);
    reset_case();planned_options(&plan);cancel_target=1U;
    state=start_and_run();assert(state.state==CERBERUS_S3_CAPTURE_IDLE && !starts && !stops && !observed);

    reset_case();planned_options(&plan);hop_error=ESP_ERR_TIMEOUT;
    state=start_and_run();assert(state.state==CERBERUS_S3_CAPTURE_COMPLETE && !state.hop_valid && state.hop_error==ESP_ERR_TIMEOUT);
    reset_case();planned_options(&plan);hop_bad_reply=true;
    state=start_and_run();assert(state.state==CERBERUS_S3_CAPTURE_COMPLETE && !state.hop_valid && state.hop_error==ESP_ERR_INVALID_RESPONSE);

    reset_case();planned_options(&plan);plan_query_error=ESP_ERR_TIMEOUT;
    assert(cerberus_s3_capture_start_options(&expected)==ESP_ERR_TIMEOUT && !allocations && !task_creates);
    for(unsigned fail=1U;fail<=3U;++fail){
        reset_case();planned_options(&plan);expected.save_pcap=true;alloc_fail_on=fail;
        assert(cerberus_s3_capture_start_options(&expected)==ESP_ERR_NO_MEM && !live_allocations && !starts);
    }
    reset_case();planned_options(&plan);staged.generation=UINT32_MAX;
    assert(cerberus_s3_capture_start_options(&expected)==ESP_ERR_INVALID_STATE && !allocations);

    /* Abandoned staged IDs also fence a later legacy fixed-channel capture. */
    reset_case();plan_supported=true;staged.generation=20000U;stop_after=100U;
    assert(cerberus_s3_capture_start_options_generation(&expected,&reserved)==ESP_OK && reserved>20000U);
    state=run_capture();assert(state.state==CERBERUS_S3_CAPTURE_COMPLETE && !state.planned && !plan_stages);

    cerberus_wifi_capture_plan_clear(&plan);
    for(unsigned i=0;i<32U;++i){uint8_t address[6]={2,1,2,3,4,(uint8_t)i};
        assert(cerberus_wifi_capture_plan_add_target(&plan,address,6U));}
    reset_case();planned_options(&plan);state=start_and_run();
    assert(state.state==CERBERUS_S3_CAPTURE_COMPLETE && plan_targets==32U && staged.target_present_mask==UINT32_MAX);
    cerberus_wifi_capture_plan_clear(&plan);plan.channel_mask=cerberus_wifi_capture_plan_band_mask(0U);
    reset_case();planned_options(&plan);state=start_and_run();
    assert(state.state==CERBERUS_S3_CAPTURE_COMPLETE && !plan_targets && state.hop_valid);
}

int main(void)
{
    cerberus_s3_capture_status_t state;
    const uint8_t bssid[]={2,1,2,3,4,5};
    reset_case();expected.save_pcap=true;expected.filter_bssid=true;
    expected.mode_mask=CERBERUS_WIFI_CAPTURE_MODE_RAW;expected.observer=NULL;
    memcpy(expected.bssid,bssid,6U);stop_after=100U;
    assert(cerberus_s3_capture_start(6U,expected.mode_mask,bssid,true)==ESP_OK);
    state=run_capture();
    assert(state.state==CERBERUS_S3_CAPTURE_SAVED && state.written==1U && saved==1U);
    assert(reads==1U && acks==1U && !state.pending && last_block[0]==3U);
    assert(!strcmp(state.file_name,"WCAP0001.PCAPNG") && allocations==2U);

    reset_case();begin_error=ESP_ERR_NOT_FOUND;expected.hopping=true;expected.channel=0U;
    frame_count=3U;stop_on_observed=3U;state=start_and_run();
    assert(state.state==CERBERUS_S3_CAPTURE_COMPLETE && observed==3U && reads==3U);
    assert(!begins && !appends && !finishes && allocations==1U && state.channel==1U);
    assert(state.generation==remote.generation && !state.written && !state.file_name[0]);

    reset_case();ack_failures=12U;state=start_and_run();
    assert(state.state==CERBERUS_S3_CAPTURE_ERROR && state.error_code==ESP_ERR_TIMEOUT);
    assert(observed==1U && reads==1U && acks==13U && !state.pending);

    reset_case();expected.save_pcap=true;lost_ack_reply=true;stop_after=150U;
    state=start_and_run();assert(state.state==CERBERUS_S3_CAPTURE_SAVED);
    assert(observed==1U && reads==1U && appends==1U && acks==2U);

    reset_case();expected.save_pcap=true;append_error=ESP_FAIL;ack_failures=12U;
    frame_count=2U;state=start_and_run();
    assert(state.state==CERBERUS_S3_CAPTURE_ERROR && state.error_code==ESP_FAIL);
    assert(observed==2U && reads==2U && appends==1U && !state.written && discards==1U);
    assert(!state.pending && !state.file_name[0]);

    reset_case();expected.save_pcap=true;finish_timeouts=4U;stop_after=100U;
    state=start_and_run();assert(state.state==CERBERUS_S3_CAPTURE_SAVED && finishes==5U);
    assert(observed==1U && saved==1U && milliseconds>=500U);

    reset_case();expected.save_pcap=true;finish_error=ESP_FAIL;stop_after=100U;
    state=start_and_run();assert(state.state==CERBERUS_S3_CAPTURE_ERROR && !state.file_name[0]);
    assert(finishes==1U && !saved);

    reset_case();expected.save_pcap=true;begin_error=ESP_ERR_NOT_FOUND;
    state=start_and_run();assert(state.state==CERBERUS_S3_CAPTURE_ERROR && !starts && !finishes);

    reset_case();lost_start_reply=true;stop_after=100U;state=start_and_run();
    assert(state.state==CERBERUS_S3_CAPTURE_COMPLETE && observed==1U && starts==1U);

    reset_case();lost_start_reply=true;lost_start_status=true;state=start_and_run();
    assert(state.state==CERBERUS_S3_CAPTURE_ERROR && stops && observed==1U && !state.pending);

    reset_case();lost_start_reply=true;wrong_recovery_mode=true;frame_count=0U;
    state=start_and_run();assert(state.state==CERBERUS_S3_CAPTURE_ERROR && !observed);
    reset_case();lost_start_reply=true;wrong_recovery_channel=true;frame_count=0U;
    state=start_and_run();assert(state.state==CERBERUS_S3_CAPTURE_ERROR && !observed);

    reset_case();frame_count=0U;stop_transport_failures=1U;stop_payload_failures=1U;
    stop_after=100U;state=start_and_run();
    assert(state.state==CERBERUS_S3_CAPTURE_COMPLETE && stops==3U);

    reset_case();frame_count=0U;lease_at=400U;state=start_and_run();
    assert(state.state==CERBERUS_S3_CAPTURE_ERROR && state.error_code==ESP_ERR_TIMEOUT);
    assert(!state.pending && stops==1U && milliseconds>=500U);

    reset_case();read_failures=2U;stop_after=100U;state=start_and_run();
    assert(state.state==CERBERUS_S3_CAPTURE_COMPLETE && reads==3U && observed==1U);

    reset_case();read_failures=3U;state=start_and_run();
    assert(state.state==CERBERUS_S3_CAPTURE_ERROR && state.error_code==ESP_ERR_INVALID_CRC);
    assert(reads==3U && !observed && acks==1U && !state.pending);

    reset_case();frame_count=0U;inject_frame_at_stop=true;stop_after=100U;state=start_and_run();
    assert(state.state==CERBERUS_S3_CAPTURE_COMPLETE && observed==1U && reads==1U);

    reset_case();start_rejected=true;state=start_and_run();
    assert(state.state==CERBERUS_S3_CAPTURE_ERROR && !observed && !reads && stops==1U);

    reset_case();assert(cerberus_s3_capture_start_options(&expected)==ESP_OK);
    cerberus_s3_capture_stop();state=run_capture();
    assert(state.state==CERBERUS_S3_CAPTURE_IDLE && !starts && !begins);

    reset_case();assert(cerberus_s3_capture_start_options(&expected)==ESP_OK);
    cerberus_s3_capture_status(&state);
    assert(!cerberus_s3_capture_stop_generation(0U));
    assert(!cerberus_s3_capture_stop_generation(state.generation+1U));
    cerberus_s3_capture_status(&state);assert(state.state==CERBERUS_S3_CAPTURE_STARTING);
    stop_after=100U;state=run_capture();
    assert(state.state==CERBERUS_S3_CAPTURE_COMPLETE && observed==1U);
    assert(!cerberus_s3_capture_stop_generation(state.generation));

    reset_case();assert(cerberus_s3_capture_start_options(&expected)==ESP_OK);
    cerberus_s3_capture_status(&state);assert(cerberus_s3_capture_stop_generation(state.generation));
    state=run_capture();assert(state.state==CERBERUS_S3_CAPTURE_IDLE && !starts && !observed);

    reset_case();reenter_preflight=true;stop_after=100U;state=start_and_run();
    assert(state.state==CERBERUS_S3_CAPTURE_COMPLETE && observed==1U);
    reset_case();reenter_preflight=true;stop_during_preflight=true;
    assert(cerberus_s3_capture_start_options(&expected)==ESP_OK);
    cerberus_s3_capture_status(&state);assert(state.state==CERBERUS_S3_CAPTURE_STOPPING);
    state=run_capture();assert(state.state==CERBERUS_S3_CAPTURE_IDLE && !starts && !observed);

    reset_case();status_failures=1U;
    assert(cerberus_s3_capture_start_options(&expected)==ESP_ERR_TIMEOUT);
    cerberus_s3_capture_status(&state);assert(!state.busy && !allocations && !starts);

    reset_case();expected.save_pcap=true;append_error=ESP_ERR_INVALID_SIZE;
    state=start_and_run();assert(state.state==CERBERUS_S3_CAPTURE_ERROR && saved==1U && !discards);
    assert(observed==1U && state.written==0U && state.file_name[0]);

    for(unsigned allocation=1U;allocation<=2U;++allocation){
        reset_case();expected.save_pcap=true;alloc_fail_on=allocation;
        assert(cerberus_s3_capture_start_options(&expected)==ESP_ERR_NO_MEM);
        cerberus_s3_capture_status(&state);assert(!state.busy && !live_allocations && !starts && !begins);
        assert(!task_creates && state.state==CERBERUS_S3_CAPTURE_ERROR);
    }
    reset_case();task_create_failure=true;
    assert(cerberus_s3_capture_start_options(&expected)==ESP_ERR_NO_MEM);
    cerberus_s3_capture_status(&state);assert(!state.busy && !live_allocations && !starts && !begins);

    reset_case();expected.hopping=true;expected.channel=6U;
    assert(cerberus_s3_capture_start_options(&expected)==ESP_ERR_INVALID_ARG);
    expected.channel=0U;expected.mode_mask=CERBERUS_WIFI_CAPTURE_MODE_RAW;
    assert(cerberus_s3_capture_start_options(&expected)==ESP_ERR_INVALID_ARG);
    expected.mode_mask=CERBERUS_WIFI_CAPTURE_MODE_BEACON;expected.filter_bssid=true;
    memcpy(expected.bssid,bssid,6U);
    assert(cerberus_s3_capture_start_options(&expected)==ESP_ERR_INVALID_ARG);
    assert(!allocations && !task_creates);

    reset_case();finish_and_replace_during_create=true;stop_after=100U;
    uint32_t remote_before=remote.generation, returned_generation=0U;
    assert(cerberus_s3_capture_start_options_generation(&expected,&returned_generation)==ESP_OK);
    cerberus_s3_capture_status(&state);
    assert(returned_generation>remote_before && state.generation==returned_generation+1U);
    assert(state.busy && !cerberus_s3_capture_stop_generation(returned_generation));
    assert(cerberus_s3_capture_stop_generation(state.generation));
    state=run_capture();assert(state.state==CERBERUS_S3_CAPTURE_IDLE && observed==1U);

    reset_case();returned_generation=UINT32_MAX;alloc_fail_on=1U;
    assert(cerberus_s3_capture_start_options_generation(&expected,&returned_generation)==ESP_ERR_NO_MEM);
    assert(returned_generation==0U && !live_allocations && !queued_worker);

    /* A successful local start may fail opening SD before sending C5 START.
     * Its retained generation cannot name the next foreign AP capture. */
    reset_case();expected.save_pcap=true;begin_error=ESP_ERR_NOT_FOUND;
    remote_before=remote.generation;
    uint32_t failed_generation=0U;
    assert(cerberus_s3_capture_start_options_generation(&expected,&failed_generation)==ESP_OK);
    state=run_capture();
    assert(state.state==CERBERUS_S3_CAPTURE_ERROR && state.generation==failed_generation);
    assert(failed_generation>remote_before && remote.generation==remote_before && !starts);

    reset_case();expected.save_pcap=true;expected.filter_bssid=true;
    expected.mode_mask=CERBERUS_WIFI_CAPTURE_MODE_RAW;memcpy(expected.bssid,bssid,6U);
    assert(remote.generation==remote_before);
    assert(cerberus_s3_capture_start_options_generation(&expected,&returned_generation)==ESP_OK);
    cerberus_s3_capture_status(&state);
    assert(returned_generation==failed_generation+1U && state.generation==returned_generation);
    assert(state.busy && !cerberus_s3_capture_stop_generation(failed_generation));
    stop_after=100U;state=run_capture();
    assert(state.state==CERBERUS_S3_CAPTURE_SAVED && observed==1U && remote.generation==returned_generation);

    /* Allocator/task failures also burn reservations without advancing C5. */
    uint32_t previous_reserved=returned_generation;
    remote_before=remote.generation;
    for(unsigned kind=0U;kind<3U;++kind){
        reset_case();expected.save_pcap=true;returned_generation=UINT32_MAX;
        if(kind<2U)alloc_fail_on=kind+1U;else task_create_failure=true;
        assert(cerberus_s3_capture_start_options_generation(&expected,&returned_generation)==ESP_ERR_NO_MEM);
        cerberus_s3_capture_status(&state);
        assert(!state.busy && state.generation==previous_reserved+1U && !returned_generation);
        assert(remote.generation==remote_before && !starts && !live_allocations);
        previous_reserved=state.generation;
    }
    reset_case();remote.generation=0U;stop_after=100U;
    assert(cerberus_s3_capture_start_options_generation(&expected,&returned_generation)==ESP_OK);
    assert(returned_generation==previous_reserved+1U); /* C5 reset cannot reset S3 floor. */
    assert(!cerberus_s3_capture_stop_generation(previous_reserved));
    state=run_capture();assert(state.state==CERBERUS_S3_CAPTURE_COMPLETE && observed==1U);

    plan_cases();

    reset_case();remote.generation=UINT32_MAX;returned_generation=17U;
    assert(cerberus_s3_capture_start_options_generation(&expected,&returned_generation)==ESP_ERR_INVALID_STATE);
    cerberus_s3_capture_status(&state);
    assert(!returned_generation && !state.busy && !allocations && !starts);

    reset_case();remote.generation=UINT32_MAX-1U;alloc_fail_on=1U;
    assert(cerberus_s3_capture_start_options_generation(&expected,&returned_generation)==ESP_ERR_NO_MEM);
    cerberus_s3_capture_status(&state);
    assert(!returned_generation && state.generation==UINT32_MAX && !state.busy);
    assert(remote.generation==UINT32_MAX-1U && !starts);

    reset_case();remote.generation=0U;returned_generation=17U;
    assert(cerberus_s3_capture_start_options_generation(&expected,&returned_generation)==ESP_ERR_INVALID_STATE);
    cerberus_s3_capture_status(&state);
    assert(!returned_generation && !state.busy && !allocations && !task_creates && !starts);
    assert(!cerberus_s3_capture_stop_generation(UINT32_MAX));
    printf("S3 capture session passed %u cases: PSRAM lifetime, fixed/hopping provenance, no-card analysis, once-only ingestion/ACK retry, START recovery, Stop/drain/lease, SD write/close and allocation faults.\n",cases);
}
