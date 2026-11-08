/* Reuse the SDK fixture, not a second implementation of the receiver. */
#define main monitor_regressions
#include "wifi_packet_monitor_worker_check.c"
#undef main

static uint64_t analyzer_begin(uint8_t channel)
{
    cerberus_wifi_channel_analyzer_status_t status;
    cerberus_wifi_channel_analyzer_info(&status);
    assert(!(status.flags & CERBERUS_WIFI_CHANNEL_ANALYZER_HELD));
    cerberus_wifi_channel_analyzer_start_t start = {status.session_id + 1U, channel};
    assert(cerberus_wifi_channel_analyzer_start(&start, &status) == ESP_OK);
    assert(status.session_id == start.session_id && status.channel == channel &&
        status.target_ms == 100U && status.flags == 3U);
    assert(!record_heap_bytes);
    return start.session_id;
}

static wifi_promiscuous_pkt_t beacon(uint8_t name)
{
    wifi_promiscuous_pkt_t packet = {.rx_ctrl = {0U, 45U, current_channel, -54}};
    packet.payload[0] = 0x80U;
    memset(packet.payload + 4U, 255, 6U);
    packet.payload[10] = packet.payload[16] = 2U;
    packet.payload[15] = packet.payload[21] = name;
    packet.payload[36] = 0U; packet.payload[37] = 3U;
    packet.payload[38] = name; packet.payload[39] = 0U; packet.payload[40] = 0xffU;
    return packet;
}

static void traffic(void)
{
    assert(current_filter.filter_mask == 7U);
    assert(current_ctrl_filter.filter_mask == WIFI_PROMIS_CTRL_FILTER_MASK_ALL);
    wifi_promiscuous_pkt_t packet = beacon('A');
    packet_monitor_receive(&packet, WIFI_PKT_MGMT);
    packet.rx_ctrl.sig_len = 14U; /* short ACK-like callback */
    packet.payload[0] = 0xd4U;
    packet_monitor_receive(&packet, WIFI_PKT_CTRL);
    packet.payload[0] = 0x08U; packet.rx_ctrl.sig_len = 28U;
    packet_monitor_receive(&packet, WIFI_PKT_DATA);
    packet_monitor_receive(&packet, 3U); /* MISC never counts */
    packet.rx_ctrl.rx_state = 1U;
    packet_monitor_receive(&packet, WIFI_PKT_DATA);
    packet.rx_ctrl.rx_state = 0U; packet.rx_ctrl.channel = current_channel == 1U ? 6U : 1U;
    packet_monitor_receive(&packet, WIFI_PKT_CTRL);
}

static void delay_first_window(void)
{ traffic(); if (samples == 1U) clock_ms += 50U; }
static void quiet_channel(void) {}
static void partial_window(void)
{ traffic(); clock_ms -= 50U; }
static void invalid_clock(void)
{ traffic(); clock_ms -= 100U; }
static void invalid_lease_clock(void)
{
    traffic();
    /* Still after the start of the sample, but before a prior status read. */
    packet_monitor_lease_clock_us = esp_timer_get_time() + 1;
    cerberus_wifi_channel_analyzer_status_t status;
    assert(cerberus_wifi_channel_analyzer_status(packet_monitor_session_id, &status) == ESP_OK);
}
static void saturation(void)
{
    channel_analyzer_frames = UINT32_MAX - 1U;
    traffic();
}

static cerberus_wifi_channel_analyzer_sample_t sample(uint64_t session, uint32_t sequence)
{
    cerberus_wifi_channel_analyzer_read_t request = {session, sequence};
    cerberus_wifi_channel_analyzer_sample_t out;
    assert(cerberus_wifi_channel_analyzer_sample(&request, &out) == ESP_OK);
    return out;
}

static void read_hint(uint64_t session, uint32_t projection,
    cerberus_wifi_channel_analyzer_hint_t *hint)
{
    uint8_t bytes[CERBERUS_WIFI_CHANNEL_ANALYZER_HINT_PARTS * 9U] = {0};
    for (uint8_t part = 0U; part < 5U; ++part) {
        cerberus_wifi_channel_analyzer_hint_request_t request = {session, projection, part};
        cerberus_wifi_channel_analyzer_hint_part_t reply;
        assert(cerberus_wifi_channel_analyzer_hint_read(&request, &reply) == ESP_OK);
        assert(reply.session_id == session && reply.projection == projection && reply.part == part);
        memcpy(bytes + part * 9U, reply.bytes, 9U);
    }
    memcpy(hint, bytes, sizeof(*hint));
    assert(bytes[42] == 0U && bytes[43] == 0U && bytes[44] == 0U);
}

