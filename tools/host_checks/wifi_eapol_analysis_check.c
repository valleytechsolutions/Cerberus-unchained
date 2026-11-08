#include "wifi_eapol_analysis.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const uint8_t ap[6] = {2,3,4,5,6,7};
static cerberus_wifi_eapol_table_t table;
static cerberus_wifi_eapol_observation_t meta;
static uint8_t frame[512];
static unsigned checks;
#define CHECK(x) do { ++checks; assert(x); } while(0)
static void put16(uint8_t *p, unsigned n) { p[0]=(uint8_t)(n>>8);p[1]=(uint8_t)n; }
static void put64(uint8_t *p, uint64_t n) { for(unsigned i=0;i<8;++i){p[7-i]=(uint8_t)n;n>>=8;} }
static size_t build(unsigned phase, unsigned station, uint8_t descriptor, uint8_t version, unsigned mic_length,
    uint64_t replay, size_t data_length)
{
    memset(frame,0,sizeof(frame));
    bool from_ap=phase==1||phase==3;
    frame[0]=8;frame[1]=from_ap?2:1;
    uint8_t sta[6]={2,0xbb,0xcc,0xdd,0xee,(uint8_t)station};
    memcpy(frame+(from_ap?10:4),ap,6);memcpy(frame+(from_ap?4:10),sta,6);
    memcpy(frame+16,ap,6);
    memcpy(frame+24,"\xaa\xaa\x03\x00\x00\x00\x88\x8e",8);
    uint8_t *eapol=frame+32,*key=eapol+4;
    eapol[0]=2;eapol[1]=3;put16(eapol+2,(unsigned)(79+mic_length+data_length));
    static const unsigned flags[]={0,0x0088,0x0108,0x13c8,0x0308};
    key[0]=descriptor;put16(key+1,flags[phase]|version);put16(key+3,16);put64(key+5,replay);
    if(phase!=4)memset(key+13,phase==2?0x22:0x11,32);
    if(phase!=1)memset(key+77,0xa5,mic_length);
    put16(key+77+mic_length,(unsigned)data_length);
    return 36+79+mic_length+data_length;
}
static int feed(size_t length)
{
    ++meta.sequence;meta.observed_us+=100;
    return cerberus_wifi_eapol_observe(&table,&meta,frame,length);
}
static void init(void)
{
    CHECK(cerberus_wifi_eapol_init(&table,ap,6));
    meta=(cerberus_wifi_eapol_observation_t){.channel=6,.rssi=-55};
}
static void exchanges(void)
{
    init();
    for(unsigned phase=1;phase<=4;++phase){size_t n=build(phase,1,2,2,16,phase<=2?100:101,0);CHECK(feed(n)==0);}
    CHECK(table.summary.exchanges==1&&table.records[0].exchanges==1&&table.summary.count==1);
    for(unsigned p=0;p<4;++p)CHECK(table.summary.phase[p]==1&&table.records[0].phase[p]==1);
    uint64_t first=table.records[0].first_us;uint32_t first_seq=table.records[0].first_sequence;
    size_t n=build(4,1,2,2,16,101,0);CHECK(feed(n)==0&&table.summary.exchanges==1&&table.summary.retries==1);
    CHECK(table.records[0].first_us==first&&table.records[0].first_sequence==first_seq);
    cerberus_wifi_eapol_record_t before=table.records[0];
    CHECK(cerberus_wifi_eapol_observe(&table,&meta,frame,n)==-1);
    CHECK(!memcmp(&before,&table.records[0],sizeof(before))&&table.summary.sequence_rejected==1);
    /* A higher-counter M3 retransmission after completion remains one exchange. */
    CHECK(feed(build(3,1,2,2,16,102,0))==0);CHECK(feed(build(4,1,2,2,16,102,0))==0);
    CHECK(table.summary.exchanges==1&&table.summary.retries==3);
    /* New exchange, including a higher-counter retransmitted M3 before M4. */
    CHECK(feed(build(1,1,2,2,16,200,0))==0);CHECK(feed(build(2,1,2,2,16,200,0))==0);
    CHECK(feed(build(3,1,2,2,16,201,0))==0);CHECK(feed(build(3,1,2,2,16,202,0))==0);
    CHECK(feed(build(4,1,2,2,16,202,0))==0&&table.summary.exchanges==2);
    /* Same BSSID, different stations never form a combined exchange. */
    init();
    for(unsigned p=1;p<=4;++p)CHECK(feed(build(p,p,2,2,16,p<=2?1:2,0))>=0);
    CHECK(table.summary.exchanges==0&&table.summary.count==4);
    /* Wrong replay, wrong nonce, incompatible descriptor or expiration fail. */
    for(unsigned fault=0;fault<5;++fault){
        init();CHECK(feed(build(1,1,2,2,16,10,0))==0);
        n=build(2,1,2,2,16,fault==0?11:10,0);
        if(fault==1)memcpy(frame+36+13,table.exchange[0].anonce,32);
        if(fault==2)frame[36]=254;
        if(fault==3)meta.observed_us+=CERBERUS_WIFI_EAPOL_EXCHANGE_US;
        CHECK(feed(n)==0);
        n=build(3,1,2,2,16,11,0);if(fault==4)frame[36+13]^=1;CHECK(feed(n)==0);
        CHECK(feed(build(4,1,2,2,16,11,0))==0);CHECK(table.summary.exchanges==0);
    }
    /* WPA's legacy M4 can lack Secure; RSN M3 cannot. */
    init();for(unsigned p=1;p<=4;++p){n=build(p,1,254,1,16,p<=2?1:2,0);frame[37]&=(uint8_t)~2U;CHECK(feed(n)==0);}
    CHECK(table.summary.exchanges==1);
    init();n=build(3,1,2,2,16,2,0);frame[37]&=(uint8_t)~2U;CHECK(feed(n)==0);
    CHECK(table.summary.phase[2]==0);
}
static void layouts_and_pmkid(void)
{
    for(unsigned mic=16;mic<=24;mic+=8){
        init();for(unsigned p=1;p<=4;++p)CHECK(feed(build(p,1,2,0,mic,p<=2?1:2,0))==0);
        CHECK(table.summary.exchanges==1&&table.summary.unknown_layout==0);
    }
    init();size_t n=build(1,1,2,0,16,1,8);
    /* Offset101 is zero, so both16-byte+8data and24-byte+0data are plausible. */
    CHECK(feed(n)==0&&table.summary.unknown_layout==1&&table.exchange[0].stage==0);
    init();n=build(1,1,2,2,16,1,22);uint8_t *data=frame+131;
    data[0]=221;data[1]=20;memcpy(data+2,"\x00\x0f\xac\x04",4);memset(data+6,0x66,16);
    CHECK(feed(n)==0&&table.summary.pmkid_frames==1&&table.summary.pmkid_elements==1);
    CHECK(table.records[0].pmkid_frames==1);
    /* A truncated tail cannot lend credibility to an earlier valid KDE. */
    put16(frame+34,118);put16(frame+129,23);frame[n++]=221;
    CHECK(feed(n)==-1&&table.summary.pmkid_frames==1&&table.summary.malformed==1);
    init();n=build(3,1,2,2,16,2,22);data=frame+131;
    data[0]=221;data[1]=20;memcpy(data+2,"\x00\x0f\xac\x04",4);memset(data+6,0x66,16);
    CHECK(feed(n)==0&&table.summary.pmkid_frames==0&&table.summary.encrypted_key_data==1);
    /* WPA key data is not an RSN KDE stream. */
    init();n=build(2,1,254,1,16,1,5);memset(frame+131,0xff,5);
    CHECK(feed(n)==0&&table.summary.pmkid_frames==0);
    init();n=build(1,1,2,2,16,1,22);data=frame+131;data[0]=221;data[1]=19;
    memcpy(data+2,"\x00\x0f\xac\x04",4);CHECK(feed(n)==-1&&table.summary.malformed==1);
}
static void boundaries(void)
{
    init();size_t n=build(1,1,2,2,16,1,0);uint8_t saved[512];memcpy(saved,frame,n);
    for(size_t len=0;len<n;++len){init();memcpy(frame,saved,n);CHECK(feed(len)==-1&&table.summary.keys==0);}
    for(unsigned fault=0;fault<13;++fault){
        init();n=build(1,1,2,2,16,1,0);
        switch(fault){
        case 0:frame[1]|=0x40;break;case 1:frame[1]|=4;break;case 2:frame[22]=1;break;
        case 3:frame[10]^=2;break;case 4:frame[0]|=1;break;case 5:frame[1]=3;break;
        case 6:frame[4]|=1;break;case 7:frame[24]=0;break;case 8:frame[32]=0;break;
        case 9:put16(frame+34,512);break;case 10:put16(frame+129,1);break;
        case 11:frame[36]=1;break;case 12:frame[38]=(uint8_t)((frame[38]&~7U)|4U);break;
        }
        CHECK(feed(n)==-1&&table.summary.count==0);
    }
    /* QoS + HT-control offsets, and A-MSDU exclusion. */
    init();n=build(1,1,2,2,16,1,0);memmove(frame+30,frame+24,n-24);memset(frame+24,0,6);
    frame[0]=0x88;frame[1]|=0x80;n+=6;CHECK(feed(n)==0);
    frame[24]=0x80;CHECK(feed(n)==-1);
    /* MIC/key-info shape never pretends a phase when contradictory. */
    for(unsigned fault=0;fault<4;++fault){init();n=build(2,1,2,2,16,1,0);
        if(fault==0)memset(frame+36+77,0,16);
        if(fault==1)frame[37]|=4;
        if(fault==2)frame[37]|=8;
        if(fault==3)frame[38]&=(uint8_t)~8U;
        CHECK(feed(n)==0&&table.summary.phase[1]==0);
    }
    init();n=build(1,1,2,2,16,1,0);CHECK(feed(n)==0);
    uint64_t stamp=table.records[0].last_us;meta.observed_us=0;
    ++meta.sequence;CHECK(cerberus_wifi_eapol_observe(&table,&meta,frame,n)==-1&&table.summary.clock_rejected==1);
    CHECK(table.records[0].last_us==stamp);
    ++meta.sequence;meta.channel=11;CHECK(cerberus_wifi_eapol_observe(&table,&meta,frame,n)==-1&&table.summary.scope_rejected==1);
    meta.channel=6;meta.rssi=127;meta.observed_us=1000;CHECK(feed(n)==0&&!table.records[0].rssi_valid);
    cerberus_wifi_eapol_record_t r;memset(&r,0xff,sizeof(r));CHECK(!cerberus_wifi_eapol_copy(&table,99,&r));
    uint8_t zeros[sizeof(r)];memset(zeros,0,sizeof(zeros));CHECK(!memcmp(&r,zeros,sizeof(r)));
}
static void capacity_and_saturation(void)
{
    init();for(unsigned i=0;i<CERBERUS_WIFI_EAPOL_MAX_PEERS;++i)CHECK(feed(build(1,i,2,2,16,1,0))==(int)i);
    CHECK(feed(build(1,99,2,2,16,1,0))==-1&&table.summary.peer_dropped==1);
    meta.observed_us+=CERBERUS_WIFI_EAPOL_EXCHANGE_US;
    CHECK(feed(build(1,99,2,2,16,1,0))==0&&table.summary.peer_replaced==1&&table.records[0].station[5]==99);
    CHECK(table.records[0].first_sequence==meta.sequence);
    table.summary.keys=UINT32_MAX;CHECK(feed(build(2,99,2,2,16,1,0))==0&&table.summary.saturated&&table.summary.keys==UINT32_MAX);
    /* Deterministic arbitrary input over every short length must stay bounded. */
    uint32_t rng=17;
    for(unsigned i=0;i<10000;++i){for(unsigned j=0;j<sizeof(frame);++j){rng=rng*1664525U+1013904223U;frame[j]=(uint8_t)(rng>>24);}
        (void)feed(i%sizeof(frame));CHECK(table.summary.count<=CERBERUS_WIFI_EAPOL_MAX_PEERS);}
}
static void maximum_key_data(void)
{
    /* A maximal clear key-data stream: many zero-length IEs, then a PMKID KDE
     * whose value contains zeros, followed by padding. No suffix rescans. */
    uint8_t large[CERBERUS_WIFI_EAPOL_FRAME_MAX+1U];
    const size_t body_offset=36U, data_offset=131U;
    init();(void)build(1,1,2,2,16,1,0);
    memset(large,0,sizeof(large));memcpy(large,frame,data_offset);
    put16(large+34,(unsigned)(CERBERUS_WIFI_EAPOL_FRAME_MAX-body_offset));
    put16(large+129,(unsigned)(CERBERUS_WIFI_EAPOL_FRAME_MAX-data_offset));
    uint8_t *kde=large+CERBERUS_WIFI_EAPOL_FRAME_MAX-23U;
    CHECK((size_t)(kde-(large+data_offset))%2U==0U);
    kde[0]=221;kde[1]=20;memcpy(kde+2,"\x00\x0f\xac\x04",4);kde[6]=0x66;
    meta.sequence=1;meta.observed_us=100;
    CHECK(cerberus_wifi_eapol_observe(&table,&meta,large,CERBERUS_WIFI_EAPOL_FRAME_MAX)==0);
    CHECK(table.summary.keys==1&&table.summary.pmkid_elements==1&&table.summary.phase[0]==1);
    ++meta.sequence;meta.observed_us+=100;
    CHECK(cerberus_wifi_eapol_observe(&table,&meta,large,sizeof(large))==-1);
    CHECK(table.summary.malformed==1&&table.summary.keys==1);
    /* A valid KDE before a nonzero truncated padding tail proves nothing. */
    large[CERBERUS_WIFI_EAPOL_FRAME_MAX-1U]=221U;
    ++meta.sequence;meta.observed_us+=100;
    CHECK(cerberus_wifi_eapol_observe(&table,&meta,large,CERBERUS_WIFI_EAPOL_FRAME_MAX)==-1);
    CHECK(table.summary.malformed==2&&table.summary.pmkid_elements==1);
    /* Vendor padding must terminate the stream, not conceal a later KDE. */
    large[data_offset]=221;large[data_offset+1U]=0;
    large[CERBERUS_WIFI_EAPOL_FRAME_MAX-1U]=0;
    ++meta.sequence;meta.observed_us+=100;
    CHECK(cerberus_wifi_eapol_observe(&table,&meta,large,CERBERUS_WIFI_EAPOL_FRAME_MAX)==-1);
    CHECK(table.summary.malformed==3&&table.summary.pmkid_elements==1);
}

