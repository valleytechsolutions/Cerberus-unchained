#pragma once
/* Passive IEEE 802.15.4 observations. All multibyte wire fields are little endian.
 * No field authenticates an endpoint, identifies an application or proves a route. */
#include <stdint.h>

#define CERBERUS_MESH_OBSERVATION_MIN_FIRMWARE 72U
#define CERBERUS_MESH_OBSERVATION_VERSION 1U
#define CERBERUS_MESH_OBSERVATION_CAPACITY 16U
#define CERBERUS_MESH_OBSERVATION_MAC_BYTES 125U
#define CERBERUS_MESH_OBSERVATION_RECORD_BYTES 139U
#define CERBERUS_MESH_OBSERVATION_SUMMARY_BYTES 128U
#define CERBERUS_MESH_OBSERVATION_CHUNK_BYTES 9U
#define CERBERUS_MESH_OBSERVATION_RECORD_PARTS 16U
#define CERBERUS_MESH_OBSERVATION_SUMMARY_PARTS 15U
#define CERBERUS_MESH_OBSERVATION_LEASE_MS 10000U
#define CERBERUS_MESH_OBSERVATION_RETENTION_MS 30000U
#define CERBERUS_MESH_OBSERVATION_SEQUENCE_MAX UINT32_C(0x7fffffff)
#define CERBERUS_MESH_OBSERVATION_SUMMARY_BASE UINT32_C(0x80000000)
#define CERBERUS_MESH_OBSERVATION_COMMAND_INFO 146U
#define CERBERUS_MESH_OBSERVATION_COMMAND_START 147U
#define CERBERUS_MESH_OBSERVATION_COMMAND_STATUS 148U
#define CERBERUS_MESH_OBSERVATION_COMMAND_STOP 149U
#define CERBERUS_MESH_OBSERVATION_COMMAND_PEEK 150U
#define CERBERUS_MESH_OBSERVATION_COMMAND_READ 151U
#define CERBERUS_MESH_OBSERVATION_COMMAND_ACK 152U
#define CERBERUS_MESH_OBSERVATION_COMMAND_RELEASE 153U

enum { CERBERUS_MESH_OBSERVATION_FIXED=0, CERBERUS_MESH_OBSERVATION_SWEEP=1 };
enum { CERBERUS_MESH_OBSERVATION_IDLE=0, CERBERUS_MESH_OBSERVATION_QUEUED=1,
    CERBERUS_MESH_OBSERVATION_RECEIVING=2, CERBERUS_MESH_OBSERVATION_STOPPING=3,
    CERBERUS_MESH_OBSERVATION_COMPLETE=4, CERBERUS_MESH_OBSERVATION_ERROR=5 };
enum { CERBERUS_MESH_OBSERVATION_BOUND=1U, CERBERUS_MESH_OBSERVATION_HELD=2U,
    CERBERUS_MESH_OBSERVATION_RX_ACTIVE=4U, CERBERUS_MESH_OBSERVATION_DATA_READY=8U,
    CERBERUS_MESH_OBSERVATION_CLOCK_INVALID=16U, CERBERUS_MESH_OBSERVATION_SATURATED=32U,
    CERBERUS_MESH_OBSERVATION_EXHAUSTED=64U };
enum { CERBERUS_MESH_OBSERVATION_TIME_VALID=1U, CERBERUS_MESH_OBSERVATION_RSSI_VALID=2U,
    CERBERUS_MESH_OBSERVATION_LQI_VALID=4U };
enum { CERBERUS_MESH_OBSERVATION_START_TIME_VALID=1U,
    CERBERUS_MESH_OBSERVATION_STOP_TIME_VALID=2U,
    CERBERUS_MESH_OBSERVATION_COPY_TIME_VALID=4U,
    CERBERUS_MESH_OBSERVATION_STOPPED=8U,
    CERBERUS_MESH_OBSERVATION_SUMMARY_CLOCK_INVALID=16U,
    CERBERUS_MESH_OBSERVATION_COUNTERS_SATURATED=32U,
    CERBERUS_MESH_OBSERVATION_SEQUENCE_EXHAUSTED=64U };
enum { CERBERUS_MESH_OBSERVATION_ROW=0, CERBERUS_MESH_OBSERVATION_SUMMARY=1 };

