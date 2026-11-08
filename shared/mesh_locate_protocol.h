#pragma once
/* Original passive selected-source observation protocol. Multibyte fields are
 * little endian. A matching clear header is not authenticated device identity.
 * Capability72's Mesh survey ABI remains unchanged. */
#include <stdint.h>

#define CERBERUS_MESH_LOCATE_MIN_FIRMWARE 73U
#define CERBERUS_MESH_LOCATE_VERSION 1U
#define CERBERUS_MESH_LOCATE_HEADER_BYTES 72U
#define CERBERUS_MESH_LOCATE_MAC_BYTES 125U
#define CERBERUS_MESH_LOCATE_PROJECTION_BYTES 197U
#define CERBERUS_MESH_LOCATE_CHUNK_BYTES 9U
#define CERBERUS_MESH_LOCATE_PARTS 22U
#define CERBERUS_MESH_LOCATE_LEASE_MS 10000U
#define CERBERUS_MESH_LOCATE_RETENTION_MS 30000U
#define CERBERUS_MESH_LOCATE_SEQUENCE_MAX UINT32_C(0x7fffffff)
#define CERBERUS_MESH_LOCATE_PIN_BASE UINT32_C(0x80000000)
#define CERBERUS_MESH_LOCATE_COMMAND_INFO 154U
#define CERBERUS_MESH_LOCATE_COMMAND_START 155U
#define CERBERUS_MESH_LOCATE_COMMAND_STATUS 156U
#define CERBERUS_MESH_LOCATE_COMMAND_STOP 157U
#define CERBERUS_MESH_LOCATE_COMMAND_PEEK 158U
#define CERBERUS_MESH_LOCATE_COMMAND_READ 159U
#define CERBERUS_MESH_LOCATE_COMMAND_RELEASE 160U

enum { CERBERUS_MESH_LOCATE_IDLE=0, CERBERUS_MESH_LOCATE_QUEUED=1,
    CERBERUS_MESH_LOCATE_RECEIVING=2, CERBERUS_MESH_LOCATE_STOPPING=3,
    CERBERUS_MESH_LOCATE_COMPLETE=4, CERBERUS_MESH_LOCATE_ERROR=5 };
enum { CERBERUS_MESH_LOCATE_BOUND=1U, CERBERUS_MESH_LOCATE_HELD=2U,
    CERBERUS_MESH_LOCATE_RX_ACTIVE=4U, CERBERUS_MESH_LOCATE_DATA_READY=8U,
    CERBERUS_MESH_LOCATE_CLOCK_INVALID=16U, CERBERUS_MESH_LOCATE_SATURATED=32U,
    CERBERUS_MESH_LOCATE_EXHAUSTED=64U };
enum { CERBERUS_MESH_LOCATE_COPY_TIME_VALID=1U,
    CERBERUS_MESH_LOCATE_MATCH_TIME_VALID=2U,
    CERBERUS_MESH_LOCATE_RSSI_VALID=4U, CERBERUS_MESH_LOCATE_LQI_VALID=8U,
    CERBERUS_MESH_LOCATE_STOPPED=16U,
    CERBERUS_MESH_LOCATE_PROJECTION_CLOCK_INVALID=32U,
    CERBERUS_MESH_LOCATE_COUNTERS_SATURATED=64U,
    CERBERUS_MESH_LOCATE_SEQUENCE_EXHAUSTED=128U };

typedef struct __attribute__((packed)) {
    uint64_t session_id, address;
    uint16_t pan;
    uint8_t channel, address_mode, pan_known, version;
} cerberus_mesh_locate_start_t;
/* Fixed channel11..26; address_mode2=SHORT16 or3=EXT64. SHORT16 must fit16bits
 * and exclude0xfffe/0xffff. pan_known is0/1; unknown requires pan0 and matches
 * only an unknown source PAN, never a wildcard. Other reserved values reject.
 * START accepts only the next common IEEE session identity. Exact replay is
 * idempotent but never renews/rearms, including after retirement. */
