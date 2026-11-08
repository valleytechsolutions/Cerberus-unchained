#undef NDEBUG
#include "wifi_packet_monitor_core.h"
#include "wifi_channel_analyzer.h"
#include <assert.h>
#include <setjmp.h>
#include <stdatomic.h>
static atomic_bool packet_count_radio_reserved;
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
typedef unsigned TickType_t;
typedef int wifi_band_mode_t;
typedef int wifi_second_chan_t;
typedef void *TaskHandle_t;
enum { WIFI_BAND_2G = 2, WIFI_BAND_5G = 5,
    WIFI_SECOND_CHAN_NONE = 0, WIFI_PKT_MGMT = 0, WIFI_PKT_CTRL = 1, WIFI_PKT_DATA = 2,
    WIFI_PROMIS_FILTER_MASK_MGMT = 1, WIFI_PROMIS_FILTER_MASK_CTRL = 2,
    WIFI_PROMIS_FILTER_MASK_DATA = 4, WIFI_SCAN_TYPE_PASSIVE = 1 };
#define WIFI_PROMIS_CTRL_FILTER_MASK_ALL 0xff800000U
#define pdTRUE 1
#define portMAX_DELAY 0xffffffffU
#define pdMS_TO_TICKS(ms) (ms)
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
#define WIFI_PASSIVE_DWELL_MS 120U
typedef unsigned wifi_promiscuous_pkt_type_t;
typedef struct { unsigned filter_mask; } wifi_promiscuous_filter_t;
typedef struct {
    struct { unsigned rx_state, sig_len, channel; int8_t rssi; } rx_ctrl;
    uint8_t payload[128];
} wifi_promiscuous_pkt_t;
typedef struct {
    bool show_hidden;
    unsigned scan_type;
    struct { unsigned passive; } scan_time;
    struct { uint16_t ghz_2_channels; uint32_t ghz_5_channels; } channel_bitmap;
} wifi_scan_config_t;

static unsigned scanner_mutex = 1U;
static TaskHandle_t scanner_task_handle = &scanner_mutex;
static bool scanner_ready = true, station_reserved, scan_cancelled;
static uint8_t scan_band;
static cerberus_wifi_scan_status_payload_t scan_status;
static cerberus_wifi_defense_status_payload_t defense_status;
/* Separate passive Probe owner; the actual admission body below checks it. */
static atomic_bool probe_sniff_radio_reserved;
static atomic_bool capture_radio_reserved;
static cerberus_wifi_packet_monitor_status_payload_t packet_monitor_status;
static int64_t packet_monitor_started_us;
static atomic_bool packet_monitor_cancelled, packet_monitor_radio_reserved;
static int packet_monitor_mux, defense_capture_mux;
static bool packet_monitor_receiving, watch_capture_active;
static uint8_t packet_monitor_receive_channel;
static cerberus_wifi_packet_monitor_counters_t packet_monitor_counts;
static wifi_band_mode_t packet_monitor_previous_band_mode;
static uint8_t packet_monitor_previous_channel;
static wifi_second_chan_t packet_monitor_previous_second;
static wifi_promiscuous_filter_t packet_monitor_previous_filter;
static bool packet_monitor_restore_pending, packet_monitor_disable_pending;
static struct { struct { uint8_t flags; } status; uint64_t capture_epoch; } watch_capture;
static uint64_t watch_capture_epoch;
static cerberus_wifi_probe_table_t probe_capture;
static cerberus_wifi_relation_table_t relation_capture;
static atomic_bool relation_receiving;
static uint8_t watch_capture_channel;
static uint64_t watch_capture_deadline_us;
static atomic_bool remote_id_radio_reserved;
static atomic_bool csi_radio_reserved;
static atomic_bool mac_radio_reserved;
static unsigned mac_polls,probe_polls;
static void mac_worker_tick(void) { assert(atomic_load(&mac_radio_reserved)); ++mac_polls; }
static unsigned csi_polls;
static void csi_worker_tick(void) { assert(atomic_load(&csi_radio_reserved)); ++csi_polls; }
static unsigned remote_polls;
static void remote_id_worker_tick(void)
{ assert(atomic_load(&remote_id_radio_reserved)); ++remote_polls; }

