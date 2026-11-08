#include "wifi_packet_count_protocol.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if(!(x)){fprintf(stderr,"FAIL line %u: %s\n",__LINE__,#x);exit(1);} }while(0)
static const uint8_t a[6]={2,1,2,3,4,5},b[6]={4,6,7,8,9,10},c[6]={6,11,12,13,14,15};
static uint8_t frame[65536];
static cerberus_wifi_packet_count_table_t table,saved;
static cerberus_wifi_packet_count_target_t targets[16];
static void make_frame(unsigned type,unsigned sub,unsigned flags)
{
    memset(frame,0,sizeof(frame));frame[0]=(uint8_t)((sub<<4)|(type<<2));frame[1]=(uint8_t)flags;
    memcpy(frame+4,a,6);memcpy(frame+10,b,6);memcpy(frame+16,c,6);memcpy(frame+24,a,6);
}
static void init(unsigned n)
{
    memset(targets,0,sizeof(targets));
    for(unsigned i=0;i<n;++i){targets[i].role=i&1?CERBERUS_WIFI_PACKET_COUNT_STA:CERBERUS_WIFI_PACKET_COUNT_AP;
        targets[i].channel_hint=177;targets[i].mac[0]=2;targets[i].mac[5]=(uint8_t)(i+1);}
    CHECK(cerberus_wifi_packet_count_init(&table,targets,n,(UINT64_C(1)<<42)-1U,100));
}
static void headers(void)
{
    cerberus_wifi_packet_count_frame_t f;
    for(unsigned type=0;type<=2;type+=2)for(unsigned sub=0;sub<16;++sub)
    for(unsigned ds=0;ds<4;++ds)for(unsigned order=0;order<2;++order){
        make_frame(type,sub,ds|(order?128:0));
        unsigned sdk=type?CERBERUS_WIFI_PACKET_COUNT_DATA:CERBERUS_WIFI_PACKET_COUNT_MGMT;
        size_t need=24+(type&&ds==3?6:0)+(type&&(sub&8)?2+(order?4:0):0)+(!type&&order?4:0);
        unsigned expected=(!type&&(sub==7||sub==15))||(type&&sub==13)?CERBERUS_WIFI_PACKET_COUNT_DECODE_UNSUPPORTED:
            !type&&ds?CERBERUS_WIFI_PACKET_COUNT_DECODE_MALFORMED:CERBERUS_WIFI_PACKET_COUNT_DECODE_OK;
        CHECK(cerberus_wifi_packet_count_decode(frame,need,sdk,&f)==expected);
        if(expected!=CERBERUS_WIFI_PACKET_COUNT_DECODE_OK)continue;
        CHECK(f.header_bytes==need);CHECK(!memcmp(f.ra,a,6)&&!memcmp(f.ta,b,6));
        const uint8_t *pan=!type||ds==0?c:ds==1?a:ds==2?b:NULL;
        CHECK((f.roles&CERBERUS_WIFI_PACKET_COUNT_ROLE_BSSID)==(pan?4:0));
        if(pan)CHECK(!memcmp(f.bssid,pan,6));
        for(size_t n=0;n<need;++n){memset(&f,0xa5,sizeof(f));
            CHECK(cerberus_wifi_packet_count_decode(frame,n,sdk,&f)==CERBERUS_WIFI_PACKET_COUNT_DECODE_MALFORMED);
            cerberus_wifi_packet_count_frame_t empty={0};CHECK(!memcmp(&f,&empty,sizeof(f)));}
    }
    make_frame(2,0,0);frame[0]|=1;
    CHECK(cerberus_wifi_packet_count_decode(frame,24,1,&f)==CERBERUS_WIFI_PACKET_COUNT_DECODE_UNSUPPORTED);
    make_frame(1,13,0);CHECK(cerberus_wifi_packet_count_decode(frame,10,2,&f)==CERBERUS_WIFI_PACKET_COUNT_DECODE_UNSUPPORTED);
    make_frame(2,0,0);CHECK(cerberus_wifi_packet_count_decode(frame,24,0,&f)==CERBERUS_WIFI_PACKET_COUNT_DECODE_MALFORMED);
    frame[10]|=1;CHECK(cerberus_wifi_packet_count_decode(frame,24,1,&f)==CERBERUS_WIFI_PACKET_COUNT_DECODE_MALFORMED);
    make_frame(2,0,0);memset(frame+4,0xff,6);CHECK(cerberus_wifi_packet_count_decode(frame,24,1,&f)==0);
    memset(frame+4,0,6);CHECK(cerberus_wifi_packet_count_decode(frame,24,1,&f)==CERBERUS_WIFI_PACKET_COUNT_DECODE_MALFORMED);
    make_frame(2,8,3|8|64|4|128);frame[22]=3;
    CHECK(cerberus_wifi_packet_count_decode(frame,36,1,&f)==0&&f.retry&&f.protected_frame&&f.fragmented);
    CHECK(cerberus_wifi_packet_count_decode(frame,65535,1,&f)==0);
    CHECK(cerberus_wifi_packet_count_decode(frame,65536,1,&f)==CERBERUS_WIFI_PACKET_COUNT_DECODE_MALFORMED);
    uint8_t before[64];memcpy(before,frame,sizeof(before));
    CHECK(cerberus_wifi_packet_count_decode(frame,36,1,(void *)(frame+1))==CERBERUS_WIFI_PACKET_COUNT_DECODE_MALFORMED);
    CHECK(!memcmp(before,frame,sizeof(before)));
}
static void matching(void)
{
    init(4);memcpy(targets[0].mac,a,6);memcpy(targets[1].mac,b,6);memcpy(targets[2].mac,c,6);
    targets[3]=targets[0];targets[3].role=CERBERUS_WIFI_PACKET_COUNT_STA;
    CHECK(cerberus_wifi_packet_count_init(&table,targets,4,1,100));
    cerberus_wifi_packet_count_attempt(&table,1,true);
    CHECK(cerberus_wifi_packet_count_window_begin(&table,1,0));
    make_frame(2,0,8|64);CHECK(cerberus_wifi_packet_count_observe(&table,frame,24,1,true,1,-45,0)==11);
    CHECK(table.summary.valid_frames==1&&table.summary.matched_frames==1);
    CHECK(table.rows[0].observations==1&&table.rows[0].addressed==1&&table.rows[0].transmitted==0);
    CHECK(!(table.rows[0].flags&CERBERUS_WIFI_PACKET_COUNT_ROW_RSSI));
    CHECK(table.rows[1].transmitted==1&&table.rows[1].rssi==-45&&table.rows[1].last_tx_us==0);
    CHECK((table.rows[1].flags&7)==7&&table.rows[1].target.channel_hint==177);
    CHECK(table.rows[2].observations==0&&table.rows[2].bssid_context==1);
    CHECK(table.rows[0].retry==1&&table.rows[0].protected_frames==1);
    memcpy(frame+10,a,6);CHECK(cerberus_wifi_packet_count_observe(&table,frame,24,1,true,1,-60,10)==9);
    CHECK(table.rows[0].observations==2&&table.rows[0].addressed==2&&table.rows[0].transmitted==1);
    CHECK(table.rows[0].rssi==-60&&table.rows[0].last_tx_us==10&&table.summary.matched_frames==2);
    memcpy(frame+10,b,6);CHECK(cerberus_wifi_packet_count_observe(&table,frame,24,1,true,1,12,20)==11);
    CHECK(!(table.rows[1].flags&CERBERUS_WIFI_PACKET_COUNT_ROW_RSSI)&&table.rows[1].rssi==0);
    make_frame(0,8,0);CHECK(cerberus_wifi_packet_count_observe(&table,frame,24,0,true,1,-40,30)==11);
    CHECK(table.rows[0].management==1&&table.rows[0].data==3);
    CHECK(cerberus_wifi_packet_count_observe(&table,frame,24,0,true,2,-40,31)==0);
    CHECK(cerberus_wifi_packet_count_observe(&table,frame,24,0,false,1,-40,32)==0);
    CHECK(cerberus_wifi_packet_count_observe(&table,frame,23,0,true,1,-40,33)==0);
    CHECK(cerberus_wifi_packet_count_observe(&table,frame,24,2,true,1,-40,34)==0);
    CHECK(table.summary.callbacks==8&&table.summary.outside_window_frames==1&&table.summary.invalid_metadata==1&&
        table.summary.malformed_frames==1&&table.summary.unsupported_frames==1);
    uint64_t covered=0;CHECK(cerberus_wifi_packet_count_copy_channel(&table,0,100,&covered)&&covered==100);
    CHECK(cerberus_wifi_packet_count_window_end(&table,100));
    cerberus_wifi_packet_count_attempt(&table,2,false); /* not requested: unchanged */
    CHECK(table.summary.channel_failures==0);
    cerberus_wifi_packet_count_finish(&table,150);saved=table;
    CHECK(cerberus_wifi_packet_count_observe(&table,frame,24,0,true,1,-40,160)==0&&!memcmp(&saved,&table,sizeof(table)));
    CHECK(cerberus_wifi_packet_count_copy_channel(&table,0,200,&covered)&&covered==100);
    cerberus_wifi_packet_count_summary_t summary;
    CHECK(cerberus_wifi_packet_count_copy_summary(&table,200,&summary)&&summary.started_source_us==0&&summary.stopped_source_us==150&&summary.copied_source_us==200);
    uint8_t wire[128];cerberus_packet_count_record_stats_t raw={0};
    CHECK(cerberus_packet_count_encode_summary(&table,200,5,0,0,0,15,&raw,wire));
    CHECK(cerberus_packet_count_get(wire+20,4)==8&&cerberus_packet_count_get(wire+48,4)==4);
    CHECK(cerberus_packet_count_encode_row(&table,0,200,wire));
    CHECK(cerberus_packet_count_get(wire+8,4)==4&&cerberus_packet_count_get(wire+80,8)==100);
}
static void bounds(void)
{
    for(unsigned i=0;i<42;++i){uint8_t ch=cerberus_wifi_packet_count_channel(i);
        CHECK(cerberus_wifi_packet_count_channel_index(ch)==(int)i&&cerberus_wifi_packet_count_channel_bit(ch)==(UINT64_C(1)<<i));}
    CHECK(cerberus_wifi_packet_count_channel(41)==177&&cerberus_wifi_packet_count_channel(42)==0);
    CHECK(!cerberus_wifi_packet_count_mask_valid(0)&&!cerberus_wifi_packet_count_mask_valid(UINT64_C(1)<<42));
    init(16);saved=table;targets[1]=targets[0];CHECK(!cerberus_wifi_packet_count_init(&table,targets,16,1,100)&&!memcmp(&table,&saved,sizeof(table)));
    CHECK(!cerberus_wifi_packet_count_init(&table,&table.rows[0].target,1,1,100));
    CHECK(!cerberus_wifi_packet_count_copy_summary(&table,0,&table.summary));
    CHECK(!cerberus_wifi_packet_count_copy_row(&table,0,&table.rows[0]));
    CHECK(!cerberus_wifi_packet_count_copy_channel(&table,0,0,&table.channel_window_us[0]));
    init(1);memcpy(table.rows[0].target.mac,a,6);CHECK(cerberus_wifi_packet_count_window_begin(&table,1,100));
    make_frame(2,0,0);CHECK(cerberus_wifi_packet_count_observe(&table,frame,24,1,true,1,-70,99)==0);
    CHECK(table.summary.clock_dropped_frames==1&&(table.summary.flags&CERBERUS_WIFI_PACKET_COUNT_CLOCK_INVALID));
    CHECK(cerberus_wifi_packet_count_observe(&table,frame,24,1,true,1,-70,101)==0&&table.summary.clock_dropped_frames==2);
    cerberus_wifi_packet_count_finish(&table,110);CHECK(!(table.summary.flags&CERBERUS_WIFI_PACKET_COUNT_STOPPED_TIME));
    init(1);memcpy(table.rows[0].target.mac,a,6);CHECK(cerberus_wifi_packet_count_window_begin(&table,1,0));
    table.rows[0].observations=UINT32_MAX-1;table.summary.callbacks=UINT32_MAX-1;table.summary.channel_failures=UINT32_MAX-1;
    CHECK(cerberus_wifi_packet_count_observe(&table,frame,24,1,true,1,-60,0)==1);
    CHECK(table.rows[0].observations==UINT32_MAX&&(table.rows[0].flags&CERBERUS_WIFI_PACKET_COUNT_ROW_SATURATED));
    CHECK(table.summary.callbacks==UINT32_MAX&&(table.summary.flags&CERBERUS_WIFI_PACKET_COUNT_SATURATED));
    cerberus_wifi_packet_count_attempt(&table,1,false);CHECK(table.summary.channel_failures==UINT32_MAX);
    CHECK(cerberus_wifi_packet_count_observe(&table,frame,24,1,true,1,-60,1)==1&&table.summary.callbacks==UINT32_MAX);
}
static void random_boundaries(void)
{
    uint32_t random=1234567;
    struct {uint64_t pre;cerberus_wifi_packet_count_frame_t f;uint64_t post;} guarded;
    for(unsigned sample=0;sample<20000;++sample){
        for(unsigned i=0;i<64;++i){random=random*1664525U+1013904223U;frame[i]=(uint8_t)(random>>24);}
        guarded.pre=UINT64_MAX;guarded.post=UINT64_C(0x123456789abcdef0);
        unsigned r=cerberus_wifi_packet_count_decode(frame,sample%65U,sample%3U,&guarded.f);
        CHECK(r<=2&&guarded.pre==UINT64_MAX&&guarded.post==UINT64_C(0x123456789abcdef0));
        if(!r)CHECK(guarded.f.header_bytes<=sample%65U&&guarded.f.header_bytes>=24&&guarded.f.kind<=1);
    }
}
int main(void)
{headers();matching();bounds();random_boundaries();printf("Packet Count model PASS %u checks; table %zu bytes\n",checks,sizeof(table));return 0;}
