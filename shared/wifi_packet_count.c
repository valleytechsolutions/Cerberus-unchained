#include "wifi_packet_count.h"
#include <limits.h>
#include <string.h>

static const uint8_t channels[CERBERUS_WIFI_PACKET_COUNT_CHANNELS]={
    1,2,3,4,5,6,7,8,9,10,11,12,13,14,36,40,44,48,52,56,60,64,
    100,104,108,112,116,120,124,128,132,136,140,144,149,153,157,161,165,169,173,177};
static bool overlap(const void *a,size_t n,const void *b,size_t m)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return n&&m&&(x>UINTPTR_MAX-n||y>UINTPTR_MAX-m||(x<y+m&&y<x+n));
}
static bool mac_valid(const uint8_t *p)
{
    uint8_t any=0;for(unsigned i=0;i<6U;++i)any|=p[i];
    return any&&!(p[0]&1U);
}
uint8_t cerberus_wifi_packet_count_channel(unsigned i)
{return i<CERBERUS_WIFI_PACKET_COUNT_CHANNELS?channels[i]:0U;}
int cerberus_wifi_packet_count_channel_index(uint8_t ch)
{for(unsigned i=0;i<CERBERUS_WIFI_PACKET_COUNT_CHANNELS;++i)if(channels[i]==ch)return (int)i;return -1;}
uint64_t cerberus_wifi_packet_count_channel_bit(uint8_t ch)
{int i=cerberus_wifi_packet_count_channel_index(ch);return i<0?0U:UINT64_C(1)<<(unsigned)i;}
bool cerberus_wifi_packet_count_mask_valid(uint64_t m)
{return m&&!(m>>CERBERUS_WIFI_PACKET_COUNT_CHANNELS);}
bool cerberus_wifi_packet_count_target_valid(const cerberus_wifi_packet_count_target_t *t)
{return t&&(t->role==CERBERUS_WIFI_PACKET_COUNT_AP||t->role==CERBERUS_WIFI_PACKET_COUNT_STA)&&
    mac_valid(t->mac)&&(!t->channel_hint||cerberus_wifi_packet_count_channel_bit(t->channel_hint));}
