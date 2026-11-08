/* Actual receiver/worker and shared parser; no alternate radio model. */
#define main monitor_regressions
#include "wifi_packet_monitor_worker_check.c"
#undef main

static const uint8_t target[6] = {2,3,4,5,6,7};
static cerberus_wifi_fox_hunt_start_t begin(uint8_t channel, uint8_t band)
{
    cerberus_wifi_fox_hunt_status_t status;
    cerberus_wifi_fox_hunt_info(&status);
    assert(!(status.flags & CERBERUS_WIFI_FOX_HUNT_HELD));
    cerberus_wifi_fox_hunt_start_t request = {.session_id=status.session_id+1U,
        .initial_channel=channel,.search_band=band};
    memcpy(request.target,target,6);
    assert(cerberus_wifi_fox_hunt_start(&request,&status)==ESP_OK);
    assert(status.flags==3U && status.state==CERBERUS_WIFI_PACKET_MONITOR_RUNNING);
    assert(status.current_channel==0 && status.last_target_channel==0 && !record_heap_bytes);
    return request;
}
static wifi_promiscuous_pkt_t frame(uint8_t fc, unsigned bytes)
{
    wifi_promiscuous_pkt_t p={.rx_ctrl={0,bytes+4U,current_channel,-43}};
    p.payload[0]=fc;memcpy(p.payload+10,target,6);return p;
}
static void receive(wifi_promiscuous_pkt_t *p,unsigned type)
{wifi_fox_hunt_receive(p,type,esp_timer_get_time(),atomic_load(&wifi_fox_hunt_admission_token));}
static cerberus_wifi_fox_hunt_signal_t signal(void)
{
    cerberus_wifi_fox_hunt_signal_t out;
    assert(cerberus_wifi_fox_hunt_signal(packet_monitor_session_id,&out)==ESP_OK);return out;
}
static void traffic(void)
{
    assert(current_filter.filter_mask==7U && current_ctrl_filter.filter_mask==WIFI_PROMIS_CTRL_FILTER_MASK_ALL);
    uint32_t before=signal().sequence;
    wifi_promiscuous_pkt_t p=frame(0x80,24);
    memcpy(p.payload+4,target,6);memcpy(p.payload+16,target,6);p.payload[15]++;
    receive(&p,WIFI_PKT_MGMT);assert(signal().sequence==before); /* RA/BSSID is not TA. */
    p=frame(0xd4,10);receive(&p,WIFI_PKT_CTRL); /* ACK has no TA. */
    p=frame(0x94,16);receive(&p,WIFI_PKT_CTRL); /* variable BA excluded. */
    p=frame(0x88,24);receive(&p,WIFI_PKT_DATA); /* truncated QoS. */
    p=frame(0x80,24);p.rx_ctrl.rssi=-128;receive(&p,WIFI_PKT_MGMT);
    p.rx_ctrl.rssi=21;receive(&p,WIFI_PKT_MGMT);
    p.rx_ctrl.rssi=-43;p.rx_ctrl.rx_state=1;receive(&p,WIFI_PKT_MGMT);
    p.rx_ctrl.rx_state=0;p.rx_ctrl.channel=36;receive(&p,WIFI_PKT_MGMT);
    p.rx_ctrl.channel=current_channel;
    wifi_fox_hunt_receive(&p,WIFI_PKT_MGMT,esp_timer_get_time(),atomic_load(&wifi_fox_hunt_admission_token)+1U);
    wifi_fox_hunt_receive(&p,WIFI_PKT_MGMT,wifi_fox_hunt_armed_us-1,atomic_load(&wifi_fox_hunt_admission_token));
    assert(signal().sequence==before);
    receive(&p,WIFI_PKT_MGMT);
    p=frame(0xb4,16);receive(&p,WIFI_PKT_CTRL);
    p=frame(0x08,24);receive(&p,WIFI_PKT_DATA);
    assert(signal().sequence==before+3U && signal().rssi==-43 && signal().age_ms==0);
    packet_monitor_receive(&p,WIFI_PKT_DATA); /* Cannot populate Monitor/Analyzer history. */
    assert(!packet_monitor_ring.latest_sequence);
}
static uint32_t previous_token;
static unsigned hop_seen;
static void search_timing(void)
{
    uint32_t elapsed=clock_ms-1000U;
    if (elapsed<=1500U) assert(current_channel==1U);
    else if (elapsed<=1750U) assert(current_channel==2U);
    else if (elapsed<=2000U) assert(current_channel==3U);
    else if (elapsed<=2250U) assert(current_channel==1U);
    cerberus_wifi_fox_hunt_status_t status;
    assert(cerberus_wifi_fox_hunt_status(packet_monitor_session_id,&status)==ESP_OK);
    if (elapsed<=2250U) assert(!status.sweeps);
    else assert(status.sweeps==1U);
    uint32_t token=atomic_load(&wifi_fox_hunt_admission_token);
    if (previous_token && token!=previous_token) {
        wifi_promiscuous_pkt_t p=frame(0x80,24);
        wifi_fox_hunt_receive(&p,WIFI_PKT_MGMT,esp_timer_get_time(),previous_token);
        assert(!signal().sequence);++hop_seen;
    }
    previous_token=token;
}
static void reacquire(void)
{
    if (clock_ms==2600U) {
        assert(current_channel==2U);
        wifi_promiscuous_pkt_t p=frame(0x80,24);receive(&p,WIFI_PKT_MGMT);
    }
    if (clock_ms>2600U && clock_ms<=4100U) assert(current_channel==2U);
    assert(!wifi_fox_hunt_sweeps);
}
static void clock_fault(void) {clock_ms=900U;}
static void sequence_exhaustion(void)
{wifi_fox_hunt_latest.sequence=UINT32_MAX;wifi_promiscuous_pkt_t p=frame(0x80,24);receive(&p,WIFI_PKT_MGMT);}
static void rejected_hop(void)
{
    if (clock_ms==2550U) {
        cerberus_wifi_fox_hunt_status_t status;
        assert(cerberus_wifi_fox_hunt_status(packet_monitor_session_id,&status)==ESP_OK);
        assert(status.flags&CERBERUS_WIFI_FOX_HUNT_CHANNELS_SKIPPED);
        assert(!(status.flags&CERBERUS_WIFI_FOX_HUNT_RECEIVING) && !status.current_channel);
        assert(status.error_code==ESP_FAIL && status.state==CERBERUS_WIFI_PACKET_MONITOR_RUNNING);
    }
}
static void all_rejected(void)
{reject_tune_channel=wifi_fox_hunt_channels[(samples-30U)%wifi_fox_hunt_channel_count];}
static void five_ghz_search(void)
{
    assert(current_band==WIFI_BAND_5G && (current_channel==36 || current_channel==40));
    assert(wifi_fox_hunt_channel_count==2);
    if (clock_ms<=2500U) assert(current_channel==36);
    else if (clock_ms<=2750U) assert(current_channel==40);
}
static unsigned prior_enables;
static void delayed_worker(void)
{
    assert(enable_calls-prior_enables<=1U);
    prior_enables=enable_calls;
    if (samples==31) clock_ms+=1200U;
}

