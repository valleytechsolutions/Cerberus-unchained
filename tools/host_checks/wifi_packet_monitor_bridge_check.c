/* Production C5 service and dispatcher, S3 wire adapter, controller and UI.
 * The fixture supplies only SDK scheduling, SPI transport and page destinations. */
#define main native_monitor_suite
#include "wifi_packet_monitor_worker_check.c"
#undef main
#include "c5_spi_link.h"
#include "wifi_packet_monitor_scan.h"
#include "wifi_packet_count_record.h"

static cerberus_spi_frame_t response_frame;
static uint8_t tx_buffer[64];
static size_t received_transfer_bytes;
static bool response_ready;
static void prepare_frame_response(cerberus_spi_command_t command, uint8_t token,
    const void *payload, size_t size)
{
    assert(cerberus_protocol_prepare_frame(&response_frame, command, token, payload, size));
    response_ready = true;
}
static void wire_dispatch(const cerberus_spi_frame_t *request)
{
    response_ready = false;
    memset(tx_buffer, 0, sizeof(tx_buffer));
    switch (request->command) {
#include "monitor_dispatch.h"
    default: assert(false);
    }
}

static int link_sentinel;
static void *link_mutex = &link_sentinel;
static bool link_locked, lock_failed, lose_start, gate_failed, lose_record_ack;
static uint8_t sequence;
static uint32_t peer_firmware_version, firmware = 76U;
static unsigned exchanges, starts, stops, reads, faults, wire_delay_ms;
static int take_link_mutex(unsigned timeout)
{
    assert(timeout == 20U && !link_locked);
    if (lock_failed) return 0;
    link_locked = true;
    return pdTRUE;
}
static void give_link_mutex(void) { assert(link_locked); link_locked = false; }
static esp_err_t get_status_v2_passive_locked(uint8_t token, uint32_t deadline,
    cerberus_c5_status_t *out)
{
    (void)token; (void)deadline;
    assert(link_locked);
    ++exchanges;
    memset(out, 0, sizeof(*out));
    out->firmware_version = firmware;
    return gate_failed ? ESP_ERR_TIMEOUT : ESP_OK;
}
static esp_err_t spi_request_response_passive(const void *data, void *discarded,
    void *output, size_t size, uint32_t deadline)
{
    (void)discarded;
    const cerberus_spi_frame_t *q = data;
    cerberus_spi_frame_t *r = output;
    assert(link_locked && (size == sizeof(*q) || size == 64U) && cerberus_protocol_frame_is_valid(q));
    assert((uint32_t)(deadline - clock_ms) <= 20U);
    ++exchanges;
    starts += q->command == CERBERUS_SPI_COMMAND_WIFI_PACKET_MONITOR_SESSION_START ||
        q->command == CERBERUS_MONITOR_RECORD_START;
    stops += q->command == CERBERUS_SPI_COMMAND_WIFI_PACKET_MONITOR_SESSION_STOP;
    reads += q->command == CERBERUS_SPI_COMMAND_WIFI_PACKET_MONITOR_SESSION_SAMPLE;
    received_transfer_bytes = size;
    wire_dispatch(q);
    if (size == 64U) {
        cerberus_packet_count_raw_frame_t raw;
        memcpy(&raw, tx_buffer, sizeof(raw));
        if (!cerberus_protocol_monitor_record_raw_valid(&raw)) return ESP_ERR_TIMEOUT;
        if (faults) { raw.crc16 ^= 1U; faults = 0U; }
        memcpy(output, &raw, sizeof(raw));
        clock_ms += wire_delay_ms;
        return ESP_OK;
    }
    if (!response_ready) return ESP_ERR_TIMEOUT;
    *r = response_frame;
    clock_ms += wire_delay_ms;
    if (lose_record_ack && q->command == CERBERUS_MONITOR_RECORD_RAW_ACK) {
        lose_record_ack = false;
        return ESP_ERR_TIMEOUT;
    }
    if (lose_start && (q->command == CERBERUS_SPI_COMMAND_WIFI_PACKET_MONITOR_SESSION_START ||
        q->command == CERBERUS_MONITOR_RECORD_START)) {
        lose_start = false;
        return ESP_ERR_TIMEOUT;
    }
    if (faults) {
        unsigned fault = faults;
        faults = 0;
        if (fault == 1U) { r->payload[0] ^= 1U; return ESP_OK; }
        uint8_t bytes[22];
        memcpy(bytes, r->payload, sizeof(bytes));
        if (fault == 2U) ++r->command;
        if (fault == 3U) ++r->sequence;
        if (fault == 4U) --r->payload_length;
        if (fault == 5U) bytes[0] ^= 1U;
        if (q->command == CERBERUS_SPI_COMMAND_WIFI_PACKET_MONITOR_SESSION_SAMPLE) {
            cerberus_wifi_packet_monitor_session_sample_t sample;
            memcpy(&sample, bytes, sizeof(sample));
            if (fault == 6U) sample.sample.sequence++;
            if (fault == 7U) sample.sample.generation = 0U;
            if (fault == 8U) sample.sample.channel = 37U;
            memcpy(bytes, &sample, sizeof(sample));
        } else {
            cerberus_wifi_packet_monitor_session_status_t status;
            memcpy(&status, bytes, sizeof(status));
            if (fault == 6U) status.flags |= 0x80U;
            if (fault == 7U) status.flags &= ~CERBERUS_WIFI_PACKET_MONITOR_HELD;
            if (fault == 8U) status.channel = 37U;
            memcpy(bytes, &status, sizeof(status));
        }
        assert(cerberus_protocol_prepare_frame(r, (cerberus_spi_command_t)r->command,
            r->sequence, bytes, r->payload_length));
    }
    return ESP_OK;
}
#include "monitor_frame.h"
#include "wifi_packet_monitor_link.h"
#include "c5_wifi_packet_count_link_impl.h"