static jmp_buf worker_done;
static bool locked, promiscuous, cancel_during_enable;
static bool fail_disable, fail_restore_band, fail_restore_channel;
static unsigned notifications, samples, stop_after, ap_scans, capture_polls;
static unsigned enable_calls, disable_calls, station_calls;
static uint32_t clock_ms;
static wifi_band_mode_t current_band;
static uint8_t current_channel;
static wifi_second_chan_t current_second;
static wifi_promiscuous_filter_t current_filter;
static wifi_promiscuous_filter_t current_ctrl_filter;
static bool fail_ctrl_filter_restore;
static bool fail_filter_restore, record_heap_fail;
static bool record_test_data;
static void (*analyzer_test_delay)(void);
static size_t record_heap_bytes, record_heap_free = 65536U;
#define MALLOC_CAP_INTERNAL 1U
#define MALLOC_CAP_8BIT 2U
static size_t heap_caps_get_free_size(unsigned caps)
{ assert(caps == 3U); return record_heap_free; }
static void *heap_caps_calloc(size_t n, size_t bytes, unsigned caps)
{
    assert(caps == 3U && !record_heap_bytes);
    if (record_heap_fail) return NULL;
    void *p = calloc(n, bytes);
    if (p) record_heap_bytes = n * bytes;
    return p;
}
static void heap_caps_free(void *p)
{ assert(p && record_heap_bytes); record_heap_bytes = 0U; free(p); }

static void xSemaphoreTake(unsigned mutex, unsigned timeout)
{ assert(mutex == 1U && timeout == portMAX_DELAY && !locked); locked = true; }
static void xSemaphoreGive(unsigned mutex)
{ assert(mutex == 1U && locked); locked = false; }
static void xTaskNotifyGive(TaskHandle_t task)
{ assert(task == scanner_task_handle); ++notifications; }
static void ulTaskNotifyTake(unsigned clear, unsigned timeout)
{
    assert(clear == pdTRUE && (timeout == portMAX_DELAY || timeout == 250U) && !locked);
    if (!notifications) longjmp(worker_done, 1);
    notifications = 0U;
}
static int64_t esp_timer_get_time(void) { return (int64_t)clock_ms * 1000; }
static uint32_t esp_random(void) { return 0x12345678U; }
static TickType_t xTaskGetTickCount(void) { return clock_ms; }
static void vTaskDelayUntil(TickType_t *last, TickType_t period);
static void vTaskDelay(TickType_t delay)
{
    if (delay == 20U) {
        if(atomic_load(&packet_count_radio_reserved)){clock_ms+=delay;atomic_store(&packet_count_radio_reserved,false);return;}
        if (atomic_load(&probe_sniff_radio_reserved)) {
            clock_ms += delay;atomic_store(&probe_sniff_radio_reserved,false);return;
        }
        if (atomic_load(&mac_radio_reserved)) {
            clock_ms += delay; atomic_store(&mac_radio_reserved, false); return;
        }
        if (atomic_load(&csi_radio_reserved)) {
            clock_ms += delay; atomic_store(&csi_radio_reserved, false); return;
        }
        assert(atomic_load(&remote_id_radio_reserved));
        clock_ms += delay;
        atomic_store(&remote_id_radio_reserved, false);
        return;
    }
    assert(delay == 50U && atomic_load(&capture_radio_reserved));
    ++capture_polls;
    clock_ms += delay;
    atomic_store(&capture_radio_reserved, false);
}
static bool cerberus_c5_test_running(void) { return false; }
static unsigned survey_preempts;
static esp_err_t cerberus_wifi_channel_survey_preempt(void) { ++survey_preempts; return ESP_OK; }
static bool another_radio_operation_is_active(void)
{ return atomic_load(&capture_radio_reserved) || atomic_load(&packet_monitor_radio_reserved); }
static bool wifi_connection_is_active_locked(void) { assert(locked); return false; }
static esp_err_t cerberus_wifi_packet_monitor_stop(uint8_t generation);
esp_err_t cerberus_wifi_channel_analyzer_stop(uint64_t session,
    cerberus_wifi_channel_analyzer_status_t *out);
