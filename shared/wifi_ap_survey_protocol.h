#pragma once
/* Passive AP/client survey. Little-endian copied evidence, no RF operations. */
#include "wifi_ap_survey.h"
#include <string.h>

#define CERBERUS_AP_SURVEY_MIN_FIRMWARE 90U
#define CERBERUS_AP_SURVEY_LEASE_MS 10000U
#define CERBERUS_AP_SURVEY_RETENTION_MS 30000U
#define CERBERUS_AP_SURVEY_CHUNK_BYTES 40U
#define CERBERUS_AP_SURVEY_TRANSFER_BYTES 64U
#define CERBERUS_AP_SURVEY_COMMAND_INFO 208U
#define CERBERUS_AP_SURVEY_COMMAND_START 209U
#define CERBERUS_AP_SURVEY_COMMAND_STATUS 210U
#define CERBERUS_AP_SURVEY_COMMAND_RENEW 211U
#define CERBERUS_AP_SURVEY_COMMAND_STOP 212U
#define CERBERUS_AP_SURVEY_COMMAND_RELEASE 213U
#define CERBERUS_AP_SURVEY_COMMAND_SNAPSHOT_BEGIN 214U
#define CERBERUS_AP_SURVEY_COMMAND_SNAPSHOT_READ 215U

enum { CERBERUS_AP_SURVEY_IDLE, CERBERUS_AP_SURVEY_QUEUED,
    CERBERUS_AP_SURVEY_RUNNING, CERBERUS_AP_SURVEY_STOPPING,
    CERBERUS_AP_SURVEY_COMPLETE, CERBERUS_AP_SURVEY_ERROR };
enum { CERBERUS_AP_SURVEY_BOUND=1U, CERBERUS_AP_SURVEY_HELD=2U,
    CERBERUS_AP_SURVEY_RECEIVING=4U, CERBERUS_AP_SURVEY_DATA_READY=8U };

typedef struct __attribute__((packed)) {
    uint64_t session_id, channel_mask;
    uint16_t dwell_ms, reserved;
} cerberus_ap_survey_start_t;
typedef struct __attribute__((packed)) { uint64_t session_id; } cerberus_ap_survey_request_t;
typedef struct __attribute__((packed)) {
    uint64_t session_id;
    int32_t error_code;
    uint32_t revision;
    uint8_t state, flags, channel, ap_count, relation_count, reserved;
} cerberus_ap_survey_control_t;
typedef struct __attribute__((packed)) {
    uint64_t session_id;
    uint32_t projection;
    int32_t error_code;
    uint16_t length;
    uint8_t ap_count, relation_count, flags, reserved;
} cerberus_ap_survey_projection_t;
typedef struct __attribute__((packed)) {
    uint64_t session_id;
    uint32_t projection;
    uint16_t offset;
} cerberus_ap_survey_read_t;
typedef struct __attribute__((packed)) {
    uint64_t session_id;
    uint32_t projection;
    uint16_t offset;
    uint8_t data[CERBERUS_AP_SURVEY_CHUNK_BYTES];
} cerberus_ap_survey_chunk_t;
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t protocol_version, command, sequence, payload_length;
    cerberus_ap_survey_chunk_t payload;
    uint16_t crc16;
} cerberus_ap_survey_frame_t;

/* INFO returns last consumed epoch; START accepts exactly epoch+1. Accepted
 * identical START retries return that session without rearming or renewing,
 * including terminal state. Conflicts reject. INFO may report HELD without
 * BOUND for a competing owner; it grants no authority over that owner.
 * Only RENEW extends the active 10-second lease, after checking expiry.
 * STOP closes admission; RELEASE also requests disposal after SDK cleanup and
 * callback drain. Failed cleanup retains HELD. Exact released RELEASE retries
 * succeed. Unbound failures contain only requested session and error.
 * STATUS/RENEW/BEGIN/READ extend only terminal data retention (30 seconds).
 * BEGIN always makes a fresh immutable 3816-byte image without stopping RF.
 * READ has exact session/projection/40-aligned offset; final padding is zero.
 * READ failure has requested session/offset, projection0, signed error LE in
 * data[0..3], all remaining bytes zero. Projection IDs never wrap/reuse.
 * Snapshot flags are the pure collector flags; control flags are distinct. */
static inline size_t cerberus_ap_survey_request_bytes(uint8_t command)
{
    switch(command) {
    case CERBERUS_AP_SURVEY_COMMAND_INFO: return 0U;
    case CERBERUS_AP_SURVEY_COMMAND_START: return sizeof(cerberus_ap_survey_start_t);
    case CERBERUS_AP_SURVEY_COMMAND_STATUS:
    case CERBERUS_AP_SURVEY_COMMAND_RENEW:
    case CERBERUS_AP_SURVEY_COMMAND_STOP:
    case CERBERUS_AP_SURVEY_COMMAND_RELEASE:
    case CERBERUS_AP_SURVEY_COMMAND_SNAPSHOT_BEGIN: return sizeof(cerberus_ap_survey_request_t);
    case CERBERUS_AP_SURVEY_COMMAND_SNAPSHOT_READ: return sizeof(cerberus_ap_survey_read_t);
    default: return SIZE_MAX;
    }
}
static inline size_t cerberus_ap_survey_reply_bytes(uint8_t command)
{
    if(cerberus_ap_survey_request_bytes(command)==SIZE_MAX)return SIZE_MAX;
    return command==CERBERUS_AP_SURVEY_COMMAND_SNAPSHOT_READ ?
        sizeof(cerberus_ap_survey_chunk_t):sizeof(cerberus_ap_survey_control_t);
}
static inline void cerberus_ap_survey_read_error(cerberus_ap_survey_chunk_t *out,
    uint64_t session,uint16_t offset,int32_t error)
{
    memset(out,0,sizeof(*out));out->session_id=session;out->offset=offset;
    memcpy(out->data,&error,sizeof(error));
}

