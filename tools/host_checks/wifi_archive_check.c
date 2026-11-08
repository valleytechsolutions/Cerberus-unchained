#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "wifi_archive_capture.h"

static unsigned checks;
#define CHECK(x) do { ++checks; if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);assert(x);} } while(0)
static cerberus_wifi_archive_t doc,decoded,prior,subset;
static cerberus_dual_wifi_snapshot_t snap;
static uint8_t wire[CERBERUS_WIFI_ARCHIVE_MAX_SIZE+1],original[sizeof(wire)],encoded[sizeof(wire)];

/* Independent CRC reference, polynomial long division rather than codec's
 * reflected update. Every malformed-vector repair still covers whole file. */
static uint32_t reverse(uint32_t value,unsigned bits)
{ uint32_t out=0;while(bits--){out=(out<<1)|(value&1);value>>=1;}return out; }
static uint32_t reference_crc(const uint8_t *bytes,size_t n)
{
    uint32_t c=UINT32_MAX;
    for(size_t i=0;i<n;++i){c^=reverse(i>=20&&i<24?0:bytes[i],8)<<24;
        for(unsigned b=0;b<8;++b)c=(c<<1)^((c&UINT32_C(0x80000000))?UINT32_C(0x04c11db7):0);}
    return ~reverse(c,32);
}
static void repair(size_t n)
{ uint32_t c=reference_crc(wire,n);for(unsigned i=0;i<4;++i)wire[20+i]=(uint8_t)(c>>(8*i)); }
static void seed(unsigned count)
{
    memset(&snap,0,sizeof(snap));snap.count=(uint8_t)count;
    snap.publication=(cerberus_wifi_archive_publication_t){
        .received_us=UINT64_C(0x100000010)*1000,.c5_session_id=UINT64_C(0x100000002),
        .c5_relation_epoch=UINT64_C(0x200000003),.generation=19,.elapsed_ms=4011,
        .s3_total=49,.c5_total=61,.s3_count=(uint8_t)(count/2),.c5_count=(uint8_t)(count-count/2),
        .s3_coverage=1,.c5_coverage=2,.split=true};
    for(unsigned i=0;i<count;++i){cerberus_dual_wifi_row_t *r=&snap.rows[i];
        bool s3=i<count/2;r->source=s3?1:2;r->source_index=(uint8_t)(s3?i:i-count/2);
        r->source_generation=s3?UINT32_MAX:0; /* C5 gen8 wrap is valid historical evidence. */
        r->received_ms=(uint32_t)(snap.publication.received_us/1000);r->ap.bssid[0]=2;r->ap.bssid[5]=(uint8_t)(i%32);
        memcpy(r->ap.ssid,"same SSID",10);r->ap.channel=s3?6:149;r->ap.security=3;r->ap.rssi=-45;
        if(!s3)r->observation=(cerberus_s3_wifi_observation_t){.valid=true,
            .request_started_us=snap.publication.received_us-1999,.source_age_ms=6};
    }
}
static void reject_decode(size_t n)
{ memset(&decoded,0x5a,sizeof(decoded));prior=decoded;CHECK(!cerberus_wifi_archive_decode(wire,n,&decoded));CHECK(!memcmp(&decoded,&prior,sizeof(prior))); }
static void reject_model(void)
{ memset(encoded,0x66,sizeof(encoded));size_t n=999;CHECK(!cerberus_wifi_archive_validate(&decoded));
  CHECK(!cerberus_wifi_archive_encode(&decoded,encoded,sizeof(encoded),&n));CHECK(n==0);
  for(size_t i=0;i<sizeof(encoded);++i)CHECK(encoded[i]==0x66); }

