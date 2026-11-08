#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

typedef int esp_err_t;
enum {ESP_OK=0,ESP_FAIL=-1,ESP_ERR_NO_MEM=0x101,ESP_ERR_INVALID_ARG,ESP_ERR_INVALID_STATE,
    ESP_ERR_TIMEOUT,ESP_ERR_NOT_FOUND,ESP_ERR_WIFI_NOT_STARTED,ESP_ERR_NOT_FINISHED,ESP_ERR_NOT_SUPPORTED,ESP_ERR_INVALID_RESPONSE};
static unsigned checks;
#define CHECK(x) do {++checks;if(!(x)){fprintf(stderr,"FAIL line %u: %s\n",__LINE__,#x);exit(1);}}while(0)
typedef int portMUX_TYPE;
static unsigned critical_depth;
static void (*unlock_hook)(void);
static void after_unlock(void) { if (!critical_depth && unlock_hook) { void (*f)(void)=unlock_hook; unlock_hook=NULL; f(); } }
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) do {(void)(p);++critical_depth;}while(0)
#define portEXIT_CRITICAL(p) do {(void)(p);CHECK(critical_depth);--critical_depth;after_unlock();}while(0)
#define pdMS_TO_TICKS(x) (x)
#define pdPASS 1
enum {MALLOC_CAP_INTERNAL=1,MALLOC_CAP_8BIT=2,MALLOC_CAP_SPIRAM=4};
enum {WIFI_STORAGE_RAM=1,WIFI_MODE_NULL=0,WIFI_SECOND_CHAN_NONE=0,
    WIFI_PROMIS_FILTER_MASK_MGMT=1,WIFI_PROMIS_FILTER_MASK_DATA=2};
typedef enum {WIFI_PKT_MGMT=0,WIFI_PKT_CTRL=1,WIFI_PKT_DATA=2,WIFI_PKT_MISC=3} wifi_promiscuous_pkt_type_t;
typedef struct {unsigned sig_len,channel,rx_state;int rssi;} wifi_pkt_rx_ctrl_t;
typedef struct {wifi_pkt_rx_ctrl_t rx_ctrl;uint8_t payload[4200];} wifi_promiscuous_pkt_t;
typedef struct {unsigned filter_mask;} wifi_promiscuous_filter_t;
typedef struct {int static_rx_buf_num,dynamic_rx_buf_num,dynamic_tx_buf_num,rx_ba_win;
    int ampdu_rx_enable,ampdu_tx_enable,amsdu_tx_enable;} wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){0})
#include "s3_wifi_owner.h"
#include "s3_ble_host.h"
#include "s3_wifi_ap_survey.h"
#include "wifi_ap_survey_protocol.h"

