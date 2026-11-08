/* Actual C5 owner/GAP/snapshot producer and S3 companion reader. */
#define main owner_regressions
#include "dual_ble_owner_check.c"
#undef main
#include "dual_ble_scan.h"

static const uint8_t saved_address[6]={1,2,3,4,5,6};
static void stop_release(cerberus_dual_ble_binding_t b)
{
    cerberus_dual_ble_status_t s;
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_STOP,&b,sizeof(b),&s,sizeof(s));
    assert(!(s.flags&CERBERUS_DUAL_BLE_OWNED));
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_RELEASE,&b,sizeof(b),&s,sizeof(s));
    assert(!s.flags && !dual_attached && !dual_response);
}
static void emit(uint8_t first,uint8_t type,bool extended,int8_t rssi,uint8_t payload)
{
    uint8_t raw[]={2,1,payload};
    struct ble_gap_event event={.type=extended?BLE_GAP_EVENT_EXT_DISC:BLE_GAP_EVENT_DISC};
    if (extended) event.ext_disc=(struct ble_gap_ext_disc_desc){.addr={{first,2,3,4,5,6},type},
        .rssi=rssi,.sid=7,.prim_phy=3,.sec_phy=1,.data=raw,.length_data=sizeof(raw)};
    else event.disc=(struct ble_gap_disc_desc){.addr={{first,2,3,4,5,6},type},
        .rssi=rssi,.event_type=0,.data=raw,.length_data=sizeof(raw)};
    assert(!gap_event(&event,(void *)(uintptr_t)scan_callback_epoch));
}
static void receiver_checks(void)
{
    dual_boot_id=123;host_synced=true;now_ms=1000;
    cerberus_dual_ble_start_t r={.boot_id=123,.job_id=1,.version=2,
        .mode=CERBERUS_DUAL_BLE_REACQUIRE,.address_type=CERBERUS_DUAL_BLE_ANY_OBSERVED_TYPE};
    memcpy(r.address,saved_address,6);
    assert(cerberus_dual_ble_start_valid(&r));
    for (unsigned type=0;type<255;++type) {r.address_type=(uint8_t)type;assert(!cerberus_dual_ble_start_valid(&r));}
    r.address_type=255;r.version=1;assert(!cerberus_dual_ble_start_valid(&r));r.version=2;
    memset(r.address,0,6);assert(!cerberus_dual_ble_start_valid(&r));
    memset(r.address,255,6);assert(!cerberus_dual_ble_start_valid(&r));memcpy(r.address,saved_address,6);
    cerberus_dual_ble_status_t s;
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_START,&r,sizeof(r),&s,sizeof(s));
    assert(!s.error_code && s.epoch && starts==1 && scan_inventory.enabled && !dual_response);
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_START,&r,sizeof(r),&s,sizeof(s));assert(starts==1);
    r.address[0]++;command(CERBERUS_SPI_COMMAND_DUAL_BLE_START,&r,sizeof(r),&s,sizeof(s));
    assert(s.error_code && starts==1);r.address[0]--;
    unsigned before=parse_calls;
    for (unsigned i=0;i<200;++i) emit((uint8_t)(2U+i),i%4U,i%2U,-40,6);
    assert(!scan_status.stored_count && !scan_status.dropped_reports && !scan_inventory.dropped && parse_calls==before);
    for (uint8_t type=0;type<4;++type) {now_ms+=1;emit(1,type,type%2U,-31,6);}
    assert(scan_status.stored_count==4 && !dual_track_seen);
    now_ms+=1;emit(1,0,false,-81,7);assert(scan_results[0].rssi==-31 && scan_results[0].latest_rssi==-81);
    before=parse_calls;emit(1,255,false,-15,6);assert(parse_calls==before);
    uint8_t raw[]={2,1,8};struct ble_gap_event event={.type=BLE_GAP_EVENT_DISC,
        .disc={.addr={{1,2,3,4,5,6},0},.rssi=-10,.event_type=4,.data=raw,.length_data=3}};
    assert(!gap_event(&event,(void *)(uintptr_t)scan_callback_epoch));
    event.disc.event_type=0;assert(!gap_event(&event,(void *)(uintptr_t)(scan_callback_epoch-1U)));
    record_advertisement(&event.disc,scan_callback_epoch,-1);
    record_advertisement(&event.disc,scan_callback_epoch,999000);
    event.type=BLE_GAP_EVENT_EXT_DISC;event.ext_disc=(struct ble_gap_ext_disc_desc){
        .addr={{1,2,3,4,5,6},0},.rssi=-10,.sid=7,.prim_phy=1,.data=raw,.length_data=3,.data_status=1};
    assert(!gap_event(&event,(void *)(uintptr_t)scan_callback_epoch));
    event.ext_disc.data_status=0;event.ext_disc.props=8;
    assert(!gap_event(&event,(void *)(uintptr_t)scan_callback_epoch));
    assert(parse_calls==before && scan_results[0].latest_rssi==-81);
    cerberus_dual_ble_binding_t b={123,1};
    cerberus_dual_ble_snapshot_status_t frozen=snapshot_begin(b,1);assert(!frozen.error_code && frozen.count==4);
    cerberus_dual_ble_snapshot_metadata_row_t old=snapshot_metadata_row(b,frozen.handle,0);
    assert(old.row.address_type==0 && old.row.latest_rssi==-81 && old.row.raw[2]==7 && old.row.age_ms==0);
    assert(old.receive.source_kind==CERBERUS_DUAL_BLE_RECEIVE_LEGACY && old.receive.data_status==255);
    cerberus_dual_ble_snapshot_metadata_row_t ext=snapshot_metadata_row(b,frozen.handle,1);
    assert(ext.row.address_type==1 && ext.receive.source_kind==CERBERUS_DUAL_BLE_RECEIVE_EXTENDED && ext.receive.primary_phy==3);
    now_ms+=200;emit(1,0,false,-60,9);
    cerberus_dual_ble_snapshot_status_t retry=snapshot_begin(b,1);
    assert(retry.handle==frozen.handle);
    cerberus_dual_ble_snapshot_metadata_row_t unchanged=snapshot_metadata_row(b,frozen.handle,0);
    assert(!memcmp(&old,&unchanged,sizeof(old)));
    now_ms-=1;assert(snapshot_begin(b,2).error_code==ESP_ERR_INVALID_STATE);now_ms+=1;
    cerberus_dual_ble_binding_t foreign={123,2};
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_STOP,&foreign,sizeof(foreign),&s,sizeof(s));
    assert(s.error_code==ESP_ERR_NOT_FOUND && dual_radio_active);
    cancel_fails=true;command(CERBERUS_SPI_COMMAND_DUAL_BLE_STOP,&b,sizeof(b),&s,sizeof(s));
    assert(s.flags&CERBERUS_DUAL_BLE_OWNED);
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_RELEASE,&b,sizeof(b),&s,sizeof(s));assert(dual_attached && s.error_code);
    cancel_fails=false;stop_release(b);
    r.job_id=dual_job_highwater+1;r.mode=CERBERUS_DUAL_BLE_DISCOVERY;r.address_type=0;memset(r.address,0,6);
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_START,&r,sizeof(r),&s,sizeof(s));assert(!s.error_code);
    emit(22,1,false,-33,6);assert(scan_status.stored_count==1 && scan_results[0].address[0]==22);
    stop_release((cerberus_dual_ble_binding_t){123,r.job_id});
}

