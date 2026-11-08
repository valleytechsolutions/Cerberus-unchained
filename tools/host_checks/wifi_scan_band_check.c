#include "ap_survey_foreign_boundary.h"
#include "wardrive_foreign_boundary.h"
#undef NDEBUG
#include "c5_spi_link.h"
#include <assert.h>
#include <stdatomic.h>
static atomic_bool packet_count_radio_reserved;
#include <stdio.h>
#include <string.h>

#define pdTRUE 1
#define portMAX_DELAY 0xffffffffU
#define pdMS_TO_TICKS(ms) (ms)
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
static void *scanner_mutex = (void *)1;
static unsigned defense_capture_mux;
static void *scanner_task_handle = (void *)1;
static bool scanner_ready = true, station_reserved, scan_cancelled;
/* Separate passive Probe owner; the actual admission body below checks it. */
static atomic_bool probe_sniff_radio_reserved;
static atomic_bool mac_radio_reserved, csi_radio_reserved, relation_receiving, channel_survey_radio_reserved;
static cerberus_wifi_channel_survey_status_payload_t channel_survey_status;
static cerberus_wifi_scan_status_payload_t scan_status;
static cerberus_wifi_defense_status_payload_t defense_status;
static cerberus_wifi_radio_stats_payload_t radio_stats;
static cerberus_wifi_probe_table_t probe_capture;
static cerberus_wifi_relation_table_t relation_capture;
static uint8_t scan_band;
static uint64_t scan_session_id, scan_session_epoch;
static int64_t scan_started_us;
static unsigned notifications, stops, preempts;
static bool scanner_locked, other_busy, test_busy, fail_stop;
static int64_t now_us = 1000000;
static void xSemaphoreTake(void *mutex, unsigned ticks)
{ assert(mutex == (void *)1 && ticks == portMAX_DELAY && !scanner_locked); scanner_locked = true; }
static void xSemaphoreGive(void *mutex)
{ assert(mutex == (void *)1 && scanner_locked); scanner_locked = false; }
static void xTaskNotifyGive(void *task) { assert(task == scanner_task_handle); ++notifications; }
static int64_t esp_timer_get_time(void) { return now_us; }
static bool cerberus_c5_test_running(void) { return test_busy; }
static bool another_radio_operation_is_active(void) { return other_busy; }
static bool wifi_connection_is_active_locked(void) { assert(scanner_locked); return false; }
static esp_err_t cerberus_wifi_channel_survey_preempt(void) { ++preempts; return ESP_OK; }
static void scan_observation_begin(void) { assert(scanner_locked); }
static void scan_observation_finish(bool keep) { assert(scanner_locked && !keep); }
static esp_err_t esp_wifi_scan_stop(void) { ++stops; return fail_stop ? ESP_FAIL : ESP_OK; }
esp_err_t cerberus_wifi_scanner_stop(void);
#include "wifi_scan_band_service.h"

