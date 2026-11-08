#ifdef NDEBUG
#undef NDEBUG
#endif
#include "recon_journal.h"
#include "sentinel_watch_detect.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { ++checks; assert(x); } while (0)

static cerberus_recon_record_t wifi(uint64_t sequence)
{
    cerberus_recon_record_t r = {
        .session_id=9, .sequence=sequence, .consumed_us=100,
        .address={0x02,0x12,0x34,0xab,0xcd,0xef},
        .radio=CERBERUS_RECON_RADIO_WIFI, .generation=255,
        .channel=6, .channel_valid=true, .rssi=-45, .rssi_valid=true,
    };
    return r;
}

static cerberus_recon_record_t ble(uint64_t sequence)
{
    cerberus_recon_record_t r=wifi(sequence);
    r.radio=CERBERUS_RECON_RADIO_BLE;
    r.address_type=1;
    r.channel=0; r.channel_valid=false;
    return r;
}

static void validation(void)
{
    cerberus_recon_record_t r=wifi(1), b=ble(1);
    CHECK(cerberus_recon_record_valid(&r) && cerberus_recon_record_valid(&b));
    CHECK(!cerberus_recon_record_valid(NULL));
    for(unsigned i=0;i<256;++i) {
        cerberus_recon_record_t x=r;
        x.radio=(uint8_t)i;
        CHECK(cerberus_recon_record_valid(&x)==(i==CERBERUS_RECON_RADIO_WIFI));
        x=r; x.address_type=(uint8_t)i;
        CHECK(cerberus_recon_record_valid(&x)==(i==0));
        x=b; x.address_type=(uint8_t)i;
        CHECK(cerberus_recon_record_valid(&x)==(i<=3));
        x=r; x.channel=(uint8_t)i;
        CHECK(cerberus_recon_record_valid(&x)==(i>=1&&i<=196));
        x.channel_valid=false;
        CHECK(cerberus_recon_record_valid(&x));
        x=r; x.rssi=(int8_t)i;
        CHECK(cerberus_recon_record_valid(&x)==(x.rssi>=-127&&x.rssi<=0));
        x.rssi_valid=false; CHECK(cerberus_recon_record_valid(&x));
        x=b; x.rssi=(int8_t)i;
        CHECK(cerberus_recon_record_valid(&x)==(x.rssi>=-127&&x.rssi<=20));
        x.rssi_valid=false; CHECK(cerberus_recon_record_valid(&x));
        x=b; x.alert_state=(uint8_t)i;
        CHECK(cerberus_recon_record_valid(&x)==(i<=CERBERUS_RECON_ALERT_NOT_QUEUED));
    }
    r=wifi(0);CHECK(!cerberus_recon_record_valid(&r));
    r=wifi(1);r.session_id=0;CHECK(!cerberus_recon_record_valid(&r));
    r=wifi(1);r.name_length=33;CHECK(!cerberus_recon_record_valid(&r));
    r=wifi(1);r.source_limits=32;CHECK(!cerberus_recon_record_valid(&r));
    r=wifi(1);r.source_limits=31;CHECK(cerberus_recon_record_valid(&r));
    r=wifi(1);r.ibeacon=true;CHECK(!cerberus_recon_record_valid(&r));
    b=ble(1);b.ibeacon=true;CHECK(cerberus_recon_record_valid(&b));
    b.channel=1;CHECK(!cerberus_recon_record_valid(&b));
    b.channel=0;b.channel_valid=true;CHECK(!cerberus_recon_record_valid(&b));
    r=wifi(1); r.source_kind=CERBERUS_RECON_SOURCE_C5_AGE_MS;
    CHECK(!cerberus_recon_record_valid(&r));
    r.source_flags=CERBERUS_RECON_SOURCE_TIME_VALID|CERBERUS_RECON_SOURCE_ANCHOR_VALID;
    CHECK(cerberus_recon_record_valid(&r)); /* Genuine zero source and anchor. */
    r.source_time=UINT32_MAX;r.source_anchor_us=100;
    CHECK(cerberus_recon_record_valid(&r));
    r.source_time=(uint64_t)UINT32_MAX+1;
    CHECK(!cerberus_recon_record_valid(&r));r.source_time=7;
    r.source_anchor_us=101;CHECK(!cerberus_recon_record_valid(&r));
    r.source_anchor_us=0;r.source_flags=CERBERUS_RECON_SOURCE_TIME_VALID;
    CHECK(!cerberus_recon_record_valid(&r));
    b=ble(1); b.source_kind=CERBERUS_RECON_SOURCE_S3_ADV_MS32;
    b.source_flags=CERBERUS_RECON_SOURCE_TIME_VALID|CERBERUS_RECON_SOURCE_SEQUENCE_VALID;
    CHECK(!cerberus_recon_record_valid(&b));b.source_sequence=1;
    CHECK(cerberus_recon_record_valid(&b));
    b.source_time=UINT32_MAX;CHECK(cerberus_recon_record_valid(&b));
    b.source_time=(uint64_t)UINT32_MAX+1;CHECK(!cerberus_recon_record_valid(&b));
    b.source_time=10;b.source_kind=CERBERUS_RECON_SOURCE_S3_RESPONSE_MS32;
    CHECK(cerberus_recon_record_valid(&b));
    b.source_kind=CERBERUS_RECON_SOURCE_S3_CALLBACK_US;b.source_time=101;
    CHECK(!cerberus_recon_record_valid(&b));b.source_time=100;
    CHECK(cerberus_recon_record_valid(&b));
    b.source_flags|=CERBERUS_RECON_SOURCE_ANCHOR_VALID;
    CHECK(!cerberus_recon_record_valid(&b));
    for(unsigned bit=3;bit<8;++bit) {
        b=ble(1);b.source_flags=(uint8_t)(1U<<bit);
        CHECK(!cerberus_recon_record_valid(&b));
    }
    b=ble(1);b.source_time=1;CHECK(!cerberus_recon_record_valid(&b));
    b=ble(1);b.source_anchor_us=1;CHECK(!cerberus_recon_record_valid(&b));
    b=ble(1);b.source_sequence=1;CHECK(!cerberus_recon_record_valid(&b));
    b=ble(1);b.classified=true;b.category=SENTINEL_WATCH_BEACON;
    b.confidence=SENTINEL_WATCH_CONFIDENCE_HIGH;b.mode=2;
    strcpy(b.rule,"beacon.ibeacon");
    CHECK(cerberus_recon_record_valid(&b));
    for(unsigned i=0;i<256;++i) {
        cerberus_recon_record_t x=b;x.category=(uint8_t)i;
        CHECK(cerberus_recon_record_valid(&x)==(i>0&&i<SENTINEL_WATCH_CATEGORY_COUNT));
        x=b;x.confidence=(uint8_t)i;
        CHECK(cerberus_recon_record_valid(&x)==(i>0&&i<=SENTINEL_WATCH_CONFIDENCE_HIGH));
        x=b;x.mode=(uint8_t)i;CHECK(cerberus_recon_record_valid(&x)==(i<=2));
        x=b;x.alert_state=(uint8_t)i;CHECK(cerberus_recon_record_valid(&x)==(i<=2));
    }
    b.rule[0]=0;CHECK(!cerberus_recon_record_valid(&b));
    memset(b.rule,'a',sizeof(b.rule));CHECK(!cerberus_recon_record_valid(&b));
    b.rule[31]=0;CHECK(cerberus_recon_record_valid(&b));
    b.rule[0]='=';CHECK(!cerberus_recon_record_valid(&b));
    b.rule[0]='a';b.rule[1]='\n';CHECK(!cerberus_recon_record_valid(&b));
    b=ble(1);strcpy(b.rule,"unexpected");CHECK(!cerberus_recon_record_valid(&b));
    b=ble(1);b.category=1;CHECK(!cerberus_recon_record_valid(&b));
    b=ble(1);b.confidence=1;CHECK(!cerberus_recon_record_valid(&b));
    b=ble(1);b.mode=1;CHECK(!cerberus_recon_record_valid(&b));
}

