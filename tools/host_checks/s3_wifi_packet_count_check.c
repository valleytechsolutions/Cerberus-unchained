#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

typedef int esp_err_t;
enum {ESP_OK=0,ESP_FAIL=-1,ESP_ERR_NO_MEM=0x101,ESP_ERR_INVALID_ARG,ESP_ERR_INVALID_STATE,
    ESP_ERR_TIMEOUT,ESP_ERR_NOT_FOUND,ESP_ERR_WIFI_NOT_STARTED,ESP_ERR_NOT_FINISHED};
static unsigned checks;
#define CHECK(x) do {++checks;if(!(x)){fprintf(stderr,"FAIL line %u: %s\n",__LINE__,#x);exit(1);}}while(0)
typedef int portMUX_TYPE;
static unsigned critical_depth;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) do {(void)(p);++critical_depth;}while(0)
#define portEXIT_CRITICAL(p) do {(void)(p);CHECK(critical_depth);--critical_depth;}while(0)
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
#include "s3_wifi_packet_count.h"
#include "wifi_packet_count_protocol.h"

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
    CHECK(!critical_depth&&!strcmp(name,"s3_wifi_count")&&stack==4096&&priority==3&&!handle);
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
static esp_err_t esp_wifi_set_channel(uint8_t channel,int second)
{CHECK(channel>=1&&channel<=14&&second==WIFI_SECOND_CHAN_NONE);return sdk("channel");}
static esp_err_t esp_wifi_set_promiscuous(bool value)
{
    if(!value)++disable_calls;esp_err_t e=sdk(value?"enable":"disable");
    if(!e)driver_receiving=value;
    if(value&&stop_in_enable){cerberus_packet_count_request_t q={session};cerberus_packet_count_control_t r;
        CHECK(cerberus_s3_wifi_packet_count_exchange(CERBERUS_PACKET_COUNT_COMMAND_STOP,&q,sizeof(q),&r,sizeof(r))==ESP_OK);
        CHECK(r.flags&CERBERUS_PACKET_COUNT_HELD);stop_in_enable=false;}
    return e;
}
static esp_err_t esp_wifi_stop(void){++stop_calls;esp_err_t e=sdk("stop");if(!e)driver_started=false;return e;}
static esp_err_t esp_wifi_deinit(void){++deinit_calls;esp_err_t e=sdk("deinit");if(!e)driver_initialized=false;return e;}
#include "../../s3_main/main/s3_wifi_owner.c"
#include "../../s3_main/main/s3_wifi_packet_count.c"

