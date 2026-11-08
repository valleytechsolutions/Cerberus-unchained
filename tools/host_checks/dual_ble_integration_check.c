/* Wire both production ends together. Only RF/controller completion, mutex,
 * clock, packet parser and transport failure are mocked by the owner shim. */
#define main c5_owner_fixture_main
#include "dual_ble_owner_check.c"
#undef main
#include "dual_ble_scan.h"
#include "dual_ble_archive_capture.h"
#include "dual_ble_family.h"
#include "c5_ble_legacy_getters.h"

typedef struct {
    bool legacy, lose_begin, mutated, extended, old_metadata, bad_fence, bad_analyzer_binding, family, response;
    uint64_t rf_started_ms, first_begin_ms, fallback_handle;
    unsigned begin_calls, reads, releases, frames, metadata_reads, old_reads, unsupported_at, analyzer_reads, family_reads, response_reads;
    unsigned compact_reads, full_response_reads, compact_unsupported_at;
} integration_t;

static uint64_t integration_clock(void *context) { (void)context; return now_ms; }
static void integration_report(int8_t rssi, uint8_t payload)
{
    uint8_t raw[]={2,1,payload};
    struct ble_gap_disc_desc packet={.addr={{1,2,3,4,5,6},1},.rssi=rssi,
        .event_type=0,.data=raw,.length_data=sizeof(raw)};
    struct ble_gap_event event={.type=BLE_GAP_EVENT_DISC,.disc=packet};
    /* Actual callback obtains its own clock; fixtures do not substitute an
     * invented source time for the receive metadata path. */
    assert(!gap_event(&event,(void*)(uintptr_t)scan_callback_epoch));
}
static void integration_extended_report(uint8_t address,uint8_t type,int8_t rssi,uint8_t sid)
{
    uint8_t payload[255]={254,0xff,0x4c,0};
    struct ble_gap_event event={.type=BLE_GAP_EVENT_EXT_DISC,.ext_disc={
        .addr={{address,2,3,4,5,6},type},.rssi=rssi,.sid=sid,.prim_phy=3,.sec_phy=1,
        .length_data=sizeof(payload),.data=payload}};
    assert(!gap_event(&event,(void*)(uintptr_t)scan_callback_epoch));
}
static void integration_family_report(uint8_t address,uint8_t type,int8_t rssi,uint8_t suffix)
{
    /* The HC name begins beyond the retained31-byte prefix. The full callback
     * is valid even though its prefix terminates inside the first AD field. */
    uint8_t payload[47]={39,0xff,0x4c,0};
    memcpy(payload+40,(uint8_t[]){6,9,'H','C','-','0',suffix},7);
    struct ble_gap_event event={.type=BLE_GAP_EVENT_EXT_DISC,.ext_disc={
        .addr={{address,2,3,4,5,6},type},.rssi=rssi,.sid=7,.prim_phy=3,.sec_phy=1,
        .length_data=sizeof(payload),.data=payload}};
    assert(!gap_event(&event,(void*)(uintptr_t)scan_callback_epoch));
}

static void integration_response_original(uint8_t address,uint8_t type,uint8_t sid,bool partial)
{
    uint8_t bytes[]={2,1,6};
    struct ble_gap_event event={.type=BLE_GAP_EVENT_EXT_DISC,.ext_disc={
        .addr={{address,2,3,4,5,6},type},.rssi=-30,.props=2,.sid=sid,.prim_phy=1,.sec_phy=1,
        .data_status=partial?1:0,.length_data=sizeof(bytes),.data=bytes}};
    assert(!gap_event(&event,(void*)(uintptr_t)scan_callback_epoch));
}
static void integration_response_packet(uint8_t address,uint8_t type,uint8_t sid,uint8_t suffix)
{
    uint8_t payload[255]={39,0xff,0x4c,0};
    memcpy(payload+40,(uint8_t[]){6,9,'H','C','-','0',suffix},7);
    struct ble_gap_event event={.type=BLE_GAP_EVENT_EXT_DISC,.ext_disc={
        .addr={{address,2,3,4,5,6},type},.rssi=-49,.props=10,.sid=sid,.prim_phy=1,.sec_phy=1,
        .length_data=sizeof(payload),.data=payload}};
    assert(!gap_event(&event,(void*)(uintptr_t)scan_callback_epoch));
}
static void integration_response_pair(uint8_t suffix)
{
    integration_response_original(1,1,7,false);now_ms+=3U;
    integration_response_packet(1,1,7,suffix);
}

