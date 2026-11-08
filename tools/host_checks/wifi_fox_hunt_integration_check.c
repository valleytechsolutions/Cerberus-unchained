/* Production receiver, dispatcher, SPI adapter and model. Only the SDK clock,
 * packet delivery and transport failures are controlled by this fixture. */
#include "monitor_bridge_fixture.h"
#include "wifi_fox_hunt_scan.h"
#include "wifi_fox_hunt_link.h"

static const uint8_t fox_target[6] = {2, 0, 0, 0, 0, 41};
static bool fox_drop_start;
static unsigned fox_start_calls;
static esp_err_t fox_start(const cerberus_wifi_fox_hunt_start_t *request,
    cerberus_wifi_fox_hunt_status_t *out)
{
    ++fox_start_calls;
    esp_err_t error = c5_spi_link_wifi_fox_hunt_start(request, out);
    if (error == ESP_OK && fox_drop_start) {
        fox_drop_start = false;
        memset(out, 0, sizeof(*out));
        return ESP_ERR_TIMEOUT;
    }
    return error;
}
static const wifi_fox_hunt_transport_t fox_transport = {
    .info = c5_spi_link_wifi_fox_hunt_info, .start = fox_start,
    .status = c5_spi_link_wifi_fox_hunt_status, .stop = c5_spi_link_wifi_fox_hunt_stop,
    .signal = c5_spi_link_wifi_fox_hunt_signal
};

static void fox_tick(wifi_fox_hunt_scan_t *scan, unsigned elapsed)
{
    clock_ms += elapsed;
    unsigned before = exchanges;
    (void)wifi_fox_hunt_scan_tick(scan, clock_ms, &fox_transport);
    assert(exchanges - before <= 2U && !link_locked);
}
static void fox_begin(wifi_fox_hunt_scan_t *scan, uint8_t channel, bool keep)
{
    assert(wifi_fox_hunt_scan_start(scan, fox_target, channel,
        CERBERUS_WIFI_FOX_HUNT_BAND_BOTH, keep));
    fox_tick(scan, 0);
    assert(scan->phase == WIFI_FOX_START);
    fox_tick(scan, 20);
    assert(scan->phase == WIFI_FOX_WATCH && scan->session);
    assert(cerberus_wifi_packet_monitor_active());
}
static void fox_collect(wifi_fox_hunt_scan_t *scan)
{
    for (unsigned n = 0; n < 200 && scan->phase != WIFI_FOX_OFF; ++n)
        fox_tick(scan, 20);
    assert(scan->phase == WIFI_FOX_OFF && !scan->session);
    assert(!cerberus_wifi_packet_monitor_active());
}

static void compatibility_and_ownership(void)
{
    bridge_reset(); firmware = 82;
    wifi_fox_hunt_scan_t scan = {0};
    assert(wifi_fox_hunt_scan_start(&scan, fox_target, 36,
        CERBERUS_WIFI_FOX_HUNT_BAND_BOTH, false));
    fox_tick(&scan, 0);
    assert(scan.phase == WIFI_FOX_OFF && scan.error == ESP_ERR_NOT_SUPPORTED);
    assert(!cerberus_wifi_packet_monitor_active());

    bridge_reset(); firmware = 83;
    memset(&scan, 0, sizeof(scan)); fox_start_calls = 0;
    fox_begin(&scan, 36, false);
    cerberus_wifi_packet_monitor_session_status_t monitor;
    assert(c5_spi_link_wifi_packet_monitor_session_stop(scan.session, &monitor) == ESP_OK);
    assert(!(monitor.flags & CERBERUS_WIFI_PACKET_MONITOR_BOUND));
    assert(monitor.error_code != ESP_OK && cerberus_wifi_packet_monitor_active());
    cerberus_wifi_channel_analyzer_status_t analyzer;
    assert(cerberus_wifi_channel_analyzer_stop(scan.session, &analyzer) == ESP_ERR_NOT_FOUND);
    assert(!(analyzer.flags & CERBERUS_WIFI_CHANNEL_ANALYZER_BOUND));
    assert(analyzer.error_code != ESP_OK && cerberus_wifi_packet_monitor_active());
    cerberus_wifi_fox_hunt_status_t wrong;
    assert(c5_spi_link_wifi_fox_hunt_stop(scan.session - 1U, &wrong) == ESP_OK);
    assert(!(wrong.flags & CERBERUS_WIFI_FOX_HUNT_BOUND));
    assert(wrong.error_code != ESP_OK && cerberus_wifi_packet_monitor_active());
    cerberus_wifi_fox_hunt_signal_t missing;
    assert(c5_spi_link_wifi_fox_hunt_signal(scan.session - 1U, &missing) == ESP_ERR_NOT_FOUND);
    assert(!missing.session_id && !missing.sequence && cerberus_wifi_packet_monitor_active());
    dispatch(); fox_collect(&scan);
    assert(fox_start_calls == 1U);
    run_ap_and_capture();
}