typedef struct __attribute__((packed)) { uint64_t session_id; } cerberus_mesh_locate_request_t;
typedef struct __attribute__((packed)) {
    uint64_t session_id;
    uint32_t sequence;
    int32_t error_code;
    uint8_t state, flags, channel, version, projection_bytes, reserved;
} cerberus_mesh_locate_control_t;
/* Ordinary sequence is the latest match1..0x7fffffff, or0 before any match.
 * PEEK sequence is its immutable snapshot pin0x80000001..0xffffffff. Neither
 * wraps. BOUND+HELD persists through stopped retention/cleanup. RX_ACTIVE alone
 * says reception is active. DATA_READY says a projection can be pinned/read.
 * projection_bytes is72+latest raw length; PEEK returns its exact pinned size.
 * Unbound error replies contain requested session/error only. RELEASE success
 * contains requested session only. INFO unbound returns the latest common
 * boot-seeded session, version1, projection_bytes72 and otherwise zeros. When
 * another IEEE owner exists, INFO additionally returns HELD and QUEUED, without
 * BOUND. INFO never renews or stops. Seed uniqueness is not authentication.
 * Active context controls have version1, reserved0, projection_bytes72..197;
 * terminal controls have channel0. Successful PEEK matches its projection's
 * state/channel/source error. STATUS leaves a prior pin unchanged. */
typedef struct __attribute__((packed)) {
    uint8_t version, flags, state, channel;
    uint8_t address_mode, pan_known, mac_length, reserved0;
    uint16_t pan;
    int8_t rssi;
    uint8_t lqi;
    uint32_t match_sequence;
    uint64_t address, copied_us, last_match_us;
    uint32_t callbacks, invalid_metadata, malformed, unsupported;
    uint32_t filtered_nonmatch, matches;
    int32_t error_code;
    uint32_t reserved1;
    uint8_t mac[CERBERUS_MESH_LOCATE_MAC_BYTES];
} cerberus_mesh_locate_projection_t;
/* Only72+mac_length bytes are transferred. Before first match, mac_length,
 * match_sequence,matches,last_match_us,RSSI,LQI and their validity flags are0.
 * Thereafter MAC length2..125, match_sequence==matches, exact target fields and
 * a complete normalized latest source witness are retained in the same pin.
 * Invalid time/signal values are canonical0. MAC excludes PHY length and the
 * SDK's replacement RSSI/LQI footer (no retained FCS). Unused MAC tail is0.
 * callbacks = saturated_sum(invalid_metadata,malformed,unsupported,
 * filtered_nonmatch,matches). These count admitted SDK callbacks, not RF totals.
 * Coalesced matches are intentional omitted intermediate samples, not queue or
 * RF loss. No FIFO or ACK exists. Every new sample requires S3 raw/header source
 * validation; duplicate match sequences must preserve witness/signal/time.
 * copied_us and last_match_us share C5 uptime. An S3 timestamp taken BEFORE
 * PEEK can yield a conservative callback-age upper bound; receipt time alone
 * is not source freshness. STOPPED is terminal; final channel0, source error
 * preserved. Counter saturation/clock failure/identity exhaustion are explicit.
 */
typedef struct __attribute__((packed)) {
    uint64_t session_id;
    uint32_t pin;
    uint8_t part;
} cerberus_mesh_locate_read_t;
typedef struct __attribute__((packed)) {
    uint64_t session_id;
    uint32_t pin;
    uint8_t part, data[CERBERUS_MESH_LOCATE_CHUNK_BYTES];
} cerberus_mesh_locate_chunk_t;
/* INFO has no request; START22; STATUS/STOP/PEEK/RELEASE8; READ13. All replies22.
 * PEEK replaces only its own immutable pin. READ cannot alter pin or RF lease.
 * READ errors echo session/part, pin0, signed errorLE32 in data[0..3], tail0.
 * Success final-part padding is0; parts beyond exact pin length reject.
 * Valid STATUS renews active10s lease. After terminal state, valid STATUS/PEEK/
 * READ renew30s retention only after checking existing expiry. Expiry stops RF
 * and records timeout; worker retries cleanup without SPI traffic. Stop and
 * Release never rearm. Release may abandon a pin and finishes asynchronously.
 * A stopped final projection plus matching final STATUS and canonical released
 * response is required before S3 calls a run frozen/exportable. */
#ifdef __cplusplus
#define CERBERUS_LOCATE_ASSERT static_assert
#else
#define CERBERUS_LOCATE_ASSERT _Static_assert
#endif
CERBERUS_LOCATE_ASSERT(sizeof(cerberus_mesh_locate_start_t)==22U,"locate start ABI");
CERBERUS_LOCATE_ASSERT(sizeof(cerberus_mesh_locate_control_t)==22U,"locate control ABI");
CERBERUS_LOCATE_ASSERT(sizeof(cerberus_mesh_locate_projection_t)==197U,"locate projection ABI");
CERBERUS_LOCATE_ASSERT(sizeof(cerberus_mesh_locate_read_t)==13U,"locate read ABI");
CERBERUS_LOCATE_ASSERT(sizeof(cerberus_mesh_locate_chunk_t)==22U,"locate chunk ABI");
#undef CERBERUS_LOCATE_ASSERT