static void test_hints(void)
{
    reset();
    uint64_t session = analyzer_begin(36U);
    analyzer_test_delay = traffic; dispatch();
    cerberus_wifi_channel_analyzer_hint_status_t peek;
    assert(cerberus_wifi_channel_analyzer_hint_peek(session, &peek) == ESP_OK);
    assert(peek.length == 42U && peek.flags == 1U && peek.projection == 1U && peek.age_ms == 0U);
    cerberus_wifi_channel_analyzer_hint_t original, pinned;
    read_hint(session, peek.projection, &original);
    assert(original.ssid_length == 3U && original.ssid[0] == 'A' && original.ssid[1] == 0U &&
        original.ssid[2] == 0xffU && original.channel == 36U && original.rssi == -54);
    /* New live callback must not mix an already pinned image. */
    packet_monitor_receiving = true;
    wifi_promiscuous_pkt_t packet = beacon('B');
    packet.rx_ctrl.channel = 36U;
    packet.rx_ctrl.sig_len = 74U; packet.payload[37] = 32U;
    memset(packet.payload + 38U, 'B', 32U);
    packet_monitor_receive(&packet, WIFI_PKT_MGMT);
    packet.payload[70] = 0U; packet.payload[71] = 0U; packet.rx_ctrl.sig_len = 76U;
    packet_monitor_receive(&packet, WIFI_PKT_MGMT); /* duplicate SSID cannot replace it */
    packet_monitor_receiving = false;
    read_hint(session, 1U, &pinned);
    assert(!memcmp(&original, &pinned, sizeof(original)));
    assert(cerberus_wifi_channel_analyzer_hint_peek(session + 1U, &peek) == ESP_ERR_NOT_FOUND);
    assert(peek.projection == 0U && peek.length == 0U && peek.flags == 0U && peek.age_ms == UINT32_MAX);
    read_hint(session, 1U, &pinned); /* foreign PEEK did not invalidate the image */
    clock_ms += 250U;
    assert(cerberus_wifi_channel_analyzer_hint_peek(session, &peek) == ESP_OK && peek.age_ms == 250U);
    read_hint(session, 2U, &pinned);
    assert(pinned.ssid_length == 32U && pinned.ssid[0] == 'B' && pinned.ssid[31] == 'B');
    cerberus_wifi_channel_analyzer_hint_request_t old = {session, 1U, 0U};
    cerberus_wifi_channel_analyzer_hint_part_t part;
    assert(cerberus_wifi_channel_analyzer_hint_read(&old, &part) == ESP_ERR_NOT_FOUND);
    old.projection = 2U; old.part = 5U;
    assert(cerberus_wifi_channel_analyzer_hint_read(&old, &part) == ESP_ERR_NOT_FOUND);

    /* The bounded decoder keeps exact hidden/binary names and rejects a
     * malformed trailing IE, duplicate SSID, non-beacon and invalid RSSI. */
    cerberus_wifi_channel_analyzer_hint_t decoded;
    packet = beacon('C');
    assert(cerberus_wifi_channel_analyzer_beacon(packet.payload, 41U, 36U, -50, &decoded));
    packet.payload[41] = 0U; packet.payload[42] = 0U;
    assert(!cerberus_wifi_channel_analyzer_beacon(packet.payload, 43U, 36U, -50, &decoded));
    packet.payload[41] = 7U; packet.payload[42] = 3U;
    assert(!cerberus_wifi_channel_analyzer_beacon(packet.payload, 43U, 36U, -50, &decoded));
    packet.payload[0] = 0x50U;
    assert(!cerberus_wifi_channel_analyzer_beacon(packet.payload, 41U, 36U, -50, &decoded));
    packet.payload[0] = 0x80U;
    assert(!cerberus_wifi_channel_analyzer_beacon(packet.payload, 41U, 36U, -128, &decoded));
    packet.payload[37] = 0U;
    assert(cerberus_wifi_channel_analyzer_beacon(packet.payload, 38U, 36U, -50, &decoded));
    assert(decoded.ssid_length == 0U && decoded.ssid[0] == 0U);
    channel_analyzer_projection_id = UINT32_MAX;
    assert(cerberus_wifi_channel_analyzer_hint_peek(session, &peek) == ESP_ERR_INVALID_STATE);
    assert(!channel_analyzer_projection_valid && !peek.projection && peek.age_ms == UINT32_MAX);
}

