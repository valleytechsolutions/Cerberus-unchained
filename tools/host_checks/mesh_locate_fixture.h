#pragma once
/* Synthetic source through the ACTUAL controller/parser. No valid-state mock.
 * Shared by report/worker fixtures; this is not a native-radio simulation. */
#include "mesh_locate.h"
#include <string.h>

typedef struct {
    cerberus_mesh_locate_projection_t live, pin;
    uint64_t session;
    uint32_t pin_id;
    int64_t clock;
    bool bound, running;
} mesh_locate_test_peer_t;
static inline void mesh_locate_test_le(uint8_t *p,uint64_t v,size_t n)
{ for(size_t i=0U;i<n;++i)p[i]=(uint8_t)(v>>(8U*i)); }
static inline void mesh_locate_test_encode(const cerberus_mesh_locate_projection_t *p,uint8_t out[197])
{
    memset(out,0,197U);out[0]=p->version;out[1]=p->flags;out[2]=p->state;out[3]=p->channel;
    out[4]=p->address_mode;out[5]=p->pan_known;out[6]=p->mac_length;
    mesh_locate_test_le(out+8U,p->pan,2U);out[10]=(uint8_t)p->rssi;out[11]=p->lqi;
    mesh_locate_test_le(out+12U,p->match_sequence,4U);mesh_locate_test_le(out+16U,p->address,8U);
    mesh_locate_test_le(out+24U,p->copied_us,8U);mesh_locate_test_le(out+32U,p->last_match_us,8U);
    mesh_locate_test_le(out+40U,p->callbacks,4U);mesh_locate_test_le(out+44U,p->invalid_metadata,4U);
    mesh_locate_test_le(out+48U,p->malformed,4U);mesh_locate_test_le(out+52U,p->unsupported,4U);
    mesh_locate_test_le(out+56U,p->filtered_nonmatch,4U);mesh_locate_test_le(out+60U,p->matches,4U);
    mesh_locate_test_le(out+64U,(uint32_t)p->error_code,4U);memcpy(out+72U,p->mac,125U);
}
static inline void mesh_locate_test_reply(mesh_locate_test_peer_t *p,cerberus_mesh_locate_control_t *r)
{
    memset(r,0,sizeof(*r));r->session_id=p->session;
    if(!p->bound)return;
    p->live.state=p->running?CERBERUS_MESH_LOCATE_RECEIVING:
        p->live.error_code?CERBERUS_MESH_LOCATE_ERROR:CERBERUS_MESH_LOCATE_COMPLETE;
    p->live.channel=p->running?11U:0U;
    if(!p->running)p->live.flags|=CERBERUS_MESH_LOCATE_STOPPED;
    r->sequence=p->live.match_sequence;r->state=p->live.state;r->channel=p->live.channel;
    r->flags=CERBERUS_MESH_LOCATE_BOUND|CERBERUS_MESH_LOCATE_HELD|CERBERUS_MESH_LOCATE_DATA_READY;
    if(p->running)r->flags|=CERBERUS_MESH_LOCATE_RX_ACTIVE;
    r->version=1U;r->projection_bytes=(uint8_t)(72U+p->live.mac_length);r->error_code=p->live.error_code;
}
static inline esp_err_t mesh_locate_test_exchange(void *ctx,uint8_t cmd,const void *q,size_t n,void *out,size_t outn)
{
    mesh_locate_test_peer_t *p=ctx;cerberus_mesh_locate_control_t *r=out;
    if(!out || outn!=22U)return ESP_ERR_INVALID_ARG;
    if(cmd==CERBERUS_MESH_LOCATE_COMMAND_INFO){
        if(q || n)return ESP_ERR_INVALID_ARG;
        memset(r,0,sizeof(*r));r->session_id=256U;r->version=1U;r->projection_bytes=72U;return ESP_OK;
    }
    if(cmd==CERBERUS_MESH_LOCATE_COMMAND_START){
        if(!q || n!=22U)return ESP_ERR_INVALID_ARG;
        const cerberus_mesh_locate_start_t *start=q;
        if(start->session_id!=257U || p->bound)return ESP_ERR_INVALID_STATE;
        p->session=start->session_id;p->bound=p->running=true;p->live.version=1U;
        p->live.address=start->address;p->live.address_mode=start->address_mode;
        p->live.pan=start->pan;p->live.pan_known=start->pan_known;
        mesh_locate_test_reply(p,r);return ESP_OK;
    }
    if(cmd==CERBERUS_MESH_LOCATE_COMMAND_READ){
        if(!q || n!=13U)return ESP_ERR_INVALID_ARG;
        const cerberus_mesh_locate_read_t *read=q;cerberus_mesh_locate_chunk_t *chunk=out;
        if(!p->bound || read->session_id!=p->session || read->pin!=p->pin_id)return ESP_ERR_INVALID_STATE;
        size_t offset=(size_t)read->part*9U,length=72U+p->pin.mac_length;
        if(offset>=length)return ESP_ERR_INVALID_ARG;
        uint8_t data[197];mesh_locate_test_encode(&p->pin,data);
        memset(chunk,0,sizeof(*chunk));chunk->session_id=p->session;chunk->pin=p->pin_id;chunk->part=read->part;
        size_t amount=length-offset;if(amount>9U)amount=9U;memcpy(chunk->data,data+offset,amount);return ESP_OK;
    }
    if(!q || n!=8U || ((const cerberus_mesh_locate_request_t*)q)->session_id!=p->session)return ESP_ERR_INVALID_ARG;
    if(cmd==CERBERUS_MESH_LOCATE_COMMAND_STOP)p->running=false;
    else if(cmd==CERBERUS_MESH_LOCATE_COMMAND_RELEASE)p->bound=false;
    else if(cmd!=CERBERUS_MESH_LOCATE_COMMAND_STATUS && cmd!=CERBERUS_MESH_LOCATE_COMMAND_PEEK)return ESP_ERR_NOT_SUPPORTED;
    mesh_locate_test_reply(p,r);
    if(cmd==CERBERUS_MESH_LOCATE_COMMAND_PEEK){
        p->pin=p->live;p->pin.copied_us=(uint64_t)p->clock+1000000U;
        p->pin.flags|=CERBERUS_MESH_LOCATE_COPY_TIME_VALID;r->sequence=++p->pin_id;
    }
    return ESP_OK;
}
static inline int64_t mesh_locate_test_clock(void *ctx)
{ return ((mesh_locate_test_peer_t*)ctx)->clock; }
static inline void mesh_locate_test_tick(mesh_locate_t *s,mesh_locate_test_peer_t *p,
                                        mesh_scan_transport_t *t,uint64_t *now)
{ mesh_locate_tick(s,*now,t);*now+=100U;p->clock+=100000; }
static inline bool mesh_locate_test_fill_options(mesh_locate_t *s,unsigned count,uint32_t stride,esp_err_t source_error)
{
    if(!s || !stride || count>1000U || (uint64_t)count*stride>CERBERUS_MESH_LOCATE_SEQUENCE_MAX)return false;
    mesh_locate_test_peer_t p={.clock=1000000,.pin_id=CERBERUS_MESH_LOCATE_PIN_BASE};
    mesh_scan_transport_t t={.exchange=mesh_locate_test_exchange,.clock_us=mesh_locate_test_clock,.context=&p};
    mesh_observation_address_key_t key={.value=0x1122U,.mode=MESH_MAC_ADDRESS_SHORT16,
        .pan={.value=0x1234U,.channel=11U,.known=true}};
    uint64_t now=0U;mesh_locate_init(s);
    if(mesh_locate_begin(s,&key,0xabcU)!=ESP_OK)return false;
    mesh_locate_test_tick(s,&p,&t,&now);
    for(unsigned i=1U;i<=count;++i){
        p.live.match_sequence=p.live.matches=i*stride;p.live.callbacks=p.live.matches;
        p.live.last_match_us=(uint64_t)p.clock+1000000U;p.live.rssi=(int8_t)(-30-(int)(i%60U));p.live.lqi=(uint8_t)i;
        p.live.flags|=CERBERUS_MESH_LOCATE_MATCH_TIME_VALID|CERBERUS_MESH_LOCATE_RSSI_VALID|CERBERUS_MESH_LOCATE_LQI_VALID;
        const uint8_t raw[]={0x41,0x88,7,0x34,0x12,0x44,0x33,0x22,0x11};
        memcpy(p.live.mac,raw,sizeof(raw));p.live.mac_length=sizeof(raw);p.live.mac[2]=(uint8_t)i;
        for(unsigned tick=0U;tick<100U && (!s->has_snapshot || s->snapshot.match_sequence!=p.live.match_sequence);++tick)
            mesh_locate_test_tick(s,&p,&t,&now);
        if(s->faulted || !s->has_snapshot || s->snapshot.match_sequence!=p.live.match_sequence)return false;
    }
    p.live.error_code=source_error;mesh_locate_stop(s);
    for(unsigned i=0U;i<100U && !mesh_locate_released(s);++i)mesh_locate_test_tick(s,&p,&t,&now);
    return mesh_locate_frozen_valid(s);
}
static inline bool mesh_locate_test_fill(mesh_locate_t *s,unsigned count)
{ return mesh_locate_test_fill_options(s,count,1U,ESP_OK); }
