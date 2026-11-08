#include "dual_ble_archive_capture.h"
#include "ble_archive_response_fixture.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static cerberus_dual_ble_row_t observation(void)
{
    cerberus_dual_ble_row_t row={.address={1,2,3,4,5,6},.address_type=3,.rssi=-93,
        .name="OLD PEER NAME",.uuid={0x44,0xfd},.uuid_length=2,
        .raw={5,0xff,0x06,0x00,0x71,0x72},.raw_length=6,
        .manufacturer_id=0x004c,.reports=65535,.flags=CERBERUS_BLE_RESULT_CONNECTABLE,
        .timing_flags=CERBERUS_DUAL_BLE_SEEN|CERBERUS_DUAL_BLE_RSSI_VALID,
        .callback_age_ms=7000,.request_started_ms=100};
    return row;
}
static const cerberus_dual_ble_provenance_t provenance={
    .boot_id=UINT64_C(0xfedcba9876543210),.epoch=UINT64_C(0x123456789abcdef0),
    .generation=0,.index=31};
static void assert_exact_times_unknown(const cerberus_ble_archive_row_t *row)
{
    assert(!row->source_time_valid && !row->original_source_uptime_ms && !row->copy_ceiling_ms);
    assert(row->response_state==CERBERUS_BLE_ARCHIVE_RESPONSE_UNKNOWN);
    assert(!row->response_length && !row->response_original_length && !row->response_event_type &&
        !row->response_flags && !row->response_sid && !row->response_sid_valid && !row->response_time_valid &&
        !row->response_original_sequence && !row->response_sequence && !row->response_original_observed_ms &&
        !row->response_observed_ms && !row->response_source_uptime_ms);
    for(unsigned i=0;i<sizeof(row->response);++i)assert(!row->response[i]);
}
static void reject_unchanged(const cerberus_dual_ble_row_t *source,
    const cerberus_dual_ble_provenance_t *identity)
{
    cerberus_ble_archive_row_t out;memset(&out,0xa5,sizeof(out));
    const cerberus_ble_archive_row_t sentinel=out;
    assert(!cerberus_dual_ble_archive_capture_row(&out,source,identity,500));
    assert(!memcmp(&out,&sentinel,sizeof(out)));
}

static void receive_roundtrip(const cerberus_dual_ble_row_t *source)
{
    cerberus_ble_archive_t document={.saved_uptime_ms=500,.count=1},decoded;
    assert(cerberus_dual_ble_archive_capture_row(&document.rows[0],source,&provenance,500));
    assert(!memcmp(&document.rows[0].receive,&source->receive,sizeof(source->receive)));
    assert(!memcmp(&document.rows[0].family,&source->family,sizeof(source->family)));
    assert_exact_times_unknown(&document.rows[0]);
    assert(cerberus_ble_archive_validate(&document));
    uint8_t encoded[CERBERUS_BLE_ARCHIVE_MAX_SIZE];size_t length=0;
    assert(cerberus_ble_archive_encode(&document,encoded,sizeof(encoded),&length));
    assert(length==CERBERUS_BLE_ARCHIVE_HEADER_SIZE+CERBERUS_BLE_ARCHIVE_ROW_SIZE);
    assert(cerberus_ble_archive_decode(encoded,length,&decoded));
    const cerberus_ble_archive_row_t *row=&decoded.rows[0];
    assert(!memcmp(&row->receive,&source->receive,sizeof(source->receive)));
    assert(!memcmp(&row->family,&source->family,sizeof(source->family)));
    assert(row->receiver_boot_id==provenance.boot_id&&row->scan_epoch==provenance.epoch&&
        row->generation==provenance.generation&&row->index==provenance.index);
    assert(row->advertisement_length==source->raw_length&&row->advertisement_flags==source->raw_flags&&
        row->advertisement_event_type==source->event_type&&!memcmp(row->advertisement,source->raw,source->raw_length));
    assert(row->age_upper_valid&&row->age_upper_at_save_ms==7400);
    assert_exact_times_unknown(row);
}

