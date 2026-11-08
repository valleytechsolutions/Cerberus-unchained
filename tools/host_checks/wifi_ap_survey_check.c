#include "wifi_ap_survey.h"
#include "wifi_capture_plan.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"line %u: %s\n",__LINE__,#x); assert(x); } } while (0)
static cerberus_ap_survey_table_t table;
static cerberus_ap_survey_snapshot_t snapshot, old;
static uint8_t frame[128];
static uint64_t now;

static void address(uint8_t *out, unsigned id)
{ memset(out,0,6); out[0]=2; out[4]=(uint8_t)(id>>8); out[5]=(uint8_t)id; }

static size_t beacon(unsigned id, const uint8_t *ssid, unsigned length)
{
    memset(frame,0,sizeof(frame)); frame[0]=0x80;
    memset(frame+4,255,6); address(frame+10,id); address(frame+16,id);
    frame[34]=0x11; frame[36]=0; frame[37]=(uint8_t)length;
    memcpy(frame+38,ssid,length); return 38U+length;
}

static size_t relation(unsigned ap, unsigned sta, unsigned direction)
{
    memset(frame,0,sizeof(frame)); frame[0]=0x08; frame[1]=(uint8_t)direction;
    address(frame+4,direction==1?ap:sta); address(frame+10,direction==1?sta:ap);
    address(frame+16,77); return 24;
}

static void begin(void)
{
    uint64_t mask=cerberus_wifi_capture_plan_channel_bit(1);
    CHECK(cerberus_ap_survey_begin(&table, UINT64_C(0x10000000001),mask,mask));
    CHECK(cerberus_ap_survey_note_channel(&table,1,true));
    CHECK(cerberus_ap_survey_arm(&table,table.live.session_id,1,1,100));
    now=100;
}

static bool commit(size_t length, int rssi)
{
    cerberus_ap_survey_decoded_t decoded;
    ++now;
    cerberus_ap_survey_decode(frame,length,1,rssi,now,&decoded);
    bool result=cerberus_ap_survey_commit(&table,table.live.session_id,table.arm_token,1,now,&decoded);
    CHECK(cerberus_ap_survey_snapshot(&table,now,&snapshot));
    CHECK(cerberus_ap_survey_snapshot_valid(&snapshot));
    return result;
}

static void identity_and_direction(void)
{
    begin();
    CHECK(commit(relation(1,2,1),-40));
    CHECK(snapshot.ap_count==0 && snapshot.relation_count==1);
    CHECK(snapshot.relations[0].row.to_ap_frames==1 && snapshot.relations[0].row.from_ap_frames==0);
    uint64_t station_time=snapshot.relations[0].row.to_ap_callback_us;
    CHECK(commit(relation(1,2,2),-70));
    CHECK(snapshot.relations[0].row.to_ap_callback_us==station_time);
    CHECK(snapshot.relations[0].row.to_ap_rssi==-40 && snapshot.relations[0].row.from_ap_rssi==-70);
    uint8_t binary[32]; for(unsigned i=0;i<32;++i)binary[i]=(uint8_t)(i*8);
    CHECK(commit(beacon(1,binary,32),-50));
    CHECK(snapshot.ap_count==1 && !memcmp(snapshot.aps[0].ssid,binary,32));
    uint32_t incarnation=snapshot.aps[0].incarnation;
    uint64_t first=snapshot.aps[0].first_callback_us;
    CHECK(commit(beacon(1,(const uint8_t *)"X",1),127));
    CHECK(snapshot.aps[0].incarnation==incarnation && snapshot.aps[0].first_callback_us==first);
    CHECK(snapshot.aps[0].ssid_length==1 && snapshot.aps[0].rssi==127 && !(snapshot.aps[0].flags&1));
    for(unsigned i=1;i<32;++i)CHECK(snapshot.aps[0].ssid[i]==0);
    CHECK(commit(beacon(1,(const uint8_t *)"",0),-60));
    CHECK(snapshot.aps[0].ssid_length==0 && snapshot.aps[0].capabilities==0x11);
    old=snapshot;
    CHECK(cerberus_ap_survey_snapshot(&table,now+500,&snapshot));
    CHECK(table.accepting && snapshot.receive_us==old.receive_us+500);
    CHECK(snapshot.aps[0].last_callback_us==old.aps[0].last_callback_us);
    CHECK(commit(relation(1,2,1),128));
    CHECK(snapshot.relations[0].row.to_ap_rssi==127 && !(snapshot.relations[0].row.rssi_valid&1));
    CHECK(old.relations[0].row.to_ap_rssi==-40);
}

