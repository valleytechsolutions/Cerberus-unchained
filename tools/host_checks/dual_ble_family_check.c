#include "dual_ble_family.h"
#include "s3_ble_snapshot.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>

static unsigned checks;
static const uint8_t ordinary_address[6] = {1,2,3,4,5,6};
static const cerberus_dual_ble_provenance_t proof = {UINT64_C(0x1122334455667788),
    UINT64_C(0x8877665544332211), 0U, 31U};
static const uint8_t apple[] = {30,0xff,0x4c,0,0x12,0x19,
    1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25};
static const uint8_t beacon_value[] = {0x4c,0,2,0x15,
    0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,0x12,0x34,0xab,0xcd,0xc5};
static const uint8_t fmna128[] = {0xfb,0x34,0x9b,0x5f,0x80,0,0,0x80,0,0x10,0,0,0x44,0xfd,0,0};
static const uint8_t dult128[] = {0x85,0x2a,0x9f,0x57,0xc5,0x2a,0xed,0x88,0x26,0xc2,0xf4,0x12,1,0,0x19,0x15};
static const uint8_t accessory[] = {0x6f,0x2e,0x17,0xdf,0x14,0x18,0xe5,0x9f,0xa8,0x46,0x32,0x95,0x38,0xbb,0xcc,0xe8};

static size_t field(uint8_t *out,uint8_t type,const void *value,size_t n)
{
    assert(n<=254U);out[0]=(uint8_t)(n+1U);out[1]=type;
    if(n)memcpy(out+2U,value,n);
    return n+2U;
}

static cerberus_dual_ble_row_t row(const uint8_t *data,size_t length)
{
    assert(length<=31U);
    cerberus_dual_ble_row_t r={0};memcpy(r.address,ordinary_address,6U);
    if(length)memcpy(r.raw,data,length);
    r.raw_length=(uint8_t)length;r.event_type=3U;
    r.receive=(cerberus_dual_ble_receive_metadata_t){
        .source_kind=CERBERUS_DUAL_BLE_RECEIVE_LEGACY,.callback_length=(uint8_t)length,
        .sid=0xffU,.data_status=0xffU,.ad_validation=CERBERUS_DUAL_BLE_AD_VALID};
    return r;
}

static cerberus_dual_ble_family_evidence_t expect(const cerberus_dual_ble_row_t *r,
    const cerberus_dual_ble_provenance_t *p,bool available,uint8_t categories,uint8_t state)
{
    struct {uint32_t before;cerberus_dual_ble_family_evidence_t value;uint32_t after;} guarded;
    memset(&guarded,0xa5,sizeof(guarded));guarded.before=0x12345678U;guarded.after=0x87654321U;
    cerberus_dual_ble_row_t before;if(r)before=*r;
    cerberus_dual_ble_provenance_t old;if(p)old=*p;
    assert(cerberus_dual_ble_family_classify(r,p,&guarded.value)==available);
    assert(guarded.before==0x12345678U&&guarded.after==0x87654321U);
    assert(guarded.value.available==available&&guarded.value.categories==categories&&guarded.value.state==state);
    if(r)assert(!memcmp(r,&before,sizeof(before)));
    if(p)assert(!memcmp(p,&old,sizeof(old)));
    if(!available){
        cerberus_dual_ble_family_evidence_t zero;memset(&zero,0,sizeof(zero));zero.state=state;
        assert(!memcmp(&zero,&guarded.value,sizeof(zero)));
    }
    if(available){
        cerberus_dual_ble_family_wire_t wire;cerberus_dual_ble_family_evidence_t decoded;
        assert(cerberus_dual_ble_family_wire_encode(&guarded.value,1,&wire));
        assert(cerberus_dual_ble_family_wire_decode(&wire,&r->receive,r->raw_length,r->event_type,
            r->raw_flags,r->address,r->address_type,&decoded));
        assert(!memcmp(&decoded,&guarded.value,sizeof(decoded)));
    }
    ++checks;return guarded.value;
}

static cerberus_dual_ble_family_evidence_t valid(const uint8_t *data,size_t n,uint8_t bits)
{
    cerberus_dual_ble_row_t r=row(data,n);
    return expect(&r,&proof,true,bits,CERBERUS_DUAL_BLE_FAMILY_COMPLETE);
}