static int64_t now_us=1000;
static unsigned allocations,alloc_calls,fail_alloc,internal_allocations,psram_allocations;
static size_t heap_free=131072;
static bool task_fail,driver_initialized,driver_started,driver_receiving,stop_in_enable;
static unsigned sdk_calls,disable_calls,unregister_calls,stop_calls,deinit_calls,task_deletes;
static unsigned fail_remaining,ble_queries,ble_at_query;
static const char *fail_stage;
static void (*queued_worker)(void *),(*delay_hook)(unsigned),(*timer_hook)(void);
static void *worker_arg;
static void (*rx_callback)(void *,wifi_promiscuous_pkt_type_t);
static uint64_t session;
typedef struct { unsigned schan,nchan; } wifi_country_t;
static wifi_country_t country={1,11};
static unsigned channel_calls;
static uint8_t tuned_channel;
static uint64_t failed_channels;
static cerberus_s3_ble_owner_t ble_owner;
static bool fault(const char *stage)
{if(fail_stage&&!strcmp(stage,fail_stage)&&fail_remaining){--fail_remaining;return true;}return false;}
static int64_t esp_timer_get_time(void)
{if(timer_hook){void (*f)(void)=timer_hook;timer_hook=NULL;f();}return now_us;}
cerberus_s3_ble_owner_t cerberus_s3_ble_host_owner(void)
{++ble_queries;if(ble_at_query==ble_queries)ble_owner=CERBERUS_S3_BLE_OWNER_SCANNER;return ble_owner;}
static esp_err_t esp_netif_init(void){CHECK(!critical_depth);return fault("netif")?ESP_FAIL:ESP_OK;}
static esp_err_t esp_event_loop_create_default(void){CHECK(!critical_depth);return fault("event")?ESP_FAIL:ESP_OK;}
static size_t heap_caps_get_free_size(unsigned caps){CHECK(caps==3);return heap_free;}
static void *heap_caps_calloc(size_t n,size_t size,unsigned caps)
{
    CHECK(!critical_depth);++alloc_calls;if(alloc_calls==fail_alloc)return NULL;
    CHECK(caps==3||caps==6);if(caps==3)++internal_allocations;else ++psram_allocations;
    void *p=calloc(n,size);CHECK(p);++allocations;return p;
}
static void heap_caps_free(void *p){CHECK(!critical_depth);if(p){CHECK(allocations);--allocations;free(p);}}
static int xTaskCreate(void (*fn)(void *),const char *name,unsigned stack,void *arg,unsigned priority,void *handle)
{
    CHECK(!critical_depth&&!strcmp(name,"s3_ap_survey")&&stack==4096&&priority==3&&!handle);
    if(task_fail)return 0;CHECK(!queued_worker);queued_worker=fn;worker_arg=arg;return pdPASS;
}
static void vTaskDelete(void *p){CHECK(!p&&!critical_depth);++task_deletes;}
static void vTaskDelay(unsigned ms){CHECK(!critical_depth);now_us+=(int64_t)ms*1000;if(delay_hook)delay_hook(ms);}
static esp_err_t sdk(const char *name){CHECK(!critical_depth);++sdk_calls;return fault(name)?ESP_FAIL:ESP_OK;}
static esp_err_t esp_wifi_init(const wifi_init_config_t *c)
{
    CHECK(c->static_rx_buf_num==4&&c->dynamic_rx_buf_num==8&&c->dynamic_tx_buf_num==4&&c->rx_ba_win==4&&
        !c->ampdu_rx_enable&&!c->ampdu_tx_enable&&!c->amsdu_tx_enable);
    esp_err_t e=sdk("init");if(!e){CHECK(!driver_initialized);driver_initialized=true;}return e;
}
static esp_err_t esp_wifi_set_storage(int value){CHECK(value==WIFI_STORAGE_RAM);return sdk("storage");}
static esp_err_t esp_wifi_set_mode(int value){CHECK(value==WIFI_MODE_NULL);return sdk("mode");}
static esp_err_t esp_wifi_start(void){esp_err_t e=sdk("start");if(!e)driver_started=true;return e;}
static esp_err_t esp_wifi_set_promiscuous_filter(const wifi_promiscuous_filter_t *f)
{CHECK(f->filter_mask==(WIFI_PROMIS_FILTER_MASK_MGMT|WIFI_PROMIS_FILTER_MASK_DATA));return sdk("filter");}
static esp_err_t esp_wifi_set_promiscuous_rx_cb(void (*cb)(void *,wifi_promiscuous_pkt_type_t))
{if(!cb)++unregister_calls;esp_err_t e=sdk(cb?"callback":"unregister");if(!e)rx_callback=cb;return e;}
static esp_err_t esp_wifi_get_country(wifi_country_t *out) { esp_err_t e=sdk("country");if(!e)*out=country;return e; }
static esp_err_t esp_wifi_set_channel(uint8_t channel,int second)
{CHECK(channel>=1&&channel<=14&&second==WIFI_SECOND_CHAN_NONE);++channel_calls;tuned_channel=channel;esp_err_t e=sdk("channel");return failed_channels&(UINT64_C(1)<<(channel-1U))?ESP_FAIL:e;}
static esp_err_t esp_wifi_set_promiscuous(bool value)
{
    if(!value)++disable_calls;esp_err_t e=sdk(value?"enable":"disable");
    if(!e)driver_receiving=value;
    if(value&&stop_in_enable){cerberus_ap_survey_request_t q={session};cerberus_ap_survey_control_t r;
        CHECK(cerberus_s3_wifi_ap_survey_exchange(CERBERUS_AP_SURVEY_COMMAND_STOP,&q,sizeof(q),&r,sizeof(r))==ESP_OK);
        CHECK(r.flags&CERBERUS_AP_SURVEY_HELD);stop_in_enable=false;}
    return e;
}
static esp_err_t esp_wifi_stop(void){++stop_calls;esp_err_t e=sdk("stop");if(!e)driver_started=false;return e;}
static esp_err_t esp_wifi_deinit(void){++deinit_calls;esp_err_t e=sdk("deinit");if(!e)driver_initialized=false;return e;}
#include "../../s3_main/main/s3_wifi_owner.c"
#include "../../s3_main/main/s3_wifi_ap_survey.c"