/* The callback runs before the included service defines its private mode. */
static void cancel_receive_owner(void);
static esp_err_t esp_wifi_get_band_mode(wifi_band_mode_t *band)
{ *band = current_band; return ESP_OK; }
static esp_err_t esp_wifi_get_channel(uint8_t *channel, wifi_second_chan_t *second)
{ *channel = current_channel; *second = current_second; return ESP_OK; }
static esp_err_t esp_wifi_set_band(unsigned band)
{ current_band = (int)band; return ESP_OK; }
static esp_err_t esp_wifi_set_band_mode(wifi_band_mode_t band)
{ if (fail_restore_band) return ESP_FAIL; current_band = band; return ESP_OK; }
static esp_err_t esp_wifi_set_channel(uint8_t channel, wifi_second_chan_t second)
{
    if (channel == 6U && fail_restore_channel) return ESP_FAIL;
    current_channel = channel; current_second = second; return ESP_OK;
}
static esp_err_t esp_wifi_set_promiscuous_filter(const wifi_promiscuous_filter_t *filter)
{
    if (fail_filter_restore && filter->filter_mask == 0x20U) return ESP_FAIL;
    current_filter = *filter;
    return ESP_OK;
}
static esp_err_t esp_wifi_get_promiscuous_filter(wifi_promiscuous_filter_t *filter)
{ *filter = current_filter; return ESP_OK; }
static esp_err_t esp_wifi_get_promiscuous_ctrl_filter(wifi_promiscuous_filter_t *filter)
{ *filter = current_ctrl_filter; return ESP_OK; }
static esp_err_t esp_wifi_set_promiscuous_ctrl_filter(const wifi_promiscuous_filter_t *filter)
{
    if (fail_ctrl_filter_restore && filter->filter_mask == 0x800000U) return ESP_FAIL;
    current_ctrl_filter = *filter;
    return ESP_OK;
}
static esp_err_t esp_wifi_set_promiscuous(bool enabled)
{
    if (enabled) {
        ++enable_calls; promiscuous = true;
        if (cancel_during_enable) cancel_receive_owner();
    } else {
        ++disable_calls;
        if (fail_disable) return ESP_FAIL;
        promiscuous = false;
    }
    return ESP_OK;
}

#include "wifi_packet_monitor_service.h"

static void cancel_receive_owner(void)
{
    if (channel_analyzer_mode) {
        cerberus_wifi_channel_analyzer_status_t reply;
        assert(cerberus_wifi_channel_analyzer_stop(packet_monitor_session_id, &reply) == ESP_OK);
    } else assert(cerberus_wifi_packet_monitor_stop(packet_monitor_status.generation) == ESP_OK);
}