static void all_families(void)
{
    uint8_t data[255];size_t n;
    cerberus_dual_ble_family_evidence_t e=valid(NULL,0,0);
    assert(e.ibeacon_result==CERBERUS_IBEACON_NONE);
    e=valid(apple,sizeof(apple),CERBERUS_DUAL_BLE_FAMILY_FIND_MY);
    assert(e.findmy.bits==CERBERUS_BLE_FINDMY_HINT_APPLE&&e.meta_blocked);
    assert(cerberus_ble_meta_evidence_has(&e.meta,8,CERBERUS_BLE_META_MFG16));
    const uint8_t fmna16[]={0x44,0xfd},fmna32[]={0x44,0xfd,0,0};
    const uint8_t *uuid[]={fmna16,fmna32,fmna128,dult128};
    const uint8_t widths[]={2,4,16,16},types[]={3,5,7,7};
    for(unsigned i=0;i<4;++i){
        n=field(data,types[i],uuid[i],widths[i]);e=valid(data,n,CERBERUS_DUAL_BLE_FAMILY_FIND_MY);
        assert(e.findmy.bits==(i==3?CERBERUS_BLE_FINDMY_HINT_DULT:CERBERUS_BLE_FINDMY_HINT_FMNA));
        assert(e.findmy.fmna_uuid_widths==(i==3?0U:(1U<<i)));
    }
    n=field(data,0xff,beacon_value,sizeof(beacon_value));e=valid(data,n,CERBERUS_DUAL_BLE_FAMILY_IBEACON);
    assert(e.ibeacon_result==CERBERUS_IBEACON_FOUND&&e.ibeacon.major==0x1234&&e.ibeacon.minor==0xabcd);
    assert(e.ibeacon.measured_power_dbm==-59&&e.meta_blocked);
    for(unsigned i=0;i<16;++i)assert(e.ibeacon.uuid[i]==i);
    for(unsigned code=0;code<4;++code){
        uint8_t service[]={(uint8_t)(0x80+code),0x30};n=field(data,3,service,2);
        if(code==0){valid(data,n,0);n+=field(data+n,9,"Flipper",7);}
        e=valid(data,n,CERBERUS_DUAL_BLE_FAMILY_FLIPPER);
        assert(e.flipper.bits==(1U<<code)&&(e.flipper.flags&CERBERUS_BLE_FLIPPER_UUID16));
    }
    n=field(data,9,"Flipper",7);e=valid(data,n,0);assert(e.flipper.flags==CERBERUS_BLE_FLIPPER_NAME);
    const uint8_t serial_service[]={0x81,0x30};n=field(data,0x16,serial_service,2);valid(data,n,0);
    n+=field(data+n,9,"Flipper A",9);e=valid(data,n,CERBERUS_DUAL_BLE_FAMILY_FLIPPER);
    assert(e.flipper.flags&CERBERUS_BLE_FLIPPER_SERVICE_DATA);
    const char *names[]={"HC-03","HC-05","HC-06"};const uint8_t codes[]={3,5,6};
    for(unsigned i=0;i<3;++i){
        n=field(data,9,names[i],5);e=valid(data,n,CERBERUS_DUAL_BLE_FAMILY_HC_NAME_HINT);assert(e.hc_name==codes[i]);
        n=field(data,8,names[i],5);valid(data,n,0);
    }
    n=field(data,9,"hc-05",5);valid(data,n,0);
    n=field(data,9,"HC-05x",6);valid(data,n,0);
    const uint8_t meta[]={0x5f,0xfd};n=field(data,3,meta,2);e=valid(data,n,CERBERUS_DUAL_BLE_FAMILY_META_HINT);
    assert(!e.meta_blocked&&cerberus_ble_meta_evidence_has(&e.meta,0,CERBERUS_BLE_META_LIST16));
    const uint8_t flock_mfg[]={0xc8,9,0x55};n=field(data,0xff,flock_mfg,3);
    n+=field(data+n,9,"FS Ext Battery",14);e=valid(data,n,CERBERUS_DUAL_BLE_FAMILY_FLOCK);
    assert(cerberus_ble_flock_hints_level(&e.flock)==CERBERUS_BLE_FLOCK_LEVEL_CORROBORATED);
    n=field(data,7,accessory,16);e=valid(data,n,CERBERUS_DUAL_BLE_FAMILY_FLOCK);
    assert(e.flock.accessory_contexts==CERBERUS_BLE_FLOCK_LIST128);
    const uint8_t raven[]={0,0x31,0,0x32};n=field(data,3,raven,4);e=valid(data,n,CERBERUS_DUAL_BLE_FAMILY_FLOCK);
    assert(e.flock.raven_3100_contexts==1&&e.flock.raven_3200_contexts==1);
    n=field(data,0xff,flock_mfg,2);e=valid(data,n,CERBERUS_DUAL_BLE_FAMILY_FLOCK);
    assert(cerberus_ble_flock_hints_level(&e.flock)==CERBERUS_BLE_FLOCK_LEVEL_CLUE);
}