static cerberus_wifi_capture_plan_t plan(bool selected)
{
    cerberus_wifi_capture_plan_t p;
    cerberus_wifi_capture_plan_clear(&p);
    p.dwell_ms = CERBERUS_WIFI_CAPTURE_PLAN_EAPOL_DWELL_MS;
    if (selected) CHECK(cerberus_wifi_capture_plan_add_target(&p, ap, 6));
    else p.channel_mask = cerberus_wifi_capture_plan_band_mask(CERBERUS_WIFI_CAPTURE_PLAN_BOTH);
    CHECK(cerberus_wifi_capture_plan_valid(&p));
    return p;
}

static void replace_ap(const uint8_t bssid[6])
{
    /* Addr3 deliberately stays unchanged: infrastructure DS direction is the
     * BSSID proof, not the final source/destination address. */
    memcpy(frame + ((frame[1] & 3U) == 2U ? 10U : 4U), bssid, 6U);
}

static void planned_peers(void)
{
    const uint8_t second_ap[6] = {4,3,4,5,6,7};
    cerberus_wifi_capture_plan_t p = plan(true), frozen = p;
    CHECK(cerberus_wifi_eapol_init_options(&table, 0, false, NULL, &p));
    p.bssids[0][0] = 0x44; p.channel_mask = 1U;
    CHECK(!memcmp(&frozen, &table.plan, sizeof(frozen)));
    meta = (cerberus_wifi_eapol_observation_t){.channel=6,.rssi=-52};
    size_t n = build(1,1,2,2,16,1,0); replace_ap(second_ap);
    CHECK(feed(n) == -1 && table.summary.scope_rejected == 1 && !table.summary.count);
    CHECK(feed(build(1,1,2,2,16,1,0)) == 0);
    meta.channel = 36; CHECK(feed(build(2,1,2,2,16,1,0)) == -1);
    CHECK(table.summary.scope_rejected == 2 && table.records[0].keys == 1);
    p = plan(false);
    CHECK(!cerberus_wifi_eapol_init_options(&table, 6, false, NULL, &p));
    CHECK(!cerberus_wifi_eapol_init_options(&table, 0, true, ap, &p));
    p.dwell_ms = CERBERUS_WIFI_CAPTURE_PLAN_DWELL_MS;
    CHECK(!cerberus_wifi_eapol_init_options(&table, 0, false, NULL, &p));
    CHECK(!cerberus_wifi_eapol_init_options(&table, 0, false, NULL, NULL));
    CHECK(!cerberus_wifi_eapol_init_options(&table, 15, false, NULL, NULL));

    /* One station associated with two APs owns two independent correlations. */
    p = plan(false); CHECK(cerberus_wifi_eapol_init_options(&table, 0, false, NULL, &p));
    meta = (cerberus_wifi_eapol_observation_t){.channel=6,.rssi=-52};
    for (unsigned phase=1; phase<=4; ++phase) {
        for (unsigned network=0; network<2; ++network) {
            n = build(phase,1,2,2,16,phase<=2?10:11,0);
            if (network) replace_ap(second_ap);
            CHECK(feed(n) == (int)network);
        }
    }
    CHECK(table.summary.count == 2 && table.summary.exchanges == 2);
    CHECK(!memcmp(table.records[0].bssid,ap,6) && !memcmp(table.records[1].bssid,second_ap,6));
    CHECK(!memcmp(table.records[0].station,table.records[1].station,6));
    CHECK(table.records[0].exchanges == 1 && table.records[1].exchanges == 1);
    CHECK(cerberus_wifi_eapol_init_options(&table, 6, false, NULL, NULL));
    meta = (cerberus_wifi_eapol_observation_t){.channel=6,.rssi=-52};
    for (unsigned phase=1; phase<=4; ++phase) {
        n = build(phase,1,2,2,16,phase<=2?10:11,0);
        if (phase % 2U == 0U) replace_ap(second_ap);
        CHECK(feed(n) >= 0);
    }
    CHECK(table.summary.count == 2 && !table.summary.exchanges);

    /* A reused BSSID/station on another channel cannot supply missing phases. */
    CHECK(cerberus_wifi_eapol_init_options(&table, 0, false, NULL, &p));
    meta = (cerberus_wifi_eapol_observation_t){.channel=6,.rssi=-52};
    CHECK(feed(build(1,1,2,2,16,10,0)) == 0);
    meta.channel = 36; CHECK(feed(build(2,1,2,2,16,10,0)) == 0);
    meta.channel = 6;
    CHECK(feed(build(3,1,2,2,16,11,0)) == 0);
    CHECK(feed(build(4,1,2,2,16,11,0)) == 0 && !table.summary.exchanges);
    meta.channel = 36;
    for (unsigned phase=1; phase<=4; ++phase) CHECK(feed(build(phase,1,2,2,16,phase<=2?20:21,0)) == 0);
    CHECK(table.summary.exchanges == 1 && table.records[0].channel == 36);
    CHECK(table.records[0].gaps > 0);
    /* A group address cannot become an AP in an unfiltered plan. */
    n = build(1,1,2,2,16,30,0); frame[10] |= 1U;
    CHECK(feed(n) == -1 && table.summary.count == 1);
}