static void check_receive_capture(void)
{
    cerberus_dual_ble_row_t source=observation();
    receive_roundtrip(&source); /* Old-peer canonical UNKNOWN. */
    source.receive=(cerberus_dual_ble_receive_metadata_t){
        .source_kind=CERBERUS_DUAL_BLE_RECEIVE_LEGACY,.callback_length=6,.sid=255,
        .data_status=255,.ad_validation=CERBERUS_DUAL_BLE_AD_VALID};
    receive_roundtrip(&source);
    source.raw_flags=CERBERUS_BLE_ADVERTISEMENT_MALFORMED;
    receive_roundtrip(&source); /* Parser failure need not invalidate full layout. */
    source.raw_flags=0;
    source.receive=(cerberus_dual_ble_receive_metadata_t){
        .source_kind=CERBERUS_DUAL_BLE_RECEIVE_EXTENDED,.properties=0x13,.callback_length=6,
        .sid=255,.primary_phy=1,.ad_validation=CERBERUS_DUAL_BLE_AD_VALID};
    receive_roundtrip(&source); /* Legacy PDU delivered via EXT callback. */
    source.receive.properties=1;source.receive.sid=7;source.receive.primary_phy=3;source.receive.secondary_phy=2;
    receive_roundtrip(&source);
    source.raw[0]=20;source.raw_flags=CERBERUS_BLE_ADVERTISEMENT_MALFORMED;
    source.receive.ad_validation=CERBERUS_DUAL_BLE_AD_INVALID;
    receive_roundtrip(&source); /* Malformed full source remains inspectable. */
    source.raw_length=31;source.raw[0]=32;source.raw[1]=0xff;
    source.raw_flags=CERBERUS_BLE_ADVERTISEMENT_MALFORMED|CERBERUS_BLE_ADVERTISEMENT_TRUNCATED;
    source.receive.callback_length=255;source.receive.ad_validation=CERBERUS_DUAL_BLE_AD_VALID;
    receive_roundtrip(&source); /* Valid full source with an incomplete preview. */

    for(unsigned fault=0;fault<20;++fault){
        source=observation();
        source.receive=(cerberus_dual_ble_receive_metadata_t){.source_kind=2,.properties=1,
            .callback_length=6,.sid=7,.primary_phy=3,.secondary_phy=2,.ad_validation=1};
        switch(fault){
        case 0:source.receive.source_kind=3;break;
        case 1:source.receive.properties=0x80;break;
        case 2:source.receive.properties=8;break;
        case 3:source.receive.callback_length=5;break;
        case 4:source.receive.sid=16;break;
        case 5:source.receive.primary_phy=0;break;
        case 6:source.receive.secondary_phy=4;break;
        case 7:source.receive.data_status=1;break;
        case 8:source.receive.ad_validation=0;break;
        case 9:source.receive.ad_validation=3;break;
        case 10:source.receive.callback_length=255;break;
        case 11:source.raw_flags=CERBERUS_BLE_ADVERTISEMENT_TRUNCATED;break;
        case 12:source.event_type=1;break;
        case 13:source.receive.source_kind=0;break; /* Noncanonical UNKNOWN. */
        default:
            source.receive=(cerberus_dual_ble_receive_metadata_t){.source_kind=1,
                .callback_length=6,.sid=255,.data_status=255,.ad_validation=1};
            if(fault==14)source.receive.data_status=0;
            if(fault==15)source.receive.sid=0;
            if(fault==16)source.receive.primary_phy=1;
            if(fault==17)source.receive.properties=1;
            if(fault==18)source.receive.secondary_phy=1;
            if(fault==19){source.receive.callback_length=32;source.raw_length=31;source.raw_flags=1;}
            break;
        }
        reject_unchanged(&source,&provenance);
    }
    /* Saving an earlier selected copy never borrows the live row's newer
     * receive envelope or its later source identity. Actual publication and
     * replacement are exercised by dual_ble_integration_check as well. */
    source=observation();source.receive=(cerberus_dual_ble_receive_metadata_t){
        .source_kind=2,.properties=1,.callback_length=6,.sid=7,.primary_phy=3,.secondary_phy=2,.ad_validation=1};
    const cerberus_dual_ble_row_t selected=source;
    cerberus_dual_ble_provenance_t live_identity=provenance;
    const cerberus_dual_ble_provenance_t selected_identity=live_identity;
    source.receive.sid=12;source.raw[2]=0x99;source.rssi=-10;
    ++live_identity.boot_id;++live_identity.epoch;++live_identity.generation;live_identity.index=0;
    cerberus_ble_archive_row_t saved;
    assert(cerberus_dual_ble_archive_capture_row(&saved,&selected,&selected_identity,500));
    assert(saved.receive.sid==7&&saved.rssi==-93&&saved.advertisement[2]==6&&
        saved.receiver_boot_id==selected_identity.boot_id&&saved.scan_epoch==selected_identity.epoch&&
        saved.generation==0&&saved.index==31);
    assert(source.receive.sid==12&&live_identity.boot_id!=saved.receiver_boot_id);
}