static void source_gates(void)
{
    const uint8_t data[]={3,3,0x44,0xfd};cerberus_dual_ble_row_t base=row(data,sizeof(data)),r;
    expect(NULL,&proof,false,0,CERBERUS_DUAL_BLE_FAMILY_INVALID_SOURCE);
    assert(!cerberus_dual_ble_family_classify(&base,&proof,NULL));
    expect(&base,NULL,false,0,CERBERUS_DUAL_BLE_FAMILY_NO_PROVENANCE);
    for(unsigned field_number=0;field_number<3;++field_number){
        cerberus_dual_ble_provenance_t p=proof;
        if(field_number==0)p.boot_id=0;else if(field_number==1)p.epoch=0;else p.index=32;
        expect(&base,&p,false,0,CERBERUS_DUAL_BLE_FAMILY_NO_PROVENANCE);
    }
    for(unsigned generation=0;generation<256;++generation){
        cerberus_dual_ble_provenance_t p=proof;p.generation=(uint8_t)generation;p.index=(uint8_t)(generation%32);
        expect(&base,&p,true,CERBERUS_DUAL_BLE_FAMILY_FIND_MY,CERBERUS_DUAL_BLE_FAMILY_COMPLETE);
    }
    for(unsigned type=0;type<256;++type){
        r=base;r.address_type=(uint8_t)type;
        expect(&r,&proof,type<=3,type<=3?CERBERUS_DUAL_BLE_FAMILY_FIND_MY:0,
            type<=3?CERBERUS_DUAL_BLE_FAMILY_COMPLETE:CERBERUS_DUAL_BLE_FAMILY_INVALID_SOURCE);
    }
    for(unsigned event=0;event<256;++event){
        r=base;r.event_type=(uint8_t)event;
        expect(&r,&proof,event<=3,event<=3?CERBERUS_DUAL_BLE_FAMILY_FIND_MY:0,
            event<=3?CERBERUS_DUAL_BLE_FAMILY_COMPLETE:CERBERUS_DUAL_BLE_FAMILY_INVALID_SOURCE);
    }
    r=base;memset(&r.receive,0,sizeof(r.receive));expect(&r,&proof,false,0,CERBERUS_DUAL_BLE_FAMILY_RECEIVE_UNKNOWN);
    r.receive.sid=1;expect(&r,&proof,false,0,CERBERUS_DUAL_BLE_FAMILY_INVALID_SOURCE);
    r=base;r.raw_flags=CERBERUS_BLE_ADVERTISEMENT_MALFORMED;
    expect(&r,&proof,false,0,CERBERUS_DUAL_BLE_FAMILY_MALFORMED);
    r=base;r.receive.ad_validation=CERBERUS_DUAL_BLE_AD_INVALID;
    expect(&r,&proof,false,0,CERBERUS_DUAL_BLE_FAMILY_MALFORMED);
    r=base;r.receive.ad_validation=CERBERUS_DUAL_BLE_AD_UNKNOWN;
    expect(&r,&proof,false,0,CERBERUS_DUAL_BLE_FAMILY_INVALID_SOURCE);
    r=base;r.raw_length=32;expect(&r,&proof,false,0,CERBERUS_DUAL_BLE_FAMILY_INVALID_SOURCE);
    r=base;++r.receive.callback_length;expect(&r,&proof,false,0,CERBERUS_DUAL_BLE_FAMILY_INVALID_SOURCE);
    r=base;r.raw_flags=0x80;expect(&r,&proof,false,0,CERBERUS_DUAL_BLE_FAMILY_INVALID_SOURCE);
    /* Known complete nonlegacy originals: SID unavailable is not a response
     * association failure because this helper makes no such association. */
    r=base;r.receive.source_kind=CERBERUS_DUAL_BLE_RECEIVE_EXTENDED;
    r.receive.sid=0xff;r.receive.data_status=0;r.receive.primary_phy=1;
    for(unsigned sid=0;sid<256;++sid){
        r.receive.sid=(uint8_t)sid;bool valid_sid=sid<=15||sid==255;
        expect(&r,&proof,valid_sid,valid_sid?CERBERUS_DUAL_BLE_FAMILY_FIND_MY:0,
            valid_sid?CERBERUS_DUAL_BLE_FAMILY_COMPLETE:CERBERUS_DUAL_BLE_FAMILY_INVALID_SOURCE);
    }
    r.receive.sid=3;r.receive.properties=0x08;expect(&r,&proof,false,0,CERBERUS_DUAL_BLE_FAMILY_INVALID_SOURCE);
    r.receive.properties=0;r.receive.data_status=1;expect(&r,&proof,false,0,CERBERUS_DUAL_BLE_FAMILY_INVALID_SOURCE);
    r.receive.data_status=2;expect(&r,&proof,false,0,CERBERUS_DUAL_BLE_FAMILY_INVALID_SOURCE);
    r.receive.data_status=0;r.raw_length=31;r.receive.callback_length=32;r.raw_flags=1;
    expect(&r,&proof,false,0,CERBERUS_DUAL_BLE_FAMILY_TRUNCATED);
    r.receive.callback_length=255;r.raw_flags=3;expect(&r,&proof,false,0,CERBERUS_DUAL_BLE_FAMILY_TRUNCATED);
    const uint8_t props[]={0x10,0x12,0x13,0x15},events[]={3,2,0,1};
    for(unsigned i=0;i<4;++i){r=base;r.event_type=events[i];
        r.receive.source_kind=CERBERUS_DUAL_BLE_RECEIVE_EXTENDED;r.receive.properties=props[i];
        r.receive.primary_phy=1;r.receive.data_status=0;
        expect(&r,&proof,true,CERBERUS_DUAL_BLE_FAMILY_FIND_MY,CERBERUS_DUAL_BLE_FAMILY_COMPLETE);
    }
}

