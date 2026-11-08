#undef NDEBUG
#include "esp_err.h"
#include "cerberus_protocol.h"
#include "c5_spi_link.h"
#include "wifi_ap_survey_protocol.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x) do { ++checks;assert(x); } while(0)
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(x) (x)
#define pdTRUE 1
#define MALLOC_CAP_INTERNAL 1U
#define MALLOC_CAP_8BIT 2U
#define WIFI_PROMIS_FILTER_MASK_MGMT 1U
#define WIFI_PROMIS_FILTER_MASK_DATA 4U
#define WIFI_BAND_MODE_AUTO 3
#define WIFI_BAND_2G 1
#define WIFI_BAND_5G 2
#define WIFI_SECOND_CHAN_NONE 0
#define WIFI_PKT_MGMT 0U
#define WIFI_PKT_CTRL 1U
#define WIFI_PKT_DATA 2U
#define WIFI_PKT_MISC 3U
typedef unsigned wifi_promiscuous_pkt_type_t;
typedef int wifi_band_mode_t;
typedef int wifi_second_chan_t;
typedef struct { unsigned filter_mask; } wifi_promiscuous_filter_t;
typedef struct { uint8_t schan,nchan; } wifi_country_t;
typedef struct { unsigned sig_len,rx_state,channel;int rssi,noise_floor; } wifi_pkt_rx_ctrl_t;
typedef struct { wifi_pkt_rx_ctrl_t rx_ctrl;uint8_t payload[2400]; } wifi_promiscuous_pkt_t;
static int mutex,task,ap_survey_mux,defense_capture_mux;
static void *scanner_mutex=&mutex,*scanner_task_handle=&task,*link_mutex=&task;
static bool locked,critical,foreign_radio,scanner_ready=true,station_reserved;
static atomic_bool ap_survey_radio_reserved,probe_sniff_radio_reserved,packet_count_radio_reserved,
    channel_survey_radio_reserved,mac_radio_reserved,csi_radio_reserved,remote_id_radio_reserved,
    capture_radio_reserved,packet_monitor_radio_reserved,relation_receiving;