static cerberus_ap_survey_control_t exchange(unsigned command,const void *request,size_t length)
{
    cerberus_ap_survey_control_t out;
    CHECK(cerberus_s3_wifi_ap_survey_exchange((uint8_t)command,request,length,&out,sizeof(out))==ESP_OK);
    uint64_t expected=0;if(length)memcpy(&expected,request,sizeof(expected));
    if(expected||command==CERBERUS_AP_SURVEY_COMMAND_INFO)
        CHECK(cerberus_ap_survey_control_valid(&out,command==CERBERUS_AP_SURVEY_COMMAND_INFO,expected));
    return out;
}
static cerberus_ap_survey_control_t control(unsigned command)
{cerberus_ap_survey_request_t request={session};return exchange(command,&request,sizeof(request));}
static cerberus_ap_survey_start_t start_request;
static cerberus_ap_survey_control_t start(uint64_t mask)
{
    session=exchange(CERBERUS_AP_SURVEY_COMMAND_INFO,NULL,0).session_id+1U;
    start_request=(cerberus_ap_survey_start_t){.session_id=session,.channel_mask=mask,.dwell_ms=250};
    return exchange(CERBERUS_AP_SURVEY_COMMAND_START,&start_request,sizeof(start_request));
}
static void reset(void)
{
    CHECK(!survey_context&&!queued_worker&&!allocations&&!critical_depth);
    now_us=1000;heap_free=131072;alloc_calls=fail_alloc=internal_allocations=psram_allocations=0;
    task_fail=driver_initialized=driver_started=driver_receiving=stop_in_enable=false;
    sdk_calls=disable_calls=unregister_calls=stop_calls=deinit_calls=task_deletes=0;
    ble_queries=ble_at_query=fail_remaining=channel_calls=0;ble_owner=CERBERUS_S3_BLE_OWNER_NONE;
    fail_stage=NULL;delay_hook=NULL;timer_hook=NULL;unlock_hook=NULL;rx_callback=NULL;worker_arg=NULL;
    survey_epoch=1;survey_admission_counter=0;survey_command=survey_launching=false;
    atomic_store(&owner,CERBERUS_S3_WIFI_NONE);atomic_store(&survey_admission,0);netif_ready=event_ready=false;
    country=(wifi_country_t){1,11};failed_channels=0;tuned_channel=0;
}
static wifi_promiscuous_pkt_t beacon(uint8_t identity)
{
    wifi_promiscuous_pkt_t p={.rx_ctrl={.sig_len=45,.channel=tuned_channel,.rssi=-43}};
    p.payload[0]=0x80;memset(p.payload+4,0xff,6);
    const uint8_t mac[6]={2,1,2,3,4,identity};
    memcpy(p.payload+10,mac,6);memcpy(p.payload+16,mac,6);
    p.payload[34]=0x10;p.payload[36]=0;p.payload[37]=3;
    p.payload[38]='A';p.payload[39]=0;p.payload[40]=0xff;
    return p;
}
static void emit_ap(uint8_t identity)
{wifi_promiscuous_pkt_t p=beacon(identity);CHECK(rx_callback);rx_callback(&p,WIFI_PKT_MGMT);}
static void emit_relation(uint8_t ds)
{
    wifi_promiscuous_pkt_t p={.rx_ctrl={.sig_len=28,.channel=tuned_channel,.rssi=-61}};
    p.payload[0]=8;p.payload[1]=ds|0x40;
    const uint8_t ap[6]={2,1,2,3,4,5},sta[6]={4,1,2,3,4,5};
    memcpy(p.payload+4,ds==1?ap:sta,6);memcpy(p.payload+10,ds==1?sta:ap,6);
    CHECK(rx_callback);rx_callback(&p,WIFI_PKT_DATA);
}
static cerberus_ap_survey_projection_t begin_snapshot(void)
{
    cerberus_ap_survey_request_t request={session};cerberus_ap_survey_projection_t out;
    CHECK(cerberus_s3_wifi_ap_survey_exchange(CERBERUS_AP_SURVEY_COMMAND_SNAPSHOT_BEGIN,
        &request,sizeof(request),&out,sizeof(out))==ESP_OK);
    CHECK(cerberus_ap_survey_projection_valid(&out,session));
    return out;
}
static cerberus_ap_survey_chunk_t read_chunk(uint32_t projection,uint16_t offset)
{
    cerberus_ap_survey_read_t request={session,projection,offset};cerberus_ap_survey_chunk_t out;
    CHECK(cerberus_s3_wifi_ap_survey_exchange(CERBERUS_AP_SURVEY_COMMAND_SNAPSHOT_READ,
        &request,sizeof(request),&out,sizeof(out))==ESP_OK);
    return out;
}
static void read_snapshot(uint32_t projection,cerberus_ap_survey_snapshot_t *out)
{
    memset(out,0,sizeof(*out));
    for(unsigned offset=0;offset<sizeof(*out);offset+=40){
        cerberus_ap_survey_chunk_t chunk=read_chunk(projection,(uint16_t)offset);
        CHECK(chunk.session_id==session&&chunk.projection==projection&&chunk.offset==offset);
        size_t n=sizeof(*out)-offset;if(n>40)n=40;
        memcpy((uint8_t *)out+offset,chunk.data,n);
        for(size_t i=n;i<40;++i)CHECK(!chunk.data[i]);
    }
    CHECK(cerberus_ap_survey_snapshot_valid(out));
}
static unsigned emitted,terminal_seen,cleanup_retries,scenario;
static cerberus_ap_survey_snapshot_t snapshot_a,snapshot_b;
static void same_channel_rearm(void)
{
    s3_ap_survey_context_t *c=survey_context;
    CHECK(c&&c->accepting);
    CHECK(cerberus_ap_survey_disarm(&c->banks->table,session,c->banks->table.arm_token,(uint64_t)now_us));
    CHECK(cerberus_ap_survey_arm(&c->banks->table,session,++survey_admission_counter,tuned_channel,(uint64_t)now_us));
    atomic_store(&survey_admission,survey_admission_counter);
}
static void stop_during_decode(void)
{CHECK(survey_context->inflight==1);CHECK(control(CERBERUS_AP_SURVEY_COMMAND_STOP).flags&CERBERUS_AP_SURVEY_HELD);}
static void lifecycle(unsigned delay)
{
    s3_ap_survey_context_t *c=survey_context;CHECK(c);
    if(delay==1U){
        CHECK(scenario==7&&c->stop&&c->inflight==1&&disable_calls==0&&unregister_calls==0);
        --c->inflight;return;
    }
    if(delay==250U){
        ++cleanup_retries;CHECK(c->held&&c->stop&&!c->accepting&&cerberus_s3_wifi_owner()==CERBERUS_S3_WIFI_SURVEY);
        CHECK(control(CERBERUS_AP_SURVEY_COMMAND_STATUS).flags&CERBERUS_AP_SURVEY_HELD);return;
    }
    if(c->accepting&&!emitted){
        ++emitted;
        if(scenario==1){timer_hook=same_channel_rearm;emit_ap(5);CHECK(!c->banks->table.live.received_frames);}
        if(scenario==2){unlock_hook=stop_during_decode;emit_ap(5);CHECK(!c->banks->table.live.received_frames);return;}
        if(scenario==3){now_us=(int64_t)c->banks->table.armed_us-1;emit_ap(5);CHECK(c->stop&&!c->accepting);return;}
        if(scenario==4){c->projection_counter=UINT32_MAX;CHECK(begin_snapshot().error_code==ESP_ERR_INVALID_STATE&&c->stop);return;}
        if(scenario==5){c->banks->table.live.revision=UINT32_MAX-1;emit_ap(5);CHECK(c->stop);return;}
        if(scenario==6){
            uint64_t old=c->deadline_us;now_us=(int64_t)old-1000;
            CHECK(control(CERBERUS_AP_SURVEY_COMMAND_STATUS).flags&CERBERUS_AP_SURVEY_RECEIVING);
            CHECK(c->deadline_us==old);CHECK(begin_snapshot().projection&&c->deadline_us==old);
            CHECK(!control(CERBERUS_AP_SURVEY_COMMAND_RENEW).error_code&&c->deadline_us>old);
            now_us=(int64_t)old+1;CHECK(control(CERBERUS_AP_SURVEY_COMMAND_STATUS).flags&CERBERUS_AP_SURVEY_RECEIVING);
            now_us=(int64_t)c->deadline_us;CHECK(control(CERBERUS_AP_SURVEY_COMMAND_RENEW).error_code==ESP_ERR_TIMEOUT&&c->stop);return;
        }
        if(scenario==7){
            ++c->inflight;CHECK(control(CERBERUS_AP_SURVEY_COMMAND_STOP).flags&CERBERUS_AP_SURVEY_HELD);return;
        }
        emit_relation(1);CHECK(c->banks->table.live.relation_count==1&&!c->banks->table.live.ap_count);
        emit_relation(2);emit_ap(5);
        CHECK(c->banks->table.live.ap_count==1&&c->banks->table.live.relation_count==1);
        CHECK(c->banks->table.live.relations[0].row.directions==3);
        unsigned calls=sdk_calls;uint64_t deadline=c->deadline_us;
        cerberus_ap_survey_projection_t pin=begin_snapshot();CHECK(!pin.error_code&&pin.projection&&pin.length==3816);
        CHECK(c->accepting&&driver_receiving&&sdk_calls==calls&&c->deadline_us==deadline);
        emit_ap(6);read_snapshot(pin.projection,&snapshot_a);
        CHECK(snapshot_a.ap_count==1&&snapshot_a.aps[0].ssid_length==3&&snapshot_a.aps[0].ssid[1]==0&&snapshot_a.aps[0].ssid[2]==0xff);
        CHECK(cerberus_s3_wifi_ap_survey_snapshot(session,&snapshot_b)==ESP_OK&&snapshot_b.ap_count==2);
        CHECK(sdk_calls==calls&&c->accepting&&c->deadline_us==deadline);
        CHECK(cerberus_s3_wifi_ap_survey_snapshot(session+1,&snapshot_b)==ESP_ERR_NOT_FOUND&&!snapshot_b.session_id);
        CHECK(read_chunk(pin.projection,1).projection==0&&read_chunk(pin.projection,3840).projection==0);
        cerberus_ap_survey_projection_t second=begin_snapshot();CHECK(second.projection!=pin.projection);
        CHECK(!read_chunk(pin.projection,0).projection);read_snapshot(second.projection,&snapshot_b);CHECK(snapshot_b.ap_count==2);
        uint32_t frames=c->banks->table.live.received_frames;
        wifi_promiscuous_pkt_t invalid=beacon(7);invalid.rx_ctrl.rx_state=1;rx_callback(&invalid,WIFI_PKT_MGMT);
        invalid.rx_ctrl.rx_state=0;invalid.rx_ctrl.channel=14;rx_callback(&invalid,WIFI_PKT_MGMT);
        invalid.rx_ctrl.channel=tuned_channel;invalid.rx_ctrl.sig_len=3;rx_callback(&invalid,WIFI_PKT_MGMT);
        CHECK(c->banks->table.live.received_frames==frames);
        uint64_t current=session;session+=1;CHECK(control(CERBERUS_AP_SURVEY_COMMAND_STOP).error_code==ESP_ERR_NOT_FOUND);session=current;
        CHECK(c->accepting);
        CHECK(control(CERBERUS_AP_SURVEY_COMMAND_STOP).flags&CERBERUS_AP_SURVEY_HELD);
        emit_ap(8);CHECK(c->banks->table.live.received_frames==frames);
    }
    if(!c->held){
        ++terminal_seen;
        CHECK(!driver_initialized&&!driver_started&&!driver_receiving&&!rx_callback);
        CHECK(cerberus_s3_wifi_owner()==CERBERUS_S3_WIFI_NONE&&!cerberus_s3_wifi_ap_survey_active());
        unsigned calls=sdk_calls;uint64_t deadline=c->deadline_us;
        CHECK(exchange(CERBERUS_AP_SURVEY_COMMAND_START,&start_request,sizeof(start_request)).flags&CERBERUS_AP_SURVEY_BOUND);
        CHECK(sdk_calls==calls&&!c->accepting&&c->deadline_us==deadline);
        if(c->data_ready&&!c->clock_invalid&&scenario!=4){
            CHECK(cerberus_s3_wifi_ap_survey_snapshot(session,&snapshot_a)==ESP_OK);
            CHECK(!(snapshot_a.flags&CERBERUS_AP_SURVEY_SNAPSHOT_RECEIVING));
        }
        if(scenario==8){
            deadline=c->deadline_us;now_us+=1000;
            CHECK(!control(CERBERUS_AP_SURVEY_COMMAND_STATUS).error_code&&c->deadline_us>deadline);
            deadline=c->deadline_us;
            CHECK(!read_chunk(0,0).projection&&c->deadline_us==deadline);
            now_us=(int64_t)deadline;return;
        }
        CHECK(control(CERBERUS_AP_SURVEY_COMMAND_RELEASE).flags&CERBERUS_AP_SURVEY_BOUND);
    }
}
static void run(void)
{
    CHECK(queued_worker);void(*fn)(void*)=queued_worker;void *arg=worker_arg;
    queued_worker=NULL;worker_arg=NULL;emitted=terminal_seen=cleanup_retries=0;
    delay_hook=lifecycle;fn(arg);delay_hook=NULL;
    CHECK(!survey_context&&!allocations&&!critical_depth&&task_deletes==1);
    CHECK(control(CERBERUS_AP_SURVEY_COMMAND_RELEASE).error_code==ESP_OK);
    CHECK(control(CERBERUS_AP_SURVEY_COMMAND_STATUS).error_code==ESP_ERR_NOT_FOUND);
}
static void admission_tests(void)
{
    reset();CHECK(cerberus_s3_wifi_claim(CERBERUS_S3_WIFI_WEB)==ESP_OK);
    CHECK(start(1).error_code==ESP_ERR_INVALID_STATE&&!allocations&&!sdk_calls);
    CHECK(cerberus_s3_wifi_owner()==CERBERUS_S3_WIFI_WEB);CHECK(cerberus_s3_wifi_release(CERBERUS_S3_WIFI_WEB)==ESP_OK);
    reset();ble_owner=CERBERUS_S3_BLE_OWNER_SCANNER;CHECK(start(1).error_code==ESP_ERR_INVALID_STATE&&!allocations);
    reset();ble_at_query=2;CHECK(start(1).error_code==ESP_ERR_INVALID_STATE&&!allocations&&cerberus_s3_wifi_owner()==CERBERUS_S3_WIFI_NONE);
    for(unsigned i=1;i<=2;++i){reset();fail_alloc=i;CHECK(start(1).error_code==ESP_ERR_NO_MEM&&!allocations&&!queued_worker);}
    reset();heap_free=65535;CHECK(start(1).error_code==ESP_ERR_NO_MEM&&!alloc_calls);
    reset();task_fail=true;CHECK(start(1).error_code==ESP_ERR_NO_MEM&&!allocations&&!queued_worker);CHECK(survey_epoch==2);
    reset();CHECK(start(UINT64_C(1)<<14).error_code==ESP_ERR_INVALID_ARG&&!alloc_calls);
    reset();start_request=(cerberus_ap_survey_start_t){.session_id=2,.channel_mask=1,.dwell_ms=249};
    CHECK(exchange(CERBERUS_AP_SURVEY_COMMAND_START,&start_request,sizeof(start_request)).error_code==ESP_ERR_INVALID_ARG);
    start_request.dwell_ms=250;start_request.reserved=1;
    CHECK(exchange(CERBERUS_AP_SURVEY_COMMAND_START,&start_request,sizeof(start_request)).error_code==ESP_ERR_INVALID_ARG);
    start_request.reserved=0;start_request.session_id=3;
    CHECK(exchange(CERBERUS_AP_SURVEY_COMMAND_START,&start_request,sizeof(start_request)).error_code==ESP_ERR_INVALID_STATE);
    start_request.session_id=2;
    CHECK(cerberus_s3_wifi_ap_survey_exchange(CERBERUS_AP_SURVEY_COMMAND_START,&start_request,sizeof(start_request),&start_request,22)==ESP_ERR_INVALID_ARG);
    CHECK(start_request.session_id==2);
    now_us=INT64_MAX-1;CHECK(start(1).error_code==ESP_ERR_INVALID_STATE&&!allocations);
    reset();survey_epoch=UINT64_MAX;CHECK(start(1).error_code==ESP_ERR_INVALID_ARG&&!alloc_calls);survey_epoch=1;
}
static void lifecycle_tests(void)
{
    scenario=0;reset();CHECK(!start(1).error_code);CHECK(internal_allocations==1&&psram_allocations==1);
    uint64_t deadline=survey_context->deadline_us;
    CHECK(!exchange(CERBERUS_AP_SURVEY_COMMAND_START,&start_request,sizeof(start_request)).error_code&&survey_context->deadline_us==deadline);
    start_request.channel_mask=2;CHECK(exchange(CERBERUS_AP_SURVEY_COMMAND_START,&start_request,sizeof(start_request)).error_code==ESP_ERR_INVALID_STATE);start_request.channel_mask=1;
    run();CHECK(emitted==1&&terminal_seen==1&&channel_calls==1);
    const char *setup[]={"netif","event","init","storage","mode","start","country","filter","callback","channel","enable"};
    for(unsigned i=0;i<sizeof(setup)/sizeof(setup[0]);++i){reset();CHECK(!start(1).error_code);fail_stage=setup[i];fail_remaining=100;run();CHECK(!emitted&&terminal_seen==1);}
    const char *cleanup[]={"disable","unregister","stop","deinit"};
    for(unsigned i=0;i<4;++i){reset();CHECK(!start(1).error_code);fail_stage=cleanup[i];fail_remaining=2;run();CHECK(cleanup_retries==2&&terminal_seen==1);}
    reset();CHECK(!start(1).error_code);stop_in_enable=true;run();CHECK(!emitted&&disable_calls==1);
    reset();CHECK(!start(1).error_code);CHECK(control(CERBERUS_AP_SURVEY_COMMAND_STOP).flags&CERBERUS_AP_SURVEY_HELD);run();CHECK(!sdk_calls);
    reset();CHECK(!start(1).error_code);now_us+=INT64_C(10000000);run();CHECK(!sdk_calls&&!emitted);
    reset();country=(wifi_country_t){12,3};CHECK(!start(1).error_code);run();CHECK(!emitted&&!channel_calls);
    reset();country=(wifi_country_t){0,11};CHECK(!start(1).error_code);run();CHECK(!emitted&&!channel_calls);
    for(scenario=1;scenario<=8;++scenario){reset();CHECK(!start(1).error_code);run();CHECK(emitted==1);}
    scenario=0;reset();survey_admission_counter=UINT32_MAX;CHECK(!start(1).error_code);run();CHECK(!emitted&&terminal_seen==1);
    scenario=0;
}
static unsigned hop_seen;
static uint8_t previous_channel;
static void hop_lifecycle(unsigned delay)
{
    (void)delay;s3_ap_survey_context_t *c=survey_context;
    if(c->accepting&&previous_channel!=c->channel){
        previous_channel=c->channel;++hop_seen;emit_ap(5);
        CHECK(c->channel==(hop_seen==2?6:1));
        if(hop_seen==3)CHECK(control(CERBERUS_AP_SURVEY_COMMAND_STOP).flags&CERBERUS_AP_SURVEY_HELD);
    }
    if(!c->held){
        CHECK(hop_seen==3&&c->banks->table.live.ap_count==2&&c->banks->table.live.completed_passes==1);
        CHECK(c->banks->table.live.effective_channels==33&&c->banks->table.live.applied_channels==33);
        CHECK(c->banks->table.live.receive_us==525000);
        CHECK(!control(CERBERUS_AP_SURVEY_COMMAND_RELEASE).error_code);
    }
}
static void hopping_test(void)
{
    reset();CHECK(!start(33|(UINT64_C(1)<<13)).error_code);
    hop_seen=previous_channel=0;delay_hook=hop_lifecycle;
    void(*fn)(void*)=queued_worker;void *arg=worker_arg;queued_worker=NULL;worker_arg=NULL;fn(arg);delay_hook=NULL;
    CHECK(!survey_context&&!allocations&&task_deletes==1&&channel_calls==3);
}
static unsigned continuous_ticks;
static uint32_t first_token;
static bool skipped_test;
static void continuous_lifecycle(unsigned delay)
{
    (void)delay;s3_ap_survey_context_t *c=survey_context;
    if(c->accepting){
        ++continuous_ticks;
        if(continuous_ticks==1){first_token=atomic_load(&survey_admission);emit_ap(5);}
        if(!skipped_test){
            CHECK(channel_calls==1&&atomic_load(&survey_admission)==first_token&&c->channel==1);
            if(continuous_ticks%5==0){
                uint64_t deadline=c->deadline_us;unsigned calls=sdk_calls;
                CHECK(cerberus_s3_wifi_ap_survey_snapshot(session,&snapshot_a)==ESP_OK);
                CHECK(cerberus_ap_survey_snapshot_valid(&snapshot_a)&&snapshot_a.receive_us==(uint64_t)now_us-c->banks->table.armed_us);
                CHECK(c->deadline_us==deadline&&sdk_calls==calls&&c->accepting);
            }
        }else {
            CHECK(c->channel==6&&c->error==ESP_OK&&c->tune_error==ESP_FAIL);
            cerberus_ap_survey_control_t status=control(CERBERUS_AP_SURVEY_COMMAND_STATUS);
            CHECK(status.state==CERBERUS_AP_SURVEY_RUNNING&&status.error_code==ESP_FAIL);
        }
        if(continuous_ticks==(skipped_test?11U:36U))
            CHECK(control(CERBERUS_AP_SURVEY_COMMAND_STOP).flags&CERBERUS_AP_SURVEY_HELD);
    }
    if(!c->held){
        if(skipped_test){
            CHECK(c->state==CERBERUS_AP_SURVEY_COMPLETE&&c->error==ESP_OK&&c->tune_error==ESP_FAIL);
            CHECK(c->banks->table.live.tune_failures==2&&c->banks->table.live.attempted_channels==33);
            CHECK(c->banks->table.live.applied_channels==32&&c->banks->table.live.completed_passes==1);
        }else CHECK(c->banks->table.live.completed_passes==3&&c->banks->table.live.receive_us==900000);
        CHECK(control(CERBERUS_AP_SURVEY_COMMAND_RELEASE).flags&CERBERUS_AP_SURVEY_BOUND);
    }
}
static void continuous_and_skipped_test(void)
{
    for(unsigned i=0;i<2;++i){
        reset();skipped_test=i!=0;continuous_ticks=0;
        if(skipped_test)failed_channels=1;
        CHECK(!start(skipped_test?33:1).error_code);
        delay_hook=continuous_lifecycle;
        void(*fn)(void*)=queued_worker;void *arg=worker_arg;queued_worker=NULL;worker_arg=NULL;fn(arg);delay_hook=NULL;
        CHECK(!survey_context&&!allocations&&task_deletes==1);
    }
}
int main(void)
{
    admission_tests();lifecycle_tests();hopping_test();continuous_and_skipped_test();reset();
    printf("S3 passive AP survey PASS %u checks; internal %zu bytes, PSRAM banks %zu bytes, worker 4096 bytes\n",
        checks,sizeof(s3_ap_survey_context_t),sizeof(s3_ap_survey_banks_t));
    return 0;
}