static void invalid_suffixes_and_full_reports(void)
{
    uint8_t data[255]={3,3,0x44,0xfd};cerberus_dual_ble_row_t r;
    for(unsigned claimed=1;claimed<256;++claimed){
        data[4]=(uint8_t)claimed;
        r=row(data,5);expect(&r,&proof,false,0,CERBERUS_DUAL_BLE_FAMILY_MALFORMED);
    }
    data[4]=0;
    for(unsigned nonzero=1;nonzero<256;++nonzero){
        data[5]=(uint8_t)nonzero;r=row(data,6);expect(&r,&proof,false,0,CERBERUS_DUAL_BLE_FAMILY_MALFORMED);
    }
    size_t n=field(data,9,"HC-05",5);n+=field(data+n,9,"HC-06",5);
    r=row(data,n);expect(&r,&proof,false,0,CERBERUS_DUAL_BLE_FAMILY_MALFORMED);
    n=field(data,9,"HC-05",5);n+=field(data+n,9,"HC-05",5);valid(data,n,CERBERUS_DUAL_BLE_FAMILY_HC_NAME_HINT);
    n=field(data,3,"x",1);r=row(data,n);expect(&r,&proof,false,0,CERBERUS_DUAL_BLE_FAMILY_MALFORMED);
    for(size_t cut=1;cut<sizeof(apple);++cut){r=row(apple,cut);expect(&r,&proof,false,0,CERBERUS_DUAL_BLE_FAMILY_MALFORMED);}
    memset(data,0,sizeof(data));data[0]=32;data[1]=0x7f; /* Complete unknown32-byte field. */
    n=33;n+=field(data+n,9,"HC-05",5);
    cerberus_dual_ble_family_evidence_t e;
    assert(cerberus_dual_ble_family_parse_ad(data,n,ordinary_address,0,&e));
    assert(e.available&&e.categories==CERBERUS_DUAL_BLE_FAMILY_HC_NAME_HINT);
    r=row(data,31);r.raw_flags=3;r.receive=(cerberus_dual_ble_receive_metadata_t){
        .source_kind=CERBERUS_DUAL_BLE_RECEIVE_EXTENDED,.callback_length=(uint8_t)n,
        .sid=2,.primary_phy=1,.ad_validation=CERBERUS_DUAL_BLE_AD_VALID};
    expect(&r,&proof,false,0,CERBERUS_DUAL_BLE_FAMILY_TRUNCATED);
    memset(data,0,sizeof(data));assert(cerberus_dual_ble_family_parse_ad(data,255,ordinary_address,0,&e));assert(!e.categories);
    assert(!cerberus_dual_ble_family_parse_ad(data,256,ordinary_address,0,&e));
    assert(!cerberus_dual_ble_family_parse_ad(NULL,1,ordinary_address,0,&e));
    /* iBeacon ambiguity is interpretation-local, not invented full-stream corruption. */
    n=field(data,0xff,beacon_value,25);n+=field(data+n,0xff,beacon_value,25);data[n-1]^=1;
    assert(cerberus_dual_ble_family_parse_ad(data,n,ordinary_address,0,&e));
    assert(e.available&&e.ibeacon_result==CERBERUS_IBEACON_MALFORMED&&!(e.categories&CERBERUS_DUAL_BLE_FAMILY_IBEACON));
    cerberus_ibeacon_t zero={0};assert(!memcmp(&e.ibeacon,&zero,sizeof(zero)));
    ++checks;
}