static void replacement(void)
{
    begin();
    for(unsigned i=1;i<=32;++i)CHECK(commit(beacon(i,(const uint8_t *)"AP",2),-50));
    uint32_t replaced=snapshot.aps[0].incarnation;
    CHECK(commit(beacon(1,(const uint8_t *)"AP",2),-40));
    CHECK(commit(beacon(33,(const uint8_t *)"NEW",3),-70));
    CHECK(snapshot.ap_count==32 && snapshot.ap_replacements==1);
    CHECK(snapshot.aps[0].incarnation==replaced && snapshot.aps[1].bssid[5]==33);
    CHECK(snapshot.aps[1].incarnation>replaced);
    for(unsigned i=1;i<=33;++i)CHECK(commit(relation(1,100+i,1),-60));
    CHECK(snapshot.relation_count==32 && snapshot.relation_replacements==1);
    CHECK(snapshot.relations[0].row.station[5]==133);
    CHECK(snapshot.ap_frames==34 && snapshot.relation_frames==33 && snapshot.received_frames==67);
}

static void parser_and_bounds(void)
{
    const uint8_t ssid[3]={'A',0,'Z'};
    for(unsigned length=0;length<41;++length){
        begin(); beacon(1,ssid,3);
        CHECK(!commit(length,-50)); CHECK(snapshot.ap_count==0 && snapshot.rejected_frames==1);
    }
    begin(); size_t length=beacon(1,ssid,3); frame[length++]=1; frame[length++]=4;
    CHECK(!commit(length,-50));
    for(unsigned ds=0;ds<4;++ds){begin();CHECK(commit(relation(1,2,ds),-50)==(ds==1||ds==2));}
    begin(); relation(1,2,1); frame[0]=0x88; frame[1]|=0x40;
    CHECK(!commit(25,-55)); CHECK(commit(26,-55));
    begin(); relation(1,2,1); frame[22]=1; CHECK(!commit(24,-50));
    begin(); relation(1,2,1); frame[4]|=1; CHECK(!commit(24,-50));
    begin(); beacon(1,ssid,3); frame[34]=255; frame[35]=255; CHECK(commit(41,-127));
    CHECK(snapshot.aps[0].capabilities==65535 && snapshot.aps[0].rssi==-127);
}

static void admission_and_clock(void)
{
    begin(); cerberus_ap_survey_decoded_t decoded;
    beacon(1,(const uint8_t *)"A",1);
    CHECK(cerberus_ap_survey_decode(frame,39,1,-40,101,&decoded)==CERBERUS_AP_SURVEY_AP);
    cerberus_ap_survey_table_t before=table;
    CHECK(!cerberus_ap_survey_commit(&table,table.live.session_id+1,1,1,101,&decoded));
    CHECK(!cerberus_ap_survey_commit(&table,table.live.session_id,2,1,101,&decoded));
    CHECK(!cerberus_ap_survey_commit(&table,table.live.session_id,1,6,101,&decoded));
    CHECK(!memcmp(&before,&table,sizeof(table)));
    CHECK(cerberus_ap_survey_disarm(&table,table.live.session_id,1,150));
    CHECK(!cerberus_ap_survey_arm(&table,table.live.session_id,1,1,160));
    CHECK(!cerberus_ap_survey_arm(&table,table.live.session_id,2,1,149));
    CHECK(cerberus_ap_survey_arm(&table,table.live.session_id,2,1,160));
    before=table;
    CHECK(!cerberus_ap_survey_commit(&table,table.live.session_id,1,1,170,&decoded));
    CHECK(!memcmp(&before,&table,sizeof(table)));
    CHECK(!cerberus_ap_survey_snapshot(&table,159,&snapshot));
    CHECK(cerberus_ap_survey_snapshot(&table,180,&snapshot));
    CHECK(snapshot.receive_us==70);
    CHECK(!cerberus_ap_survey_disarm(&table,table.live.session_id,2,159));
    CHECK(!table.accepting);
    CHECK(!cerberus_ap_survey_snapshot(&table,(uint64_t)INT64_MAX+1,&snapshot));
}

