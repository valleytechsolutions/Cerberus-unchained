#pragma once
/* Original receive-only selected AP/STA counter protocol. Integer fields are
 * little endian; clear MAC roles are observations, not authenticated identity. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "wifi_packet_count.h"

#define CERBERUS_PACKET_COUNT_MIN_FIRMWARE 74U
#define CERBERUS_PACKET_COUNT_VERSION 1U
#define CERBERUS_PACKET_COUNT_TARGETS 16U
#define CERBERUS_PACKET_COUNT_LEASE_MS 10000U
#define CERBERUS_PACKET_COUNT_RETENTION_MS 30000U
#define CERBERUS_PACKET_COUNT_CHUNK_BYTES 8U
#define CERBERUS_PACKET_COUNT_RAW_CHUNK_BYTES 40U
#define CERBERUS_PACKET_COUNT_RAW_TRANSFER_BYTES 64U
#define CERBERUS_PACKET_COUNT_ROW_BYTES 96U
#define CERBERUS_PACKET_COUNT_SUMMARY_BYTES 128U
#define CERBERUS_PACKET_COUNT_CHANNEL_BYTES 16U
#define CERBERUS_PACKET_COUNT_RAW_META_BYTES 28U
#define CERBERUS_PACKET_COUNT_PIN_BASE UINT32_C(0x80000000)
#define CERBERUS_PACKET_COUNT_SEQUENCE_MAX UINT32_C(0x7fffffff)
#define CERBERUS_PACKET_COUNT_COMMAND_INFO 161U
#define CERBERUS_PACKET_COUNT_COMMAND_BEGIN 162U
#define CERBERUS_PACKET_COUNT_COMMAND_TARGET 163U
#define CERBERUS_PACKET_COUNT_COMMAND_START 164U
#define CERBERUS_PACKET_COUNT_COMMAND_STATUS 165U
#define CERBERUS_PACKET_COUNT_COMMAND_STOP 166U
#define CERBERUS_PACKET_COUNT_COMMAND_PEEK 167U
#define CERBERUS_PACKET_COUNT_COMMAND_READ 168U
#define CERBERUS_PACKET_COUNT_COMMAND_RELEASE 169U
#define CERBERUS_PACKET_COUNT_COMMAND_RAW_READ 170U
#define CERBERUS_PACKET_COUNT_COMMAND_RAW_ACK 171U

enum { CERBERUS_PACKET_COUNT_IDLE=0, CERBERUS_PACKET_COUNT_STAGING=1,
    CERBERUS_PACKET_COUNT_QUEUED=2, CERBERUS_PACKET_COUNT_RUNNING=3,
    CERBERUS_PACKET_COUNT_STOPPING=4, CERBERUS_PACKET_COUNT_COMPLETE=5,
    CERBERUS_PACKET_COUNT_ERROR=6 };
enum { CERBERUS_PACKET_COUNT_BOUND=1U, CERBERUS_PACKET_COUNT_HELD=2U,
    CERBERUS_PACKET_COUNT_RX_ACTIVE=4U, CERBERUS_PACKET_COUNT_DATA_READY=8U,
    CERBERUS_PACKET_COUNT_CLOCK_INVALID=16U, CERBERUS_PACKET_COUNT_SATURATED=32U,
    CERBERUS_PACKET_COUNT_EXHAUSTED=64U, CERBERUS_PACKET_COUNT_RECORDING=128U };
enum { CERBERUS_PACKET_COUNT_READ_SUMMARY=0, CERBERUS_PACKET_COUNT_READ_ROW=1,
    CERBERUS_PACKET_COUNT_READ_CHANNEL=2, CERBERUS_PACKET_COUNT_READ_RAW_META=3 };

typedef struct __attribute__((packed)) {
    uint64_t session_id, channel_mask; /* canonical42-channel map, nonempty */
    uint8_t target_count;
    uint16_t dwell_ms; /*model dwell bounds; one channel means fixed */
    uint8_t record; /*exact0/1; optional two-slot4096B queue */
    uint16_t reserved; /*0; canonical22-byte BEGIN*/
} cerberus_packet_count_begin_t;
typedef struct __attribute__((packed)) {
    uint64_t session_id;
    uint8_t index, role, channel_hint, mac[6], reserved;
} cerberus_packet_count_target_t;
typedef struct __attribute__((packed)) { uint64_t session_id; } cerberus_packet_count_request_t;
typedef struct __attribute__((packed)) {
    uint64_t session_id;
    uint32_t sequence; /*model revision; PEEK pin; successful RAW_ACK sequence */
    int32_t error_code;
    uint16_t configured; /*uploaded target bitmap*/
    uint8_t state, flags, channel, count;
} cerberus_packet_count_control_t;
typedef struct __attribute__((packed)) {
    uint64_t session_id;
    uint8_t kind,index;
} cerberus_packet_count_peek_t;
typedef struct __attribute__((packed)) {
    uint64_t session_id;
    uint32_t pin;
    uint16_t part;
} cerberus_packet_count_read_t;
typedef struct __attribute__((packed)) {
    uint64_t session_id;
    uint32_t pin;
    uint16_t part;
    uint8_t data[CERBERUS_PACKET_COUNT_CHUNK_BYTES];
} cerberus_packet_count_chunk_t;
typedef struct __attribute__((packed)) { uint64_t session_id; uint32_t sequence; } cerberus_packet_count_ack_t;
typedef struct __attribute__((packed)) {
    uint64_t session_id; uint32_t pin; uint16_t part;
    uint8_t data[CERBERUS_PACKET_COUNT_RAW_CHUNK_BYTES];
} cerberus_packet_count_raw_chunk_t;
typedef struct __attribute__((packed)) {
    uint32_t magic; uint8_t protocol_version,command,sequence,payload_length;
    cerberus_packet_count_raw_chunk_t payload; uint16_t crc16;
} cerberus_packet_count_raw_frame_t;
static inline size_t cerberus_packet_count_request_bytes(uint8_t command)
{
    switch(command){
    case CERBERUS_PACKET_COUNT_COMMAND_INFO:return 0U;
    case CERBERUS_PACKET_COUNT_COMMAND_BEGIN:return sizeof(cerberus_packet_count_begin_t);
    case CERBERUS_PACKET_COUNT_COMMAND_TARGET:return sizeof(cerberus_packet_count_target_t);
    case CERBERUS_PACKET_COUNT_COMMAND_PEEK:return sizeof(cerberus_packet_count_peek_t);
    case CERBERUS_PACKET_COUNT_COMMAND_READ:case CERBERUS_PACKET_COUNT_COMMAND_RAW_READ:return sizeof(cerberus_packet_count_read_t);
    case CERBERUS_PACKET_COUNT_COMMAND_RAW_ACK:return sizeof(cerberus_packet_count_ack_t);
    case CERBERUS_PACKET_COUNT_COMMAND_START:case CERBERUS_PACKET_COUNT_COMMAND_STATUS:
    case CERBERUS_PACKET_COUNT_COMMAND_STOP:case CERBERUS_PACKET_COUNT_COMMAND_RELEASE:return sizeof(cerberus_packet_count_request_t);
    default:return SIZE_MAX;
    }
}

