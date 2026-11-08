#include "wardrive_foreign_boundary.h"
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#undef NDEBUG
#include "cerberus_protocol.h"
#include "probe_sniff_core.h"
#include "esp_err.h"
#include <assert.h>
#include <stdatomic.h>
static atomic_bool packet_count_radio_reserved;
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
static unsigned checks;
#define CHECK(x) do{++checks;assert(x);}while(0)
#define portMAX_DELAY 0xffffffffU
#define pdMS_TO_TICKS(x) (x)
#define MALLOC_CAP_INTERNAL 1U
#define MALLOC_CAP_8BIT 2U
#define WIFI_PROMIS_FILTER_MASK_MGMT 1U
#define WIFI_BAND_2G 1
#define WIFI_BAND_5G 2
#define WIFI_BAND_MODE_AUTO 3
#define WIFI_SECOND_CHAN_NONE 0
#define WIFI_PKT_MGMT 0U
#define WIFI_PKT_CTRL 1U
#define WIFI_PKT_DATA 2U
#define WIFI_PKT_MISC 3U
#define SENTINEL_WATCH_PROTOCOL_PWNAGOTCHI 1U
typedef unsigned wifi_promiscuous_pkt_type_t;
typedef int wifi_band_mode_t;
typedef int wifi_second_chan_t;
typedef struct{unsigned filter_mask;}wifi_promiscuous_filter_t;
typedef struct{unsigned sig_len:14;unsigned dump_len:14;unsigned rx_state:8;unsigned channel:8;signed rssi:8;signed noise_floor:8;}wifi_pkt_rx_ctrl_t;
typedef struct{wifi_pkt_rx_ctrl_t rx_ctrl;uint8_t payload[2400];}wifi_promiscuous_pkt_t;
static int mutex,task,probe_sniff_mux,defense_capture_mux;
static void *scanner_mutex=&mutex,*scanner_task_handle=&task;
static bool locked,critical,scanner_ready=true,station_reserved,foreign_radio;
static atomic_bool probe_sniff_radio_reserved,channel_survey_radio_reserved,mac_radio_reserved,csi_radio_reserved,
    capture_radio_reserved,remote_id_radio_reserved,packet_monitor_radio_reserved,relation_receiving;