static void source_separation_and_timing(void)
{
    uint8_t data[31];const uint8_t allowed[]={0x5f,0xfd},blocked[]={0x4c,0},fmna[]={0x44,0xfd};
    size_t n=field(data,3,allowed,2);n+=field(data+n,0xff,blocked,2);n+=field(data+n,3,fmna,2);
    cerberus_dual_ble_family_evidence_t e=valid(data,n,CERBERUS_DUAL_BLE_FAMILY_FIND_MY);
    assert(e.meta_blocked&&cerberus_ble_meta_evidence_allowed(&e.meta)&&cerberus_ble_meta_evidence_blocked(&e.meta));
    n=field(data,3,allowed,2);e=valid(data,n,CERBERUS_DUAL_BLE_FAMILY_META_HINT);assert(!e.meta_blocked&&!e.findmy.bits);
    cerberus_dual_ble_row_t r=row(data,n),frozen=r;
    strcpy(r.name,"Flipper HC-05");memcpy(r.uuid,fmna,2);r.uuid_length=2;r.manufacturer_id=0x004c;
    r.rssi=127;r.reports=0xffff;r.timing_flags=0xff;r.callback_age_ms=UINT32_MAX;r.request_started_ms=UINT64_MAX;r.flags=0xff;
    e=expect(&r,&proof,true,CERBERUS_DUAL_BLE_FAMILY_META_HINT,CERBERUS_DUAL_BLE_FAMILY_COMPLETE);
    assert(!e.findmy.bits&&!e.flipper.bits&&!e.hc_name&&!e.meta_blocked);
    /* A live replacement cannot retarget the previously copied row/proof. */
    r=row(apple,sizeof(apple));cerberus_dual_ble_provenance_t next=proof;++next.epoch;++next.boot_id;next.index=0;
    expect(&r,&next,true,CERBERUS_DUAL_BLE_FAMILY_FIND_MY,CERBERUS_DUAL_BLE_FAMILY_COMPLETE);
    expect(&frozen,&proof,true,CERBERUS_DUAL_BLE_FAMILY_META_HINT,CERBERUS_DUAL_BLE_FAMILY_COMPLETE);
    r=row(NULL,0);r.address[5]=0x58;r.address[4]=0x8e;r.address[3]=0x81;
    for(unsigned type=0;type<4;++type){r.address_type=(uint8_t)type;bool oui=type==0||type==2;
        e=expect(&r,&proof,true,oui?CERBERUS_DUAL_BLE_FAMILY_FLOCK:0,CERBERUS_DUAL_BLE_FAMILY_COMPLETE);assert(e.flock_oui==oui);
    }
    r.address_type=0;r.address[5]^=2;expect(&r,&proof,true,0,CERBERUS_DUAL_BLE_FAMILY_COMPLETE);
    r=row(NULL,0);memcpy(r.address,(uint8_t[]){0x58,0x8e,0x81,0,0,0},6);expect(&r,&proof,true,0,CERBERUS_DUAL_BLE_FAMILY_COMPLETE);
    /* Invalid full AD cannot be rescued by an otherwise matching public prefix. */
    r=row((uint8_t[]){4,9,'x'},3);r.address[5]=0x58;r.address[4]=0x8e;r.address[3]=0x81;
    expect(&r,&proof,false,0,CERBERUS_DUAL_BLE_FAMILY_MALFORMED);
}