static void check_family_capture(void)
{
    const uint8_t samples[6][31]={
        {26,0xff,0x4c,0,2,0x15,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,0x12,0x34,0xab,0xcd,0xc5},
        {3,3,0x44,0xfd},{3,3,0x81,0x30},{15,9,'F','S',' ','E','x','t',' ','B','a','t','t','e','r','y'},
        {3,3,0x5f,0xfd},{6,9,'H','C','-','0','5'}};
    const uint8_t lengths[]={27,4,4,16,4,7};
    for(unsigned family=1;family<=6;++family){
        cerberus_dual_ble_row_t source=observation();uint8_t full[255]={39,0x30};
        memcpy(full+40,samples[family-1],lengths[family-1]);
        source.raw_length=31;source.event_type=3;source.raw_flags=3;memcpy(source.raw,full,31);
        source.receive=(cerberus_dual_ble_receive_metadata_t){.source_kind=2,.callback_length=255,
            .sid=255,.primary_phy=3,.secondary_phy=2,.ad_validation=1};
        cerberus_dual_ble_family_evidence_t e;
        assert(cerberus_dual_ble_family_parse_ad(full,255,source.address,source.address_type,&e));
        assert(e.categories==(1U<<(family-1)));
        assert(cerberus_dual_ble_family_wire_encode(&e,(uint8_t)family,&source.family));
        receive_roundtrip(&source);
        const cerberus_dual_ble_row_t selected=source;
        const cerberus_dual_ble_provenance_t selected_proof=provenance;
        /* A replacement live row/lease does not retarget the frozen selection. */
        memset(&source.family,0,sizeof(source.family));memset(source.raw,0,sizeof(source.raw));source.address[0]^=1;
        cerberus_dual_ble_provenance_t next=provenance;++next.boot_id;++next.epoch;++next.generation;next.index=0;
        cerberus_ble_archive_row_t saved;
        assert(cerberus_dual_ble_archive_capture_row(&saved,&selected,&selected_proof,500));
        assert(!memcmp(&saved.family,&selected.family,sizeof(saved.family))&&saved.receiver_boot_id==selected_proof.boot_id&&
            saved.scan_epoch==selected_proof.epoch&&saved.index==31&&saved.generation==0);
        assert(saved.receiver_boot_id!=next.boot_id&&saved.scan_epoch!=next.epoch);
        for(unsigned fault=0;fault<15;++fault){
            source=selected;
            if(fault==0)source.family.version=0;
            if(fault==1)source.family.version=2;
            if(fault==2)source.family.family_code=0;
            if(fault==3)source.family.family_code=7;
            if(fault==4)source.family.flags|=0x80;
            if(fault==5)source.family.reserved[0]=1;
            if(fault==6)source.family.reserved[1]=1;
            if(fault==7)source.family.categories^=0x80;
            if(fault==8)source.family.hc_name=4;
            if(fault==9)source.family.meta_pairs[9]|=0x80;
            if(fault==10)memset(&source.receive,0,sizeof(source.receive));
            if(fault==11)source.receive.ad_validation=2;
            if(fault==12)source.event_type=4;
            if(fault==13)source.address_type=4;
            if(fault==14)source.raw_flags=0;
            reject_unchanged(&source,&provenance);
        }
        source=selected;full[254]=1;
        assert(!cerberus_dual_ble_family_parse_ad(full,255,source.address,source.address_type,&e));
        assert(cerberus_dual_ble_family_wire_encode(&e,(uint8_t)family,&source.family));
        source.receive.ad_validation=2;receive_roundtrip(&source); /* Historical dequalification, not absent. */
        source=selected;cerberus_dual_ble_family_unavailable(&e,CERBERUS_DUAL_BLE_FAMILY_RECEIVE_UNKNOWN);
        assert(cerberus_dual_ble_family_wire_encode(&e,(uint8_t)family,&source.family));
        memset(&source.receive,0,sizeof(source.receive));receive_roundtrip(&source);
    }
    /* A known legacy callback carrying a complete short original is also a
     * genuine full proof; the callback source must survive unchanged. */
    cerberus_dual_ble_row_t source=observation();source.raw_length=7;memcpy(source.raw,samples[5],7);
    source.receive=(cerberus_dual_ble_receive_metadata_t){.source_kind=1,.callback_length=7,.sid=255,.data_status=255,.ad_validation=1};
    cerberus_dual_ble_family_evidence_t e;
    assert(cerberus_dual_ble_family_parse_ad(source.raw,7,source.address,source.address_type,&e));
    assert(cerberus_dual_ble_family_wire_encode(&e,6,&source.family));receive_roundtrip(&source);
    source.receive=(cerberus_dual_ble_receive_metadata_t){.source_kind=2,.properties=0x13,.callback_length=7,
        .sid=255,.primary_phy=1,.ad_validation=1};receive_roundtrip(&source);
}