static void queue_contract(void)
{
    cerberus_recon_journal_t j;
    memset(&j,0xa5,sizeof(j));
    cerberus_recon_journal_t prior=j;
    CHECK(!cerberus_recon_journal_init(&j,0,0)&&!memcmp(&j,&prior,sizeof(j)));
    CHECK(!cerberus_recon_journal_init(NULL,1,0));
    CHECK(cerberus_recon_journal_init(&j,9,0));
    CHECK(!cerberus_recon_journal_peek(&j)&&!cerberus_recon_journal_commit(&j,1));
    for(uint64_t seq=1;seq<=64;++seq) {
        cerberus_recon_record_t r=seq%2?wifi(seq):ble(seq), before=r;
        r.ibeacon=seq%4U==0U;
        r.consumed_us=seq;before=r;
        CHECK(cerberus_recon_journal_offer(&j,&r));
        CHECK(!memcmp(&r,&before,sizeof(r)));
        r.address[5]=0;r.name[0]=255; /* Journal owns a copy. */
        CHECK(j.rows[seq-1].address[5]==0xef && j.rows[seq-1].name[0]==0);
    }
    CHECK(j.count==64&&j.counters.accepted==64);
    CHECK(j.counters.accepted_wifi_rows==32&&j.counters.accepted_ble_rows==32&&
        j.counters.accepted_ibeacon_rows==16&&!j.counters.written_wifi_rows&&
        !j.counters.written_ble_rows&&!j.counters.written_ibeacon_rows);
    cerberus_recon_record_t r=wifi(65);r.consumed_us=65;
    prior=j;CHECK(!cerberus_recon_journal_offer(&j,&r));
    CHECK(j.counters.dropped==1&&j.last_sequence==65&&j.last_consumed_us==65);
    CHECK(j.counters.accepted_wifi_rows==32&&j.counters.accepted_ble_rows==32&&
        j.counters.accepted_ibeacon_rows==16);
    CHECK(!memcmp(j.rows,prior.rows,sizeof(j.rows)));
    CHECK(!cerberus_recon_journal_commit(&j,2)&&j.count==64);
    for(uint64_t seq=1;seq<=16;++seq) {
        const cerberus_recon_record_t *p=cerberus_recon_journal_peek(&j);
        CHECK(p&&p->sequence==seq&&p->address[5]==0xef);
        CHECK(cerberus_recon_journal_commit(&j,seq));
    }
    CHECK(j.count==48&&j.counters.written==16);
    CHECK(j.counters.written_wifi_rows==8&&j.counters.written_ble_rows==8&&
        j.counters.written_ibeacon_rows==4);
    CHECK(!cerberus_recon_journal_offer(&j,&r)); /* Dropped sequence cannot reenter. */
    CHECK(j.counters.invalid==1);
    r.sequence=66;r.consumed_us=64;CHECK(!cerberus_recon_journal_offer(&j,&r));
    r.consumed_us=66;r.session_id=8;CHECK(!cerberus_recon_journal_offer(&j,&r));
    CHECK(!cerberus_recon_journal_offer(&j,NULL));
    for(uint64_t seq=66;seq<=81;++seq) {
        r=ble(seq);r.consumed_us=seq;r.ibeacon=seq%3U==0U;
        CHECK(cerberus_recon_journal_offer(&j,&r));
    }
    CHECK(j.count==64&&j.head==16);
    for(uint64_t seq=17;seq<=64;++seq) {
        CHECK(cerberus_recon_journal_peek(&j)->sequence==seq);
        CHECK(cerberus_recon_journal_commit(&j,seq));
    }
    for(uint64_t seq=66;seq<=81;++seq) {
        CHECK(cerberus_recon_journal_peek(&j)->sequence==seq);
        CHECK(cerberus_recon_journal_commit(&j,seq));
    }
    CHECK(!j.count&&j.counters.written==80&&j.counters.accepted==80);
    CHECK(j.counters.accepted_wifi_rows==32&&j.counters.written_wifi_rows==32&&
        j.counters.accepted_ble_rows==48&&j.counters.written_ble_rows==48&&
        j.counters.accepted_ibeacon_rows==22&&j.counters.written_ibeacon_rows==22);
    CHECK(!cerberus_recon_journal_commit(&j,81));
    r=wifi(82);r.consumed_us=99;CHECK(cerberus_recon_journal_offer(&j,&r));
    r.sequence=83;CHECK(cerberus_recon_journal_offer(&j,&r)); /* Same time permitted. */
    cerberus_recon_journal_abandon(&j);
    CHECK(!j.count&&!j.head&&j.counters.abandoned==2&&j.counters.written==80);
    CHECK(j.counters.accepted_wifi_rows==34&&j.counters.written_wifi_rows==32&&
        j.counters.accepted_ble_rows==48&&j.counters.written_ble_rows==48);
    uint8_t zero[sizeof(j.rows)]={0};CHECK(!memcmp(j.rows,zero,sizeof(zero)));
    cerberus_recon_journal_abandon(&j);CHECK(j.counters.abandoned==2);
    CHECK(cerberus_recon_journal_init(&j,10,500));
    cerberus_recon_counters_t zero_counters={0};
    CHECK(!memcmp(&j.counters,&zero_counters,sizeof(zero_counters)));
    r=wifi(1);r.session_id=10;r.consumed_us=499;
    CHECK(!cerberus_recon_journal_offer(&j,&r));
    r.consumed_us=500;CHECK(cerberus_recon_journal_offer(&j,&r));
    r.sequence=UINT64_MAX;CHECK(cerberus_recon_journal_offer(&j,&r));
    r.sequence=1;CHECK(!cerberus_recon_journal_offer(&j,&r));
    r.sequence=UINT64_MAX;CHECK(!cerberus_recon_journal_offer(&j,&r));
    CHECK(j.count==2);
    CHECK(cerberus_recon_journal_init(&j,9,0));
    r=wifi(1);r.consumed_us=0;CHECK(cerberus_recon_journal_offer(&j,&r));
    CHECK(cerberus_recon_journal_peek(&j)->consumed_us==0);
    r.sequence=2;r.consumed_us=UINT64_C(4294967296)+100;
    CHECK(cerberus_recon_journal_offer(&j,&r));
    r.sequence=3;r.consumed_us=100;
    CHECK(!cerberus_recon_journal_offer(&j,&r)); /* No low32 clock resurrection. */
    CHECK(!cerberus_recon_journal_offer(NULL,&r));
    CHECK(!cerberus_recon_journal_peek(NULL));
    CHECK(!cerberus_recon_journal_commit(NULL,1));
    cerberus_recon_journal_abandon(NULL);
    j.count=65;CHECK(!cerberus_recon_journal_peek(&j));
    CHECK(!cerberus_recon_journal_offer(&j,&r));
}

