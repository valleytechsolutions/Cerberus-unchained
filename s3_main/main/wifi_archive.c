#include "wifi_archive.h"
#include <limits.h>
#include <string.h>

static const uint8_t magic[8] = {'C','W','F','I','A','R','C',0};
static bool overlap(const void *a, size_t n, const void *b, size_t m)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return n && m && (x <= y ? y - x < n : x - y < m);
}
static bool zero(const void *p, size_t n)
{ const uint8_t *b = p; for (size_t i=0;i<n;++i) if (b[i]) return false; return true; }
static bool boolean(const bool *p)
{ const bool f=false,t=true; return !memcmp(p,&f,sizeof(f)) || !memcmp(p,&t,sizeof(t)); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (uint16_t)p[1]<<8); }
static uint32_t rd32(const uint8_t *p)
{ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static uint64_t rd64(const uint8_t *p) { return rd32(p)|(uint64_t)rd32(p+4)<<32; }
static void wr16(uint8_t *p,uint16_t v) { p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8); }
static void wr32(uint8_t *p,uint32_t v) { for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(v>>(8*i)); }
static void wr64(uint8_t *p,uint64_t v) { wr32(p,(uint32_t)v);wr32(p+4,(uint32_t)(v>>32)); }
static uint32_t crc(const uint8_t *p,size_t n)
{
    uint32_t c=UINT32_MAX;
    for(size_t i=0;i<n;++i) {
        c^=(i>=20 && i<24)?0:p[i];
        for(unsigned j=0;j<8;++j)c=(c>>1)^(UINT32_C(0xedb88320)&(0U-(c&1U)));
    }
    return ~c;
}
static bool channel(uint8_t c)
{
    return (c>=1 && c<=14) || (c>=36 && c<=64 && c%4==0) ||
        (c>=100 && c<=144 && c%4==0) || (c>=149 && c<=177 && (c-149)%4==0);
}
static bool publication_valid(const cerberus_wifi_archive_publication_t *p)
{
    if(!boolean(&p->split)||!boolean(&p->fallback)||!p->generation||
       p->received_us>INT64_MAX||!p->c5_session_id||!p->c5_relation_epoch||
       p->s3_count>32||p->c5_count>32||p->s3_total<p->s3_count||p->c5_total<p->c5_count||
       p->c5_error!=0) return false;
    if(p->split) return !p->fallback && p->s3_error==0 &&
        p->s3_coverage==CERBERUS_WIFI_ARCHIVE_COVERAGE_2G &&
        p->c5_coverage==CERBERUS_WIFI_ARCHIVE_COVERAGE_5G;
    return p->fallback && p->s3_error!=0 && p->s3_count==0 && p->s3_total==0 &&
        p->s3_coverage==CERBERUS_WIFI_ARCHIVE_COVERAGE_NONE &&
        p->c5_coverage==CERBERUS_WIFI_ARCHIVE_COVERAGE_ALL;
}
bool cerberus_wifi_archive_validate_publication(const cerberus_wifi_archive_publication_t *p,uint64_t saved,
    bool time_valid,bool subset,uint8_t count)
{
    if(!p)return false;
    unsigned total=(unsigned)p->s3_count+p->c5_count;
    return publication_valid(p) && count<=64 && count<=total && subset==(count<total) &&
        (time_valid ? saved<=INT64_MAX && saved>=p->received_us : saved==0);
}
bool cerberus_wifi_archive_validate_row(const cerberus_wifi_archive_row_t *r,
    const cerberus_wifi_archive_publication_t *p,uint64_t saved,bool time_valid)
{
    if(!r||!p||!publication_valid(p))return false;
    if(!boolean(&r->rssi_valid)||!boolean(&r->frame_present)||!boolean(&r->frame_age_upper_valid)) return false;
    const char *end=memchr(r->ssid,0,sizeof(r->ssid));
    if(!end||!zero(end,sizeof(r->ssid)-(size_t)(end-r->ssid))||!channel(r->channel)||r->security>9||
       (r->bssid[0]&1U)||zero(r->bssid,6)||
       r->rssi_valid!=(r->rssi>=-127 && r->rssi<0)) return false;
    if(r->source==CERBERUS_WIFI_ARCHIVE_SOURCE_S3) {
        if(r->source_index>=p->s3_count||r->channel>14||!r->source_generation||r->frame_present) return false;
    } else if(r->source==CERBERUS_WIFI_ARCHIVE_SOURCE_C5) {
        if(r->source_index>=p->c5_count||r->source_generation>255||
           (p->c5_coverage==CERBERUS_WIFI_ARCHIVE_COVERAGE_5G && r->channel<=14)) return false;
    } else return false;
    if(!r->frame_present) return !r->frame_request_started_us && !r->frame_source_age_ms &&
        !r->frame_age_upper_valid && !r->frame_age_upper_at_save_ms;
    if(r->frame_request_started_us>p->received_us) return false;
    if(!r->frame_age_upper_valid) return !r->frame_age_upper_at_save_ms;
    if(!time_valid||saved<r->frame_request_started_us||r->frame_source_age_ms==UINT32_MAX) return false;
    uint64_t elapsed=saved-r->frame_request_started_us;
    uint64_t bound=elapsed/1000U+(elapsed%1000U!=0);
    return bound<=UINT64_MAX-r->frame_source_age_ms &&
        r->frame_age_upper_at_save_ms==bound+r->frame_source_age_ms;
}
static bool identity(const cerberus_wifi_archive_row_t *r,uint32_t seen[2],uint32_t generations[2])
{
    unsigned s=r->source-1U; uint32_t bit=UINT32_C(1)<<r->source_index;
    if((seen[s]&bit)||(seen[s] && generations[s]!=r->source_generation))return false;
    seen[s]|=bit;generations[s]=r->source_generation;return true;
}
bool cerberus_wifi_archive_validate(const cerberus_wifi_archive_t *d)
{
    if(!d||!boolean(&d->saved_time_valid)||!boolean(&d->subset)||
       !cerberus_wifi_archive_validate_publication(&d->publication,d->saved_us,d->saved_time_valid,d->subset,d->count))return false;
    uint32_t seen[2]={0},generations[2]={0};
    for(unsigned i=0;i<d->count;++i)
        if(!cerberus_wifi_archive_validate_row(&d->rows[i],&d->publication,d->saved_us,d->saved_time_valid)||
           !identity(&d->rows[i],seen,generations))return false;
    return true;
}
static void encode_row(const cerberus_wifi_archive_row_t *r,uint8_t *b)
{
    b[0]=r->source;b[1]=r->source_index;b[2]=(uint8_t)strlen(r->ssid);
    b[3]=(uint8_t)(r->rssi_valid|(r->frame_present<<1)|(r->frame_age_upper_valid<<2));
    memcpy(b+4,r->bssid,6);b[10]=r->channel;b[11]=r->security;b[12]=(uint8_t)r->rssi;
    wr32(b+16,r->source_generation);wr32(b+20,r->received_ms);
    wr64(b+24,r->frame_request_started_us);wr32(b+32,r->frame_source_age_ms);
    wr64(b+40,r->frame_age_upper_at_save_ms);memcpy(b+48,r->ssid,b[2]);
}
static bool decode_row(const uint8_t *b,cerberus_wifi_archive_row_t *r)
{
    if(b[2]>32||(b[3]&~7U)||!zero(b+13,3)||!zero(b+36,4)||
       !zero(b+48+b[2],32-b[2])||memchr(b+48,0,b[2]))return false;
    memset(r,0,sizeof(*r));r->source=b[0];r->source_index=b[1];
    r->rssi_valid=(b[3]&1)!=0;r->frame_present=(b[3]&2)!=0;r->frame_age_upper_valid=(b[3]&4)!=0;
    memcpy(r->bssid,b+4,6);r->channel=b[10];r->security=b[11];r->rssi=(int8_t)b[12];
    r->source_generation=rd32(b+16);r->received_ms=rd32(b+20);
    r->frame_request_started_us=rd64(b+24);r->frame_source_age_ms=rd32(b+32);
    r->frame_age_upper_at_save_ms=rd64(b+40);memcpy(r->ssid,b+48,b[2]);return true;
}
bool cerberus_wifi_archive_encode(const cerberus_wifi_archive_t *d,uint8_t *b,size_t cap,size_t *written)
{
    if(!written||(d && overlap(d,sizeof(*d),written,sizeof(*written)))||
       (b && overlap(b,cap,written,sizeof(*written))))return false;
    *written=0;
    if(!d||!b||overlap(d,sizeof(*d),b,cap)||!cerberus_wifi_archive_validate(d))return false;
    size_t n=96U+(size_t)d->count*80U;if(cap<n)return false;
    memset(b,0,n);memcpy(b,magic,8);wr16(b+8,1);wr16(b+10,96);wr16(b+12,80);wr32(b+16,(uint32_t)n);
    const cerberus_wifi_archive_publication_t *p=&d->publication;
    wr64(b+24,d->saved_us);wr64(b+32,p->received_us);wr64(b+40,p->c5_session_id);wr64(b+48,p->c5_relation_epoch);
    wr32(b+56,p->generation);wr32(b+60,p->elapsed_ms);wr32(b+64,(uint32_t)p->s3_error);wr32(b+68,(uint32_t)p->c5_error);
    wr16(b+72,p->s3_total);wr16(b+74,p->c5_total);b[76]=p->s3_count;b[77]=p->c5_count;
    b[78]=p->s3_coverage;b[79]=p->c5_coverage;b[80]=d->count;
    b[81]=(uint8_t)(p->split|(p->fallback<<1)|(d->subset<<2)|(d->saved_time_valid<<3));
    for(unsigned i=0;i<d->count;++i)encode_row(&d->rows[i],b+96U+i*80U);
    wr32(b+20,crc(b,n));*written=n;return true;
}
bool cerberus_wifi_archive_decode(const uint8_t *b,size_t n,cerberus_wifi_archive_t *d)
{
    if(!b||!d||overlap(b,n,d,sizeof(*d))||n<96||n>5216||memcmp(b,magic,8)||
       rd16(b+8)!=1||rd16(b+10)!=96||rd16(b+12)!=80||rd16(b+14)||rd32(b+16)!=n||
       b[80]>64||n!=96U+(size_t)b[80]*80U||(b[81]&~15U)||!zero(b+82,14)||rd32(b+20)!=crc(b,n))return false;
    cerberus_wifi_archive_publication_t p={0};
    p.received_us=rd64(b+32);p.c5_session_id=rd64(b+40);p.c5_relation_epoch=rd64(b+48);
    p.generation=rd32(b+56);p.elapsed_ms=rd32(b+60);p.s3_error=(int32_t)rd32(b+64);p.c5_error=(int32_t)rd32(b+68);
    p.s3_total=rd16(b+72);p.c5_total=rd16(b+74);p.s3_count=b[76];p.c5_count=b[77];
    p.s3_coverage=b[78];p.c5_coverage=b[79];p.split=(b[81]&1)!=0;p.fallback=(b[81]&2)!=0;
    uint64_t saved=rd64(b+24);bool subset=(b[81]&4)!=0,time_valid=(b[81]&8)!=0;
    if(!cerberus_wifi_archive_validate_publication(&p,saved,time_valid,subset,b[80]))return false;
    cerberus_wifi_archive_row_t row;uint32_t seen[2]={0},generations[2]={0};
    for(unsigned i=0;i<b[80];++i)
        if(!decode_row(b+96U+i*80U,&row)||!cerberus_wifi_archive_validate_row(&row,&p,saved,time_valid)||!identity(&row,seen,generations))return false;
    memset(d,0,sizeof(*d));d->publication=p;d->saved_us=saved;d->subset=subset;d->saved_time_valid=time_valid;d->count=b[80];
    for(unsigned i=0;i<d->count;++i)(void)decode_row(b+96U+i*80U,&d->rows[i]);
    return true;
}
bool cerberus_wifi_archive_subset(const cerberus_wifi_archive_t *d,uint64_t mask,cerberus_wifi_archive_t *out)
{
    if(!d||!out||overlap(d,sizeof(*d),out,sizeof(*out))||!cerberus_wifi_archive_validate(d))return false;
    uint64_t valid=d->count==64?UINT64_MAX:(UINT64_C(1)<<d->count)-1U;
    if(mask==UINT64_MAX)mask=valid;
    if(mask&~valid)return false;
    memset(out,0,sizeof(*out));out->publication=d->publication;out->saved_us=d->saved_us;out->saved_time_valid=d->saved_time_valid;
    for(unsigned i=0;i<d->count;++i)if(mask&(UINT64_C(1)<<i))out->rows[out->count++]=d->rows[i];
    out->subset=out->count<(unsigned)d->publication.s3_count+d->publication.c5_count;return true;
}
