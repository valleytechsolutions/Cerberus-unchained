#pragma once
/* Receive-only Probe Sniff contract (capability71).
 * Receive-only inventory, not a live association/physical-device identity API. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CERBERUS_PROBE_SNIFF_MIN_FIRMWARE 71U
#define CERBERUS_PROBE_SNIFF_CAPACITY 32U
#define CERBERUS_PROBE_SNIFF_CHANNEL_COUNT 42U
#define CERBERUS_PROBE_SNIFF_MAX_FRAME_BYTES 2304U
#define CERBERUS_PROBE_SNIFF_LEASE_MS 10000U
#define CERBERUS_PROBE_SNIFF_RETENTION_MS 30000U
#define CERBERUS_PROBE_SNIFF_DWELL_MS 250U
#define CERBERUS_PROBE_SNIFF_CHUNK_BYTES 9U
#define CERBERUS_PROBE_SNIFF_ROW_BYTES 68U
#define CERBERUS_PROBE_SNIFF_PROJECTION_BYTES 76U
#define CERBERUS_PROBE_SNIFF_SUMMARY_BYTES 84U
#define CERBERUS_PROBE_SNIFF_ROW_PARTS 9U
#define CERBERUS_PROBE_SNIFF_SUMMARY_PARTS 10U
/* Scoped command IDs140..145; legacy Wardrive probe commands are unchanged. */
#define CERBERUS_PROBE_SNIFF_COMMAND_INFO 140U
#define CERBERUS_PROBE_SNIFF_COMMAND_START 141U
#define CERBERUS_PROBE_SNIFF_COMMAND_STATUS 142U
#define CERBERUS_PROBE_SNIFF_COMMAND_STOP 143U
#define CERBERUS_PROBE_SNIFF_COMMAND_READ 144U
#define CERBERUS_PROBE_SNIFF_COMMAND_RELEASE 145U

enum { CERBERUS_PROBE_SNIFF_FIXED=0, CERBERUS_PROBE_SNIFF_HOP_24=1,
    CERBERUS_PROBE_SNIFF_HOP_5=2, CERBERUS_PROBE_SNIFF_HOP_BOTH=3 };
enum { CERBERUS_PROBE_SNIFF_IDLE=0, CERBERUS_PROBE_SNIFF_QUEUED=1,
    CERBERUS_PROBE_SNIFF_RUNNING=2, CERBERUS_PROBE_SNIFF_STOPPING=3,
    CERBERUS_PROBE_SNIFF_COMPLETE=4, CERBERUS_PROBE_SNIFF_ERROR=5 };
enum { CERBERUS_PROBE_SNIFF_BOUND=1U, CERBERUS_PROBE_SNIFF_OWNED=2U,
    CERBERUS_PROBE_SNIFF_RECEIVING=4U, CERBERUS_PROBE_SNIFF_DATA_READY=8U,
    CERBERUS_PROBE_SNIFF_OVERFLOW=16U, CERBERUS_PROBE_SNIFF_SATURATED=32U };
enum { CERBERUS_PROBE_SNIFF_ROW_RETRY=1U, CERBERUS_PROBE_SNIFF_ROW_SATURATED=2U,
    CERBERUS_PROBE_SNIFF_ROW_COPY_TIME_VALID=4U };
enum { CERBERUS_PROBE_SNIFF_START_TIME_VALID=1U, CERBERUS_PROBE_SNIFF_STOP_TIME_VALID=2U,
    CERBERUS_PROBE_SNIFF_COPY_TIME_VALID=4U, CERBERUS_PROBE_SNIFF_STOPPED=8U,
    CERBERUS_PROBE_SNIFF_COUNTERS_SATURATED=16U, CERBERUS_PROBE_SNIFF_REVISION_EXHAUSTED=32U };
enum { CERBERUS_PROBE_SNIFF_READ_SUMMARY=0U, CERBERUS_PROBE_SNIFF_READ_ROW=1U };

