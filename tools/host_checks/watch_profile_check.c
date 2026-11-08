#include "cerberus_watch_capture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}}while(0)
static uint8_t message[6][25];
static const uint8_t address[6]={0x20,1,2,3,4,5};
static void put16(uint8_t *p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8U);}
static void put32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4U;++i)p[i]=(uint8_t)(v>>(8U*i));}
static void messages(void)
{
    memset(message,0,sizeof(message));
    for(unsigned i=0;i<6U;++i)message[i][0]=(uint8_t)(i<<4U|2U);
    message[0][1]=0x12U;memcpy(message[0]+2U,"TEST-ONLY",9U);
    message[1][1]=0x24U;message[1][2]=180U;message[1][3]=8U;message[1][4]=0xfcU;
    put32(message[1]+5U,123456789U);put32(message[1]+9U,(uint32_t)-987654321);
    put16(message[1]+13U,2001U);put16(message[1]+15U,2020U);put16(message[1]+17U,2004U);
    message[1][19]=0x65U;message[1][20]=0x42U;put16(message[1]+21U,100U);message[1][23]=3U;
    message[2][1]=0x10U;message[2][2]=2U;message[2][3]=63U;put32(message[2]+4U,1234567U);
    memset(message[2]+8U,0xa7U,17U);
    message[3][1]=1U;memcpy(message[3]+2U,"TEST DESCRIPTION",16U);
    message[4][1]=5U;put32(message[4]+2U,123456789U);put32(message[4]+6U,(uint32_t)-987654321);
    put16(message[4]+10U,3U);message[4][12]=255U;put16(message[4]+13U,2020U);
    put16(message[4]+15U,1990U);message[4][17]=0x23U;put16(message[4]+18U,2010U);put32(message[4]+20U,42U);
    memcpy(message[5]+2U,"TEST OPERATOR",13U);
}
static bool single(const uint8_t *m,sentinel_watch_protocol_record_t *out)
{
    uint8_t ad[31]={30U,0x16U,0xfaU,0xffU,0x0dU,0U};
    memcpy(ad+6U,m,25U);return sentinel_watch_protocol_decode_ble(ad,sizeof(ad),out);
}
static bool pack(const uint8_t *const *messages_,unsigned count,sentinel_watch_protocol_record_t *out)
{
    uint8_t ad[255]={0,0x16U,0xfaU,0xffU,0x0dU,0U,0xf2U,25U,0U};
    size_t length=9U+25U*count;ad[0]=(uint8_t)(length-1U);ad[8]=(uint8_t)count;
    for(unsigned i=0;i<count;++i)memcpy(ad+9U+25U*i,messages_[i],25U);
    return sentinel_watch_protocol_decode_ble(ad,length,out);
}
static sentinel_watch_profile_timing_t get(cerberus_watch_capture_t *c,sentinel_watch_profile_t *p,uint64_t now)
{
    sentinel_watch_profile_timing_t t;
    CHECK(cerberus_watch_capture_get_profile(c,c->status.radio,c->status.generation,0U,now,p,&t));return t;
}
static void observe(cerberus_watch_capture_t *c,unsigned group,uint64_t time)
{
    sentinel_watch_protocol_record_t r;CHECK(single(message[group],&r));
    cerberus_watch_capture_observe_timed(c,&r,address,0U,-40,6U,time);
}
static void repeated_text_basic(void)
{
    uint8_t first[25]={2U,0x12U},last[25]={2U,0x12U};
    memset(first+2U,'A',20U);
    const uint8_t *items[2]={first,last};
    for(unsigned length=1U;length<=20U;++length){
        memset(last+2U,0,20U);memset(last+2U,'B',length);
        sentinel_watch_protocol_record_t r;CHECK(pack(items,2U,&r));
        CHECK(strlen(r.id)==length&&!memcmp(r.id,last+2U,length));
        CHECK(sentinel_watch_profile_zero((const uint8_t *)r.id+length,sizeof(r.id)-length));
        sentinel_watch_profile_basic_t basic;CHECK(sentinel_watch_profile_basic(&r.profile,&basic));
        CHECK(basic.text&&basic.length==length&&!memcmp(basic.id,r.id,length));
        cerberus_watch_capture_t capture;
        cerberus_watch_capture_begin_with_epoch(&capture,CERBERUS_WATCH_RADIO_BLE,1U,0U,1U);
        cerberus_watch_capture_observe_timed(&capture,&r,address,0U,-40,0U,1000U);
        CHECK(capture.status.stored_count==1U&&!memcmp(capture.records[0].id,r.id,sizeof(r.id)));
    }
}
int main(void)
{
    repeated_text_basic();
    messages();sentinel_watch_protocol_record_t r;
    const uint8_t *all[6];for(unsigned i=0;i<6U;++i)all[i]=message[i];
    CHECK(pack(all,6U,&r));CHECK(r.profile.present==63U&&sentinel_watch_profile_valid(&r.profile));
    sentinel_watch_profile_t expected_profile=r.profile;
    uint8_t wifi[256]={0x80U};wifi[36]=221U;wifi[37]=158U;
    const uint8_t oui[5]={0xfaU,0x0bU,0xbcU,0x0dU,1U};memcpy(wifi+38U,oui,5U);
    wifi[43]=0xf2U;wifi[44]=25U;wifi[45]=6U;
    for(unsigned i=0;i<6U;++i)memcpy(wifi+46U+25U*i,message[i],25U);
    CHECK(sentinel_watch_protocol_decode_wifi(wifi,196U,&r));CHECK(!memcmp(&r.profile,&expected_profile,sizeof(expected_profile)));
    for(size_t length=36U;length<196U;++length){
        memset(&r,0xff,sizeof(r));CHECK(!sentinel_watch_protocol_decode_wifi(wifi,length,&r));CHECK(!r.profile.present);
    }
    CHECK(pack(all,6U,&r));
    CHECK(sizeof(r.profile)==128U);
    sentinel_watch_profile_basic_t basic;CHECK(sentinel_watch_profile_basic(&r.profile,&basic));
    CHECK(basic.id_type==1U&&basic.ua_type==2U&&basic.text&&basic.length==9U);
    sentinel_watch_profile_location_t location;CHECK(sentinel_watch_profile_location(&r.profile,&location));
    CHECK(location.latitude_e7==123456789&&location.longitude_e7==-987654321&&location.position_valid);
    CHECK(location.status==2U&&location.direction_deg==180U&&location.speed_qms==8U&&location.vertical_speed_half_ms==-4);
    CHECK(location.pressure_altitude_dm==5&&location.geo_altitude_dm==100&&location.height_dm==20&&location.height_agl);
    CHECK(location.horizontal_accuracy==5U&&location.vertical_accuracy==6U&&location.speed_accuracy==2U&&location.baro_accuracy==4U);
    CHECK(location.timestamp_ds==100U&&location.timestamp_accuracy==3U&&location.timestamp_valid);
    sentinel_watch_profile_auth_t auth;CHECK(sentinel_watch_profile_auth(&r.profile,&auth));
    CHECK(auth.type==1U&&auth.page_zero&&auth.last_page==2U&&auth.length==63U&&auth.timestamp_2019_s==1234567U);
    for(size_t i=0;i<sizeof(r.profile);++i)CHECK(((uint8_t *)&r.profile)[i]!=0xa7U);
    sentinel_watch_profile_self_t self;CHECK(sentinel_watch_profile_self(&r.profile,&self)&&self.type==1U&&self.length==16U);
    sentinel_watch_profile_operator_t op;CHECK(sentinel_watch_profile_operator(&r.profile,&op)&&op.type==0U&&op.length==13U);
    sentinel_watch_profile_system_t system;CHECK(sentinel_watch_profile_system(&r.profile,&system));
    CHECK(system.location_type==1U&&system.classification_type==1U&&system.position_valid);
    CHECK(system.latitude_e7==123456789&&system.longitude_e7==-987654321&&system.area_count==3U&&system.radius_m==2550U);
    CHECK(system.ceiling_dm==100&&system.floor_dm==-50&&system.altitude_dm==50&&system.category_eu==2U&&system.class_eu==3U&&system.timestamp_2019_s==42U);

    for(unsigned b=0;b<256U;++b){
        message[1][4]=(uint8_t)b;CHECK(single(message[1],&r));CHECK(sentinel_watch_profile_location(&r.profile,&location));
        int expected=b<=127U?(int)b:(int)b-256;
        CHECK(location.vertical_speed_half_ms==expected);CHECK(location.vertical_speed_valid==(expected>=-124&&expected<=124));
    }
    for(unsigned a=0;a<=UINT16_MAX;++a){
        put16(message[1]+13U,(uint16_t)a);CHECK(single(message[1],&r));
        CHECK(sentinel_watch_profile_location(&r.profile,&location));
        CHECK(location.pressure_altitude_dm==(int32_t)a*5-10000&&location.pressure_valid==(a!=0U));
    }
    messages();message[1][2]=181U;message[1][1]|=2U;message[1][3]=255U;message[1][1]|=1U;
    memset(message[1]+5U,0,14U);put16(message[1]+21U,UINT16_MAX);
    CHECK(single(message[1],&r)&&sentinel_watch_profile_location(&r.profile,&location));
    CHECK(!location.position_valid&&!location.direction_valid&&!location.speed_valid&&!location.pressure_valid&&!location.geo_valid&&!location.height_valid&&!location.timestamp_valid);
    messages();message[0][1]=0x34U;memset(message[0]+2U,0,20U);message[0][2]=0xffU;message[0][21]=0x80U;
    CHECK(single(message[0],&r)&&sentinel_watch_profile_basic(&r.profile,&basic));
    CHECK(!basic.text&&basic.length==20U&&basic.id[0]==0xffU&&basic.id[19]==0x80U&&!(r.flags&SENTINEL_WATCH_PROTOCOL_HAS_ID));
    uint8_t text_id[25];memcpy(text_id,message[0],25U);text_id[1]=0x12U;memset(text_id+2U,0,20U);memcpy(text_id+2U,"TEXT",4U);
    const uint8_t *duplicate[2]={text_id,message[0]};CHECK(pack(duplicate,2U,&r));CHECK(!r.id[0]&&!(r.flags&SENTINEL_WATCH_PROTOCOL_HAS_ID));
    CHECK(sentinel_watch_profile_basic(&r.profile,&basic)&&!basic.text);
    uint8_t nonzero_auth[25];memcpy(nonzero_auth,message[2],25U);nonzero_auth[1]=0x12U;
    duplicate[0]=message[2];duplicate[1]=nonzero_auth;CHECK(pack(duplicate,2U,&r));
    CHECK(sentinel_watch_profile_auth(&r.profile,&auth)&&auth.page==2U&&!auth.page_zero&&!auth.length&&!auth.last_page&&!auth.timestamp_2019_s);
    duplicate[0]=message[1];duplicate[1]=message[1];CHECK(!pack(duplicate,2U,&r));
    CHECK(sentinel_watch_profile_valid(&r.profile)&&!r.profile.present);
    messages();message[5][22]=1U;CHECK(!pack(all,6U,&r));CHECK(!r.profile.present&&!r.kind);messages();
    sentinel_watch_profile_t profile;CHECK(pack(all,6U,&r));profile=r.profile;
    for(unsigned group=0;group<6U;++group){
        sentinel_watch_profile_t bad=profile;bad.version[group]=3U;CHECK(!sentinel_watch_profile_valid(&bad));
        bad=profile;bad.present&=(uint8_t)~(1U<<group);CHECK(!sentinel_watch_profile_valid(&bad));
    }
    sentinel_watch_profile_t bad=profile;bad.reserved[0]=1U;CHECK(!sentinel_watch_profile_valid(&bad));

    cerberus_watch_capture_t c;cerberus_watch_capture_begin_with_epoch(&c,CERBERUS_WATCH_RADIO_WIFI,7U,0U,UINT64_C(0x1234567800000001));
    sentinel_watch_profile_timing_t timing;
    CHECK(!cerberus_watch_capture_get_profile(&c,CERBERUS_WATCH_RADIO_WIFI,7U,0U,1000U,&profile,&timing));
    for(unsigned group=0;group<6U;++group)observe(&c,group,1000U+100U*group);
    observe(&c,0U,2000U);cerberus_watch_capture_finish(&c,CERBERUS_WATCH_COMPLETE,3U);
    timing=get(&c,&profile,2501U);CHECK(profile.present==63U&&timing.valid_mask==63U);
    CHECK(timing.age_ms[0]==1U&&timing.age_ms[1]==2U&&timing.age_ms[5]==2U);
    CHECK(timing.capture_epoch==UINT64_C(0x1234567800000001));
    timing=get(&c,&profile,1200U);CHECK(!(timing.valid_mask&1U)&&(timing.valid_mask&(1U<<1U)));
    timing=get(&c,&profile,UINT64_MAX);CHECK(!timing.valid_mask);
    timing=get(&c,&profile,UINT64_C(4294967295000)+3000U);CHECK(!timing.valid_mask&&timing.age_ms[0]==UINT32_MAX);
    CHECK(!cerberus_watch_capture_get_profile(&c,CERBERUS_WATCH_RADIO_WIFI,8U,0U,3000U,&profile,&timing));
    CHECK(!profile.present&&!timing.capture_epoch&&timing.age_ms[0]==UINT32_MAX);
    /* Same source, changed binary identity clears old Location and text. */
    c.status.state=CERBERUS_WATCH_RUNNING;message[0][1]=0x31U;memset(message[0]+2U,0,20U);message[0][2]=1U;observe(&c,0U,3000U);
    CHECK(c.profiles[0].present==1U&&!c.records[0].id[0]&&!c.records[0].flags&&!c.motion[0].flags);
    observe(&c,1U,3100U);message[0][2]=2U;observe(&c,0U,3200U);CHECK(c.profiles[0].present==1U&&!c.motion[0].flags);
    messages();observe(&c,0U,3300U);CHECK(!strcmp(c.records[0].id,"TEST-ONLY")&&c.profiles[0].present==1U);
    observe(&c,1U,3400U);observe(&c,1U,3399U);CHECK(!(c.profile_valid[0]&(1U<<1U)));
    observe(&c,1U,3398U);CHECK(!(c.profile_valid[0]&(1U<<1U)));observe(&c,1U,3400U);CHECK(c.profile_valid[0]&(1U<<1U));
    CHECK(single(message[3],&r));cerberus_watch_capture_observe(&c,&r,address,0U,-40,6U);CHECK(!(c.profile_valid[0]&(1U<<3U)));
    observe(&c,2U,UINT64_MAX);CHECK(!(c.profile_valid[0]&(1U<<2U)));
    cerberus_watch_capture_finish(&c,CERBERUS_WATCH_COMPLETE,4U);timing=get(&c,&profile,5000U);CHECK(!(timing.valid_mask&(1U<<2U)));
    /* Other source/transport/address-type tuples never borrow values. */
    c.status.state=CERBERUS_WATCH_RUNNING;CHECK(single(message[3],&r));
    cerberus_watch_capture_observe_timed(&c,&r,address,1U,-40,0U,5000U);CHECK(c.status.stored_count==2U&&c.profiles[1].present==(1U<<3U));
    cerberus_watch_capture_finish(&c,CERBERUS_WATCH_COMPLETE,5U);
    cerberus_watch_capture_begin_with_epoch(&c,CERBERUS_WATCH_RADIO_WIFI,8U,0U,2U);CHECK(!c.profiles[0].present&&!c.profile_valid[0]);
    sentinel_watch_protocol_record_t pwn={.kind=SENTINEL_WATCH_PROTOCOL_PWNAGOTCHI,.transport=SENTINEL_WATCH_TRANSPORT_WIFI_BEACON};
    cerberus_watch_capture_observe_timed(&c,&pwn,address,0U,-40,6U,500U);cerberus_watch_capture_finish(&c,CERBERUS_WATCH_COMPLETE,1U);
    timing=get(&c,&profile,600U);CHECK(!profile.present&&!timing.valid_mask&&timing.age_ms[5]==UINT32_MAX);
    cerberus_watch_capture_begin_with_epoch(&c,CERBERUS_WATCH_RADIO_WIFI,9U,0U,3U);observe(&c,0U,1000U);cerberus_watch_capture_finish(&c,CERBERUS_WATCH_ERROR,1U);
    CHECK(!c.profiles[0].present&&!c.profile_valid[0]&&!c.status.stored_count);
    /* A protected incoming source can evict one retained non-target; it must
     * receive none of that slot's old groups or clocks. */
    cerberus_watch_capture_begin_with_epoch(&c,CERBERUS_WATCH_RADIO_WIFI,10U,0U,4U);
    CHECK(cerberus_watch_capture_protect_wifi_source(&c,SENTINEL_WATCH_TRANSPORT_WIFI_BEACON,address));
    CHECK(single(message[3],&r));r.transport=SENTINEL_WATCH_TRANSPORT_WIFI_BEACON;
    for(unsigned i=0;i<CERBERUS_WATCH_MAX_OBSERVATIONS;++i){
        uint8_t other[6]={0x22U,1,2,3,4,(uint8_t)i};
        cerberus_watch_capture_observe_timed(&c,&r,other,0U,-50,6U,1000U+i);
    }
    CHECK(c.status.stored_count==12U&&c.profiles[11].present==(1U<<SENTINEL_WATCH_PROFILE_SELF));
    CHECK(single(message[0],&r));r.transport=SENTINEL_WATCH_TRANSPORT_WIFI_BEACON;
    cerberus_watch_capture_observe_timed(&c,&r,address,0U,-40,6U,3000U);
    CHECK(!memcmp(c.records[11].address,address,6U)&&c.profiles[11].present==1U&&c.profile_valid[11]==1U);
    CHECK(c.profile_us[11][SENTINEL_WATCH_PROFILE_SELF]==0U&&c.status.dropped_records==1U);
    printf("Remote ID profiles: %u checks; profile %zu, protocol record %zu, collector %zu bytes\n",checks,sizeof(profile),sizeof(r),sizeof(c));return 0;
}