typedef struct {
    uint32_t accepted,acked,dropped_full,dropped_other,last_sequence;
    uint8_t pending;
} cerberus_packet_count_record_stats_t;
static inline void cerberus_packet_count_put(uint8_t *p,uint64_t v,unsigned n)
{ for(unsigned i=0;i<n;++i)p[i]=(uint8_t)(v>>(i*8U)); }
static inline uint64_t cerberus_packet_count_get(const uint8_t *p,unsigned n)
{ uint64_t v=0;for(unsigned i=0;i<n;++i)v|=(uint64_t)p[i]<<(i*8U);return v; }
/* Fieldwise projections never serialize native padding. All reserved bytes0.
 * Row: target0..7; nine counters8..43; first/last/lastTX44..67; RSSI68,
 * lastchannel69,rowflags70,reserved71; sourcecopy72,totalRXwindow80,
 * revision88,summaryflags92,reserved94..95. RXwindow is the sum of this
 * receiver's chosen-channel windows, not target airtime or RF coverage. */
static inline bool cerberus_packet_count_encode_row(const cerberus_wifi_packet_count_table_t *t,
    unsigned index,int64_t now,uint8_t out[CERBERUS_PACKET_COUNT_ROW_BYTES])
{
    cerberus_wifi_packet_count_row_t r;
    cerberus_wifi_packet_count_summary_t s;
    if(!out||!cerberus_wifi_packet_count_copy_row(t,index,&r)||
       !cerberus_wifi_packet_count_copy_summary(t,now,&s))return false;
    uint64_t total=0;
    for(unsigned i=0;i<CERBERUS_WIFI_PACKET_COUNT_CHANNELS;++i){uint64_t v=0;
        if(!cerberus_wifi_packet_count_copy_channel(t,i,now,&v))return false;
        total=UINT64_MAX-total<v?UINT64_MAX:total+v;}
    memset(out,0,CERBERUS_PACKET_COUNT_ROW_BYTES);
    memcpy(out,&r.target,8U);
    const uint32_t counts[]={r.observations,r.transmitted,r.addressed,r.bssid_context,
        r.management,r.data,r.retry,r.protected_frames,r.fragmented};
    for(unsigned i=0;i<9U;++i)cerberus_packet_count_put(out+8U+i*4U,counts[i],4U);
    cerberus_packet_count_put(out+44U,r.first_us,8U);cerberus_packet_count_put(out+52U,r.last_us,8U);
    cerberus_packet_count_put(out+60U,r.last_tx_us,8U);out[68]=(uint8_t)r.rssi;out[69]=r.last_channel;out[70]=r.flags;
    cerberus_packet_count_put(out+72U,s.copied_source_us,8U);cerberus_packet_count_put(out+80U,total,8U);
    cerberus_packet_count_put(out+88U,s.revision,4U);cerberus_packet_count_put(out+92U,s.flags,2U);return true;
}
/* Summary128: version/state/currentchannel/record0..3; model flags4,dwell6,
 * count8,rawpending9,reserved10; sourceerror12,revision16; nine counters20..55
 * (callbacks,invalid,malformed,unsupported,clock,outside,valid,matched,
 * channel_failures at52); times56/64/72,
 * requested/attempted/successful masks80/88/96; rawaccepted104,acked108,
 * dropfull112,dropother116,lastsequence120,configured124,reserved126. */