typedef struct {bool lose_start,lose_begin,metadata_unsupported,corrupt_row;unsigned starts,old_reads;} bridge_t;
static uint64_t source_clock(void *context) {(void)context;return now_ms;}
static esp_err_t exchange_reacquire(void *context,uint8_t cmd,const void *in,size_t n,void *out,size_t length)
{
    bridge_t *bridge=context;
    if (cmd==CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ) {++bridge->old_reads;assert(false);}
    if (cmd==CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_METADATA && bridge->metadata_unsupported)
        return ESP_ERR_NOT_SUPPORTED;
    size_t bytes=0;
    assert(cerberus_ble_dual_command(cmd,in,n,out,&bytes) && bytes==length);
    if (cmd==CERBERUS_SPI_COMMAND_DUAL_BLE_START) {
        ++bridge->starts;
        for (unsigned i=0;i<80;++i) emit((uint8_t)(i+2U),1,false,-20,6);
        for (unsigned i=0;i<4;++i) emit(1,(uint8_t)i,i%2U,(int8_t)(-40-(int)i),6);
        if (bridge->lose_start) {bridge->lose_start=false;now_ms+=8;return ESP_ERR_TIMEOUT;}
    }
    if (cmd==CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_BEGIN && bridge->lose_begin) {
        bridge->lose_begin=false;now_ms+=8;return ESP_ERR_TIMEOUT;
    }
    if (cmd==CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_METADATA && bridge->corrupt_row) {
        const cerberus_dual_ble_snapshot_read_t *r=in;
        if (!r->chunk) ((cerberus_dual_ble_snapshot_chunk_t *)out)->data[0]^=1U;
    }
    now_ms+=8;return ESP_OK;
}
static void drain(cerberus_dual_ble_scan_t *scan)
{
    cerberus_dual_ble_stop(scan);
    for (unsigned i=0;i<100 && !cerberus_dual_ble_released(scan);++i) {
        now_ms+=250;cerberus_dual_ble_tick(scan,now_ms);
    }
    assert(cerberus_dual_ble_released(scan) && !dual_attached);
}
static void reader_checks(void)
{
    bridge_t bridge={.lose_start=true,.lose_begin=true};
    cerberus_dual_ble_ops_t ops={&bridge,exchange_reacquire,source_clock};
    cerberus_dual_ble_scan_t scan;cerberus_dual_ble_init(&scan,&ops);
    cerberus_dual_ble_row_t row;cerberus_dual_ble_provenance_t proof;
    assert(cerberus_dual_ble_begin_reacquire(&scan,saved_address)==ESP_OK);
    assert(!cerberus_dual_ble_reacquire_row(&scan,0,saved_address,&row,&proof));
    unsigned before=starts;
    for (unsigned i=0;i<200 && !scan.snapshots;++i) {now_ms+=40;cerberus_dual_ble_tick(&scan,now_ms);}
    assert(scan.snapshots && scan.count==4 && starts==before+1 && bridge.starts==2 && !bridge.old_reads);
    for (uint8_t type=0;type<4;++type) {
        assert(cerberus_dual_ble_reacquire_row(&scan,type,saved_address,&row,&proof));
        assert(row.address_type==type && proof.boot_id==123 && proof.epoch && row.event_type<=3 && row.response.version==0);
    }
    uint8_t wrong[6]={2,2,3,4,5,6};
    assert(!cerberus_dual_ble_reacquire_row(&scan,0,wrong,&row,&proof) && !proof.epoch);
    drain(&scan);
    assert(cerberus_dual_ble_reacquire_row(&scan,0,saved_address,&row,&proof)); /* Retained current lookup evidence. */
    assert(cerberus_dual_ble_begin_reacquire(&scan,saved_address)==ESP_OK);
    assert(!cerberus_dual_ble_reacquire_row(&scan,0,saved_address,&row,&proof)); /* New lookup cannot reuse it. */
    drain(&scan);
    for (unsigned fault=0;fault<2;++fault) {
        bridge=(bridge_t){.metadata_unsupported=fault==0,.corrupt_row=fault==1};
        assert(cerberus_dual_ble_begin_reacquire(&scan,saved_address)==ESP_OK);
        for (unsigned i=0;i<200 && !scan.error;++i) {now_ms+=50;cerberus_dual_ble_tick(&scan,now_ms);}
        assert(scan.error && !scan.snapshots && !bridge.old_reads);
        assert(!cerberus_dual_ble_reacquire_row(&scan,0,saved_address,&row,&proof));drain(&scan);
    }
    assert(!response_allocations);
}
int main(void)
{
    receiver_checks();reader_checks();
    puts("Passive saved-address reacquisition: actual C5 GAP/owner/snapshots and S3 reader, typed filters, capacity, provenance, clocks, retries and cleanup passed.");
    return 0;
}
