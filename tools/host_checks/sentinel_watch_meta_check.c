#ifdef NDEBUG
#undef NDEBUG
#endif
#include "sentinel_watch_meta.h"
#include <assert.h>
#include <stdio.h>

static unsigned checks;
#define CHECK(x) do { ++checks; assert(x); } while (0)
static cerberus_s3_ble_snapshot_t snapshot;
static const uint64_t lease = UINT64_C(0x100000002);
static const uint8_t ordinary[] = {2,1,6};
static const uint8_t allowed[] = {3,3,0x5f,0xfd};
static const uint8_t blocked[] = {3,3,0x4c,0};

typedef struct {
    cerberus_s3_ble_focus_t focus;
    cerberus_c5_ble_advertisement_t raw;
    cerberus_c5_ble_result_t peer;
    uint32_t observed;
    uint8_t generation;
} frozen_t;

static cerberus_s3_ble_report_t report(const uint8_t *bytes, size_t length)
{
    return (cerberus_s3_ble_report_t){.address={1,2,3,4,5,6}, .address_type=1,
        .rssi=-55, .event_type=0, .complete=true, .data=bytes, .length=length};
}

static void begin(uint8_t generation, uint32_t now)
{ cerberus_s3_ble_snapshot_begin_with_epoch(&snapshot,generation,now,true,lease); }

/* Copy the actual producer's value fields, without recreating its parser or
 * classification. The separate UI fixture exercises its native lease fence. */
static frozen_t freeze(void)
{
    const cerberus_s3_ble_record_t *r = &snapshot.records[0];
    frozen_t f = {.focus=r->focus, .observed=r->last_advertisement_ms,
        .generation=snapshot.status.generation};
    memcpy(f.peer.address,r->address,6); f.peer.address_type=r->address_type;
    f.peer.rssi=r->rssi; memcpy(f.peer.name,r->name,r->name_length);
    memcpy(f.raw.address,r->address,6); f.raw.address_type=r->address_type;
    f.raw.length=r->advertisement_length; f.raw.event_type=r->advertisement_event_type;
    f.raw.flags=r->advertisement_flags; memcpy(f.raw.data,r->advertisement,f.raw.length);
    return f;
}

static bool project(const frozen_t *f, uint64_t captured, uint64_t accepted,
    sentinel_watch_match_t *match)
{
    return sentinel_watch_project_meta(&f->focus,&f->raw,&f->peer,f->generation,
        f->observed,captured,accepted,match);
}

static void expected(const frozen_t *f, bool qualifies)
{
    sentinel_watch_match_t m={0};
    CHECK(project(f,lease,lease,&m)==qualifies);
    if(qualifies) {
        CHECK(m.category==SENTINEL_WATCH_SMART_GLASSES);
        CHECK(m.confidence==SENTINEL_WATCH_CONFIDENCE_LOW);
        CHECK(!strcmp(m.rule_id,"meta.original-id-hint"));
        CHECK(!strcmp(m.label,"Meta identifier hint"));
    } else {
        sentinel_watch_match_t empty={0}; CHECK(!memcmp(&m,&empty,sizeof(m)));
    }
}

static size_t pair(uint8_t *out,uint16_t id,unsigned context)
{
    static const uint8_t types[]={0xff,3,5,7,0x16,0x20,0x21};
    static const uint8_t widths[]={2,2,4,16,2,4,16};
    static const uint8_t base[]={0xfb,0x34,0x9b,0x5f,0x80,0,0,0x80,0,0x10,0,0};
    unsigned width=widths[context]; memset(out,0,18);
    out[0]=(uint8_t)(width+1U);out[1]=types[context];
    unsigned position=2;
    if(width==16){memcpy(out+2,base,sizeof(base));position=14;}
    out[position]=(uint8_t)id;out[position+1]=(uint8_t)(id>>8);
    return width+2U;
}

