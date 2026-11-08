#include "wardrive_foreign_boundary.h"
#undef NDEBUG
#include "cerberus_protocol.h"
#include "wifi_channel_survey_metrics.h"
#include "esp_err.h"
#include <assert.h>
#include <setjmp.h>
#include <stdatomic.h>
static atomic_bool packet_count_radio_reserved;
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { ++checks; assert(x); } while (0)
#define portMAX_DELAY 0
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
#define WIFI_PROMIS_FILTER_MASK_MGMT 1U
#define WIFI_PROMIS_FILTER_MASK_CTRL 2U
#define WIFI_PROMIS_FILTER_MASK_DATA 4U
#define WIFI_BAND_2G 1
#define WIFI_BAND_5G 2
#define WIFI_SECOND_CHAN_NONE 0
#define WIFI_PKT_MISC 3
typedef unsigned wifi_promiscuous_pkt_type_t;
typedef struct { unsigned filter_mask; } wifi_promiscuous_filter_t;
typedef struct { struct { unsigned rx_state,sig_len,channel; int8_t rssi; } rx_ctrl; } wifi_promiscuous_pkt_t;
static int mutex,task,mux;
static void *scanner_mutex=&mutex,*channel_survey_task_handle=&task;
static bool locked,critical,scanner_ready=true,station_reserved;
static cerberus_wifi_scan_status_payload_t scan_status;
static cerberus_wifi_connection_status_payload_t connection_status;
static cerberus_wifi_defense_status_payload_t defense_status;
static cerberus_ble_scan_status_payload_t ble_status;
static cerberus_ble_connection_status_payload_t ble_connection;
/* Separate passive Probe owner; the actual admission body below checks it. */
static atomic_bool probe_sniff_radio_reserved;
static atomic_bool mac_radio_reserved,csi_radio_reserved,remote_id_radio_reserved,
    capture_radio_reserved,packet_monitor_radio_reserved;
/* Same production storage types, with no duplicate result bank. */
static cerberus_wifi_channel_survey_status_payload_t channel_survey_status;
static cerberus_wifi_channel_survey_result_payload_t channel_survey_results[CERBERUS_WIFI_CHANNEL_SURVEY_MAX_RESULTS];
static uint8_t channel_survey_channels[CERBERUS_WIFI_CHANNEL_SURVEY_MAX_RESULTS];
static int64_t channel_survey_started_us,clock_us;
static atomic_bool channel_survey_cancelled,channel_survey_radio_reserved;
#define channel_survey_mux mux
static bool channel_survey_receiving;
static uint8_t channel_survey_receive_channel;
static uint32_t channel_survey_packet_count;
static int64_t channel_survey_rssi_sum;
static int8_t channel_survey_strongest_rssi;
static unsigned notifications,enable_calls,disable_calls,channel_calls,delay_calls,worker_notifications;
static unsigned cancel_after_delay, cleanup_recover_after, cleanup_delays;
static bool clock_backwards, clock_stalled, expire_during_filter, start_during_preempt;
static unsigned extra_delay_us;
static esp_err_t nested_start_error;
static void (*delay_hook)(unsigned);
static esp_err_t cerberus_wifi_channel_survey_start(uint8_t band);
static uint32_t random_word = 0x12345678U;
static uint32_t esp_random(void){return ++random_word;}
static int filter_error,enable_error,disable_error,channel_error;
static bool sdk_promiscuous,worker_running,run_worker_during_wait,run_worker_on_notify,hold_worker;
static jmp_buf worker_exit;
static void run_worker(void);
static void channel_survey_receive(const wifi_promiscuous_pkt_t *, wifi_promiscuous_pkt_type_t);
static void cerberus_wifi_scanner_get_status(cerberus_wifi_scan_status_payload_t *out);
static void xSemaphoreTake(void *m,unsigned t){(void)t;CHECK(m==scanner_mutex&&!locked);locked=true;}
static void xSemaphoreGive(void *m){CHECK(m==scanner_mutex&&locked);locked=false;}
#define portENTER_CRITICAL(m) do { CHECK((m)==&mux&&!critical);critical=true; } while(0)
#define portEXIT_CRITICAL(m) do { CHECK((m)==&mux&&critical);critical=false; } while(0)
static int64_t esp_timer_get_time(void){return clock_us;}
static bool cerberus_c5_test_running(void){return false;}
static bool cerberus_ieee802154_scanner_is_running(void){return false;}
static bool cerberus_wifi_defense_survey_is_running(void){return defense_status.state==CERBERUS_WIFI_DEFENSE_RUNNING;}
static bool cerberus_wifi_station_running(void){return station_reserved;}
static void cerberus_ble_scanner_get_status(cerberus_ble_scan_status_payload_t *out){*out=ble_status;}
static void cerberus_ble_connection_get_status(cerberus_ble_connection_status_payload_t *out){*out=ble_connection;}
static void cerberus_wifi_connection_get_status(cerberus_wifi_connection_status_payload_t *out){*out=connection_status;}
static void xTaskNotifyGive(void *t){CHECK(t==channel_survey_task_handle);++notifications;if(run_worker_on_notify)run_worker();}
static void ulTaskNotifyTake(int clear,unsigned timeout)
{(void)timeout;CHECK(clear==pdTRUE);if(worker_notifications++)longjmp(worker_exit,1);}
static int esp_wifi_set_promiscuous_filter(const wifi_promiscuous_filter_t *f)
{CHECK(f->filter_mask==7U);if(expire_during_filter)clock_us+=11000000;return filter_error;}
static int esp_wifi_set_promiscuous(bool on)
{CHECK(!locked);if(on){++enable_calls;if(!enable_error)sdk_promiscuous=true;return enable_error;}
 ++disable_calls;if(!disable_error)sdk_promiscuous=false;return disable_error;}