static void saturation(void)
{
    cerberus_recon_journal_t j;CHECK(cerberus_recon_journal_init(&j,9,0));
    j.counters.accepted=UINT32_MAX;
    cerberus_recon_record_t r=wifi(1);
    CHECK(cerberus_recon_journal_offer(&j,&r)&&j.counters.accepted==UINT32_MAX);
    j.counters.written=UINT32_MAX;
    CHECK(cerberus_recon_journal_commit(&j,1)&&j.counters.written==UINT32_MAX);
    j.counters.invalid=UINT32_MAX;
    CHECK(!cerberus_recon_journal_offer(&j,&r)&&j.counters.invalid==UINT32_MAX);
    for(uint64_t seq=2;seq<=65;++seq) {r.sequence=seq;CHECK(cerberus_recon_journal_offer(&j,&r));}
    j.counters.dropped=UINT32_MAX;r.sequence=66;
    CHECK(!cerberus_recon_journal_offer(&j,&r)&&j.counters.dropped==UINT32_MAX);
    j.counters.abandoned=UINT32_MAX-1;
    cerberus_recon_journal_abandon(&j);
    CHECK(j.counters.abandoned==UINT32_MAX&&!j.count);
    CHECK(cerberus_recon_journal_init(&j,9,0));
    j.counters.accepted=j.counters.written=UINT32_MAX;
    j.counters.accepted_wifi_rows=j.counters.written_wifi_rows=UINT32_MAX-1U;
    j.counters.accepted_ble_rows=j.counters.written_ble_rows=UINT32_MAX-1U;
    j.counters.accepted_ibeacon_rows=j.counters.written_ibeacon_rows=UINT32_MAX-1U;
    for(uint64_t seq=1;seq<=4;++seq) {
        r=seq%2?wifi(seq):ble(seq);r.ibeacon=r.radio==CERBERUS_RECON_RADIO_BLE;
        CHECK(cerberus_recon_journal_offer(&j,&r));
        CHECK(cerberus_recon_journal_commit(&j,seq));
    }
    CHECK(j.counters.accepted==UINT32_MAX&&j.counters.written==UINT32_MAX&&
        j.counters.accepted_wifi_rows==UINT32_MAX&&j.counters.written_wifi_rows==UINT32_MAX&&
        j.counters.accepted_ble_rows==UINT32_MAX&&j.counters.written_ble_rows==UINT32_MAX&&
        j.counters.accepted_ibeacon_rows==UINT32_MAX&&j.counters.written_ibeacon_rows==UINT32_MAX);
}

