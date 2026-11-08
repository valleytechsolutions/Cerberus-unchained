#pragma once

#include "cerberus_protocol.h"

#define CERBERUS_WIFI_CHANNEL_ANALYZER_MIN_FIRMWARE 82U
#define CERBERUS_WIFI_CHANNEL_ANALYZER_TARGET_MS 100U
#define CERBERUS_WIFI_CHANNEL_ANALYZER_SAMPLES 64U
#define CERBERUS_WIFI_CHANNEL_ANALYZER_BOUND 1U
#define CERBERUS_WIFI_CHANNEL_ANALYZER_HELD 2U
#define CERBERUS_WIFI_CHANNEL_ANALYZER_CANCELLED 4U
#define CERBERUS_WIFI_CHANNEL_ANALYZER_SATURATED 1U
#define CERBERUS_WIFI_CHANNEL_ANALYZER_HINT_VALID 1U
#define CERBERUS_WIFI_CHANNEL_ANALYZER_HINT_BYTES 42U
#define CERBERUS_WIFI_CHANNEL_ANALYZER_HINT_PART_BYTES 9U
#define CERBERUS_WIFI_CHANNEL_ANALYZER_HINT_PARTS 5U

typedef struct __attribute__((packed)) {
    uint64_t session_id;
    uint8_t channel;
} cerberus_wifi_channel_analyzer_start_t;

/* INFO reports the shared Monitor/Analyzer last consumed session ID. HELD
 * means either mode occupies the receiver; only BOUND permits Analyzer use.
 * Status/Stop echo the requested ID. Unbound replies have no session fields. */
typedef struct __attribute__((packed)) {
    uint64_t session_id;
    uint32_t latest_sequence;
    int32_t error_code;
    uint16_t target_ms;
    uint8_t state, channel, stored_count, flags;
} cerberus_wifi_channel_analyzer_status_t;

typedef struct __attribute__((packed)) {
    uint64_t session_id;
    uint32_t sequence;
} cerberus_wifi_channel_analyzer_read_t;

/* SAMPLE and HINT_READ return this shorter payload on failure. key echoes
 * the requested sequence or projection; the SPI sequence binds the request. */
typedef struct __attribute__((packed)) {
    uint64_t session_id;
    uint32_t key;
    int32_t error_code;
} cerberus_wifi_channel_analyzer_read_error_t;

typedef struct __attribute__((packed)) {
    uint64_t session_id;
    uint32_t sequence, frames, window_us;
    uint8_t channel, flags;
} cerberus_wifi_channel_analyzer_sample_t;

/* PEEK pins one immutable 42-byte hint image. age_ms is measured at PEEK;
 * UINT32_MAX means unknown. A new PEEK supersedes the prior projection.
 * No hint yields NOT_FOUND with no projection. IDs never wrap within a session. */
typedef struct __attribute__((packed)) {
    uint64_t session_id;
    uint32_t projection, age_ms;
    int32_t error_code;
    uint8_t length, flags;
} cerberus_wifi_channel_analyzer_hint_status_t;

typedef struct __attribute__((packed)) {
    uint64_t session_id;
    uint32_t projection;
    uint8_t part;
} cerberus_wifi_channel_analyzer_hint_request_t;

typedef struct __attribute__((packed)) {
    uint64_t session_id;
    uint32_t projection;
    uint8_t part, bytes[CERBERUS_WIFI_CHANNEL_ANALYZER_HINT_PART_BYTES];
} cerberus_wifi_channel_analyzer_hint_part_t;

/* Chunk payload image. Unused SSID bytes and final chunk padding are zero. */
typedef struct __attribute__((packed)) {
    uint8_t bssid[6], ssid[32], ssid_length;
    int8_t rssi;
    uint8_t channel, flags;
} cerberus_wifi_channel_analyzer_hint_t;

_Static_assert(sizeof(cerberus_wifi_channel_analyzer_start_t)==9U,"Analyzer start wire size");
_Static_assert(sizeof(cerberus_wifi_channel_analyzer_status_t)==22U,"Analyzer status wire size");
_Static_assert(sizeof(cerberus_wifi_channel_analyzer_sample_t)==22U,"Analyzer sample wire size");
_Static_assert(sizeof(cerberus_wifi_channel_analyzer_read_t)==12U,"Analyzer read wire size");
_Static_assert(sizeof(cerberus_wifi_channel_analyzer_read_error_t)==16U,"Analyzer read error wire size");
_Static_assert(sizeof(cerberus_wifi_channel_analyzer_hint_status_t)==22U,"Analyzer hint status wire size");
_Static_assert(sizeof(cerberus_wifi_channel_analyzer_hint_request_t)==13U,"Analyzer hint request wire size");
_Static_assert(sizeof(cerberus_wifi_channel_analyzer_hint_part_t)==22U,"Analyzer hint part wire size");
_Static_assert(sizeof(cerberus_wifi_channel_analyzer_hint_t)==42U,"Analyzer hint image size");