static cerberus_packet_count_control_t exchange(unsigned cmd,const void *q,size_t n)
{
    cerberus_packet_count_control_t r;
    CHECK(cerberus_s3_wifi_packet_count_exchange((uint8_t)cmd,q,n,&r,sizeof(r))==ESP_OK);return r;
}
static cerberus_packet_count_control_t control(unsigned cmd)
{cerberus_packet_count_request_t q={session};return exchange(cmd,&q,sizeof(q));}
static void reset(void)
{
    CHECK(!count_context&&!queued_worker&&!allocations&&!critical_depth);
    now_us=1000;heap_free=131072;alloc_calls=fail_alloc=internal_allocations=psram_allocations=0;
    task_fail=driver_initialized=driver_started=driver_receiving=stop_in_enable=false;
    sdk_calls=disable_calls=unregister_calls=stop_calls=deinit_calls=task_deletes=0;
    ble_queries=ble_at_query=fail_remaining=0;ble_owner=CERBERUS_S3_BLE_OWNER_NONE;
    fail_stage=NULL;delay_hook=NULL;timer_hook=NULL;rx_callback=NULL;worker_arg=NULL;
    count_epoch=1;count_admission_counter=0;count_command=count_launching=false;
    atomic_store(&owner,CERBERUS_S3_WIFI_NONE);atomic_store(&count_admission,0);netif_ready=event_ready=false;
}
static cerberus_packet_count_control_t begin(bool record)
{
    cerberus_packet_count_control_t info=exchange(CERBERUS_PACKET_COUNT_COMMAND_INFO,NULL,0);
    session=info.session_id+1U;cerberus_packet_count_begin_t q={.session_id=session,.channel_mask=1,.target_count=2,.dwell_ms=50,.record=record};
    return exchange(CERBERUS_PACKET_COUNT_COMMAND_BEGIN,&q,sizeof(q));
}
static void configure(void)
{
    for(unsigned i=0;i<2;++i){cerberus_packet_count_target_t q={.session_id=session,.index=(uint8_t)i,
        .role=i?CERBERUS_WIFI_PACKET_COUNT_STA:CERBERUS_WIFI_PACKET_COUNT_AP,.channel_hint=177,.mac={2,1,2,3,4,5}};
        if(i)q.mac[0]=4;
        cerberus_packet_count_control_t r=exchange(CERBERUS_PACKET_COUNT_COMMAND_TARGET,&q,sizeof(q));
        CHECK(!r.error_code&&r.configured==(1U<<(i+1U))-1U);
        CHECK(!exchange(CERBERUS_PACKET_COUNT_COMMAND_TARGET,&q,sizeof(q)).error_code);
    }
    CHECK(!control(CERBERUS_PACKET_COUNT_COMMAND_START).error_code);
}
static void emit(unsigned length,unsigned flags)
{
    wifi_promiscuous_pkt_t packet={.rx_ctrl={.sig_len=length+4U,.channel=count_context?count_context->channel:1,.rssi=-47}};
    packet.payload[0]=8;packet.payload[1]=(uint8_t)flags;
    const uint8_t a[6]={2,1,2,3,4,5},b[6]={4,1,2,3,4,5};
    memcpy(packet.payload+4,a,6);memcpy(packet.payload+10,b,6);memcpy(packet.payload+16,a,6);
    CHECK(rx_callback);rx_callback(&packet,WIFI_PKT_DATA);
}
static void read_pin(uint32_t pin,uint8_t *bytes,unsigned n)
{
    for(unsigned part=0;part<(n+7U)/8U;++part){cerberus_packet_count_read_t q={session,pin,(uint16_t)part};cerberus_packet_count_chunk_t r;
        CHECK(cerberus_s3_wifi_packet_count_exchange(CERBERUS_PACKET_COUNT_COMMAND_READ,&q,sizeof(q),&r,sizeof(r))==ESP_OK);
        CHECK(r.session_id==session&&r.pin==pin&&r.part==part);unsigned take=n-part*8U;if(take>8)take=8;
        memcpy(bytes+part*8U,r.data,take);for(unsigned i=take;i<8;++i)CHECK(!r.data[i]);}
}
static uint32_t peek(unsigned kind,unsigned index)
{cerberus_packet_count_peek_t q={session,(uint8_t)kind,(uint8_t)index};cerberus_packet_count_control_t r=exchange(CERBERUS_PACKET_COUNT_COMMAND_PEEK,&q,sizeof(q));CHECK(r.sequence&UINT32_C(0x80000000));return r.sequence;}
static unsigned emitted,terminal_seen,cleanup_retries;
static uint32_t expected_frames;
static bool recorded,expire_only,clock_fault,late_admission,runtime_expire;
static void invalidate_admission(void){atomic_store(&count_admission,++count_admission_counter);}
static void lifecycle(unsigned delay)
{
    CHECK(count_context);s3_packet_count_context_t *c=count_context;
    if(delay==250U){++cleanup_retries;CHECK(c->held&&c->stop&&!c->accepting&&cerberus_s3_wifi_owner()==CERBERUS_S3_WIFI_COUNT);
        CHECK(!c->table.window_open&&c->table.channel_window_us[0]==25000U);
        CHECK(control(CERBERUS_PACKET_COUNT_COMMAND_STATUS).flags&CERBERUS_PACKET_COUNT_HELD);return;}
    if(c->accepting&&!emitted){
        ++emitted;
        if(late_admission){timer_hook=invalidate_admission;emit(24,0);CHECK(!c->table.summary.callbacks);}
        if(clock_fault){now_us=(int64_t)c->table.window_started_us-1;emit(24,0);CHECK(c->stop&&!c->accepting);return;}
        emit(24,0);emit(24,8);emit(24,64);emit(4100,0);
        CHECK(c->table.summary.callbacks==4&&c->table.summary.matched_frames==4);
        CHECK(c->table.rows[0].observations==4&&c->table.rows[1].observations==4);
        CHECK(!(c->table.rows[0].flags&CERBERUS_WIFI_PACKET_COUNT_ROW_RSSI));
        if(recorded){
            CHECK(c->raw&&atomic_load(&c->raw->accepted)==2&&atomic_load(&c->raw->dropped_full)==1&&atomic_load(&c->raw->dropped_oversize)==1);
            uint8_t meta[28],row[96],same[28];uint32_t rawpin=peek(CERBERUS_PACKET_COUNT_READ_RAW_META,0);
            CHECK(!(rawpin&1U));read_pin(rawpin,meta,sizeof(meta));
            uint32_t metric=peek(CERBERUS_PACKET_COUNT_READ_ROW,1);CHECK(metric&1U);read_pin(metric,row,sizeof(row));
            read_pin(rawpin,same,sizeof(same));CHECK(!memcmp(meta,same,sizeof(meta)));
            ++c->inflight;
            cerberus_packet_count_peek_t summary_request={session,CERBERUS_PACKET_COUNT_READ_SUMMARY,0};
            cerberus_packet_count_control_t pending=exchange(CERBERUS_PACKET_COUNT_COMMAND_PEEK,&summary_request,sizeof(summary_request));
            CHECK(!pending.sequence&&pending.error_code==ESP_ERR_NOT_FINISHED&&!c->stop);
            --c->inflight;read_pin(metric,row,sizeof(row));
            uint32_t rawpin2=peek(CERBERUS_PACKET_COUNT_READ_RAW_META,0);CHECK(rawpin2!=rawpin&&!(rawpin2&1U));
            read_pin(metric,row,sizeof(row));
            CHECK(cerberus_packet_count_get(meta,4)==1&&cerberus_packet_count_get(meta+4,2)==24&&meta[20]==1&&meta[22]==3);
            cerberus_packet_count_read_t q={session,1,0};cerberus_packet_count_raw_chunk_t raw;
            CHECK(cerberus_s3_wifi_packet_count_exchange(CERBERUS_PACKET_COUNT_COMMAND_RAW_READ,&q,sizeof(q),&raw,sizeof(raw))==ESP_OK);
            CHECK(raw.pin==1&&raw.data[0]==8);for(unsigned i=24;i<40;++i)CHECK(!raw.data[i]);
            CHECK(count_crc(raw.data,24)==cerberus_packet_count_get(meta+16,4));
            q.pin=metric;CHECK(cerberus_s3_wifi_packet_count_exchange(CERBERUS_PACKET_COUNT_COMMAND_RAW_READ,&q,sizeof(q),&raw,sizeof(raw))==ESP_OK&&!raw.pin);
            cerberus_packet_count_ack_t ack={session,2};CHECK(!exchange(CERBERUS_PACKET_COUNT_COMMAND_RAW_ACK,&ack,sizeof(ack)).sequence);
            ack.sequence=1;CHECK(exchange(CERBERUS_PACKET_COUNT_COMMAND_RAW_ACK,&ack,sizeof(ack)).sequence==1);
            CHECK(exchange(CERBERUS_PACKET_COUNT_COMMAND_RAW_ACK,&ack,sizeof(ack)).sequence==1&&c->acked==1);
            read_pin(rawpin2,same,sizeof(same));CHECK(!memcmp(meta,same,sizeof(meta))); /* frozen metadata survives ACK */
        }else CHECK(!c->raw&&psram_allocations==0);
        uint64_t old=session;session+=100;CHECK(control(CERBERUS_PACKET_COUNT_COMMAND_STOP).error_code==ESP_ERR_NOT_FOUND);session=old;
        CHECK(c->accepting);
        if(runtime_expire){now_us=(int64_t)c->deadline_us;
            cerberus_packet_count_control_t expired=control(CERBERUS_PACKET_COUNT_COMMAND_STATUS);
            CHECK(expired.error_code==ESP_ERR_TIMEOUT&&c->stop&&!c->accepting);
        }else CHECK(control(CERBERUS_PACKET_COUNT_COMMAND_STOP).flags&CERBERUS_PACKET_COUNT_HELD);
        uint32_t before=c->table.summary.callbacks;emit(24,0);CHECK(c->table.summary.callbacks==before);
    }
    if(!c->held){
        ++terminal_seen;CHECK(!driver_initialized&&!driver_started&&!driver_receiving&&!rx_callback);
        if(c->data_ready)CHECK((c->error!=ESP_OK)==(fail_stage!=NULL||clock_fault||runtime_expire));
        else CHECK(c->error==ESP_OK||c->error==ESP_ERR_TIMEOUT);
        CHECK(cerberus_s3_wifi_owner()==CERBERUS_S3_WIFI_NONE&&!cerberus_s3_wifi_packet_count_active());
        if(c->data_ready){
            CHECK(c->table.summary.flags&CERBERUS_WIFI_PACKET_COUNT_STOPPED);
            CHECK(c->table.summary.callbacks==expected_frames);
            uint8_t summary[128];uint32_t pin=peek(CERBERUS_PACKET_COUNT_READ_SUMMARY,0);read_pin(pin,summary,sizeof(summary));
            CHECK(cerberus_packet_count_get(summary+20,4)==expected_frames&&summary[2]==0);
            uint64_t deadline=c->deadline_us;now_us+=1000;
            uint32_t calls=sdk_calls;CHECK(control(CERBERUS_PACKET_COUNT_COMMAND_START).flags&CERBERUS_PACKET_COUNT_BOUND);CHECK(sdk_calls==calls&&!c->accepting&&c->deadline_us==deadline);
            cerberus_packet_count_read_t bad={session,pin,65535};cerberus_packet_count_chunk_t rejected;
            CHECK(cerberus_s3_wifi_packet_count_exchange(CERBERUS_PACKET_COUNT_COMMAND_READ,&bad,sizeof(bad),&rejected,sizeof(rejected))==ESP_OK);
            CHECK(!rejected.pin&&c->deadline_us==deadline);
        }
        if(expire_only){now_us+=INT64_C(31000000);return;}
        CHECK(control(CERBERUS_PACKET_COUNT_COMMAND_RELEASE).flags&CERBERUS_PACKET_COUNT_BOUND);
    }
}
static void run(void)
{
    CHECK(queued_worker);void (*fn)(void *)=queued_worker;void *arg=worker_arg;queued_worker=NULL;worker_arg=NULL;
    emitted=terminal_seen=cleanup_retries=0;delay_hook=lifecycle;fn(arg);delay_hook=NULL;
    CHECK(!count_context&&!allocations&&!critical_depth&&task_deletes==1);
    CHECK(control(CERBERUS_PACKET_COUNT_COMMAND_RELEASE).error_code==0);
    CHECK(control(CERBERUS_PACKET_COUNT_COMMAND_STATUS).error_code==ESP_ERR_NOT_FOUND);
}
static void admission(void)
{
    reset();recorded=expire_only=clock_fault=late_admission=runtime_expire=false;expected_frames=4;
    CHECK(cerberus_s3_wifi_claim(CERBERUS_S3_WIFI_WEB)==ESP_OK);
    CHECK(begin(false).error_code==ESP_ERR_INVALID_STATE&&!queued_worker&&!allocations&&!sdk_calls);
    CHECK(cerberus_s3_wifi_owner()==CERBERUS_S3_WIFI_WEB);CHECK(cerberus_s3_wifi_release(CERBERUS_S3_WIFI_WEB)==ESP_OK);
    reset();ble_owner=CERBERUS_S3_BLE_OWNER_SCANNER;CHECK(begin(false).error_code==ESP_ERR_INVALID_STATE&&!allocations&&!sdk_calls);
    reset();ble_at_query=2;CHECK(begin(false).error_code==ESP_ERR_INVALID_STATE&&!allocations&&!queued_worker&&cerberus_s3_wifi_owner()==CERBERUS_S3_WIFI_NONE);
    for(unsigned fail=1;fail<=2;++fail){reset();fail_alloc=fail;CHECK(begin(true).error_code==ESP_ERR_NO_MEM&&!allocations&&!queued_worker&&cerberus_s3_wifi_owner()==CERBERUS_S3_WIFI_NONE);}
    reset();heap_free=65535;CHECK(begin(false).error_code==ESP_ERR_NO_MEM&&!alloc_calls);
    reset();task_fail=true;CHECK(begin(false).error_code==ESP_ERR_NO_MEM&&!allocations&&!queued_worker&&cerberus_s3_wifi_owner()==CERBERUS_S3_WIFI_NONE);
    reset();cerberus_packet_count_begin_t q={.session_id=2,.channel_mask=UINT64_C(1)<<14,.target_count=1,.dwell_ms=100};
    CHECK(exchange(CERBERUS_PACKET_COUNT_COMMAND_BEGIN,&q,sizeof(q)).error_code==ESP_ERR_INVALID_ARG&&!alloc_calls);
    q.channel_mask=1;q.reserved=1;CHECK(exchange(CERBERUS_PACKET_COUNT_COMMAND_BEGIN,&q,sizeof(q)).error_code==ESP_ERR_INVALID_ARG&&!alloc_calls);
    q.reserved=0;q.session_id=3;CHECK(exchange(CERBERUS_PACKET_COUNT_COMMAND_BEGIN,&q,sizeof(q)).error_code==ESP_ERR_INVALID_STATE&&!alloc_calls);
    q.session_id=2;CHECK(cerberus_s3_wifi_packet_count_exchange(CERBERUS_PACKET_COUNT_COMMAND_BEGIN,&q,sizeof(q),&q,sizeof(q))==ESP_ERR_INVALID_ARG&&q.session_id==2);
    now_us=INT64_MAX-100;CHECK(begin(false).error_code==ESP_ERR_INVALID_STATE&&!allocations&&!queued_worker);now_us=1000;
    reset();count_epoch=UINT64_MAX;CHECK(begin(false).error_code==ESP_ERR_INVALID_ARG&&!alloc_calls);count_epoch=1;
}
static void normal_and_faults(void)
{
    for(unsigned record=0;record<2;++record){reset();recorded=record;expected_frames=4;expire_only=clock_fault=late_admission=false;
        CHECK(!begin(record).error_code);CHECK(cerberus_s3_wifi_packet_count_active());configure();run();CHECK(emitted==1&&terminal_seen==1);}
    const char *setup[]={"netif","event","init","storage","mode","start","filter","callback","channel","enable"};
    for(unsigned i=0;i<sizeof(setup)/sizeof(setup[0]);++i){reset();recorded=false;expected_frames=0;
        CHECK(!begin(false).error_code);configure();fail_stage=setup[i];fail_remaining=100;
        run();CHECK(terminal_seen==1&&!emitted);}
    const char *cleanup[]={"disable","unregister","stop","deinit"};
    for(unsigned i=0;i<4;++i){reset();expected_frames=4;CHECK(!begin(false).error_code);configure();fail_stage=cleanup[i];fail_remaining=2;
        run();CHECK(cleanup_retries==2&&terminal_seen==1&&emitted==1);}
    reset();expected_frames=0;CHECK(!begin(false).error_code);configure();stop_in_enable=true;run();CHECK(!emitted&&disable_calls==1);
    reset();expected_frames=0;CHECK(!begin(false).error_code);CHECK(control(CERBERUS_PACKET_COUNT_COMMAND_STOP).flags&CERBERUS_PACKET_COUNT_HELD);run();CHECK(!sdk_calls);
    reset();expected_frames=0;CHECK(!begin(false).error_code);run();CHECK(!sdk_calls&&now_us>=INT64_C(10000000)); /* staging lease expiry */
    reset();expected_frames=4;expire_only=true;CHECK(!begin(false).error_code);configure();run();CHECK(terminal_seen==1);expire_only=false;
    reset();expected_frames=0;clock_fault=true;CHECK(!begin(false).error_code);configure();run();clock_fault=false;
    reset();expected_frames=4;late_admission=true;CHECK(!begin(false).error_code);configure();run();late_admission=false;
    reset();expected_frames=4;runtime_expire=true;CHECK(!begin(false).error_code);configure();run();runtime_expire=false;
}
static unsigned hop_seen;
static uint8_t hop_channel;
static void hop_lifecycle(unsigned delay)
{
    (void)delay;s3_packet_count_context_t *c=count_context;CHECK(c);
    if(c->accepting&&c->channel!=hop_channel){
        hop_channel=c->channel;++hop_seen;emit(24,0);
        CHECK(hop_channel==(hop_seen==2?6:1));
        if(hop_seen==3)CHECK(control(CERBERUS_PACKET_COUNT_COMMAND_STOP).flags&CERBERUS_PACKET_COUNT_HELD);
    }
    if(!c->held){
        CHECK(hop_seen==3&&c->table.summary.matched_frames==3&&c->table.summary.successful_channels==33);
        CHECK(c->table.channel_window_us[0]==75000&&c->table.channel_window_us[5]==50000);
        CHECK(!control(CERBERUS_PACKET_COUNT_COMMAND_RELEASE).error_code);
    }
}
static void channel_windows(void)
{
    reset();session=2;cerberus_packet_count_begin_t q={.session_id=2,.channel_mask=33,.target_count=2,.dwell_ms=50};
    CHECK(!exchange(CERBERUS_PACKET_COUNT_COMMAND_BEGIN,&q,sizeof(q)).error_code);configure();
    hop_seen=0;hop_channel=0;delay_hook=hop_lifecycle;
    void (*fn)(void *)=queued_worker;void *arg=worker_arg;queued_worker=NULL;worker_arg=NULL;fn(arg);delay_hook=NULL;
    CHECK(!count_context&&!allocations&&task_deletes==1);
}
static unsigned exhaustion_kind;
static void exhaustion_lifecycle(unsigned delay)
{
    (void)delay;s3_packet_count_context_t *c=count_context;CHECK(c);
    if(c->accepting&&!emitted){
        ++emitted;
        if(exhaustion_kind==1){c->table.summary.revision=UINT32_MAX-1U;emit(24,0);}
        else if(exhaustion_kind==2){atomic_store(&c->raw->next_sequence,CERBERUS_PACKET_COUNT_SEQUENCE_MAX-1U);emit(24,0);}
        else {
            c->pin_counter=CERBERUS_PACKET_COUNT_SEQUENCE_MAX-2U;
            (void)peek(CERBERUS_PACKET_COUNT_READ_ROW,0);
            cerberus_packet_count_peek_t q={session,CERBERUS_PACKET_COUNT_READ_ROW,0};
            cerberus_packet_count_control_t r=exchange(CERBERUS_PACKET_COUNT_COMMAND_PEEK,&q,sizeof(q));
            CHECK(!r.sequence&&r.error_code==ESP_ERR_INVALID_STATE&&c->stop);
        }
        CHECK(control(CERBERUS_PACKET_COUNT_COMMAND_STATUS).flags&CERBERUS_PACKET_COUNT_EXHAUSTED);
    }
    if(!c->held){
        CHECK(c->error==ESP_ERR_INVALID_STATE&&!c->accepting&&c->table.summary.flags&CERBERUS_WIFI_PACKET_COUNT_STOPPED);
        CHECK(c->table.summary.callbacks==(exhaustion_kind==3?0U:1U));
        CHECK(control(CERBERUS_PACKET_COUNT_COMMAND_RELEASE).flags&CERBERUS_PACKET_COUNT_BOUND);
    }
}
static void exhaustion(void)
{
    for(exhaustion_kind=1;exhaustion_kind<=3;++exhaustion_kind){
        reset();CHECK(!begin(exhaustion_kind==2).error_code);configure();emitted=0;delay_hook=exhaustion_lifecycle;
        void (*fn)(void *)=queued_worker;void *arg=worker_arg;queued_worker=NULL;worker_arg=NULL;fn(arg);delay_hook=NULL;
        CHECK(emitted==1&&!count_context&&!allocations&&task_deletes==1);
    }
}
int main(void)
{
    admission();normal_and_faults();channel_windows();exhaustion();reset();
    printf("S3 Packet Count producer PASS %u checks; context %zu bytes, optional ring %zu bytes\n",checks,sizeof(s3_packet_count_context_t),sizeof(cerberus_wifi_capture_ring_t));return 0;
}