static void output_bounds(void)
{
    cerberus_recon_record_t r=ble(UINT64_MAX);
    r.session_id=UINT64_MAX;r.consumed_us=UINT64_MAX;
    r.source_kind=CERBERUS_RECON_SOURCE_S3_CALLBACK_US;
    r.source_flags=CERBERUS_RECON_SOURCE_TIME_VALID|CERBERUS_RECON_SOURCE_SEQUENCE_VALID;
    r.source_time=UINT64_MAX;r.source_sequence=UINT32_MAX;
    r.source_limits=31;r.name_length=32;memset(r.name,255,32);
    r.classified=true;r.category=SENTINEL_WATCH_BEACON;r.confidence=3;r.mode=2;
    r.alert_state=CERBERUS_RECON_ALERT_QUEUED;
    memset(r.rule,'a',31);r.rule[31]=0;
    char expected[CERBERUS_RECON_CSV_CAPACITY];
    size_t length=cerberus_recon_row_format(&r,expected,sizeof(expected));
    CHECK(length>0&&length<sizeof(expected)&&strlen(expected)==length);
    CHECK(!strcmp(expected+length-2,"\r\n"));
    for(size_t cap=0;cap<=length+2;++cap) {
        char out[CERBERUS_RECON_CSV_CAPACITY+4];memset(out,0xa5,sizeof(out));
        size_t actual=cerberus_recon_row_format(&r,out+1,cap);
        CHECK((unsigned char)out[0]==0xa5);
        if(cap<=length) {
            CHECK(!actual);
            if(cap) CHECK(out[1]==0);
            size_t start=cap?2:1;
            for(size_t i=start;i<sizeof(out);++i)CHECK((unsigned char)out[i]==0xa5);
        } else {
            CHECK(actual==length&&!memcmp(out+1,expected,length+1));
            for(size_t i=length+2;i<sizeof(out);++i)CHECK((unsigned char)out[i]==0xa5);
        }
    }
    CHECK(!cerberus_recon_row_format(&r,NULL,100));
    expected[0]='x';CHECK(!cerberus_recon_row_format(NULL,expected,sizeof(expected))&&!expected[0]);
    r.name_length=33;expected[0]='x';
    CHECK(!cerberus_recon_row_format(&r,expected,sizeof(expected))&&!expected[0]);
    CHECK(strlen(cerberus_recon_csv_header())<CERBERUS_RECON_CSV_CAPACITY);
}

