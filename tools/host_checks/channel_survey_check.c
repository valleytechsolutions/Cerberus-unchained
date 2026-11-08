#ifdef NDEBUG
#undef NDEBUG
#endif
#include "wifi_channel_survey_metrics.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    assert(CERBERUS_SPI_COMMAND_WIFI_CHANNEL_SURVEY_START == 81);
    assert(CERBERUS_SPI_COMMAND_WIFI_CHANNEL_SURVEY_RESULT == 84);
    assert(sizeof(cerberus_wifi_channel_survey_status_payload_t) == 22U);
    assert(sizeof(cerberus_wifi_channel_survey_result_payload_t) == 22U);
    cerberus_wifi_channel_survey_result_payload_t result = {
        .generation = 4, .index = 2, .channel = 11,
        .dwell_ms = CERBERUS_WIFI_CHANNEL_SURVEY_DWELL_MS,
    };
    assert(cerberus_wifi_channel_survey_metrics_update(&result, 0, 0, -127, 100000U, 100));
    assert(result.seen == 1 && result.rate_per_sec == 0);
    assert(result.average_rssi == -127 && result.strongest_rssi == -127);
    assert(result.sample_count == 1 && result.updated_ms == 100);
    assert(cerberus_wifi_channel_survey_metrics_update(&result, 17, -1020, -42, 100000U, 1210));
    assert(result.last_packets == 17 && result.rate_per_sec == 170);
    assert(result.total_packets == 17 && result.average_rssi == -60);
    assert(result.strongest_rssi == -42 && result.sample_count == 2);
    assert(result.updated_ms == 1210 && result.channel == 11);
    result.total_packets = UINT32_MAX - 3U;
    result.sample_count = UINT16_MAX;
    assert(cerberus_wifi_channel_survey_metrics_update(&result, 100000U,
        -6000000LL, -30, 100000U, 2300));
    assert(result.total_packets == UINT32_MAX);
    assert(result.last_packets == UINT16_MAX && result.rate_per_sec == UINT16_MAX);
    assert(result.sample_count == UINT16_MAX && result.average_rssi == -60);
    assert(cerberus_wifi_channel_survey_metrics_update(&result, 17U, -1020, -42, 120000U, 3000));
    assert(result.rate_per_sec == 141U && result.dwell_ms == 120U);
    assert(cerberus_wifi_channel_survey_metrics_update(&result, 1U, -50, -50, 100001U, 3200));
    assert(result.rate_per_sec == 9U && result.dwell_ms == 101U);
    assert(cerberus_wifi_channel_survey_metrics_update(&result, UINT32_MAX,
        (int64_t)UINT32_MAX * -60, -40, 1U, 3300));
    assert(result.rate_per_sec == UINT16_MAX && result.dwell_ms == 1U);
    assert(cerberus_wifi_channel_survey_metrics_update(&result, 1U, -50, -50,
        (uint64_t)UINT16_MAX * 1000U, 3400));
    assert(result.rate_per_sec == 0U && result.dwell_ms == UINT16_MAX);
    cerberus_wifi_channel_survey_result_payload_t unchanged = result;
    assert(!cerberus_wifi_channel_survey_metrics_update(&result, 1U, -50, -50, 0U, 3500));
    assert(!cerberus_wifi_channel_survey_metrics_update(&result, 1U, -50, -50,
        (uint64_t)UINT16_MAX * 1000U + 1U, 3500));
    assert(!cerberus_wifi_channel_survey_metrics_update(NULL, 1U, -50, -50, 100000U, 3500));
    assert(!memcmp(&unchanged, &result, sizeof(result)));
    puts("channel survey protocol and dwell metrics: pass");
    return 0;
}
