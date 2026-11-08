/* Real C5 service/dispatcher, S3 wire adapter and model on the existing SDK
 * fixture. Device scheduling and a lost reply are controlled at the boundary. */
#include "monitor_bridge_fixture.h"
#include "wifi_channel_analyzer_scan.h"
#include "wifi_channel_analyzer_link.h"

static bool analyzer_drop_start;
static unsigned analyzer_start_calls;
static esp_err_t analyzer_start(uint64_t session, uint8_t channel,
    cerberus_wifi_channel_analyzer_status_t *out)
{
    ++analyzer_start_calls;
    esp_err_t error=c5_spi_link_wifi_channel_analyzer_start(session,channel,out);
    if(error==ESP_OK&&analyzer_drop_start){
        analyzer_drop_start=false;memset(out,0,sizeof(*out));return ESP_ERR_TIMEOUT;
    }
    return error;
}
static const wifi_channel_analyzer_transport_t analyzer_transport={
    .info=c5_spi_link_wifi_channel_analyzer_info,.start=analyzer_start,
    .status=c5_spi_link_wifi_channel_analyzer_status,.stop=c5_spi_link_wifi_channel_analyzer_stop,
    .sample=c5_spi_link_wifi_channel_analyzer_sample,
    .hint_peek=c5_spi_link_wifi_channel_analyzer_hint_peek,.hint_read=c5_spi_link_wifi_channel_analyzer_hint_read};