static int esp_wifi_set_band(int band){CHECK(band==1||band==2);return channel_error;}
static int esp_wifi_set_channel(uint8_t channel,int second)
{CHECK(channel&&second==0);++channel_calls;return channel_error;}
static void vTaskDelay(unsigned ms)
{
    CHECK(!locked);
    if (!clock_stalled) clock_us+=(int64_t)ms*1000;
    if (worker_running && ms==10U) clock_us+=extra_delay_us;
    if (clock_backwards) { clock_backwards=false; clock_us=-1; }
    if (start_during_preempt) { start_during_preempt=false; nested_start_error=cerberus_wifi_channel_survey_start(2); }
    if (ms==250) {
        ++cleanup_delays;
        CHECK(sdk_promiscuous && atomic_load(&channel_survey_radio_reserved));
        CHECK(channel_survey_status.state==CERBERUS_WIFI_CHANNEL_SURVEY_RUNNING);
        if(cleanup_recover_after && cleanup_delays>=cleanup_recover_after) disable_error=ESP_OK;
    }
    if(delay_hook)delay_hook(ms);
    if(worker_running){
        ++delay_calls;
        wifi_promiscuous_pkt_t packet={.rx_ctrl={.sig_len=24,.channel=channel_survey_receive_channel,.rssi=-51}};
        channel_survey_receive(&packet,0);
        cerberus_wifi_scan_status_payload_t projected;cerberus_wifi_scanner_get_status(&projected);
        CHECK(projected.state==CERBERUS_WIFI_SCAN_RUNNING);
        if(cancel_after_delay&&delay_calls>=cancel_after_delay)
            atomic_store(&channel_survey_cancelled,true);
    }else if(run_worker_during_wait&&!hold_worker)run_worker();
}
#include "survey_admission.h"
#include "wifi_channel_survey_service.h"
static void run_worker(void)
{
    CHECK(!worker_running);worker_running=true;worker_notifications=0;
    if(!setjmp(worker_exit))channel_survey_task(NULL);
    worker_running=false;CHECK(!locked&&!critical);
}

/* Actual main dispatch and S3 transport validators, with only frame exchange
 * and link mutex boundaries replaced. A lost reply can occur after mutation. */
