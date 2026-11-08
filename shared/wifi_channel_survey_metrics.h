#pragma once

#include "cerberus_protocol.h"
#include <limits.h>

/* Completed passive receive window. Status carries the target dwell; each row
 * carries the actual duration, rounded up to milliseconds for presentation.
 * The rate uses microseconds. No frame bytes or identities are retained. */
static inline bool cerberus_wifi_channel_survey_metrics_update(
    cerberus_wifi_channel_survey_result_payload_t *result,
    uint32_t packets, int64_t rssi_sum, int8_t strongest,
    uint64_t receive_us, uint32_t updated_ms)
{
    if (!result || !receive_us || receive_us > (uint64_t)UINT16_MAX * 1000U)
        return false;
    result->seen = 1U;
    result->total_packets = UINT32_MAX - result->total_packets < packets
        ? UINT32_MAX : result->total_packets + packets;
    result->last_packets = packets > UINT16_MAX ? UINT16_MAX : (uint16_t)packets;
    uint64_t rate = (uint64_t)packets * 1000000U / receive_us;
    result->rate_per_sec = rate > UINT16_MAX ? UINT16_MAX : (uint16_t)rate;
    result->average_rssi = packets ? (int8_t)(rssi_sum / (int64_t)packets) : -127;
    result->strongest_rssi = packets ? strongest : -127;
    if (result->sample_count < UINT16_MAX) ++result->sample_count;
    result->updated_ms = updated_ms;
    result->dwell_ms = (uint16_t)((receive_us + 999U) / 1000U);
    return true;
}