static cerberus_recon_summary_t summary(void)
{
    cerberus_recon_summary_t s={.session_id=9,.started_us=0,.ended_us=100,
        .last_sequence=258,.counters={.accepted=258,.written=258,
            .accepted_wifi_rows=1,.accepted_ble_rows=257,.accepted_ibeacon_rows=1,
            .written_wifi_rows=1,.written_ble_rows=257,.written_ibeacon_rows=1},
        .state=CERBERUS_RECON_MANIFEST_COMPLETE};
    strcpy(s.session_name,"R00001");return s;
}

static void manifest_contract(void)
{
    char expected[CERBERUS_RECON_MANIFEST_CAPACITY],out[CERBERUS_RECON_MANIFEST_CAPACITY+4];
    cerberus_recon_summary_t s=summary();
    size_t length=cerberus_recon_manifest_format(&s,expected,sizeof(expected));
    CHECK(length&&length<sizeof(expected)&&strlen(expected)==length);
    for(size_t cap=0;cap<=length+1;++cap) {
        memset(out,0xa5,sizeof(out));
        size_t actual=cerberus_recon_manifest_format(&s,out+1,cap);
        CHECK((unsigned char)out[0]==0xa5);
        if(cap<=length) {
            CHECK(!actual);if(cap)CHECK(out[1]==0);
            for(size_t i=cap?2:1;i<sizeof(out);++i)CHECK((unsigned char)out[i]==0xa5);
        } else CHECK(actual==length&&!memcmp(out+1,expected,length+1));
    }
#define BAD(change) do{s=summary();change;out[0]='x';CHECK(!cerberus_recon_manifest_format(&s,out,sizeof(out))&&!out[0]);}while(0)
    BAD(s.session_id=0);BAD(s.session_name[0]='x');BAD(s.session_name[1]='/');
    BAD(s.session_name[5]='0');BAD(s.session_name[6]='x');BAD(s.state=(cerberus_recon_manifest_state_t)-1);
    BAD(s.state=(cerberus_recon_manifest_state_t)3);BAD(s.queued=65);BAD(s.queued=1);
    BAD(s.error_code=1);BAD(s.started_us=101);BAD(s.counters.written=257);
    BAD(s.counters.written=257;s.counters.abandoned=1);
    BAD(s.last_sequence=0);BAD(s.state=CERBERUS_RECON_MANIFEST_ERROR);
    BAD(s.state=CERBERUS_RECON_MANIFEST_ACTIVE);
    BAD(s.counters.accepted_wifi_rows=2);BAD(s.counters.accepted_ble_rows=256);
    BAD(s.counters.written_wifi_rows=0);BAD(s.counters.written_ble_rows=258);
    BAD(s.counters.accepted_ibeacon_rows=258);BAD(s.counters.written_ibeacon_rows=2);
    BAD(s.counters.accepted_ibeacon_rows=2); /* COMPLETE cannot leave typed rows unwritten. */
    BAD(s.counters.accepted=UINT32_MAX); /* Saturation cannot hide impossible radio sums. */
    BAD(s.counters.invalid=UINT32_MAX;s.counters.written=257);
    BAD(s.counters.dropped=UINT32_MAX;s.counters.accepted_ble_rows=256);
#undef BAD
    s=summary();s.state=CERBERUS_RECON_MANIFEST_ACTIVE;s.ended_us=0;
    CHECK(cerberus_recon_manifest_format(&s,out,sizeof(out))&&strstr(out,"\"ended_s3_us\":null"));
    s=summary();s.state=CERBERUS_RECON_MANIFEST_ERROR;s.error_code=-1;
    CHECK(cerberus_recon_manifest_format(&s,out,sizeof(out))&&strstr(out,"\"error_code\":-1"));
    s=summary();s.counters.accepted=s.counters.written=UINT32_MAX;
    s.counters.accepted_wifi_rows=s.counters.written_wifi_rows=UINT32_MAX;
    CHECK(cerberus_recon_manifest_format(&s,out,sizeof(out))&&strstr(out,"\"counter_lower_bounds\":true"));
    s.state=CERBERUS_RECON_MANIFEST_ERROR;s.error_code=-1;
    s.counters.accepted_wifi_rows=UINT32_MAX-5U;s.counters.written_wifi_rows=UINT32_MAX-6U;
    s.counters.accepted_ble_rows=s.counters.written_ble_rows=10;s.counters.abandoned=1;
    CHECK(cerberus_recon_manifest_format(&s,out,sizeof(out))); /* Both total sums saturate. */
    s.counters.accepted_ibeacon_rows=2;
    CHECK(!cerberus_recon_manifest_format(&s,out,sizeof(out))); /* No unwritten BLE rows. */
    s.counters.accepted_ibeacon_rows=1;s.counters.written_ble_rows=11;
    CHECK(!cerberus_recon_manifest_format(&s,out,sizeof(out))); /* Saturated totals do not hide kind mismatch. */
    s=summary();s.state=CERBERUS_RECON_MANIFEST_ERROR;s.error_code=-1;
    s.counters.accepted=s.counters.written=UINT32_MAX;
    s.counters.accepted_ble_rows=s.counters.written_ble_rows=UINT32_MAX;
    s.counters.written_wifi_rows=0;
    CHECK(!cerberus_recon_manifest_format(&s,out,sizeof(out))); /* One known unwritten Wi-Fi row. */
    s.counters.abandoned=1;CHECK(cerberus_recon_manifest_format(&s,out,sizeof(out)));
    s.counters.accepted_ibeacon_rows=3;s.counters.written_ibeacon_rows=1;
    CHECK(!cerberus_recon_manifest_format(&s,out,sizeof(out))); /* Plus two known unwritten iBeacon rows. */
    s.counters.abandoned=3;CHECK(cerberus_recon_manifest_format(&s,out,sizeof(out)));
    s=summary();s.session_id=UINT64_MAX;s.started_us=UINT64_MAX;s.ended_us=UINT64_MAX;
    s.last_sequence=UINT64_MAX;memset(&s.counters,255,sizeof(s.counters));strcpy(s.session_name,"R99999");
    s.state=CERBERUS_RECON_MANIFEST_ERROR;s.error_code=INT32_MIN;
    CHECK(cerberus_recon_manifest_format(&s,out,CERBERUS_RECON_MANIFEST_CAPACITY));
    s=summary();memset(&s.counters,0,sizeof(s.counters));s.last_sequence=0;s.ended_us=0;
    CHECK(cerberus_recon_manifest_format(&s,out,sizeof(out))&&strstr(out,"\"written\":0"));
    CHECK(!cerberus_recon_manifest_format(NULL,out,sizeof(out)));
    CHECK(!cerberus_recon_manifest_format(&s,NULL,100));
    /* Exhaust all small typed subsets independently of queue implementation. */
    for(uint32_t aw=0;aw<4;++aw)for(uint32_t ab=0;ab<4;++ab)
    for(uint32_t ai=0;ai<4;++ai)for(uint32_t ww=0;ww<4;++ww)
    for(uint32_t wb=0;wb<4;++wb)for(uint32_t wi=0;wi<4;++wi) {
        s=summary();s.state=CERBERUS_RECON_MANIFEST_ERROR;s.error_code=-1;
        s.counters=(cerberus_recon_counters_t){.accepted=aw+ab,.written=ww+wb,
            .accepted_wifi_rows=aw,.accepted_ble_rows=ab,.accepted_ibeacon_rows=ai,
            .written_wifi_rows=ww,.written_ble_rows=wb,.written_ibeacon_rows=wi};
        s.counters.abandoned=ww+wb<=aw+ab?aw+ab-ww-wb:0;
        bool valid=ww<=aw&&wb<=ab&&ai<=ab&&wi<=wb&&wi<=ai&&
            ai-wi<=ab-wb;
        CHECK((cerberus_recon_manifest_format(&s,out,sizeof(out))!=0)==valid);
    }
}