static void vTaskDelayUntil(TickType_t *last, TickType_t period)
{
    assert(period == (channel_analyzer_mode ? 100U : 200U) &&
        !locked && atomic_load(&packet_monitor_radio_reserved));
    *last += period; clock_ms = *last; ++samples;
    if (channel_analyzer_mode && analyzer_test_delay) analyzer_test_delay();
    else {
    wifi_promiscuous_pkt_t packet = {.rx_ctrl = {0U, 28U, current_channel, -55}};
    packet.payload[0] = 0x80U;
    packet_monitor_receive(&packet, WIFI_PKT_MGMT);
    if (record_test_data) {
        packet.payload[0] = 0x08U;
        packet_monitor_receive(&packet, WIFI_PKT_DATA);
    }
    }
    if (stop_after && samples >= stop_after) {
        if (channel_analyzer_mode) {
            cerberus_wifi_channel_analyzer_status_t reply;
            assert(cerberus_wifi_channel_analyzer_stop(packet_monitor_session_id, &reply) == ESP_OK);
        } else if (packet_monitor_session_id) {
            cerberus_wifi_packet_monitor_session_status_t reply;
            assert(cerberus_wifi_packet_monitor_session_stop(packet_monitor_session_id, &reply) == ESP_OK);
        } else assert(cerberus_wifi_packet_monitor_stop(packet_monitor_status.generation) == ESP_OK);
    }
}
static bool station_run(void) { ++station_calls; return false; }
static void wifi_capture_worker_tick(void)
{ assert(atomic_load(&capture_radio_reserved)); }
static void cerberus_watch_capture_begin(void *capture, unsigned radio,
    uint8_t generation, uint32_t now)
{ (void)capture; (void)radio; (void)generation; (void)now; }
static void cerberus_watch_capture_begin_with_epoch(void *capture, unsigned radio,
    uint8_t generation, uint32_t now, uint64_t epoch)
{ cerberus_watch_capture_begin(capture,radio,generation,now); watch_capture.capture_epoch=epoch; }
static esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *config, bool block)
{
    assert(block && config->show_hidden && config->scan_type == WIFI_SCAN_TYPE_PASSIVE);
    assert(config->channel_bitmap.ghz_2_channels == cerberus_wifi_scan_band_2g_bitmap(scan_band));
    assert(config->channel_bitmap.ghz_5_channels == cerberus_wifi_scan_band_5g_bitmap(scan_band));
    assert(!atomic_load(&packet_monitor_radio_reserved)); ++ap_scans; return ESP_OK;
}
static void finish_scan(esp_err_t error)
{ assert(error == ESP_OK); scan_status.state = CERBERUS_WIFI_SCAN_COMPLETE;
  cerberus_wifi_probe_finish(&probe_capture, CERBERUS_WIFI_PROBE_COMPLETE); }
/* Normal AP evidence is exercised by check_wifi_scan_observation.py. This
 * fixture keeps its focus on shared monitor dispatch and restoration. */
static void scan_observation_arm(uint8_t generation, bool available)
{ assert(generation == scan_status.generation && available); }
static void scan_observation_finish(bool keep) { assert(keep); }
static bool probe_sniff_retained(void){return false;}
static bool packet_count_retained(void){return atomic_load(&packet_count_radio_reserved);}
static unsigned count_polls;
static void packet_count_worker_tick(void){if(atomic_load(&packet_count_radio_reserved))++count_polls;}
static void probe_sniff_worker_tick(void){if(atomic_load(&probe_sniff_radio_reserved))++probe_polls;}
#include "wifi_scan_worker_impl.h"