static void check_response_capture(void)
{
    for (uint8_t family=1;family<=6;++family) {
        cerberus_dual_ble_row_t source=observation();
        cerberus_ble_archive_row_t facts={0};
        memcpy(facts.address,source.address,6);facts.address_type=source.address_type;
        assert(archive_test_response_row(&facts,family,family));
        memcpy(source.raw,facts.advertisement,31);source.raw_length=3;source.event_type=3;source.raw_flags=0;
        source.receive=facts.receive;source.family=facts.family;source.response=facts.c5_response;
        source.callback_age_ms=125;source.request_started_ms=100;
        const cerberus_dual_ble_row_t selected=source;
        const cerberus_dual_ble_provenance_t proof=provenance;
        cerberus_ble_archive_t document={.saved_uptime_ms=500,.count=1},decoded;
        assert(cerberus_dual_ble_archive_capture_row(&document.rows[0],&selected,&proof,500));
        assert(document.rows[0].c5_response_age_upper_valid&&document.rows[0].c5_response_age_upper_at_save_ms==523);
        assert(document.rows[0].age_upper_at_save_ms==525&&document.rows[0].rssi==source.rssi);
        assert(document.rows[0].reports_seen==source.reports&&document.rows[0].response_state==0);
        assert(!memcmp(&document.rows[0].c5_response,&source.response,sizeof(source.response)));
        assert(cerberus_ble_archive_validate(&document));
        uint8_t wire[CERBERUS_BLE_ARCHIVE_MAX_SIZE];size_t length=0;
        assert(cerberus_ble_archive_encode(&document,wire,sizeof(wire),&length)&&length==464);
        assert(cerberus_ble_archive_decode(wire,length,&decoded));
        assert(!memcmp(&document,&decoded,sizeof(document)));
        uint64_t age=0;assert(cerberus_ble_archive_c5_response_age_upper_at_save(&decoded,0,&age)&&age==523);
        assert_exact_times_unknown(&decoded.rows[0]);
        /* Selection remains its own immutable observation after a new live
         * response, raw original, bank or boot replaces current state. */
        source.response.response_sequence+=9;source.response.raw[0]^=1;source.family.categories=0;
        cerberus_dual_ble_provenance_t live=proof;++live.boot_id;++live.epoch;
        cerberus_ble_archive_row_t again;
        assert(cerberus_dual_ble_archive_capture_row(&again,&selected,&proof,500));
        assert(!memcmp(&again,&document.rows[0],sizeof(again))&&again.receiver_boot_id!=live.boot_id);
        for (unsigned fault=0;fault<15;++fault) {
            source=selected;
            if(fault==0)source.response.version=0;
            if(fault==1)source.response.state=0;
            if(fault==2)source.response.original_sequence=0;
            if(fault==3)source.response.response_sequence=source.response.original_sequence;
            if(fault==4)source.response.interval_us=2000001;
            if(fault==5)source.response.receive.sid=8;
            if(fault==6)source.response.receive.properties=2;
            if(fault==7)source.response.receive.ad_validation=2;
            if(fault==8)source.response.raw_length=32;
            if(fault==9)source.response.raw_flags=0;
            if(fault==10)source.response.family.family_code=(uint8_t)(family%6+1);
            if(fault==11)source.response.reserved_tail=1;
            if(fault==12)source.response.age_ms=126;
            if(fault==13)source.response.interval_us=1000;
            if(fault==14)source.callback_age_ms=UINT32_MAX;
            reject_unchanged(&source,&proof);
        }
        source=selected;
        assert(cerberus_dual_ble_archive_capture_row(&again,&source,&proof,99));
        assert(!again.age_upper_valid&&!again.c5_response_age_upper_valid&&!again.c5_response_age_upper_at_save_ms);
        source.request_started_ms=0;
        assert(cerberus_dual_ble_archive_capture_row(&again,&source,&proof,UINT64_MAX-124));
        assert(!again.age_upper_valid&&again.c5_response_age_upper_valid&&again.c5_response_age_upper_at_save_ms==UINT64_MAX-1);
        source=selected;source.response.flags&=(uint8_t)~CERBERUS_DUAL_BLE_RESPONSE_AGE_VALID;
        source.response.age_ms=UINT32_MAX;
        assert(cerberus_dual_ble_archive_capture_row(&again,&source,&proof,500));
        assert(again.age_upper_valid&&!again.c5_response_age_upper_valid);
        source=selected;memset(&source.response,0,sizeof(source.response));
        source.response.version=1;source.response.state=CERBERUS_DUAL_BLE_RESPONSE_NONE;
        source.response.original_sequence=selected.response.original_sequence;
        assert(cerberus_dual_ble_archive_capture_row(&again,&source,&proof,500));
        assert(again.c5_response.state==CERBERUS_DUAL_BLE_RESPONSE_NONE&&!again.c5_response_age_upper_valid);
        source=selected;memset(&source.response,0,sizeof(source.response));
        assert(cerberus_dual_ble_archive_capture_row(&again,&source,&proof,500));
        assert(again.c5_response.state==CERBERUS_DUAL_BLE_RESPONSE_UNKNOWN&&!again.c5_response_age_upper_valid);
    }
}