static void lost_start_reply(void)
{
    bridge_reset(); firmware = 83;
    wifi_fox_hunt_scan_t scan = {0};
    fox_start_calls = 0; fox_drop_start = true;
    fox_begin(&scan, 1, false);
    uint64_t session = scan.session;
    assert(scan.transport_error == ESP_ERR_TIMEOUT && fox_start_calls == 1U);
    fox_tick(&scan, 700);
    assert(scan.session == session && scan.transport_error == ESP_OK);
    assert(fox_start_calls == 1U);
    dispatch(); fox_collect(&scan);
    run_ap_and_capture();
}

static wifi_fox_hunt_scan_t *live_scan;
static unsigned valid_reports;
static void deliver_fox_packet(const wifi_promiscuous_pkt_t *packet)
{
    wifi_fox_hunt_receive(packet, WIFI_PKT_DATA, esp_timer_get_time(),
        atomic_load_explicit(&wifi_fox_hunt_admission_token, memory_order_acquire));
}
static void receive_selected_transmitter(void)
{
    wifi_promiscuous_pkt_t packet = {.rx_ctrl = {0U, 28U, current_channel, -58}};
    packet.payload[0] = 0x08U;
    memcpy(packet.payload + 4U, fox_target, sizeof(fox_target));
    packet.payload[10] = 2U; packet.payload[15] = 99U;
    memcpy(packet.payload + 16U, fox_target, sizeof(fox_target));
    deliver_fox_packet(&packet);
    cerberus_wifi_fox_hunt_signal_t signal;
    assert(c5_spi_link_wifi_fox_hunt_signal(live_scan->session, &signal) == ESP_OK);
    assert(signal.sequence == valid_reports);

    memcpy(packet.payload + 10U, fox_target, sizeof(fox_target));
    deliver_fox_packet(&packet); ++valid_reports;
    assert(c5_spi_link_wifi_fox_hunt_signal(live_scan->session, &signal) == ESP_OK);
    assert(signal.sequence == valid_reports && signal.rssi == -58);
    assert(signal.channel == current_channel && signal.age_ms == 0U);
    packet.rx_ctrl.rx_state = 1U;
    deliver_fox_packet(&packet);
    packet.rx_ctrl.rx_state = 0U; packet.rx_ctrl.channel = current_channel == 1U ? 36U : 1U;
    deliver_fox_packet(&packet);
    assert(c5_spi_link_wifi_fox_hunt_signal(live_scan->session, &signal) == ESP_OK);
    assert(signal.sequence == valid_reports);
    fox_tick(live_scan, 0);
}
static void transmitter_signal_and_history(void)
{
    bridge_reset(); firmware = 83;
    wifi_fox_hunt_scan_t scan = {0}; live_scan = &scan; valid_reports = 0;
    fox_test_delay_hook = receive_selected_transmitter; stop_after = 8U;
    fox_begin(&scan, 36, false);
    assert(!scan.have_signal);
    dispatch(); fox_collect(&scan);
    assert(valid_reports && scan.history_count);
    unsigned readings = 0;
    for (unsigned i = 0; i < scan.history_count; ++i) {
        wifi_fox_hunt_entry_t point;
        assert(wifi_fox_hunt_scan_history(&scan, i, &point));
        if (point.kind == WIFI_FOX_SIGNAL) {
            ++readings;
            assert(point.rssi == -58 && point.channel == 36U && point.sequence);
        }
    }
    assert(readings);
    wifi_fox_hunt_view_t view;
    wifi_fox_hunt_scan_view(&scan, clock_ms, &view);
    assert(!view.current && view.state == WIFI_FOX_STOPPED);
    unsigned retained = scan.history_count;
    fox_test_delay_hook = NULL;
    fox_begin(&scan, 1, true);
    assert(!scan.have_signal && scan.history_count >= retained);
    dispatch(); fox_collect(&scan);
    run_ap_and_capture();
}

