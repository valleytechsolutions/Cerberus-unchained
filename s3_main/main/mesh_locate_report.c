#include "mesh_locate_report.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static bool overlaps(const void *a,size_t an,const void *b,size_t bn)
{
    if (!a || !b || !an || !bn) return false;
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return x<=y ? y-x<an : x-y<bn;
}
bool mesh_locate_report_valid(const mesh_locate_t *locate)
{ return mesh_locate_frozen_valid(locate); }

typedef struct { char *out; size_t capacity,used; bool good; } writer_t;
static void bytes(writer_t *w,const char *text,size_t count)
{
    if (!w->good) return;
    if (w->used>MESH_LOCATE_REPORT_MAX_BYTES || count>MESH_LOCATE_REPORT_MAX_BYTES-w->used ||
        (w->out && (w->used>=w->capacity || count>=w->capacity-w->used))) {
        w->good=false; return;
    }
    if (w->out) { memcpy(w->out+w->used,text,count); w->out[w->used+count]='\0'; }
    w->used+=count;
}
static void append(writer_t *w,const char *fmt,...)
{
    if (!w->good) return;
    char fragment[256];
    va_list args; va_start(args,fmt);
    int count=vsnprintf(fragment,sizeof(fragment),fmt,args);
    va_end(args);
    if (count<0 || (size_t)count>=sizeof(fragment)) { w->good=false; return; }
    bytes(w,fragment,(size_t)count);
}
static bool format(const mesh_locate_t *s,char *out,size_t capacity,size_t *length)
{
    writer_t w={out,capacity,0U,true};
    const cerberus_mesh_locate_projection_t *p=&s->snapshot;
    append(&w,"# CERBERUS_MESH_LOCATE_CSV,1\r\n");
    append(&w,"# Passive selected-source header observations; not authenticated device identity or network membership.\r\n");
    append(&w,"# Signal is source RSSI/LQI; no distance or bearing is inferred. Unknown PAN is an exact unknown predicate.\r\n");
    append(&w,"# C5 callback/copy uptime and S3 anchor uptime are separate clocks; neither is UTC.\r\n");
    append(&w,"# C5 coalesces intermediate matches. History retains the latest 32 published samples; not all RF frames.\r\n");
    append(&w,"# Only the final source sample has a complete raw witness here. Raw bytes exclude PHY length and SDK RSSI/LQI footer; no FCS.\r\n");
    append(&w,"[SESSION]\r\norigin_survey_session,locate_session,final_state,source_error\r\n");
    append(&w,"%016" PRIX64 ",%016" PRIX64 ",%u,%" PRId32 "\r\n",
        s->origin_session,s->session,p->state,p->error_code);
    append(&w,"[SELECTED_SOURCE]\r\naddress_type,address_hex,channel,pan_known,pan_hex\r\n");
    append(&w,"%s,%016" PRIX64 ",%u,%u,%04X\r\n",s->target.mode==MESH_MAC_ADDRESS_EXT64?"EXT64":"SHORT16",
        s->target.value,s->target.pan.channel,s->target.pan.known,s->target.pan.value);
    append(&w,"[C5_COUNTERS]\r\ncallbacks,invalid_metadata,malformed,unsupported,filtered_nonmatch,matches,saturated,sequence_exhausted\r\n");
    append(&w,"%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%u,%u\r\n",
        p->callbacks,p->invalid_metadata,p->malformed,p->unsupported,p->filtered_nonmatch,p->matches,
        !!(p->flags&CERBERUS_MESH_LOCATE_COUNTERS_SATURATED),!!(p->flags&CERBERUS_MESH_LOCATE_SEQUENCE_EXHAUSTED));
    append(&w,"[FINAL_SIGNAL]\r\nmatch_sequence,callback_valid,callback_us,rssi_valid,rssi_dbm,lqi_valid,lqi,copied_valid,copied_us,clock_invalid\r\n");
    append(&w,"%" PRIu32 ",%u,%" PRIu64 ",%u,%d,%u,%u,%u,%" PRIu64 ",%u\r\n",p->match_sequence,
        !!(p->flags&CERBERUS_MESH_LOCATE_MATCH_TIME_VALID),p->last_match_us,
        !!(p->flags&CERBERUS_MESH_LOCATE_RSSI_VALID),p->rssi,!!(p->flags&CERBERUS_MESH_LOCATE_LQI_VALID),p->lqi,
        !!(p->flags&CERBERUS_MESH_LOCATE_COPY_TIME_VALID),p->copied_us,
        !!(p->flags&CERBERUS_MESH_LOCATE_PROJECTION_CLOCK_INVALID));
    append(&w,"[S3_HISTORY]\r\nretained_samples,published_samples,coalesced_match_updates,omitted_old_samples\r\n");
    append(&w,"%u,%" PRIu32 ",%" PRIu32 ",%" PRIu32 "\r\n",
        s->history_count,s->samples,s->coalesced_matches,s->samples-s->history_count);
    append(&w,"index,match_sequence,callback_valid,callback_us,rssi_valid,rssi_dbm,lqi_valid,lqi\r\n");
    for (uint8_t i=0U;i<s->history_count;++i) {
        mesh_locate_sample_t sample;
        if (!mesh_locate_history_get(s,i,&sample)) return false;
        append(&w,"%u,%" PRIu32 ",%u,%" PRIu64 ",%u,%d,%u,%u\r\n",i,sample.sequence,
            !!(sample.flags&CERBERUS_MESH_LOCATE_MATCH_TIME_VALID),sample.callback_us,
            !!(sample.flags&CERBERUS_MESH_LOCATE_RSSI_VALID),sample.rssi,
            !!(sample.flags&CERBERUS_MESH_LOCATE_LQI_VALID),sample.lqi);
    }
    append(&w,"[SOURCE_AGE_BOUND]\r\nvalid,s3_anchor_us,callback_age_upper_at_anchor_us\r\n");
    /* Export the retained bound at its recorded anchor. The current-time age
     * query intentionally rejects times before the last observed S3 clock. */
    bool age_valid=s->anchor_valid && !s->clock_failed;
    append(&w,"%u,%" PRIu64 ",%" PRIu64 "\r\n",age_valid,
        age_valid?s->snapshot_anchor_us:0U,age_valid?s->snapshot_age_us:0U);
    append(&w,"[FINAL_RAW_WITNESS]\r\nmatch_sequence,mac_length,mac_hex\r\n");
    append(&w,"%" PRIu32 ",%u,",p->match_sequence,p->mac_length);
    static const char digits[]="0123456789ABCDEF";
    for (uint8_t i=0U;i<p->mac_length;++i) {
        char pair[2]={digits[p->mac[i]>>4U],digits[p->mac[i]&15U]};
        bytes(&w,pair,sizeof(pair));
    }
    append(&w,"\r\n");
    if (!w.good) return false;
    *length=w.used; return true;
}
bool mesh_locate_report_encode(const mesh_locate_t *s,char *out,size_t capacity,size_t *length)
{
    if (!length || overlaps(s,s?sizeof(*s):0U,length,sizeof(*length)) ||
        overlaps(out,capacity,length,sizeof(*length)) || overlaps(s,s?sizeof(*s):0U,out,capacity)) return false;
    *length=0U;
    if ((!out && capacity) || !mesh_locate_report_valid(s)) return false;
    size_t needed=0U;
    if (!format(s,NULL,0U,&needed)) return false;
    if (!out) { *length=needed; return true; }
    if (capacity<=needed) return false;
    return format(s,out,capacity,length);
}