bool cerberus_wifi_packet_count_targets_valid(const cerberus_wifi_packet_count_target_t *t,unsigned n)
{
    if(!t||!n||n>CERBERUS_WIFI_PACKET_COUNT_LIMIT)return false;
    for(unsigned i=0;i<n;++i){
        if(!cerberus_wifi_packet_count_target_valid(t+i))return false;
        for(unsigned j=0;j<i;++j)if(t[i].role==t[j].role&&!memcmp(t[i].mac,t[j].mac,6U))return false;
    }
    return true;
}
uint64_t cerberus_wifi_packet_count_target_channels(const cerberus_wifi_packet_count_target_t *t,unsigned n)
{
    if(!cerberus_wifi_packet_count_targets_valid(t,n))return 0;
    uint64_t mask=0;for(unsigned i=0;i<n;++i)mask|=cerberus_wifi_packet_count_channel_bit(t[i].channel_hint);
    return mask;
}
unsigned cerberus_wifi_packet_count_decode(const uint8_t *p,size_t n,unsigned sdk,
    cerberus_wifi_packet_count_frame_t *out)
{
    if(!out||(p&&overlap(p,n,out,sizeof(*out))))return CERBERUS_WIFI_PACKET_COUNT_DECODE_MALFORMED;
    memset(out,0,sizeof(*out));
    if(!p||n<2U||n>CERBERUS_WIFI_PACKET_COUNT_FRAME_MAX)return CERBERUS_WIFI_PACKET_COUNT_DECODE_MALFORMED;
    if((p[0]&3U)||sdk>CERBERUS_WIFI_PACKET_COUNT_DATA)return CERBERUS_WIFI_PACKET_COUNT_DECODE_UNSUPPORTED;
    unsigned type=(p[0]>>2U)&3U,sub=p[0]>>4U,ds=p[1]&3U;
    if(type!=(sdk==CERBERUS_WIFI_PACKET_COUNT_MGMT?0U:2U))return CERBERUS_WIFI_PACKET_COUNT_DECODE_MALFORMED;
    if((type==0U&&(sub==7U||sub==15U))||(type==2U&&sub==13U))return CERBERUS_WIFI_PACKET_COUNT_DECODE_UNSUPPORTED;
    if(type==0U&&ds)return CERBERUS_WIFI_PACKET_COUNT_DECODE_MALFORMED;
    size_t header=24U;
    if(type==2U){if(ds==3U)header+=6U;if(sub&8U)header+=2U+((p[1]&128U)?4U:0U);}
    else if(p[1]&128U)header+=4U;
    if(n<header||!mac_valid(p+10U))return CERBERUS_WIFI_PACKET_COUNT_DECODE_MALFORMED;
    uint8_t any_ra=0;for(unsigned i=0;i<6U;++i)any_ra|=p[4U+i];
    if(!any_ra)return CERBERUS_WIFI_PACKET_COUNT_DECODE_MALFORMED;
    cerberus_wifi_packet_count_frame_t v={0};
    memcpy(v.ra,p+4U,6U);memcpy(v.ta,p+10U,6U);
    v.roles=CERBERUS_WIFI_PACKET_COUNT_ROLE_RA|CERBERUS_WIFI_PACKET_COUNT_ROLE_TA;
    const uint8_t *bssid=type==0U||ds==0U?p+16U:ds==1U?p+4U:ds==2U?p+10U:NULL;
    if(bssid&&mac_valid(bssid)){memcpy(v.bssid,bssid,6U);v.roles|=CERBERUS_WIFI_PACKET_COUNT_ROLE_BSSID;}
    v.kind=(uint8_t)sdk;v.subtype=(uint8_t)sub;v.header_bytes=(uint16_t)header;
    v.retry=(p[1]&8U)!=0;v.protected_frame=(p[1]&64U)!=0;
    v.fragmented=(p[1]&4U)!=0||(p[22]&15U)!=0;
    *out=v;return CERBERUS_WIFI_PACKET_COUNT_DECODE_OK;
}
static void add32(cerberus_wifi_packet_count_table_t *t,uint32_t *v)
{
    if(*v!=UINT32_MAX)++*v;
    if(*v==UINT32_MAX)t->summary.flags|=CERBERUS_WIFI_PACKET_COUNT_SATURATED;
}
static void changed(cerberus_wifi_packet_count_table_t *t)
{add32(t,&t->summary.revision);}
static bool clock_ok(cerberus_wifi_packet_count_table_t *t,int64_t now)
{
    if(now<0||(uint64_t)now<t->clock_floor||(t->summary.flags&CERBERUS_WIFI_PACKET_COUNT_CLOCK_INVALID)){
        t->summary.flags|=CERBERUS_WIFI_PACKET_COUNT_CLOCK_INVALID;return false;
    }
    t->clock_floor=(uint64_t)now;return true;
}
bool cerberus_wifi_packet_count_init(cerberus_wifi_packet_count_table_t *t,
    const cerberus_wifi_packet_count_target_t *targets,unsigned n,uint64_t mask,uint16_t dwell)
{
    if(!t||!cerberus_wifi_packet_count_targets_valid(targets,n)||
       !cerberus_wifi_packet_count_mask_valid(mask)||dwell<CERBERUS_WIFI_PACKET_COUNT_DWELL_MIN_MS||
       dwell>CERBERUS_WIFI_PACKET_COUNT_DWELL_MAX_MS||overlap(t,sizeof(*t),targets,n*sizeof(*targets)))return false;
    memset(t,0,sizeof(*t));t->summary.count=(uint8_t)n;t->summary.requested_channels=mask;
    t->summary.dwell_ms=dwell;
    for(unsigned i=0;i<n;++i)t->rows[i].target=targets[i];
    return true;
}
void cerberus_wifi_packet_count_attempt(cerberus_wifi_packet_count_table_t *t,uint8_t ch,bool success)
{
    uint64_t bit=cerberus_wifi_packet_count_channel_bit(ch);
    if(!t||!bit||!(t->summary.requested_channels&bit)||(t->summary.flags&CERBERUS_WIFI_PACKET_COUNT_STOPPED))return;
    t->summary.attempted_channels|=bit;
    if(success)t->summary.successful_channels|=bit;else add32(t,&t->summary.channel_failures);
    changed(t);
}
bool cerberus_wifi_packet_count_window_begin(cerberus_wifi_packet_count_table_t *t,uint8_t ch,int64_t now)
{
    uint64_t bit=cerberus_wifi_packet_count_channel_bit(ch);
    if(!t||t->window_open||!bit||!(t->summary.requested_channels&bit)||
       (t->summary.flags&CERBERUS_WIFI_PACKET_COUNT_STOPPED))return false;
    if(!clock_ok(t,now)){changed(t);return false;}
    if(!(t->summary.flags&CERBERUS_WIFI_PACKET_COUNT_STARTED_TIME)){
        t->summary.started_source_us=(uint64_t)now;t->summary.flags|=CERBERUS_WIFI_PACKET_COUNT_STARTED_TIME;
    }
    t->window_open=true;t->window_channel=ch;t->window_started_us=(uint64_t)now;
    changed(t);return true;
}
bool cerberus_wifi_packet_count_window_end(cerberus_wifi_packet_count_table_t *t,int64_t now)
{
    if(!t)return false;
    bool valid=clock_ok(t,now);
    if(t->window_open&&valid){
        unsigned i=(unsigned)cerberus_wifi_packet_count_channel_index(t->window_channel);
        uint64_t delta=(uint64_t)now-t->window_started_us;
        if(UINT64_MAX-t->channel_window_us[i]<=delta){t->channel_window_us[i]=UINT64_MAX;t->summary.flags|=CERBERUS_WIFI_PACKET_COUNT_SATURATED;}
        else t->channel_window_us[i]+=delta;
    }
    t->window_open=false;t->window_channel=0;t->window_started_us=0;changed(t);return valid;
}
void cerberus_wifi_packet_count_finish(cerberus_wifi_packet_count_table_t *t,int64_t now)
{
    if(!t||(t->summary.flags&CERBERUS_WIFI_PACKET_COUNT_STOPPED))return;
    bool valid=cerberus_wifi_packet_count_window_end(t,now);
    if(valid){t->summary.stopped_source_us=(uint64_t)now;t->summary.flags|=CERBERUS_WIFI_PACKET_COUNT_STOPPED_TIME;}
    t->summary.flags|=CERBERUS_WIFI_PACKET_COUNT_STOPPED;changed(t);
}
static void row_add(cerberus_wifi_packet_count_table_t *t,cerberus_wifi_packet_count_row_t *r,uint32_t *n)
{add32(t,n);if(*n==UINT32_MAX)r->flags|=CERBERUS_WIFI_PACKET_COUNT_ROW_SATURATED;}
uint16_t cerberus_wifi_packet_count_observe(cerberus_wifi_packet_count_table_t *t,
    const uint8_t *p,size_t n,unsigned kind,bool rx_ok,uint8_t ch,int rssi,int64_t now)
{
    if(!t||(t->summary.flags&CERBERUS_WIFI_PACKET_COUNT_STOPPED))return 0;
    add32(t,&t->summary.callbacks);changed(t);
    if(!clock_ok(t,now)){add32(t,&t->summary.clock_dropped_frames);return 0;}
    if(!t->window_open||ch!=t->window_channel){add32(t,&t->summary.outside_window_frames);return 0;}
    if(!rx_ok||!p||n>CERBERUS_WIFI_PACKET_COUNT_FRAME_MAX){add32(t,&t->summary.invalid_metadata);return 0;}
    cerberus_wifi_packet_count_frame_t f;
    unsigned parsed=cerberus_wifi_packet_count_decode(p,n,kind,&f);
    if(parsed!=CERBERUS_WIFI_PACKET_COUNT_DECODE_OK){
        add32(t,parsed==CERBERUS_WIFI_PACKET_COUNT_DECODE_UNSUPPORTED?&t->summary.unsupported_frames:&t->summary.malformed_frames);return 0;
    }
    add32(t,&t->summary.valid_frames);uint16_t mask=0;
    for(unsigned i=0;i<t->summary.count;++i){
        cerberus_wifi_packet_count_row_t *r=t->rows+i;
        bool tx=!memcmp(r->target.mac,f.ta,6U),rx=!memcmp(r->target.mac,f.ra,6U);
        if((f.roles&CERBERUS_WIFI_PACKET_COUNT_ROLE_BSSID)&&!memcmp(r->target.mac,f.bssid,6U))row_add(t,r,&r->bssid_context);
        if(!tx&&!rx)continue;
        mask|=(uint16_t)(1U<<i);row_add(t,r,&r->observations);
        row_add(t,r,kind==CERBERUS_WIFI_PACKET_COUNT_MGMT?&r->management:&r->data);
        if(tx)row_add(t,r,&r->transmitted);
        if(rx)row_add(t,r,&r->addressed);
        if(f.retry)row_add(t,r,&r->retry);
        if(f.protected_frame)row_add(t,r,&r->protected_frames);
        if(f.fragmented)row_add(t,r,&r->fragmented);
        if(!(r->flags&CERBERUS_WIFI_PACKET_COUNT_ROW_TIME))r->first_us=(uint64_t)now;
        r->flags|=CERBERUS_WIFI_PACKET_COUNT_ROW_TIME;r->last_us=(uint64_t)now;r->last_channel=ch;
        if(tx){
            r->last_tx_us=(uint64_t)now;r->flags|=CERBERUS_WIFI_PACKET_COUNT_ROW_TX_TIME;
            r->flags&=(uint8_t)~CERBERUS_WIFI_PACKET_COUNT_ROW_RSSI;r->rssi=0;
            if(rssi>=-127&&rssi<=0){r->rssi=(int8_t)rssi;r->flags|=CERBERUS_WIFI_PACKET_COUNT_ROW_RSSI;}
        }
    }
    if(mask)add32(t,&t->summary.matched_frames);
    return mask;
}
bool cerberus_wifi_packet_count_copy_summary(const cerberus_wifi_packet_count_table_t *t,
    int64_t now,cerberus_wifi_packet_count_summary_t *out)
{
    if(!t||!out||overlap(t,sizeof(*t),out,sizeof(*out)))return false;
    *out=t->summary;out->copied_source_us=0;out->flags&=(uint16_t)~CERBERUS_WIFI_PACKET_COUNT_COPIED_TIME;
    if(now<0||(uint64_t)now<t->clock_floor||(out->flags&CERBERUS_WIFI_PACKET_COUNT_CLOCK_INVALID))out->flags|=CERBERUS_WIFI_PACKET_COUNT_CLOCK_INVALID;
    else{out->copied_source_us=(uint64_t)now;out->flags|=CERBERUS_WIFI_PACKET_COUNT_COPIED_TIME;}
    return true;
}
bool cerberus_wifi_packet_count_copy_row(const cerberus_wifi_packet_count_table_t *t,unsigned i,cerberus_wifi_packet_count_row_t *out)
{
    if(!t||!out||i>=t->summary.count||i>=CERBERUS_WIFI_PACKET_COUNT_LIMIT||overlap(t,sizeof(*t),out,sizeof(*out)))return false;
    *out=t->rows[i];return true;
}
bool cerberus_wifi_packet_count_copy_channel(const cerberus_wifi_packet_count_table_t *t,
    unsigned i,int64_t now,uint64_t *out)
{
    if(!t||!out||i>=CERBERUS_WIFI_PACKET_COUNT_CHANNELS||overlap(t,sizeof(*t),out,sizeof(*out)))return false;
    uint64_t v=t->channel_window_us[i];
    if(t->window_open&&t->window_channel==channels[i]){
        if(now<0||(uint64_t)now<t->clock_floor||(t->summary.flags&CERBERUS_WIFI_PACKET_COUNT_CLOCK_INVALID))return false;
        uint64_t delta=(uint64_t)now-t->window_started_us;
        v=UINT64_MAX-v<delta?UINT64_MAX:v+delta;
    }
    *out=v;return true;
}