static atomic_uint ap_survey_admission,mac_admission_token,wifi_fox_hunt_admission_token;
static cerberus_wifi_scan_status_payload_t scan_status;
static cerberus_wifi_defense_status_payload_t defense_status;
static int64_t now_us=1000000;
static unsigned sdk_calls,allocations,frees,notifications,enable_calls,disable_calls;
static unsigned fail_sdk_at,fail_channel;
static int disable_error,restore_error;
static bool allocation_fail,sdk_receiving;
static wifi_band_mode_t sdk_mode=7;
static uint8_t sdk_channel=11;
static wifi_second_chan_t sdk_second=1;
static wifi_promiscuous_filter_t sdk_filter={9U};
static void (*clock_hook)(void),(*decode_hook)(void);
static int64_t esp_timer_get_time(void)
{ if(clock_hook){void(*hook)(void)=clock_hook;clock_hook=NULL;hook();}return now_us; }
static uint32_t esp_random(void){return 12345U;}
static void xSemaphoreTake(void *m,unsigned timeout){CHECK(m==&mutex&&timeout==portMAX_DELAY&&!locked);locked=true;}
static void xSemaphoreGive(void *m){CHECK(m==&mutex&&locked);locked=false;}
static void enter(int *m){CHECK((m==&ap_survey_mux||m==&defense_capture_mux)&&!critical);critical=true;}
static void leave(int *m){CHECK(critical);critical=false;(void)m;if(decode_hook){void(*hook)(void)=decode_hook;decode_hook=NULL;hook();}}
#define portENTER_CRITICAL(m) enter(m)
#define portEXIT_CRITICAL(m) leave(m)
static void xTaskNotifyGive(void *t){CHECK(t==&task);++notifications;}
static bool cerberus_c5_test_running(void){return false;}
static bool another_radio_operation_is_active(void){return foreign_radio||atomic_load(&ap_survey_radio_reserved);}
static bool wifi_connection_is_active_locked(void){CHECK(locked);return false;}
static bool cerberus_wardrive_radio_held(void){return false;}
static int sdk(void){CHECK(!locked&&!critical);++sdk_calls;return sdk_calls==fail_sdk_at?ESP_FAIL:ESP_OK;}
static esp_err_t esp_wifi_get_band_mode(wifi_band_mode_t *o){int e=sdk();if(!e)*o=sdk_mode;return e;}
static esp_err_t esp_wifi_get_channel(uint8_t *o,wifi_second_chan_t *s){int e=sdk();if(!e){*o=sdk_channel;*s=sdk_second;}return e;}
static esp_err_t esp_wifi_get_promiscuous_filter(wifi_promiscuous_filter_t *o){int e=sdk();if(!e)*o=sdk_filter;return e;}
static esp_err_t esp_wifi_get_country(wifi_country_t *o){int e=sdk();if(!e){o->schan=1;o->nchan=11;}return e;}
static esp_err_t esp_wifi_set_band_mode(wifi_band_mode_t v){int e=sdk();if(v==7&&restore_error)e=restore_error;if(!e)sdk_mode=v;return e;}
static esp_err_t esp_wifi_set_band(int b){CHECK(b==1||b==2);return sdk();}
static esp_err_t esp_wifi_set_channel(uint8_t ch,wifi_second_chan_t s){int e=sdk();if(ch==fail_channel)e=ESP_ERR_INVALID_ARG;if(!e){sdk_channel=ch;sdk_second=s;}return e;}
static esp_err_t esp_wifi_set_promiscuous_filter(const wifi_promiscuous_filter_t *v){int e=sdk();if(!e)sdk_filter=*v;return e;}
static esp_err_t esp_wifi_set_promiscuous(bool on){int e=sdk();if(on)++enable_calls;else{++disable_calls;if(disable_error)e=disable_error;}if(!e)sdk_receiving=on;return e;}
static void *allocation;
static void *heap_caps_calloc(size_t n,size_t bytes,unsigned caps){CHECK(!locked&&!critical&&n==1&&caps==3&&!allocation);if(allocation_fail)return NULL;allocation=calloc(n,bytes);CHECK(allocation);++allocations;return allocation;}
static void heap_caps_free(void *p){CHECK(!locked&&!critical&&p==allocation);free(p);allocation=NULL;++frees;}
#include "wifi_ap_survey_service.h"

/* Actual outer callback, with unrelated consumers inactive. */
static cerberus_wifi_probe_table_t probe_capture;
static cerberus_wifi_relation_table_t relation_capture;
static struct { struct { uint8_t generation; } status;uint64_t capture_epoch; } watch_capture;
static bool watch_capture_active,defense_capture_active;
static uint8_t watch_capture_channel;
static uint64_t watch_capture_armed_us,watch_capture_deadline_us;
static uint32_t watch_callback_inflight,defense_management_frames,defense_control_frames,defense_data_frames,
    defense_beacon_frames,defense_deauth_frames,defense_disassoc_frames,defense_noise_samples;
