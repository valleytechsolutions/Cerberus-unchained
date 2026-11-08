#include "wifi_archive_capture.h"
#include <string.h>

static bool overlap(const void *a,size_t n,const void *b,size_t m)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return n && m && (x<=y ? y-x<n : x-y<m);
}
static bool convert(const cerberus_dual_wifi_row_t *in,
    const cerberus_wifi_archive_publication_t *p,uint64_t saved,bool time_valid,
    cerberus_wifi_archive_row_t *out)
{
    const bool f=false,t=true;
    if(memcmp(&in->observation.valid,&f,sizeof(f)) && memcmp(&in->observation.valid,&t,sizeof(t)))return false;
    const char *end=memchr(in->ap.ssid,0,sizeof(in->ap.ssid));if(!end)return false;
    memset(out,0,sizeof(*out));memcpy(out->ssid,in->ap.ssid,(size_t)(end-in->ap.ssid));
    memcpy(out->bssid,in->ap.bssid,6);out->channel=in->ap.channel;out->security=in->ap.security;
    out->source=in->source;out->source_index=in->source_index;out->source_generation=in->source_generation;
    out->received_ms=in->received_ms;out->rssi=in->ap.rssi;out->rssi_valid=in->ap.rssi>=-127 && in->ap.rssi<0;
    out->frame_present=in->observation.valid;
    out->frame_request_started_us=in->observation.request_started_us;
    out->frame_source_age_ms=in->observation.source_age_ms;
    if(out->frame_present && time_valid && saved>=out->frame_request_started_us &&
       out->frame_source_age_ms!=UINT32_MAX) {
        uint64_t elapsed=saved-out->frame_request_started_us;
        uint64_t bound=elapsed/1000U+(elapsed%1000U!=0);
        if(bound<=UINT64_MAX-out->frame_source_age_ms) {
            out->frame_age_upper_at_save_ms=bound+out->frame_source_age_ms;
            out->frame_age_upper_valid=true;
        }
    }
    return cerberus_wifi_archive_validate_row(out,p,saved,time_valid);
}
bool cerberus_wifi_archive_capture_rows(const cerberus_wifi_archive_publication_t *p,
    const cerberus_dual_wifi_row_t *rows,uint8_t count,int64_t now,uint64_t mask,
    cerberus_wifi_archive_t *out)
{
    if(!p||!rows||!out||count>64||count!=(unsigned)p->s3_count+p->c5_count||
       overlap(p,sizeof(*p),out,sizeof(*out))||overlap(rows,(size_t)count*sizeof(*rows),out,sizeof(*out)))return false;
    uint64_t valid=count==64?UINT64_MAX:(UINT64_C(1)<<count)-1U;
    if(mask==UINT64_MAX)mask=valid;
    if(mask&~valid)return false;
    uint8_t selected=0;for(unsigned i=0;i<count;++i)if(mask&(UINT64_C(1)<<i))++selected;
    bool time_valid=now>=0 && (uint64_t)now>=p->received_us;
    uint64_t saved=time_valid?(uint64_t)now:0;
    if(!cerberus_wifi_archive_validate_publication(p,saved,time_valid,selected<count,selected))return false;
    cerberus_wifi_archive_row_t row;uint32_t seen[2]={0},generation[2]={0};
    for(unsigned i=0;i<count;++i) {
        if(!convert(&rows[i],p,saved,time_valid,&row))return false;
        unsigned s=row.source-1U;uint32_t bit=UINT32_C(1)<<row.source_index;
        if((seen[s]&bit)||(seen[s] && generation[s]!=row.source_generation))return false;
        seen[s]|=bit;generation[s]=row.source_generation;
    }
    memset(out,0,sizeof(*out));out->publication=*p;out->saved_us=saved;out->saved_time_valid=time_valid;
    out->subset=selected<count;
    for(unsigned i=0;i<count;++i)if(mask&(UINT64_C(1)<<i))
        (void)convert(&rows[i],p,saved,time_valid,&out->rows[out->count++]);
    return true;
}
bool cerberus_wifi_archive_capture(const cerberus_dual_wifi_snapshot_t *snapshot,
    int64_t now,cerberus_wifi_archive_t *out)
{
    if(!snapshot||!out||overlap(snapshot,sizeof(*snapshot),out,sizeof(*out)))return false;
    return cerberus_wifi_archive_capture_rows(&snapshot->publication,snapshot->rows,
        snapshot->count,now,UINT64_MAX,out);
}