static uint8_t response_bytes[22],response_command,response_sequence;
static size_t response_size;
static void prepare_frame_response(cerberus_spi_command_t command,uint8_t sequence,const void *data,size_t size)
{CHECK(size<=sizeof(response_bytes));memcpy(response_bytes,data,size);response_size=size;response_command=command;response_sequence=sequence;}
typedef struct { uint8_t command,sequence,payload_length,payload[22]; } test_request_t;
static void dispatch(const test_request_t *request)
{switch(request->command){
#include "survey_dispatch.h"
default:CHECK(false);}}

static void reset(void)
{
    CHECK(!locked&&!critical&&!worker_running);
    memset(&scan_status,0,sizeof(scan_status));memset(&connection_status,0,sizeof(connection_status));
    memset(&ble_status,0,sizeof(ble_status));memset(&ble_connection,0,sizeof(ble_connection));
    memset(&defense_status,0,sizeof(defense_status));
    memset(&channel_survey_status,0,sizeof(channel_survey_status));
    channel_survey_status.dwell_ms=100;memset(channel_survey_results,0,sizeof(channel_survey_results));
    atomic_store(&channel_survey_cancelled,false);atomic_store(&channel_survey_radio_reserved,false);
    scanner_ready=true;station_reserved=false;notifications=enable_calls=disable_calls=channel_calls=delay_calls=0;
    filter_error=enable_error=disable_error=channel_error=0;sdk_promiscuous=false;
    cancel_after_delay=0;run_worker_during_wait=true;run_worker_on_notify=false;hold_worker=false;clock_us=1000000;
    cleanup_recover_after=cleanup_delays=extra_delay_us=0;
    clock_backwards=clock_stalled=expire_during_filter=start_during_preempt=false;
    delay_hook=NULL;nested_start_error=ESP_OK;channel_survey_session_epoch=100;channel_survey_session_id=0;
    channel_survey_lease_deadline_us=channel_survey_lease_clock_us=0;channel_survey_control_busy=false;
    channel_survey_pin_counter=channel_survey_pin=0;channel_survey_pin_session=0;
    channel_survey_pin_kind=channel_survey_pin_index=0;memset(channel_survey_pin_bytes,0,22);
}
static cerberus_wifi_channel_survey_session_status_t begin(uint8_t band)
{
    cerberus_wifi_channel_survey_session_start_t q={channel_survey_session_epoch+1U,band};
    cerberus_wifi_channel_survey_session_status_t out;
    CHECK(cerberus_wifi_channel_survey_session_start(&q,&out)==ESP_OK);
    CHECK(out.session_id==q.session_id && out.band==band && out.flags==3 && out.state==1);
    CHECK(ble_other_radio_active());
    return out;
}
static void cancel_and_drain(uint64_t token)
{
    cerberus_wifi_channel_survey_session_status_t out;
    unsigned delay=delay_calls;
    CHECK(cerberus_wifi_channel_survey_session_stop(token,&out)==ESP_OK);
    CHECK(out.flags==7 && delay_calls==delay);
    run_worker();
    CHECK(cerberus_wifi_channel_survey_session_status(token,&out)==ESP_OK && out.flags==5 && out.state==0);
    CHECK(!ble_other_radio_active());
}
static void replay_reset_and_exclusion(void)
{
    reset();cerberus_wifi_channel_survey_session_status_t info;
    cerberus_wifi_channel_survey_session_info(&info);CHECK(info.session_id==100 && info.flags==0);
    /* Reboot between read-only INFO and START: exact proposed identity rejected
     * before notifying the worker. This was an orphan with predicted gen8. */
    cerberus_wifi_channel_survey_session_start_t q={101,1};channel_survey_session_epoch=500;
    CHECK(cerberus_wifi_channel_survey_session_start(&q,&info)==ESP_ERR_INVALID_STATE);
    CHECK(info.session_id==101&&!info.flags&&!info.state&&!info.band&&!notifications&&!enable_calls);
    q.session_id=501;CHECK(cerberus_wifi_channel_survey_session_start(&q,&info)==ESP_OK);
    uint8_t gen=info.generation;int64_t deadline=channel_survey_lease_deadline_us;
    CHECK(notifications==1);
    /* Lost accepted reply retransmission is the exact same job, no rearm or renewal. */
    clock_us+=100000;
    CHECK(cerberus_wifi_channel_survey_session_start(&q,&info)==ESP_OK);
    CHECK(info.session_id==501&&info.generation==gen&&notifications==1&&channel_survey_lease_deadline_us==deadline);
    q.band=2;CHECK(cerberus_wifi_channel_survey_session_start(&q,&info)==ESP_ERR_INVALID_ARG);
    CHECK(info.flags==3&&info.band==1&&notifications==1);
    int64_t before_clock=clock_us;clock_us=deadline;
    CHECK(cerberus_wifi_channel_survey_session_start(&q,&info)==ESP_ERR_INVALID_ARG);
    CHECK(!atomic_load(&channel_survey_cancelled)&&channel_survey_lease_deadline_us==deadline);
    clock_us=before_clock;q.band=1;
    CHECK(cerberus_wifi_channel_survey_start(1)==ESP_ERR_INVALID_STATE);
    CHECK(cerberus_wifi_channel_survey_legacy_stop()==ESP_ERR_INVALID_STATE);
    CHECK(!atomic_load(&channel_survey_cancelled)&&notifications==1);
    CHECK(cerberus_wifi_channel_survey_session_stop(500,&info)==ESP_ERR_NOT_FOUND);
    CHECK(info.session_id==500&&!info.flags&&!info.band&&!atomic_load(&channel_survey_cancelled));
    cancel_and_drain(501);
    CHECK(cerberus_wifi_channel_survey_session_start(&q,&info)==ESP_OK);
    CHECK(info.flags==5&&!info.state&&notifications==1); /* No rearm after Stop. */
    (void)begin(2);CHECK(channel_survey_session_id==502);
    CHECK(cerberus_wifi_channel_survey_session_stop(501,&info)==ESP_ERR_NOT_FOUND);
    CHECK(!atomic_load(&channel_survey_cancelled));
    cancel_and_drain(502);
    /* Legacy replacement invalidates old scoped data/read authority. */
    CHECK(cerberus_wifi_channel_survey_start(1)==ESP_OK);
    CHECK(cerberus_wifi_channel_survey_session_status(502,&info)==ESP_ERR_NOT_FOUND&&!info.flags);
    CHECK(cerberus_wifi_channel_survey_preempt()==ESP_OK);
    CHECK(cerberus_wifi_channel_survey_legacy_stop()==ESP_OK);
    reset();channel_survey_session_init_locked();CHECK(channel_survey_session_epoch!=100&&channel_survey_session_epoch);
}
static void cleanup_observer(unsigned ms)
{
    if(ms!=250)return;
    cerberus_wifi_channel_survey_session_status_t out;
    uint64_t token=channel_survey_session_id;
    int64_t deadline=channel_survey_lease_deadline_us;
    CHECK(cerberus_wifi_channel_survey_session_status(token,&out)==ESP_OK);
    CHECK(out.flags==7 && out.state==CERBERUS_WIFI_CHANNEL_SURVEY_RUNNING);
    CHECK(out.error_code==ESP_FAIL && channel_survey_lease_deadline_us==deadline);
    cerberus_wifi_channel_survey_session_start_t next={token+1U,2};
    CHECK(cerberus_wifi_channel_survey_session_start(&next,&out)==ESP_ERR_INVALID_STATE && !out.flags);
    CHECK(cerberus_wifi_channel_survey_session_stop(token,&out)==ESP_OK && out.flags==7);
    CHECK(cerberus_wifi_channel_survey_legacy_stop()==ESP_ERR_INVALID_STATE);
    CHECK(ble_other_radio_active());
}
static void lease_expiry_and_cleanup(void)
{
    reset();cerberus_wifi_channel_survey_session_status_t info=begin(1);uint64_t token=info.session_id;
    int64_t deadline=channel_survey_lease_deadline_us;
    clock_us=deadline-1;cerberus_wifi_channel_survey_session_info(&info);
    CHECK(channel_survey_lease_deadline_us==deadline&&!atomic_load(&channel_survey_cancelled));
    CHECK(cerberus_wifi_channel_survey_session_status(token,&info)==ESP_OK && info.flags==3);
    CHECK(channel_survey_lease_deadline_us==clock_us+10000000);
    clock_us=channel_survey_lease_deadline_us;
    CHECK(cerberus_wifi_channel_survey_session_status(token,&info)==ESP_OK && info.flags==7);
    int64_t expired=channel_survey_lease_deadline_us;clock_us+=1;
    CHECK(cerberus_wifi_channel_survey_session_status(token,&info)==ESP_OK && info.flags==7);
    CHECK(channel_survey_lease_deadline_us==expired);
    run_worker();CHECK(!enable_calls&&!ble_other_radio_active()); /* Queued expiry. */
    cerberus_wifi_channel_survey_session_start_t q={token,1};
    CHECK(cerberus_wifi_channel_survey_session_start(&q,&info)==ESP_OK&&info.flags==5&&notifications==1);
    reset();info=begin(2);run_worker(); /* No client: actual dwell worker expires. */
    CHECK(clock_us>=11000000&&clock_us<11100000&&enable_calls==1&&disable_calls==1&&!sdk_promiscuous);
    CHECK(!ble_other_radio_active()&&channel_survey_status.total_sweeps>0);
    reset();info=begin(1);clock_backwards=true;run_worker();
    CHECK(disable_calls==1&&!ble_other_radio_active());
    reset();info=begin(1);clock_us=-1;
    CHECK(cerberus_wifi_channel_survey_session_status(info.session_id,&info)==ESP_OK&&info.flags==7);
    run_worker();CHECK(enable_calls==0);
    reset();info=begin(1);expire_during_filter=true;run_worker();
    CHECK(enable_calls==0&&!ble_other_radio_active());
    reset();info=begin(1);cancel_after_delay=1;disable_error=ESP_FAIL;cleanup_recover_after=5;delay_hook=cleanup_observer;
    run_worker();CHECK(cleanup_delays==5&&disable_calls==6&&clock_us>=2260000);
    CHECK(!sdk_promiscuous&&!ble_other_radio_active()&&channel_survey_status.error_code==ESP_OK);
    CHECK(cerberus_wifi_channel_survey_session_status(info.session_id,&info)==ESP_OK&&info.flags==5);
    /* Error before SDK reservation still retains exact historical identity. */
    reset();info=begin(1);filter_error=ESP_FAIL;run_worker();
    CHECK(cerberus_wifi_channel_survey_session_status(info.session_id,&info)==ESP_OK&&info.flags==5&&info.error_code==ESP_FAIL);
    CHECK(!enable_calls&&!ble_other_radio_active());
    reset();clock_us=INT64_MAX-9999999;
    q=(cerberus_wifi_channel_survey_session_start_t){101,1};
    CHECK(cerberus_wifi_channel_survey_session_start(&q,&info)==ESP_ERR_INVALID_STATE&&!notifications);
}
static void measured_receive_windows(void)
{
    /* Two delayed ticks complete a 120 ms window. A cancellation in the next
     * window must not publish its partial count, even if callbacks arrived. */
    reset();begin(1);extra_delay_us=50000;cancel_after_delay=3;run_worker();
    CHECK(channel_survey_results[0].sample_count==1);
    CHECK(channel_survey_results[0].last_packets==2&&channel_survey_results[0].total_packets==2);
    CHECK(channel_survey_results[0].rate_per_sec==16&&channel_survey_results[0].dwell_ms==120);
    CHECK(channel_survey_results[0].average_rssi==-51&&channel_survey_results[0].updated_ms==120);
    CHECK(!channel_survey_results[1].seen&&!channel_survey_results[1].sample_count);
    CHECK(channel_survey_status.total_packets==2&&channel_survey_status.error_code==ESP_OK);
    CHECK(channel_survey_status.dwell_ms==100&&!sdk_promiscuous&&!ble_other_radio_active());

    /* Timer faults release the receiver without publishing plausible rates. */
    for(unsigned fault=0;fault<2;++fault){
        reset();begin(1);clock_stalled=fault==0;clock_backwards=fault==1;run_worker();
        CHECK(channel_survey_status.state==CERBERUS_WIFI_CHANNEL_SURVEY_ERROR);
        CHECK(channel_survey_status.error_code==ESP_ERR_INVALID_STATE);
        CHECK(!channel_survey_status.total_packets&&!channel_survey_results[0].seen);
        CHECK(disable_calls==1&&!sdk_promiscuous&&!ble_other_radio_active());
    }
    reset();channel_survey_receiving=true;channel_survey_receive_channel=6;
    channel_survey_packet_count=UINT32_MAX;channel_survey_rssi_sum=-(int64_t)UINT32_MAX*51;
    channel_survey_strongest_rssi=-51;
    wifi_promiscuous_pkt_t packet={.rx_ctrl={.sig_len=24,.channel=6,.rssi=-40}};
    channel_survey_receive(&packet,0);
    CHECK(channel_survey_packet_count==UINT32_MAX&&channel_survey_rssi_sum==-(int64_t)UINT32_MAX*51);
    CHECK(channel_survey_strongest_rssi==-40);channel_survey_receiving=false;
}
static void immutable_projection(void)
{
    reset();cerberus_wifi_channel_survey_session_status_t info=begin(1);uint64_t token=info.session_id;
    for(unsigned kind=0;kind<2;++kind){
        uint8_t expected[22],actual[22];
        channel_survey_status.total_packets=123456;channel_survey_status.current_channel=6;
        channel_survey_results[2]=(cerberus_wifi_channel_survey_result_payload_t){
            .generation=info.generation,.index=2,.channel=3,.seen=1,.total_packets=UINT32_MAX,.last_packets=60000,
            .rate_per_sec=65000,.average_rssi=-67,.strongest_rssi=-20,.sample_count=UINT16_MAX,.updated_ms=12345,.dwell_ms=100};
        if(kind==0){cerberus_wifi_channel_survey_status_payload_t status;cerberus_wifi_channel_survey_get_status(&status);memcpy(expected,&status,22);}
        else memcpy(expected,&channel_survey_results[2],22);
        cerberus_wifi_channel_survey_session_read_t q={token,0,(uint8_t)kind,kind?2:0,0};
        cerberus_wifi_channel_survey_session_chunk_t out;
        int64_t deadline=channel_survey_lease_deadline_us;
        CHECK(cerberus_wifi_channel_survey_session_read(&q,&out)==ESP_OK&&out.pin&&out.session_id==token);
        memcpy(actual,out.data,9);q.pin=out.pin;
        channel_survey_status.total_packets=42;memset(&channel_survey_results[2],0,22);clock_us+=456789;
        for(unsigned part=1;part<3;++part){q.part=(uint8_t)part;
            CHECK(cerberus_wifi_channel_survey_session_read(&q,&out)==ESP_OK&&out.pin==q.pin&&out.part==part);
            memcpy(actual+part*9,out.data,part==2?4:9);
            if(part==2)for(unsigned n=4;n<9;++n)CHECK(out.data[n]==0);
        }
        CHECK(!memcmp(actual,expected,22)&&channel_survey_lease_deadline_us==deadline);
        /* Wrong pin/session/kind/index and every invalid part cannot borrow bytes. */
        for(unsigned mutation=0;mutation<8;++mutation){
            cerberus_wifi_channel_survey_session_read_t bad=q;
            switch(mutation){case 0:++bad.pin;break;case 1:--bad.session_id;break;case 2:bad.kind=2;break;
            case 3:bad.index=25;break;case 4:bad.part=3;break;case 5:bad.part=255;break;case 6:bad.pin=0;break;case 7:bad.part=0;break;}
            CHECK(cerberus_wifi_channel_survey_session_read(&bad,&out)!=ESP_OK&&out.pin==0&&out.session_id==bad.session_id&&out.part==bad.part);
            for(unsigned n=4;n<9;++n)CHECK(out.data[n]==0);
        }
        cerberus_wifi_channel_survey_session_read_t newer={token,0,0,0,0};
        CHECK(cerberus_wifi_channel_survey_session_read(&newer,&out)==ESP_OK&&out.pin!=q.pin);
        CHECK(cerberus_wifi_channel_survey_session_read(&q,&out)==ESP_ERR_NOT_FOUND);
    }
    cerberus_wifi_channel_survey_session_read_t q={token,0,0,0,0};cerberus_wifi_channel_survey_session_chunk_t out;
    channel_survey_pin_counter=UINT32_MAX-1;
    CHECK(cerberus_wifi_channel_survey_session_read(&q,&out)==ESP_OK&&out.pin==UINT32_MAX);
    CHECK(cerberus_wifi_channel_survey_session_read(&q,&out)==ESP_ERR_INVALID_STATE&&out.pin==0);
    cancel_and_drain(token);(void)begin(2);q.pin=UINT32_MAX;q.part=1;
    CHECK(cerberus_wifi_channel_survey_session_read(&q,&out)==ESP_ERR_NOT_FOUND);
}
static void generation_alias_and_native_arbiter(void)
{
    reset();cerberus_wifi_channel_survey_session_status_t first=begin(1),out;
    uint64_t first_token=first.session_id;uint8_t first_gen=first.generation;
    cancel_and_drain(first_token);
    for(unsigned i=0;i<255;++i){out=begin(1);if(i<254)cancel_and_drain(out.session_id);}
    CHECK(out.generation==first_gen&&out.session_id!=first_token);
    CHECK(cerberus_wifi_channel_survey_session_stop(first_token,&out)==ESP_ERR_NOT_FOUND);
    CHECK(!atomic_load(&channel_survey_cancelled)&&ble_other_radio_active());
    /* Trusted arbiter can cancel scoped work, legacy wire cannot. */
    CHECK(cerberus_wifi_channel_survey_legacy_stop()==ESP_ERR_INVALID_STATE);
    hold_worker=true;start_during_preempt=true;
    CHECK(cerberus_wifi_channel_survey_preempt()==ESP_ERR_TIMEOUT);
    CHECK(nested_start_error==ESP_ERR_INVALID_STATE&&ble_other_radio_active());
    hold_worker=false;CHECK(cerberus_wifi_channel_survey_preempt()==ESP_OK&&!ble_other_radio_active());
    reset();channel_survey_session_epoch=UINT64_MAX-1;out=begin(1);CHECK(out.session_id==UINT64_MAX);
    cancel_and_drain(UINT64_MAX);
    cerberus_wifi_channel_survey_session_start_t q={UINT64_MAX,1};
    CHECK(cerberus_wifi_channel_survey_session_start(&q,&out)==ESP_OK&&!out.state);
    q.session_id=0;CHECK(cerberus_wifi_channel_survey_session_start(&q,&out)==ESP_ERR_INVALID_ARG);
    q.session_id=1;CHECK(cerberus_wifi_channel_survey_session_start(&q,&out)==ESP_ERR_INVALID_STATE);
    CHECK(notifications==1);
}
static void actual_dispatch(void)
{
    reset();test_request_t q={.command=CERBERUS_SPI_COMMAND_WIFI_CHANNEL_SURVEY_SESSION_INFO,.sequence=91};
    dispatch(&q);CHECK(response_size==22&&response_sequence==91&&response_command==q.command);
    cerberus_wifi_channel_survey_session_status_t out;memcpy(&out,response_bytes,22);CHECK(out.session_id==100);
    cerberus_wifi_channel_survey_session_start_t start={101,1};
    q.command=CERBERUS_SPI_COMMAND_WIFI_CHANNEL_SURVEY_SESSION_START;q.payload_length=sizeof(start);memcpy(q.payload,&start,sizeof(start));
    dispatch(&q);memcpy(&out,response_bytes,22);CHECK(out.flags==3&&out.session_id==101&&notifications==1);
    dispatch(&q);CHECK(notifications==1); /* Lost response replay. */
    q.command=CERBERUS_SPI_COMMAND_WIFI_CHANNEL_SURVEY_STOP;q.payload_length=0;dispatch(&q);
    cerberus_wifi_channel_survey_status_payload_t legacy;memcpy(&legacy,response_bytes,22);
    CHECK(legacy.error_code==ESP_ERR_INVALID_STATE&&!atomic_load(&channel_survey_cancelled));
    cerberus_wifi_channel_survey_session_request_t bound={100};
    q.command=CERBERUS_SPI_COMMAND_WIFI_CHANNEL_SURVEY_SESSION_STOP;q.payload_length=8;memcpy(q.payload,&bound,8);
    dispatch(&q);memcpy(&out,response_bytes,22);CHECK(out.session_id==100&&!out.flags&&out.error_code==ESP_ERR_NOT_FOUND);
    bound.session_id=101;memcpy(q.payload,&bound,8);dispatch(&q);memcpy(&out,response_bytes,22);
    CHECK(out.flags==7&&disable_calls==0);run_worker();
    q.command=CERBERUS_SPI_COMMAND_WIFI_CHANNEL_SURVEY_SESSION_STATUS;dispatch(&q);memcpy(&out,response_bytes,22);CHECK(out.flags==5);
    cerberus_wifi_channel_survey_session_read_t read={101,0,0,0,0};
    q.command=CERBERUS_SPI_COMMAND_WIFI_CHANNEL_SURVEY_SESSION_READ;q.payload_length=sizeof(read);memcpy(q.payload,&read,sizeof(read));
    dispatch(&q);cerberus_wifi_channel_survey_session_chunk_t chunk;memcpy(&chunk,response_bytes,22);CHECK(chunk.pin&&chunk.session_id==101);
    for(unsigned command=135;command<=139;++command){q.command=(uint8_t)command;q.payload_length=21;response_size=0;dispatch(&q);CHECK(response_size==0);}
}
int main(void)
{
    reset();atomic_store(&probe_sniff_radio_reserved,true);
    cerberus_wifi_channel_survey_session_start_t blocked={101,1};
    cerberus_wifi_channel_survey_session_status_t blocked_reply;
    CHECK(cerberus_wifi_channel_survey_session_start(&blocked,&blocked_reply)==ESP_ERR_INVALID_STATE);
    CHECK(!notifications&&!enable_calls&&!channel_survey_session_id&&ble_other_radio_active());
    CHECK(cerberus_wifi_channel_survey_start(1)==ESP_ERR_INVALID_STATE&&!notifications);
    atomic_store(&probe_sniff_radio_reserved,false);
    { atomic_store(&packet_count_radio_reserved,true);
    cerberus_wifi_channel_survey_session_start_t blocked={101,1};
    cerberus_wifi_channel_survey_session_status_t blocked_reply;
    CHECK(cerberus_wifi_channel_survey_session_start(&blocked,&blocked_reply)==ESP_ERR_INVALID_STATE);
    CHECK(!notifications&&!enable_calls&&!channel_survey_session_id&&ble_other_radio_active());
    CHECK(cerberus_wifi_channel_survey_start(1)==ESP_ERR_INVALID_STATE&&!notifications);
    atomic_store(&packet_count_radio_reserved,false); }

    replay_reset_and_exclusion();lease_expiry_and_cleanup();measured_receive_windows();immutable_projection();generation_alias_and_native_arbiter();actual_dispatch();
    printf("Scoped Channel Survey: %u assertions passed; actual worker/dispatch, reboot rejection, replay, expiry/failed disable, stale control, immutable metrics and exhaustion.\n",checks);
    return 0;
}