static esp_err_t integration_exchange(void *context,uint8_t command_id,const void *request,
    size_t request_size,void *response,size_t response_size)
{
    integration_t *i=context;++i->frames;
    if(command_id==CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_FAMILY)++i->family_reads;
    if(command_id==CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_RESPONSE){
        ++i->response_reads;++i->full_response_reads;
        if(i->compact_unsupported_at){const cerberus_dual_ble_snapshot_read_t *r=request;
            assert(r->handle==i->fallback_handle);}
    }
    if(command_id==CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_RESPONSE_COMPACT){
        ++i->response_reads;++i->compact_reads;
        if(i->compact_unsupported_at==i->compact_reads){
            const cerberus_dual_ble_snapshot_read_t *r=request;
            i->fallback_handle=r->handle;now_ms+=8U;return ESP_ERR_NOT_SUPPORTED;
        }
    }
    cerberus_dual_ble_binding_t wrong_binding;
    if(command_id==CERBERUS_SPI_COMMAND_DUAL_BLE_ANALYZER_SAMPLE){
        ++i->analyzer_reads;
        if(i->bad_analyzer_binding){memcpy(&wrong_binding,request,sizeof(wrong_binding));
            ++wrong_binding.boot_id;request=&wrong_binding;}
    }
    if(command_id==CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_METADATA){
        ++i->metadata_reads;
        if(i->old_metadata&&i->metadata_reads==i->unsupported_at){
            const cerberus_dual_ble_snapshot_read_t *r=request;
            i->fallback_handle=r->handle;now_ms+=8U;return ESP_ERR_NOT_SUPPORTED;
        }
    }
    if(command_id==CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ){
        ++i->old_reads;
        if(i->old_metadata){const cerberus_dual_ble_snapshot_read_t *r=request;
            assert(r->handle==i->fallback_handle);}
    }
    if(command_id==CERBERUS_SPI_COMMAND_BLE_SCAN_RESULT){
        assert(i->legacy&&request_size==sizeof(cerberus_ble_scan_result_request_t));
        const cerberus_ble_scan_result_request_t *r=request;
        esp_err_t result=cerberus_ble_scanner_get_result(r->generation,r->index,response);now_ms+=8U;return result;
    }
    if(command_id==CERBERUS_SPI_COMMAND_BLE_SCAN_DATA){
        assert(i->legacy&&request_size==sizeof(cerberus_ble_scan_data_request_t));
        const cerberus_ble_scan_data_request_t *r=request;
        esp_err_t result=cerberus_ble_scanner_get_data_chunk(r->generation,r->index,r->kind,r->chunk,response);now_ms+=8U;return result;
    }
    if(command_id>=CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_BEGIN&&
       command_id<=CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_STATUS)assert(!i->legacy);
    unsigned old_starts=starts;size_t size=0;
    bool handled=cerberus_ble_dual_command(command_id,request,request_size,response,&size);
    assert(handled&&size==response_size&&!held);
    if(command_id==CERBERUS_SPI_COMMAND_DUAL_BLE_INFO&&i->legacy){
        /* Compatibility shim presents the old INFO capability/version only;
         * accepted v1 START/status and completed-bank getters remain actual C5. */
        cerberus_dual_ble_info_t *info=response;info->version=1;
        info->flags&=(uint8_t)~CERBERUS_DUAL_BLE_SNAPSHOTS;
    }
    if(command_id==CERBERUS_SPI_COMMAND_DUAL_BLE_START&&starts!=old_starts){
        i->rf_started_ms=now_ms;
        if(i->response)integration_response_pair('5');
        else if(i->family)integration_family_report(1,1,-30,'5');
        else if(i->extended)integration_extended_report(1,1,-30,7);else integration_report(-30,6);
        if(dual_mode!=CERBERUS_DUAL_BLE_ANALYZER){
            memcpy(scan_results[0].name,"REAL",4);scan_results[0].name_length=4;
            scan_results[0].uuid_length=2;scan_results[0].uuid[0]=0xaa;scan_results[0].uuid[1]=0xfe;
            scan_results[0].manufacturer_id=0x004c;
        }
    }
    if(command_id==CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_BEGIN){
        ++i->begin_calls;if(!i->first_begin_ms)i->first_begin_ms=now_ms;
        if(i->lose_begin){i->lose_begin=false;now_ms+=8U;return ESP_ERR_TIMEOUT;}
    }
    if(command_id==CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ||
        command_id==CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_METADATA||
        command_id==CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_FAMILY||
        command_id==CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_RESPONSE||
        command_id==CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_RESPONSE_COMPACT){
        ++i->reads;
        /* This update occurs AFTER the first real chunk reply, while remaining
         * chunks are still in flight. No mixture may reach the S3 publisher. */
        if(!i->mutated){
            if(i->response)integration_response_pair('6');
            else if(i->family)integration_family_report(1,1,-90,'6');
            else if(i->extended||scan_status.stored_count==32)integration_extended_report(1,1,-90,12);
            else integration_report(-90,7);
            i->mutated=true;
        }
    }
    if(command_id==CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_STATUS&&i->bad_fence)
        ++((cerberus_dual_ble_snapshot_status_t*)response)->count;
    if(command_id==CERBERUS_SPI_COMMAND_DUAL_BLE_RELEASE)++i->releases;
    now_ms+=8U; /* Exercise the real eight-ms tick budget with one in-flight exchange. */
    return ESP_OK;
}

static void integration_reset(integration_t *i,cerberus_dual_ble_scan_t *s,bool legacy)
{
    assert(!held);memset(i,0,sizeof(*i));i->legacy=legacy;
    memset(scan_results,0,sizeof(scan_results));memset(&scan_status,0,sizeof(scan_status));
    memset(&dual_snapshot,0,sizeof(dual_snapshot));
    dual_boot_id=UINT64_C(0x123456789abcdef);dual_snapshot.counter=dual_boot_id;
    dual_epoch=dual_epoch_counter=0;dual_job_id=dual_job_highwater=dual_last_contact_ms=0;
    dual_attached=dual_radio_active=dual_track_seen=false;dual_error=0;
    scan_callback_epoch=0;starts=stops=0;other_radio_busy=cancel_fails=snapshot_busy=false;
    host_synced=true;parse_fails=false;now_ms=1000;
    cerberus_dual_ble_ops_t ops={.context=i,.exchange=integration_exchange,.now_ms=integration_clock};
    cerberus_dual_ble_init(s,&ops);
    assert(cerberus_dual_ble_begin(s,CERBERUS_DUAL_BLE_DISCOVERY,NULL,0)==ESP_OK);
}

