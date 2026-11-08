/* Exercise the actual outer callback and capture service together. All other
 * consumers use the Watch fixture's inactive SDK boundaries. The first foreign
 * consumer can delay the frame across a capture turnover before publication. */
#define cerberus_wardrive_radio_wifi_rx inactive_wardrive_receive
#include "wardrive_foreign_boundary.h"
#undef cerberus_wardrive_radio_wifi_rx
static void (*capture_callback_hook)(void);
static void cerberus_wardrive_radio_wifi_rx(void *packet,int type)
{(void)packet;(void)type;if(capture_callback_hook)capture_callback_hook();}
#define CERBERUS_CAPTURE_OUTER_TEST 1
#define main watch_fixture_main
#include "c5_wifi_watch_timing_check.c"
#undef main
#include <stdlib.h>

size_t heap_caps_get_free_size(unsigned caps){(void)caps;return 100000U;}
void *heap_caps_malloc(size_t size,unsigned caps){(void)caps;return malloc(size);}

static cerberus_wifi_capture_start_request_t capture_request={
    .generation=1U,.channel=149U,.mode_mask=CERBERUS_WIFI_CAPTURE_MODE_BEACON,
    .lease_ms=1000U};
static cerberus_wifi_capture_status_payload_t capture_status(void)
{cerberus_wifi_capture_status_payload_t s;cerberus_wifi_capture_service_status(&s);return s;}
static void start_capture(void)
{
    assert(cerberus_wifi_capture_service_start(&capture_request)==ESP_OK);
    assert(!cerberus_wifi_capture_service_admission_token());
    assert(cerberus_wifi_capture_service_set_channel(capture_request.channel)==ESP_OK);
    assert(cerberus_wifi_capture_service_admission_token()!=0U);
}
static void stop_drain(void)
{
    cerberus_wifi_capture_service_stop(ESP_OK);
    assert(!cerberus_wifi_capture_service_inflight());
    cerberus_wifi_capture_peek_payload_t p;
    while(cerberus_wifi_capture_service_peek(capture_request.generation,&p)==ESP_OK&&p.sequence){
        cerberus_wifi_capture_ack_request_t ack={capture_request.generation,p.sequence};
        assert(cerberus_wifi_capture_service_ack(&ack)==ESP_OK);
    }
}
static void delay_only(void){clock_us+=5000;}
static void new_capture(void)
{
    capture_callback_hook=NULL;
    stop_drain();++capture_request.generation;clock_us+=5000;
    start_capture();
}
static void rearm_capture(void)
{
    capture_callback_hook=NULL;
    capture_clock_hook=NULL;
    assert(cerberus_wifi_capture_service_set_channel(0U)==ESP_OK);
    clock_us+=5000;
    assert(cerberus_wifi_capture_service_set_channel(capture_request.channel)==ESP_OK);
}
static void assert_empty(void)
{
    cerberus_wifi_capture_status_payload_t s=capture_status();
    assert(s.generation==capture_request.generation&&!s.accepted&&!s.pending);
    assert(!cerberus_wifi_capture_service_inflight());
}
static void assert_entry_record(int64_t entered)
{
    static uint32_t last_sequence,last_generation;
    cerberus_wifi_capture_peek_payload_t p;
    assert(cerberus_wifi_capture_service_peek(capture_request.generation,&p)==ESP_OK);
    uint32_t expected=p.generation==last_generation?last_sequence+1U:1U;
    assert(p.generation==capture_request.generation&&p.sequence==expected&&p.length==73U);
    last_generation=p.generation;
    last_sequence=p.sequence;
    assert(p.channel==149U&&p.rssi_dbm==-40);
    assert(p.crc32==cerberus_protocol_crc32(packet.payload,73U));
    uint64_t time=0;for(unsigned i=0;i<6U;++i)time|=(uint64_t)p.monotonic_us_le48[i]<<(8U*i);
    assert(time==(uint64_t)entered);
}
int main(void)
{
    begin(1);watch_capture_active=false;clock_us=1000000;start_capture();
    capture_callback_hook=delay_only;
    receive();assert(clock_us==1005000);assert_entry_record(1000000);
    stop_drain();++capture_request.generation;start_capture();
    /* Same channel, new generation: old callback cannot become a new record. */
    capture_callback_hook=new_capture;receive();assert_empty();
    receive();assert_entry_record(clock_us);stop_drain();
    /* A callback entering with admission closed cannot join a later capture. */
    capture_callback_hook=new_capture;receive();assert_empty();
    receive();assert_entry_record(clock_us);stop_drain();
    ++capture_request.generation;start_capture();
    /* The outer callback can be preempted between its admission-token and
     * clock reads. Same-generation rearm must reject that old token even
     * though the callback then samples a timestamp after the new arm. */
    capture_clock_hook=rearm_capture;receive();assert_empty();
    receive();assert_entry_record(clock_us);stop_drain();
    ++capture_request.generation;start_capture();
    /* Same generation/channel after a retune still requires its new arm time. */
    capture_callback_hook=rearm_capture;receive();assert_empty();
    receive();assert_entry_record(clock_us);stop_drain();
    ++capture_request.generation;start_capture();
    /* The actual callback preserves signed time and rejects impossible clocks. */
    clock_us=-1;receive();assert_empty();
    clock_us=INT64_C(0x1000000000000);receive();assert_empty();
    stop_drain();
    ++capture_request.generation;capture_request.mode_mask=CERBERUS_WIFI_CAPTURE_MODE_RAW;
    clock_us=2000000;start_capture();capture_callback_hook=delay_only;
    memset(packet.payload,0,sizeof(packet.payload));packet.payload[0]=0xd4;
    packet.rx_ctrl.sig_len=14U; /* Ten-byte ACK header and four FCS bytes. */
    memset(packet.payload+10,0xb7,4);
    wifi_promiscuous_callback(&packet,WIFI_PKT_CTRL);
    cerberus_wifi_capture_peek_payload_t control;
    assert(cerberus_wifi_capture_service_peek(capture_request.generation,&control)==ESP_OK);
    assert(control.sequence==1&&control.length==10&&control.channel==149&&control.rssi_dbm==-40);
    assert(control.crc32==cerberus_protocol_crc32(packet.payload,10));
    uint64_t entered=0;for(unsigned i=0;i<6;++i)entered|=(uint64_t)control.monotonic_us_le48[i]<<(8U*i);
    assert(entered==2000000U&&clock_us==2005000U);
    packet.rx_ctrl.rx_state=1;wifi_promiscuous_callback(&packet,WIFI_PKT_CTRL);
    packet.rx_ctrl.rx_state=0;wifi_promiscuous_callback(&packet,WIFI_PKT_MISC);
    packet.rx_ctrl.sig_len=4;wifi_promiscuous_callback(&packet,WIFI_PKT_CTRL);
    assert(capture_status().accepted==1);stop_drain();
    puts("C5 actual capture callback: entry timestamp/CRC/FCS, short CTRL, same-channel generation turnover, closed admission, rearm boundary, negative/48-bit clock rejection passed.");
    return 0;
}