static void write_exports(const char *directory)
{
    char path[1024];CHECK(snprintf(path,sizeof(path),"%s/OBS.CSV",directory)<(int)sizeof(path));
    FILE *f=fopen(path,"wb");CHECK(f);
    const char *header=cerberus_recon_csv_header();CHECK(fwrite(header,1,strlen(header),f)==strlen(header));
    cerberus_recon_journal_t journal;CHECK(cerberus_recon_journal_init(&journal,9,0));
    char out[CERBERUS_RECON_CSV_CAPACITY];
    for(unsigned i=0;i<256;++i) {
        cerberus_recon_record_t r=ble(i+1U);r.name_length=32;memset(r.name,(int)i,32);
        size_t length=cerberus_recon_row_format(&r,out,sizeof(out));CHECK(length);
        CHECK(fwrite(out,1,length,f)==length);
        CHECK(cerberus_recon_journal_offer(&journal,&r));
        CHECK(cerberus_recon_journal_commit(&journal,r.sequence));
    }
    cerberus_recon_record_t r=wifi(257);
    const uint8_t unsafe[]={'=',',','"','\\',0,'\n',255};
    memcpy(r.name,unsafe,sizeof(unsafe));r.name_length=sizeof(unsafe);
    r.source_kind=CERBERUS_RECON_SOURCE_C5_AGE_MS;r.source_time=17;r.source_anchor_us=80;
    r.source_flags=CERBERUS_RECON_SOURCE_TIME_VALID|CERBERUS_RECON_SOURCE_ANCHOR_VALID;
    size_t length=cerberus_recon_row_format(&r,out,sizeof(out));CHECK(length);
    CHECK(fwrite(out,1,length,f)==length);
    CHECK(cerberus_recon_journal_offer(&journal,&r));
    CHECK(cerberus_recon_journal_commit(&journal,r.sequence));
    r=ble(258);r.rssi=127;r.rssi_valid=false;r.ibeacon=true;
    r.source_kind=CERBERUS_RECON_SOURCE_S3_ADV_MS32;r.source_sequence=1;
    r.source_flags=CERBERUS_RECON_SOURCE_TIME_VALID|CERBERUS_RECON_SOURCE_SEQUENCE_VALID;
    r.classified=true;r.category=SENTINEL_WATCH_BEACON;r.confidence=3;r.mode=1;
    r.alert_state=CERBERUS_RECON_ALERT_NOT_QUEUED;strcpy(r.rule,"beacon.ibeacon");
    length=cerberus_recon_row_format(&r,out,sizeof(out));CHECK(length);
    CHECK(fwrite(out,1,length,f)==length);CHECK(!fclose(f));
    CHECK(cerberus_recon_journal_offer(&journal,&r));
    CHECK(cerberus_recon_journal_commit(&journal,r.sequence));
    CHECK(snprintf(path,sizeof(path),"%s/SESSION.JSON",directory)<(int)sizeof(path));
    f=fopen(path,"wb");CHECK(f);
    cerberus_recon_summary_t s=summary();char manifest[CERBERUS_RECON_MANIFEST_CAPACITY];
    s.counters=journal.counters;
    length=cerberus_recon_manifest_format(&s,manifest,sizeof(manifest));CHECK(length);
    CHECK(fwrite(manifest,1,length,f)==length);CHECK(!fclose(f));
}