static atomic_uint mac_admission_token, wifi_fox_hunt_admission_token;
static cerberus_wifi_scan_status_payload_t scan_status;
static cerberus_wifi_defense_status_payload_t defense_status;
static cerberus_wifi_channel_survey_status_payload_t channel_survey_status;
static int64_t now_us=1000000;
static unsigned notifications,sdk_calls,enable_calls,disable_calls,allocations,frees;
static unsigned fail_sdk_at,restore_clock_jump;
static int enable_error,disable_error,restore_error;
static bool sdk_receiving,allocation_failure;
static wifi_band_mode_t sdk_mode=7;
static uint8_t sdk_channel=11;
static wifi_second_chan_t sdk_second=1;
static wifi_promiscuous_filter_t sdk_filter={7};
static void (*before_mutex)(void),(*after_probe_unlock)(void);
static void xSemaphoreTake(void *m,unsigned timeout)
{CHECK(m==&mutex&&timeout==portMAX_DELAY&&!locked);if(before_mutex){void(*f)(void)=before_mutex;before_mutex=NULL;f();}locked=true;}
static void xSemaphoreGive(void *m){CHECK(m==&mutex&&locked);locked=false;}
static void enter_critical(int *m){CHECK((m==&probe_sniff_mux||m==&defense_capture_mux)&&!critical);critical=true;}
static void exit_critical(int *m){CHECK(critical);critical=false;if(m==&probe_sniff_mux&&after_probe_unlock){void(*f)(void)=after_probe_unlock;after_probe_unlock=NULL;f();}}
#define portENTER_CRITICAL(m) enter_critical(m)
#define portEXIT_CRITICAL(m) exit_critical(m)
static int64_t esp_timer_get_time(void){return now_us;}
static uint32_t esp_random(void){static uint32_t n=17;return ++n;}
static void xTaskNotifyGive(void *t){CHECK(t==&task);++notifications;}
static bool cerberus_c5_test_running(void){return false;}
static bool another_radio_operation_is_active(void){return foreign_radio;}
static bool wifi_connection_is_active_locked(void){CHECK(locked);return false;}
static int sdk(void){CHECK(!locked&&!critical);++sdk_calls;return fail_sdk_at==sdk_calls?ESP_FAIL:ESP_OK;}
static esp_err_t esp_wifi_get_band_mode(wifi_band_mode_t *out){int e=sdk();if(!e)*out=sdk_mode;return e;}
static esp_err_t esp_wifi_get_channel(uint8_t *out,wifi_second_chan_t *second){int e=sdk();if(!e){*out=sdk_channel;*second=sdk_second;}return e;}
static esp_err_t esp_wifi_get_promiscuous_filter(wifi_promiscuous_filter_t *out){int e=sdk();if(!e)*out=sdk_filter;return e;}
static esp_err_t esp_wifi_set_band_mode(wifi_band_mode_t mode){int e=sdk();if(mode==7&&restore_error)e=restore_error;if(!e)sdk_mode=mode;if(mode==7&&restore_clock_jump)now_us+=(int64_t)restore_clock_jump*1000;return e;}
static esp_err_t esp_wifi_set_band(int band){CHECK(band==1||band==2);return sdk();}
static esp_err_t esp_wifi_set_channel(uint8_t channel,wifi_second_chan_t second){int e=sdk();if(!e){sdk_channel=channel;sdk_second=second;}return e;}
static esp_err_t esp_wifi_set_promiscuous_filter(const wifi_promiscuous_filter_t *filter){int e=sdk();if(!e)sdk_filter=*filter;return e;}
static esp_err_t esp_wifi_set_promiscuous(bool enabled){int e=sdk();if(enabled){++enable_calls;if(enable_error)e=enable_error;}else{++disable_calls;if(disable_error)e=disable_error;}if(!e)sdk_receiving=enabled;return e;}
static void *heap_object;static size_t heap_size;
static size_t heap_caps_get_largest_free_block(unsigned caps){CHECK(caps==3&&!locked&&!critical);return allocation_failure?0U:65536U;}
static void *heap_caps_calloc(size_t n,size_t size,unsigned caps)
{
    CHECK(n==1U&&caps==3&&!locked&&!critical&&!heap_object);if(allocation_failure)return NULL;
    uint8_t *p=calloc(1,size+16U);CHECK(p);memset(p,0xa5,8);memset(p+size+8,0x5a,8);heap_object=p+8;heap_size=size;++allocations;return heap_object;
}
static void heap_caps_free(void *ptr)
{
    CHECK(!locked&&!critical&&ptr==heap_object);uint8_t *p=(uint8_t *)ptr-8;
    for(unsigned i=0;i<8;++i)CHECK(p[i]==0xa5&&p[heap_size+8+i]==0x5a);
    free(p);heap_object=NULL;++frees;
}
/* Existing unrelated callback consumers remain inert; actual common dispatch
 * still executes, so RX-state/type/FCS integration is exercised, not mirrored. */