static void actual_source_cases(void)
{
    uint8_t bytes[255];
    for(unsigned id=0;id<CERBERUS_BLE_META_ID_COUNT;++id)
        for(unsigned context=0;context<CERBERUS_BLE_META_CONTEXT_COUNT;++context){
            size_t n=pair(bytes,cerberus_ble_meta_id_value(id),context);
            begin(3,100);cerberus_s3_ble_report_t r=report(bytes,n);
            cerberus_s3_ble_snapshot_observe(&snapshot,&r,110);
            frozen_t f=freeze();expected(&f,id<CERBERUS_BLE_META_ALLOWED_ID_COUNT);
            CHECK(cerberus_ble_meta_evidence_has(&f.focus.discovery.meta_advertisement,
                id,(cerberus_ble_meta_context_t)context));
        }
    /* Full original evidence survives a cut raw preview without parsing that
     * prefix again. The hint is beyond byte31 in a complete255-byte report. */
    memset(bytes,0,sizeof(bytes));bytes[0]=39;bytes[1]=0x30;
    memcpy(bytes+40,allowed,sizeof(allowed));
    begin(0,0);cerberus_s3_ble_report_t r=report(bytes,sizeof(bytes));
    r.sid_valid=true;r.sid=15;
    cerberus_s3_ble_snapshot_observe(&snapshot,&r,0);
    frozen_t f=freeze();CHECK(f.raw.length==31);CHECK(f.raw.flags==CERBERUS_BLE_ADVERTISEMENT_TRUNCATED);
    expected(&f,true);
    frozen_t retained=f;
    r.data=ordinary;r.length=sizeof(ordinary);
    cerberus_s3_ble_snapshot_observe(&snapshot,&r,1);
    f=freeze();expected(&f,false);expected(&retained,true);
    /* New partial and malformed originals cannot leave a prior hint current. */
    for(unsigned kind=0;kind<2;++kind){
        begin(9,10);r=report(allowed,sizeof(allowed));
        cerberus_s3_ble_snapshot_observe(&snapshot,&r,11);
        r.complete=kind!=0;uint8_t malformed[]={3,3,0x5f,0xfd,7,3};
        r.data=malformed;r.length=sizeof(malformed);
        cerberus_s3_ble_snapshot_observe(&snapshot,&r,12);
        f=freeze();expected(&f,false);
    }
    /* A response-only Meta match is legitimate Discovery evidence, but is
     * deliberately not promoted by this original-only Recon adapter. */
    begin(10,20);r=report(ordinary,sizeof(ordinary));
    cerberus_s3_ble_snapshot_observe(&snapshot,&r,21);
    r.scan_response=true;r.event_type=4;r.data=allowed;r.length=sizeof(allowed);
    cerberus_s3_ble_snapshot_observe(&snapshot,&r,22);
    f=freeze();CHECK(f.focus.category_bits&CERBERUS_S3_BLE_FOCUS_META_HINT);expected(&f,false);
    /* Valid blocked response suppresses even a later qualifying original in
     * this sweep. No clearing the producer's deliberate sticky suppression. */
    begin(11,30);r=report(allowed,sizeof(allowed));
    cerberus_s3_ble_snapshot_observe(&snapshot,&r,31);
    r.scan_response=true;r.event_type=4;r.data=blocked;r.length=sizeof(blocked);
    cerberus_s3_ble_snapshot_observe(&snapshot,&r,32);
    r=report(allowed,sizeof(allowed));cerberus_s3_ble_snapshot_observe(&snapshot,&r,33);
    f=freeze();CHECK(f.focus.meta_suppressed);expected(&f,false);
    /* A complete name/aggregate projection alone cannot manufacture proof. */
    begin(12,40);r=report(ordinary,sizeof(ordinary));
    cerberus_s3_ble_snapshot_observe(&snapshot,&r,41);f=freeze();
    strcpy(f.peer.name,"Ray-Ban Meta");f.peer.manufacturer_id=0x058e;expected(&f,false);
}