enum { UI_WIFI_MODE_PACKET_MONITOR, UI_WIFI_MODE_CHANNELS_24, UI_WIFI_MODE_CHANNELS_5,
    UI_WIFI_MODE_TARGET, UI_WIFI_MODE_LIST, UI_WIFI_MODE_MENU };
#include "monitor_ui_state.h"
static unsigned wifi_mode, survey_opens, list_opens;
static esp_err_t wifi_scan_error;
static void wifi_channel_survey_begin_ui(void) { ++survey_opens; }
static esp_err_t load_wifi_results(void) { ++list_opens; return ESP_OK; }
/* SD scheduling is controlled independently of the real C5/S3 transport. */
static wifi_packet_count_record_status_t record_stub;
static bool record_allow_drain, record_allow_finish, record_sd_fail;
esp_err_t wifi_packet_count_record_start_monitor(const wifi_packet_count_transport_t *t,uint64_t *job)
{
    assert(t&&t->exchange&&!record_stub.job);*job=record_stub.job=1;
    record_stub.busy=record_stub.ready=!record_sd_fail;record_stub.error=record_sd_fail?ESP_FAIL:ESP_OK;
    return ESP_OK;
}
bool wifi_packet_count_record_status(uint64_t job,wifi_packet_count_record_status_t *out)
{ assert(job==record_stub.job);*out=record_stub;return true; }
esp_err_t wifi_packet_count_record_bind(uint64_t job,unsigned source,uint64_t session)
{ assert(job==record_stub.job&&source==1&&record_stub.ready);record_stub.source[1].session=session;record_stub.source[1].bound=true;return ESP_OK; }
bool wifi_packet_count_record_drained(uint64_t job,unsigned source,uint64_t session)
{
    assert(job==record_stub.job&&source==1&&session==record_stub.source[1].session);
    assert(!cerberus_wifi_packet_monitor_active());record_stub.source[1].draining=true;
    if(record_allow_drain){record_stub.source[1].bound=false;record_stub.source[1].drained=true;}
    return record_allow_drain;
}
esp_err_t wifi_packet_count_record_finish(uint64_t job)
{
    assert(job==record_stub.job&&!record_stub.source[1].bound);
    record_stub.finishing=true;if(record_allow_finish)record_stub.busy=false;return ESP_OK;
}
esp_err_t wifi_packet_count_record_release(uint64_t job)
{ assert(job==record_stub.job&&!record_stub.busy);memset(&record_stub,0,sizeof(record_stub));return ESP_OK; }
#include "wifi_packet_monitor_ui.h"

#define UI_WIFI_CONTROLS_Y 360U
#define ESP_ERROR_CHECK(value) assert((value) == ESP_OK)
enum { UI_PAGE_WIFI, UI_PAGE_MAIN };
static unsigned current_page;
static bool screen_asleep;
static esp_err_t draw_detail_page(void) { return ESP_OK; }
#include "monitor_ui_routes.h"