typedef struct __attribute__((packed)) {
    uint64_t session_id;
    uint16_t channel_mask; /* bit0 channel11 through bit15 channel26. */
    uint16_t dwell_ms;     /* 100..2000. Fixed requires exactly one mask bit. */
    uint8_t mode;
} cerberus_mesh_observation_start_t;
typedef struct __attribute__((packed)) { uint64_t session_id; } cerberus_mesh_observation_request_t;
typedef struct __attribute__((packed)) {
    uint64_t session_id;
    uint32_t sequence;
    int32_t error_code;
    uint8_t state,flags,count,capacity,channel,version;
} cerberus_mesh_observation_control_t;
/* BOUND+HELD remains set while the optional context exists, including stopped
 * queue drain. RX_ACTIVE alone indicates current reception. DATA_READY means
 * the context can be read, not that a live summary is final. Sequence is last
 * enqueued for ordinary controls, selected head/pin for PEEK and echoed for ACK.
 * Unbound rejection: requested session/error only. Successful RELEASE: session
 * only. INFO unbound: latest boot-seeded session, capacity16/version1; HELD is
 * additionally set when a legacy IEEE worker owns the radio (state QUEUED,
 * sequence/count/channel zero). INFO never renews or cancels. Seed uniqueness
 * across power cycles is probabilistic, not an authentication guarantee. */
typedef struct __attribute__((packed)) {
    uint64_t callback_us;
    uint8_t channel;
    int8_t rssi;
    uint8_t lqi,mac_length,flags,reserved;
    uint8_t mac[CERBERUS_MESH_OBSERVATION_MAC_BYTES];
} cerberus_mesh_observation_record_t;
/* Complete normalized MAC bytes only, length2..125, no SDK PHY length/footer.
 * Unused tail/reserved are zero. TIME_VALID clear requires callback_us zero.
 * RSSI/LQI describe this delivered frame; no sender authentication is implied. */
typedef struct __attribute__((packed)) {
    uint8_t version,flags,mode,channel;
    uint16_t requested_mask,tuned_mask,dwell_ms,reserved;
    uint64_t started_us,stopped_us,copied_us;
    uint32_t callbacks,invalid_callbacks,queue_drops,enqueued,acked,last_sequence;
    uint32_t completed_dwell_ms[16];
    int32_t error_code;
} cerberus_mesh_observation_summary_t;
/* Counters describe SDK-delivered/admitted work only, saturated with a flag.
 * tuned_mask means successful receive setup, not complete RF coverage. Dwell
 * counts completed windows using the C5 monotonic clock, never UTC/S3 time. */
typedef struct __attribute__((packed)) {
    uint64_t session_id;
    uint8_t kind;
} cerberus_mesh_observation_peek_t;
typedef struct __attribute__((packed)) {
    uint64_t session_id;
    uint32_t sequence;
    uint8_t kind,part;
} cerberus_mesh_observation_read_t;
typedef struct __attribute__((packed)) {
    uint64_t session_id;
    uint32_t sequence;
} cerberus_mesh_observation_ack_t;
typedef struct __attribute__((packed)) {
    uint64_t session_id;
    uint32_t sequence;
    uint8_t part,data[CERBERUS_MESH_OBSERVATION_CHUNK_BYTES];
} cerberus_mesh_observation_chunk_t;
/* ROW sequence1..0x7fffffff; SUMMARY pin0x80000001..0xffffffff. Never wrap.
 * Head cannot change before exact ACK. Repeated ACK of last retired head is
 * idempotent, all other stale ACKs fail. PEEK ROW empty succeeds sequence0.
 * PEEK SUMMARY replaces only the summary pin. STATUS preserves both pins.
 * READ errors echo session/part, sequence0, signed error LE32 in data[0..3],
 * remainingzero; success last-part paddingzero. READ never renews RF lease.
 * Valid STATUS renews active10s lease; stopped valid STATUS/PEEK/READ/ACK renew
 * only30s data retention, strictly after checking existing expiry. Stop and
 * Release never rearm; Release can abandon unread records and is asynchronous.
 * Replayed START has no renewal/rearm effect, including after retirement. */
#ifdef __cplusplus
#define CERBERUS_MESH_ASSERT static_assert
#else
#define CERBERUS_MESH_ASSERT _Static_assert
#endif
CERBERUS_MESH_ASSERT(sizeof(cerberus_mesh_observation_start_t)==13U,"mesh start ABI");
CERBERUS_MESH_ASSERT(sizeof(cerberus_mesh_observation_control_t)==22U,"mesh control ABI");
CERBERUS_MESH_ASSERT(sizeof(cerberus_mesh_observation_record_t)==139U,"mesh record ABI");
CERBERUS_MESH_ASSERT(sizeof(cerberus_mesh_observation_summary_t)==128U,"mesh summary ABI");
CERBERUS_MESH_ASSERT(sizeof(cerberus_mesh_observation_chunk_t)==22U,"mesh chunk ABI");
#undef CERBERUS_MESH_ASSERT
