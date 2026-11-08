#pragma once

#include "cerberus_protocol.h"

#define CERBERUS_WIFI_PACKET_MONITOR_SESSION_MIN_FIRMWARE 78U
#define CERBERUS_WIFI_PACKET_MONITOR_LEASE_MS 10000U
#define CERBERUS_WIFI_PACKET_MONITOR_BOUND 1U
#define CERBERUS_WIFI_PACKET_MONITOR_HELD 2U
#define CERBERUS_WIFI_PACKET_MONITOR_CANCELLED 4U
#define CERBERUS_WIFI_PACKET_MONITOR_RECORDING 8U

typedef struct __attribute__((packed)) {
    uint64_t session_id;
    uint8_t channel;
} cerberus_wifi_packet_monitor_session_start_t;

/* INFO returns the last consumed ID. A new Start must use exactly ID + 1.
 * Control replies echo the requested ID; only BOUND grants read/stop authority.
 * A rejected or retired request has no current-session fields. */
typedef struct __attribute__((packed)) {
    uint64_t session_id;
    uint32_t elapsed_ms;
    int32_t error_code;
    uint16_t latest_sequence;
    uint8_t state, channel, stored_count, flags;
} cerberus_wifi_packet_monitor_session_status_t;

typedef struct __attribute__((packed)) {
    uint64_t session_id;
    uint16_t sequence;
} cerberus_wifi_packet_monitor_session_read_t;

typedef struct __attribute__((packed)) {
    uint64_t session_id;
    cerberus_wifi_packet_monitor_sample_payload_t sample;
} cerberus_wifi_packet_monitor_session_sample_t;

_Static_assert(sizeof(cerberus_wifi_packet_monitor_session_start_t) == 9U,
    "monitor Start wire size");
_Static_assert(sizeof(cerberus_wifi_packet_monitor_session_status_t) == 22U,
    "monitor status must fit one SPI frame");
_Static_assert(sizeof(cerberus_wifi_packet_monitor_session_read_t) == 10U,
    "monitor sample request wire size");
_Static_assert(sizeof(cerberus_wifi_packet_monitor_session_sample_t) == 22U,
    "monitor sample must fit one SPI frame");