static inline bool cerberus_packet_count_encode_summary(const cerberus_wifi_packet_count_table_t *t,
    int64_t now,uint8_t state,uint8_t channel,uint8_t record,int32_t error,uint16_t configured,
    const cerberus_packet_count_record_stats_t *raw,uint8_t out[CERBERUS_PACKET_COUNT_SUMMARY_BYTES])
{
    cerberus_wifi_packet_count_summary_t s;
    if(!out||!raw||!cerberus_wifi_packet_count_copy_summary(t,now,&s))return false;
    memset(out,0,CERBERUS_PACKET_COUNT_SUMMARY_BYTES);
    out[0]=CERBERUS_PACKET_COUNT_VERSION;out[1]=state;out[2]=channel;out[3]=record;
    cerberus_packet_count_put(out+4U,s.flags,2U);cerberus_packet_count_put(out+6U,s.dwell_ms,2U);
    out[8]=s.count;out[9]=raw->pending;cerberus_packet_count_put(out+12U,(uint32_t)error,4U);
    cerberus_packet_count_put(out+16U,s.revision,4U);
    const uint32_t counts[]={s.callbacks,s.invalid_metadata,s.malformed_frames,s.unsupported_frames,
        s.clock_dropped_frames,s.outside_window_frames,s.valid_frames,s.matched_frames,s.channel_failures};
    for(unsigned i=0;i<9U;++i)cerberus_packet_count_put(out+20U+i*4U,counts[i],4U);
    cerberus_packet_count_put(out+56U,s.started_source_us,8U);cerberus_packet_count_put(out+64U,s.stopped_source_us,8U);
    cerberus_packet_count_put(out+72U,s.copied_source_us,8U);cerberus_packet_count_put(out+80U,s.requested_channels,8U);
    cerberus_packet_count_put(out+88U,s.attempted_channels,8U);cerberus_packet_count_put(out+96U,s.successful_channels,8U);
    cerberus_packet_count_put(out+104U,raw->accepted,4U);cerberus_packet_count_put(out+108U,raw->acked,4U);
    cerberus_packet_count_put(out+112U,raw->dropped_full,4U);cerberus_packet_count_put(out+116U,raw->dropped_other,4U);
    cerberus_packet_count_put(out+120U,raw->last_sequence,4U);cerberus_packet_count_put(out+124U,configured,2U);return true;
}
static inline bool cerberus_packet_count_encode_channel(const cerberus_wifi_packet_count_table_t *t,
    unsigned index,int64_t now,uint8_t out[CERBERUS_PACKET_COUNT_CHANNEL_BYTES])
{
    uint64_t v=0;cerberus_wifi_packet_count_summary_t s;
    if(!out||!cerberus_wifi_packet_count_copy_channel(t,index,now,&v)||
       !cerberus_wifi_packet_count_copy_summary(t,now,&s))return false;
    memset(out,0,CERBERUS_PACKET_COUNT_CHANNEL_BYTES);out[0]=cerberus_wifi_packet_count_channel(index);
    out[1]=(uint8_t)s.flags;cerberus_packet_count_put(out+4U,s.revision,4U);cerberus_packet_count_put(out+8U,v,8U);return true;
}