static void test_cross_mode(void)
{
    reset();
    uint64_t session = analyzer_begin(36U);
    cerberus_wifi_packet_monitor_session_status_t monitor;
    cerberus_wifi_packet_monitor_session_info(&monitor);
    assert(monitor.session_id == session && monitor.flags == CERBERUS_WIFI_PACKET_MONITOR_HELD &&
        monitor.error_code == ESP_ERR_INVALID_STATE && !monitor.channel && !monitor.latest_sequence);
    cerberus_wifi_packet_monitor_session_start_t start = {session, 36U};
    assert(cerberus_wifi_packet_monitor_session_start(&start, &monitor) == ESP_ERR_INVALID_ARG);
    assert(cerberus_wifi_packet_monitor_session_status(session, &monitor) == ESP_ERR_NOT_FOUND);
    assert(cerberus_wifi_packet_monitor_session_stop(session, &monitor) == ESP_ERR_NOT_FOUND);
    assert(!atomic_load(&packet_monitor_cancelled));
    cerberus_wifi_packet_monitor_status_payload_t legacy;
    cerberus_wifi_packet_monitor_get_status(&legacy);
    assert(legacy.state == CERBERUS_WIFI_PACKET_MONITOR_ERROR && legacy.error_code == ESP_ERR_INVALID_STATE);
    analyzer_test_delay = traffic; dispatch();
    cerberus_wifi_packet_monitor_session_read_t read = {session, 1U};
    cerberus_wifi_packet_monitor_session_sample_t out;
    assert(cerberus_wifi_packet_monitor_session_sample(&read, &out) == ESP_ERR_NOT_FOUND);
    start.session_id = session + 1U;
    assert(cerberus_wifi_packet_monitor_session_start(&start, &monitor) == ESP_OK);
    cerberus_wifi_channel_analyzer_status_t analyzer;
    cerberus_wifi_channel_analyzer_info(&analyzer);
    assert(analyzer.session_id == start.session_id && analyzer.flags == CERBERUS_WIFI_CHANNEL_ANALYZER_HELD &&
        analyzer.error_code == ESP_ERR_INVALID_STATE && !analyzer.target_ms && !analyzer.channel);
    assert(cerberus_wifi_channel_analyzer_stop(start.session_id, &analyzer) == ESP_ERR_NOT_FOUND);
    assert(cerberus_wifi_channel_analyzer_status(start.session_id, &analyzer) == ESP_ERR_NOT_FOUND);
    assert(!atomic_load(&packet_monitor_cancelled));
    cerberus_wifi_channel_analyzer_start_t attempt = {start.session_id, 36U};
    assert(cerberus_wifi_channel_analyzer_start(&attempt, &analyzer) == ESP_ERR_INVALID_ARG);
    dispatch();
    cerberus_wifi_channel_analyzer_info(&analyzer);
    assert(!analyzer.flags && !analyzer.channel && !analyzer.error_code);
}

static void test_cleanup(void)
{
    for (unsigned failure = 0U; failure < 5U; ++failure) {
        reset(); uint64_t session = analyzer_begin(36U);
        analyzer_test_delay = traffic;
        fail_disable = failure == 0U; fail_restore_band = failure == 1U;
        fail_restore_channel = failure == 2U; fail_filter_restore = failure == 3U;
        fail_ctrl_filter_restore = failure == 4U;
        dispatch();
        cerberus_wifi_channel_analyzer_status_t status;
        assert(cerberus_wifi_channel_analyzer_status(session, &status) == ESP_OK);
        assert(status.state == CERBERUS_WIFI_PACKET_MONITOR_ERROR && (status.flags & 2U));
        assert(atomic_load(&packet_monitor_radio_reserved) && !packet_monitor_receiving);
        cerberus_wifi_channel_analyzer_start_t next = {session + 1U, 1U};
        assert(cerberus_wifi_channel_analyzer_start(&next, &status) == ESP_ERR_INVALID_STATE);
        fail_disable = fail_restore_band = fail_restore_channel = fail_filter_restore = false;
        fail_ctrl_filter_restore = false;
        assert(cerberus_wifi_channel_analyzer_stop(session, &status) == ESP_OK);
        dispatch();
        assert(cerberus_wifi_channel_analyzer_status(session, &status) == ESP_OK);
        assert(status.state == CERBERUS_WIFI_PACKET_MONITOR_IDLE && !(status.flags & 2U));
        assert(current_band == 9 && current_channel == 6U && current_filter.filter_mask == 0x20U);
        assert(current_ctrl_filter.filter_mask == 0x800000U);
        run_ap_and_capture();
    }
}