static inline bool cerberus_ap_survey_start_valid(const cerberus_ap_survey_start_t *request)
{
    return request && request->session_id && request->channel_mask &&
        !(request->channel_mask & ~((UINT64_C(1) << 42U) - 1U)) &&
        request->dwell_ms == CERBERUS_AP_SURVEY_DWELL_MS && !request->reserved;
}

static inline bool cerberus_ap_survey_control_valid(const cerberus_ap_survey_control_t *reply,
    bool info, uint64_t expected_session)
{
    if (!reply || !reply->session_id || (!info && reply->session_id != expected_session) ||
        reply->reserved || reply->state > CERBERUS_AP_SURVEY_ERROR || (reply->flags & ~15U) ||
        reply->ap_count > CERBERUS_AP_SURVEY_AP_CAPACITY ||
        reply->relation_count > CERBERUS_AP_SURVEY_RELATION_CAPACITY ||
        (reply->channel && !cerberus_wifi_relation_channel_valid(reply->channel))) return false;
    if (!(reply->flags & CERBERUS_AP_SURVEY_BOUND)) {
        if (reply->revision || reply->channel || reply->ap_count || reply->relation_count) return false;
        if (info && reply->flags == CERBERUS_AP_SURVEY_HELD)
            return reply->state == CERBERUS_AP_SURVEY_QUEUED;
        return !reply->flags && reply->state == CERBERUS_AP_SURVEY_IDLE;
    }
    if (reply->state == CERBERUS_AP_SURVEY_IDLE ||
        (!!(reply->flags & CERBERUS_AP_SURVEY_DATA_READY) != !!reply->revision)) return false;
    if (!(reply->flags & CERBERUS_AP_SURVEY_DATA_READY) &&
        (reply->channel || reply->ap_count || reply->relation_count)) return false;
    if (!(reply->flags & CERBERUS_AP_SURVEY_HELD) &&
        reply->state != CERBERUS_AP_SURVEY_COMPLETE && reply->state != CERBERUS_AP_SURVEY_ERROR) return false;
    if ((reply->flags & CERBERUS_AP_SURVEY_HELD) && reply->state == CERBERUS_AP_SURVEY_COMPLETE) return false;
    if (reply->flags & CERBERUS_AP_SURVEY_RECEIVING)
        return (reply->flags & CERBERUS_AP_SURVEY_HELD) &&
            (reply->flags & CERBERUS_AP_SURVEY_DATA_READY) &&
            reply->state == CERBERUS_AP_SURVEY_RUNNING && reply->channel;
    return true;
}

static inline bool cerberus_ap_survey_projection_valid(const cerberus_ap_survey_projection_t *reply,
    uint64_t expected_session)
{
    if (!reply || !expected_session || reply->session_id != expected_session || reply->reserved) return false;
    if (reply->error_code)
        return !reply->projection && !reply->length && !reply->ap_count && !reply->relation_count && !reply->flags;
    return reply->projection && reply->length == sizeof(cerberus_ap_survey_snapshot_t) &&
        reply->ap_count <= CERBERUS_AP_SURVEY_AP_CAPACITY &&
        reply->relation_count <= CERBERUS_AP_SURVEY_RELATION_CAPACITY && !(reply->flags & ~7U) &&
        !((reply->flags & CERBERUS_AP_SURVEY_SNAPSHOT_RECEIVING) &&
          (reply->flags & CERBERUS_AP_SURVEY_SNAPSHOT_EXHAUSTED));
}
#ifdef __cplusplus
#define CERBERUS_AP_SURVEY_ASSERT static_assert
#else
#define CERBERUS_AP_SURVEY_ASSERT _Static_assert
#endif
CERBERUS_AP_SURVEY_ASSERT(sizeof(cerberus_ap_survey_start_t)==20U,"survey start ABI");
CERBERUS_AP_SURVEY_ASSERT(sizeof(cerberus_ap_survey_control_t)==22U,"survey control ABI");
CERBERUS_AP_SURVEY_ASSERT(sizeof(cerberus_ap_survey_projection_t)==22U,"survey projection ABI");
CERBERUS_AP_SURVEY_ASSERT(sizeof(cerberus_ap_survey_read_t)==14U,"survey read ABI");
CERBERUS_AP_SURVEY_ASSERT(sizeof(cerberus_ap_survey_frame_t)==64U,"survey frame ABI");
#undef CERBERUS_AP_SURVEY_ASSERT
