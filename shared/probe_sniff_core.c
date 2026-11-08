/* Receive-only implementation; no SDK, radio ownership or heap calls. */
#include "probe_sniff_core.h"
#include <limits.h>
#include <string.h>

static const uint8_t channels[] = {
    1,2,3,4,5,6,7,8,9,10,11,12,13,14,
    36,40,44,48,52,56,60,64,100,104,108,112,116,120,124,128,
    132,136,140,144,149,153,157,161,165,169,173,177,
};
static bool overlaps(const void *a,size_t an,const void *b,size_t bn)
{
    if(!a||!b||!an||!bn)return false;
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return x<=y ? y-x<an : x-y<bn;
}
uint8_t cerberus_probe_sniff_channel_at(uint8_t index)
{return index<sizeof(channels)?channels[index]:0U;}
bool cerberus_probe_sniff_channel_valid(uint8_t channel)
{
    for(size_t i=0;i<sizeof(channels);++i)if(channels[i]==channel)return true;
    return false;
}
bool cerberus_probe_sniff_mac_valid(const uint8_t mac[6])
{
    if(!mac||(mac[0]&1U))return false;
    uint8_t any=0;for(unsigned i=0;i<6;++i)any|=mac[i];return any!=0U;
}
bool cerberus_probe_sniff_control_valid(const cerberus_probe_sniff_control_t *c,
    bool info,bool release,uint64_t requested_session)
{
    if(!c||!c->session_id||(!info&&c->session_id!=requested_session)||
        (c->flags&~63U))return false;
    if(!(c->flags&CERBERUS_PROBE_SNIFF_BOUND)){
        if(info)return c->state==CERBERUS_PROBE_SNIFF_IDLE&&!c->flags&&!c->count&&!c->mode&&!c->channel&&
            !c->revision&&c->capacity==CERBERUS_PROBE_SNIFF_CAPACITY;
        return (c->error_code!=0||release)&&!c->state&&!c->flags&&!c->revision&&!c->count&&!c->mode&&!c->channel&&!c->capacity;
    }
    if(c->state<CERBERUS_PROBE_SNIFF_QUEUED||c->state>CERBERUS_PROBE_SNIFF_ERROR||
       c->mode>CERBERUS_PROBE_SNIFF_HOP_BOTH||c->capacity!=CERBERUS_PROBE_SNIFF_CAPACITY||c->count>c->capacity||
       ((c->flags&CERBERUS_PROBE_SNIFF_OVERFLOW)&&c->count!=c->capacity))return false;
    bool owned=(c->flags&CERBERUS_PROBE_SNIFF_OWNED)!=0;
    bool receiving=(c->flags&CERBERUS_PROBE_SNIFF_RECEIVING)!=0;
    if(owned!=(c->state<=CERBERUS_PROBE_SNIFF_STOPPING)||
       (receiving&&(!owned||c->state!=CERBERUS_PROBE_SNIFF_RUNNING))||
       (receiving?!cerberus_probe_sniff_channel_valid(c->channel):c->channel!=0U)||
       ((c->flags&CERBERUS_PROBE_SNIFF_DATA_READY)&&owned))return false;
    return true;
}
static bool row_valid(const cerberus_probe_sniff_row_t *row,bool projected)
{
    if(!row||!cerberus_probe_sniff_mac_valid(row->transmitter)||row->ssid_length>32U||
       !cerberus_probe_sniff_channel_valid(row->last_channel)||row->last_rssi<-127||row->last_rssi>0||
       (row->flags&~(projected?7U:3U))||!row->frames||row->retry_frames>row->frames||
       row->first_callback_us>row->last_callback_us||row->last_callback_us>INT64_MAX||
       (row->last_sequence_control&15U)||
       ((row->flags&CERBERUS_PROBE_SNIFF_ROW_RETRY)&&!row->retry_frames)||
       (!!(row->flags&CERBERUS_PROBE_SNIFF_ROW_SATURATED)!=(row->frames==UINT32_MAX)))return false;
    for(unsigned i=row->ssid_length;i<32U;++i)if(row->ssid[i])return false;
    return true;
}
bool cerberus_probe_sniff_row_valid(const cerberus_probe_sniff_row_t *row)
{return row_valid(row,false);}
bool cerberus_probe_sniff_projection_valid(const cerberus_probe_sniff_projection_t *row)
{
    if(!row||!row_valid(&row->row,true))return false;
    if(row->row.flags&CERBERUS_PROBE_SNIFF_ROW_COPY_TIME_VALID)
        return row->copied_source_us>=row->row.last_callback_us&&row->copied_source_us<=INT64_MAX;
    return row->copied_source_us==0U;
}
bool cerberus_probe_sniff_summary_valid(const cerberus_probe_sniff_summary_t *s)
{
    if(!s||s->capacity!=CERBERUS_PROBE_SNIFF_CAPACITY||s->count>s->capacity||s->reserved[0]||s->reserved[1]||
       s->dwell_ms!=CERBERUS_PROBE_SNIFF_DWELL_MS||(s->flags&~63U)||
       (!s->count&&s->valid_frames)||s->retained_frames<s->count||s->retained_frames>s->valid_frames||s->retry_frames>s->valid_frames||
       s->capacity_dropped_frames>s->valid_frames||
       (s->capacity_dropped_frames&&s->count!=s->capacity)||
       (s->attempted_channels>>CERBERUS_PROBE_SNIFF_CHANNEL_COUNT)||(s->successful_channels&~s->attempted_channels)||
       ((s->flags&CERBERUS_PROBE_SNIFF_REVISION_EXHAUSTED)&&s->revision!=UINT32_MAX))return false;
    bool saturated=s->valid_frames==UINT32_MAX||s->retained_frames==UINT32_MAX||
        s->capacity_dropped_frames==UINT32_MAX||s->retry_frames==UINT32_MAX||
        s->malformed_frames==UINT32_MAX||s->unsupported_frames==UINT32_MAX||
        s->clock_dropped_frames==UINT32_MAX||s->channel_failures==UINT32_MAX;
    if(!!(s->flags&CERBERUS_PROBE_SNIFF_COUNTERS_SATURATED)!=saturated)return false;
    if(!(s->flags&CERBERUS_PROBE_SNIFF_START_TIME_VALID)){if(s->started_source_us)return false;}
    else if(s->started_source_us>INT64_MAX)return false;
    if(!(s->flags&CERBERUS_PROBE_SNIFF_STOP_TIME_VALID)){if(s->stopped_source_us)return false;}
    else if(!(s->flags&CERBERUS_PROBE_SNIFF_STOPPED)||!(s->flags&CERBERUS_PROBE_SNIFF_START_TIME_VALID)||
        s->stopped_source_us<s->started_source_us||s->stopped_source_us>INT64_MAX)return false;
    if(!(s->flags&CERBERUS_PROBE_SNIFF_COPY_TIME_VALID)){if(s->copied_source_us)return false;}
    else if(s->copied_source_us>INT64_MAX||
        ((s->flags&CERBERUS_PROBE_SNIFF_START_TIME_VALID)&&s->copied_source_us<s->started_source_us)||
        ((s->flags&CERBERUS_PROBE_SNIFF_STOP_TIME_VALID)&&s->copied_source_us<s->stopped_source_us))return false;
    if(s->valid_frames!=UINT32_MAX && (uint64_t)s->retained_frames+s->capacity_dropped_frames!=s->valid_frames)return false;
    return true;
}
cerberus_probe_sniff_decode_result_t cerberus_probe_sniff_decode(
    const uint8_t *p,size_t length,uint8_t rx_channel,int rssi,uint64_t callback_us,
    cerberus_probe_sniff_row_t *out)
{
    if(!out||overlaps(p,length,out,sizeof(*out)))return CERBERUS_PROBE_SNIFF_DECODE_MALFORMED;
    memset(out,0,sizeof(*out));
    if(!p||!length)return CERBERUS_PROBE_SNIFF_DECODE_MALFORMED;
    if(p[0]!=0x40U)return CERBERUS_PROBE_SNIFF_DECODE_IGNORE;
    if(length>CERBERUS_PROBE_SNIFF_MAX_FRAME_BYTES)return CERBERUS_PROBE_SNIFF_DECODE_UNSUPPORTED;
    if(length<26U)return CERBERUS_PROBE_SNIFF_DECODE_MALFORMED;
    /* Version/type/subtype fixed by byte0. Retry, PowerManagement and MoreData
     * flags are accepted; DS, protected, fragmented and HT-control variants
     * are outside this complete24-byte management-header parser. */
    if((p[1]&0xc7U)||(p[22]&15U))return CERBERUS_PROBE_SNIFF_DECODE_UNSUPPORTED;
    if(!cerberus_probe_sniff_mac_valid(p+10U))return CERBERUS_PROBE_SNIFF_DECODE_MALFORMED;
    if(!cerberus_probe_sniff_channel_valid(rx_channel)||rssi<-127||rssi>0||callback_us>INT64_MAX)
        return CERBERUS_PROBE_SNIFF_DECODE_UNSUPPORTED;
    const uint8_t *ssid=NULL;uint8_t size=0;
    for(size_t at=24U;at<length;){
        if(length-at<2U)return CERBERUS_PROBE_SNIFF_DECODE_MALFORMED;
        uint8_t id=p[at],amount=p[at+1U];at+=2U;
        if(amount>length-at)return CERBERUS_PROBE_SNIFF_DECODE_MALFORMED;
        if(!id){if(ssid||amount>32U)return CERBERUS_PROBE_SNIFF_DECODE_MALFORMED;ssid=p+at;size=amount;}
        at+=amount;
    }
    if(!ssid)return CERBERUS_PROBE_SNIFF_DECODE_MALFORMED;
    memcpy(out->transmitter,p+10U,6U);memcpy(out->ssid,ssid,size);out->ssid_length=size;
    out->last_channel=rx_channel;out->last_rssi=(int8_t)rssi;
    out->first_callback_us=out->last_callback_us=callback_us;out->frames=1U;
    out->last_sequence_control=(uint16_t)p[22]|((uint16_t)p[23]<<8U);
    if(p[1]&8U){out->flags=CERBERUS_PROBE_SNIFF_ROW_RETRY;out->retry_frames=1U;}
    return CERBERUS_PROBE_SNIFF_DECODE_VALID;
}
static uint32_t increment(uint32_t value,cerberus_probe_sniff_table_t *t)
{
    if(value<UINT32_MAX)++value;
    if(value==UINT32_MAX)t->summary.flags|=CERBERUS_PROBE_SNIFF_COUNTERS_SATURATED;
    return value;
}
static bool revise(cerberus_probe_sniff_table_t *t)
{
    if(t->summary.revision==UINT32_MAX){t->accepting=false;t->summary.flags|=CERBERUS_PROBE_SNIFF_REVISION_EXHAUSTED;return false;}
    ++t->summary.revision;
    if(t->summary.revision==UINT32_MAX){t->accepting=false;t->summary.flags|=CERBERUS_PROBE_SNIFF_REVISION_EXHAUSTED;}
    return true;
}
static bool admit(cerberus_probe_sniff_table_t *t,uint64_t session,uint64_t now)
{
    if(!t||!t->accepting||!session||session!=t->session_id)return false;
    if(now>INT64_MAX||now<t->armed_us||now<t->clock_high_water_us){
        if(revise(t))t->summary.clock_dropped_frames=increment(t->summary.clock_dropped_frames,t);
        return false;
    }
    t->clock_high_water_us=now;return true;
}
void cerberus_probe_sniff_table_begin(cerberus_probe_sniff_table_t *t,uint64_t session,uint64_t armed_us)
{
    if(!t)return;
    memset(t,0,sizeof(*t));t->summary.capacity=CERBERUS_PROBE_SNIFF_CAPACITY;
    t->summary.dwell_ms=CERBERUS_PROBE_SNIFF_DWELL_MS;
    if(!session||armed_us>INT64_MAX)return;
    t->session_id=session;t->armed_us=t->clock_high_water_us=armed_us;t->accepting=true;
    t->summary.started_source_us=armed_us;t->summary.flags=CERBERUS_PROBE_SNIFF_START_TIME_VALID;
}
bool cerberus_probe_sniff_table_commit(cerberus_probe_sniff_table_t *t,uint64_t session,
    const cerberus_probe_sniff_row_t *row,uint64_t now)
{
    if(!row||!cerberus_probe_sniff_row_valid(row)||row->frames!=1U||row->retry_frames>1U||
        row->first_callback_us!=now||row->last_callback_us!=now||
        !admit(t,session,now)||!revise(t))return false;
    unsigned index=0;
    while(index<t->summary.count&&(memcmp(t->rows[index].transmitter,row->transmitter,6U)||
        t->rows[index].ssid_length!=row->ssid_length||memcmp(t->rows[index].ssid,row->ssid,row->ssid_length)))++index;
    t->summary.valid_frames=increment(t->summary.valid_frames,t);
    if(row->retry_frames)t->summary.retry_frames=increment(t->summary.retry_frames,t);
    if(index==CERBERUS_PROBE_SNIFF_CAPACITY){
        t->summary.capacity_dropped_frames=increment(t->summary.capacity_dropped_frames,t);return false;
    }
    uint64_t first=index<t->summary.count?t->rows[index].first_callback_us:now;
    uint32_t count=index<t->summary.count?t->rows[index].frames:0U;
    uint32_t retries=index<t->summary.count?t->rows[index].retry_frames:0U;
    if(index==t->summary.count)++t->summary.count;
    t->rows[index]=*row;t->rows[index].first_callback_us=first;
    t->rows[index].frames=increment(count,t);
    t->rows[index].retry_frames=row->retry_frames?increment(retries,t):retries;
    if(t->rows[index].frames==UINT32_MAX)t->rows[index].flags|=CERBERUS_PROBE_SNIFF_ROW_SATURATED;
    t->summary.retained_frames=increment(t->summary.retained_frames,t);return true;
}
void cerberus_probe_sniff_table_rejected(cerberus_probe_sniff_table_t *t,uint64_t session,
    cerberus_probe_sniff_decode_result_t result,uint64_t now)
{
    if((result!=CERBERUS_PROBE_SNIFF_DECODE_MALFORMED&&result!=CERBERUS_PROBE_SNIFF_DECODE_UNSUPPORTED)||
        !admit(t,session,now)||!revise(t))return;
    if(result==CERBERUS_PROBE_SNIFF_DECODE_MALFORMED)
        t->summary.malformed_frames=increment(t->summary.malformed_frames,t);
    else t->summary.unsupported_frames=increment(t->summary.unsupported_frames,t);
}
void cerberus_probe_sniff_table_finish(cerberus_probe_sniff_table_t *t,uint64_t session,int64_t stopped_us)
{
    if(!t||!session||session!=t->session_id||(t->summary.flags&CERBERUS_PROBE_SNIFF_STOPPED))return;
    t->accepting=false;(void)revise(t);t->summary.flags|=CERBERUS_PROBE_SNIFF_STOPPED;
    if(stopped_us>=0&&(uint64_t)stopped_us>=t->clock_high_water_us){
        t->summary.stopped_source_us=(uint64_t)stopped_us;t->summary.flags|=CERBERUS_PROBE_SNIFF_STOP_TIME_VALID;
    }
}
bool cerberus_probe_sniff_table_copy_row(const cerberus_probe_sniff_table_t *t,uint64_t session,
    uint8_t index,int64_t copied_us,cerberus_probe_sniff_projection_t *out)
{
    if(!out||overlaps(t,t?sizeof(*t):0,out,sizeof(*out)))return false;
    memset(out,0,sizeof(*out));
    if(!t||!session||session!=t->session_id||index>=t->summary.count)return false;
    out->row=t->rows[index];
    if(copied_us>=0&&(uint64_t)copied_us>=t->clock_high_water_us&&
       (!(t->summary.flags&CERBERUS_PROBE_SNIFF_STOP_TIME_VALID)||(uint64_t)copied_us>=t->summary.stopped_source_us)){
        out->copied_source_us=(uint64_t)copied_us;out->row.flags|=CERBERUS_PROBE_SNIFF_ROW_COPY_TIME_VALID;
    }
    return true;
}
bool cerberus_probe_sniff_table_copy_summary(const cerberus_probe_sniff_table_t *t,uint64_t session,
    int64_t copied_us,cerberus_probe_sniff_summary_t *out)
{
    if(!out||overlaps(t,t?sizeof(*t):0,out,sizeof(*out)))return false;
    memset(out,0,sizeof(*out));
    if(!t||!session||session!=t->session_id)return false;
    *out=t->summary;
    if(copied_us>=0&&(uint64_t)copied_us>=t->clock_high_water_us&&
       (!(out->flags&CERBERUS_PROBE_SNIFF_STOP_TIME_VALID)||(uint64_t)copied_us>=out->stopped_source_us)){
        out->copied_source_us=(uint64_t)copied_us;out->flags|=CERBERUS_PROBE_SNIFF_COPY_TIME_VALID;
    }
    return true;
}