static void binding_and_precedence(void)
{
    begin(255,UINT32_MAX-10);cerberus_s3_ble_report_t r=report(allowed,sizeof(allowed));
    cerberus_s3_ble_snapshot_observe(&snapshot,&r,UINT32_MAX-1);
    frozen_t good=freeze();expected(&good,true);
    for(unsigned fault=0;fault<27;++fault){
        frozen_t f=good;
        switch(fault){
        case 0:f.generation++;break;
        case 1:f.peer.address_type=4;break;
        case 2:f.peer.address[0]^=1;break;
        case 3:f.raw.address_type^=1;break;
        case 4:f.raw.address[0]^=1;break;
        case 5:f.raw.length=32;break;
        case 6:f.raw.event_type=4;break;
        case 7:f.raw.flags=CERBERUS_BLE_ADVERTISEMENT_MALFORMED;break;
        case 8:f.raw.flags=CERBERUS_BLE_ADVERTISEMENT_TRUNCATED;break;
        case 9:f.focus.discovery.flags&=~CERBERUS_S3_BLE_EVIDENCE_ADV_COMPLETE;break;
        case 10:f.focus.discovery.flags&=~CERBERUS_S3_BLE_EVIDENCE_ADV_VALID;break;
        case 11:f.focus.discovery.flags|=CERBERUS_S3_BLE_EVIDENCE_SID_INVALID;break;
        case 12:f.focus.discovery.flags|=CERBERUS_S3_BLE_EVIDENCE_SID_VALID;f.focus.discovery.sid=16;break;
        case 13:f.focus.discovery.generation++;break;
        case 14:f.focus.discovery.address_type^=1;break;
        case 15:f.focus.discovery.address[0]^=1;break;
        case 16:f.focus.discovery.advertisement_sequence=0;break;
        case 17:f.focus.discovery.raw_advertisement_sequence++;break;
        case 18:f.observed++;break;
        case 19:f.focus.discovery.flags&=~CERBERUS_S3_BLE_EVIDENCE_RAW_COMPLETE;break;
        case 20:f.focus.category_bits=0;break;
        case 21:f.focus.meta_suppressed=true;break;
        case 22:f.focus.discovery.meta_advertisement.pairs[9]|=0x80;break;
        case 23:memset(&f.focus.discovery.meta_advertisement,0,sizeof(f.focus.discovery.meta_advertisement));break;
        case 24:cerberus_ble_meta_evidence_add(&f.focus.discovery.meta_advertisement,0x004c,CERBERUS_BLE_META_MFG16);break;
        case 25:f.focus.discovery.flags|=0x80;break;
        case 26:f.focus.discovery.scan_response_sequence=99;break;
        }
        expected(&f,false);
    }
    sentinel_watch_match_t m={0};
    CHECK(!project(&good,0,0,&m));CHECK(!project(&good,lease,lease+(UINT64_C(1)<<32),&m));
    CHECK(!project(&good,lease,0,&m));CHECK(!project(&good,lease-1,lease,&m));
    CHECK(!sentinel_watch_project_meta(NULL,&good.raw,&good.peer,good.generation,good.observed,lease,lease,&m));
    CHECK(!sentinel_watch_project_meta(&good.focus,NULL,&good.peer,good.generation,good.observed,lease,lease,&m));
    CHECK(!sentinel_watch_project_meta(&good.focus,&good.raw,NULL,good.generation,good.observed,lease,lease,&m));
    CHECK(!project(&good,lease,lease,NULL));
    for(unsigned category=1;category<SENTINEL_WATCH_CATEGORY_COUNT;++category)
        for(unsigned confidence=1;confidence<=SENTINEL_WATCH_CONFIDENCE_HIGH;++confidence){
            m=(sentinel_watch_match_t){(sentinel_watch_category_t)category,
                (sentinel_watch_confidence_t)confidence,"existing","Existing Watch match","Keep evidence"};
            sentinel_watch_match_t before=m;
            CHECK(!project(&good,lease,lease,&m));CHECK(!memcmp(&before,&m,sizeof(m)));
        }
    /* Actual Watch Axon rule wins even when the same original also carries
     * valid Meta evidence. No override of that independently classified row. */
    const uint8_t both[]={5,3,0x5f,0xfd,0x81,0xfc};
    begin(7,1);r=report(both,sizeof(both));cerberus_s3_ble_snapshot_observe(&snapshot,&r,2);
    good=freeze();sentinel_watch_ble_observation_t o={.advertisement=both,.advertisement_length=sizeof(both)};
    CHECK(sentinel_watch_classify_ble(&o,&m));CHECK(m.category==SENTINEL_WATCH_AXON);
    sentinel_watch_match_t before=m;CHECK(!project(&good,lease,lease,&m));CHECK(!memcmp(&before,&m,sizeof(m)));
    CHECK(!strcmp(sentinel_watch_category_name(SENTINEL_WATCH_SMART_GLASSES),"Meta identifier hint"));
}

int main(void)
{
    actual_source_cases();binding_and_precedence();
    printf("Recon Meta projection: %u checks passed; actual snapshot evidence, original-only binding, suppression, invalidation and existing-match precedence.\n",checks);
    return 0;
}
