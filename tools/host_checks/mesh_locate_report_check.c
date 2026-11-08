#undef NDEBUG
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include "mesh_locate_report.h"
#include "mesh_locate_fixture.h"
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"Locate CSV %u: %s\n",__LINE__,#x); exit(1); } } while(0)
static mesh_locate_t source, before;
static char output[MESH_LOCATE_REPORT_MAX_BYTES + 1U], saved[MESH_LOCATE_REPORT_MAX_BYTES + 1U];
static void fill(unsigned count,uint32_t stride,esp_err_t error)
{
    CHECK(mesh_locate_test_fill_options(&source,count,stride,error));
    CHECK(mesh_locate_report_valid(&source)); before=source;
}
static void rejected(void)
{
    size_t length=99U;
    memset(output,0xCD,sizeof(output)); memcpy(saved,output,sizeof(output));
    CHECK(!mesh_locate_report_encode(&source,output,sizeof(output),&length));
    CHECK(!length && !memcmp(output,saved,sizeof(output)));
}
int main(void)
{
    const unsigned counts[]={0,1,32,40};
    size_t largest=0;
    for(size_t n=0;n<sizeof(counts)/sizeof(counts[0]);++n) {
        for(unsigned coalesce=1;coalesce<=3;coalesce+=2) {
            fill(counts[n],coalesce,ESP_OK);
            size_t measured=0,length=0;
            CHECK(mesh_locate_report_encode(&source,NULL,0,&measured));
            CHECK(measured>500U && measured<MESH_LOCATE_REPORT_MAX_BYTES);
            CHECK(mesh_locate_report_encode(&source,output,sizeof(output),&length));
            CHECK(length==measured && output[length]==0 && strlen(output)==length);
            CHECK(!memcmp(&source,&before,sizeof(source)));
            CHECK(strstr(output,"# CERBERUS_MESH_LOCATE_CSV,1\r\n")==output);
            CHECK(strstr(output,"SHORT16,0000000000001122,11,1,1234\r\n"));
            CHECK(strstr(output,"0000000000000ABC,0000000000000101,4,0\r\n"));
            CHECK(strstr(output,"not authenticated device identity or network membership"));
            CHECK(strstr(output,"no distance or bearing is inferred"));
            char expected[160];
            snprintf(expected,sizeof(expected),"%u,%u,%u,%u\r\nindex,match_sequence",
                counts[n]>32U?32U:counts[n],counts[n],counts[n]*(coalesce-1U),counts[n]>32U?counts[n]-32U:0U);
            CHECK(strstr(output,expected));
            if(counts[n]) {
                CHECK(source.anchor_valid && !source.clock_failed);
                snprintf(expected,sizeof(expected),"1,%" PRIu64 ",%" PRIu64 "\r\n[FINAL_RAW_WITNESS]",
                    source.snapshot_anchor_us,source.snapshot_age_us);
                CHECK(strstr(output,expected));
                snprintf(expected,sizeof(expected),"%u,9,4188%02X341244332211\r\n",counts[n]*coalesce,counts[n]);
                CHECK(strstr(output,expected));
            } else {
                CHECK(strstr(output,"0,0,0\r\n[FINAL_RAW_WITNESS]"));
                CHECK(strstr(output,"match_sequence,mac_length,mac_hex\r\n0,0,\r\n"));
            }
            memcpy(saved,output,sizeof(output));
            CHECK(mesh_locate_report_encode(&source,output,length+1U,&measured) && measured==length);
            CHECK(!memcmp(output,saved,length+1U));
            memset(output,0xCD,sizeof(output)); memcpy(saved,output,sizeof(output)); measured=99;
            CHECK(!mesh_locate_report_encode(&source,output,length,&measured));
            CHECK(!measured && !memcmp(output,saved,sizeof(output)));
            if(length>largest)largest=length;
        }
    }
    fill(1,1,ESP_FAIL); size_t length=0;
    CHECK(mesh_locate_report_encode(&source,output,sizeof(output),&length));
    CHECK(strstr(output,"0000000000000ABC,0000000000000101,5,-1\r\n"));
    /* Invalid lifecycle/raw/history input cannot be saved as a completed run. */
    for(unsigned fault=0;fault<10;++fault) {
        fill(1,1,ESP_OK);
        if(fault==0)source.frozen=false;
        if(fault==1)source.final_valid=false;
        if(fault==2)source.faulted=true;
        if(fault==3)source.request.session_id=source.session;
        if(fault==4)source.snapshot.mac[8]^=1U;
        if(fault==5)source.history_count=33U;
        if(fault==6)source.snapshot.flags&=(uint8_t)~CERBERUS_MESH_LOCATE_STOPPED;
        if(fault==7)source.coalesced_matches++;
        if(fault==8)source.snapshot_age_us=0;
        if(fault==9)source.history[0].sequence++;
        rejected();
    }
    fill(1,1,ESP_OK);
    source.clock_failed=true; source.anchor_valid=false;
    source.snapshot_anchor_us=source.snapshot_age_us=0;
    CHECK(mesh_locate_report_valid(&source));
    CHECK(mesh_locate_report_encode(&source,output,sizeof(output),&length));
    CHECK(strstr(output,"0,0,0\r\n[FINAL_RAW_WITNESS]"));
    fill(1,1,ESP_OK); length=99;
    CHECK(!mesh_locate_report_encode(NULL,output,sizeof(output),&length) && !length);
    CHECK(!mesh_locate_report_encode(&source,NULL,1,&length) && !length);
    CHECK(!mesh_locate_report_encode(&source,output,sizeof(output),NULL));
    CHECK(!mesh_locate_report_encode(&source,(char*)&source,sizeof(source),&length));
    CHECK(!memcmp(&source,&before,sizeof(source)));
    CHECK(!mesh_locate_report_encode(&source,output,sizeof(output),(size_t*)&source));
    CHECK(!memcmp(&source,&before,sizeof(source)));
    CHECK(!mesh_locate_report_encode(&source,output,sizeof(output),(size_t*)output));
    printf("Mesh Locate CSV: %u checks; largest fixture %zu / %u bytes; no RF acceptance claimed.\n",checks,largest,MESH_LOCATE_REPORT_MAX_BYTES);
    return 0;
}