static void wire_parser_corner_cases(void)
{
    uint8_t data[255],value[16];
    const uint8_t types[]={0xff,3,5,7,0x16,0x20,0x21},widths[]={2,2,4,16,2,4,16};
    for(unsigned id=0;id<CERBERUS_BLE_META_ID_COUNT;++id)for(unsigned context=0;context<7;++context){
        uint16_t identifier=cerberus_ble_meta_id_value(id);memset(value,0,sizeof(value));
        unsigned pos=0;if(widths[context]==16){memcpy(value,fmna128,16);pos=12;}
        value[pos]=(uint8_t)identifier;value[pos+1]=(uint8_t)(identifier>>8);
        size_t n=field(data,types[context],value,widths[context]);
        cerberus_dual_ble_family_evidence_t e=valid(data,n,id<6?CERBERUS_DUAL_BLE_FAMILY_META_HINT:0);
        assert(cerberus_ble_meta_evidence_has(&e.meta,id,(cerberus_ble_meta_context_t)context));
        assert(e.meta_blocked==(id>=6));
    }
    /* Conflicting tuples clear only iBeacon interpretation; an independent
     * Find My UUID from the SAME complete original still roundtrips. */
    size_t n=field(data,0xff,beacon_value,sizeof(beacon_value));
    n+=field(data+n,0xff,beacon_value,sizeof(beacon_value));data[n-1]^=1;
    n+=field(data+n,3,(uint8_t[]){0x44,0xfd},2);
    cerberus_dual_ble_family_evidence_t e,decoded;
    assert(cerberus_dual_ble_family_parse_ad(data,n,ordinary_address,1,&e));
    assert(e.ibeacon_result==CERBERUS_IBEACON_MALFORMED&&e.categories==CERBERUS_DUAL_BLE_FAMILY_FIND_MY);
    cerberus_dual_ble_family_wire_t wire;assert(cerberus_dual_ble_family_wire_encode(&e,2,&wire));
    cerberus_dual_ble_receive_metadata_t receive={.source_kind=2,.callback_length=(uint8_t)n,.sid=255,
        .primary_phy=1,.ad_validation=1};
    assert(cerberus_dual_ble_family_wire_decode(&wire,&receive,31,3,3,ordinary_address,1,&decoded));
    assert(!memcmp(&e,&decoded,sizeof(e)));++checks;
}