static void snapshot_rejections(void)
{
    begin(); CHECK(commit(beacon(1,(const uint8_t *)"A",1),-50)); CHECK(commit(relation(1,2,1),-45)); old=snapshot;
#define BAD(statement) do{snapshot=old;statement;CHECK(!cerberus_ap_survey_snapshot_valid(&snapshot));}while(0)
    BAD(snapshot.ap_count=33); BAD(snapshot.relation_count=33); BAD(snapshot.flags|=128);
    BAD(snapshot.aps[1].ssid[0]=1); BAD(snapshot.relations[1].incarnation=9);
    BAD(snapshot.aps[0].ssid[31]=1); BAD(snapshot.aps[0].incarnation=0);
    BAD(snapshot.aps[0].rssi=127); BAD(snapshot.aps[0].last_callback_us=old.copied_source_us+1);
    BAD(snapshot.relations[0].incarnation=snapshot.aps[0].incarnation);
    BAD(snapshot.relations[0].row.to_ap_callback_us=old.copied_source_us+1);
    BAD(snapshot.received_frames=0); BAD(snapshot.applied_channels=0);
    BAD(snapshot.receive_us=old.copied_source_us+1); BAD(snapshot.revision=UINT32_MAX);
    BAD(snapshot.saturations=1); BAD(snapshot.flags|=CERBERUS_AP_SURVEY_SNAPSHOT_SATURATED);
#undef BAD
    snapshot=old; CHECK(cerberus_ap_survey_snapshot_valid(&snapshot));
    table.incarnation_counter=UINT32_MAX;
    CHECK(!commit(beacon(2,(const uint8_t *)"B",1),-50));
    CHECK(!table.accepting && (snapshot.flags&CERBERUS_AP_SURVEY_SNAPSHOT_EXHAUSTED));
    begin(); table.live.revision=UINT32_MAX-1;
    CHECK(commit(beacon(1,(const uint8_t *)"A",1),-50));
    CHECK(snapshot.revision==UINT32_MAX && !table.accepting);
}

static void decoded_evidence_and_stale_callbacks(void)
{
    begin();
    cerberus_ap_survey_decoded_t decoded, malformed;
    size_t length=beacon(1,(const uint8_t *)"AP",2);
    CHECK(cerberus_ap_survey_decode(frame,length,1,-40,120,&decoded)==CERBERUS_AP_SURVEY_AP);
    cerberus_ap_survey_table_t before=table;
    for(unsigned mutation=0;mutation<7;++mutation){
        malformed=decoded;
        switch(mutation){
        case 0:malformed.ap.incarnation=1;break;
        case 1:malformed.ap.frames=2;break;
        case 2:malformed.ap.first_callback_us=119;break;
        case 3:malformed.ap.last_callback_us=121;break;
        case 4:malformed.ap.channel=6;break;
        case 5:malformed.relation.bssid[0]=2;break;
        default:malformed.kind=255;break;
        }
        CHECK(!cerberus_ap_survey_commit(&table,table.live.session_id,1,1,120,&malformed));
        CHECK(!memcmp(&before,&table,sizeof(table)));
    }
    CHECK(cerberus_ap_survey_commit(&table,table.live.session_id,1,1,120,&decoded));
    CHECK(cerberus_ap_survey_decode(frame,length,1,-10,119,&decoded)==CERBERUS_AP_SURVEY_AP);
    CHECK(!cerberus_ap_survey_commit(&table,table.live.session_id,1,1,119,&decoded));
    CHECK(table.live.stale_frames==1&&table.live.received_frames==1&&table.latest_callback_us==120);
    CHECK(table.live.aps[0].last_callback_us==120&&table.live.aps[0].rssi==-40);
    memset(&decoded,0,sizeof(decoded));
    CHECK(!cerberus_ap_survey_commit(&table,table.live.session_id,1,1,121,&decoded));
    CHECK(table.live.rejected_frames==1&&table.live.received_frames==2&&table.live.aps[0].last_callback_us==120);
    CHECK(cerberus_ap_survey_snapshot(&table,121,&snapshot));
    CHECK(cerberus_ap_survey_snapshot_valid(&snapshot));
}