static unsigned link_mutex = 1U;
static bool link_locked, mutex_timeout;
static uint8_t sequence;
static uint32_t peer_firmware_version;
static unsigned mutations, reads;
static uint32_t peer_version = 61U, peer_capabilities = CERBERUS_C5_CAP_WIFI_2_4_GHZ | CERBERUS_C5_CAP_WIFI_5_GHZ;
static esp_err_t peer_error, exchange_error;
static unsigned corruption;
static int64_t exchange_elapsed_us;
static int take_link_mutex(unsigned ticks)
{ assert(ticks == 20U && !link_locked); if (mutex_timeout) return 0; link_locked = true; return pdTRUE; }
static void give_link_mutex(void) { assert(link_locked); link_locked = false; }
static esp_err_t get_status_v2_passive_locked(uint8_t seq, uint32_t deadline, cerberus_c5_status_t *out)
{
    (void)seq; assert(link_locked && deadline == (uint32_t)(now_us / 1000) + 20U);
    ++reads; memset(out, 0, sizeof(*out));
    out->firmware_version = peer_version; out->capabilities = peer_capabilities;
    return peer_error;
}
static esp_err_t spi_request_response_passive(const void *request_ptr, void *discarded,
    void *response_ptr, size_t size, uint32_t deadline)
{
    (void)discarded; (void)deadline;
    assert(link_locked && size == sizeof(cerberus_spi_frame_t));
    const cerberus_spi_frame_t *request = request_ptr;
    cerberus_wifi_scan_band_response_t reply;
    cerberus_wifi_scan_band_request_t query = {0};
    if (request->command != CERBERUS_SPI_COMMAND_WIFI_SCAN_BAND_INFO) {
        assert(request->payload_length == sizeof(query)); memcpy(&query, request->payload, sizeof(query));
    }
    if (request->command == CERBERUS_SPI_COMMAND_WIFI_SCAN_BAND_START) {
        ++mutations; (void)cerberus_wifi_scanner_start_band(&query, &reply);
    } else if (request->command == CERBERUS_SPI_COMMAND_WIFI_SCAN_BAND_STOP) {
        ++mutations; (void)cerberus_wifi_scanner_band_stop(&query, &reply);
    } else if (request->command == CERBERUS_SPI_COMMAND_WIFI_SCAN_BAND_STATUS)
        (void)cerberus_wifi_scanner_band_status(&query, &reply);
    else { assert(request->command == CERBERUS_SPI_COMMAND_WIFI_SCAN_BAND_INFO);
        (void)cerberus_wifi_scanner_band_info(&reply); }
    if (corruption == 1U) ++reply.session_id;
    if (corruption == 2U) reply.band = 200U;
    if (corruption == 3U) reply.bound = 2U;
    if (corruption == 4U) reply.status.reserved[0] = 1U;
    if (corruption == 5U) reply.status.stored_count = 33U;
    assert(cerberus_protocol_prepare_frame(response_ptr, request->command, request->sequence,
        &reply, sizeof(reply)));
    now_us += exchange_elapsed_us;
    return exchange_error;
}
#include "c5_wifi_scan_band_link_impl.h"

static void reset(void)
{
    assert(!scanner_locked && !link_locked);
    memset(&scan_status, 0, sizeof(scan_status)); memset(&radio_stats, 0, sizeof(radio_stats));
    memset(&channel_survey_status, 0, sizeof(channel_survey_status));
    atomic_store(&channel_survey_radio_reserved, false);
    scan_band = CERBERUS_WIFI_SCAN_BAND_ALL; scan_session_id = 0; scan_session_epoch = 100;
    notifications = stops = preempts = mutations = reads = corruption = 0;
    scanner_mutex = (void *)1; link_mutex = 1U; scanner_ready = true;
    other_busy = test_busy = scan_cancelled = fail_stop = mutex_timeout = false;
    peer_version = 61U; peer_capabilities = CERBERUS_C5_CAP_WIFI_2_4_GHZ | CERBERUS_C5_CAP_WIFI_5_GHZ;
    peer_error = exchange_error = ESP_OK; exchange_elapsed_us = 0;
}