static void full_callback_wire(void)
{
    _Static_assert(sizeof(cerberus_dual_ble_family_wire_t)==48,"canonical evidence wire");
    _Static_assert(sizeof(cerberus_dual_ble_snapshot_family_row_t)==158,"opt-in row");
    uint8_t data[255];
    for(uint8_t family=1;family<=6;++family){
        memset(data,0,sizeof(data));
        /* Complete unknown field crosses the preview boundary; each hint is
         * entirely after it. Full255 bytes include valid terminal padding. */
        data[0]=39;data[1]=0x30;size_t n=40;
        if(family==1)n+=field(data+n,0xff,beacon_value,sizeof(beacon_value));
        if(family==2)n+=field(data+n,3,(uint8_t[]){0x44,0xfd},2);
        if(family==3)n+=field(data+n,3,(uint8_t[]){0x81,0x30},2);
        if(family==4)n+=field(data+n,9,"FS Ext Battery",14);
        if(family==5)n+=field(data+n,3,(uint8_t[]){0x5f,0xfd},2);
        if(family==6)n+=field(data+n,9,"HC-05",5);
        (void)n;
        cerberus_dual_ble_family_evidence_t e, decoded;
        assert(cerberus_dual_ble_family_parse_ad(data,255,ordinary_address,1,&e));
        assert(e.available&&(e.categories&(1U<<(family-1U))));
        cerberus_dual_ble_row_t r={0};memcpy(r.address,ordinary_address,6);r.address_type=1;
        r.event_type=3;r.raw_length=31;r.raw_flags=3;memcpy(r.raw,data,31);
        r.receive=(cerberus_dual_ble_receive_metadata_t){.source_kind=2,.callback_length=255,
            .sid=255,.primary_phy=3,.secondary_phy=2,.ad_validation=1};
        assert(cerberus_dual_ble_family_wire_encode(&e,family,&r.family));
        cerberus_dual_ble_row_t frozen=r;
        assert(cerberus_dual_ble_family_wire_decode(&r.family,&r.receive,31,3,3,r.address,1,&decoded));
        assert(!memcmp(&e,&decoded,sizeof(e)));
        expect(&r,&proof,true,e.categories,CERBERUS_DUAL_BLE_FAMILY_COMPLETE);
        if(family==1){assert(r.family.ibeacon_major_le[0]==0x34&&r.family.ibeacon_major_le[1]==0x12);
            assert(r.family.ibeacon_minor_le[0]==0xcd&&r.family.ibeacon_minor_le[1]==0xab&&r.family.ibeacon_power==-59);}
        strcpy(r.name,"unrelated aggregate");r.uuid_length=16;memset(r.uuid,0xff,16);r.manufacturer_id=0xffff;
        r.request_started_ms=UINT64_MAX;r.callback_age_ms=UINT32_MAX;r.timing_flags=0xff;
        expect(&r,&proof,true,e.categories,CERBERUS_DUAL_BLE_FAMILY_COMPLETE);
        for(unsigned fault=0;fault<15;++fault){
            r=frozen;
            if(fault==0)r.family.version=2;
            if(fault==1)r.family.version=0; /* Nonzero remainder must not downgrade. */
            if(fault==2)r.family.family_code=0;
            if(fault==3)r.family.family_code=7;
            if(fault==4)r.family.reserved[0]=1;
            if(fault==5)r.family.reserved[1]=1;
            if(fault==6)r.family.flags|=8;
            if(fault==7)r.family.categories^=0x80;
            if(fault==8)r.family.meta_pairs[9]|=0xe0;
            if(fault==9)r.family.hc_name=4;
            if(fault==10)r.family.ibeacon_result=3;
            if(fault==11)r.receive.ad_validation=2;
            if(fault==12)memset(&r.receive,0,sizeof(r.receive));
            if(fault==13)r.event_type=4;
            if(fault==14)r.address_type=4;
            memset(&decoded,0xa5,sizeof(decoded));
            assert(!cerberus_dual_ble_family_wire_decode(&r.family,&r.receive,r.raw_length,r.event_type,
                r.raw_flags,r.address,r.address_type,&decoded));
            cerberus_dual_ble_family_evidence_t zero={0};assert(!memcmp(&decoded,&zero,sizeof(zero)));
            expect(&r,&proof,false,0,CERBERUS_DUAL_BLE_FAMILY_INVALID_SOURCE);
        }
        r=frozen;memset(&r.family,0,sizeof(r.family));
        expect(&r,&proof,false,0,CERBERUS_DUAL_BLE_FAMILY_TRUNCATED);
        /* Full-stream corruption after the positive field clears every hint. */
        data[254]=1;assert(!cerberus_dual_ble_family_parse_ad(data,255,ordinary_address,1,&e));
        assert(cerberus_dual_ble_family_wire_encode(&e,family,&r.family));r.receive.ad_validation=2;
        assert(cerberus_dual_ble_family_wire_decode(&r.family,&r.receive,31,3,3,r.address,1,&decoded));
        assert(!decoded.available&&decoded.state==CERBERUS_DUAL_BLE_FAMILY_MALFORMED);
        expect(&r,&proof,false,0,CERBERUS_DUAL_BLE_FAMILY_MALFORMED);
        /* Paint/selected evidence does not borrow replacement row or proof. */
        expect(&frozen,&proof,true,(uint8_t)(1U<<(family-1U)),CERBERUS_DUAL_BLE_FAMILY_COMPLETE);
    }
    cerberus_dual_ble_family_evidence_t e={0};cerberus_dual_ble_family_wire_t wire;
    for(unsigned bad=0;bad<4;++bad){memset(&wire,0xa5,sizeof(wire));e.state=0;
        if(bad==1)e.state=CERBERUS_DUAL_BLE_FAMILY_NO_PROVENANCE;
        if(bad==2)e.state=CERBERUS_DUAL_BLE_FAMILY_TRUNCATED;
        if(bad==3)e.state=CERBERUS_DUAL_BLE_FAMILY_INVALID_SOURCE;
        assert(!cerberus_dual_ble_family_wire_encode(&e,1,&wire));
        cerberus_dual_ble_family_wire_t zero={0};assert(!memcmp(&wire,&zero,sizeof(wire)));++checks;}
    e.state=CERBERUS_DUAL_BLE_FAMILY_RECEIVE_UNKNOWN;
    assert(cerberus_dual_ble_family_wire_encode(&e,2,&wire));
    cerberus_dual_ble_receive_metadata_t unknown={0};
    assert(cerberus_dual_ble_family_wire_decode(&wire,&unknown,31,3,3,ordinary_address,1,&e));
    assert(!e.available&&e.state==CERBERUS_DUAL_BLE_FAMILY_RECEIVE_UNKNOWN);
    /* Exact typed OUI consistency is checked even when other hints match. */
    uint8_t address[6]={1,2,3,0x81,0x8e,0x58};
    assert(cerberus_dual_ble_family_parse_ad(NULL,0,address,0,&e)&&e.flock_oui);
    assert(cerberus_dual_ble_family_wire_encode(&e,4,&wire));
    cerberus_dual_ble_receive_metadata_t legacy={.source_kind=1,.sid=255,.data_status=255,.ad_validation=1};
    assert(cerberus_dual_ble_family_wire_decode(&wire,&legacy,0,3,0,address,0,&e));
    assert(!cerberus_dual_ble_family_wire_decode(&wire,&legacy,0,3,0,address,1,&e));
    ++checks;
}