typedef struct __attribute__((packed)) {
    uint64_t session_id;
    uint8_t mode,channel; /* FIXED requires valid channel; hop modes require0. */
} cerberus_probe_sniff_start_t;
typedef struct __attribute__((packed)) { uint64_t session_id; } cerberus_probe_sniff_request_t;
typedef struct __attribute__((packed)) {
    uint64_t session_id;
    int32_t error_code;
    uint32_t revision;
    uint8_t state,flags,count,mode,channel,capacity;
} cerberus_probe_sniff_control_t;
/* Full requested token on every reply. Canonical unaccepted rejection has only
 * session/error; INFO reports current epoch and any actual retained job. */

typedef struct __attribute__((packed)) {
    uint8_t transmitter[6]; /* 802.11/display byte order. No address-type inference. */
    uint8_t ssid[32];
    uint8_t ssid_length,last_channel;
    int8_t last_rssi;
    uint8_t flags;
    uint64_t first_callback_us,last_callback_us;
    uint32_t frames,retry_frames; /* Callback observations, including repeats. */
    uint16_t last_sequence_control;
} cerberus_probe_sniff_row_t;
typedef struct __attribute__((packed)) {
    cerberus_probe_sniff_row_t row;
    uint64_t copied_source_us; /* Valid iff row.COPY_TIME_VALID;0 can be valid. */
} cerberus_probe_sniff_projection_t;
typedef struct __attribute__((packed)) {
    uint64_t started_source_us,stopped_source_us,copied_source_us;
    uint32_t valid_frames,retained_frames,capacity_dropped_frames,retry_frames;
    uint32_t malformed_frames,unsupported_frames,clock_dropped_frames;
    uint64_t attempted_channels,successful_channels; /* Canonical42-channel map. */
    uint32_t channel_failures,revision;
    uint16_t dwell_ms,flags;
    uint8_t count,capacity,reserved[2];
} cerberus_probe_sniff_summary_t;
/* Stable first32 exact transmitter+length+SSID keys; no eviction. A wildcard is
 * length0, not the literal '<hidden>'. Binary bytes and NUL are significant.
 * Last channel/RSSI/time come from the same admitted callback; channel masks
 * describe successful/attempted tuning, not complete RF reception coverage. */

typedef struct __attribute__((packed)) {
    uint64_t session_id;
    uint32_t pin;
    uint8_t kind,index,part;
} cerberus_probe_sniff_read_t;
typedef struct __attribute__((packed)) {
    uint64_t session_id;
    uint32_t pin;
    uint8_t part,data[CERBERUS_PROBE_SNIFF_CHUNK_BYTES];
} cerberus_probe_sniff_chunk_t;
/* Part0+pin0 captures one immutable row/summary; continuations require full
 * session/pin/kind/index. No READ renews RF. Stopped STATUS/READ refreshes only
 * the30s data-retention deadline. Error echoes session/part, pin0, signed error
 * LE in data[0..3], remainingzero. Tail padding zero. Pins never wrap. */

#ifdef __cplusplus
#define CERBERUS_PROBE_SNIFF_ASSERT static_assert
#else
#define CERBERUS_PROBE_SNIFF_ASSERT _Static_assert
#endif
CERBERUS_PROBE_SNIFF_ASSERT(sizeof(cerberus_probe_sniff_start_t)==10U,"probe start ABI");
CERBERUS_PROBE_SNIFF_ASSERT(sizeof(cerberus_probe_sniff_control_t)==22U,"probe control ABI");
CERBERUS_PROBE_SNIFF_ASSERT(sizeof(cerberus_probe_sniff_row_t)==68U,"probe row ABI");
CERBERUS_PROBE_SNIFF_ASSERT(sizeof(cerberus_probe_sniff_projection_t)==76U,"probe projection ABI");
CERBERUS_PROBE_SNIFF_ASSERT(sizeof(cerberus_probe_sniff_summary_t)==84U,"probe summary ABI");
CERBERUS_PROBE_SNIFF_ASSERT(sizeof(cerberus_probe_sniff_read_t)==15U,"probe read ABI");
CERBERUS_PROBE_SNIFF_ASSERT(sizeof(cerberus_probe_sniff_chunk_t)==22U,"probe chunk ABI");

#undef CERBERUS_PROBE_SNIFF_ASSERT