static int64_t defense_noise_sum;
static int8_t defense_strongest_rssi;
#define SENTINEL_WATCH_PROTOCOL_PWNAGOTCHI 1U
typedef struct { unsigned kind; } sentinel_watch_protocol_record_t;
static uint32_t cerberus_wifi_capture_service_admission_token(void){return 0U;}
static bool cerberus_wifi_capture_service_active(void){return false;}
static void cerberus_wifi_capture_service_receive(const uint8_t *p,unsigned n,unsigned c,int r,int64_t t,uint32_t g){(void)p;(void)n;(void)c;(void)r;(void)t;(void)g;CHECK(false);}
static void cerberus_wardrive_radio_wifi_rx(void *p,int type){(void)p;(void)type;}
static void packet_count_receive(const wifi_promiscuous_pkt_t *p,unsigned t,int64_t c){(void)p;(void)t;(void)c;}
static void probe_sniff_receive(const wifi_promiscuous_pkt_t *p,unsigned t,int64_t c){(void)p;(void)t;(void)c;}
static void mac_receive(const wifi_promiscuous_pkt_t *p,unsigned t,int64_t c,uint32_t k){(void)p;(void)t;(void)c;(void)k;}
static void wifi_fox_hunt_receive(const wifi_promiscuous_pkt_t *p,unsigned t,int64_t c,uint32_t k){(void)p;(void)t;(void)c;(void)k;}
static void scan_observation_receive(const wifi_promiscuous_pkt_t *p,unsigned t){(void)p;(void)t;}
static void channel_survey_receive(const wifi_promiscuous_pkt_t *p,unsigned t){(void)p;(void)t;}
static void packet_monitor_receive(const wifi_promiscuous_pkt_t *p,unsigned t){(void)p;(void)t;}
static void station_receive(const wifi_promiscuous_pkt_t *p){(void)p;}
static void cerberus_c5_test_receive(const uint8_t *p,unsigned n,unsigned c){(void)p;(void)n;(void)c;}
static bool sentinel_watch_protocol_decode_wifi(const uint8_t *p,unsigned n,sentinel_watch_protocol_record_t *o){(void)p;(void)n;(void)o;return false;}
static void cerberus_watch_capture_observe_timed(void *c,const void *p,const uint8_t *i,unsigned t,int r,unsigned ch,uint64_t u){(void)c;(void)p;(void)i;(void)t;(void)r;(void)ch;(void)u;CHECK(false);}
static void cerberus_watch_capture_observe(void *c,const void *p,const uint8_t *i,unsigned t,int r,unsigned ch){cerberus_watch_capture_observe_timed(c,p,i,t,r,ch,0U);}
#include "survey_outer_callback.h"
static uint8_t tx_buffer[64];static size_t received_transfer_bytes;
static void prepare_frame_response(cerberus_spi_command_t command,uint8_t token,const void *p,size_t n)
{CHECK(cerberus_protocol_prepare_frame((cerberus_spi_frame_t *)tx_buffer,command,token,p,n));}
static void dispatch(const cerberus_spi_frame_t *request){switch(request->command){
#include "survey_dispatch.h"
default:CHECK(false);}}

/* Wire boundary used by the actual S3 adapter and incremental reader. */
static uint8_t sequence;
static uint32_t peer_firmware_version;
static cerberus_c5_status_t fake_peer={.firmware_version=90,.protocol_version=CERBERUS_PROTOCOL_VERSION,.uptime_ms=1,.boot_count=1};
static unsigned exchanges,peer_reads,wire_error,wire_delay;
static bool link_locked,lock_fail;
static int take_link_mutex(unsigned ms){CHECK(ms==20U&&!link_locked);if(lock_fail)return 0;link_locked=true;return pdTRUE;}
static void give_link_mutex(void){CHECK(link_locked);link_locked=false;}
static esp_err_t get_status_v2_passive_locked(uint8_t token,uint32_t deadline,cerberus_c5_status_t *out)
{(void)token;(void)deadline;CHECK(link_locked);++peer_reads;*out=fake_peer;return ESP_OK;}
static esp_err_t spi_request_response_passive(const void *q,void *discarded,void *out,size_t bytes,uint32_t deadline)
{
    (void)deadline;CHECK(link_locked&&(bytes==32U||bytes==64U));++exchanges;
    memset(discarded,0,bytes);memset(tx_buffer,0,sizeof(tx_buffer));received_transfer_bytes=bytes;
    dispatch(q);memcpy(out,tx_buffer,bytes);now_us+=wire_delay;
    if(wire_error==1)((uint8_t *)out)[3]^=1U;
    if(wire_error==2)return ESP_ERR_TIMEOUT;
    if(bytes==64U&&wire_error>=3U) {
        cerberus_ap_survey_frame_t *p=out;
        if(wire_error==3U)++p->payload.session_id;
        if(wire_error==4U)++p->payload.projection;
        if(wire_error==5U)++p->payload.offset;
        if(wire_error==6U&&p->payload.offset==3800U)p->payload.data[39]=1U;
        if(wire_error==7U&&p->payload.offset==80U)p->payload.data[23]|=0x80U;
        p->crc16=cerberus_protocol_crc16(p,offsetof(cerberus_ap_survey_frame_t,crc16));
    }
    return ESP_OK;
}
static esp_err_t survey_session_frame_locked(cerberus_spi_command_t command,const void *q,size_t n,void *out,size_t bytes,uint32_t deadline)
{
    cerberus_spi_frame_t request,response;uint8_t discard[32],token=++sequence;
    CHECK(cerberus_protocol_prepare_frame(&request,command,token,q,n));
    esp_err_t e=spi_request_response_passive(&request,discard,&response,32,deadline);
    if(e==ESP_OK&&(!cerberus_protocol_frame_is_valid(&response)||response.command!=command||response.sequence!=token||response.payload_length!=bytes))e=ESP_ERR_INVALID_RESPONSE;
    if(e==ESP_OK)memcpy(out,response.payload,bytes);return e;
}
#include "c5_wifi_ap_survey_link.h"