/* BEGIN accepts only INFO epoch+1, or exact retained replay without renewal.
 * TARGET is indexed/idempotent; conflicting content or duplicate role+MAC
 * rejects. Channel is an initial planning hint, never a target match filter.
 * START seals the complete target set. Replays never rearm or renew.
 * BOUND identifies retained context. HELD means native radio/staging/cleanup
 * reservation; terminal retained data has BOUND without HELD. Unbound failure
 * contains requested session/error only; successful RELEASE only session.
 * INFO without a context returns latest epoch and otherwise0; competing radio
 * reservations project HELD+QUEUED without BOUND, never ownership authority.
 * Valid STATUS renews active10s lease after expiry check. Stopped STATUS/PEEK/
 * READ renew only30s data retention. INFO/START/ACK/activeREAD do not renew RF.
 * Stop closes admission immediately; worker disables, drains, restores before
 * clearing HELD. Expiry records TIMEOUT, cannot resurrect or clear reservation.
 * Seed is a collision-risk discriminator, not eternal uniqueness/authentication.
 * PEEK freezes one metrics projection (odd highbit pin), independently of one
 * RAW_META projection (even highbit pin). Neither PEEK invalidates the other
 * consumer's slot. STATUS never invalidates either. Pins highbit set
 * and raw sequences1..0x7fffffff never wrap. READ preserves full session/pin/
 * part; errors pin0,errorLE32 in data0..3,tail0. Final padding0. RAW_READ uses
 * exact head sequence as pin; only RAW_ACK retires it, idempotently for lastACK.
 * Failed ACK sequence0. Final report requires terminal noHELD plus stable
 * revision/count/settings/summary and exact RELEASE; live rows sampled apart.
 * Optional raw queue is independent of counters. It contains complete matched
 * SDK MPDUs excludingFCS (at most4096B); queue/full/oversize losses explicit.
 * No serial transport is implied. No raw allocation in count-only mode. */
/* RAW_READ alone clocks64bytes: ordinary32B request (then zero padding),
 * response8B commonheader +54B rawchunk +CRC16 over first62B. The command is
 * read-only; a short transaction cannot ACK/pop. Rawpart units40B, finaltail0;
 * error pin0,errorLE32 first4data,rest0. Raw metadata28B: sequence0,captured4,
 * original6,callback_us8,CRC32ofcompleteframe16,channel20,RSSI21,flags22
 * (TIME_VALID1,RSSI_VALID2),reserved24..27. PEEK nohead rejects NOT_FOUND.
 * Raw metadata pins remain immutable even if ACK retires its queue record;
 * RAW_READ still requires that exact record to be currenthead. */

#ifdef __cplusplus
#define CERBERUS_PACKET_ASSERT static_assert
#else
#define CERBERUS_PACKET_ASSERT _Static_assert
#endif
CERBERUS_PACKET_ASSERT(sizeof(cerberus_packet_count_begin_t)==22U,"count BEGIN ABI");
CERBERUS_PACKET_ASSERT(sizeof(cerberus_packet_count_target_t)==18U,"count TARGET ABI");
CERBERUS_PACKET_ASSERT(sizeof(cerberus_packet_count_control_t)==22U,"count control ABI");
CERBERUS_PACKET_ASSERT(sizeof(cerberus_packet_count_read_t)==14U,"count READ ABI");
CERBERUS_PACKET_ASSERT(sizeof(cerberus_packet_count_chunk_t)==22U,"count chunk ABI");
CERBERUS_PACKET_ASSERT(sizeof(cerberus_packet_count_raw_chunk_t)==54U,"count raw ABI");
CERBERUS_PACKET_ASSERT(sizeof(cerberus_packet_count_raw_frame_t)==64U,"count raw frame ABI");
#undef CERBERUS_PACKET_ASSERT