static uint32_t search_started;
static uint8_t recovered_channel;
static bool saw_search, saw_recovered;
static void search_and_reacquire(void)
{
    if (samples == 1U || clock_ms - search_started >= 2300U) {
        if (samples != 1U && !recovered_channel) recovered_channel = current_channel;
        wifi_promiscuous_pkt_t packet = {.rx_ctrl = {0U, 28U, current_channel, -67}};
        packet.payload[0] = 0x08U;
        memcpy(packet.payload + 10U, fox_target, sizeof(fox_target));
        deliver_fox_packet(&packet);
    }
    fox_tick(live_scan, 0);
    wifi_fox_hunt_view_t view;
    wifi_fox_hunt_scan_view(live_scan, clock_ms, &view);
    saw_search |= view.state == WIFI_FOX_SEARCHING;
    saw_recovered |= recovered_channel && view.current && view.observed_channel == recovered_channel;
}
static void search_reacquisition_through_bridge(void)
{
    bridge_reset(); firmware = 83;
    wifi_fox_hunt_scan_t scan = {0}; live_scan = &scan;
    recovered_channel = 0; saw_search = saw_recovered = false;
    fox_test_delay_hook = search_and_reacquire; stop_after = 70U;
    fox_begin(&scan, 1, false); search_started = clock_ms;
    dispatch(); fox_collect(&scan);
    assert(saw_search && saw_recovered && recovered_channel != 1U);
    assert(scan.have_signal && scan.signal.channel == recovered_channel);
    unsigned gaps = 0;
    for (unsigned i = 0; i < scan.history_count; ++i) {
        wifi_fox_hunt_entry_t point;
        assert(wifi_fox_hunt_scan_history(&scan, i, &point));
        gaps += point.kind == WIFI_FOX_GAP;
    }
    assert(gaps);
    run_ap_and_capture();
}

static void held_cleanup_and_recovery(void)
{
    bridge_reset(); firmware = 83;
    wifi_fox_hunt_scan_t scan = {0};
    fox_begin(&scan, 36, false); fail_disable = true;
    dispatch(); wifi_fox_hunt_scan_stop(&scan); fox_tick(&scan, 0);
    for (unsigned i = 0; i < 10; ++i) fox_tick(&scan, 100);
    assert(scan.phase != WIFI_FOX_OFF && scan.session);
    assert(cerberus_wifi_packet_monitor_active());
    assert(!wifi_fox_hunt_scan_start(&scan, fox_target, 1,
        CERBERUS_WIFI_FOX_HUNT_BAND_BOTH, true));
    fail_disable = false;
    for (unsigned i = 0; i < 30 && scan.phase != WIFI_FOX_OFF; ++i) {
        fox_tick(&scan, 100); dispatch();
    }
    assert(scan.phase == WIFI_FOX_OFF && !scan.session);
    assert(!cerberus_wifi_packet_monitor_active());
    run_ap_and_capture();
}

int main(void)
{
    compatibility_and_ownership(); lost_start_reply();
    transmitter_signal_and_history(); search_reacquisition_through_bridge(); held_cleanup_and_recovery();
    puts("Wi-Fi Fox Hunt bridge: old-peer refusal, exact owner/mode, lost Start, transmitter-only signal, search/reacquisition, stale history and cleanup recovery passed.");
    return 0;
}