static void independent_channels_and_window_accounting(void)
{
    uint64_t mask=cerberus_wifi_capture_plan_channel_bit(1)|cerberus_wifi_capture_plan_channel_bit(6);
    CHECK(cerberus_ap_survey_begin(&table,55,mask,mask));
    CHECK(cerberus_ap_survey_note_channel(&table,1,true));
    CHECK(cerberus_ap_survey_arm(&table,55,1,1,100));
    cerberus_ap_survey_decoded_t decoded;
    size_t length=relation(1,2,1);
    CHECK(cerberus_ap_survey_decode(frame,length,1,-40,110,&decoded)==CERBERUS_AP_SURVEY_RELATION);
    CHECK(cerberus_ap_survey_commit(&table,55,1,1,110,&decoded));
    CHECK(cerberus_ap_survey_disarm(&table,55,1,200));
    CHECK(cerberus_ap_survey_disarm(&table,55,1,210));
    CHECK(table.live.receive_us==100);
    CHECK(cerberus_ap_survey_note_channel(&table,6,true));
    CHECK(!cerberus_ap_survey_arm(&table,55,2,6,209));
    CHECK(cerberus_ap_survey_arm(&table,55,2,6,300));
    CHECK(cerberus_ap_survey_decode(frame,length,6,-70,310,&decoded)==CERBERUS_AP_SURVEY_RELATION);
    CHECK(cerberus_ap_survey_commit(&table,55,2,6,310,&decoded));
    CHECK(table.live.relation_count==2&&table.live.relations[0].row.channel==1&&table.live.relations[1].row.channel==6);
    CHECK(table.live.relations[0].incarnation!=table.live.relations[1].incarnation);
    CHECK(cerberus_ap_survey_snapshot(&table,350,&snapshot));
    CHECK(cerberus_ap_survey_snapshot_valid(&snapshot)&&snapshot.receive_us==150);
    CHECK(cerberus_ap_survey_snapshot(&table,360,&snapshot));
    CHECK(snapshot.receive_us==160&&table.live.receive_us==100);
    CHECK(cerberus_ap_survey_disarm(&table,55,2,400));
    CHECK(table.live.receive_us==200);
    CHECK(cerberus_ap_survey_note_channel(&table,1,true));
    CHECK(cerberus_ap_survey_arm(&table,55,3,1,(uint64_t)INT64_MAX-100));
    CHECK(cerberus_ap_survey_disarm(&table,55,3,(uint64_t)INT64_MAX));
    CHECK(cerberus_ap_survey_snapshot(&table,(uint64_t)INT64_MAX,&snapshot));
    CHECK(cerberus_ap_survey_snapshot_valid(&snapshot)&&snapshot.receive_us==300);
}

static void saturation_and_replacement_identity(void)
{
    begin();CHECK(commit(beacon(1,(const uint8_t *)"A",1),-40));
    table.live.ap_frames=table.live.received_frames=table.live.aps[0].frames=UINT32_MAX-1U;
    CHECK(commit(beacon(1,(const uint8_t *)"A",1),-41));
    CHECK(snapshot.ap_frames==UINT32_MAX&&snapshot.received_frames==UINT32_MAX&&!snapshot.saturations);
    CHECK(commit(beacon(1,(const uint8_t *)"A",1),-42));
    CHECK(snapshot.saturations==3&&(snapshot.flags&CERBERUS_AP_SURVEY_SNAPSHOT_SATURATED));
    CHECK(snapshot.aps[0].rssi==-42&&snapshot.aps[0].last_callback_us==now);
    begin();
    for(unsigned i=1;i<=32;++i)CHECK(commit(relation(1,100+i,1),-50));
    old=snapshot;
    CHECK(commit(relation(1,133,1),-60));
    CHECK(snapshot.relations[0].incarnation!=old.relations[0].incarnation);
    CHECK(old.relations[0].row.station[5]==101&&snapshot.relations[0].row.station[5]==133);
    CHECK(commit(relation(1,101,2),-70));
    CHECK(snapshot.relations[1].row.station[5]==101&&snapshot.relations[1].row.directions==2);
    CHECK(snapshot.relations[1].row.to_ap_frames==0&&snapshot.relations[1].row.to_ap_callback_us==0);
    CHECK(snapshot.relations[1].incarnation!=old.relations[0].incarnation);
}

int main(void)
{
    identity_and_direction(); replacement(); parser_and_bounds(); admission_and_clock(); snapshot_rejections();
    decoded_evidence_and_stale_callbacks(); independent_channels_and_window_accounting(); saturation_and_replacement_identity();
    printf("Continuous AP/client collector: %u checks; decoder, directional ages, live snapshots, replacement, stale arms and exhaustion passed.\n",checks);
    printf("table=%zu snapshot=%zu AP=%zu relation=%zu\n",sizeof(table),sizeof(snapshot),sizeof(snapshot.aps[0]),sizeof(snapshot.relations[0]));
    return 0;
}