int main(void)
{
    assert(sizeof(cerberus_wifi_scan_band_response_t) == 22U);
    assert(cerberus_wifi_scan_band_2g_bitmap(CERBERUS_WIFI_SCAN_BAND_5G) == 1U);
    assert(cerberus_wifi_scan_band_5g_bitmap(CERBERUS_WIFI_SCAN_BAND_5G) == 0x1ffffffeU);
    assert(cerberus_wifi_scan_band_2g_bitmap(CERBERUS_WIFI_SCAN_BAND_2G) == 0x7ffeU);
    assert(cerberus_wifi_scan_band_5g_bitmap(CERBERUS_WIFI_SCAN_BAND_2G) == 1U);
    assert(!cerberus_wifi_scan_band_2g_bitmap(CERBERUS_WIFI_SCAN_BAND_ALL));
    assert(!cerberus_wifi_scan_band_5g_bitmap(CERBERUS_WIFI_SCAN_BAND_ALL));
    reset();
    cerberus_wifi_scan_band_response_t reply;
    cerberus_wifi_scan_band_request_t a = {101U, CERBERUS_WIFI_SCAN_BAND_5G};
    assert(cerberus_wifi_scanner_start_band(&a, &reply) == ESP_OK && reply.bound);
    assert(scan_session_epoch == 101U && scan_session_id == 101U && notifications == 1U);
    unsigned generation = scan_status.generation;
    assert(cerberus_wifi_scanner_start_band(&a, &reply) == ESP_OK && notifications == 1U);
    scan_status.state = CERBERUS_WIFI_SCAN_COMPLETE;
    assert(cerberus_wifi_scanner_start_band(&a, &reply) == ESP_OK && notifications == 1U);
    assert(reply.status.generation == generation && reply.status.state == CERBERUS_WIFI_SCAN_COMPLETE);
    cerberus_wifi_scan_band_request_t bad = a; bad.band = CERBERUS_WIFI_SCAN_BAND_2G;
    assert(cerberus_wifi_scanner_start_band(&bad, &reply) == ESP_ERR_INVALID_ARG && !reply.bound);
    assert(scan_status.state == CERBERUS_WIFI_SCAN_COMPLETE);
    assert(cerberus_wifi_scanner_start() == ESP_OK && scan_session_epoch == 102U && scan_session_id == 0U);
    assert(scan_band == CERBERUS_WIFI_SCAN_BAND_ALL && notifications == 2U);
    assert(cerberus_wifi_scanner_start_band(&a, &reply) == ESP_ERR_INVALID_STATE && !reply.bound);
    assert(cerberus_wifi_scanner_band_stop(&a, &reply) == ESP_ERR_INVALID_STATE && stops == 0U);
    assert(scan_status.state == CERBERUS_WIFI_SCAN_RUNNING);
    for (unsigned i = 0; i < 260U; ++i) {
        scan_status.state = CERBERUS_WIFI_SCAN_COMPLETE; assert(cerberus_wifi_scanner_start() == ESP_OK);
    }
    assert(cerberus_wifi_scanner_start_band(&a, &reply) == ESP_ERR_INVALID_STATE);
    assert(cerberus_wifi_scanner_band_stop(&a, &reply) == ESP_ERR_INVALID_STATE && !stops);
    scan_status.state = CERBERUS_WIFI_SCAN_COMPLETE; scan_session_epoch = UINT64_MAX;
    assert(cerberus_wifi_scanner_start() == ESP_ERR_INVALID_STATE);
    a.session_id = 1U; assert(cerberus_wifi_scanner_start_band(&a, &reply) == ESP_ERR_INVALID_STATE);

    reset(); a.session_id = 101U;
    atomic_store(&probe_sniff_radio_reserved,true);
    assert(cerberus_wifi_scanner_start_band(&a,&reply)==ESP_ERR_INVALID_STATE);
    assert(cerberus_wifi_scanner_start()==ESP_ERR_INVALID_STATE);
    assert(!notifications && scan_session_epoch==100U);
    atomic_store(&probe_sniff_radio_reserved,false);
    { atomic_store(&packet_count_radio_reserved,true);
    assert(cerberus_wifi_scanner_start_band(&a,&reply)==ESP_ERR_INVALID_STATE);
    assert(cerberus_wifi_scanner_start()==ESP_ERR_INVALID_STATE);
    assert(!notifications && scan_session_epoch==100U);
    atomic_store(&packet_count_radio_reserved,false); }


    reset(); a.session_id = 101U;
    channel_survey_status.state = CERBERUS_WIFI_CHANNEL_SURVEY_RUNNING;
    assert(cerberus_wifi_scanner_start_band(&a, &reply) == ESP_ERR_INVALID_STATE);
    assert(!preempts && !notifications && scan_session_epoch == 100U);
    channel_survey_status.state = CERBERUS_WIFI_CHANNEL_SURVEY_ERROR;
    atomic_store(&channel_survey_radio_reserved, true);
    assert(cerberus_wifi_scanner_start_band(&a, &reply) == ESP_ERR_INVALID_STATE && !preempts && !notifications);
    assert(cerberus_wifi_scanner_start() == ESP_OK && preempts == 1U); /* Legacy policy retained. */

    reset(); cerberus_wifi_scan_status_payload_t status;
    peer_version = 60U;
    assert(c5_spi_link_wifi_scan_band_info(&reply) == ESP_ERR_NOT_SUPPORTED && !mutations);
    assert(c5_spi_link_start_wifi_scan_band(2U, 101U, &status) == ESP_ERR_NOT_SUPPORTED && !mutations);
    peer_version = 61U; peer_capabilities = CERBERUS_C5_CAP_WIFI_2_4_GHZ;
    assert(c5_spi_link_start_wifi_scan_band(2U, 101U, &status) == ESP_ERR_NOT_SUPPORTED && !mutations);
    peer_capabilities |= CERBERUS_C5_CAP_WIFI_5_GHZ;
    assert(c5_spi_link_wifi_scan_band_info(&reply) == ESP_OK && reply.session_id == 100U && !reply.bound);
    exchange_error = ESP_ERR_TIMEOUT; /* Accepted start, lost response. */
    assert(c5_spi_link_start_wifi_scan_band(2U, 101U, &status) == ESP_ERR_TIMEOUT);
    assert(notifications == 1U && scan_session_id == 101U);
    exchange_error = ESP_OK;
    assert(c5_spi_link_start_wifi_scan_band(2U, 101U, &status) == ESP_OK && notifications == 1U);
    for (corruption = 1U; corruption <= 5U; ++corruption)
        assert(c5_spi_link_wifi_scan_band_status(2U, 101U, &status) == ESP_ERR_INVALID_RESPONSE && !status.state);
    corruption = 0U;
    exchange_elapsed_us = 20000;
    assert(c5_spi_link_wifi_scan_band_status(2U, 101U, &status) == ESP_ERR_TIMEOUT && !status.state);
    exchange_elapsed_us = -1;
    assert(c5_spi_link_wifi_scan_band_status(2U, 101U, &status) == ESP_ERR_INVALID_STATE && !status.state);
    exchange_elapsed_us = 0; fail_stop = true;
    assert(c5_spi_link_stop_wifi_scan_band(2U, 101U, &status) == ESP_OK);
    assert(status.state == CERBERUS_WIFI_SCAN_RUNNING && status.error_code == ESP_FAIL && scan_cancelled);
    fail_stop = false;
    assert(c5_spi_link_stop_wifi_scan_band(2U, 101U, &status) == ESP_OK && stops == 2U);
    scan_status.state = CERBERUS_WIFI_SCAN_IDLE; scan_cancelled = false;
    assert(c5_spi_link_wifi_scan_band_status(2U, 101U, &status) == ESP_OK && status.state == CERBERUS_WIFI_SCAN_IDLE);
    assert(cerberus_wifi_scanner_start() == ESP_OK);
    assert(c5_spi_link_stop_wifi_scan_band(2U, 101U, &status) == ESP_ERR_NOT_FOUND && stops == 2U);
    assert(c5_spi_link_wifi_scan_band_status(2U, 101U, &status) == ESP_ERR_NOT_FOUND);
    assert(scan_status.state == CERBERUS_WIFI_SCAN_RUNNING);
    assert(c5_spi_link_start_wifi_scan_band(255U, 101U, &status) == ESP_ERR_INVALID_ARG);
    assert(c5_spi_link_start_wifi_scan_band(2U, 0U, &status) == ESP_ERR_INVALID_ARG);
    puts("Wi-Fi band leases: masks, default isolation, replay, generation wrap, stale Stop, overflow, old-peer and transport faults passed.");
    return 0;
}