static void bridge_reset(void)
{
    reset();
    memset(&packet_monitor, 0, sizeof(packet_monitor));
    packet_monitor_action = PACKET_MONITOR_NONE;
    memset(&record_stub,0,sizeof(record_stub));
    memset(&packet_monitor_record_status,0,sizeof(packet_monitor_record_status));
    packet_monitor_record_enabled=packet_monitor_record_preparing=false;packet_monitor_record_job=0;
    record_allow_drain=record_allow_finish=record_sd_fail=false;
    packet_monitor_channel = 36U;
    packet_monitor_from_target = false;
    link_mutex = &link_sentinel;
    link_locked = lock_failed = lose_start = gate_failed = false;
    peer_firmware_version = 0U;
    firmware = 76U;
    exchanges = starts = stops = reads = faults = wire_delay_ms = 0U;
    survey_opens = list_opens = 0U;
    wifi_mode = UI_WIFI_MODE_PACKET_MONITOR;
    current_page = UI_PAGE_WIFI;
    screen_asleep = false;
}
static void step(unsigned elapsed, bool visible)
{
    clock_ms += elapsed;
    unsigned before = exchanges;
    screen_asleep = !visible;
    actual_tick(clock_ms);
    /* One operation per tick; INFO/START may also refresh capability within
     * that operation's shared 20 ms deadline. */
    assert(exchanges - before <= 2U && !link_locked);
}
static void begin(void)
{
    packet_monitor_start_ui();
    step(0U, true);
    assert(packet_monitor.phase == WIFI_MONITOR_START && starts == 0U);
    step(20U, true);
    assert(packet_monitor.phase == WIFI_MONITOR_WATCH && starts == 1U);
    assert(cerberus_wifi_packet_monitor_active());
}
static void close_monitor(void)
{
    packet_monitor_request_action(PACKET_MONITOR_BACK_MENU);
    step(0U, true);
    dispatch();
    step(250U, true);
    assert(packet_monitor.phase == WIFI_MONITOR_OFF && !packet_monitor.session);
    assert(wifi_mode == UI_WIFI_MODE_MENU && !cerberus_wifi_packet_monitor_active());
}
static void malformed_frames(void)
{
    bridge_reset(); begin();
    uint64_t session = packet_monitor.session;
    cerberus_wifi_packet_monitor_session_status_t status;
    for (faults = 1U; faults <= 8U;) {
        unsigned fault = faults;
        assert(c5_spi_link_wifi_packet_monitor_session_status(session, &status) == ESP_ERR_INVALID_RESPONSE);
        assert(!memcmp(&status, &(cerberus_wifi_packet_monitor_session_status_t){0}, sizeof(status)));
        faults = fault + 1U;
    }
    faults = 0;
    lock_failed = true;
    unsigned before = exchanges;
    assert(c5_spi_link_wifi_packet_monitor_session_status(session, &status) == ESP_ERR_TIMEOUT);
    assert(exchanges == before);
    lock_failed = false; gate_failed = true; peer_firmware_version = 0;
    assert(c5_spi_link_wifi_packet_monitor_session_status(session, &status) == ESP_ERR_TIMEOUT);
    assert(peer_firmware_version == 0U);
    gate_failed = false;
    assert(c5_spi_link_wifi_packet_monitor_session_status(session, &status) == ESP_OK);
    wire_delay_ms = 20U;
    assert(c5_spi_link_wifi_packet_monitor_session_status(session, &status) == ESP_ERR_TIMEOUT);
    assert(!status.session_id);
    wire_delay_ms = 0U;
    dispatch();
    assert(c5_spi_link_wifi_packet_monitor_session_status(session, &status) == ESP_OK);
    assert(status.stored_count);
    cerberus_wifi_packet_monitor_session_sample_t sample;
    for (faults = 1U; faults <= 8U;) {
        unsigned fault = faults;
        assert(c5_spi_link_wifi_packet_monitor_session_sample(session, status.latest_sequence, &sample) == ESP_ERR_INVALID_RESPONSE);
        assert(!memcmp(&sample, &(cerberus_wifi_packet_monitor_session_sample_t){0}, sizeof(sample)));
        faults = fault + 1U;
    }
    faults = 0;
    close_monitor();
}
static void navigation_and_cleanup(void)
{
    bridge_reset(); begin();
    fail_disable = true;
    dispatch();
    step(250U, true);
    assert(packet_monitor.error == ESP_FAIL && packet_monitor.session);
    packet_monitor_from_target = true;
    actual_back();
    unsigned disabled = disable_calls;
    step(0U, true);
    assert(disable_calls == disabled && wifi_mode == UI_WIFI_MODE_PACKET_MONITOR);
    dispatch();
    step(250U, true);
    assert(packet_monitor.phase == WIFI_MONITOR_STOP && packet_monitor.session);
    assert(wifi_mode == UI_WIFI_MODE_PACKET_MONITOR && cerberus_wifi_packet_monitor_active());
    fail_disable = false;
    dispatch();
    step(250U, true);
    assert(wifi_mode == UI_WIFI_MODE_TARGET && !packet_monitor.session);
    run_ap_and_capture();

    bridge_reset(); begin();
    actual_tap(100U, UI_WIFI_CONTROLS_Y);
    actual_tap(100U, UI_WIFI_CONTROLS_Y); /* Do not queue several retunes. */
    step(0U, true);
    assert(packet_monitor_channel == 36U);
    dispatch(); step(250U, true);
    assert(packet_monitor_channel == 40U && packet_monitor.phase == WIFI_MONITOR_INFO);
    step(20U, true); step(20U, true);
    assert(starts == 2U && packet_monitor.view.channel == 40U);
    actual_tap(170U, 340U);
    step(0U, true); dispatch(); step(250U, true);
    assert(survey_opens == 1U && wifi_mode == UI_WIFI_MODE_CHANNELS_5);

    bridge_reset(); begin();
    actual_tap(20U, UI_WIFI_CONTROLS_Y);
    actual_tap(250U, UI_WIFI_CONTROLS_Y);
    step(0U, true); dispatch(); step(250U, true);
    assert(list_opens == 1U && wifi_mode == UI_WIFI_MODE_LIST && starts == 1U);

    bridge_reset(); begin();
    packet_monitor_request_action(PACKET_MONITOR_SURVEY);
    step(0U, false); dispatch(); step(250U, false);
    assert(!packet_monitor.session && survey_opens == 0U && starts == 1U);
    step(1000U, true); /* Waking never restarts a released monitor. */
    assert(starts == 1U);

    bridge_reset(); begin();
    fail_disable = true; dispatch();
    step(0U, false); /* A hidden page still owns a failed release. */
    packet_monitor_channel = 6U; packet_monitor_start_ui();
    assert(packet_monitor_channel == 36U && packet_monitor_next_channel == 6U);
    step(250U, true); dispatch();
    assert(starts == 1U && packet_monitor_channel == 36U);
    fail_disable = false;
    step(250U, true); dispatch(); step(250U, true);
    assert(packet_monitor.phase == WIFI_MONITOR_INFO && packet_monitor_channel == 6U);
    step(20U, true); step(20U, true);
    assert(starts == 2U && packet_monitor.view.channel == 6U);
    close_monitor();
}
static void ownership_and_loss(void)
{
    bridge_reset(); firmware = 75U;
    packet_monitor_start_ui(); step(0U, true);
    assert(packet_monitor.error == ESP_ERR_NOT_SUPPORTED && !starts && !packet_monitor.session);
    bridge_reset(); packet_monitor_start_ui();
    packet_monitor_request_action(PACKET_MONITOR_BACK_AP); step(0U, true);
    assert(!exchanges && wifi_mode == UI_WIFI_MODE_TARGET);
    bridge_reset(); packet_monitor_start_ui(); step(0U, true);
    packet_monitor_request_action(PACKET_MONITOR_BACK_MENU); step(0U, true);
    assert(!starts && !packet_monitor.session);

    bridge_reset(); lose_start = true; begin();
    uint64_t claim = packet_monitor.session;
    assert(packet_monitor.error == ESP_ERR_TIMEOUT);
    step(700U, true);
    assert(packet_monitor.session == claim && packet_monitor.error == ESP_OK && starts == 1U);
    close_monitor();
    bridge_reset(); lose_start = true; begin();
    close_monitor(); /* Stop can resolve an accepted Start with a lost reply. */

    bridge_reset(); begin();
    claim = packet_monitor.session;
    dispatch();
    cerberus_wifi_packet_monitor_session_status_t reply;
    cerberus_wifi_packet_monitor_session_start_t next = {claim + 1U, 36U};
    assert(cerberus_wifi_packet_monitor_session_start(&next, &reply) == ESP_OK);
    packet_monitor_request_action(PACKET_MONITOR_BACK_MENU);
    step(0U, true);
    assert(!packet_monitor.session && !atomic_load(&packet_monitor_cancelled));
    assert(cerberus_wifi_packet_monitor_active() && packet_monitor_session_id == next.session_id);
    dispatch();

    bridge_reset(); begin();
    claim = packet_monitor.session;
    dispatch();
    next = (cerberus_wifi_packet_monitor_session_start_t){claim + 1U, 36U};
    assert(cerberus_wifi_packet_monitor_session_start(&next, &reply) == ESP_OK);
    step(250U, true); /* A normal status poll discovers the retired session. */
    assert(packet_monitor.phase == WIFI_MONITOR_OFF && packet_monitor.view.state == CERBERUS_WIFI_PACKET_MONITOR_ERROR);
    assert(packet_monitor.error == ESP_ERR_NOT_FOUND && !atomic_load(&packet_monitor_cancelled));
    actual_tap(170U, UI_WIFI_CONTROLS_Y); step(0U, true);
    assert(wifi_mode == UI_WIFI_MODE_PACKET_MONITOR && packet_monitor.phase == WIFI_MONITOR_INFO);
    step(20U, true); /* Wait for the foreign receiver instead of adopting it. */
    assert(packet_monitor.phase == WIFI_MONITOR_INFO && !packet_monitor.session && starts == 1U);
    dispatch(); step(700U, true); step(20U, true);
    assert(packet_monitor.session == claim + 2U && starts == 2U);
    close_monitor();

    bridge_reset();
    atomic_store(&probe_sniff_radio_reserved, true);
    packet_monitor_start_ui(); step(0U, true); step(20U, true);
    assert(packet_monitor.phase == WIFI_MONITOR_INFO && !packet_monitor.session);
    atomic_store(&probe_sniff_radio_reserved, false);
    step(700U, true); step(20U, true);
    assert(packet_monitor.phase == WIFI_MONITOR_WATCH && starts == 2U);
    close_monitor();

    bridge_reset(); begin();
    clock_ms -= 100U; step(0U, true);
    assert(packet_monitor.phase == WIFI_MONITOR_STOP);
    dispatch(); step(250U, true);
    assert(!packet_monitor.session && !cerberus_wifi_packet_monitor_active());
}
static void samples_and_dispatch(void)
{
    bridge_reset(); begin(); dispatch();
    step(250U, true); step(20U, true); step(20U, true);
    assert(packet_monitor.view.generation && packet_monitor.view.stored_count == 2U);
    assert(packet_monitor.samples[1].beacons == 1U && packet_monitor.samples[2].beacons == 1U);
    assert(reads == 2U);
    uint64_t old = packet_monitor.session;
    actual_tap(170U, UI_WIFI_CONTROLS_Y); /* Stopped graph offers RESTART. */
    step(0U, true); step(20U, true); step(20U, true);
    assert(starts == 2U && packet_monitor.session == old + 1U);
    close_monitor();
    const unsigned sizes[] = {0U, 9U, 8U, 8U, 10U};
    for (unsigned command = 172U; command <= 176U; ++command) {
        for (unsigned size = 0U; size <= 22U; ++size) {
            if (size == sizes[command - 172U]) continue;
            cerberus_spi_frame_t q;
            uint8_t payload[22] = {0};
            assert(cerberus_protocol_prepare_frame(&q, (cerberus_spi_command_t)command, 9U, payload, size));
            wire_dispatch(&q);
            assert(!response_ready);
        }
    }
}
static void repeated_sessions(void)
{
    bridge_reset();
    uint64_t first = 0U, previous = 0U;
    for (unsigned i = 0U; i < 300U; ++i) {
        wifi_mode = UI_WIFI_MODE_PACKET_MONITOR;
        packet_monitor_start_ui(); step(20U, true); step(20U, true);
        uint64_t session = packet_monitor.session;
        assert(session && (!previous || session == previous + 1U));
        cerberus_wifi_packet_monitor_session_status_t reply;
        if (first) {
            assert(c5_spi_link_wifi_packet_monitor_session_stop(first, &reply) == ESP_OK);
            assert(!reply.flags && reply.error_code == ESP_ERR_NOT_FOUND);
            assert(!atomic_load(&packet_monitor_cancelled));
        } else first = session;
        packet_monitor_request_action(PACKET_MONITOR_BACK_MENU);
        lock_failed = true; step(0U, true);
        assert(packet_monitor.session == session && wifi_mode == UI_WIFI_MODE_PACKET_MONITOR);
        lock_failed = false; step(700U, true); dispatch(); step(250U, true);
        assert(!packet_monitor.session && wifi_mode == UI_WIFI_MODE_MENU);
        unsigned sent = starts;
        assert(c5_spi_link_wifi_packet_monitor_session_start(session, 36U, &reply) == ESP_OK);
        assert(reply.state == CERBERUS_WIFI_PACKET_MONITOR_IDLE && !(reply.flags & CERBERUS_WIFI_PACKET_MONITOR_HELD));
        assert(starts == sent + 1U && !cerberus_wifi_packet_monitor_active());
        previous = session;
    }
    assert(packet_monitor_status.generation == 45U); /* Generation wrapped; IDs did not. */
    assert(!enable_calls && !cerberus_wifi_packet_monitor_active());
}
static void record_protocol(void)
{
    reset(); firmware = 76U; wire_delay_ms = 0U;
    cerberus_wifi_packet_monitor_session_status_t status;
    assert(c5_spi_link_wifi_packet_monitor_session_info(&status) == ESP_OK);
    uint64_t session = status.session_id + 1U;
    assert(c5_spi_link_wifi_packet_monitor_record_start(session, 36U, &status) == ESP_ERR_NOT_SUPPORTED);
    assert(!record_heap_bytes && !cerberus_wifi_packet_monitor_active());
    firmware = 77U; record_heap_free = 1024U;
    assert(c5_spi_link_wifi_packet_monitor_record_start(session, 36U, &status) == ESP_OK);
    assert(status.error_code == ESP_ERR_NO_MEM && !status.flags && !record_heap_bytes);
    record_heap_free = 65536U;
    assert(c5_spi_link_wifi_packet_monitor_session_info(&status) == ESP_OK);
    session = status.session_id + 1U;
    record_heap_fail = true;
    assert(c5_spi_link_wifi_packet_monitor_record_start(session, 36U, &status) == ESP_OK);
    assert(status.error_code == ESP_ERR_NO_MEM && !record_heap_bytes);
    record_heap_fail = false;
    assert(c5_spi_link_wifi_packet_monitor_session_info(&status) == ESP_OK);
    session = status.session_id + 1U;
    assert(c5_spi_link_wifi_packet_monitor_record_start(session, 36U, &status) == ESP_OK);
    assert(status.error_code == ESP_OK && (status.flags & CERBERUS_WIFI_PACKET_MONITOR_RECORDING));
    assert(record_heap_bytes == sizeof(packet_monitor_record_context_t));
    assert(c5_spi_link_wifi_packet_monitor_session_start(session, 36U, &status) == ESP_OK);
    assert(status.error_code == ESP_ERR_INVALID_ARG); /* Same ID cannot change graph/record mode. */
    cerberus_packet_count_request_t request = {session};
    cerberus_packet_count_control_t control;
    assert(c5_spi_link_wifi_packet_monitor_record_exchange(CERBERUS_MONITOR_RECORD_RELEASE,
        &request, sizeof(request), &control, sizeof(control)) == ESP_OK);
    assert(control.error_code == ESP_ERR_INVALID_STATE && record_heap_bytes);
    record_test_data = true; stop_after = 2U;
    dispatch();
    assert(!cerberus_wifi_packet_monitor_active() && !promiscuous);
    assert(current_filter.filter_mask == 0x20U && record_heap_bytes);
    assert(c5_spi_link_wifi_packet_monitor_session_status(session, &status) == ESP_OK);
    assert((status.flags & CERBERUS_WIFI_PACKET_MONITOR_RECORDING) && !(status.flags & CERBERUS_WIFI_PACKET_MONITOR_HELD));
    for (uint32_t sequence = 1U; sequence <= 2U; ++sequence) {
        cerberus_packet_count_peek_t peek = {session, CERBERUS_PACKET_COUNT_READ_RAW_META, 0U};
        assert(c5_spi_link_wifi_packet_monitor_record_exchange(CERBERUS_MONITOR_RECORD_PEEK,
            &peek, sizeof(peek), &control, sizeof(control)) == ESP_OK);
        assert(control.sequence > CERBERUS_PACKET_COUNT_PIN_BASE && control.error_code == ESP_OK);
        uint32_t pin = control.sequence;
        uint8_t meta[32];
        for (unsigned part = 0U; part < 4U; ++part) {
            cerberus_packet_count_read_t read = {session, pin, (uint16_t)part};
            cerberus_packet_count_chunk_t chunk;
            assert(c5_spi_link_wifi_packet_monitor_record_exchange(CERBERUS_MONITOR_RECORD_READ,
                &read, sizeof(read), &chunk, sizeof(chunk)) == ESP_OK);
            assert(chunk.pin == pin);
            memcpy(meta + part * 8U, chunk.data, 8U);
        }
        assert(cerberus_packet_count_get(meta, 4U) == sequence && meta[20] == 36U);
        assert(cerberus_packet_count_get(meta + 4U, 2U) == 24U);
        cerberus_packet_count_read_t read = {session, sequence, 0U};
        cerberus_packet_count_raw_chunk_t raw;
        faults = 1U;
        assert(c5_spi_link_wifi_packet_monitor_record_exchange(CERBERUS_MONITOR_RECORD_RAW_READ,
            &read, sizeof(read), &raw, sizeof(raw)) == ESP_ERR_INVALID_RESPONSE);
        assert(c5_spi_link_wifi_packet_monitor_record_exchange(CERBERUS_MONITOR_RECORD_RAW_READ,
            &read, sizeof(read), &raw, sizeof(raw)) == ESP_OK);
        assert(raw.pin == sequence && raw.data[0] == (sequence == 1U ? 0x80U : 0x08U));
        assert(cerberus_protocol_crc32(raw.data, 24U) == cerberus_packet_count_get(meta + 16U, 4U));
        for (unsigned i = 24U; i < sizeof(raw.data); ++i) assert(!raw.data[i]);
        cerberus_packet_count_ack_t ack = {session + 1U, sequence};
        assert(c5_spi_link_wifi_packet_monitor_record_exchange(CERBERUS_MONITOR_RECORD_RAW_ACK,
            &ack, sizeof(ack), &control, sizeof(control)) == ESP_OK);
        assert(control.error_code == ESP_ERR_NOT_FOUND && !control.flags && !control.sequence);
        ack.session_id = session; lose_record_ack = true;
        assert(c5_spi_link_wifi_packet_monitor_record_exchange(CERBERUS_MONITOR_RECORD_RAW_ACK,
            &ack, sizeof(ack), &control, sizeof(control)) == ESP_ERR_TIMEOUT);
        assert(c5_spi_link_wifi_packet_monitor_record_exchange(CERBERUS_MONITOR_RECORD_RAW_ACK,
            &ack, sizeof(ack), &control, sizeof(control)) == ESP_OK);
        assert(control.sequence == sequence);
    }
    cerberus_packet_count_peek_t peek = {session, CERBERUS_PACKET_COUNT_READ_SUMMARY, 0U};
    assert(c5_spi_link_wifi_packet_monitor_record_exchange(CERBERUS_MONITOR_RECORD_PEEK,
        &peek, sizeof(peek), &control, sizeof(control)) == ESP_OK);
    uint32_t pin = control.sequence;
    uint8_t summary[64];
    for (unsigned part = 0U; part < 8U; ++part) {
        cerberus_packet_count_read_t read = {session, pin, (uint16_t)part};
        cerberus_packet_count_chunk_t chunk;
        assert(c5_spi_link_wifi_packet_monitor_record_exchange(CERBERUS_MONITOR_RECORD_READ,
            &read, sizeof(read), &chunk, sizeof(chunk)) == ESP_OK);
        memcpy(summary + part * 8U, chunk.data, 8U);
    }
    assert(summary[0] == 1U && summary[1] == 0U && summary[2] == 0U);
    assert(cerberus_packet_count_get(summary + 4U, 4U) == 2U);
    assert(cerberus_packet_count_get(summary + 8U, 4U) == 2U);
    assert(cerberus_packet_count_get(summary + 12U, 4U) == 2U);
    assert(c5_spi_link_wifi_packet_monitor_record_exchange(CERBERUS_MONITOR_RECORD_RELEASE,
        &request, sizeof(request), &control, sizeof(control)) == ESP_OK);
    assert(control.flags & CERBERUS_PACKET_COUNT_BOUND); /* Worker has not freed it yet. */
    dispatch();
    assert(!record_heap_bytes && !packet_monitor_record_context);
    assert(c5_spi_link_wifi_packet_monitor_record_exchange(CERBERUS_MONITOR_RECORD_RELEASE,
        &request, sizeof(request), &control, sizeof(control)) == ESP_OK);
    assert(!control.flags && control.error_code == ESP_OK);
    /* A forgotten recording expires after radio cleanup; active reads cannot
     * extend RF ownership and stale receipts cannot revive its queue. */
    assert(c5_spi_link_wifi_packet_monitor_session_info(&status) == ESP_OK);
    session = status.session_id + 1U;
    assert(c5_spi_link_wifi_packet_monitor_record_start(session, 1U, &status) == ESP_OK);
    assert(c5_spi_link_wifi_packet_monitor_session_stop(session, &status) == ESP_OK);
    dispatch();
    clock_ms += CERBERUS_MONITOR_RECORD_RETENTION_MS;
    xTaskNotifyGive(scanner_task_handle); dispatch();
    assert(!record_heap_bytes && !packet_monitor_record_context);
    request.session_id = session;
    assert(c5_spi_link_wifi_packet_monitor_record_exchange(CERBERUS_MONITOR_RECORD_STATUS,
        &request, sizeof(request), &control, sizeof(control)) == ESP_OK);
    assert(control.error_code == ESP_ERR_NOT_FOUND && !control.flags);
    reset();
}