static cerberus_ap_survey_control_t control(uint8_t cmd,uint64_t session)
{
    cerberus_ap_survey_request_t q={session};cerberus_ap_survey_control_t out;
    esp_err_t e=cerberus_wifi_ap_survey_exchange(cmd,cmd==CERBERUS_AP_SURVEY_COMMAND_INFO?NULL:&q,
        cmd==CERBERUS_AP_SURVEY_COMMAND_INFO?0U:sizeof(q),&out,sizeof(out));
    CHECK(e==ESP_OK||e==ESP_ERR_NOT_FOUND);
    CHECK(cerberus_ap_survey_control_valid(&out,cmd==CERBERUS_AP_SURVEY_COMMAND_INFO,session));return out;
}
static void tick(unsigned ms){now_us+=(int64_t)ms*1000;ap_survey_worker_tick();}
static uint64_t start(uint64_t mask)
{
    cerberus_ap_survey_control_t info=control(CERBERUS_AP_SURVEY_COMMAND_INFO,0U);
    cerberus_ap_survey_start_t q={info.session_id+1U,mask,250U,0};cerberus_ap_survey_control_t out;
    CHECK(cerberus_wifi_ap_survey_exchange(CERBERUS_AP_SURVEY_COMMAND_START,&q,sizeof(q),&out,sizeof(out))==ESP_OK);
    CHECK(out.flags&CERBERUS_AP_SURVEY_HELD);CHECK(ap_survey_retained());
    uint64_t deadline=ap_survey_context->lease;
    CHECK(cerberus_wifi_ap_survey_exchange(CERBERUS_AP_SURVEY_COMMAND_START,&q,sizeof(q),&out,sizeof(out))==ESP_OK);
    CHECK(ap_survey_context->lease==deadline); /* lost START reply replay */
    tick(20);tick(20);CHECK(ap_survey_context->state==CERBERUS_AP_SURVEY_RUNNING);
    return q.session_id;
}
static void release(uint64_t session)
{
    (void)control(CERBERUS_AP_SURVEY_COMMAND_RELEASE,session);
    for(unsigned i=0;i<70&&ap_survey_context;++i)tick(20);
    CHECK(!ap_survey_context&&!allocation&&!atomic_load(&ap_survey_radio_reserved));
    CHECK(control(CERBERUS_AP_SURVEY_COMMAND_RELEASE,session).flags==0U);
}
static void reset(void)
{
    CHECK(!ap_survey_context&&!allocation&&!locked&&!critical);
    now_us=1000000;fail_sdk_at=fail_channel=wire_error=wire_delay=0;disable_error=restore_error=0;
    sdk_mode=7;sdk_channel=11;sdk_second=1;sdk_filter.filter_mask=9U;sdk_receiving=false;
    foreign_radio=allocation_fail=lock_fail=false;clock_hook=decode_hook=NULL;
    sdk_calls=enable_calls=disable_calls=0;fake_peer.firmware_version=90;fake_peer.uptime_ms=1;fake_peer.boot_count=1;
    peer_firmware_version=90;ap_survey_boot_init();ap_survey_last_session=0U;ap_survey_arm_counter=0U;
    memset(&ap_survey_last_request,0,sizeof(ap_survey_last_request));atomic_store(&ap_survey_admission,0U);
}
static wifi_promiscuous_pkt_t beacon(unsigned id)
{
    wifi_promiscuous_pkt_t p={0};p.rx_ctrl.sig_len=43;p.rx_ctrl.channel=sdk_channel;p.rx_ctrl.rssi=-40;
    p.payload[0]=0x80;memset(p.payload+4,255,6);p.payload[10]=p.payload[16]=2;
    p.payload[15]=p.payload[21]=(uint8_t)id;p.payload[36]=0;p.payload[37]=1;p.payload[38]='A';return p;
}
static void receive_beacon(unsigned id)
{wifi_promiscuous_pkt_t p=beacon(id);++now_us;wifi_promiscuous_callback(&p,WIFI_PKT_MGMT);}
static void receive_relation(bool to_ap)
{
    wifi_promiscuous_pkt_t p={0};p.rx_ctrl.sig_len=28;p.rx_ctrl.channel=sdk_channel;p.rx_ctrl.rssi=-55;
    p.payload[0]=8;p.payload[1]=to_ap?1:2;p.payload[4]=p.payload[10]=2;
    p.payload[9]=to_ap?1:99;p.payload[15]=to_ap?99:1;++now_us;wifi_promiscuous_callback(&p,WIFI_PKT_DATA);
}
static void rearm_same_channel(void)
{
    ap_survey_context_t *c=ap_survey_context;ap_survey_close(c,now_us);
    now_us+=1000;portENTER_CRITICAL(&ap_survey_mux);
    CHECK(cerberus_ap_survey_arm(&c->table,c->request.session_id,(uint64_t)ap_survey_arm_counter+1,sdk_channel,(uint64_t)now_us));
    atomic_store(&ap_survey_admission,++ap_survey_arm_counter);portEXIT_CRITICAL(&ap_survey_mux);
}
static void run_snapshot(uint64_t session,cerberus_ap_survey_snapshot_t *out,bool update)
{
    cerberus_c5_ap_survey_reader_t reader;bool complete=false;
    CHECK(c5_spi_link_wifi_ap_survey_reader_begin(&reader,session,out,(uint32_t)(now_us/1000)+10000U)==ESP_OK);
    for(unsigned step=0;step<110&&!complete;++step) {
        unsigned before=exchanges+peer_reads;
        CHECK(c5_spi_link_wifi_ap_survey_reader_step(&reader,&complete)==ESP_OK);
        CHECK(exchanges+peer_reads==before+1U);
        if(update&&reader.phase==AP_SURVEY_READER_READ)receive_beacon(2);
    }
    CHECK(complete&&reader.request_started_us<=reader.receipt_us);CHECK(cerberus_ap_survey_snapshot_valid(out));
}
int main(void)
{
    reset();uint64_t session=start(UINT64_C(1)<<14);
    receive_beacon(1);receive_relation(true);receive_relation(false);
    CHECK(ap_survey_context->table.live.ap_count==1&&ap_survey_context->table.live.relation_count==1);
    CHECK(ap_survey_context->table.live.relations[0].row.directions==3U);
    uint32_t arm=atomic_load(&ap_survey_admission);unsigned sdk_before=sdk_calls;
    tick(250);CHECK(atomic_load(&ap_survey_admission)==arm&&sdk_calls==sdk_before);
    CHECK(ap_survey_context->table.live.completed_passes==1U);
    cerberus_ap_survey_snapshot_t snapshot;
    unsigned disabled=disable_calls,enabled=enable_calls;
    run_snapshot(session,&snapshot,true);
    CHECK(snapshot.ap_count==1&&ap_survey_context->table.live.ap_count==2);
    CHECK(disable_calls==disabled&&enable_calls==enabled&&sdk_receiving);
    uint64_t lease=ap_survey_context->lease;
    (void)control(CERBERUS_AP_SURVEY_COMMAND_STATUS,session);CHECK(ap_survey_context->lease==lease);
    ++now_us;(void)control(CERBERUS_AP_SURVEY_COMMAND_RENEW,session);CHECK(ap_survey_context->lease>lease);
    CHECK(control(CERBERUS_AP_SURVEY_COMMAND_STOP,session+1).error_code==ESP_ERR_NOT_FOUND);
    CHECK(ap_survey_context->table.accepting);
    wifi_promiscuous_pkt_t p=beacon(3);uint32_t frames=ap_survey_context->table.live.ap_frames;
    clock_hook=rearm_same_channel;wifi_promiscuous_callback(&p,WIFI_PKT_MGMT);
    CHECK(ap_survey_context->table.live.ap_frames==frames); /* old token, fresh clock */
    p.rx_ctrl.rx_state=1;wifi_promiscuous_callback(&p,WIFI_PKT_MGMT);CHECK(ap_survey_context->table.live.ap_frames==frames);
    (void)control(CERBERUS_AP_SURVEY_COMMAND_STOP,session);
    for(unsigned i=0;i<5;++i)tick(20);
    CHECK(!atomic_load(&ap_survey_radio_reserved)&&!sdk_receiving&&sdk_mode==7&&sdk_channel==11&&sdk_filter.filter_mask==9U);
    run_snapshot(session,&snapshot,false);release(session);CHECK(allocations==frees);

    reset();session=start((UINT64_C(1)<<14)|(UINT64_C(1)<<15));
    tick(250);CHECK(ap_survey_context->table.live.completed_passes==0);tick(20);CHECK(sdk_channel==40);
    tick(249);CHECK(ap_survey_context->table.live.completed_passes==0);tick(1);
    CHECK(ap_survey_context->table.live.completed_passes==1);release(session);
    reset();session=start(UINT64_C(1)<<14);disable_error=ESP_FAIL;
    (void)control(CERBERUS_AP_SURVEY_COMMAND_STOP,session);tick(20);
    CHECK(atomic_load(&ap_survey_radio_reserved)&&ap_survey_context->state==CERBERUS_AP_SURVEY_STOPPING);
    disable_error=0;restore_error=ESP_FAIL;for(unsigned i=0;i<20;++i)tick(20);
    CHECK(atomic_load(&ap_survey_radio_reserved));restore_error=0;release(session);
    reset();session=start(UINT64_C(1)<<14);now_us=(int64_t)ap_survey_context->lease;
    CHECK(control(CERBERUS_AP_SURVEY_COMMAND_RENEW,session).error_code==ESP_ERR_TIMEOUT);release(session);
    reset();session=start(UINT64_C(1)<<14);now_us-=100000;
    CHECK(control(CERBERUS_AP_SURVEY_COMMAND_STATUS,session).error_code==ESP_ERR_INVALID_STATE);release(session);

    reset();session=start(UINT64_C(1)<<14);receive_beacon(1);
    cerberus_c5_ap_survey_reader_t reader;bool complete;
    CHECK(c5_spi_link_wifi_ap_survey_reader_begin(&reader,session,&snapshot,(uint32_t)(now_us/1000)+1000)==ESP_OK);
    CHECK(c5_spi_link_wifi_ap_survey_reader_step(&reader,&complete)==ESP_OK);
    CHECK(c5_spi_link_wifi_ap_survey_reader_step(&reader,&complete)==ESP_OK);
    wire_error=1;CHECK(c5_spi_link_wifi_ap_survey_reader_step(&reader,&complete)==ESP_ERR_INVALID_RESPONSE);
    cerberus_ap_survey_snapshot_t zero={0};CHECK(!memcmp(&snapshot,&zero,sizeof(zero)));wire_error=0;
    CHECK(c5_spi_link_wifi_ap_survey_reader_begin(&reader,session,&snapshot,(uint32_t)(now_us/1000)+1000)==ESP_OK);
    CHECK(c5_spi_link_wifi_ap_survey_reader_step(&reader,&complete)==ESP_OK);
    c5_spi_link_wifi_ap_survey_reader_cancel(&reader);CHECK(!memcmp(&snapshot,&zero,sizeof(zero)));
    fake_peer.firmware_version=89;unsigned before=exchanges;
    cerberus_ap_survey_control_t ctl;
    CHECK(c5_spi_link_wifi_ap_survey_exchange(CERBERUS_AP_SURVEY_COMMAND_INFO,NULL,0,&ctl,sizeof(ctl))==ESP_ERR_NOT_SUPPORTED);
    CHECK(exchanges==before);fake_peer.firmware_version=90;release(session);
    for(unsigned corrupt=3U;corrupt<=7U;++corrupt) {
        reset();session=start(UINT64_C(1)<<14);receive_beacon(1);
        CHECK(c5_spi_link_wifi_ap_survey_reader_begin(&reader,session,&snapshot,(uint32_t)(now_us/1000)+1000)==ESP_OK);
        wire_error=corrupt;esp_err_t error=ESP_OK;complete=false;
        for(unsigned step=0;step<110&&error==ESP_OK&&!complete;++step)
            error=c5_spi_link_wifi_ap_survey_reader_step(&reader,&complete);
        CHECK(error==ESP_ERR_INVALID_RESPONSE&&!complete&&!memcmp(&snapshot,&zero,sizeof(zero)));
        wire_error=0;release(session);
    }
    reset();session=start(UINT64_C(1)<<14);receive_beacon(1);
    CHECK(c5_spi_link_wifi_ap_survey_reader_begin(&reader,session,&snapshot,(uint32_t)(now_us/1000)+1000)==ESP_OK);
    for(unsigned step=0;step<100&&reader.phase!=AP_SURVEY_READER_FINAL;++step)
        CHECK(c5_spi_link_wifi_ap_survey_reader_step(&reader,&complete)==ESP_OK);
    ++fake_peer.boot_count;
    CHECK(c5_spi_link_wifi_ap_survey_reader_step(&reader,&complete)==ESP_ERR_INVALID_RESPONSE);
    CHECK(!memcmp(&snapshot,&zero,sizeof(zero)));release(session);

    reset();session=start(UINT64_C(1)<<14);receive_beacon(1);
    cerberus_ap_survey_request_t request={session};cerberus_ap_survey_projection_t projection;
    CHECK(cerberus_wifi_ap_survey_exchange(CERBERUS_AP_SURVEY_COMMAND_SNAPSHOT_BEGIN,&request,sizeof(request),&projection,sizeof(projection))==ESP_OK);
    cerberus_ap_survey_read_t read={session,projection.projection,0U};cerberus_ap_survey_chunk_t a,b;
    CHECK(cerberus_wifi_ap_survey_exchange(CERBERUS_AP_SURVEY_COMMAND_SNAPSHOT_READ,&read,sizeof(read),&a,sizeof(a))==ESP_OK);
    receive_beacon(2);now_us+=200;
    CHECK(cerberus_wifi_ap_survey_exchange(CERBERUS_AP_SURVEY_COMMAND_SNAPSHOT_READ,&read,sizeof(read),&b,sizeof(b))==ESP_OK);
    CHECK(!memcmp(&a,&b,sizeof(a)));
    CHECK(cerberus_wifi_ap_survey_exchange(CERBERUS_AP_SURVEY_COMMAND_SNAPSHOT_BEGIN,&request,sizeof(request),&projection,sizeof(projection))==ESP_OK);
    CHECK(projection.projection!=read.projection);
    CHECK(cerberus_wifi_ap_survey_exchange(CERBERUS_AP_SURVEY_COMMAND_SNAPSHOT_READ,&read,sizeof(read),&b,sizeof(b))==ESP_ERR_NOT_FOUND);
    ap_survey_context->projection_counter=UINT32_MAX;
    CHECK(cerberus_wifi_ap_survey_exchange(CERBERUS_AP_SURVEY_COMMAND_SNAPSHOT_BEGIN,&request,sizeof(request),&projection,sizeof(projection))==ESP_ERR_INVALID_STATE);
    CHECK(ap_survey_context->stop&&!atomic_load(&ap_survey_admission));release(session);
    reset();session=start(UINT64_C(1)<<14);receive_beacon(1);
    CHECK(c5_spi_link_wifi_ap_survey_reader_begin(&reader,session,&snapshot,(uint32_t)(now_us/1000)+1000)==ESP_OK);
    CHECK(c5_spi_link_wifi_ap_survey_reader_step(&reader,&complete)==ESP_OK);
    wire_delay=20000;
    CHECK(c5_spi_link_wifi_ap_survey_reader_step(&reader,&complete)==ESP_ERR_TIMEOUT);
    CHECK(!complete&&!memcmp(&snapshot,&zero,sizeof(zero)));wire_delay=0;
    CHECK(c5_spi_link_wifi_ap_survey_reader_begin(&reader,session,&snapshot,(uint32_t)(now_us/1000)+1000)==ESP_OK);
    CHECK(c5_spi_link_wifi_ap_survey_reader_step(&reader,&complete)==ESP_OK);
    --now_us;CHECK(c5_spi_link_wifi_ap_survey_reader_step(&reader,&complete)==ESP_ERR_INVALID_STATE);
    CHECK(!memcmp(&snapshot,&zero,sizeof(zero)));++now_us;release(session);

    reset();session=start(UINT64_C(1)<<14);(void)control(CERBERUS_AP_SURVEY_COMMAND_STOP,session);
    for(unsigned step=0;step<4;++step)tick(20);
    CHECK(!atomic_load(&ap_survey_radio_reserved));
    uint64_t retention=ap_survey_context->retention;
    ++now_us;(void)control(CERBERUS_AP_SURVEY_COMMAND_STATUS,session);
    CHECK(ap_survey_context->retention>retention);
    now_us=(int64_t)ap_survey_context->retention;ap_survey_worker_tick();
    CHECK(!ap_survey_context&&!allocation); /* automatic terminal disposal */
    reset();session=start(UINT64_C(1)<<14);receive_beacon(1);
    ap_survey_context->inflight=1; /* entered callback has not committed */
    (void)control(CERBERUS_AP_SURVEY_COMMAND_RELEASE,session);
    for(unsigned step=0;step<6;++step)tick(20);
    CHECK(ap_survey_context&&atomic_load(&ap_survey_radio_reserved)&&!sdk_receiving);
    ap_survey_context->inflight=0;release(session);
    reset();session=start((UINT64_C(1)<<10)|(UINT64_C(1)<<13));
    CHECK(ap_survey_context->table.live.requested_channels==((UINT64_C(1)<<10)|(UINT64_C(1)<<13)));
    CHECK(ap_survey_context->table.live.effective_channels==(UINT64_C(1)<<10));release(session);
    reset();session=start((UINT64_C(1)<<14)|(UINT64_C(1)<<15));fail_channel=40;
    tick(250);tick(20);CHECK(ap_survey_context->table.live.tune_failures==1U);
    CHECK(ap_survey_context->table.live.completed_passes==1U&&!ap_survey_context->stop);release(session);

    for(unsigned failure=1U;failure<=8U;++failure) {
        reset();fail_sdk_at=failure;
        cerberus_ap_survey_start_t request={ap_survey_epoch+1U,UINT64_C(1)<<14,250U,0};
        CHECK(cerberus_wifi_ap_survey_exchange(CERBERUS_AP_SURVEY_COMMAND_START,&request,sizeof(request),&ctl,sizeof(ctl))==ESP_OK);
        for(unsigned step=0;step<10;++step)tick(20);
        CHECK(!ap_survey_context->table.accepting&&ap_survey_context->error!=ESP_OK);
        fail_sdk_at=0;release(request.session_id);
    }
    reset();allocation_fail=true;
    cerberus_ap_survey_start_t q={ap_survey_epoch+1,UINT64_C(1)<<14,250,0};
    CHECK(cerberus_wifi_ap_survey_exchange(CERBERUS_AP_SURVEY_COMMAND_START,&q,sizeof(q),&ctl,sizeof(ctl))==ESP_ERR_NO_MEM);
    CHECK(!ap_survey_context&&!atomic_load(&ap_survey_radio_reserved));
    printf("AP survey C5 receiver/worker/dispatcher/S3 wire: %u checks passed; context %zu bytes; live snapshot continuity, exact ownership, failures and immutable transfer.\n",checks,sizeof(ap_survey_context_t));
    return 0;
}
