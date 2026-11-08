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
#define WIFI_PROMIS_FILTER_MASK_DATA 2U
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
typedef struct{wifi_pkt_rx_ctrl_t rx_ctrl;uint8_t payload[5000];}wifi_promiscuous_pkt_t;
static int mutex,task,probe_sniff_mux,defense_capture_mux,packet_count_mux;
static atomic_bool packet_count_radio_reserved;
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
static void (*during_enable)(void);
static unsigned allocation_fail_at;
static wifi_band_mode_t sdk_mode=7;
static uint8_t sdk_channel=11;
static wifi_second_chan_t sdk_second=1;
static wifi_promiscuous_filter_t sdk_filter={7};
static void (*before_mutex)(void),(*after_probe_unlock)(void),(*after_count_unlock)(void);
static void xSemaphoreTake(void *m,unsigned timeout)
{CHECK(m==&mutex&&timeout==portMAX_DELAY&&!locked);if(before_mutex){void(*f)(void)=before_mutex;before_mutex=NULL;f();}locked=true;}
static void xSemaphoreGive(void *m){CHECK(m==&mutex&&locked);locked=false;}
static void enter_critical(int *m){CHECK((m==&probe_sniff_mux||m==&defense_capture_mux||m==&packet_count_mux)&&!critical);critical=true;}
static void exit_critical(int *m){CHECK(critical);critical=false;if(m==&probe_sniff_mux&&after_probe_unlock){void(*f)(void)=after_probe_unlock;after_probe_unlock=NULL;f();}
    if(m==&packet_count_mux&&after_count_unlock){void(*f)(void)=after_count_unlock;after_count_unlock=NULL;f();}}
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
static esp_err_t esp_wifi_set_promiscuous(bool enabled){int e=sdk();if(enabled){++enable_calls;if(during_enable){void(*f)(void)=during_enable;during_enable=NULL;f();}if(enable_error)e=enable_error;}else{++disable_calls;if(disable_error)e=disable_error;}if(!e)sdk_receiving=enabled;return e;}
static void *heap_objects[2];static size_t heap_sizes[2];
static void *heap_caps_calloc(size_t n,size_t size,unsigned caps)
{
    CHECK(n==1U&&caps==3&&!locked&&!critical);++allocations;
    if(allocation_failure||allocations==allocation_fail_at)return NULL;
    unsigned i=heap_objects[0]?1U:0U;CHECK(!heap_objects[i]);
    uint8_t *p=calloc(1,size+32U);CHECK(p);memset(p,0xa5,16);memset(p+size+16,0x5a,16);
    heap_objects[i]=p+16;heap_sizes[i]=size;return p+16;
}
static void heap_caps_free(void *ptr)
{
    CHECK(!locked&&!critical);if(!ptr)return;
    unsigned i=heap_objects[0]==ptr?0U:1U;CHECK(heap_objects[i]==ptr);
    uint8_t *p=(uint8_t *)ptr-16;
    for(unsigned j=0;j<16;++j)CHECK(p[j]==0xa5&&p[heap_sizes[i]+16+j]==0x5a);
    free(p);heap_objects[i]=NULL;++frees;
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
static uint32_t cerberus_wifi_capture_service_admission_token(void){return 0;}
static bool cerberus_wifi_capture_service_active(void){return false;}
static void cerberus_wifi_capture_service_receive(const uint8_t*p,unsigned n,unsigned ch,int rssi,int64_t time,uint32_t generation){(void)generation;(void)p;(void)n;(void)ch;(void)rssi;(void)time;CHECK(false);}
static void station_receive(const wifi_promiscuous_pkt_t*p){(void)p;}
static void cerberus_c5_test_receive(const uint8_t*p,unsigned n,unsigned ch){(void)p;(void)n;(void)ch;}
static bool sentinel_watch_protocol_decode_wifi(const uint8_t*p,unsigned n,sentinel_watch_protocol_record_t*out){(void)p;(void)n;(void)out;return false;}
static void cerberus_watch_capture_observe_timed(void*c,const void*p,const uint8_t*id,unsigned type,int rssi,unsigned ch,uint64_t time){(void)c;(void)p;(void)id;(void)type;(void)rssi;(void)ch;(void)time;CHECK(false);}
static void cerberus_watch_capture_observe(void*c,const void*p,const uint8_t*id,unsigned type,int rssi,unsigned ch){cerberus_watch_capture_observe_timed(c,p,id,type,rssi,ch,0);}
static void probe_sniff_receive(const wifi_promiscuous_pkt_t*p,unsigned kind,int64_t now){(void)p;(void)kind;(void)now;}
#include "wifi_packet_count_service.h"
#include "count_native_callback.h"
#include "count_native_admission.h"
static uint8_t tx_buffer[64];static size_t received_transfer_bytes=32;
static void prepare_frame_response(cerberus_spi_command_t command,uint8_t sequence,const void *payload,size_t size)
{CHECK(size==22U);CHECK(cerberus_protocol_prepare_frame((cerberus_spi_frame_t *)tx_buffer,command,sequence,payload,size));}
static void dispatch(const cerberus_spi_frame_t *request){switch((cerberus_spi_command_t)request->command){
#include "count_native_dispatch.h"
default:CHECK(false);}}
static cerberus_packet_count_control_t control;
static esp_err_t exchange(unsigned cmd,const void *q,size_t n)
{return cerberus_wifi_packet_count_exchange((uint8_t)cmd,q,n,&control,sizeof(control));}
static uint64_t token;
static cerberus_packet_count_request_t req;
static void tick(unsigned ms){now_us+=(int64_t)ms*1000;packet_count_worker_tick();}
static void reset(void)
{
    CHECK(!locked&&!critical&&!heap_objects[0]&&!heap_objects[1]&&!packet_count_context);
    scanner_ready=true;station_reserved=foreign_radio=allocation_failure=false;
    allocation_fail_at=0;now_us=1000000;sdk_calls=notifications=enable_calls=disable_calls=fail_sdk_at=restore_clock_jump=0;
    enable_error=disable_error=restore_error=0;sdk_receiving=false;sdk_channel=11;sdk_mode=7;sdk_second=1;sdk_filter.filter_mask=7;
    atomic_store(&packet_count_radio_reserved,false);memset(&scan_status,0,sizeof(scan_status));
    memset(&defense_status,0,sizeof(defense_status));memset(&channel_survey_status,0,sizeof(channel_survey_status));
    during_enable=NULL;before_mutex=NULL;after_probe_unlock=NULL;after_count_unlock=NULL;packet_count_boot_init();
}
static cerberus_packet_count_begin_t begin_request(bool raw,unsigned count)
{return (cerberus_packet_count_begin_t){.session_id=packet_count_epoch+1U,.channel_mask=UINT64_C(1)<<10,
    .target_count=(uint8_t)count,.dwell_ms=100,.record=(uint8_t)raw};}
static const uint8_t target_mac[6]={2,3,4,5,6,7};
static void begin_channel(bool raw,unsigned count,uint8_t channel)
{
    cerberus_packet_count_begin_t q=begin_request(raw,count);
    q.channel_mask=cerberus_wifi_packet_count_channel_bit(channel);token=q.session_id;req.session_id=token;
    CHECK(exchange(CERBERUS_PACKET_COUNT_COMMAND_BEGIN,&q,sizeof(q))==ESP_OK);
    CHECK(control.state==CERBERUS_PACKET_COUNT_STAGING&&(control.flags&CERBERUS_PACKET_COUNT_HELD));
    CHECK(!sdk_calls);CHECK(packet_count_retained());
    for(unsigned i=0;i<count;++i){cerberus_packet_count_target_t t={.session_id=token,.index=(uint8_t)i,.role=(uint8_t)(i+1U),.channel_hint=channel};
        memcpy(t.mac,target_mac,6U);CHECK(exchange(CERBERUS_PACKET_COUNT_COMMAND_TARGET,&t,sizeof(t))==ESP_OK);
        CHECK(exchange(CERBERUS_PACKET_COUNT_COMMAND_TARGET,&t,sizeof(t))==ESP_OK);}
}
static void begin(bool raw,unsigned count){begin_channel(raw,count,11);}
static void start(void)
{
    CHECK(exchange(CERBERUS_PACKET_COUNT_COMMAND_START,&req,sizeof(req))==ESP_OK);
    tick(0);tick(0);CHECK(packet_count_context->state==CERBERUS_PACKET_COUNT_RUNNING);
    CHECK(sdk_receiving);cerberus_wifi_scan_status_payload_t v;cerberus_wifi_scanner_get_status(&v);CHECK(v.state==CERBERUS_WIFI_SCAN_RUNNING);
}
static wifi_promiscuous_pkt_t packet;
static void frame(unsigned length)
{
    memset(&packet,0,sizeof(packet));packet.rx_ctrl.sig_len=length+4;packet.rx_ctrl.dump_len=length;
    packet.rx_ctrl.channel=11;packet.rx_ctrl.rssi=-41;packet.payload[0]=0x80;
    memset(packet.payload+4,0xff,6U);memcpy(packet.payload+10,target_mac,6U);memcpy(packet.payload+16,target_mac,6U);
}
static void receive(unsigned length){frame(length);now_us+=10;wifi_promiscuous_callback(&packet,WIFI_PKT_MGMT);}
static void stop(void)
{
    CHECK(exchange(CERBERUS_PACKET_COUNT_COMMAND_STOP,&req,sizeof(req))==ESP_OK);
    for(unsigned i=0;i<20U&&atomic_load(&packet_count_radio_reserved);++i)tick(20);
    CHECK(!atomic_load(&packet_count_radio_reserved));CHECK(!sdk_receiving);CHECK(packet_count_context->channel==0);
}
static void release(void)
{
    CHECK(exchange(CERBERUS_PACKET_COUNT_COMMAND_RELEASE,&req,sizeof(req))==ESP_OK);
    for(unsigned i=0;i<40U&&packet_count_context;++i)tick(20);
    CHECK(!packet_count_context);CHECK(!heap_objects[0]&&!heap_objects[1]);
    CHECK(exchange(CERBERUS_PACKET_COUNT_COMMAND_RELEASE,&req,sizeof(req))==ESP_OK);
    CHECK(control.session_id==token&&!control.flags&&!control.error_code);
}
static uint32_t peek(unsigned kind,unsigned index)
{
    cerberus_packet_count_peek_t q={.session_id=token,.kind=(uint8_t)kind,.index=(uint8_t)index};
    CHECK(exchange(CERBERUS_PACKET_COUNT_COMMAND_PEEK,&q,sizeof(q))==ESP_OK);return control.sequence;
}
static void projection(uint32_t pin,uint8_t *bytes,size_t size)
{
    for(unsigned i=0;i<(size+7U)/8U;++i){cerberus_packet_count_read_t q={token,pin,(uint16_t)i};cerberus_packet_count_chunk_t out;
        CHECK(cerberus_wifi_packet_count_exchange(CERBERUS_PACKET_COUNT_COMMAND_READ,&q,sizeof(q),&out,sizeof(out))==ESP_OK);
        CHECK(out.session_id==token&&out.pin==pin&&out.part==i);size_t n=size-i*8U;if(n>8U)n=8U;memcpy(bytes+i*8U,out.data,n);
        for(size_t j=n;j<8U;++j)CHECK(!out.data[j]);}
}
static void immediate(void)
{
    CHECK(exchange(CERBERUS_PACKET_COUNT_COMMAND_STATUS,&req,sizeof(req))==ESP_OK);
    CHECK(!(control.flags&CERBERUS_PACKET_COUNT_RX_ACTIVE));receive(40);
}
/* The callback metadata ends exactly at an inaccessible page. A mistaken
 * header read for MISC/CTRL or a short SIG therefore fails at the OS boundary. */
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
    unsigned callbacks=packet_count_context->table.summary.callbacks;
    wifi_promiscuous_callback(p,WIFI_PKT_MISC);
    wifi_promiscuous_callback(p,WIFI_PKT_CTRL);
    CHECK(packet_count_context->table.summary.callbacks==callbacks);
    for(unsigned n=0;n<4U;++n){
        p->rx_ctrl.sig_len=n;p->rx_ctrl.dump_len=0;
        wifi_promiscuous_callback(p,WIFI_PKT_MGMT);
    }
    CHECK(packet_count_context->table.summary.invalid_metadata==4U);
    p->rx_ctrl.sig_len=4;wifi_promiscuous_callback(p,WIFI_PKT_MGMT);
    CHECK(packet_count_context->table.summary.malformed_frames==1U);
    p->rx_ctrl.sig_len=44;p->rx_ctrl.rx_state=1;
    wifi_promiscuous_callback(p,WIFI_PKT_MGMT);
    CHECK(packet_count_context->table.summary.invalid_metadata==5U);
#ifdef _WIN32
    CHECK(VirtualFree(memory,0,MEM_RELEASE));
#else
    CHECK(!munmap(memory,page*2U));
#endif
}
static void recorded_extent(const cerberus_wifi_capture_record_t *r)
{
    uint8_t block[CERBERUS_WIFI_PCAP_RECORD_MAX_SIZE+1U];memset(block,0xa5,sizeof(block));
    size_t n=cerberus_wifi_pcapng_record(&r->metadata,r->frame,r->captured_length,
        r->original_length,block,sizeof(block)-1U);
    CHECK(n==32U+r->captured_length&&block[n]==0xa5);
    CHECK(cerberus_packet_count_get(block,4)==3U); /* Untimed SPB, no invented UTC. */
    CHECK(cerberus_packet_count_get(block+8,4)==16U+r->original_length);
    CHECK(block[20]==0U&&!memcmp(block+28,r->frame,r->captured_length)); /* No FCS flag/bytes. */
    CHECK(cerberus_packet_count_get(block+n-4U,4)==n);
}
static void public_payload_contract(void)
{
    const uint8_t channels[]={11,48};
    const unsigned dump_lengths[]={0,44,1,39,41,40};
    for(unsigned ch=0;ch<sizeof(channels);++ch){
        reset();begin_channel(true,1,channels[ch]);start();unsigned matched=0;
        for(unsigned kind=0;kind<2U;++kind)for(unsigned d=0;d<sizeof(dump_lengths)/sizeof(*dump_lengths);++d){
            frame(40);packet.rx_ctrl.channel=channels[ch];packet.rx_ctrl.dump_len=dump_lengths[d];
            packet.payload[0]=kind?0x08:0x80;memset(packet.payload+40,0xee,4);
            now_us+=10;wifi_promiscuous_callback(&packet,kind?WIFI_PKT_DATA:WIFI_PKT_MGMT);
            CHECK(packet_count_context->table.summary.matched_frames==++matched);
            CHECK(!packet_count_context->table.summary.invalid_metadata);
            const cerberus_wifi_capture_record_t *r=cerberus_wifi_capture_ring_peek(packet_count_context->raw);
            CHECK(r&&r->captured_length==40&&r->original_length==40&&!r->metadata.frame_has_fcs);
            CHECK(r->metadata.channel==channels[ch]&&!memcmp(r->frame,packet.payload,40));
            CHECK(r->frame[40]==0); /* Neither the FCS nor arbitrary dump extent was copied. */
            recorded_extent(r);
            cerberus_packet_count_ack_t ack={token,r->sequence};
            CHECK(exchange(CERBERUS_PACKET_COUNT_COMMAND_RAW_ACK,&ack,sizeof(ack))==ESP_OK);
        }
        frame(4096);packet.rx_ctrl.channel=channels[ch];packet.rx_ctrl.dump_len=0;
        memset(packet.payload+24,0x5a,4096U-24U);memset(packet.payload+4096,0xee,4);
        wifi_promiscuous_callback(&packet,WIFI_PKT_MGMT);
        const cerberus_wifi_capture_record_t *r=cerberus_wifi_capture_ring_peek(packet_count_context->raw);
        CHECK(r&&r->captured_length==4096&&r->original_length==4096&&!memcmp(r->frame,packet.payload,4096));
        recorded_extent(r);
        cerberus_packet_count_ack_t ack={token,r->sequence};CHECK(exchange(CERBERUS_PACKET_COUNT_COMMAND_RAW_ACK,&ack,sizeof(ack))==ESP_OK);
        frame(4097);packet.rx_ctrl.channel=channels[ch];packet.rx_ctrl.dump_len=0;
        wifi_promiscuous_callback(&packet,WIFI_PKT_MGMT);
        CHECK(packet_count_context->table.summary.matched_frames==matched+2U);
        cerberus_wifi_capture_ring_stats_t stats;cerberus_wifi_capture_ring_stats(packet_count_context->raw,&stats);
        CHECK(stats.accepted==matched+1U&&stats.dropped_oversize==1U&&!stats.pending);
        metadata_only_callback();stop();release();
    }
}
static void basic(void)
{
    reset();begin(false,2);during_enable=immediate;start();
    CHECK(packet_count_context->table.rows[0].observations==1&&packet_count_context->table.rows[1].observations==1);
    receive(40);receive(40);CHECK(packet_count_context->table.summary.matched_frames==3);
    uint8_t row[96],sum[128];uint32_t pin=peek(CERBERUS_PACKET_COUNT_READ_ROW,1);projection(pin,row,sizeof(row));
    CHECK(cerberus_packet_count_get(row+8,4)==3);CHECK(row[68]==(uint8_t)-41);
    uint8_t old[96];memcpy(old,row,sizeof(old));receive(40);projection(pin,row,sizeof(row));CHECK(!memcmp(row,old,sizeof(row)));
    stop();pin=peek(CERBERUS_PACKET_COUNT_READ_SUMMARY,0);projection(pin,sum,sizeof(sum));CHECK(sum[1]==CERBERUS_PACKET_COUNT_COMPLETE);
    CHECK(cerberus_packet_count_get(sum+48,4)==4);CHECK(cerberus_packet_count_get(sum+4,2)&CERBERUS_WIFI_PACKET_COUNT_STOPPED);
    unsigned enables=enable_calls;CHECK(exchange(CERBERUS_PACKET_COUNT_COMMAND_START,&req,sizeof(req))==ESP_OK);tick(0);CHECK(enable_calls==enables);
    release();
}
static void recording(void)
{
    reset();begin(true,1);start();for(unsigned i=0;i<5;++i)receive(4096);
    CHECK(packet_count_context->table.summary.matched_frames==5);
    uint32_t rawpin=peek(CERBERUS_PACKET_COUNT_READ_RAW_META,0);uint8_t meta[28];projection(rawpin,meta,sizeof(meta));
    uint32_t seq=(uint32_t)cerberus_packet_count_get(meta,4);CHECK(seq==1);CHECK(cerberus_packet_count_get(meta+4,2)==4096);
    uint32_t metric=peek(CERBERUS_PACKET_COUNT_READ_SUMMARY,0);uint8_t sum[128];projection(metric,sum,sizeof(sum));
    CHECK(cerberus_packet_count_get(sum+104,4)==2&&cerberus_packet_count_get(sum+112,4)==3);
    projection(rawpin,meta,sizeof(meta));CHECK((rawpin&1U)==0&&(metric&1U)==1);
    packet_count_context->inflight=1;
    cerberus_packet_count_peek_t busy={token,CERBERUS_PACKET_COUNT_READ_SUMMARY,0};
    CHECK(exchange(CERBERUS_PACKET_COUNT_COMMAND_PEEK,&busy,sizeof(busy))==ESP_ERR_NOT_FINISHED&&!control.sequence);
    projection(rawpin,meta,sizeof(meta));packet_count_context->inflight=0;
    uint8_t raw[4096];
    for(unsigned i=0;i<103U;++i){cerberus_packet_count_read_t q={token,seq,(uint16_t)i};cerberus_spi_frame_t request;
        CHECK(cerberus_protocol_prepare_frame(&request,CERBERUS_SPI_COMMAND_WIFI_PACKET_COUNT_RAW_READ,19,&q,sizeof(q)));
        received_transfer_bytes=64;memset(tx_buffer,0,sizeof(tx_buffer));dispatch(&request);
        cerberus_packet_count_raw_frame_t out;memcpy(&out,tx_buffer,sizeof(out));CHECK(cerberus_protocol_packet_count_raw_valid(&out));
        CHECK(out.sequence==19&&out.payload.pin==seq&&out.payload.part==i);size_t n=4096U-i*40U;if(n>40)n=40;
        memcpy(raw+i*40U,out.payload.data,n);for(size_t j=n;j<40;++j)CHECK(!out.payload.data[j]);}
    CHECK(cerberus_protocol_crc32(raw,sizeof(raw))==cerberus_packet_count_get(meta+16,4));
    cerberus_packet_count_ack_t ack={token,seq};CHECK(exchange(CERBERUS_PACKET_COUNT_COMMAND_RAW_ACK,&ack,sizeof(ack))==ESP_OK);
    CHECK(control.sequence==seq);CHECK(exchange(CERBERUS_PACKET_COUNT_COMMAND_RAW_ACK,&ack,sizeof(ack))==ESP_OK);CHECK(packet_count_context->acked==1);
    ack.sequence=99;CHECK(exchange(CERBERUS_PACKET_COUNT_COMMAND_RAW_ACK,&ack,sizeof(ack))!=ESP_OK);CHECK(!control.sequence);
    projection(metric,sum,sizeof(sum));CHECK(cerberus_packet_count_get(sum+112,4)==3);
    stop();release();
    reset();begin(true,1);start();
    atomic_store(&packet_count_context->raw->next_sequence,CERBERUS_PACKET_COUNT_SEQUENCE_MAX-1U);
    receive(24);receive(24); /* second callback sees exhausted sequence before worker */
    CHECK(exchange(CERBERUS_PACKET_COUNT_COMMAND_STATUS,&req,sizeof(req))==ESP_OK);
    CHECK(control.flags&CERBERUS_PACKET_COUNT_EXHAUSTED);
    for(unsigned i=0;i<20&&atomic_load(&packet_count_radio_reserved);++i)tick(20);
    CHECK(packet_count_context->error==ESP_ERR_INVALID_STATE);
    CHECK(atomic_load(&packet_count_context->raw->next_sequence)==CERBERUS_PACKET_COUNT_SEQUENCE_MAX);
    release();
}
static void failures(void)
{
    reset();begin(false,1);start();receive(24);disable_error=ESP_FAIL;
    uint64_t exposure=0;CHECK(cerberus_wifi_packet_count_copy_channel(&packet_count_context->table,10,now_us,&exposure));
    CHECK(exchange(CERBERUS_PACKET_COUNT_COMMAND_STOP,&req,sizeof(req))==ESP_OK);
    CHECK(!packet_count_context->table.window_open&&packet_count_context->table.channel_window_us[10]==exposure);
    for(unsigned i=0;i<800U;++i)tick(20);CHECK(packet_count_context&&atomic_load(&packet_count_radio_reserved));
    CHECK(packet_count_context->table.channel_window_us[10]==exposure);
    CHECK(exchange(CERBERUS_PACKET_COUNT_COMMAND_STATUS,&req,sizeof(req))==ESP_OK);CHECK(control.flags&CERBERUS_PACKET_COUNT_HELD);
    disable_error=0;for(unsigned i=0;i<50U&&atomic_load(&packet_count_radio_reserved);++i)tick(20);
    CHECK(packet_count_context->table.channel_window_us[10]==exposure);
    CHECK(!atomic_load(&packet_count_radio_reserved)&&packet_count_context->error==ESP_FAIL);release();
    reset();begin(false,1);enable_error=ESP_FAIL;during_enable=immediate;
    CHECK(exchange(CERBERUS_PACKET_COUNT_COMMAND_START,&req,sizeof(req))==ESP_OK);
    for(unsigned i=0;i<20U&&atomic_load(&packet_count_radio_reserved);++i)tick(20);
    CHECK(packet_count_context->table.summary.matched_frames==1);CHECK(packet_count_context->error==ESP_FAIL);release();
    reset();begin(false,1);tick(10001);tick(0);CHECK(!atomic_load(&packet_count_radio_reserved));CHECK(packet_count_context->error==ESP_ERR_TIMEOUT);release();
    for(unsigned fail=1;fail<=2;++fail){reset();unsigned before=frees;allocation_fail_at=allocations+fail;
        cerberus_packet_count_begin_t q=begin_request(true,1);CHECK(exchange(CERBERUS_PACKET_COUNT_COMMAND_BEGIN,&q,sizeof(q))==ESP_ERR_NO_MEM);
        CHECK(!packet_count_context&&!heap_objects[0]&&!heap_objects[1]);CHECK(frees-before==fail-1U);}
    reset();begin(false,1);start();now_us=-1;tick(0);now_us=2000000;for(unsigned i=0;i<20U&&atomic_load(&packet_count_radio_reserved);++i)tick(20);
    CHECK(packet_count_context->table.summary.flags&CERBERUS_WIFI_PACKET_COUNT_CLOCK_INVALID);release();
    reset();begin(false,1);start();receive(24);now_us-=15;receive(24);now_us+=100;
    CHECK(packet_count_context->table.summary.flags&CERBERUS_WIFI_PACKET_COUNT_CLOCK_INVALID);
    tick(0);CHECK(packet_count_context->error==ESP_ERR_INVALID_STATE&&atomic_load(&packet_count_context->cancelled));
    for(unsigned i=0;i<20U&&atomic_load(&packet_count_radio_reserved);++i)tick(20);
    CHECK(!atomic_load(&packet_count_radio_reserved));release();
}
static void stop_inside_enable(void)
{
    CHECK(exchange(CERBERUS_PACKET_COUNT_COMMAND_STOP,&req,sizeof(req))==ESP_OK);
    receive(40); /* admission already revoked */
}
static void protocol_edges(void)
{
    reset();begin(false,1);
    cerberus_packet_count_begin_t same=packet_count_context->request;
    unsigned before=allocations;CHECK(exchange(CERBERUS_PACKET_COUNT_COMMAND_BEGIN,&same,sizeof(same))==ESP_OK);CHECK(allocations==before);
    same.reserved=1;CHECK(exchange(CERBERUS_PACKET_COUNT_COMMAND_BEGIN,&same,sizeof(same))==ESP_ERR_INVALID_ARG);
    CHECK(control.flags&CERBERUS_PACKET_COUNT_BOUND);same.reserved=0;
    cerberus_packet_count_target_t bad={.session_id=token,.index=0,.role=1,.channel_hint=11};memcpy(bad.mac,target_mac,6U);bad.mac[5]++;
    CHECK(exchange(CERBERUS_PACKET_COUNT_COMMAND_TARGET,&bad,sizeof(bad))==ESP_ERR_INVALID_ARG);
    cerberus_packet_count_request_t foreign={token+1U};CHECK(exchange(CERBERUS_PACKET_COUNT_COMMAND_STOP,&foreign,sizeof(foreign))==ESP_ERR_NOT_FOUND);
    CHECK(!atomic_load(&packet_count_context->cancelled));
    during_enable=stop_inside_enable;CHECK(exchange(CERBERUS_PACKET_COUNT_COMMAND_START,&req,sizeof(req))==ESP_OK);
    for(unsigned i=0;i<20&&atomic_load(&packet_count_radio_reserved);++i)tick(20);
    CHECK(!atomic_load(&packet_count_radio_reserved));CHECK(!packet_count_context->table.summary.matched_frames);release();
    reset();begin(false,2);packet_count_context->configured=1;
    CHECK(exchange(CERBERUS_PACKET_COUNT_COMMAND_START,&req,sizeof(req))==ESP_ERR_INVALID_STATE&&!sdk_calls);
    release();
    reset();begin(false,1);start();receive(24);
    uint32_t pin=peek(CERBERUS_PACKET_COUNT_READ_ROW,0);cerberus_packet_count_chunk_t out;
    for(unsigned i=0;i<8;++i){cerberus_packet_count_read_t q={token,pin,12};
        if(i==1)q.session_id++;if(i==2)q.pin++;if(i==3)q.part=UINT16_MAX;
        CHECK(cerberus_wifi_packet_count_exchange(CERBERUS_PACKET_COUNT_COMMAND_READ,&q,sizeof(q),&out,sizeof(out))!=ESP_OK);
        CHECK(out.session_id==q.session_id&&out.part==q.part&&!out.pin);for(unsigned j=4;j<8;++j)CHECK(!out.data[j]);}
    packet_count_context->pin_counter=UINT32_MAX;
    cerberus_packet_count_peek_t q={token,CERBERUS_PACKET_COUNT_READ_ROW,0};CHECK(exchange(CERBERUS_PACKET_COUNT_COMMAND_PEEK,&q,sizeof(q))!=ESP_OK);
    CHECK(!control.sequence);for(unsigned i=0;i<20&&atomic_load(&packet_count_radio_reserved);++i)tick(20);release();
    reset();packet_count_epoch=UINT64_MAX;cerberus_packet_count_begin_t z=begin_request(false,1);z.session_id=1;
    CHECK(exchange(CERBERUS_PACKET_COUNT_COMMAND_BEGIN,&z,sizeof(z))==ESP_ERR_INVALID_STATE&&!packet_count_context);
    reset();begin(false,1);start();stop();
    same=packet_count_context->request;
    now_us+=INT64_C(30001000); /* no idle-worker tick: a legacy worker may be busy */
    CHECK(exchange(CERBERUS_PACKET_COUNT_COMMAND_BEGIN,&same,sizeof(same))==ESP_OK);
    CHECK(packet_count_context->release_requested&&!(control.flags&CERBERUS_PACKET_COUNT_DATA_READY));
    CHECK(!atomic_load(&packet_count_radio_reserved));release();
}
static void sdk_matrix(void)
{
    for(unsigned at=1;at<=9;++at){reset();begin(false,1);fail_sdk_at=at;
        CHECK(exchange(CERBERUS_PACKET_COUNT_COMMAND_START,&req,sizeof(req))==ESP_OK);
        for(unsigned i=0;i<60&&atomic_load(&packet_count_radio_reserved);++i)tick(20);
        if(atomic_load(&packet_count_radio_reserved))stop();
        CHECK(packet_count_context->error==ESP_FAIL);release();}
    reset();begin(false,1);start();restore_error=ESP_FAIL;
    CHECK(exchange(CERBERUS_PACKET_COUNT_COMMAND_STOP,&req,sizeof(req))==ESP_OK);
    for(unsigned i=0;i<900;++i)tick(20);
    CHECK(atomic_load(&packet_count_radio_reserved)&&!sdk_receiving&&packet_count_context->error==ESP_FAIL);
    CHECK(!packet_count_context->release_requested);restore_error=0;
    for(unsigned i=0;i<50&&atomic_load(&packet_count_radio_reserved);++i)tick(20);release();
    reset();begin(true,1);start();receive(4097);CHECK(packet_count_context->table.summary.matched_frames==1);
    cerberus_packet_count_record_stats_t raw;packet_count_raw_stats(packet_count_context,&raw);
    CHECK(!raw.accepted&&raw.dropped_other==1);
    packet.rx_ctrl.rx_state=1;wifi_promiscuous_callback(&packet,WIFI_PKT_MGMT);
    CHECK(packet_count_context->table.summary.invalid_metadata==1);
    stop();release();
    reset();begin(false,1);start();receive(24);tick(10001);
    CHECK(packet_count_context->error==ESP_ERR_TIMEOUT);
    for(unsigned i=0;i<20&&atomic_load(&packet_count_radio_reserved);++i)tick(20);
    uint32_t pin=peek(CERBERUS_PACKET_COUNT_READ_SUMMARY,0);CHECK(control.error_code==ESP_ERR_TIMEOUT&&pin);
    uint8_t sum[128];projection(pin,sum,sizeof(sum));CHECK((int32_t)cerberus_packet_count_get(sum+12,4)==ESP_ERR_TIMEOUT);
    release();
}
static void stop_during_raw_commit(void)
{
    now_us+=100;
    CHECK(exchange(CERBERUS_PACKET_COUNT_COMMAND_STOP,&req,sizeof(req))==ESP_OK);
    tick(0);tick(0);tick(0);
    CHECK(packet_count_context->inflight==1&&atomic_load(&packet_count_radio_reserved));
    CHECK(packet_count_context->table.summary.matched_frames==1);
    CHECK(!(packet_count_context->table.summary.flags&CERBERUS_WIFI_PACKET_COUNT_CLOCK_INVALID));
}
static void admitted_callback_drain(void)
{
    reset();begin(true,1);start();after_count_unlock=stop_during_raw_commit;receive(40);
    CHECK(!packet_count_context->inflight);
    CHECK(cerberus_wifi_capture_ring_peek(packet_count_context->raw)!=NULL);
    for(unsigned i=0;i<20&&atomic_load(&packet_count_radio_reserved);++i)tick(20);
    CHECK(!atomic_load(&packet_count_radio_reserved));
    CHECK(!(packet_count_context->table.summary.flags&CERBERUS_WIFI_PACKET_COUNT_CLOCK_INVALID));
    release();
}
int main(void){public_payload_contract();basic();recording();failures();protocol_edges();sdk_matrix();admitted_callback_drain();printf("Packet Count native: %u checks PASS; context=%zu raw=%zu\n",checks,sizeof(packet_count_context_t),sizeof(cerberus_wifi_capture_ring_t));return 0;}