static void integration_tick(integration_t *i,cerberus_dual_ble_scan_t *s)
{
    now_ms+=20;
    if(i->rf_started_ms&&dual_radio_active&&now_ms-i->rf_started_ms>=8000U){
        /* Mock the existing eight-second controller completion only. */
        finish_scan_for_epoch(0,scan_callback_epoch);
    }
    unsigned frames=i->frames;cerberus_dual_ble_tick(s,now_ms);
    assert(i->frames-frames<=1U); /* Each mocked transport costs the full8ms tick budget. */
    assert(!s->error&&!held);
}

static void integration_analyzer(void)
{
    integration_t i;cerberus_dual_ble_scan_t s;
    integration_reset(&i,&s,false);cerberus_dual_ble_stop(&s);
    assert(cerberus_dual_ble_begin(&s,CERBERUS_DUAL_BLE_ANALYZER,NULL,0)==ESP_OK);
    parse_calls=0;
    for(unsigned n=0;n<100&&!s.analyzer.samples;++n)integration_tick(&i,&s);
    assert(s.analyzer.samples==1&&s.analyzer.reports==1&&!scan_status.stored_count&&!parse_calls);
    /* Greater than inventory capacity, then partial extended originals. All
     * count without parsing or inserting rows. Responses/anonymous do not. */
    uint8_t bytes[]={2,1,6};
    struct ble_gap_event packet={.type=BLE_GAP_EVENT_DISC,.disc={
        .addr={{1,2,3,4,5,6},1},.rssi=-55,.event_type=0,.data=bytes,.length_data=3}};
    for(unsigned n=0;n<64;++n){packet.disc.addr.val[0]=(uint8_t)n;
        ++now_ms;assert(!gap_event(&packet,(void*)(uintptr_t)scan_callback_epoch));}
    struct ble_gap_event partial={.type=BLE_GAP_EVENT_EXT_DISC,.ext_disc={
        .addr={{99,2,3,4,5,6},1},.rssi=-75,.sid=3,.prim_phy=3,.data_status=1,
        .data=bytes,.length_data=3}};
    for(unsigned n=0;n<10;++n){++now_ms;assert(!gap_event(&partial,(void*)(uintptr_t)scan_callback_epoch));}
    partial.ext_disc.props=BLE_HCI_ADV_SCAN_RSP_MASK;
    assert(!gap_event(&partial,(void*)(uintptr_t)scan_callback_epoch));
    partial.ext_disc.props=0;partial.ext_disc.addr.type=255;
    assert(!gap_event(&partial,(void*)(uintptr_t)scan_callback_epoch));
    packet.disc.event_type=BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP;
    assert(!gap_event(&packet,(void*)(uintptr_t)scan_callback_epoch));
    assert(dual_analyzer.reports==75&&!parse_calls&&!scan_status.stored_count&&!scan_inventory.replaced);
    for(unsigned n=0;n<100&&s.analyzer.samples<2;++n)integration_tick(&i,&s);
    cerberus_dual_ble_analyzer_view_t view;cerberus_dual_ble_analyzer_view(&s,&view);
    assert(view.reports==75&&view.history_count==1&&view.history[0].reports==74&&view.history[0].duration_ms>=500);
    uint32_t rate;assert(cerberus_dual_ble_analyzer_rate(&view.history[0],&rate));
    assert(rate==(uint32_t)(UINT64_C(74000)/view.history[0].duration_ms));
    for(unsigned n=0;n<100&&s.analyzer.samples<3;++n)integration_tick(&i,&s);
    cerberus_dual_ble_analyzer_view(&s,&view);
    assert(view.history_count==2&&view.history[1].valid&&!view.history[1].reports&&view.history[1].duration_ms>=500);
    assert(!i.begin_calls&&!i.reads&&!i.metadata_reads&&!i.old_reads&&!s.snapshots&&!s.count);
    uint64_t first_epoch=view.epoch;
    for(unsigned n=0;n<500&&!s.sweeps;++n)integration_tick(&i,&s);
    assert(s.sweeps==1&&s.analyzer.final&&s.analyzer.reports==75&&s.analyzer.elapsed_ms>=8000);
    for(unsigned n=0;n<100&&s.analyzer.epoch==first_epoch;++n)integration_tick(&i,&s);
    cerberus_dual_ble_analyzer_view(&s,&view);
    assert(starts==2&&view.epoch!=first_epoch&&view.reports==1&&view.sweeps==1);
    assert(view.history_count&&!view.history[view.history_count-1].valid);
    unsigned samples=i.analyzer_reads;cerberus_dual_ble_stop(&s);
    for(unsigned n=0;n<50&&!cerberus_dual_ble_released(&s);++n)integration_tick(&i,&s);
    assert(cerberus_dual_ble_released(&s)&&!dual_attached&&!dual_radio_active&&i.analyzer_reads==samples);

    integration_reset(&i,&s,false);cerberus_dual_ble_stop(&s);
    assert(cerberus_dual_ble_begin(&s,2,NULL,0)==ESP_OK);
    for(unsigned n=0;n<100&&!s.analyzer.samples;++n)integration_tick(&i,&s);
    i.bad_analyzer_binding=true;
    for(unsigned n=0;n<100&&!s.error;++n){now_ms+=20;cerberus_dual_ble_tick(&s,now_ms);}
    assert(s.error==ESP_ERR_INVALID_RESPONSE&&!s.analyzer.sample_valid&&!s.snapshots);
    i.bad_analyzer_binding=false;cerberus_dual_ble_stop(&s);
    for(unsigned n=0;n<50&&!cerberus_dual_ble_released(&s);++n){now_ms+=20;cerberus_dual_ble_tick(&s,now_ms);}
    assert(cerberus_dual_ble_released(&s)&&!dual_attached&&!dual_radio_active);
    puts("Actual C5 callbacks -> bound Analyzer sample -> S3: >32 identities, partial originals, exclusions, silence, final sweep, restart gap and stale binding passed");
}
static void integration_family(void)
{
    integration_t i;cerberus_dual_ble_scan_t s;
    integration_reset(&i,&s,false);cerberus_dual_ble_stop(&s);i.family=true;i.lose_begin=true;
    assert(cerberus_dual_ble_begin_family(&s,6)==ESP_OK);
    for(unsigned n=0;n<250&&!s.snapshots;++n)integration_tick(&i,&s);
    assert(s.snapshots==1&&s.count==1&&s.published_family==6&&i.family_reads==16);
    assert(!i.metadata_reads&&!i.old_reads&&i.begin_calls>=2&&starts==1&&!s.sweeps);
    cerberus_dual_ble_row_t row;cerberus_dual_ble_provenance_t proof;
    assert(cerberus_dual_ble_copy_row(&s,0,&row,&proof));
    cerberus_dual_ble_family_evidence_t evidence;
    assert(cerberus_dual_ble_family_classify(&row,&proof,&evidence));
    assert(evidence.hc_name==CERBERUS_BLE_HC_05&&
        (evidence.categories&CERBERUS_DUAL_BLE_FAMILY_HC_NAME_HINT));
    assert(row.raw_length==31&&row.receive.callback_length==47&&row.raw_flags==3&&row.rssi==-30);
    assert(row.request_started_ms==i.first_begin_ms&&i.mutated);
    for(unsigned n=0;n<250&&s.snapshots<2;++n)integration_tick(&i,&s);
    assert(s.snapshots==2&&s.rows[0].rssi==-90);
    assert(cerberus_dual_ble_family_classify(&s.rows[0],&proof,&evidence)&&evidence.hc_name==CERBERUS_BLE_HC_06);
    assert(cerberus_dual_ble_family_classify(&row,&proof,&evidence)&&evidence.hc_name==CERBERUS_BLE_HC_05);
    /* A newer original without this family dequalifies its existing typed row;
     * it cannot keep the previous callback's evidence. New unmatched peers do
     * not occupy family admission capacity. */
    integration_report(-75,6);
    integration_extended_report(2,1,-60,1);
    for(unsigned n=0;n<250&&s.snapshots<3;++n)integration_tick(&i,&s);
    assert(s.snapshots==3&&s.count==1&&s.rows[0].rssi==-75);
    assert(cerberus_dual_ble_family_classify(&s.rows[0],&proof,&evidence)&&!evidence.categories);
    uint8_t malformed[]={4,0xff,0x4c,0};
    struct ble_gap_event invalid={.type=BLE_GAP_EVENT_DISC,.disc={
        .addr={{1,2,3,4,5,6},1},.rssi=-80,.event_type=0,.data=malformed,.length_data=sizeof(malformed)}};
    assert(!gap_event(&invalid,(void*)(uintptr_t)scan_callback_epoch));
    integration_family_report(1,0,-45,'5'); /* Equal address bytes, distinct typed identity. */
    for(unsigned n=0;n<250&&s.snapshots<4;++n)integration_tick(&i,&s);
    assert(s.snapshots==4&&s.count==2&&s.rows[0].address_type==1&&s.rows[1].address_type==0);
    assert(!cerberus_dual_ble_family_classify(&s.rows[0],&proof,&evidence)&&
        evidence.state==CERBERUS_DUAL_BLE_FAMILY_MALFORMED&&!evidence.available);
    cerberus_dual_ble_provenance_t second;
    cerberus_dual_ble_row_t second_row;
    assert(cerberus_dual_ble_copy_row(&s,1,&second_row,&second));
    assert(cerberus_dual_ble_family_classify(&second_row,&second,&evidence)&&evidence.hc_name==CERBERUS_BLE_HC_05);
    for(unsigned n=0;n<500&&!s.sweeps;++n)integration_tick(&i,&s);
    assert(s.sweeps==1&&s.published_family==6);
    cerberus_dual_ble_stop(&s);
    for(unsigned n=0;n<50&&!cerberus_dual_ble_released(&s);++n)integration_tick(&i,&s);
    assert(cerberus_dual_ble_released(&s)&&!dual_attached&&!dual_radio_active);
    /* Final-suffix cancellation must not publish even though the base110-byte
     * metadata projection has already arrived. */
    for(unsigned stop_at=1;stop_at<=16;stop_at+=15){
        integration_reset(&i,&s,false);cerberus_dual_ble_stop(&s);i.family=true;
        assert(cerberus_dual_ble_begin_family(&s,6)==ESP_OK);
        for(unsigned n=0;n<250&&i.family_reads<stop_at;++n)integration_tick(&i,&s);
        assert(i.family_reads==stop_at&&!s.snapshots);
        cerberus_dual_ble_stop(&s);
        for(unsigned n=0;n<50&&!cerberus_dual_ble_released(&s);++n)integration_tick(&i,&s);
        assert(cerberus_dual_ble_released(&s)&&!s.snapshots&&i.family_reads==stop_at&&!dual_attached);
    }
    puts("Actual C5 full callback -> FAMILY158 -> S3: beyond-prefix hints, frozen mutation, first-BEGIN age, dequalification, focused admission, final sweep and Stop passed");
}
static void integration_response(void)
{
    integration_t i;cerberus_dual_ble_scan_t s;
    for(uint8_t family=0;family<=6;family+=6){
        integration_reset(&i,&s,false);cerberus_dual_ble_stop(&s);i.response=true;i.lose_begin=true;
        assert(cerberus_dual_ble_begin_response(&s,family)==ESP_OK);
        for(unsigned n=0;n<300&&!s.snapshots;++n)integration_tick(&i,&s);
        assert(s.snapshots==1&&s.count==1&&s.published_response&&s.published_family==family);
        assert(i.response_reads==31&&!i.family_reads&&!i.metadata_reads&&!i.old_reads&&i.begin_calls==2);
        assert(starts==1&&!s.sweeps&&dual_radio_active&&i.mutated);
        cerberus_dual_ble_row_t selected;cerberus_dual_ble_provenance_t proof;
        assert(cerberus_dual_ble_copy_row(&s,0,&selected,&proof));
        cerberus_dual_ble_family_evidence_t evidence;
        assert(cerberus_dual_ble_response_present(&selected,&proof));
        assert(cerberus_dual_ble_response_classify(&selected,&proof,&evidence)&&evidence.hc_name==CERBERUS_BLE_HC_05);
        assert(selected.raw_length==3&&selected.raw[2]==6&&selected.rssi==-30);
        assert(selected.response.raw_length==31&&selected.response.raw_flags==1&&
            selected.response.receive.callback_length==255&&selected.response.rssi==-49);
        assert(selected.response.interval_us==3000&&selected.response.response_sequence>selected.response.original_sequence);
        assert(selected.request_started_ms==i.first_begin_ms&&
            selected.callback_age_ms-selected.response.age_ms==3U);
        uint32_t age;
        assert(cerberus_dual_ble_response_age(&selected,now_ms,&age)&&age>=selected.response.age_ms);
        for(unsigned n=0;n<300&&s.snapshots<2;++n)integration_tick(&i,&s);
        assert(s.snapshots==2&&cerberus_dual_ble_response_classify(&s.rows[0],&proof,&evidence)&&
            evidence.hc_name==CERBERUS_BLE_HC_06);
        assert(cerberus_dual_ble_response_classify(&selected,&proof,&evidence)&&evidence.hc_name==CERBERUS_BLE_HC_05);
        /* The raw original can survive an incomplete newer callback, but its
         * response must no longer be available under that newer context. */
        integration_response_original(1,1,7,true);
        integration_response_packet(1,1,7,'5');
        for(unsigned n=0;n<300&&s.snapshots<3;++n)integration_tick(&i,&s);
        assert(s.snapshots==3&&s.count==1&&s.rows[0].raw[2]==6&&
            s.rows[0].response.state==CERBERUS_DUAL_BLE_RESPONSE_UNKNOWN);
        assert(!cerberus_dual_ble_response_present(&s.rows[0],&proof));
        assert(cerberus_dual_ble_response_present(&selected,&proof));
        /* New valid original + mismatched SID/type cannot join; NONE means
         * no admitted matching response, not an assertion about the air. */
        integration_response_original(1,1,7,false);
        integration_response_packet(1,1,8,'5');integration_response_packet(1,0,7,'5');
        for(unsigned n=0;n<300&&s.snapshots<4;++n)integration_tick(&i,&s);
        assert(s.snapshots==4&&s.count==1&&s.rows[0].response.state==CERBERUS_DUAL_BLE_RESPONSE_NONE);
        assert(s.rows[0].response.original_sequence&&!s.rows[0].response.response_sequence);
        for(unsigned n=0;n<500&&!s.sweeps;++n)integration_tick(&i,&s);
        assert(s.sweeps==1&&s.published_response&&s.published_family==family);
        s.enabled=false;
        for(unsigned n=0;n<50&&!cerberus_dual_ble_released(&s);++n)integration_tick(&i,&s);
        assert(cerberus_dual_ble_released(&s)&&!dual_attached&&!dual_radio_active&&starts==1);
        assert(cerberus_dual_ble_response_present(&selected,&proof));
    }
    /* Stop at an early and final chunk, including the offset beyond255, never
     * publishes the partially read prefix or starts another RF procedure. */
    const unsigned stops_at[]={1,26,31};
    for(unsigned stop=0;stop<sizeof(stops_at)/sizeof(stops_at[0]);++stop){
        integration_reset(&i,&s,false);cerberus_dual_ble_stop(&s);i.response=true;
        assert(cerberus_dual_ble_begin_response(&s,6)==ESP_OK);
        for(unsigned n=0;n<300&&i.response_reads<stops_at[stop];++n)integration_tick(&i,&s);
        assert(i.response_reads==stops_at[stop]&&!s.snapshots);
        cerberus_dual_ble_stop(&s);
        for(unsigned n=0;n<50&&!cerberus_dual_ble_released(&s);++n)integration_tick(&i,&s);
        assert(cerberus_dual_ble_released(&s)&&!s.snapshots&&!s.published_response&&
            i.response_reads==stops_at[stop]&&!dual_attached&&starts==1);
    }
    integration_reset(&i,&s,false);cerberus_dual_ble_stop(&s);i.response=true;i.bad_fence=true;
    assert(cerberus_dual_ble_begin_response(&s,0)==ESP_OK);
    for(unsigned n=0;n<300&&!s.error;++n){now_ms+=20;cerberus_dual_ble_tick(&s,now_ms);}
    assert(s.error==ESP_ERR_INVALID_RESPONSE&&!s.snapshots&&!s.count&&!s.published_response);
    i.bad_fence=false;cerberus_dual_ble_stop(&s);
    for(unsigned n=0;n<50&&!cerberus_dual_ble_released(&s);++n){now_ms+=20;cerberus_dual_ble_tick(&s,now_ms);}
    assert(cerberus_dual_ble_released(&s)&&!dual_attached&&!s.snapshots);
    puts("Actual C5 original/response255 -> frozen310 -> S3: response-only family, preserved first-BEGIN age, immutable selection, partial/SID/type invalidation, final sweep, fence and Stop passed");
}
static void integration_compact_response(void)
{
    /* Exercise real callback admission, freeze, compact projection, reader
     * reconstruction and archive capture. Late optional-command rejection
     * must reread the same bank, including already staged complete rows. */
    const unsigned fallback_at[]={0,1,16,31,32,47,65,712};
    for(unsigned pass=0;pass<sizeof(fallback_at)/sizeof(fallback_at[0]);++pass){
        integration_t i;cerberus_dual_ble_scan_t s;
        integration_reset(&i,&s,false);cerberus_dual_ble_stop(&s);
        i.response=true;i.lose_begin=true;i.compact_unsupported_at=fallback_at[pass];
        assert(cerberus_dual_ble_begin_response(&s,0)==ESP_OK);
        for(unsigned n=0;n<20&&!i.rf_started_ms;++n)integration_tick(&i,&s);
        assert(i.rf_started_ms&&!i.begin_calls&&scan_status.stored_count==1);
        for(uint8_t address=2;address<=32;++address){
            ++now_ms;
            if(address%3U==0U)integration_extended_report(address,1,-60,7);
            else {
                integration_response_original(address,1,7,false);
                if(address%3U==2U){now_ms+=3U;integration_response_packet(address,1,7,'5');}
            }
        }
        assert(scan_status.stored_count==32);
        for(unsigned n=0;n<2100&&!s.snapshots;++n)integration_tick(&i,&s);
        assert(s.snapshots==1&&s.count==32&&i.begin_calls==2&&starts==1&&i.mutated);
        assert(i.compact_reads==(fallback_at[pass]?fallback_at[pass]:712));
        assert(i.full_response_reads==(fallback_at[pass]?992U:0U));
        assert(!i.family_reads&&!i.metadata_reads&&!i.old_reads);
        assert(!fallback_at[pass]||s.snapshot.handle==i.fallback_handle);
        unsigned states[3]={0};
        cerberus_dual_ble_binding_t binding={s.start.boot_id,s.start.job_id};
        for(uint8_t index=0;index<32;++index){
            cerberus_dual_ble_row_t row;cerberus_dual_ble_provenance_t proof;
            assert(cerberus_dual_ble_copy_row(&s,index,&row,&proof));
            /* Independent existing full133 and base127 projections from the
             * exact same frozen handle establish lossless reconstruction. */
            cerberus_dual_ble_snapshot_response_row_t full=snapshot_response_row(binding,s.snapshot.handle,index);
            assert(!memcmp(row.address,full.family.metadata.row.address,6)&&
                row.address_type==full.family.metadata.row.address_type&&
                row.rssi==full.family.metadata.row.latest_rssi&&
                row.callback_age_ms==full.family.metadata.row.age_ms&&
                row.request_started_ms==i.first_begin_ms&&
                row.reports==full.family.metadata.row.reports&&
                row.raw_length==full.family.metadata.row.raw_length&&
                !memcmp(row.raw,full.family.metadata.row.raw,sizeof(row.raw))&&
                !memcmp(&row.receive,&full.family.metadata.receive,sizeof(row.receive))&&
                !memcmp(&row.family,&full.family.family,sizeof(row.family))&&
                !memcmp(&row.response,&full.response,sizeof(row.response)));
            assert(row.response.state<=CERBERUS_DUAL_BLE_RESPONSE_PRESENT);
            ++states[row.response.state];
            cerberus_ble_archive_row_t archived;
            assert(cerberus_dual_ble_archive_capture_row(&archived,&row,&proof,now_ms));
            assert(!memcmp(&archived.c5_response,&full.response,sizeof(full.response))&&
                !memcmp(&archived.receive,&full.family.metadata.receive,sizeof(archived.receive))&&
                archived.receiver_boot_id==proof.boot_id&&archived.scan_epoch==proof.epoch);
        }
        assert(states[0]==10&&states[1]==10&&states[2]==12);
        cerberus_dual_ble_stop(&s);
        for(unsigned n=0;n<50&&!cerberus_dual_ble_released(&s);++n)integration_tick(&i,&s);
        assert(cerberus_dual_ble_released(&s)&&!dual_attached&&!dual_radio_active&&starts==1);
    }
    puts("Actual C5 mixed32 response callbacks -> compact712/full992 reads -> S3/archive: exact frozen equivalence, lost-BEGIN anchor, late fallback, immutable live mutation and Stop passed");
}