int main(void)
{
    _Static_assert((int)CERBERUS_DUAL_BLE_FAMILY_IBEACON==(int)CERBERUS_S3_BLE_FOCUS_IBEACON,"iBeacon mapping");
    _Static_assert((int)CERBERUS_DUAL_BLE_FAMILY_FIND_MY==(int)CERBERUS_S3_BLE_FOCUS_FIND_MY,"Find My mapping");
    _Static_assert((int)CERBERUS_DUAL_BLE_FAMILY_FLIPPER==(int)CERBERUS_S3_BLE_FOCUS_FLIPPER,"Flipper mapping");
    _Static_assert((int)CERBERUS_DUAL_BLE_FAMILY_FLOCK==(int)CERBERUS_S3_BLE_FOCUS_FLOCK,"Flock mapping");
    _Static_assert((int)CERBERUS_DUAL_BLE_FAMILY_META_HINT==(int)CERBERUS_S3_BLE_FOCUS_META_HINT,"Meta mapping");
    _Static_assert((int)CERBERUS_DUAL_BLE_FAMILY_HC_NAME_HINT==(int)CERBERUS_S3_BLE_FOCUS_HC_NAME_HINT,"HC mapping");
    wire_parser_corner_cases();full_callback_wire();all_families();source_gates();invalid_suffixes_and_full_reports();source_separation_and_timing();
    printf("dual BLE family checks passed: %u cases; evidence=%zu bytes; no RF/transport/clock calls\n",checks,sizeof(cerberus_dual_ble_family_evidence_t));
    return 0;
}