static void codec_cases(void)
{
    seed(64);CHECK(cerberus_wifi_archive_capture(&snap,(int64_t)snap.publication.received_us+1,&doc));
    CHECK(doc.rows[32].frame_age_upper_at_save_ms==8);CHECK(doc.count==64&&!doc.subset);
    size_t n=0;CHECK(cerberus_wifi_archive_encode(&doc,wire,sizeof(wire),&n));CHECK(n==5216);
    CHECK(cerberus_wifi_archive_decode(wire,n,&decoded));CHECK(!memcmp(&doc,&decoded,sizeof(doc)));
    CHECK(cerberus_wifi_archive_encode(&decoded,encoded,sizeof(encoded),&n));CHECK(!memcmp(encoded,wire,n));
    memcpy(original,wire,sizeof(wire));
    /* Every truncated length and every corrupted byte fails without mutation. */
    for(size_t length=0;length<5216;++length)reject_decode(length);
    reject_decode(5217);
    for(size_t i=0;i<5216;++i){wire[i]^=0x80;reject_decode(5216);wire[i]^=0x80;}
    const unsigned offsets[]={8,10,12,14,16,76,77,78,79,80,81,82,95,96,97,98,99,100,106,107,109,132};
    for(unsigned i=0;i<sizeof(offsets)/sizeof(offsets[0]);++i){memcpy(wire,original,sizeof(wire));wire[offsets[i]]=255;repair(5216);reject_decode(5216);}
    memcpy(wire,original,sizeof(wire));wire[176+1]=0;repair(5216);reject_decode(5216); /* duplicate source index */
    memcpy(wire,original,sizeof(wire));wire[176+16]^=1;repair(5216);reject_decode(5216); /* mixed generation */
    memcpy(wire,original,sizeof(wire));
    CHECK(!cerberus_wifi_archive_decode(wire,5216,(cerberus_wifi_archive_t*)wire));
    size_t written=0;CHECK(!cerberus_wifi_archive_encode(&doc,(uint8_t*)&doc,sizeof(doc),&written));
    CHECK(!cerberus_wifi_archive_encode(&doc,wire,sizeof(wire),(size_t*)&doc));
    CHECK(!cerberus_wifi_archive_encode(&doc,wire,sizeof(wire),(size_t*)wire));
    written=1;CHECK(!cerberus_wifi_archive_encode(NULL,wire,sizeof(wire),&written)&&written==0);
    written=1;CHECK(!cerberus_wifi_archive_encode(&doc,NULL,sizeof(wire),&written)&&written==0);
    for(size_t cap=0;cap<5216;cap+=79){memset(wire,0x55,sizeof(wire));written=1;
        CHECK(!cerberus_wifi_archive_encode(&doc,wire,cap,&written));CHECK(written==0);CHECK(wire[0]==0x55&&wire[5216]==0x55);}
    for(unsigned bad=0;bad<15;++bad){decoded=doc;switch(bad){
        case 0:decoded.rows[0].channel=15;break;case 1:decoded.rows[32].channel=6;break;
        case 2:decoded.rows[0].security=10;break;case 3:decoded.rows[0].bssid[0]=3;break;
        case 4:memset(decoded.rows[0].bssid,0,6);break;case 5:decoded.rows[0].ssid[32]='x';break;
        case 6:decoded.rows[0].source=0;break;case 7:decoded.rows[0].source_index=32;break;
        case 8:decoded.rows[0].rssi_valid=false;break;case 9:decoded.rows[0].frame_present=true;break;
        case 10:decoded.rows[32].source_generation=256;break;case 11:decoded.rows[32].frame_age_upper_at_save_ms++;break;
        case 12:decoded.publication.s3_total=1;break;case 13:decoded.subset=true;break;
        case 14:((uint8_t*)&decoded.saved_time_valid)[0]=2;break;}reject_model();}
    /* Source-identical AP bytes are retained independently, no deduplication. */
    CHECK(!memcmp(doc.rows[0].bssid,doc.rows[32].bssid,6));
    CHECK(cerberus_wifi_archive_subset(&doc,UINT64_C(1)|(UINT64_C(1)<<32),&subset));
    CHECK(subset.count==2&&subset.subset&&subset.publication.s3_count==32&&subset.publication.c5_count==32);
    CHECK(subset.rows[0].source==1&&subset.rows[1].source==2&&cerberus_wifi_archive_validate(&subset));
    CHECK(cerberus_wifi_archive_subset(&subset,0,&decoded));CHECK(!decoded.count&&decoded.subset&&cerberus_wifi_archive_validate(&decoded));
    CHECK(!cerberus_wifi_archive_subset(&subset,4,&decoded));CHECK(!cerberus_wifi_archive_subset(&doc,1,&doc));
    CHECK(cerberus_wifi_archive_subset(&subset,UINT64_MAX,&decoded));CHECK(decoded.count==2&&decoded.subset);
    /* Exactly32 name bytes, controls/nonUTF8 retained; no false full raw identity. */
    for(unsigned length=0;length<=32;++length){seed(2);memset(snap.rows[0].ap.ssid,0,33);
        for(unsigned i=0;i<length;++i)snap.rows[0].ap.ssid[i]=(char)(i+128);
        CHECK(cerberus_wifi_archive_capture(&snap,(int64_t)snap.publication.received_us,&doc));
        CHECK(cerberus_wifi_archive_encode(&doc,wire,sizeof(wire),&n));CHECK(cerberus_wifi_archive_decode(wire,n,&decoded));
        CHECK(!memcmp(doc.rows[0].ssid,decoded.rows[0].ssid,33));}
    for(unsigned c=1;c<=255;++c){seed(2);snap.rows[0].ap.channel=(uint8_t)c;
        CHECK(cerberus_wifi_archive_capture(&snap,(int64_t)snap.publication.received_us,&doc)==(c<=14));}
    for(unsigned c=1;c<=255;++c){seed(2);snap.rows[1].ap.channel=(uint8_t)c;
        bool valid=(c>=36&&c<=64&&c%4==0)||(c>=100&&c<=144&&c%4==0)||(c>=149&&c<=177&&(c-149)%4==0);
        CHECK(cerberus_wifi_archive_capture(&snap,(int64_t)snap.publication.received_us,&doc)==valid);}
}
static void capture_cases(void)
{
    seed(64);uint64_t received=snap.publication.received_us;
    CHECK(cerberus_wifi_archive_capture_rows(&snap.publication,snap.rows,snap.count,(int64_t)received,
        UINT64_C(1)<<63,&doc));CHECK(doc.count==1&&doc.rows[0].source_index==31&&doc.subset);
    CHECK(doc.publication.c5_session_id>UINT32_MAX&&doc.rows[0].frame_age_upper_at_save_ms==8);
    prior=doc;snap.rows[63].ap.ssid[0]='x';snap.publication.c5_session_id++;
    CHECK(!memcmp(&doc,&prior,sizeof(doc))); /* committed copy has no current-bank borrowing */
    for(unsigned bad=0;bad<7;++bad){seed(2);switch(bad){
        case 0:snap.rows[0].source_index=1;break;case 1:snap.rows[1].observation.request_started_us=received+1;break;
        case 2:snap.rows[0].observation.source_age_ms=1;break;case 3:snap.count=3;break;
        case 4:memset(snap.rows[0].ap.ssid,'a',33);break;case 5:snap.publication.c5_relation_epoch=0;break;
        case 6:snap.publication.received_us=UINT64_MAX;break;}
        memset(&doc,0x5c,sizeof(doc));prior=doc;CHECK(!cerberus_wifi_archive_capture(&snap,(int64_t)received,&doc));CHECK(!memcmp(&prior,&doc,sizeof(doc)));}
    seed(2);CHECK(!cerberus_wifi_archive_capture_rows(&snap.publication,snap.rows,2,(int64_t)received,4,&doc));
    CHECK(!cerberus_wifi_archive_capture(&snap,(int64_t)received,(cerberus_wifi_archive_t*)&snap));
    CHECK(cerberus_wifi_archive_capture(&snap,-1,&doc));CHECK(!doc.saved_time_valid&&!doc.saved_us&&!doc.rows[1].frame_age_upper_valid);
    CHECK(cerberus_wifi_archive_capture(&snap,(int64_t)received-1,&doc));CHECK(!doc.saved_time_valid&&doc.publication.received_us==received);
    snap.rows[1].observation.source_age_ms=UINT32_MAX;
    CHECK(cerberus_wifi_archive_capture(&snap,INT64_MAX,&doc));CHECK(!doc.rows[1].frame_age_upper_valid&&doc.rows[1].frame_present);
    snap.rows[1].observation.source_age_ms=UINT32_MAX-1;
    CHECK(cerberus_wifi_archive_capture(&snap,INT64_MAX,&doc));CHECK(doc.rows[1].frame_age_upper_valid&&cerberus_wifi_archive_validate(&doc));
    seed(2);snap.publication.received_us=0;snap.rows[1].observation.request_started_us=0;snap.rows[1].observation.source_age_ms=1000;
    CHECK(cerberus_wifi_archive_capture(&snap,0,&doc));CHECK(doc.saved_time_valid&&doc.rows[1].frame_age_upper_at_save_ms==1000);
    /* Empty complete bank differs from unavailable; zero-row subset retains coverage. */
    seed(0);CHECK(cerberus_wifi_archive_capture(&snap,(int64_t)snap.publication.received_us,&doc));CHECK(!doc.count&&!doc.subset);
    CHECK(cerberus_wifi_archive_validate(&doc));
}
static void golden_case(void)
{
    /* Independently specified empty fallback document, zlib CRC32. */
    static const uint8_t golden[96]={
        0x43,0x57,0x46,0x49,0x41,0x52,0x43,0x00,0x01,0x00,0x60,0x00,0x50,0x00,0x00,0x00,
        0x60,0x00,0x00,0x00,0x5e,0x97,0xb7,0xac,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,2,0,0,0,0,0,0,0,
        1,0,0,0,0,0,0,0,3,1,0,0,0,0,0,0,0,0,0,0,0,0,0,3,
        0,10,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
    CHECK(cerberus_wifi_archive_decode(golden,sizeof(golden),&doc));
    CHECK(doc.publication.c5_session_id==1&&doc.publication.c5_relation_epoch==2&&doc.publication.s3_error==259);
    CHECK(doc.publication.fallback&&doc.saved_time_valid&&!doc.count&&!doc.subset);
    size_t n=0;CHECK(cerberus_wifi_archive_encode(&doc,wire,sizeof(wire),&n));
    CHECK(n==sizeof(golden)&&!memcmp(wire,golden,n));
}
int main(void)
{
    codec_cases();capture_cases();golden_case();
    printf("Wi-Fi archive: %u checks PASS; publication=%zu row=%zu document=%zu snapshot=%zu wiremax=%u\n",
        checks,sizeof(cerberus_wifi_archive_publication_t),sizeof(cerberus_wifi_archive_row_t),
        sizeof(cerberus_wifi_archive_t),sizeof(cerberus_dual_wifi_snapshot_t),CERBERUS_WIFI_ARCHIVE_MAX_SIZE);
    return 0;
}