static cerberus_wifi_probe_table_t probe_capture;
static cerberus_wifi_relation_table_t relation_capture;
static struct{struct{uint8_t generation;}status;uint64_t capture_epoch;}watch_capture;
static bool watch_capture_active,defense_capture_active;
static uint8_t watch_capture_channel;
static uint64_t watch_capture_armed_us,watch_capture_deadline_us;
static uint32_t watch_callback_inflight;
static uint32_t defense_management_frames,defense_control_frames,defense_data_frames,defense_beacon_frames,defense_disassoc_frames,defense_deauth_frames,defense_noise_samples;
static int64_t defense_noise_sum;
static int8_t defense_strongest_rssi;
typedef struct{unsigned kind;}sentinel_watch_protocol_record_t;
static void mac_receive(const wifi_promiscuous_pkt_t*p,unsigned type,int64_t time,uint32_t token){(void)p;(void)type;(void)time;(void)token;}
static void scan_observation_receive(const wifi_promiscuous_pkt_t*p,unsigned type){(void)p;(void)type;}
static void channel_survey_receive(const wifi_promiscuous_pkt_t*p,unsigned type){(void)p;(void)type;}
/* Fox Hunt is inactive in this receiver-specific SDK fixture. */
static void wifi_fox_hunt_receive(const wifi_promiscuous_pkt_t *p,
    wifi_promiscuous_pkt_type_t type, int64_t entered, uint32_t token)
{ (void)p; (void)type; (void)entered; (void)token; }
static void packet_monitor_receive(const wifi_promiscuous_pkt_t*p,unsigned type){(void)p;(void)type;}
static bool cerberus_wifi_capture_service_active(void){return false;}
static void cerberus_wifi_capture_service_receive(const uint8_t*p,unsigned n,unsigned ch,int rssi,uint64_t time){(void)p;(void)n;(void)ch;(void)rssi;(void)time;CHECK(false);}
static void station_receive(const wifi_promiscuous_pkt_t*p){(void)p;}
static void cerberus_c5_test_receive(const uint8_t*p,unsigned n,unsigned ch){(void)p;(void)n;(void)ch;}
static bool sentinel_watch_protocol_decode_wifi(const uint8_t*p,unsigned n,sentinel_watch_protocol_record_t*out){(void)p;(void)n;(void)out;return false;}
static void cerberus_watch_capture_observe_timed(void*c,const void*p,const uint8_t*id,unsigned type,int rssi,unsigned ch,uint64_t time){(void)c;(void)p;(void)id;(void)type;(void)rssi;(void)ch;(void)time;CHECK(false);}
static void cerberus_watch_capture_observe(void*c,const void*p,const uint8_t*id,unsigned type,int rssi,unsigned ch){cerberus_watch_capture_observe_timed(c,p,id,type,rssi,ch,0);}
static void packet_count_receive(const wifi_promiscuous_pkt_t *p,wifi_promiscuous_pkt_type_t type,int64_t entered)
{(void)p;(void)type;(void)entered;}
#include "wifi_probe_sniff_service.h"
#include "probe_native_callback.h"
#include "probe_native_admission.h"
static uint8_t response_bytes[22];static unsigned response_size,response_command,response_sequence;
static void prepare_frame_response(cerberus_spi_command_t command,uint8_t sequence,const void *payload,size_t size)
{CHECK(size==22);memcpy(response_bytes,payload,22);response_size=(unsigned)size;response_command=command;response_sequence=sequence;}
static void dispatch(const cerberus_spi_frame_t *request){switch(request->command){
#include "probe_native_dispatch.h"
default:CHECK(false);}}
static void tick(unsigned ms){now_us+=(int64_t)ms*1000;probe_sniff_worker_tick();}
static void reset(void)
{
    CHECK(!locked&&!critical&&!heap_object&&!probe_sniff_context);
    memset(&scan_status,0,sizeof(scan_status));memset(&defense_status,0,sizeof(defense_status));memset(&channel_survey_status,0,sizeof(channel_survey_status));
    scanner_ready=true;station_reserved=foreign_radio=allocation_failure=false;now_us=1000000;
    sdk_calls=notifications=enable_calls=disable_calls=fail_sdk_at=restore_clock_jump=0;enable_error=disable_error=restore_error=0;
    sdk_receiving=false;sdk_mode=7;sdk_channel=11;sdk_second=1;sdk_filter.filter_mask=7;
    probe_sniff_epoch=100;probe_sniff_session=0;memset(&probe_sniff_request,0,sizeof(probe_sniff_request));probe_sniff_state=0;probe_sniff_error=0;
    atomic_store(&probe_sniff_radio_reserved,false);atomic_store(&mac_radio_reserved,false);
    before_mutex=after_probe_unlock=NULL;
}
static cerberus_probe_sniff_control_t begin(uint8_t mode,uint8_t channel)
{
    cerberus_probe_sniff_start_t q={probe_sniff_epoch+1U,mode,channel};cerberus_probe_sniff_control_t c;
    CHECK(cerberus_wifi_probe_sniff_start(&q,&c)==ESP_OK);CHECK(cerberus_probe_sniff_control_valid(&c,false,false,q.session_id));
    CHECK(c.state==CERBERUS_PROBE_SNIFF_QUEUED&&(c.flags&CERBERUS_PROBE_SNIFF_OWNED));
    return c;
}
static void dispose(uint64_t token)
{
    cerberus_probe_sniff_control_t c;CHECK(cerberus_wifi_probe_sniff_release(token,&c)==ESP_OK);
    for(unsigned i=0;probe_sniff_context&&i<100;++i)tick(20);
    CHECK(!probe_sniff_context&&!heap_object&&!atomic_load(&probe_sniff_radio_reserved));
    CHECK(cerberus_wifi_probe_sniff_release(token,&c)==ESP_OK&&c.flags==0&&c.capacity==0&&c.error_code==0);
    CHECK(cerberus_probe_sniff_control_valid(&c,false,true,token));
}
static wifi_promiscuous_pkt_t frame(const uint8_t *ssid,uint8_t length,uint8_t suffix)
{
    wifi_promiscuous_pkt_t p={0};p.rx_ctrl.channel=sdk_channel;p.rx_ctrl.rssi=-50;
    p.payload[0]=0x40;memset(p.payload+4,255,6);memset(p.payload+16,255,6);
    const uint8_t mac[]={2,1,2,3,4,suffix};memcpy(p.payload+10,mac,6);p.payload[24]=0;p.payload[25]=length;
    if(length)memcpy(p.payload+26,ssid,length);p.rx_ctrl.dump_len=26U+length;p.rx_ctrl.sig_len=p.rx_ctrl.dump_len+4U;
    return p;
}
static void packet(wifi_promiscuous_pkt_t *p){now_us+=100;wifi_promiscuous_callback(p,WIFI_PKT_MGMT);}
static void copy_object(uint64_t token,unsigned kind,unsigned index,void *out,size_t size)
{
    cerberus_probe_sniff_read_t q={token,0,(uint8_t)kind,(uint8_t)index,0};
    for(unsigned part=0;part<(size+8U)/9U;++part){
        q.part=(uint8_t)part;cerberus_probe_sniff_chunk_t c;CHECK(cerberus_wifi_probe_sniff_read(&q,&c)==ESP_OK);
        CHECK(c.session_id==token&&c.pin&&c.part==part);if(part)CHECK(c.pin==q.pin);q.pin=c.pin;
        size_t amount=size-part*9U;if(amount>9)amount=9;memcpy((uint8_t*)out+part*9U,c.data,amount);
        for(size_t i=amount;i<9;++i)CHECK(c.data[i]==0);
    }
}
static void parser_inventory(void)
{
    reset();cerberus_probe_sniff_control_t c=begin(CERBERUS_PROBE_SNIFF_FIXED,6);uint64_t token=c.session_id;
    tick(20);CHECK(sdk_receiving&&probe_sniff_context->armed&&sdk_channel==6);
    CHECK(cerberus_wifi_probe_sniff_status(token,&c)==ESP_OK&&c.state==CERBERUS_PROBE_SNIFF_RUNNING&&(c.flags&CERBERUS_PROBE_SNIFF_RECEIVING));
    uint8_t binary[32];for(unsigned i=0;i<32;++i)binary[i]=(uint8_t)(i*8U);
    wifi_promiscuous_pkt_t p=frame(binary,32,1);packet(&p);packet(&p);
    CHECK(probe_sniff_context->table.summary.count==1&&probe_sniff_context->table.rows[0].frames==2);
    p.payload[1]=8;p.rx_ctrl.rssi=-30;p.payload[23]=7;packet(&p);
    cerberus_probe_sniff_projection_t row;copy_object(token,1,0,&row,sizeof(row));
    CHECK(cerberus_probe_sniff_projection_valid(&row)&&row.row.frames==3&&row.row.retry_frames==1&&row.row.last_rssi==-30);
    CHECK(row.row.first_callback_us<row.row.last_callback_us&&!memcmp(row.row.ssid,binary,32));
    p=frame(NULL,0,1);packet(&p);CHECK(probe_sniff_context->table.summary.count==2);
    /* A literal '<hidden>' is a third exact key, never wildcard normalization. */
    p=frame((const uint8_t*)"<hidden>",8,1);packet(&p);CHECK(probe_sniff_context->table.summary.count==3);
    unsigned before=probe_sniff_context->table.summary.valid_frames;
    p.rx_ctrl.rx_state=1;packet(&p);CHECK(probe_sniff_context->table.summary.valid_frames==before);
    p=frame(binary,32,2);p.rx_ctrl.sig_len=CERBERUS_PROBE_SNIFF_MAX_FRAME_BYTES+5U;packet(&p);CHECK(probe_sniff_context->table.summary.unsupported_frames==1);
    p=frame(binary,32,2);p.payload[p.rx_ctrl.dump_len++]=0;p.rx_ctrl.sig_len=p.rx_ctrl.dump_len+4;packet(&p);
    CHECK(probe_sniff_context->table.summary.malformed_frames==1);
    p=frame(binary,32,2);p.payload[1]=0x40;packet(&p);CHECK(probe_sniff_context->table.summary.unsupported_frames==2);
    for(unsigned i=3;i<35;++i){p=frame(binary,32,(uint8_t)i);packet(&p);}
    CHECK(probe_sniff_context->table.summary.count==32&&probe_sniff_context->table.summary.capacity_dropped_frames==3);
    p=frame(binary,32,1);packet(&p);CHECK(probe_sniff_context->table.rows[0].frames==4);
    CHECK(cerberus_wifi_probe_sniff_stop(token,&c)==ESP_OK);for(unsigned i=0;i<4;++i)tick(20);
    CHECK(cerberus_wifi_probe_sniff_status(token,&c)==ESP_OK&&(c.flags&CERBERUS_PROBE_SNIFF_DATA_READY)&&!(c.flags&CERBERUS_PROBE_SNIFF_OWNED));
    cerberus_probe_sniff_summary_t summary;copy_object(token,0,0,&summary,sizeof(summary));
    CHECK(cerberus_probe_sniff_summary_valid(&summary)&&(summary.flags&CERBERUS_PROBE_SNIFF_STOPPED)&&summary.count==32);
    CHECK(summary.capacity_dropped_frames==3&&summary.valid_frames==summary.retained_frames+summary.capacity_dropped_frames);
    unsigned revision=summary.revision;packet(&p);copy_object(token,0,0,&summary,sizeof(summary));CHECK(summary.revision==revision);
    CHECK(sdk_mode==7&&sdk_channel==11&&sdk_second==1&&sdk_filter.filter_mask==7&&!sdk_receiving);
    dispose(token);
}
static void metadata_only_callback(void)
{
#ifdef _WIN32
    SYSTEM_INFO info;GetSystemInfo(&info);size_t page=info.dwPageSize;
    uint8_t *memory=VirtualAlloc(NULL,page*2U,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    CHECK(memory);DWORD old;CHECK(VirtualProtect(memory+page,page,PAGE_NOACCESS,&old));
#else
    size_t page=(size_t)sysconf(_SC_PAGESIZE);
    uint8_t *memory=mmap(NULL,page*2U,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(memory!=MAP_FAILED);CHECK(!mprotect(memory+page,page,PROT_NONE));
#endif
    wifi_promiscuous_pkt_t *p=(wifi_promiscuous_pkt_t *)(memory+page-sizeof(wifi_pkt_rx_ctrl_t));
    memset(&p->rx_ctrl,0,sizeof(p->rx_ctrl));p->rx_ctrl.channel=sdk_channel;
    p->rx_ctrl.sig_len=44;p->rx_ctrl.dump_len=40;
    unsigned revision=probe_sniff_context->table.summary.revision;
    wifi_promiscuous_callback(p,WIFI_PKT_MISC);
    wifi_promiscuous_callback(p,WIFI_PKT_CTRL);
    wifi_promiscuous_callback(p,WIFI_PKT_DATA);
    for(unsigned n=0;n<5U;++n){p->rx_ctrl.sig_len=n;packet(p);}
    p->rx_ctrl.sig_len=44;p->rx_ctrl.rx_state=1;packet(p);
    CHECK(probe_sniff_context->table.summary.revision==revision);
#ifdef _WIN32
    CHECK(VirtualFree(memory,0,MEM_RELEASE));
#else
    CHECK(!munmap(memory,page*2U));
#endif
}
static void public_payload_contract(void)
{
    const uint8_t channels[]={6,48};
    const uint8_t binary[]={0,0xff,0x80,'A'};
    const unsigned dump_lengths[]={0,34,1,29,31,30};
    for(unsigned ch=0;ch<sizeof(channels);++ch){
        reset();cerberus_probe_sniff_control_t c=begin(CERBERUS_PROBE_SNIFF_FIXED,channels[ch]);
        tick(20);CHECK(sdk_receiving&&sdk_channel==channels[ch]);
        for(unsigned d=0;d<sizeof(dump_lengths)/sizeof(*dump_lengths);++d){
            wifi_promiscuous_pkt_t p=frame(binary,sizeof(binary),1);p.rx_ctrl.dump_len=dump_lengths[d];
            memset(p.payload+30,0xee,4);packet(&p);
            CHECK(probe_sniff_context->table.summary.valid_frames==d+1U);
            CHECK(!probe_sniff_context->table.summary.malformed_frames&&!probe_sniff_context->table.summary.unsupported_frames);
            CHECK(probe_sniff_context->table.summary.count==1U&&probe_sniff_context->table.rows[0].frames==d+1U);
            CHECK(probe_sniff_context->table.rows[0].ssid_length==sizeof(binary)&&
                !memcmp(probe_sniff_context->table.rows[0].ssid,binary,sizeof(binary)));
        }
        metadata_only_callback();
        wifi_promiscuous_pkt_t p=frame(NULL,0,2);p.rx_ctrl.sig_len=5;p.rx_ctrl.dump_len=0;packet(&p);
        CHECK(probe_sniff_context->table.summary.malformed_frames==1U);
        /* Valid bounded IEs fill the exact Cerberus parser limit. */
        p=frame(NULL,0,2);size_t used=26;
        while(used<CERBERUS_PROBE_SNIFF_MAX_FRAME_BYTES){
            size_t remaining=CERBERUS_PROBE_SNIFF_MAX_FRAME_BYTES-used;
            size_t n=remaining-2U;if(n>255U)n=255U;if(remaining-n-2U==1U)--n;
            p.payload[used]=221;p.payload[used+1U]=(uint8_t)n;memset(p.payload+used+2U,0x5a,n);used+=n+2U;
        }
        p.rx_ctrl.sig_len=used+4U;p.rx_ctrl.dump_len=0;memset(p.payload+used,0xee,4);packet(&p);
        CHECK(probe_sniff_context->table.summary.valid_frames==7U&&probe_sniff_context->table.summary.count==2U);
        p.rx_ctrl.sig_len=used+5U;packet(&p);CHECK(probe_sniff_context->table.summary.unsupported_frames==1U);
        CHECK(probe_sniff_context->table.summary.valid_frames==7U);
        CHECK(cerberus_wifi_probe_sniff_stop(c.session_id,&c)==ESP_OK);for(unsigned i=0;i<4U;++i)tick(20);
        cerberus_probe_sniff_projection_t row;copy_object(c.session_id,1,0,&row,sizeof(row));
        CHECK(cerberus_probe_sniff_projection_valid(&row)&&row.row.frames==6U&&row.row.last_channel==channels[ch]);
        dispose(c.session_id);
    }
}
static void source_identity_lifetime(void)
{
    reset();cerberus_probe_sniff_control_t c;cerberus_wifi_probe_sniff_info(&c);CHECK(c.session_id==100&&cerberus_probe_sniff_control_valid(&c,true,false,0));
    cerberus_probe_sniff_start_t q={101,0,6};probe_sniff_epoch=500;
    CHECK(cerberus_wifi_probe_sniff_start(&q,&c)!=ESP_OK&&!c.flags&&!notifications&&!heap_object);
    q.session_id=501;CHECK(cerberus_wifi_probe_sniff_start(&q,&c)==ESP_OK);unsigned started=notifications;
    CHECK(cerberus_wifi_probe_sniff_start(&q,&c)==ESP_OK&&notifications==started);tick(20);
    q.channel=11;CHECK(cerberus_wifi_probe_sniff_start(&q,&c)==ESP_ERR_INVALID_ARG&&sdk_channel==6);q.channel=6;
    CHECK(cerberus_wifi_probe_sniff_stop(500,&c)==ESP_ERR_NOT_FOUND&&sdk_receiving);
    cerberus_wifi_scan_status_payload_t generic;cerberus_wifi_scanner_get_status(&generic);CHECK(generic.state==CERBERUS_WIFI_SCAN_RUNNING);
    /* Orphan radio expires; no STATUS/READ can resurrect the cancelled run. */
    tick(10000);for(unsigned i=0;i<3;++i)tick(20);
    CHECK(!sdk_receiving&&!atomic_load(&probe_sniff_radio_reserved));
    CHECK(cerberus_wifi_probe_sniff_start(&q,&c)==ESP_OK&&notifications==started&&!(c.flags&CERBERUS_PROBE_SNIFF_OWNED));
    uint64_t deadline=probe_sniff_context->retention_deadline_us;
    now_us=(int64_t)deadline-1;CHECK(cerberus_wifi_probe_sniff_status(501,&c)==ESP_OK);
    CHECK(probe_sniff_context->retention_deadline_us==((uint64_t)now_us+30000000U));
    now_us=(int64_t)probe_sniff_context->retention_deadline_us;
    CHECK(cerberus_wifi_probe_sniff_status(501,&c)==ESP_OK&&!(c.flags&CERBERUS_PROBE_SNIFF_DATA_READY));
    tick(0);CHECK(!probe_sniff_context);
    CHECK(cerberus_wifi_probe_sniff_status(501,&c)==ESP_ERR_NOT_FOUND&&c.flags==0);
    CHECK(cerberus_wifi_probe_sniff_start(&q,&c)==ESP_ERR_NOT_FOUND&&!heap_object);
    CHECK(cerberus_wifi_probe_sniff_release(501,&c)==ESP_OK&&!c.flags);
    CHECK(!sdk_receiving&&allocations==frees);
    reset();c=begin(0,6);uint64_t retained=c.session_id;tick(20);
    CHECK(cerberus_wifi_probe_sniff_stop(retained,&c)==ESP_OK);for(unsigned i=0;i<4;++i)tick(20);
    uint64_t retention=probe_sniff_context->retention_deadline_us;now_us+=1000;
    cerberus_probe_sniff_read_t bad={retained,0,0,0,10};cerberus_probe_sniff_chunk_t chunk;
    CHECK(cerberus_wifi_probe_sniff_read(&bad,&chunk)!=ESP_OK);
    CHECK(probe_sniff_context->retention_deadline_us==retention);
    bad.session_id=retained-1;bad.part=0;
    CHECK(cerberus_wifi_probe_sniff_read(&bad,&chunk)==ESP_ERR_NOT_FOUND);
    CHECK(probe_sniff_context->retention_deadline_us==retention);
    copy_object(retained,0,0,&(cerberus_probe_sniff_summary_t){0},84);
    CHECK(probe_sniff_context->retention_deadline_us==(uint64_t)now_us+30000000U);dispose(retained);
    reset();probe_sniff_epoch=UINT64_MAX-1;c=begin(0,1);CHECK(c.session_id==UINT64_MAX);dispose(c.session_id);
    q=(cerberus_probe_sniff_start_t){1,0,1};CHECK(cerberus_wifi_probe_sniff_start(&q,&c)!=ESP_OK&&!heap_object);
}
static void cleanup_and_failures(void)
{
    reset();allocation_failure=true;cerberus_probe_sniff_start_t q={101,0,6};cerberus_probe_sniff_control_t c;
    CHECK(cerberus_wifi_probe_sniff_start(&q,&c)==ESP_ERR_NO_MEM&&!notifications&&!sdk_calls&&!heap_object);allocation_failure=false;
    for(unsigned failure=1;failure<=8;++failure){
        reset();c=begin(0,6);uint64_t token=c.session_id;fail_sdk_at=failure;tick(20);
        for(unsigned i=0;i<8;++i)tick(20);
        CHECK(!atomic_load(&probe_sniff_radio_reserved));dispose(token);
    }
    reset();c=begin(0,6);uint64_t queued=c.session_id;
    CHECK(cerberus_wifi_probe_sniff_stop(queued,&c)==ESP_OK);
    tick(20);CHECK(!sdk_calls&&!sdk_receiving&&!atomic_load(&probe_sniff_radio_reserved));
    CHECK(cerberus_wifi_probe_sniff_status(queued,&c)==ESP_OK&&c.state==CERBERUS_PROBE_SNIFF_COMPLETE);
    CHECK(!(c.flags&CERBERUS_PROBE_SNIFF_DATA_READY));dispose(queued);
    reset();c=begin(0,6);queued=c.session_id;
    probe_sniff_context->table.summary.channel_failures=UINT32_MAX-1U;
    fail_sdk_at=7;tick(20);
    CHECK(probe_sniff_context->table.summary.channel_failures==UINT32_MAX);
    CHECK(cerberus_probe_sniff_summary_valid(&probe_sniff_context->table.summary));dispose(queued);
    reset();c=begin(0,6);uint64_t token=c.session_id;tick(20);disable_error=ESP_FAIL;
    CHECK(cerberus_wifi_probe_sniff_release(token,&c)==ESP_OK);unsigned free_count=frees;
    for(unsigned i=0;i<50;++i){tick(20);CHECK(probe_sniff_context&&atomic_load(&probe_sniff_radio_reserved)&&frees==free_count);}
    CHECK(disable_calls>=3&&disable_calls<=5&&sdk_receiving);disable_error=ESP_OK;
    for(unsigned i=0;probe_sniff_context&&i<35;++i)tick(20);CHECK(!probe_sniff_context&&!sdk_receiving&&frees==free_count+1);
    CHECK(cerberus_wifi_probe_sniff_release(token,&c)==ESP_OK&&!c.flags);
    reset();c=begin(0,6);token=c.session_id;tick(20);restore_error=ESP_FAIL;
    CHECK(cerberus_wifi_probe_sniff_stop(token,&c)==ESP_OK);for(unsigned i=0;i<20;++i)tick(20);
    CHECK(!sdk_receiving&&atomic_load(&probe_sniff_radio_reserved)&&probe_sniff_context);
    restore_error=0;restore_clock_jump=5000;
    for(unsigned i=0;i<20&&atomic_load(&probe_sniff_radio_reserved);++i)tick(20);
    CHECK(!atomic_load(&probe_sniff_radio_reserved)&&probe_sniff_context->retention_deadline_us==(uint64_t)now_us+30000000U);
    dispose(token);
}
static void immutable_pin_and_protocol(void)
{
    reset();cerberus_probe_sniff_control_t c=begin(0,6);uint64_t token=c.session_id;tick(20);
    wifi_promiscuous_pkt_t p=frame((const uint8_t*)"A",1,1);packet(&p);
    cerberus_probe_sniff_read_t q={token,0,1,0,0};cerberus_probe_sniff_chunk_t chunk;
    CHECK(cerberus_wifi_probe_sniff_read(&q,&chunk)==ESP_OK);uint32_t pin=chunk.pin;uint8_t saved[76];memcpy(saved,chunk.data,9);
    p.rx_ctrl.rssi=-20;packet(&p);CHECK(cerberus_wifi_probe_sniff_status(token,&c)==ESP_OK);
    cerberus_probe_sniff_read_t invalid={token,0,1,32,0};CHECK(cerberus_wifi_probe_sniff_read(&invalid,&chunk)!=ESP_OK);
    q.pin=pin;for(unsigned part=1;part<9;++part){q.part=(uint8_t)part;CHECK(cerberus_wifi_probe_sniff_read(&q,&chunk)==ESP_OK&&chunk.pin==pin);size_t n=part==8?4:9;memcpy(saved+part*9,chunk.data,n);}
    cerberus_probe_sniff_projection_t first;memcpy(&first,saved,76);CHECK(first.row.frames==1&&first.row.last_rssi==-50);
    CHECK(cerberus_probe_sniff_projection_valid(&first));
    for(unsigned part=9;part<256;++part){q.part=(uint8_t)part;CHECK(cerberus_wifi_probe_sniff_read(&q,&chunk)!=ESP_OK&&!chunk.pin);for(unsigned n=4;n<9;++n)CHECK(!chunk.data[n]);}
    uint64_t lease=probe_sniff_context->lease_deadline_us;
    now_us+=100;copy_object(token,0,0,&(cerberus_probe_sniff_summary_t){0},84);
    CHECK(probe_sniff_context->lease_deadline_us==lease); /* READ never renews RF. */
    q.part=0;q.pin=0;probe_sniff_context->pin_counter=UINT32_MAX-1;
    CHECK(cerberus_wifi_probe_sniff_read(&q,&chunk)==ESP_OK&&chunk.pin==UINT32_MAX);
    CHECK(cerberus_wifi_probe_sniff_read(&q,&chunk)==ESP_ERR_INVALID_STATE&&!chunk.pin);
    cerberus_spi_frame_t wire={.command=CERBERUS_SPI_COMMAND_WIFI_PROBE_SNIFF_STATUS,.sequence=88,.payload_length=8};memcpy(wire.payload,&token,8);dispatch(&wire);
    CHECK(response_size==22&&response_command==wire.command&&response_sequence==88);memcpy(&c,response_bytes,22);CHECK(c.session_id==token&&cerberus_probe_sniff_control_valid(&c,false,false,token));
    for(unsigned cmd=140;cmd<=145;++cmd){wire.command=(uint8_t)cmd;wire.payload_length=21;response_size=0;dispatch(&wire);CHECK(response_size==0);}
    dispose(token);
}
static void renew_before_lock(void)
{
    cerberus_probe_sniff_control_t c;now_us+=100;CHECK(cerberus_wifi_probe_sniff_status(probe_sniff_session,&c)==ESP_OK);
}
static void stop_during_parse(void)
{
    cerberus_probe_sniff_control_t c;CHECK(cerberus_wifi_probe_sniff_stop(probe_sniff_session,&c)==ESP_OK);
    probe_sniff_worker_tick();CHECK(probe_sniff_context->inflight==1&&!sdk_receiving);
}
static void interleavings(void)
{
    reset();cerberus_probe_sniff_control_t c=begin(0,6);uint64_t token=c.session_id;tick(20);
    before_mutex=renew_before_lock;tick(20);
    CHECK(!atomic_load(&probe_sniff_context->cancelled)&&sdk_receiving);
    wifi_promiscuous_pkt_t p=frame((const uint8_t*)"DRAIN",5,1);after_probe_unlock=stop_during_parse;packet(&p);
    CHECK(probe_sniff_context->table.summary.count==1&&probe_sniff_context->inflight==0);
    for(unsigned i=0;i<4;++i)tick(20);CHECK(!atomic_load(&probe_sniff_radio_reserved));
    copy_object(token,1,0,&(cerberus_probe_sniff_projection_t){0},76);dispose(token);
    reset();c=begin(CERBERUS_PROBE_SNIFF_HOP_24,0);token=c.session_id;tick(20);
    unsigned initial=sdk_channel;tick(251);CHECK(sdk_channel!=initial&&sdk_receiving);
    p=frame((const uint8_t*)"HOP",3,1);packet(&p);CHECK(probe_sniff_context->table.rows[0].last_channel==sdk_channel);
    dispose(token);CHECK(sdk_channel==11&&sdk_mode==7);
}
#include "probe_sniff_core_edges_check.h"
int main(void)
{
    public_payload_contract();
    pure_parser_edges();pure_table_edges();
    parser_inventory();source_identity_lifetime();cleanup_and_failures();immutable_pin_and_protocol();interleavings();
    reset();probe_sniff_boot_init();CHECK(probe_sniff_epoch&&probe_sniff_epoch!=100);CHECK(!probe_sniff_retained());
    printf("Probe Sniff native: %u assertions; actual RX dispatcher/worker/service, context %zu bytes, table %zu; no hardware.\n",checks,sizeof(probe_sniff_context_t),sizeof(cerberus_probe_sniff_table_t));return 0;
}