static size_t beacon(const uint8_t bssid[6], const uint8_t *ssid, size_t length)
{
    memset(frame, 0, sizeof(frame)); frame[0] = 0x80;
    memset(frame + 4, 0xff, 6); memcpy(frame + 10, bssid, 6); memcpy(frame + 16, bssid, 6);
    frame[34] = 0x34; frame[35] = 0x12; frame[36] = 0; frame[37] = (uint8_t)length;
    if (length) memcpy(frame + 38, ssid, length);
    return 38U + length;
}

static cerberus_wifi_eapol_result_t feed_full(size_t length)
{
    ++meta.sequence; meta.observed_us += 100;
    return cerberus_wifi_eapol_observe_full(&table, &meta, frame, length);
}

static void beacon_context(void)
{
    const uint8_t binary[] = {'A',0,'\r',0xff};
    init(); size_t n = build(1,1,2,2,16,1,0); CHECK(feed(n) == 0);
    cerberus_wifi_eapol_record_t key_before = table.records[0];
    n = beacon(ap, binary, sizeof(binary)); frame[10] ^= 2U;
    frame[n++] = 3; frame[n++] = 1; frame[n++] = 36;
    cerberus_wifi_eapol_result_t result = feed_full(n);
    CHECK(result.peer_index == -1 && result.context_index == 0);
    CHECK(table.summary.keys == 1 && table.summary.count == 1 && table.summary.beacons == 1);
    CHECK(!memcmp(&key_before,&table.records[0],sizeof(key_before)));
    CHECK(table.contexts[0].ssid_length == sizeof(binary) && !memcmp(table.contexts[0].ssid,binary,sizeof(binary)));
    CHECK(table.contexts[0].channel == 6 && table.contexts[0].advertised_channel == 36);
    CHECK(table.contexts[0].capabilities == 0x1234 && !table.contexts[0].hidden_like);
    CHECK(table.contexts[0].transmitter[0] != table.contexts[0].bssid[0]);
    cerberus_wifi_eapol_context_t frozen = table.contexts[0], copied;
    CHECK(cerberus_wifi_eapol_context_copy(&table,0,&copied) && !memcmp(&copied,&frozen,sizeof(copied)));
    result = cerberus_wifi_eapol_observe_full(&table,&meta,frame,n);
    CHECK(result.peer_index == -1 && result.context_index == -1 && table.summary.sequence_rejected == 1);
    CHECK(!memcmp(&frozen,&table.contexts[0],sizeof(frozen)));
    /* Full IE validation: no partial SSID claim escapes a malformed tail. */
    frame[n++] = 3; frame[n++] = 1; frame[n++] = 6;
    result = feed_full(n); CHECK(result.context_index == -1 && table.summary.beacon_malformed == 1);
    CHECK(!memcmp(&frozen,&table.contexts[0],sizeof(frozen)));
    n = beacon(ap,NULL,0); result = feed_full(n);
    CHECK(result.context_index == 0 && table.contexts[0].hidden_like && !table.contexts[0].ssid_length);
    CHECK(!memcmp(&key_before,&table.records[0],sizeof(key_before)));
    const uint8_t hidden[] = {0,0,0}; n = beacon(ap,hidden,sizeof(hidden)); CHECK(feed_full(n).context_index == 0);
    CHECK(table.contexts[0].hidden_like && table.contexts[0].ssid_length == sizeof(hidden));
    CHECK(table.contexts[0].first_sequence == frozen.first_sequence && table.contexts[0].frames == 3);
    meta.rssi = 127; CHECK(feed_full(n).context_index == 0);
    CHECK(!table.contexts[0].rssi_valid); meta.rssi = -55;
    n = build(2,1,2,2,16,1,0); frozen = table.contexts[0]; CHECK(feed(n) == 0);
    CHECK(!memcmp(&frozen,&table.contexts[0],sizeof(frozen)));
    memset(&copied,0x5a,sizeof(copied)); CHECK(!cerberus_wifi_eapol_context_copy(&table,16,&copied));
    cerberus_wifi_eapol_context_t zero = {0}; CHECK(!memcmp(&copied,&zero,sizeof(zero)));

    cerberus_wifi_capture_plan_t p = plan(true);
    CHECK(cerberus_wifi_eapol_init_options(&table,0,false,NULL,&p));
    meta = (cerberus_wifi_eapol_observation_t){.channel=6,.rssi=-52};
    uint8_t other_ap[6]; memcpy(other_ap,ap,6); other_ap[0] = 4;
    CHECK(feed_full(beacon(other_ap,binary,sizeof(binary))).context_index == -1);
    CHECK(table.summary.scope_rejected == 1 && !table.summary.context_count);
    p = plan(false); CHECK(cerberus_wifi_eapol_init_options(&table,0,false,NULL,&p));
    for (unsigned i=0; i<CERBERUS_WIFI_EAPOL_MAX_CONTEXTS; ++i) {
        other_ap[5] = (uint8_t)i;
        CHECK(feed_full(beacon(other_ap,binary,sizeof(binary))).context_index == (int)i);
    }
    CHECK(!table.summary.count && !table.summary.keys && !table.summary.exchanges);
    other_ap[5] = 99; CHECK(feed_full(beacon(other_ap,binary,sizeof(binary))).context_index == -1);
    CHECK(table.summary.context_count == 16 && table.summary.beacon_dropped == 1);
    other_ap[5] = 0; table.contexts[0].frames = UINT32_MAX;
    CHECK(feed_full(beacon(other_ap,binary,sizeof(binary))).context_index == 0);
    CHECK(table.summary.saturated && table.contexts[0].frames == UINT32_MAX);
    frozen = table.contexts[0]; n = beacon(other_ap,binary,sizeof(binary));
    ++meta.sequence; meta.observed_us = 0;
    CHECK(cerberus_wifi_eapol_observe_full(&table,&meta,frame,n).context_index == -1);
    CHECK(table.summary.clock_rejected == 1 && !memcmp(&frozen,&table.contexts[0],sizeof(frozen)));
}
int main(void)
{
    exchanges();layouts_and_pmkid();boundaries();capacity_and_saturation();maximum_key_data();
    planned_peers();beacon_context();
    printf("Passive EAPOL analysis: %u checks; %zu-byte bounded table\n",checks,sizeof(table));return 0;
}