static void analyzer_tick(wifi_channel_analyzer_scan_t *s,unsigned elapsed)
{
    clock_ms+=elapsed;unsigned before=exchanges;
    (void)wifi_channel_analyzer_scan_tick(s,clock_ms,&analyzer_transport);
    assert(exchanges-before<=2U&&!link_locked);
}
static void analyzer_begin(wifi_channel_analyzer_scan_t *s,uint8_t channel,bool keep)
{
    assert(wifi_channel_analyzer_scan_start(s,channel,keep));
    analyzer_tick(s,0);assert(s->phase==WIFI_ANALYZER_START);
    analyzer_tick(s,20);assert(s->phase==WIFI_ANALYZER_WATCH&&s->session);
    assert(cerberus_wifi_packet_monitor_active());
}
static void analyzer_collect(wifi_channel_analyzer_scan_t *s)
{
    for(unsigned n=0;n<200&&s->phase!=WIFI_ANALYZER_OFF;++n)analyzer_tick(s,20);
    assert(s->phase==WIFI_ANALYZER_OFF&&!s->session&&!cerberus_wifi_packet_monitor_active());
}
static void compatibility_and_identity(void)
{
    bridge_reset();firmware=81;wifi_channel_analyzer_scan_t s={0};
    assert(wifi_channel_analyzer_scan_start(&s,36,false));analyzer_tick(&s,0);
    assert(s.error==ESP_ERR_NOT_SUPPORTED&&s.phase==WIFI_ANALYZER_OFF&&!cerberus_wifi_packet_monitor_active());

    bridge_reset();firmware=82;memset(&s,0,sizeof(s));analyzer_start_calls=0;
    analyzer_begin(&s,36,false);uint64_t session=s.session;
    cerberus_wifi_packet_monitor_session_status_t monitor={0};
    assert(c5_spi_link_wifi_packet_monitor_session_stop(session,&monitor)==ESP_OK);
    assert(!(monitor.flags&CERBERUS_WIFI_PACKET_MONITOR_BOUND)&&monitor.error_code!=ESP_OK);
    assert(cerberus_wifi_packet_monitor_active());
    cerberus_wifi_channel_analyzer_status_t wrong={0};
    assert(c5_spi_link_wifi_channel_analyzer_stop(session-1,&wrong)==ESP_OK);
    assert(!wrong.flags&&wrong.error_code!=ESP_OK&&cerberus_wifi_packet_monitor_active());
    dispatch();analyzer_collect(&s);
    wifi_channel_analyzer_summary_t summary;
    wifi_channel_analyzer_scan_summary(&s,&summary);
    assert(summary.windows&&summary.frames&&summary.window_us&&!summary.lower_bound);
    assert(analyzer_start_calls==1);
    run_ap_and_capture();
}
static void lost_start_and_retune(void)
{
    bridge_reset();firmware=82;wifi_channel_analyzer_scan_t s={0};
    analyzer_start_calls=0;analyzer_drop_start=true;
    analyzer_begin(&s,1,false);uint64_t session=s.session;
    assert(s.transport_error==ESP_ERR_TIMEOUT&&analyzer_start_calls==1);
    analyzer_tick(&s,700);
    assert(s.session==session&&s.transport_error==ESP_OK&&analyzer_start_calls==1);
    dispatch();analyzer_collect(&s);
    unsigned first=s.history_count;
    analyzer_begin(&s,44,true);assert(s.session!=session);
    samples=0;dispatch();analyzer_collect(&s);
    assert(s.history_count>first);
    unsigned markers=0;
    for(unsigned i=0;i<s.history_count;++i){wifi_channel_analyzer_entry_t row;
        assert(wifi_channel_analyzer_scan_history(&s,i,&row));markers+=row.kind==WIFI_ANALYZER_CHANNEL;}
    assert(markers==2U);
    run_ap_and_capture();
}
static void held_cleanup(void)
{
    bridge_reset();firmware=82;wifi_channel_analyzer_scan_t s={0};
    analyzer_begin(&s,36,false);fail_disable=true;
    dispatch();wifi_channel_analyzer_scan_stop(&s);analyzer_tick(&s,0);
    for(unsigned i=0;i<10;++i)analyzer_tick(&s,100);
    assert(s.phase!=WIFI_ANALYZER_OFF&&s.session&&cerberus_wifi_packet_monitor_active());
    assert(!wifi_channel_analyzer_scan_start(&s,44,true));
    fail_disable=false;
    for(unsigned i=0;i<30&&s.phase!=WIFI_ANALYZER_OFF;++i){analyzer_tick(&s,100);dispatch();}
    assert(s.phase==WIFI_ANALYZER_OFF&&!s.session&&!cerberus_wifi_packet_monitor_active());
    run_ap_and_capture();
}
static uint32_t pinned_projection;
static uint8_t pinned_hint[CERBERUS_WIFI_CHANNEL_ANALYZER_HINT_BYTES];
static void read_hint_part(unsigned part)
{
    cerberus_wifi_channel_analyzer_hint_part_t reply;
    assert(c5_spi_link_wifi_channel_analyzer_hint_read(packet_monitor_session_id,
        pinned_projection,(uint8_t)part,&reply)==ESP_OK);
    size_t offset=part*sizeof(reply.bytes),bytes=sizeof(pinned_hint)-offset;
    if(bytes>sizeof(reply.bytes))bytes=sizeof(reply.bytes);
    memcpy(pinned_hint+offset,reply.bytes,bytes);
}
static void live_hint_exchange(void)
{
    /* The SDK callback gets three accepted types; failed RX and another
     * channel must not enter the measured count. Beacon bytes are synthetic. */
    wifi_promiscuous_pkt_t packet={.rx_ctrl={0U,46U,current_channel,-61}};
    packet.payload[0]=0x80U;memset(packet.payload+4U,255U,6U);
    packet.payload[10]=packet.payload[16]=2U;
    packet.payload[15]=packet.payload[21]=samples==1U?1U:2U;
    packet.payload[36]=0U;packet.payload[37]=4U;
    memcpy(packet.payload+38U,"L\0\\\177",4U);
    packet_monitor_receive(&packet,WIFI_PKT_MGMT);
    packet.rx_ctrl.sig_len=14U;packet_monitor_receive(&packet,WIFI_PKT_CTRL);
    packet.rx_ctrl.sig_len=28U;packet_monitor_receive(&packet,WIFI_PKT_DATA);
    packet.rx_ctrl.rx_state=1U;packet_monitor_receive(&packet,WIFI_PKT_DATA);
    packet.rx_ctrl.rx_state=0U;packet.rx_ctrl.channel=1U;
    packet_monitor_receive(&packet,WIFI_PKT_CTRL);
    if(samples==1U){
        cerberus_wifi_channel_analyzer_hint_status_t hint;
        assert(c5_spi_link_wifi_channel_analyzer_hint_peek(packet_monitor_session_id,&hint)==ESP_OK);
        assert(hint.error_code==ESP_OK&&hint.age_ms==0U);
        pinned_projection=hint.projection;read_hint_part(0U);
    }else if(samples==2U){
        for(unsigned part=1U;part<CERBERUS_WIFI_CHANNEL_ANALYZER_HINT_PARTS;++part)read_hint_part(part);
        cerberus_wifi_channel_analyzer_hint_t hint;
        memcpy(&hint,pinned_hint,sizeof(hint));
        assert(hint.bssid[5]==1U&&hint.ssid_length==4U&&hint.rssi==-61&&hint.channel==36U);
        char name[32];wifi_channel_analyzer_scan_ssid_display(&hint,name,sizeof(name));
        assert(strcmp(name,"L\\x00\\x5C\\x7F")==0);
    }else if(samples==3U){
        cerberus_wifi_channel_analyzer_hint_status_t hint;
        assert(c5_spi_link_wifi_channel_analyzer_hint_peek(packet_monitor_session_id,&hint)==ESP_OK);
        cerberus_wifi_channel_analyzer_hint_part_t stale;
        assert(c5_spi_link_wifi_channel_analyzer_hint_read(packet_monitor_session_id,
            pinned_projection,0U,&stale)==ESP_ERR_NOT_FOUND);
        pinned_projection=hint.projection;
        for(unsigned part=0U;part<CERBERUS_WIFI_CHANNEL_ANALYZER_HINT_PARTS;++part)read_hint_part(part);
        cerberus_wifi_channel_analyzer_hint_t updated;memcpy(&updated,pinned_hint,sizeof(updated));
        assert(updated.bssid[5]==2U);
    }
}
static void measured_frames_and_pinned_hints(void)
{
    bridge_reset();firmware=82;wifi_channel_analyzer_scan_t s={0};
    analyzer_test_delay=live_hint_exchange;stop_after=5U;
    analyzer_begin(&s,36,false);dispatch();analyzer_collect(&s);
    wifi_channel_analyzer_summary_t summary;wifi_channel_analyzer_scan_summary(&s,&summary);
    assert(summary.windows==4U&&summary.frames==12U&&summary.window_us==400000U);
    assert(summary.average_rate==30U&&summary.peak_rate==30U);
    cerberus_wifi_channel_analyzer_sample_t missing;
    assert(c5_spi_link_wifi_channel_analyzer_sample(packet_monitor_session_id,5U,&missing)==ESP_ERR_NOT_FOUND);
    run_ap_and_capture();
}
int main(void)
{
    compatibility_and_identity();lost_start_and_retune();held_cleanup();measured_frames_and_pinned_hints();
    puts("Analyzer bridge: real dispatcher/adapter/model, old-peer refusal, cross-mode ownership, measured MGMT/CTRL/DATA samples, pinned beacon hints, lost Start, retained retune history and cleanup recovery passed.");
    return 0;
}