static void record_navigation(void)
{
    bridge_reset();firmware=77;packet_monitor_record_enabled=true;
    packet_monitor_start_ui();assert(!starts&&packet_monitor_record_preparing);
    step(0,true);step(20,true);
    assert(packet_monitor.phase==WIFI_MONITOR_WATCH&&packet_monitor.record_bound);
    assert(record_stub.source[1].session==packet_monitor.session&&record_heap_bytes);
    dispatch();step(250,true);
    assert(packet_monitor.phase==WIFI_MONITOR_DRAIN);
    actual_back();step(0,true);
    assert(wifi_mode==UI_WIFI_MODE_PACKET_MONITOR&&record_stub.source[1].draining);
    assert(!record_stub.finishing&&packet_monitor_record_context);
    record_allow_drain=true;step(20,true);
    assert(packet_monitor.phase==WIFI_MONITOR_RELEASE);
    step(20,true);assert(packet_monitor_record_context&&record_heap_bytes);
    dispatch();step(250,true);
    assert(!packet_monitor_record_context&&!record_heap_bytes&&record_stub.finishing);
    assert(wifi_mode==UI_WIFI_MODE_PACKET_MONITOR&&packet_monitor_record_job);
    record_allow_finish=true;step(20,true);step(20,true);
    assert(!packet_monitor_record_job&&wifi_mode==UI_WIFI_MODE_MENU);
    run_ap_and_capture();
    bridge_reset();firmware=77;packet_monitor_record_enabled=record_sd_fail=true;
    packet_monitor_start_ui();step(0,true);
    assert(!starts&&!packet_monitor_record_job&&packet_monitor_record_status.error==ESP_FAIL);
    actual_back();step(0,true);assert(wifi_mode==UI_WIFI_MODE_MENU);
    bridge_reset();firmware=77;packet_monitor_record_enabled=true;
    packet_monitor_start_ui();actual_back();record_allow_finish=true;
    step(0,true);step(20,true);
    assert(!starts&&!packet_monitor_record_job&&wifi_mode==UI_WIFI_MODE_MENU);
    for(unsigned cycle=0;cycle<20;++cycle){
        bridge_reset();firmware=77;packet_monitor_record_enabled=true;lose_start=true;
        packet_monitor_start_ui();step(0,true);step(20,true);
        uint64_t claimed=packet_monitor.session;
        assert(packet_monitor.error==ESP_ERR_TIMEOUT&&!packet_monitor.record_bound);
        step(700,true);assert(packet_monitor.record_bound&&starts==1&&packet_monitor.session==claimed);
        actual_tap(170,UI_WIFI_CONTROLS_Y);step(0,true);dispatch();step(250,true);
        assert(packet_monitor.phase==WIFI_MONITOR_DRAIN);
        record_allow_drain=record_allow_finish=true;
        for(unsigned tick=0;tick<8;++tick){step(250,true);dispatch();}
        assert(!packet_monitor_record_job&&!packet_monitor_record_context&&!record_heap_bytes);
        assert(packet_monitor.phase==WIFI_MONITOR_OFF&&wifi_mode==UI_WIFI_MODE_PACKET_MONITOR);
    }
}

int main(void)
{
    malformed_frames();
    navigation_and_cleanup();
    ownership_and_loss();
    samples_and_dispatch();
    repeated_sessions();
    record_protocol();
    record_navigation();
    puts("Packet Monitor bridge: exact ownership, lost replies, bounded ticks, cleanup/navigation and samples passed.");
    return 0;
}