int main(void)
{
    cerberus_dual_ble_row_t source=observation();
    cerberus_ble_archive_row_t row;
    assert(cerberus_dual_ble_archive_capture_row(&row,&source,&provenance,500));
    assert(row.receiver==CERBERUS_BLE_ARCHIVE_RECEIVER_C5);
    assert(row.rssi_kind==CERBERUS_BLE_ARCHIVE_RSSI_LATEST_ADV && row.rssi_valid && row.rssi==-93);
    assert(row.receiver_boot_id==provenance.boot_id && row.scan_epoch_valid && row.scan_epoch==provenance.epoch);
    assert(row.generation==0 && row.index==31 && row.address_type==3);
    assert(row.reports_seen==65535 && row.age_upper_valid && row.age_upper_at_save_ms==7400);
    assert_exact_times_unknown(&row);
    /* Aggregate peer hints are copied separately from a different raw company
     * ID. A mapper must never edit the packet to make the metadata agree. */
    assert(!strcmp(row.name,"OLD PEER NAME") && row.manufacturer_id==0x004c);
    assert(row.uuid_length==2 && row.uuid[0]==0x44 && row.uuid[1]==0xfd);
    assert(row.advertisement_length==source.raw_length && !memcmp(row.advertisement,source.raw,source.raw_length));
    assert(row.advertisement[2]==0x06 && row.advertisement_event_type==source.event_type);
    cerberus_ble_archive_t document={.saved_uptime_ms=500,.count=1};document.rows[0]=row;
    assert(cerberus_ble_archive_validate(&document));
    uint64_t age=0;assert(cerberus_ble_archive_age_upper_at_save(&document,0,&age) && age==7400);
    /* Evidence older than the S3 boot is valid as a bound, not an exact time. */
    assert(age>document.saved_uptime_ms);

    /* Raw malformed/truncated evidence survives byte-for-byte. Unused tails
     * are canonicalized, not made into new evidence or parsed for eligibility. */
    source.raw_flags=CERBERUS_BLE_ADVERTISEMENT_MALFORMED|CERBERUS_BLE_ADVERTISEMENT_TRUNCATED;
    source.raw_length=31;for(unsigned i=0;i<31;++i)source.raw[i]=(uint8_t)(255-i);
    assert(cerberus_dual_ble_archive_capture_row(&row,&source,&provenance,500));
    assert(row.advertisement_flags==source.raw_flags && !memcmp(row.advertisement,source.raw,31));
    document.rows[0]=row;assert(cerberus_ble_archive_validate(&document));
    uint8_t encoded[CERBERUS_BLE_ARCHIVE_MAX_SIZE];size_t written=0;
    assert(cerberus_ble_archive_encode(&document,encoded,sizeof(encoded),&written));
    cerberus_ble_archive_t decoded;
    assert(cerberus_ble_archive_decode(encoded,written,&decoded));
    assert(decoded.rows[0].receiver_boot_id==provenance.boot_id && decoded.rows[0].scan_epoch==provenance.epoch);
    assert(decoded.rows[0].advertisement_flags==source.raw_flags && !memcmp(decoded.rows[0].advertisement,source.raw,31));
    assert(cerberus_ble_archive_age_upper_at_save(&decoded,0,&age) && age==7400);
    assert_exact_times_unknown(&decoded.rows[0]);

    source=observation();source.request_started_ms=0;source.callback_age_ms=0;
    assert(cerberus_dual_ble_archive_capture_row(&row,&source,&provenance,0));
    assert(row.age_upper_valid && row.age_upper_at_save_ms==0);assert_exact_times_unknown(&row);
    source.callback_age_ms=7;
    assert(cerberus_dual_ble_archive_capture_row(&row,&source,&provenance,UINT64_MAX-7));
    assert(row.age_upper_valid && row.age_upper_at_save_ms==UINT64_MAX);
    assert(cerberus_dual_ble_archive_capture_row(&row,&source,&provenance,UINT64_MAX-6));
    assert(!row.age_upper_valid && !row.age_upper_at_save_ms);
    source=observation();
    assert(cerberus_dual_ble_archive_capture_row(&row,&source,&provenance,99));
    assert(!row.age_upper_valid && !row.age_upper_at_save_ms);
    source.callback_age_ms=UINT32_MAX;
    assert(cerberus_dual_ble_archive_capture_row(&row,&source,&provenance,500));
    assert(!row.age_upper_valid && !row.age_upper_at_save_ms);
    source=observation();source.timing_flags=CERBERUS_DUAL_BLE_RSSI_VALID;
    assert(cerberus_dual_ble_archive_capture_row(&row,&source,&provenance,500));
    assert(!row.age_upper_valid && row.rssi_valid && row.rssi==-93);
    source.timing_flags=CERBERUS_DUAL_BLE_SEEN;source.rssi=127;
    assert(cerberus_dual_ble_archive_capture_row(&row,&source,&provenance,500));
    assert(row.age_upper_valid && !row.rssi_valid && row.rssi==CERBERUS_BLE_ARCHIVE_RSSI_UNAVAILABLE);

    source=observation();reject_unchanged(NULL,&provenance);reject_unchanged(&source,NULL);
    cerberus_dual_ble_provenance_t bad=provenance;
    bad.boot_id=0;reject_unchanged(&source,&bad);bad=provenance;
    bad.epoch=0;reject_unchanged(&source,&bad);bad=provenance;
    bad.index=32;reject_unchanged(&source,&bad);
    source.address_type=4;reject_unchanged(&source,&provenance);source=observation();
    source.uuid_length=3;reject_unchanged(&source,&provenance);source=observation();
    source.raw_length=32;reject_unchanged(&source,&provenance);source=observation();
    source.event_type=5;reject_unchanged(&source,&provenance);source=observation();
    source.flags=0x80;reject_unchanged(&source,&provenance);source=observation();
    source.raw_flags=0x80;reject_unchanged(&source,&provenance);source=observation();
    source.timing_flags=0x80;reject_unchanged(&source,&provenance);source=observation();
    source.rssi=21;reject_unchanged(&source,&provenance);source=observation();
    source.rssi=-128;reject_unchanged(&source,&provenance);source=observation();
    memset(source.name,'X',sizeof(source.name));reject_unchanged(&source,&provenance);
    source.name[32]=0;
    assert(cerberus_dual_ble_archive_capture_row(&row,&source,&provenance,500));
    assert(strlen(row.name)==32);
    check_receive_capture();
    check_family_capture();
    check_response_capture();
    puts("C5 archive capture: frozen original/response identity, all-six full255 response families, independent age bounds, v1-v6 and transactional rejection passed");
    return 0;
}
