#pragma once
/* Passive observations, not an inventory: equal identities are never merged.
 * One producer lock serializes the ring. Transport ACK removes only the exact
 * head sequence; retrying READ/ACK cannot append a second copy of a radio event. */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "cerberus_protocol.h"

enum { CERBERUS_WD_COMBINED=0, CERBERUS_WD_WIFI=1, CERBERUS_WD_BLE=2 };
/* Keep every command in the common registry. The old 172-176 assignments
 * collided with Packet Monitor before its dispatcher could inspect them. */
#define CERBERUS_WD_START CERBERUS_SPI_COMMAND_WARDRIVE_START
#define CERBERUS_WD_STATUS CERBERUS_SPI_COMMAND_WARDRIVE_STATUS
#define CERBERUS_WD_READ CERBERUS_SPI_COMMAND_WARDRIVE_READ
#define CERBERUS_WD_STOP CERBERUS_SPI_COMMAND_WARDRIVE_STOP
#define CERBERUS_WD_RELEASE CERBERUS_SPI_COMMAND_WARDRIVE_RELEASE
#define CERBERUS_WD_PAUSE CERBERUS_SPI_COMMAND_WARDRIVE_PAUSE
#define CERBERUS_WD_RESUME CERBERUS_SPI_COMMAND_WARDRIVE_RESUME
enum { CERBERUS_WD_OFF=0, CERBERUS_WD_STARTING, CERBERUS_WD_RUNNING,
       CERBERUS_WD_STOPPING, CERBERUS_WD_STOPPED,
       CERBERUS_WD_PAUSING, CERBERUS_WD_PAUSED };
enum { CERBERUS_WD_PHASE_NONE=0, CERBERUS_WD_PHASE_WIFI=1, CERBERUS_WD_PHASE_BLE=2 };
#define CERBERUS_WD_MIN_FIRMWARE 78U
#define CERBERUS_WD_PAUSE_MIN_FIRMWARE 79U
#define CERBERUS_WD_LEASE_MS 10000U
#define CERBERUS_WD_DWELL_MS 120U
#define CERBERUS_WD_BLE_WINDOW_MS 1600U

#pragma pack(push,1)
typedef struct {
    uint64_t session;
    uint32_t ack;
    uint8_t mode, discard, reserved[2];
} cerberus_wd_request_t;
typedef struct {
    uint32_t sequence, age_ms; /* Local uptime in the queue; age on the wire. */
    uint8_t address[6], label[32], label_length, kind, channel;
    int8_t rssi;
    uint8_t security, address_type;
    uint16_t manufacturer;
} cerberus_wd_record_t;
typedef struct {
    uint64_t session;
    int32_t error;
    uint32_t wifi_reports, ble_reports, dropped, wifi_sweeps, ble_ms, last_ack;
    uint16_t pending, high_water;
    uint8_t state, phase, channel, mode, held;
    uint32_t channel_rejects;
    uint8_t valid_channels,reserved[4];
} cerberus_wd_status_t;
typedef struct {
    uint32_t magic;
    uint8_t version,command,sequence,length,payload[54];
    uint16_t crc;
} cerberus_wd_frame_t;
#pragma pack(pop)
_Static_assert(sizeof(cerberus_wd_record_t)==54,"Wardrive observation wire size");
_Static_assert(sizeof(cerberus_wd_status_t)==54,"Wardrive status wire size");
_Static_assert(sizeof(cerberus_wd_frame_t)==64,"Wardrive wide frame");

typedef struct {
    cerberus_wd_record_t *rows;
    uint32_t next_sequence,last_ack,dropped;
    uint16_t capacity,head,count,high_water;
} cerberus_wd_ring_t;
static inline void cerberus_wd_increment(uint32_t *value){if(*value!=UINT32_MAX)++*value;}
static inline bool cerberus_wd_ring_push(cerberus_wd_ring_t *q,const cerberus_wd_record_t *row)
{
    if(!q||!row||!q->rows||!q->capacity||q->count>=q->capacity||q->next_sequence==UINT32_MAX){
        if(q)cerberus_wd_increment(&q->dropped);
        return false;
    }
    cerberus_wd_record_t *dst=&q->rows[(q->head+q->count)%q->capacity];
    *dst=*row;dst->sequence=++q->next_sequence;++q->count;
    if(q->count>q->high_water)q->high_water=q->count;
    return true;
}
static inline bool cerberus_wd_ring_ack(cerberus_wd_ring_t *q,uint32_t sequence)
{
    if(!sequence||sequence==q->last_ack)return true;
    if(!q->count||q->rows[q->head].sequence!=sequence)return false;
    q->last_ack=sequence;q->head=(uint16_t)((q->head+1U)%q->capacity);--q->count;return true;
}
static inline bool cerberus_wd_ring_read(const cerberus_wd_ring_t *q,uint32_t now,cerberus_wd_record_t *out)
{
    memset(out,0,sizeof(*out));if(!q->count)return false;
    *out=q->rows[q->head];out->age_ms=now-out->age_ms;return true;
}
/* AD fields are bounded independently of device identity. A scan response is
 * another observation; no invented name/manufacturer carries across packets. */
static inline bool cerberus_wd_ble_decode(const uint8_t *data,size_t length,
    const uint8_t address[6],uint8_t type,int rssi,uint32_t now,cerberus_wd_record_t *out)
{
    if(!out||!address||(!data&&length)||length>1650U||rssi < -127||rssi>20||type>3)return false;
    memset(out,0,sizeof(*out));out->kind=2;out->age_ms=now;out->rssi=(int8_t)rssi;
    out->address_type=type;out->manufacturer=UINT16_MAX;
    for(unsigned i=0;i<6;++i)out->address[i]=address[5U-i];
    bool complete_name=false;
    for(size_t pos=0;pos<length;){
        uint8_t n=data[pos++];if(!n)break;if(n>length-pos)return false;
        uint8_t tag=data[pos];
        if((tag==8U||tag==9U)&&(!complete_name||tag==9U)){
            size_t bytes=n-1U;if(bytes>32U)bytes=32U;
            memcpy(out->label,data+pos+1U,bytes);out->label_length=(uint8_t)bytes;
            complete_name=tag==9U;
        }else if(tag==255U&&n>=3U)out->manufacturer=(uint16_t)data[pos+1U]|((uint16_t)data[pos+2U]<<8U);
        pos+=n;
    }
    return true;
}