static void write_summary(const char *directory,const char *name,const cerberus_recon_summary_t *s)
{
    char path[1024],out[CERBERUS_RECON_MANIFEST_CAPACITY];
    CHECK(snprintf(path,sizeof(path),"%s/%s",directory,name)<(int)sizeof(path));
    size_t length=cerberus_recon_manifest_format(s,out,sizeof(out));CHECK(length);
    FILE *file=fopen(path,"wb");CHECK(file);
    CHECK(fwrite(out,1,length,file)==length&&fclose(file)==0);
}

static void typed_partial_exports(const char *directory)
{
    cerberus_recon_journal_t j;CHECK(cerberus_recon_journal_init(&j,9,0));
    for(uint64_t seq=1;seq<=6;++seq) {
        cerberus_recon_record_t r=seq%3U==1U?wifi(seq):ble(seq);
        r.ibeacon=seq%3U==0U;
        CHECK(cerberus_recon_journal_offer(&j,&r));
    }
    /* A worker that appended the first three rows must commit only those.
     * A later failed/partial append leaves the remaining typed rows unwritten. */
    for(uint64_t seq=1;seq<=3;++seq)CHECK(cerberus_recon_journal_commit(&j,seq));
    cerberus_recon_summary_t s=summary();s.last_sequence=j.last_sequence;
    s.counters=j.counters;s.queued=j.count;s.state=CERBERUS_RECON_MANIFEST_ACTIVE;s.ended_us=0;
    write_summary(directory,"ACTIVE.JSON",&s);
    cerberus_recon_counters_t before=j.counters;
    cerberus_recon_journal_abandon(&j);
    CHECK(j.counters.accepted_wifi_rows==before.accepted_wifi_rows&&
        j.counters.accepted_ble_rows==before.accepted_ble_rows&&
        j.counters.accepted_ibeacon_rows==before.accepted_ibeacon_rows&&
        j.counters.written_wifi_rows==before.written_wifi_rows&&
        j.counters.written_ble_rows==before.written_ble_rows&&
        j.counters.written_ibeacon_rows==before.written_ibeacon_rows);
    s.counters=j.counters;s.queued=j.count;s.state=CERBERUS_RECON_MANIFEST_ERROR;
    s.error_code=-7;s.ended_us=100;write_summary(directory,"ERROR.JSON",&s);
    CHECK(cerberus_recon_journal_init(&j,10,0));
    s=summary();s.session_id=j.session_id;s.counters=j.counters;s.last_sequence=0;
    write_summary(directory,"RESET.JSON",&s);
}

int main(int argc,char **argv)
{
    CHECK(argc==2);
    validation();queue_contract();saturation();output_bounds();manifest_contract();
    write_exports(argv[1]);typed_partial_exports(argv[1]);
    printf("Recon journal: %u checks passed; record=%zu queue=%zu bytes on host.\n",
        checks,sizeof(cerberus_recon_record_t),sizeof(cerberus_recon_journal_t));
    return 0;
}
