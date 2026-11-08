#ifdef NDEBUG
#undef NDEBUG
#endif
#include "airtag_json.h"
#include "ble_response.h"
#include "ble_archive_response_fixture.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned checks;
#define CHECK(c) do { ++checks; assert(c); } while (0)
#define OK CERBERUS_AIRTAG_JSON_OK
#define INVALID CERBERUS_AIRTAG_JSON_INVALID
#define LIMIT CERBERUS_AIRTAG_JSON_LIMIT
#define NONE CERBERUS_AIRTAG_JSON_NO_ELIGIBLE
static cerberus_airtag_json_t doc, before, decoded;
static cerberus_ble_archive_t archive;
static uint8_t bytes[CERBERUS_AIRTAG_JSON_MAX_BYTES + 1];
static uint8_t saved[sizeof(bytes)];

static void rejected(const void *s, size_t n)
{
    memset(&doc,0xa7,sizeof(doc)); before=doc;
    CHECK(cerberus_airtag_json_decode(s,n,&doc)!=OK);
    CHECK(!memcmp(&doc,&before,sizeof(doc)));
}
static void bad(const char *s) { rejected(s,strlen(s)); }
static void read_ok(const char *s)
{
    CHECK(cerberus_airtag_json_decode((const uint8_t *)s,strlen(s),&doc)==OK);
    CHECK(cerberus_airtag_json_validate(&doc)==OK);
}
static size_t encode(void)
{
    size_t n=SIZE_MAX;
    CHECK(cerberus_airtag_json_encode(&doc,bytes,sizeof(bytes),&n)==OK);
    CHECK(n<=CERBERUS_AIRTAG_JSON_MAX_BYTES);
    return n;
}
static void golden(void)
{
    /* Independent expected bytes: upstream byteArrayToHexString emits spaced
     * lowercase 0xNN tokens; root-array objects preserve duplicate MACs. */
    static const char expected[]=
        "[{\"mac\":\"AA:BB:CC:DD:EE:FF\",\"payload\":\"0x00 0x0a 0xff\",\"payload_size\":3},"
        "{\"mac\":\"AA:BB:CC:DD:EE:FF\",\"payload\":\"\",\"payload_size\":0}]";
    read_ok(expected);
    CHECK(doc.count==2 && doc.payload_used==3);
    CHECK(doc.records[0].address[0]==0xff && doc.records[0].address[5]==0xaa);
    CHECK(!memcmp(doc.records[0].address,doc.records[1].address,6));
    CHECK(doc.records[0].offset==0 && doc.records[1].offset==3 && doc.records[1].length==0);
    CHECK(doc.payload[0]==0 && doc.payload[1]==10 && doc.payload[2]==255);
    size_t n=encode();
    CHECK(n==sizeof(expected)-1 && !memcmp(bytes,expected,n));
    for(size_t i=0;i<n;++i) rejected(expected,i);
    read_ok(" \r\n [ {\"payload_size\":3,\"payload\":\" 0X00  0x0A 0xff \","
        "\"m\\u0061c\":\"aa\\u003abb:cc:dd:ee:ff\"},"
        "{\"payload\":\"  \",\"payload_size\":0,\"mac\":\"AA:BB:CC:DD:EE:FF\"} ] \t ");
    n=encode(); CHECK(n==sizeof(expected)-1 && !memcmp(bytes,expected,n));
    read_ok("[{\"extra\":{\"values\":[true,false,null,-1.25e+2,{\"x\":\"\\uD834\\uDD1E\"}]},"
        "\"extra\":\"\xc3\xa9\",\"mac\":\"00:00:00:00:00:00\",\"payload\":\"0x00\\u00200x01\",\"payload_size\":2}]");
    CHECK(doc.count==1 && doc.payload_used==2 && doc.payload[1]==1);
    read_ok("[]"); CHECK(!doc.count && !doc.payload_used);
    CHECK(encode()==2 && !memcmp(bytes,"[]",2));
}
static void malformed(void)
{
    static const char *invalid[]={
        "", "{}", "null", "[null]", "[{}]", "[1]", "[,]", "[]x", "[{},]",
        "[{\"mac\":\"00:00:00:00:00:00\",\"payload\":\"\",\"payload_size\":1}]",
        "[{\"mac\":\"00:00:00:00:00:00\",\"payload\":\"\"}]",
        "[{\"mac\":\"00:00:00:00:00:00\",\"mac\":\"00:00:00:00:00:00\",\"payload\":\"\",\"payload_size\":0}]",
        "[{\"mac\":\"00:00:00:00:00:00\",\"payload\":\"\",\"payload\":\"\",\"payload_size\":0}]",
        "[{\"mac\":\"00:00:00:00:00:00\",\"payload\":\"\",\"payload_size\":0,\"payload_\\u0073ize\":0}]",
        "[{\"mac\":\"00:00:00:00:00:00\",\"payload\":\"\",\"payload_size\":0,\"x\":01}]",
        "[{\"mac\":\"00:00:00:00:00:00\",\"payload\":\"\",\"payload_size\":0,\"x\":\"\\uD800\"}]",
        "[{\"mac\":\"00:00:00:00:00:00\",\"payload\":\"\",\"payload_size\":0,\"x\":\"\\uDC00\"}]",
        "[{\"mac\":\"00:00:00:00:00:00\",\"payload\":\"\",\"payload_size\":0,\"x\":\"\xc0\x80\"}]",
        "[{\"mac\":\"00:00:00:00:00:00\",\"payload\":\"\",\"payload_size\":0,}]",
    };
    for(size_t i=0;i<sizeof(invalid)/sizeof(*invalid);++i) bad(invalid[i]);
    static const char *payloads[]={"0x","0x0","0x000","00","ff","0xgg","0x00,0xff",
        "0x00\\t0xff","\\u0000","0x100","0x00\\n0x01","0x00\\u00a00x01"};
    for(size_t i=0;i<sizeof(payloads)/sizeof(*payloads);++i){
        snprintf((char *)bytes,sizeof(bytes),"[{\"mac\":\"00:00:00:00:00:00\",\"payload\":\"%s\",\"payload_size\":1}]",payloads[i]);
        bad((const char *)bytes);
    }
    static const char *sizes[]={"-1","0.0","0e0","00","+0","\"0\"","null","true","8193","65536","18446744073709551615"};
    for(size_t i=0;i<sizeof(sizes)/sizeof(*sizes);++i){
        snprintf((char *)bytes,sizeof(bytes),"[{\"mac\":\"00:00:00:00:00:00\",\"payload\":\"\",\"payload_size\":%s}]",sizes[i]);
        bad((const char *)bytes);
    }
    static const char *macs[]={"","00:00:00:00:00","00-00-00-00-00-00","GG:00:00:00:00:00","00:00:00:00:00:00\\u0000"};
    for(size_t i=0;i<sizeof(macs)/sizeof(*macs);++i){
        snprintf((char *)bytes,sizeof(bytes),"[{\"mac\":\"%s\",\"payload\":\"\",\"payload_size\":0}]",macs[i]);
        bad((const char *)bytes);
    }
    for(unsigned i=0;i<256;++i){
        snprintf((char *)bytes,sizeof(bytes),"[{\"payload_size\":1,\"mac\":\"00:00:00:00:00:00\",\"payload\":\"0x%02x\"}]",i);
        read_ok((const char *)bytes); CHECK(doc.payload[0]==i);
    }
}
static void limits(void)
{
    memset(&doc,0,sizeof(doc));doc.count=64;doc.payload_used=8192;
    for(unsigned i=0;i<64;++i){doc.records[i].offset=(uint16_t)(i*128);doc.records[i].length=128;doc.records[i].address[0]=(uint8_t)i;}
    for(unsigned i=0;i<8192;++i)doc.payload[i]=(uint8_t)(i*31U);
    size_t n=encode();CHECK(n<49152 && n>40960);
    printf("Canonical64/pool8192 encoded bytes=%zu\n",n);
    CHECK(cerberus_airtag_json_decode(bytes,n,&decoded)==OK && !memcmp(&doc,&decoded,sizeof(doc)));
    memcpy(saved,bytes,n);
    for(size_t cap=0;cap<n;cap+=997){
        memset(bytes,0x5a,sizeof(bytes));size_t written=777;
        CHECK(cerberus_airtag_json_encode(&doc,bytes,cap,&written)==LIMIT && written==0);
        for(size_t j=0;j<sizeof(bytes);++j)CHECK(bytes[j]==0x5a);
    }
    for(size_t i=0;i<n;i+=137){memcpy(bytes,saved,n);bytes[i]=0;rejected(bytes,n);}
    memset(bytes,' ',sizeof(bytes));rejected(bytes,sizeof(bytes));
    memset(bytes,' ',49152);bytes[0]='[';bytes[1]=']';
    CHECK(cerberus_airtag_json_decode(bytes,49152,&doc)==OK && !doc.count);
    size_t written=99;
    CHECK(cerberus_airtag_json_encode(NULL,bytes,sizeof(bytes),&written)==INVALID && written==0);
    written=99;CHECK(cerberus_airtag_json_encode(&doc,NULL,0,&written)==INVALID && written==0);
    before=doc;written=99;
    CHECK(cerberus_airtag_json_encode(&doc,(uint8_t *)&doc,sizeof(doc),&written)==INVALID && written==0);
    CHECK(!memcmp(&doc,&before,sizeof(doc)));
    CHECK(cerberus_airtag_json_encode(&doc,bytes,sizeof(bytes),(size_t *)bytes)==INVALID);
    CHECK(cerberus_airtag_json_decode((const uint8_t *)&doc,2,&doc)==INVALID);
    doc.count=65;CHECK(cerberus_airtag_json_validate(&doc)==LIMIT);
    memset(&doc,0,sizeof(doc));doc.count=1;doc.records[0].offset=1;
    CHECK(cerberus_airtag_json_validate(&doc)==INVALID);
    doc.records[0].offset=0;doc.records[0].length=8193;doc.payload_used=8193;
    CHECK(cerberus_airtag_json_validate(&doc)==LIMIT);
    memset(bytes,0x55,sizeof(bytes));memcpy(saved,bytes,sizeof(bytes));written=99;
    CHECK(cerberus_airtag_json_encode(&doc,bytes,sizeof(bytes),&written)==LIMIT && written==0);
    CHECK(!memcmp(bytes,saved,sizeof(bytes)));
    /* A single imported payload may use the entire pool; this is not a BLE
     * receive-size claim. Multiple records share the same aggregate limit. */
    memset(&doc,0,sizeof(doc));doc.count=1;doc.payload_used=8192;doc.records[0].length=8192;
    n=encode();CHECK(cerberus_airtag_json_decode(bytes,n,&decoded)==OK && decoded.records[0].length==8192);
    /* The second payload overflows the aggregate, even when each individual
     * declaration is within the per-document bound. */
    --n;const char *extra=",{\"mac\":\"00:00:00:00:00:00\",\"payload\":\"0x00\",\"payload_size\":1}]";
    memcpy(bytes+n,extra,strlen(extra));rejected(bytes,n+strlen(extra));
    /* Nesting and row limits are rejected atomically. */
    strcpy((char *)bytes,"[{\"mac\":\"00:00:00:00:00:00\",\"payload\":\"\",\"payload_size\":0,\"x\":");
    size_t p=strlen((char *)bytes);
    for(unsigned i=0;i<13;++i)bytes[p++]='[';
    bytes[p++]='0';for(unsigned i=0;i<13;++i)bytes[p++]=']';bytes[p++]='}';bytes[p++]=']';
    rejected(bytes,p);
    p=0;bytes[p++]='[';
    for(unsigned i=0;i<65;++i){if(i)bytes[p++]=',';const char *r="{\"mac\":\"00:00:00:00:00:00\",\"payload\":\"\",\"payload_size\":0}";memcpy(bytes+p,r,strlen(r));p+=strlen(r);}
    bytes[p++]=']';rejected(bytes,p);
}
static uint32_t random_state=UINT32_C(0x574d3311);
static uint32_t random_value(void)
{ random_state=random_state*UINT32_C(1664525)+UINT32_C(1013904223);return random_state; }
static void roundtrips(void)
{
    for(unsigned run=0;run<150;++run){
        memset(&doc,0,sizeof(doc));doc.count=(uint8_t)(random_value()%65);
        for(unsigned i=0;i<doc.count;++i){
            cerberus_airtag_json_record_t *r=&doc.records[i];
            for(unsigned b=0;b<6;++b)r->address[b]=(uint8_t)(random_value()>>24);
            size_t remaining=8192-doc.payload_used;
            r->length=(uint16_t)(random_value()%513);
            if(r->length>remaining)r->length=(uint16_t)remaining;
            r->offset=doc.payload_used;
            for(unsigned b=0;b<r->length;++b)doc.payload[doc.payload_used++]=(uint8_t)(random_value()>>24);
        }
        size_t n=encode();
        CHECK(cerberus_airtag_json_decode(bytes,n,&decoded)==OK);
        CHECK(!memcmp(&doc,&decoded,sizeof(doc)));
    }
}