int main(void)
{
    assert(monitor_regressions() == 0);
    assert(sizeof(packet_receive_history) < sizeof(cerberus_wifi_packet_monitor_ring_t) +
        sizeof(cerberus_wifi_channel_analyzer_ring_t));
    reset(); uint64_t session = analyzer_begin(36U);
    analyzer_test_delay = traffic; dispatch();
    cerberus_wifi_channel_analyzer_sample_t value = sample(session, 1U);
    assert(value.frames == 3U && value.window_us == 100000U && value.channel == 36U && !value.flags);
    assert(sample(session, 2U).frames == 3U && channel_analyzer_ring.latest_sequence == 2U);
    assert(!packet_monitor_receiving && !atomic_load(&packet_monitor_radio_reserved));
    /* The last delayed callback is canceled; no partial third window exists. */
    cerberus_wifi_channel_analyzer_read_t missing = {session, 3U};
    assert(cerberus_wifi_channel_analyzer_sample(&missing, &value) == ESP_ERR_NOT_FOUND);
    reset(); session = analyzer_begin(36U); analyzer_test_delay = quiet_channel; dispatch();
    value = sample(session, 1U); assert(value.frames == 0U && value.window_us == 100000U);
    reset(); (void)analyzer_begin(36U); analyzer_test_delay = partial_window; stop_after = 1U;
    dispatch(); assert(packet_monitor_status.state == CERBERUS_WIFI_PACKET_MONITOR_IDLE);
    assert(!channel_analyzer_ring.latest_sequence && !atomic_load(&packet_monitor_radio_reserved));
    reset(); session = analyzer_begin(1U); analyzer_test_delay = delay_first_window;
    stop_after = 5U; dispatch();
    assert(sample(session, 1U).window_us == 150000U);
    assert(sample(session, 2U).window_us == 150000U && sample(session, 2U).frames == 6U);
    reset(); session = analyzer_begin(36U); analyzer_test_delay = invalid_clock;
    dispatch(); assert(packet_monitor_status.state == CERBERUS_WIFI_PACKET_MONITOR_ERROR);
    assert(!channel_analyzer_ring.latest_sequence && !packet_monitor_receiving);
    reset(); (void)analyzer_begin(36U); analyzer_test_delay = invalid_lease_clock;
    dispatch(); assert(packet_monitor_status.state == CERBERUS_WIFI_PACKET_MONITOR_ERROR);
    assert(!channel_analyzer_ring.latest_sequence && !atomic_load(&packet_monitor_radio_reserved));
    reset(); session = analyzer_begin(36U); analyzer_test_delay = saturation;
    dispatch(); value = sample(session, 1U);
    assert(value.frames == UINT32_MAX && value.flags == CERBERUS_WIFI_CHANNEL_ANALYZER_SATURATED);
    reset(); (void)analyzer_begin(36U); analyzer_test_delay = traffic;
    channel_analyzer_ring.latest_sequence = UINT32_MAX;
    dispatch(); assert(packet_monitor_status.state == CERBERUS_WIFI_PACKET_MONITOR_ERROR);
    assert(channel_analyzer_ring.latest_sequence == UINT32_MAX && !channel_analyzer_ring.stored_count);
    reset(); (void)analyzer_begin(36U); cancel_during_enable = true;
    dispatch(); assert(!samples && !channel_analyzer_ring.latest_sequence && !packet_monitor_receiving);
    reset(); session = analyzer_begin(36U); analyzer_test_delay = traffic; stop_after = 0U;
    dispatch(); assert(samples == 100U && channel_analyzer_ring.latest_sequence == 99U);
    assert(channel_analyzer_ring.stored_count == 64U && !atomic_load(&packet_monitor_radio_reserved));
    missing = (cerberus_wifi_channel_analyzer_read_t){session, 35U};
    assert(cerberus_wifi_channel_analyzer_sample(&missing, &value) == ESP_ERR_NOT_FOUND);
    assert(sample(session, 36U).frames == 3U && sample(session, 99U).frames == 3U);
    test_hints(); test_cross_mode(); test_cleanup();
    puts("C5 Channel Analyzer: actual worker, callbacks, timing, hints, ownership, leases and cleanup passed.");
    return 0;
}