int main(void)
{
    integration_t i;cerberus_dual_ble_scan_t s;
    integration_reset(&i,&s,false);i.lose_begin=true;
    for(unsigned tick=0;tick<300U&&!s.snapshots;++tick)integration_tick(&i,&s);
    assert(s.snapshots==1&&s.count==1&&s.sweeps==0&&dual_radio_active&&starts==1&&!stops);
    assert(s.has_first_result&&s.first_result_ms<8000U&&s.live_updates&&i.begin_calls>=2&&i.mutated);
    cerberus_dual_ble_row_t row;assert(cerberus_dual_ble_row(&s,0,&row));
    cerberus_dual_ble_provenance_t source;
    assert(cerberus_dual_ble_copy_row(&s,0,&row,&source));
    assert(source.boot_id==dual_boot_id&&source.epoch==dual_epoch&&source.generation==scan_status.generation&&source.index==0);
    assert(row.rssi==-30&&row.reports==1&&row.raw_length==3&&row.raw[2]==6);
    assert(row.receive.source_kind==CERBERUS_DUAL_BLE_RECEIVE_LEGACY&&row.receive.data_status==255);
    assert(!strcmp(row.name,"REAL")&&row.uuid_length==2&&row.manufacturer_id==0x004c);
    assert(row.request_started_ms==i.first_begin_ms);
    assert(scan_results[0].latest_rssi==-90&&scan_results[0].advertisement[2]==7);
    for(unsigned tick=0;tick<500U&&!s.sweeps;++tick)integration_tick(&i,&s);
    assert(s.sweeps==1&&now_ms-i.rf_started_ms>=8000U&&s.snapshots>1);
    assert(cerberus_dual_ble_row(&s,0,&row)&&row.rssi==-90&&row.raw[2]==7&&row.reports==2);
    /* Prevent automatic next sweep only after final publication: release must
       still pass through the actual owner handler and settle locally. */
    s.enabled=false;
    for(unsigned tick=0;tick<20U&&!cerberus_dual_ble_released(&s);++tick)integration_tick(&i,&s);
    assert(cerberus_dual_ble_released(&s)&&!dual_attached&&!dual_snapshot.valid&&i.releases&&starts==1&&!stops);
    assert(cerberus_dual_ble_copy_row(&s,0,&row,&source));
    assert(source.boot_id==s.published_boot_id&&source.epoch==s.published_epoch&&!s.remote.epoch&&!s.start.job_id);

    integration_reset(&i,&s,false);
    for(unsigned tick=0;tick<100U&&!i.reads;++tick)integration_tick(&i,&s);
    assert(i.reads&&dual_radio_active&&!s.snapshots);
    cerberus_dual_ble_stop(&s);
    for(unsigned tick=0;tick<50U&&!cerberus_dual_ble_released(&s);++tick)integration_tick(&i,&s);
    assert(!dual_radio_active&&!dual_attached&&!dual_snapshot.valid&&stops==1&&!s.snapshots);

    integration_reset(&i,&s,true);
    for(unsigned tick=0;tick<300U&&(!i.rf_started_ms||now_ms-i.rf_started_ms<6000U);++tick)integration_tick(&i,&s);
    assert(dual_radio_active&&!s.snapshots&&!s.sweeps&&!i.begin_calls&&!i.reads);
    for(unsigned tick=0;tick<250U&&!s.sweeps;++tick)integration_tick(&i,&s);
    assert(s.sweeps==1&&s.count==1&&!s.live_updates&&!i.begin_calls&&!i.reads);
    assert(cerberus_dual_ble_row(&s,0,&row)&&row.rssi==-30&&!strcmp(row.name,"REAL")&&row.raw[2]==6);
    cerberus_dual_ble_receive_metadata_t no_metadata={0};
    assert(!memcmp(&row.receive,&no_metadata,sizeof(no_metadata))); /* v1 has no metadata claim. */
    assert(cerberus_dual_ble_copy_row(&s,0,&row,&source));
    assert(source.boot_id==dual_boot_id&&source.epoch==dual_epoch&&source.generation==scan_status.generation&&source.index==0);
    s.enabled=false;
    for(unsigned tick=0;tick<20U&&!cerberus_dual_ble_released(&s);++tick)integration_tick(&i,&s);
    assert(cerberus_dual_ble_released(&s)&&!dual_attached&&starts==1&&!stops);
    /* Dense reports run through the actual C5 callback/admission policy and
     * frozen wire commands. A later replacement during the download cannot
     * alter either the published rows or their published counter cohort. */
    integration_reset(&i,&s,false);
    for(unsigned tick=0;tick<20U&&!i.rf_started_ms;++tick)integration_tick(&i,&s);
    uint8_t raw[]={2,1,6};
    struct ble_gap_disc_desc dense={.addr={{2,2,3,4,5,6},1},.rssi=-70,.event_type=0,
        .data=raw,.length_data=sizeof(raw)};
    for(unsigned address=2;address<=33;++address){
        dense.addr.val[0]=(uint8_t)address;++now_ms;
        record_advertisement(&dense,scan_callback_epoch,(int64_t)now_ms*1000LL);
    }
    assert(scan_status.stored_count==32&&scan_inventory.replaced==1);
    for(unsigned tick=0;tick<700U&&!s.snapshots;++tick)integration_tick(&i,&s);
    assert(s.snapshots==1&&s.count==32&&s.inventory_known&&s.inventory_replacement_enabled);
    assert(s.inventory_replaced==1&&!s.inventory_dropped&&scan_inventory.replaced==2);
    cerberus_dual_ble_row_t painted;cerberus_dual_ble_provenance_t painted_source;
    assert(cerberus_dual_ble_copy_row(&s,1,&painted,&painted_source)&&painted.address[0]==2);
    for(unsigned tick=0;tick<700U&&s.snapshots<2;++tick)integration_tick(&i,&s);
    assert(s.snapshots==2&&s.inventory_known&&s.inventory_replaced==2);
    assert(cerberus_dual_ble_copy_row(&s,1,&row,&source)&&row.address[0]==1);
    assert(painted.address[0]==2&&painted_source.boot_id==source.boot_id&&painted_source.epoch==source.epoch);
    assert(painted.receive.source_kind==CERBERUS_DUAL_BLE_RECEIVE_LEGACY&&painted.receive.callback_length==3);
    assert(row.receive.source_kind==CERBERUS_DUAL_BLE_RECEIVE_EXTENDED&&row.receive.sid==12&&row.receive.callback_length==255);
    cerberus_ble_archive_row_t archived;
    assert(cerberus_dual_ble_archive_capture_row(&archived,&painted,&painted_source,now_ms));
    assert(archived.address[0]==2&&archived.receive.source_kind==CERBERUS_DUAL_BLE_RECEIVE_LEGACY&&
        archived.receive.callback_length==3&&archived.receiver_boot_id==painted_source.boot_id&&
        archived.scan_epoch==painted_source.epoch&&archived.generation==painted_source.generation&&
        archived.index==painted_source.index);
    cerberus_dual_ble_stop(&s);
    for(unsigned tick=0;tick<50U&&!cerberus_dual_ble_released(&s);++tick)integration_tick(&i,&s);
    assert(cerberus_dual_ble_released(&s)&&!dual_attached);
    /* Real EXT_DISC normalization -> immutable110 wire -> S3, with typed
     * selection surviving newer bytes/SID and a different typed identity. */
    integration_reset(&i,&s,false);i.extended=true;i.lose_begin=true;
    for(unsigned tick=0;tick<200U&&!s.snapshots;++tick)integration_tick(&i,&s);
    assert(s.snapshots==1&&starts==1&&i.metadata_reads==11&&!i.old_reads&&i.begin_calls>=2);
    assert(cerberus_dual_ble_copy_row(&s,0,&painted,&painted_source));
    assert(painted.address_type==1&&painted.receive.source_kind==CERBERUS_DUAL_BLE_RECEIVE_EXTENDED);
    assert(painted.receive.sid==7&&painted.receive.primary_phy==3&&painted.receive.secondary_phy==1);
    assert(painted.receive.callback_length==255&&painted.receive.ad_validation==CERBERUS_DUAL_BLE_AD_VALID);
    assert(painted.raw_length==31&&painted.raw_flags==3&&painted.rssi==-30);
    assert(painted.request_started_ms==i.first_begin_ms&&scan_results[0].receive.sid==12);
    integration_extended_report(1,0,-50,2); /* Same bytes, different address type. */
    for(unsigned tick=0;tick<250U&&s.snapshots<2;++tick)integration_tick(&i,&s);
    assert(s.count==2&&cerberus_dual_ble_row(&s,0,&row)&&row.receive.sid==12&&row.rssi==-90);
    assert(cerberus_dual_ble_row(&s,1,&row)&&row.address_type==0&&row.receive.sid==2);
    assert(painted.address_type==1&&painted.receive.sid==7&&painted.rssi==-30);
    assert(cerberus_dual_ble_archive_capture_row(&archived,&painted,&painted_source,now_ms));
    assert(!memcmp(&archived.receive,&painted.receive,sizeof(painted.receive))&&
        archived.receive.sid==7&&archived.address_type==1&&archived.rssi==-30&&
        archived.receiver_boot_id==painted_source.boot_id&&archived.scan_epoch==painted_source.epoch&&
        archived.generation==painted_source.generation&&archived.index==painted_source.index);
    assert(!memcmp(archived.advertisement,painted.raw,painted.raw_length)&&archived.age_upper_valid);
    cerberus_dual_ble_stop(&s);
    for(unsigned tick=0;tick<50U&&!cerberus_dual_ble_released(&s);++tick)integration_tick(&i,&s);
    assert(cerberus_dual_ble_released(&s)&&!dual_attached);
    /* Unsupported opt-in metadata, both immediately and after partial reads,
     * rereads the SAME frozen handle fromrow0. No new BEGIN/job/RF scan. */
    for(unsigned unsupported=1;unsupported<=3;unsupported+=2){
        integration_reset(&i,&s,false);i.extended=true;i.old_metadata=true;i.unsupported_at=unsupported;
        for(unsigned tick=0;tick<250U&&!s.snapshots;++tick)integration_tick(&i,&s);
        assert(s.snapshots==1&&starts==1&&i.begin_calls==1&&i.metadata_reads==unsupported&&i.old_reads==11);
        assert(cerberus_dual_ble_row(&s,0,&row)&&row.rssi==-30&&row.request_started_ms==i.first_begin_ms);
        assert(!memcmp(&row.receive,&no_metadata,sizeof(no_metadata))&&!s.metadata_read_enabled);
        assert(dual_snapshot.handle==i.fallback_handle);
        cerberus_dual_ble_stop(&s);
        for(unsigned tick=0;tick<50U&&!cerberus_dual_ble_released(&s);++tick)integration_tick(&i,&s);
        assert(cerberus_dual_ble_released(&s)&&starts==1&&stops==1);
    }
    for(unsigned stop_at=1;stop_at<=11;stop_at+=10){
        integration_reset(&i,&s,false);i.extended=true;
        for(unsigned tick=0;tick<200U&&i.metadata_reads<stop_at;++tick)integration_tick(&i,&s);
        assert(!s.snapshots&&i.metadata_reads==stop_at);
        cerberus_dual_ble_stop(&s);
        for(unsigned tick=0;tick<50U&&!cerberus_dual_ble_released(&s);++tick)integration_tick(&i,&s);
        assert(cerberus_dual_ble_released(&s)&&!s.snapshots&&!dual_snapshot.valid&&starts==1&&stops==1);
    }
    integration_reset(&i,&s,false);i.extended=true;i.bad_fence=true;
    for(unsigned tick=0;tick<250U&&!s.error;++tick){now_ms+=20;cerberus_dual_ble_tick(&s,now_ms);}
    assert(s.error&&!s.snapshots&&!s.count);
    i.bad_fence=false;cerberus_dual_ble_stop(&s);
    for(unsigned tick=0;tick<50U&&!cerberus_dual_ble_released(&s);++tick){now_ms+=20;cerberus_dual_ble_tick(&s,now_ms);}
    assert(cerberus_dual_ble_released(&s)&&!dual_attached&&!s.snapshots);
    integration_analyzer();
    integration_family();
    integration_response();
    integration_compact_response();
    puts("Actual C5 service/callback + S3 coordinator: live immutable readout, lost-BEGIN age, final 8s sweep, Stop and v1 completed-bank fallback passed");
    return 0;
}