static void original(cerberus_ble_archive_row_t *r,unsigned receiver,unsigned id)
{
    memset(r,0,sizeof(*r));r->receiver=(uint8_t)receiver;r->address[0]=(uint8_t)id;
    r->address_type=(uint8_t)(id%4);r->index=(uint8_t)(id%32);r->rssi=127;
    r->scan_epoch_valid=true;r->scan_epoch=17;
    memcpy(r->advertisement,(uint8_t[]){3,3,0x44,0xfd},4);r->advertisement_length=4;r->advertisement_event_type=2;
    if(receiver==CERBERUS_BLE_ARCHIVE_RECEIVER_C5){
        r->rssi_kind=CERBERUS_BLE_ARCHIVE_RSSI_LATEST_ADV;r->receiver_boot_id=32;
        r->receive=(cerberus_dual_ble_receive_metadata_t){.source_kind=1,.callback_length=4,.sid=255,.data_status=255,.ad_validation=1};
    }
}
static void response(cerberus_ble_archive_row_t *r)
{
    if(r->receiver==CERBERUS_BLE_ARCHIVE_RECEIVER_S3){
        r->response_state=CERBERUS_BLE_ARCHIVE_RESPONSE_PRESENT;r->response_length=4;r->response_original_length=4;
        r->response_event_type=4;r->response_original_sequence=9;r->response_sequence=10;
        memcpy(r->response,(uint8_t[]){3,3,0x44,0xfd},4);
    }else{
        cerberus_dual_ble_response_wire_t *s=&r->c5_response;
        s->version=1;s->state=CERBERUS_DUAL_BLE_RESPONSE_PRESENT;s->raw_length=4;s->event_type=4;
        s->rssi=127;s->age_ms=UINT32_MAX;s->original_sequence=10;s->response_sequence=11;s->interval_us=1000;
        s->receive=r->receive;memcpy(s->raw,(uint8_t[]){3,3,0x44,0xfd},4);
        cerberus_dual_ble_family_evidence_t e;
        CHECK(cerberus_dual_ble_family_parse_ad(s->raw,4,r->address,r->address_type,&e));
        CHECK(cerberus_dual_ble_response_family_encode(&e,0,&s->family));
    }
}
static void no_export(unsigned filtered,unsigned incomplete)
{
    memset(&doc,0x8d,sizeof(doc));before=doc;
    cerberus_airtag_json_export_stats_t stats={0};
    CHECK(cerberus_ble_archive_validate(&archive));
    CHECK(cerberus_airtag_json_from_archive(&archive,&doc,&stats)==NONE);
    CHECK(!stats.eligible && stats.filtered==filtered && stats.incomplete==incomplete);
    CHECK(!memcmp(&doc,&before,sizeof(doc)));
}
static void export_packets(void)
{
    memset(&archive,0,sizeof(archive));archive.count=2;
    original(&archive.rows[0],1,0);original(&archive.rows[1],2,0);
    response(&archive.rows[0]);response(&archive.rows[1]);
    CHECK(cerberus_ble_archive_validate(&archive));
    cerberus_airtag_json_export_stats_t stats={0};
    CHECK(cerberus_airtag_json_from_archive(&archive,&doc,&stats)==OK);
    CHECK(stats.eligible==4 && !stats.filtered && !stats.incomplete && doc.count==4);
    CHECK(doc.payload_used==16);
    for(unsigned i=0;i<4;++i){CHECK(doc.records[i].offset==i*4);CHECK(!memcmp(doc.records[i].address,archive.rows[0].address,6));}
    size_t n=encode();CHECK(cerberus_airtag_json_decode(bytes,n,&decoded)==OK && !memcmp(&doc,&decoded,sizeof(doc)));
    archive.count=1;original(&archive.rows[0],1,0);
    memcpy(archive.rows[0].advertisement,(uint8_t[]){3,3,0x18,0x18},4);no_export(1,0);
    response(&archive.rows[0]);
    CHECK(cerberus_airtag_json_from_archive(&archive,&doc,&stats)==OK && stats.eligible==1 && stats.filtered==1);
    original(&archive.rows[0],1,0);archive.rows[0].advertisement_flags=1;no_export(0,1);
    original(&archive.rows[0],1,0);archive.rows[0].advertisement[0]=9;no_export(0,1);
    original(&archive.rows[0],1,0);archive.rows[0].advertisement_length=5;
    archive.rows[0].advertisement[4]=9;no_export(0,1); /* matching prefix cannot hide bad suffix */
    original(&archive.rows[0],1,0);archive.rows[0].scan_epoch_valid=false;archive.rows[0].scan_epoch=0;no_export(0,1);
    original(&archive.rows[0],0,0);no_export(0,1);
    original(&archive.rows[0],2,0);memset(&archive.rows[0].receive,0,sizeof(archive.rows[0].receive));no_export(0,1);
    original(&archive.rows[0],2,0);CHECK(archive_test_response_row(&archive.rows[0],2,2));no_export(1,1);
    /* Full decoded original proof is not a substitute for the omitted raw
     * bytes, including a family hint beginning beyond the 31-byte preview. */
    original(&archive.rows[0],2,0);
    uint8_t full[255]={39,0x30};memcpy(full+40,(uint8_t[]){3,3,0x44,0xfd},4);
    cerberus_dual_ble_family_evidence_t evidence;
    CHECK(cerberus_dual_ble_family_parse_ad(full,sizeof(full),archive.rows[0].address,archive.rows[0].address_type,&evidence));
    CHECK(cerberus_dual_ble_family_wire_encode(&evidence,2,&archive.rows[0].family));
    archive.rows[0].advertisement_length=31;archive.rows[0].advertisement_flags=3;archive.rows[0].advertisement_event_type=3;
    memcpy(archive.rows[0].advertisement,full,31);
    archive.rows[0].receive=(cerberus_dual_ble_receive_metadata_t){.source_kind=2,.callback_length=255,.sid=1,.primary_phy=1,.ad_validation=1};
    no_export(0,1);
    /* Complete Apple manufacturer frame, without any imported/live hint
     * metadata, qualifies from the retained bytes themselves. */
    original(&archive.rows[0],1,0);
    memset(archive.rows[0].advertisement,0,31);
    memcpy(archive.rows[0].advertisement,(uint8_t[]){30,0xff,0x4c,0,0x12,0x19},6);
    archive.rows[0].advertisement_length=31;
    CHECK(cerberus_airtag_json_from_archive(&archive,&doc,&stats)==OK && stats.eligible==1);
    CHECK(doc.records[0].length==31 && !memcmp(doc.payload,archive.rows[0].advertisement,31));
    /* Known complete native EXT originals are normalized to event0 or3.
     * Legacy-PDU EXT keeps its original legacy event mapping. */
    original(&archive.rows[0],2,0);
    archive.rows[0].receive=(cerberus_dual_ble_receive_metadata_t){.source_kind=2,.properties=2,.callback_length=4,.sid=3,.primary_phy=1,.secondary_phy=2,.ad_validation=1};
    archive.rows[0].advertisement_event_type=3;
    response(&archive.rows[0]);archive.rows[0].c5_response.event_type=3;archive.rows[0].c5_response.receive.properties=10;
    CHECK(cerberus_ble_archive_validate(&archive));
    CHECK(cerberus_airtag_json_from_archive(&archive,&doc,&stats)==OK && stats.eligible==2);
    memset(&archive.rows[0].c5_response,0,sizeof(archive.rows[0].c5_response));
    archive.rows[0].receive.properties=1;archive.rows[0].advertisement_event_type=0;
    CHECK(cerberus_airtag_json_from_archive(&archive,&doc,&stats)==OK && stats.eligible==1);
    archive.rows[0].receive.properties=0x13;archive.rows[0].receive.sid=255;archive.rows[0].receive.secondary_phy=0;
    CHECK(cerberus_airtag_json_from_archive(&archive,&doc,&stats)==OK && stats.eligible==1);
    /* 128 eligible packets cannot silently become the first64. */
    memset(&archive,0,sizeof(archive));archive.count=64;
    for(unsigned i=0;i<64;++i){original(&archive.rows[i],i<32?1:2,i%32);response(&archive.rows[i]);}
    CHECK(cerberus_ble_archive_validate(&archive));before=doc;
    CHECK(cerberus_airtag_json_from_archive(&archive,&doc,&stats)==LIMIT && stats.eligible==128);
    CHECK(!memcmp(&doc,&before,sizeof(doc)));
    for(unsigned i=0;i<64;++i){
        archive.rows[i].response_state=0;archive.rows[i].response_length=0;archive.rows[i].response_original_length=0;archive.rows[i].response_event_type=0;
        archive.rows[i].response_original_sequence=0;archive.rows[i].response_sequence=0;memset(archive.rows[i].response,0,sizeof(archive.rows[i].response));
        memset(&archive.rows[i].c5_response,0,sizeof(archive.rows[i].c5_response));
    }
    CHECK(cerberus_airtag_json_from_archive(&archive,&doc,&stats)==OK && doc.count==64 && stats.eligible==64);
    archive.rows[0].address_type=4;before=doc;cerberus_airtag_json_export_stats_t previous=stats;
    CHECK(cerberus_airtag_json_from_archive(&archive,&doc,&stats)==INVALID);
    CHECK(!memcmp(&doc,&before,sizeof(doc)) && !memcmp(&stats,&previous,sizeof(stats)));
}
int main(void)
{
    golden();malformed();limits();roundtrips();export_packets();
    printf("AirTag JSON: %u checks passed; document=%zu record=%zu stats=%zu bytes\n",
        checks,sizeof(doc),sizeof(doc.records[0]),sizeof(cerberus_airtag_json_export_stats_t));
    return 0;
}
