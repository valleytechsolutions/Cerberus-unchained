#include "mesh_archive.h"
#include "mesh_report_fixture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(c) do { ++checks;if(!(c)){fprintf(stderr,"mesh archive line %d: %s\n",__LINE__,#c);exit(1);} } while(0)
static mesh_scan_t scan,scan_before;
static mesh_archive_t document,decoded,scratch,saved,bad;
static uint8_t encoded[MESH_ARCHIVE_MAX_BYTES+1U],again[MESH_ARCHIVE_MAX_BYTES+1U],mutated[MESH_ARCHIVE_MAX_BYTES+1U];
static char live_csv[MESH_REPORT_MAX_BYTES+1U],archive_csv[MESH_REPORT_MAX_BYTES+1U];

/* Independent reflected CRC reference, including the documented zero CRC slot. */
static uint32_t crc(const uint8_t *bytes,size_t length)
{
    uint32_t c=UINT32_MAX;
    for(size_t i=0;i<length;++i){
        uint8_t b=i>=24U && i<28U?0U:bytes[i];
        for(unsigned j=0;j<8U;++j){bool low=((c^b)&1U)!=0U;c>>=1U;if(low)c^=0xedb88320U;b>>=1U;}
    }
    return ~c;
}
static void put(uint8_t *p,uint64_t value,unsigned width)
{ for(unsigned i=0;i<width;++i)p[i]=(uint8_t)(value>>(8U*i)); }
static uint64_t get(const uint8_t *p,unsigned width)
{ uint64_t v=0U;for(unsigned i=0;i<width;++i)v|=(uint64_t)p[i]<<(8U*i);return v; }
static void fix_crc(uint8_t *p,size_t length){put(p+24U,crc(p,length),4U);}
static void write_file(const char *directory,const char *name,const void *data,size_t size)
{
    char path[1024];CHECK(snprintf(path,sizeof(path),"%s/%s",directory,name)>0);
    FILE *f=fopen(path,"wb");CHECK(f);CHECK(fwrite(data,1U,size,f)==size);CHECK(fclose(f)==0);
}
static size_t encode_current(void)
{
    size_t needed=999U,written=0U;
    CHECK(mesh_report_valid(&scan));scan_before=scan;
    CHECK(mesh_archive_from_scan(&scan,&document));CHECK(mesh_archive_valid(&document));
    CHECK(!memcmp(&scan,&scan_before,sizeof(scan)));
    CHECK(mesh_archive_encode(&document,NULL,0U,&needed));
    CHECK(needed>=MESH_ARCHIVE_MIN_BYTES && needed<=MESH_ARCHIVE_MAX_BYTES);
    memset(encoded,0xa5,sizeof(encoded));CHECK(mesh_archive_encode(&document,encoded,needed,&written));
    CHECK(written==needed && encoded[needed]==0xa5U);
    CHECK(get(encoded+16U,4U)==needed && get(encoded+20U,4U)==needed-32U);
    CHECK(get(encoded+24U,4U)==crc(encoded,needed));
    saved=document;
    memset(&decoded,0xa5,sizeof(decoded));memset(&scratch,0x5a,sizeof(scratch));
    CHECK(mesh_archive_decode(encoded,needed,&decoded,&scratch));CHECK(mesh_archive_valid(&decoded));
    CHECK(!memcmp(&document,&saved,sizeof(saved)));
    CHECK(mesh_archive_encode(&decoded,again,sizeof(again),&written));CHECK(written==needed);
    CHECK(!memcmp(encoded,again,needed));
    size_t live_size=0U,archive_size=0U;
    CHECK(mesh_report_encode(&scan,live_csv,sizeof(live_csv),&live_size));
    CHECK(mesh_archive_report_encode(&decoded,archive_csv,sizeof(archive_csv),&archive_size));
    CHECK(live_size==archive_size && !memcmp(live_csv,archive_csv,live_size+1U));
    CHECK(decoded.first_callback_us==scan.first_callback_us && decoded.last_callback_us==scan.last_callback_us);
    CHECK(decoded.observed_mask==scan.observed_mask && decoded.callback_time_seen==scan.callback_time_seen);
    CHECK(decoded.model.session==scan.model.session && decoded.model.counters.records==scan.model.counters.records);
    return needed;
}
static void reject_bytes(const uint8_t *bytes,size_t length)
{
    memset(&decoded,0xa5,sizeof(decoded));saved=decoded;
    CHECK(!mesh_archive_decode(bytes,length,&decoded,&scratch));
    CHECK(!memcmp(&decoded,&saved,sizeof(saved)));
}
static void roundtrips(const char *directory)
{
    size_t maximum=0U;
    for(unsigned records=0U;records<=100U;++records){
        CHECK(mesh_report_test_fill(&scan,records));size_t n=encode_current();
        if(n>maximum)maximum=n;
        if(!records){write_file(directory,"empty.msh",encoded,n);write_file(directory,"current-0.csv",archive_csv,strlen(archive_csv));}
        if(records==80U){
            CHECK(n==MESH_ARCHIVE_MAX_BYTES);
            CHECK(decoded.model.counters.witness_omissions==16U);
            CHECK(decoded.model.last_sequence==80U && decoded.model.witnesses[63].input.sequence==64U);
            CHECK(decoded.model.pan_count==16U && decoded.model.address_count==64U && decoded.model.edge_count==64U);
            bool late=false;
            for(unsigned i=0;i<decoded.model.pan_count;++i)if(decoded.model.pans[i].seen.last_sequence>64U){
                CHECK(decoded.model.pans[i].seen.last_witness==255U);late=true;
            }
            CHECK(late);write_file(directory,"maximum.msh",encoded,n);
            write_file(directory,"current-80.csv",archive_csv,strlen(archive_csv));
        }
        memset(again,0x5a,sizeof(again));memcpy(mutated,again,sizeof(again));size_t written=77U;
        CHECK(!mesh_archive_encode(&document,again,n-1U,&written) && written==0U);
        CHECK(!memcmp(again,mutated,sizeof(again)));
    }
    CHECK(maximum==MESH_ARCHIVE_MAX_BYTES);
    CHECK(mesh_report_test_fill(&scan,1000U));encode_current();
    write_file(directory,"current-1000.csv",archive_csv,strlen(archive_csv));
    CHECK(decoded.model.counters.witness_omissions==936U);
    /* Aggregate endpoint evidence still survives after its latest raw witness is
     * unavailable; first64 alone must never be used to reconstruct this map. */
    CHECK(decoded.model.pan_count==16U && decoded.model.counters.address_omissions>0U);
    /* Unused native rows are neither serialized nor interpreted. */
    CHECK(mesh_report_test_fill(&scan,0U));size_t empty=encode_current();
    memset(document.model.pans,0xa5,sizeof(document.model.pans));
    memset(document.model.addresses,0xa5,sizeof(document.model.addresses));
    memset(document.model.edges,0xa5,sizeof(document.model.edges));
    memset(document.model.witnesses,0xa5,sizeof(document.model.witnesses));
    size_t written=0U;CHECK(mesh_archive_encode(&document,again,sizeof(again),&written));
    CHECK(written==empty && !memcmp(encoded,again,empty));
}
static void one_record(const uint8_t *mac,size_t length)
{
    CHECK(mesh_report_test_fill(&scan,0U));
    mesh_observation_input_t input={0};input.session=scan.model.session;input.sequence=1U;
    input.callback_us=1000U;input.channel=11U;input.flags=7U;input.rssi=-128;input.lqi=255U;
    input.length=(uint8_t)length;memcpy(input.mac,mac,length);
    mesh_observation_result_t result=mesh_observation_ingest(&scan.model,&input);
    CHECK(result==MESH_OBSERVATION_OK || result==MESH_OBSERVATION_MALFORMED || result==MESH_OBSERVATION_UNSUPPORTED);
    scan.acked_sequence=scan.last_enqueued=1U;scan.observed_mask=1U;
    scan.callback_time_seen=true;scan.first_callback_us=scan.last_callback_us=1000U;
    scan.final_summary.callbacks=scan.final_summary.enqueued=scan.final_summary.acked=scan.final_summary.last_sequence=1U;
}
static void provenance(const char *directory)
{
    const uint8_t known_zero[]={0x41,0x88,0x55,0,0,0x44,0x33,0x22,0x11};
    one_record(known_zero,sizeof(known_zero));size_t n=encode_current();
    CHECK(decoded.model.pans[0].key.known && decoded.model.pans[0].key.value==0U);
    CHECK(decoded.model.pans[0].origin_bits==6U && decoded.model.addresses[0].key.mode==MESH_MAC_ADDRESS_SHORT16);
    write_file(directory,"known-zero.msh",encoded,n);
    const uint8_t malformed[]={0x41,0x88};one_record(malformed,sizeof(malformed));encode_current();
    CHECK(decoded.model.witnesses[0].parse_result==MESH_MAC_MALFORMED && decoded.model.counters.malformed==1U);
    const uint8_t unsupported[]={4,0};one_record(unsupported,sizeof(unsupported));encode_current();
    CHECK(decoded.model.witnesses[0].parse_result==MESH_MAC_UNSUPPORTED && !decoded.model.address_count);
    const uint8_t addressless[]={1,0,3};one_record(addressless,sizeof(addressless));encode_current();
    CHECK(decoded.model.counters.headers==1U && !decoded.model.address_count && !decoded.model.pan_count);
    const uint8_t self[]={0x41,0x88,0x55,0,0,0x22,0x11,0x22,0x11};one_record(self,sizeof(self));encode_current();
    CHECK(decoded.model.address_count==1U && decoded.model.addresses[0].source_frames==1U &&
        decoded.model.addresses[0].destination_frames==1U && decoded.model.addresses[0].seen.frames==1U);
    CHECK(mesh_report_test_fill(&scan,80U));scan.final_summary.error_code=scan.error=INT32_MIN;encode_current();
    CHECK(decoded.final_summary.error_code==INT32_MIN);
    scan.final_summary.queue_drops=scan.final_summary.callbacks=UINT32_MAX;
    scan.final_summary.flags|=CERBERUS_MESH_OBSERVATION_COUNTERS_SATURATED;encode_current();
    CHECK(decoded.final_summary.queue_drops==UINT32_MAX);
    CHECK(mesh_report_test_fill(&scan,0U));scan.final_summary.flags=CERBERUS_MESH_OBSERVATION_STOPPED|
        CERBERUS_MESH_OBSERVATION_SUMMARY_CLOCK_INVALID;
    scan.final_summary.started_us=scan.final_summary.stopped_us=scan.final_summary.copied_us=0U;
    encode_current();CHECK(!decoded.callback_time_seen && decoded.last_callback_us==0U);
}
static void corruption(void)
{
    CHECK(mesh_report_test_fill(&scan,80U));size_t n=encode_current();
    /* Every truncation rejects, including all exact section boundaries. */
    for(size_t length=0U;length<n;++length)reject_bytes(encoded,length);
    memcpy(mutated,encoded,n);mutated[n]=0U;reject_bytes(mutated,n+1U);
    /* CRC detects every individual byte alteration of the maximum document. */
    for(size_t offset=0U;offset<n;++offset){
        memcpy(mutated,encoded,n);mutated[offset]^=1U;reject_bytes(mutated,n);
    }
    /* Recomputed CRC cannot bypass semantic/version/boolean/reserved/reference
     * rejection. Offsets below are independent literal format expectations. */
    const struct {size_t offset;uint64_t value;unsigned width;} changes[]={
        {8,2,2},{10,31,2},{12,2,2},{14,1,2},{16,268,4},{20,0,4},{28,1,4},
        {50,2,1},{51,1,1},{55,1,1},{56,2,1},{57,0x80,1},{58,2,1},{59,11,1},
        {60,0,2},{64,99,2},{66,1,2},{80,1,8},{96,99,4},{108,0,4},
        {184,0,8},{200,81,4},{204,17,1},{205,65,1},{206,65,1},{207,65,1},
        {208,2,1},{209,1,1},{211,1,1},{212,79,4},{216,81,4},{264,2,1},{265,1,1},
        {268+3,2,1},{268+2,27,1},{268+4+28,64,1},{268+4+30,2,1},
        {860+12,1,1},{860+41,64,1},{860+34,0xffffffffU,4},
        {5340+32,64,1},{5340+33,255,1},{5340+48,2,1},
        {8476,1,8},{8476+16,0,4},{8476+20,10,1},{8476+21,8,1},
        {8476+22,126,1},{8476+25,1,1},{8476+153,3,1}
    };
    for(size_t i=0;i<sizeof(changes)/sizeof(changes[0]);++i){
        memcpy(mutated,encoded,n);put(mutated+changes[i].offset,changes[i].value,changes[i].width);fix_crc(mutated,n);
        reject_bytes(mutated,n);
    }
    /* A smaller complete file cannot hide appended valid-looking sections. */
    CHECK(mesh_report_test_fill(&scan,0U));n=encode_current();memcpy(mutated,encoded,n);mutated[n]=0U;
    put(mutated+16U,n+1U,4U);put(mutated+20U,n+1U-32U,4U);fix_crc(mutated,n+1U);reject_bytes(mutated,n+1U);
    const uint8_t short_mac[]={4,0};one_record(short_mac,sizeof(short_mac));n=encode_current();
    memcpy(mutated,encoded,n);mutated[n-2U]=1U;fix_crc(mutated,n);reject_bytes(mutated,n);
}
static void invalid_and_alias(void)
{
    CHECK(mesh_report_test_fill(&scan,80U));size_t n=encode_current();saved=document;
    for(unsigned i=0;i<12U;++i){bad=document;
        switch(i){
        case 0:bad.model.address_count=255U;break;
        case 1:bad.model.edges[0].source=255U;break;
        case 2:bad.model.witnesses[0].input.sequence=0U;break;
        case 3:bad.model.witnesses[0].input.reserved[0]=1U;break;
        case 4:bad.model.witnesses[0].parse_result=MESH_MAC_INVALID_ARGUMENT;break;
        case 5:bad.model.counters.witness_omissions=0U;break;
        case 6:bad.model.pans[0].seen.first_witness=255U;break;
        case 7:bad.final_summary.enqueued++;break;
        case 8:bad.final_summary.reserved=1U;break;
        case 9:bad.model.addresses[1]=bad.model.addresses[0];break;
        case 10:bad.model.edges[1]=bad.model.edges[0];break;
        case 11:bad.model.counters.wrong_session=1U;break;
        }
        CHECK(!mesh_archive_valid(&bad));memset(again,0x5a,sizeof(again));memcpy(mutated,again,sizeof(again));size_t written=99U;
        CHECK(!mesh_archive_encode(&bad,again,sizeof(again),&written) && !written);CHECK(!memcmp(again,mutated,sizeof(again)));
    }
    /* Active/foreign/faulted controllers cannot manufacture an archive. */
    for(unsigned i=0;i<8U;++i){scan_before=scan;
        switch(i){case 0:scan.frozen=false;break;case 1:scan.final_valid=false;break;case 2:scan.faulted=true;break;
        case 3:scan.requested=true;break;case 4:scan.request.session_id=scan.model.session;break;
        case 5:scan.remote.session_id++;break;case 6:scan.remote.flags=1U;break;case 7:scan.phase=MESH_SCAN_READ_ROW;break;}
        CHECK(!mesh_archive_from_scan(&scan,&document));CHECK(!memcmp(&document,&saved,sizeof(saved)));scan=scan_before;
    }
    size_t written=9U;CHECK(!mesh_archive_encode(&document,(uint8_t *)&document,sizeof(document),&written));
    CHECK(!memcmp(&document,&saved,sizeof(saved)));
    CHECK(!mesh_archive_encode(&document,again,sizeof(again),(size_t *)&document));CHECK(!memcmp(&document,&saved,sizeof(saved)));
    CHECK(!mesh_archive_from_scan(&scan,(mesh_archive_t *)&scan));CHECK(!memcmp(&scan,&scan_before,sizeof(scan)));
    CHECK(!mesh_archive_decode(encoded,n,&document,&document));CHECK(!memcmp(&document,&saved,sizeof(saved)));
    CHECK(!mesh_archive_decode((const uint8_t *)&document,n,&document,&scratch));CHECK(!memcmp(&document,&saved,sizeof(saved)));
    CHECK(!mesh_archive_decode((const uint8_t *)&document,n,&decoded,&document));CHECK(!memcmp(&document,&saved,sizeof(saved)));
    CHECK(!mesh_archive_decode(encoded,n,NULL,&scratch));CHECK(!mesh_archive_decode(encoded,n,&decoded,NULL));
    CHECK(!mesh_archive_decode(NULL,n,&decoded,&scratch));CHECK(!mesh_archive_valid(NULL));
    CHECK(!mesh_archive_encode(NULL,again,sizeof(again),&written));CHECK(!mesh_archive_encode(&document,NULL,1U,&written));
    CHECK(!mesh_archive_encode(&document,again,sizeof(again),NULL));
    union {size_t length;uint8_t bytes[32];} alias;memset(&alias,0x55,sizeof(alias));
    uint8_t old[sizeof(alias)];memcpy(old,&alias,sizeof(alias));
    CHECK(!mesh_archive_encode(&document,alias.bytes,sizeof(alias),&alias.length));CHECK(!memcmp(&alias,old,sizeof(alias)));
    mesh_evidence_view_t v={&document.model,&document.final_summary,document.first_callback_us,
        document.last_callback_us,document.observed_mask,document.callback_time_seen};
    CHECK(!mesh_evidence_report_encode(&v,(char *)&document.final_summary,sizeof(document.final_summary),&written));
    CHECK(!mesh_evidence_report_encode(&v,archive_csv,sizeof(archive_csv),(size_t *)&document.model));
    CHECK(!mesh_evidence_report_encode(&v,(char *)&v,sizeof(v),&written));
    CHECK(!mesh_archive_report_encode(&document,(char *)&document,sizeof(document),&written));
    CHECK(!memcmp(&document,&saved,sizeof(saved)));
}
static void bounded_mutations(void)
{
    CHECK(mesh_report_test_fill(&scan,80U));size_t n=encode_current();uint32_t random=0x4827c51U;
    unsigned accepted=0U,rejected=0U;
    for(unsigned i=0;i<512U;++i){
        memcpy(mutated,encoded,n);random=random*1664525U+1013904223U;size_t at=32U+random%(n-32U);
        random=random*1664525U+1013904223U;mutated[at]^=(uint8_t)((random>>24U)|1U);fix_crc(mutated,n);
        memset(&decoded,0xa5,sizeof(decoded));saved=decoded;
        if(mesh_archive_decode(mutated,n,&decoded,&scratch)){
            ++accepted;CHECK(mesh_archive_valid(&decoded));size_t written=0U;
            CHECK(mesh_archive_encode(&decoded,again,sizeof(again),&written));
            CHECK(written==n && !memcmp(mutated,again,n));
            CHECK(mesh_archive_report_encode(&decoded,archive_csv,sizeof(archive_csv),&written));
            CHECK(written<=MESH_REPORT_MAX_BYTES);
        }else{++rejected;CHECK(!memcmp(&decoded,&saved,sizeof(saved)));}
    }
    /* Opaque payload changes can be valid. A CRC is not a signature or proof of
     * capture authenticity; structural violations must still reject. */
    CHECK(accepted>0U && rejected>0U);
}
static void callback_clocks(void)
{
    CHECK(mesh_report_test_fill(&scan,0U));size_t n=encode_current();
    memcpy(mutated,encoded,n);put(mutated+32U,1000U,8U);put(mutated+40U,1000U,8U);
    mutated[50]=1U;put(mutated+192U,1000U,8U);mutated[208]=1U;fix_crc(mutated,n);reject_bytes(mutated,n);
    document.callback_time_seen=document.model.time_seen=true;
    document.first_callback_us=document.last_callback_us=document.model.last_valid_us=1000U;
    CHECK(!mesh_archive_valid(&document));
    for(unsigned count=1U;count<=80U;count+=79U){
        CHECK(mesh_report_test_fill(&scan,count));n=encode_current();
        memcpy(mutated,encoded,n);put(mutated+32U,999U,8U);fix_crc(mutated,n);reject_bytes(mutated,n);
        document.first_callback_us=999U;CHECK(!mesh_archive_valid(&document));
    }
    CHECK(mesh_report_test_fill(&scan,1U));CHECK(mesh_archive_from_scan(&scan,&document));
    ++document.last_callback_us;++document.model.last_valid_us;CHECK(!mesh_archive_valid(&document));

    /* Zero is a valid timestamp when its flag says so; all first64 clocks can
     * also be unknown with the first valid timestamp arriving only afterward. */
    for(unsigned scenario=0U;scenario<3U;++scenario){
        CHECK(mesh_report_test_fill(&scan,0U));scan.final_summary.started_us=0U;
        unsigned count=scenario==2U?65U:2U;
        for(unsigned i=0U;i<count;++i){
            mesh_observation_input_t input={0};input.session=scan.model.session;input.sequence=i+1U;
            input.channel=11U;input.length=3U;input.mac[0]=1U;input.mac[2]=(uint8_t)i;
            bool timed=scenario==0U || (scenario==2U && i==64U);
            input.flags=timed?MESH_OBSERVATION_TIME_VALID:0U;
            input.callback_us=timed?i:0U;
            CHECK(mesh_observation_ingest(&scan.model,&input)==MESH_OBSERVATION_OK);
            if(timed){if(!scan.callback_time_seen)scan.first_callback_us=input.callback_us;
                scan.callback_time_seen=true;scan.last_callback_us=input.callback_us;}
        }
        scan.acked_sequence=scan.last_enqueued=count;scan.observed_mask=1U;
        scan.final_summary.callbacks=scan.final_summary.enqueued=scan.final_summary.acked=scan.final_summary.last_sequence=count;
        encode_current();
        if(scenario==0U)CHECK(decoded.callback_time_seen && decoded.first_callback_us==0U && decoded.last_callback_us==1U);
        if(scenario==1U)CHECK(!decoded.callback_time_seen && !decoded.first_callback_us && !decoded.last_callback_us);
        if(scenario==2U)CHECK(decoded.callback_time_seen && decoded.first_callback_us==64U && decoded.last_callback_us==64U && decoded.model.witness_count==64U);
    }
}
static void golden(const char *path)
{
    FILE *f=fopen(path,"rb");CHECK(f);size_t n=fread(mutated,1U,sizeof(mutated),f);CHECK(!ferror(f));CHECK(fclose(f)==0);
    CHECK(n==MESH_ARCHIVE_MIN_BYTES);CHECK(mesh_archive_decode(mutated,n,&decoded,&scratch));
    CHECK(decoded.model.session==UINT64_C(0x1234567887654321));CHECK(decoded.final_summary.stopped_us==2000000U);
    CHECK(mesh_report_test_fill(&scan,0U));size_t actual=encode_current();CHECK(n==actual && !memcmp(mutated,encoded,n));
}
int main(int argc,char **argv)
{
    CHECK(argc==3);roundtrips(argv[1]);provenance(argv[1]);corruption();invalid_and_alias();bounded_mutations();callback_clocks();golden(argv[2]);
    printf("mesh archive: %u checks PASS; document=%zu bytes; wire maximum=%u\n",checks,sizeof(mesh_archive_t),MESH_ARCHIVE_MAX_BYTES);
    return 0;
}