int main(void)
{
    assert(monitor_regressions()==0);
    assert(cerberus_wifi_fox_hunt_age(1001,0)==2 && cerberus_wifi_fox_hunt_age(0,1)==UINT32_MAX);
    assert(cerberus_wifi_fox_hunt_age(INT64_MAX,0)==UINT32_MAX);
    assert(cerberus_wifi_fox_hunt_channel_valid(177) && !cerberus_wifi_fox_hunt_channel_valid(166));
    reset();cerberus_wifi_fox_hunt_start_t request=begin(6,3);
    cerberus_wifi_fox_hunt_status_t status;cerberus_wifi_fox_hunt_signal_t out=signal();
    assert(!out.sequence && !out.channel && !out.observed_ms && out.rssi==-128 && out.age_ms==UINT32_MAX);
    assert(cerberus_wifi_fox_hunt_start(&request,&status)==ESP_OK); /* lost response replay */
    request.target[5]++;
    assert(cerberus_wifi_fox_hunt_start(&request,&status)==ESP_ERR_INVALID_ARG);request.target[5]--;
    cerberus_wifi_packet_monitor_session_status_t monitor;
    assert(cerberus_wifi_packet_monitor_session_stop(request.session_id,&monitor)==ESP_ERR_NOT_FOUND);
    cerberus_wifi_packet_monitor_session_start_t mstart={request.session_id,6};
    assert(cerberus_wifi_packet_monitor_session_start(&mstart,&monitor)==ESP_ERR_INVALID_ARG);
    cerberus_wifi_channel_analyzer_status_t analyzer;
    assert(cerberus_wifi_channel_analyzer_stop(request.session_id,&analyzer)==ESP_ERR_NOT_FOUND);
    cerberus_wifi_packet_monitor_session_info(&monitor);assert(monitor.flags==2 && monitor.error_code==ESP_ERR_INVALID_STATE);
    cerberus_wifi_channel_analyzer_info(&analyzer);assert(analyzer.flags==2 && analyzer.error_code==ESP_ERR_INVALID_STATE);
    assert(cerberus_wifi_fox_hunt_signal(request.session_id+1,&out)==ESP_ERR_NOT_FOUND && !out.session_id);
    fox_test_delay_hook=traffic;dispatch();out=signal();assert(out.sequence==9U);
    assert(cerberus_wifi_fox_hunt_status(request.session_id,&status)==ESP_OK && status.state==0 && !status.current_channel);
    assert(!(status.flags&(CERBERUS_WIFI_FOX_HUNT_HELD|CERBERUS_WIFI_FOX_HUNT_RECEIVING)));
    assert(current_band==9 && current_channel==6 && current_filter.filter_mask==0x20 && current_ctrl_filter.filter_mask==0x800000);
    run_ap_and_capture();

    reset();test_country.nchan=3;begin(1,1);stop_after=46;previous_token=0;hop_seen=0;
    fox_test_delay_hook=search_timing;dispatch();assert(hop_seen>=3 && wifi_fox_hunt_sweeps==1);
    reset();test_country.nchan=3;begin(1,1);stop_after=62;fox_test_delay_hook=reacquire;dispatch();
    assert(signal().sequence==1 && signal().channel==2 && !wifi_fox_hunt_sweeps);
    reset();test_country.nchan=3;begin(1,1);reject_tune_channel=2;stop_after=33;
    fox_test_delay_hook=rejected_hop;dispatch();assert(wifi_fox_hunt_skipped && !atomic_load(&packet_monitor_radio_reserved));
    reset();test_country.nchan=1;begin(1,1);stop_after=0;
    fox_test_delay_hook=all_rejected;dispatch();assert(packet_monitor_status.state==CERBERUS_WIFI_PACKET_MONITOR_ERROR);
    reset();test_country.policy=WIFI_COUNTRY_POLICY_MANUAL;test_country.wifi_5g_channel_mask=6U;
    begin(36,2);stop_after=41;fox_test_delay_hook=five_ghz_search;dispatch();assert(wifi_fox_hunt_sweeps==1);
    reset();test_country.nchan=3;begin(1,1);stop_after=40;prior_enables=0;
    fox_test_delay_hook=delayed_worker;dispatch();assert(!atomic_load(&packet_monitor_radio_reserved));
    reset();request=begin(6,3);request.search_band=2;
    assert(!cerberus_wifi_fox_hunt_start_valid(&request));
    request.search_band=3;request.target[0]=1;
    assert(!cerberus_wifi_fox_hunt_start_valid(&request));dispatch();

    reset();begin(6,3);fox_test_delay_hook=clock_fault;dispatch();assert(packet_monitor_status.state==CERBERUS_WIFI_PACKET_MONITOR_ERROR);
    reset();begin(6,3);fox_test_delay_hook=sequence_exhaustion;dispatch();assert(packet_monitor_status.state==CERBERUS_WIFI_PACKET_MONITOR_ERROR && signal().sequence==UINT32_MAX);
    reset();begin(6,3);atomic_store(&wifi_fox_hunt_admission_token,UINT32_MAX);dispatch();assert(packet_monitor_status.state==CERBERUS_WIFI_PACKET_MONITOR_ERROR);
    reset();begin(6,3);cancel_during_enable=true;dispatch();assert(!signal().sequence && !packet_monitor_receiving);
    reset();begin(6,3);stop_after=0;dispatch();assert(clock_ms==11000U && !atomic_load(&packet_monitor_radio_reserved)); /* lease */
    reset();begin(14,1);dispatch();assert(packet_monitor_status.state==CERBERUS_WIFI_PACKET_MONITOR_ERROR && !enable_calls); /* country */
    reset();begin(6,3);fail_ctrl_filter_restore=true;dispatch();assert(atomic_load(&packet_monitor_radio_reserved));
    fail_ctrl_filter_restore=false;
    assert(cerberus_wifi_fox_hunt_stop(packet_monitor_session_id,&status)==ESP_OK);dispatch();
    assert(!atomic_load(&packet_monitor_radio_reserved));run_ap_and_capture();
    puts("C5 Wi-Fi Fox Hunt: TA parsing, session isolation, measured loss/search, channel faults, leases and cleanup passed.");
    return 0;
}
