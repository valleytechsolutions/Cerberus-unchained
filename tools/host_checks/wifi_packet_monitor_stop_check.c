#undef NDEBUG
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "cerberus_protocol.h"

typedef int esp_err_t;
enum { ESP_OK, ESP_FAIL, ESP_ERR_NOT_SUPPORTED, ESP_ERR_TIMEOUT };
#define pdMS_TO_TICKS(ms) (ms)

static bool packet_monitor_attempted;
static esp_err_t packet_monitor_error;
static uint8_t packet_monitor_channel;
static cerberus_wifi_packet_monitor_status_payload_t packet_monitor_status;
static cerberus_wifi_packet_monitor_status_payload_t stop_reply, poll_reply;
static esp_err_t stop_error, poll_error;
static unsigned stops, polls, delays;
static int64_t now_us;

static int64_t esp_timer_get_time(void) { return now_us; }
static void vTaskDelay(unsigned ms) { now_us += (int64_t)ms * 1000; ++delays; }
static esp_err_t c5_spi_link_wifi_packet_monitor_stop(uint8_t generation,
    cerberus_wifi_packet_monitor_status_payload_t *status)
{
    assert(generation == 7U);
    ++stops;
    *status = stop_reply;
    return stop_error;
}
static esp_err_t c5_spi_link_wifi_packet_monitor_status(
    cerberus_wifi_packet_monitor_status_payload_t *status)
{
    ++polls;
    *status = poll_reply;
    return poll_error;
}

#include "wifi_packet_monitor_stop_impl.h"

static void reset(void)
{
    packet_monitor_attempted = true;
    packet_monitor_error = ESP_OK;
    packet_monitor_channel = 6U;
    packet_monitor_status = (cerberus_wifi_packet_monitor_status_payload_t){
        .state = CERBERUS_WIFI_PACKET_MONITOR_RUNNING,
        .generation = 7U, .channel = 6U,
        .sample_ms = CERBERUS_WIFI_PACKET_MONITOR_SAMPLE_MS,
    };
    stop_reply = poll_reply = packet_monitor_status;
    stop_error = poll_error = ESP_OK;
    stops = polls = delays = 0U;
    now_us = 1000000;
}

int main(void)
{
    /* Stop admission precedes the worker's disable/restore result. An ERROR
     * observed later must keep navigation gated and preserve the retry. */
    reset();
    poll_reply.state = CERBERUS_WIFI_PACKET_MONITOR_ERROR;
    poll_reply.error_code = ESP_FAIL;
    assert(!packet_monitor_stop_ui());
    assert(packet_monitor_attempted && packet_monitor_error == ESP_FAIL);
    assert(packet_monitor_status.state == CERBERUS_WIFI_PACKET_MONITOR_ERROR);
    assert(stops == 1U && polls == 1U && delays == 1U);

    stop_reply = poll_reply;
    stop_error = ESP_FAIL;
    assert(!packet_monitor_stop_ui());
    assert(packet_monitor_attempted && stops == 2U && polls == 1U);
    stop_reply.state = CERBERUS_WIFI_PACKET_MONITOR_IDLE;
    stop_reply.error_code = ESP_OK;
    stop_error = ESP_OK;
    assert(packet_monitor_stop_ui());
    assert(!packet_monitor_attempted && stops == 3U && polls == 1U);

    reset();
    poll_reply.state = CERBERUS_WIFI_PACKET_MONITOR_ERROR;
    poll_reply.error_code = ESP_OK;
    assert(!packet_monitor_stop_ui());
    assert(packet_monitor_attempted && packet_monitor_error == ESP_FAIL);

    reset();
    poll_reply.state = CERBERUS_WIFI_PACKET_MONITOR_IDLE;
    assert(packet_monitor_stop_ui());
    assert(!packet_monitor_attempted && stops == 1U && polls == 1U);

    reset();
    assert(!packet_monitor_stop_ui());
    assert(packet_monitor_attempted && packet_monitor_error == ESP_ERR_TIMEOUT);
    assert(delays == 30U && polls == 30U);

    reset();
    poll_error = ESP_ERR_TIMEOUT;
    assert(!packet_monitor_stop_ui());
    assert(packet_monitor_attempted && packet_monitor_error == ESP_ERR_TIMEOUT);

    reset();
    packet_monitor_attempted = false;
    assert(packet_monitor_stop_ui() && stops == 0U && polls == 0U);
    reset();
    packet_monitor_error = ESP_ERR_NOT_SUPPORTED;
    assert(packet_monitor_stop_ui() && !packet_monitor_attempted && stops == 0U);
    puts("Packet Monitor UI: delayed cleanup errors retain ownership and retry; successful Stop releases navigation.");
    return 0;
}