static void dispatch(void)
{
    if (!setjmp(worker_done)) wifi_scan_task(NULL);
    assert(!locked);
}
static void reset(void)
{
    assert(!locked);
    assert(!packet_monitor_record_context && !record_heap_bytes);
    packet_monitor_record_requested = false;
    packet_monitor_record_released_session = 0U;
    record_heap_fail = fail_filter_restore = false;
    record_test_data = false;
    record_heap_free = 65536U;
    current_filter.filter_mask = 0x20U;
    current_ctrl_filter.filter_mask = 0x800000U;
    fail_ctrl_filter_restore = false;
    atomic_store(&packet_count_radio_reserved,false);count_polls=0;
    memset(&scan_status, 0, sizeof(scan_status));
    scan_band = CERBERUS_WIFI_SCAN_BAND_ALL;
    memset(&probe_capture, 0, sizeof(probe_capture));
    memset(&relation_capture, 0, sizeof(relation_capture));atomic_store(&relation_receiving,false);
    memset(&packet_monitor_status, 0, sizeof(packet_monitor_status));
    memset(&packet_receive_history, 0, sizeof(packet_receive_history));
    channel_analyzer_mode = channel_analyzer_saturated = false;
    atomic_store(&channel_analyzer_clock_invalid, false);
    channel_analyzer_receive_session = 0U; channel_analyzer_frames = 0U;
    memset(&channel_analyzer_hint, 0, sizeof(channel_analyzer_hint));
    channel_analyzer_hint_us = -1;
    memset(&channel_analyzer_projection, 0, sizeof(channel_analyzer_projection));
    channel_analyzer_projection_id = 0U; channel_analyzer_projection_valid = false;
    analyzer_test_delay = NULL;
    packet_monitor_session_epoch = packet_monitor_session_id = 0U;
    packet_monitor_control_busy = packet_monitor_cleanup_retry = false;
    atomic_store(&packet_monitor_radio_reserved, false);
    atomic_store(&packet_monitor_cancelled, false);
    atomic_store(&capture_radio_reserved, false);
    atomic_store(&remote_id_radio_reserved, false);
    atomic_store(&probe_sniff_radio_reserved,false);probe_polls=0U;
    atomic_store(&csi_radio_reserved, false); csi_polls = 0U;
    atomic_store(&mac_radio_reserved, false); mac_polls = 0U;
    remote_polls = 0U;
    packet_monitor_receiving = packet_monitor_restore_pending = false;
    packet_monitor_disable_pending = promiscuous = cancel_during_enable = false;
    fail_disable = fail_restore_band = fail_restore_channel = false;
    notifications = samples = ap_scans = capture_polls = 0U;
    enable_calls = disable_calls = station_calls = 0U;
    stop_after = 3U; clock_ms = 1000U;
    current_band = 9; current_channel = 6U; current_second = 2;
}
static void run_ap_and_capture(void)
{
    scan_status.state = CERBERUS_WIFI_SCAN_RUNNING;
    cerberus_wifi_probe_begin(&probe_capture, scan_status.generation);
    xTaskNotifyGive(scanner_task_handle); dispatch();
    assert(ap_scans == 1U && scan_status.state == CERBERUS_WIFI_SCAN_COMPLETE);
    atomic_store(&capture_radio_reserved, true);
    xTaskNotifyGive(scanner_task_handle); dispatch();
    assert(capture_polls == 1U && !atomic_load(&capture_radio_reserved));
}
int main(void)
{
    /* A band request affects only this worker's stack-local scan config.
     * Completion and the next default scan cannot strand the persistent band. */
    reset(); scan_band = CERBERUS_WIFI_SCAN_BAND_5G;
    run_ap_and_capture();
    assert(current_band == 9 && current_channel == 6U && current_second == 2);
    scan_band = CERBERUS_WIFI_SCAN_BAND_ALL; ap_scans = capture_polls = 0U;
    run_ap_and_capture();
    assert(current_band == 9 && current_channel == 6U && current_second == 2);
    reset(); atomic_store(&probe_sniff_radio_reserved,true);
    assert(cerberus_wifi_packet_monitor_start(36U)==ESP_ERR_INVALID_STATE&&!notifications&&!enable_calls);
    xTaskNotifyGive(scanner_task_handle);dispatch();
    assert(probe_polls==2U&&!atomic_load(&probe_sniff_radio_reserved));
    assert(!ap_scans&&!capture_polls&&!remote_polls&&!csi_polls&&!mac_polls&&!station_calls&&!enable_calls);
    run_ap_and_capture();
    reset(); atomic_store(&packet_count_radio_reserved,true);
    assert(cerberus_wifi_packet_monitor_start(36U)==ESP_ERR_INVALID_STATE&&!notifications&&!enable_calls);
    xTaskNotifyGive(scanner_task_handle);dispatch();
    assert(count_polls==2U&&!atomic_load(&packet_count_radio_reserved));
    assert(!ap_scans&&!capture_polls&&!remote_polls&&!csi_polls&&!mac_polls&&!station_calls&&!enable_calls);
    run_ap_and_capture();
    reset(); atomic_store(&mac_radio_reserved, true);
    unsigned prior_preempts=survey_preempts;
    assert(cerberus_wifi_packet_monitor_start(36U) == ESP_ERR_INVALID_STATE && !notifications && !enable_calls);
    assert(survey_preempts==prior_preempts);
    xTaskNotifyGive(scanner_task_handle); dispatch();
    assert(mac_polls == 1U && !atomic_load(&mac_radio_reserved));
    assert(!ap_scans && !capture_polls && !remote_polls && !csi_polls && !station_calls && !enable_calls);
    run_ap_and_capture();
    reset(); atomic_store(&csi_radio_reserved, true);
    assert(cerberus_wifi_packet_monitor_start(36U) == ESP_ERR_INVALID_STATE && !notifications && !enable_calls);
    xTaskNotifyGive(scanner_task_handle); dispatch();
    assert(csi_polls == 1U && !atomic_load(&csi_radio_reserved));
    assert(!ap_scans && !capture_polls && !remote_polls && !station_calls && !enable_calls);
    run_ap_and_capture();
    reset();
    atomic_store(&remote_id_radio_reserved, true);
    xTaskNotifyGive(scanner_task_handle); dispatch();
    assert(remote_polls == 1U && !atomic_load(&remote_id_radio_reserved));
    assert(!ap_scans && !capture_polls && !station_calls && !enable_calls);
    run_ap_and_capture(); /* Same worker resumes existing jobs after release. */
    reset();
    assert(cerberus_wifi_packet_monitor_start(36U) == ESP_OK);
    assert(notifications == 1U && cerberus_wifi_packet_monitor_active());
    assert(cerberus_wifi_packet_monitor_start(36U) == ESP_OK && notifications == 1U);
    assert(cerberus_wifi_packet_monitor_start(40U) == ESP_ERR_INVALID_STATE);
    assert(cerberus_wifi_packet_monitor_stop(0U) == ESP_ERR_INVALID_STATE);
    dispatch();
    assert(packet_monitor_status.state == CERBERUS_WIFI_PACKET_MONITOR_IDLE);
    assert(!cerberus_wifi_packet_monitor_active() && !promiscuous);
    assert(current_band == 9 && current_channel == 6U && current_second == 2);
    assert(packet_monitor_status.stored_count == 2U && station_calls == 0U);
    cerberus_wifi_packet_monitor_sample_payload_t sample;
    assert(cerberus_wifi_packet_monitor_get_sample(packet_monitor_status.generation,
        packet_monitor_status.latest_sequence, &sample) == ESP_OK);
    assert(sample.channel == 36U && sample.beacons == 1U);
    run_ap_and_capture();

    reset();
    assert(cerberus_wifi_packet_monitor_start(10U) == ESP_OK);
    assert(cerberus_wifi_packet_monitor_stop(packet_monitor_status.generation) == ESP_OK);
    dispatch(); /* Stop arrived before the shared worker took its notification. */
    assert(enable_calls == 0U && !cerberus_wifi_packet_monitor_active());
    assert(packet_monitor_status.state == CERBERUS_WIFI_PACKET_MONITOR_IDLE);
    xTaskNotifyGive(scanner_task_handle); dispatch(); /* stale wake cannot start an AP scan */
    assert(ap_scans == 0U && enable_calls == 0U);
    run_ap_and_capture();

    reset(); cancel_during_enable = true;
    assert(cerberus_wifi_packet_monitor_start(10U) == ESP_OK); dispatch();
    assert(samples == 0U && !cerberus_wifi_packet_monitor_active() && !promiscuous);

    for (unsigned fault = 0U; fault < 4U; ++fault) {
        reset();
        fail_disable = fault == 0U;
        fail_restore_band = fault == 1U;
        fail_restore_channel = fault == 2U;
        fail_filter_restore = fault == 3U;
        assert(cerberus_wifi_packet_monitor_start(36U) == ESP_OK); dispatch();
        assert(cerberus_wifi_packet_monitor_active());
        assert(packet_monitor_status.state == CERBERUS_WIFI_PACKET_MONITOR_ERROR);
        assert(cerberus_wifi_packet_monitor_start(36U) == ESP_ERR_INVALID_STATE);
        xTaskNotifyGive(scanner_task_handle); dispatch(); /* retained lease blocks other work */
        assert(station_calls == 0U && ap_scans == 0U);
        fail_disable = fail_restore_band = fail_restore_channel = false;
        fail_filter_restore = false;
        assert(cerberus_wifi_packet_monitor_stop(packet_monitor_status.generation) == ESP_OK);
        assert(!cerberus_wifi_packet_monitor_active() && !promiscuous);
        assert(current_band == 9 && current_channel == 6U && current_second == 2);
        assert(current_filter.filter_mask == 0x20U);
        run_ap_and_capture();
    }
    /* Scoped commands never borrow a generation/channel match as authority. */
    reset();
    cerberus_wifi_packet_monitor_session_status_t scoped;
    cerberus_wifi_packet_monitor_session_info(&scoped);
    assert(scoped.session_id && !scoped.flags);
    cerberus_wifi_packet_monitor_session_start_t start = {scoped.session_id + 1U, 36U};
    assert(cerberus_wifi_packet_monitor_session_start(&start, &scoped) == ESP_OK);
    uint64_t first = start.session_id;
    assert(scoped.session_id == first && scoped.flags ==
        (CERBERUS_WIFI_PACKET_MONITOR_BOUND | CERBERUS_WIFI_PACKET_MONITOR_HELD));
    assert(cerberus_wifi_packet_monitor_start(36U) == ESP_ERR_INVALID_STATE);
    assert(cerberus_wifi_packet_monitor_stop(packet_monitor_status.generation) == ESP_ERR_INVALID_STATE);
    assert(!atomic_load(&packet_monitor_cancelled));
    assert(cerberus_wifi_packet_monitor_session_start(&start, &scoped) == ESP_OK);
    assert(notifications == 1U);
    ++start.channel;
    assert(cerberus_wifi_packet_monitor_session_start(&start, &scoped) == ESP_ERR_INVALID_ARG);
    start.channel = 36U;
    assert(cerberus_wifi_packet_monitor_session_stop(first + 10U, &scoped) == ESP_ERR_NOT_FOUND);
    assert(!scoped.flags && !scoped.state && !scoped.channel && !atomic_load(&packet_monitor_cancelled));
    dispatch();
    assert(!cerberus_wifi_packet_monitor_active());
    assert(cerberus_wifi_packet_monitor_session_status(first, &scoped) == ESP_OK);
    assert(scoped.flags == (CERBERUS_WIFI_PACKET_MONITOR_BOUND | CERBERUS_WIFI_PACKET_MONITOR_CANCELLED));
    cerberus_wifi_packet_monitor_session_read_t read = {first, scoped.latest_sequence};
    cerberus_wifi_packet_monitor_session_sample_t received;
    assert(cerberus_wifi_packet_monitor_session_sample(&read, &received) == ESP_OK);
    assert(received.session_id == first && received.sample.channel == 36U);
    assert(cerberus_wifi_packet_monitor_get_sample(received.sample.generation,
        received.sample.sequence, &sample) == ESP_ERR_INVALID_STATE);
    start.session_id++;
    assert(cerberus_wifi_packet_monitor_session_start(&start, &scoped) == ESP_OK);
    assert(cerberus_wifi_packet_monitor_session_stop(first, &scoped) == ESP_ERR_NOT_FOUND);
    assert(cerberus_wifi_packet_monitor_session_sample(&read, &received) == ESP_ERR_NOT_FOUND);
    assert(!atomic_load(&packet_monitor_cancelled));
    assert(cerberus_wifi_packet_monitor_session_stop(start.session_id, &scoped) == ESP_OK);
    dispatch();

    /* A rejected Start is consumed; releasing the competing owner cannot make
     * that same request turn into an accepted scan later. */
    reset();
    cerberus_wifi_packet_monitor_session_info(&scoped);
    start = (cerberus_wifi_packet_monitor_session_start_t){scoped.session_id + 1U, 6U};
    atomic_store(&probe_sniff_radio_reserved, true);
    assert(cerberus_wifi_packet_monitor_session_start(&start, &scoped) == ESP_ERR_INVALID_STATE);
    assert(!scoped.flags && scoped.session_id == start.session_id && !notifications);
    atomic_store(&probe_sniff_radio_reserved, false);
    assert(cerberus_wifi_packet_monitor_session_start(&start, &scoped) == ESP_ERR_INVALID_STATE);
    assert(!notifications && !enable_calls);
    ++start.session_id;
    packet_monitor_control_busy = true;
    assert(cerberus_wifi_packet_monitor_session_start(&start, &scoped) == ESP_ERR_INVALID_STATE);
    packet_monitor_control_busy = false;
    assert(cerberus_wifi_packet_monitor_session_start(&start, &scoped) == ESP_ERR_INVALID_STATE);
    assert(!notifications && !enable_calls);
    ++start.session_id;
    assert(cerberus_wifi_packet_monitor_session_start(&start, &scoped) == ESP_OK);
    fail_disable = true;
    dispatch();
    assert(packet_monitor_status.state == CERBERUS_WIFI_PACKET_MONITOR_ERROR);
    assert(cerberus_wifi_packet_monitor_active());
    unsigned disabled = disable_calls;
    assert(cerberus_wifi_packet_monitor_session_stop(start.session_id, &scoped) == ESP_OK);
    assert(disable_calls == disabled && (scoped.flags & CERBERUS_WIFI_PACKET_MONITOR_HELD));
    dispatch(); /* Cleanup is on the worker, not the SPI command thread. */
    assert(disable_calls == disabled + 1U && cerberus_wifi_packet_monitor_active());
    fail_disable = false;
    assert(cerberus_wifi_packet_monitor_session_stop(start.session_id, &scoped) == ESP_OK);
    dispatch();
    assert(!cerberus_wifi_packet_monitor_active() && !promiscuous);
    run_ap_and_capture();

    /* An abandoned scoped scan expires. Ordinary INFO reads do not renew it. */
    reset(); stop_after = 0U;
    cerberus_wifi_packet_monitor_session_info(&scoped);
    start = (cerberus_wifi_packet_monitor_session_start_t){scoped.session_id + 1U, 6U};
    assert(cerberus_wifi_packet_monitor_session_start(&start, &scoped) == ESP_OK);
    clock_ms += CERBERUS_WIFI_PACKET_MONITOR_LEASE_MS - 100U;
    cerberus_wifi_packet_monitor_session_info(&scoped);
    dispatch();
    assert(samples == 1U && !cerberus_wifi_packet_monitor_active());
    assert(atomic_load(&packet_monitor_cancelled));
    reset(); stop_after = 0U;
    cerberus_wifi_packet_monitor_session_info(&scoped);
    start = (cerberus_wifi_packet_monitor_session_start_t){scoped.session_id + 1U, 6U};
    assert(cerberus_wifi_packet_monitor_session_start(&start, &scoped) == ESP_OK);
    clock_ms += CERBERUS_WIFI_PACKET_MONITOR_LEASE_MS - 100U;
    assert(cerberus_wifi_packet_monitor_session_status(start.session_id, &scoped) == ESP_OK);
    dispatch();
    assert(samples == 50U && !cerberus_wifi_packet_monitor_active());
    reset();
    cerberus_wifi_packet_monitor_session_info(&scoped);
    start = (cerberus_wifi_packet_monitor_session_start_t){scoped.session_id + 1U, 6U};
    assert(cerberus_wifi_packet_monitor_session_start(&start, &scoped) == ESP_OK);
    clock_ms = 0U;
    assert(cerberus_wifi_packet_monitor_session_status(start.session_id, &scoped) == ESP_OK);
    assert(scoped.flags & CERBERUS_WIFI_PACKET_MONITOR_CANCELLED);
    dispatch();
    assert(!enable_calls && !cerberus_wifi_packet_monitor_active());

    puts("C5 monitor: legacy/scoped ownership, stale requests, leases, cleanup retry and next scan passed.");
    return 0;
}
