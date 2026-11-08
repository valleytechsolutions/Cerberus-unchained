#include "dual_ble_scan.h"
#include "ble_family_wire.h"
#include "dual_ble_family.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint64_t now, boot, epoch;
    uint32_t job;
    unsigned starts, stops, releases, frames, track_reads;
    uint8_t count, generation, state, mode;
    bool attached, owned, lose_start, fail_stop, corrupt_fence, corrupt_data;
    bool unsupported, reset, busy;
    uint8_t version;
    bool snapshot_capability;
    uint8_t status_add_flags, status_remove_flags;
    unsigned cost_ms, snapshot_begins, snapshot_reads, snapshot_fences, lose_begin_replies;
    uint64_t request_id, snapshot_clock_ms, last_begin_ms, handle_counter, last_fence_ms;
    unsigned complete_fences;
    cerberus_dual_ble_snapshot_status_t snapshot;
    cerberus_dual_ble_snapshot_row_t frozen[CERBERUS_DUAL_BLE_CAPACITY];
    cerberus_dual_ble_receive_metadata_t frozen_receive[CERBERUS_DUAL_BLE_CAPACITY];
    cerberus_dual_ble_family_wire_t frozen_family[CERBERUS_DUAL_BLE_CAPACITY];
    cerberus_dual_ble_response_wire_t frozen_response[CERBERUS_DUAL_BLE_CAPACITY];
    bool response_supported, response_read_unsupported;
    unsigned response_reads, response_fault;
    bool compact_supported, compact_fail_enabled;
    unsigned compact_reads, compact_fail_after, compact_fault;
    bool family_supported, family_read_unsupported;
    unsigned family_reads, family_fault;
    uint8_t family_code;
    bool metadata_supported, metadata_fail_enabled;
    unsigned metadata_reads, metadata_fail_after;
    unsigned fault, busy_begin, busy_read;
    bool unknown_age;
    bool inventory_supported;
    unsigned inventory_queries, inventory_timeouts, inventory_fault;
    uint32_t replacements, dropped;
    cerberus_dual_ble_snapshot_inventory_t frozen_inventory;
    bool analyzer_supported, analyzer_pending;
    unsigned analyzer_reads, analyzer_busy, analyzer_timeouts, analyzer_fault;
    uint32_t analyzer_reports, analyzer_elapsed;
} peer_t;
enum {
    BAD_BEGIN_BOOT=1, BAD_BEGIN_ZERO_HANDLE, BAD_BEGIN_COUNT, BAD_BEGIN_FLAGS,
    BAD_BEGIN_UNKNOWN_FLAG, BAD_BEGIN_EXTENDED,
    BAD_READ_HANDLE, BAD_READ_INDEX, BAD_READ_CHUNK, BAD_READ_LENGTH,
    BAD_READ_ERROR, BAD_READ_ERROR_BODY, BAD_READ_TAIL,
    BAD_ROW_TYPE, BAD_ROW_NAME_LENGTH, BAD_ROW_UUID_LENGTH, BAD_ROW_RAW_LENGTH,
    BAD_ROW_EVENT, BAD_ROW_PEER_FLAGS, BAD_ROW_RAW_FLAGS, BAD_ROW_TIME_FLAGS,
    BAD_ROW_RSSI, BAD_ROW_NAME_PADDING, BAD_ROW_UUID_PADDING, BAD_ROW_RAW_PADDING,
    BAD_ROW_DUPLICATE, BAD_FENCE_HANDLE, BAD_FENCE_BOOT, BAD_FENCE_COUNT, BAD_FENCE_FLAGS,
    FAULT_END
};
static uint64_t clock_ms(void *ctx) { return ((peer_t *)ctx)->now; }
static cerberus_dual_ble_status_t status(peer_t *p)
{
    return (cerberus_dual_ble_status_t){ .epoch=p->epoch, .job_id=p->job,
        .state=p->state, .generation=p->generation, .count=p->count,
        .flags=((p->attached ? CERBERUS_DUAL_BLE_ATTACHED : 0) |
            (p->owned ? CERBERUS_DUAL_BLE_OWNED : 0) |
            (p->attached ? CERBERUS_DUAL_BLE_EXTENDED : 0) |
            (p->attached && p->snapshot_capability ? CERBERUS_DUAL_BLE_SNAPSHOTS : 0) |
            p->status_add_flags) & (uint8_t)~p->status_remove_flags, .version=p->version };
}
static void snapshot_freeze(peer_t *p)
{
    p->snapshot=(cerberus_dual_ble_snapshot_status_t){.boot_id=p->boot,.handle=++p->handle_counter,
        .count=p->count,.flags=(p->owned?CERBERUS_DUAL_BLE_SNAPSHOT_RUNNING:CERBERUS_DUAL_BLE_SNAPSHOT_COMPLETE)|
            CERBERUS_DUAL_BLE_SNAPSHOT_EXTENDED};
    p->snapshot_clock_ms=p->now;
    p->frozen_inventory=(cerberus_dual_ble_snapshot_inventory_t){.handle=p->snapshot.handle,
        .replaced=p->replacements,.dropped=p->dropped,.count=p->count,
        .flags=CERBERUS_DUAL_BLE_INVENTORY_REPLACEMENT};
    memset(p->frozen,0,sizeof(p->frozen));
    memset(p->frozen_receive,0,sizeof(p->frozen_receive));
    memset(p->frozen_family,0,sizeof(p->frozen_family));
    memset(p->frozen_response,0,sizeof(p->frozen_response));
    for (uint8_t i=0;i<p->count;++i) {
        cerberus_dual_ble_snapshot_row_t *r=&p->frozen[i];
        memcpy(r->address,(uint8_t[]){1,2,3,4,5,(uint8_t)(i/4U)},6);
        r->address_type=i%4U;r->latest_rssi=-85;r->name_length=4;memcpy(r->name,"SNAP",4);
        r->uuid_length=2;r->uuid[0]=0x0d;r->uuid[1]=0x18;r->manufacturer_id=0x004c;
        r->peer_flags=CERBERUS_BLE_RESULT_CONNECTABLE;r->raw_length=3;memcpy(r->raw,(uint8_t[]){2,1,6},3);
        r->reports=100U+p->snapshot_begins;r->age_ms=p->unknown_age?UINT32_MAX:17;
        r->timing_flags=p->unknown_age?0:3;
        p->frozen_receive[i]=(cerberus_dual_ble_receive_metadata_t){
            .source_kind=CERBERUS_DUAL_BLE_RECEIVE_LEGACY,.callback_length=3,
            .sid=0xff,.data_status=0xff,.ad_validation=CERBERUS_DUAL_BLE_AD_VALID};
        if(cerberus_dual_ble_family_mode(p->mode)) {
            cerberus_dual_ble_family_evidence_t evidence;
            assert(cerberus_dual_ble_family_parse_ad(r->raw,r->raw_length,r->address,r->address_type,&evidence));
            assert(cerberus_dual_ble_family_wire_encode(&evidence,p->family_code,&p->frozen_family[i]));
        }
        if(cerberus_dual_ble_response_mode(p->mode)) {
            cerberus_dual_ble_response_wire_t *response=&p->frozen_response[i];
            *response=(cerberus_dual_ble_response_wire_t){.version=1,.state=CERBERUS_DUAL_BLE_RESPONSE_PRESENT,
                .flags=CERBERUS_DUAL_BLE_RESPONSE_AGE_VALID|CERBERUS_DUAL_BLE_RESPONSE_RSSI_VALID|
                    CERBERUS_DUAL_BLE_RESPONSE_NAME_COMPLETE,
                .raw_length=7,.event_type=4,.name_length=5,.rssi=-49,
                .original_sequence=UINT64_C(0x100000001),.response_sequence=UINT64_C(0x100000002),
                .age_ms=9,.interval_us=8000,
                .receive={.source_kind=CERBERUS_DUAL_BLE_RECEIVE_LEGACY,.callback_length=7,
                    .sid=0xff,.data_status=0xff,.ad_validation=CERBERUS_DUAL_BLE_AD_VALID},
                .raw={6,9,'H','C','-','0','5'},.name={'H','C','-','0','5'}};
            cerberus_dual_ble_family_evidence_t evidence;
            assert(cerberus_dual_ble_family_parse_ad(response->raw,response->raw_length,r->address,r->address_type,&evidence));
            assert(cerberus_dual_ble_response_family_encode(&evidence,p->family_code,&response->family));
        }
    }
    cerberus_dual_ble_snapshot_row_t *r=&p->frozen[0];
    switch(p->fault) {
    case BAD_ROW_TYPE:r->address_type=4;break;
    case BAD_ROW_NAME_LENGTH:r->name_length=33;break;
    case BAD_ROW_UUID_LENGTH:r->uuid_length=3;break;
    case BAD_ROW_RAW_LENGTH:r->raw_length=32;break;
    case BAD_ROW_EVENT:r->event_type=5;break;
    case BAD_ROW_PEER_FLAGS:r->peer_flags=0x80;break;
    case BAD_ROW_RAW_FLAGS:r->raw_flags=0x80;break;
    case BAD_ROW_TIME_FLAGS:r->timing_flags=0x80;break;
    case BAD_ROW_RSSI:r->latest_rssi=21;break;
    case BAD_ROW_NAME_PADDING:r->name[31]=1;break;
    case BAD_ROW_UUID_PADDING:r->uuid[15]=1;break;
    case BAD_ROW_RAW_PADDING:r->raw[30]=1;break;
    case BAD_ROW_DUPLICATE:p->frozen[1]=*r;break;
    default:break;
    }
    switch(p->family_fault) {
    case 1:p->frozen_family[0].family_code=p->family_code==6?1:p->family_code+1;break;
    case 2:p->frozen_family[0].version=2;break;
    case 3:p->frozen_family[0].flags|=0x80;break;
    case 4:p->frozen_family[0].reserved[0]=1;break;
    case 5:memset(&p->frozen_family[0],0,sizeof(p->frozen_family[0]));break;
    case 6:p->frozen_family[0].categories=0x80;break;
    case 7:p->frozen_family[0].ibeacon_uuid[15]=1;break;
    default:break;
    }
    cerberus_dual_ble_response_wire_t *response=&p->frozen_response[0];
    switch(p->response_fault) {
    case 1:response->version=2;break;
    case 2:response->state=3;break;
    case 3:response->flags|=0x80;break;
    case 4:response->reserved_tail=1;break;
    case 5:response->original_sequence=0;break;
    case 6:response->response_sequence=response->original_sequence;break;
    case 7:response->interval_us=CERBERUS_DUAL_BLE_RESPONSE_INTERVAL_US+1U;break;
    case 8:response->age_ms=UINT32_MAX;break;
    case 9:response->rssi=21;break;
    case 10:response->name_length=33;break;
    case 11:response->raw_length=32;break;
    case 12:response->raw[30]=1;break;
    case 13:response->name[31]=1;break;
    case 14:response->receive.ad_validation=0;break;
    case 15:response->receive.sid=1;break;
    case 16:response->receive.source_kind=0;break;
    case 17:response->event_type=0;break;
    case 18:response->raw_flags=1;break;
    case 19:response->family.family_code=p->family_code==6?1:6;break;
    case 20:response->family.version=0;break;
    case 21:response->family.ibeacon_uuid[0]=1;break;
    case 22:p->frozen_receive[0].ad_validation=2;break;
    case 23:r->event_type=3;break;
    case 24:response->state=CERBERUS_DUAL_BLE_RESPONSE_UNKNOWN;break;
    case 25:response->state=CERBERUS_DUAL_BLE_RESPONSE_NONE;break;
    case 26:response->age_ms=r->age_ms+1U;break;
    case 27:response->interval_us=10000U;break;
    default:break;
    }
}
static esp_err_t snapshot_exchange(peer_t *p,uint8_t cmd,const void *request,
    size_t request_size,void *reply,size_t reply_size)
{
    assert(p->version==2 && p->snapshot_capability && reply_size==22);
    const cerberus_dual_ble_binding_t *b=request;
    assert(b->boot_id==p->boot && b->job_id==p->job && p->attached);
    if(cmd==CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ ||
        cmd==CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_METADATA ||
        cmd==CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_FAMILY ||
        cmd==CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_RESPONSE ||
        cmd==CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_RESPONSE_COMPACT) {
        bool family=cmd==CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_FAMILY;
        bool compact=cmd==CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_RESPONSE_COMPACT;
        bool response=compact||cmd==CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_RESPONSE;
        bool metadata=cmd==CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_METADATA;
        assert(request_size==22);
        if(compact) {
            ++p->compact_reads;
            if(!p->compact_supported||(p->compact_fail_enabled&&p->compact_reads>p->compact_fail_after))
                return ESP_ERR_NOT_SUPPORTED;
            if(p->compact_fault==12)return ESP_ERR_TIMEOUT;
        }
        else if(response) {++p->response_reads;if(p->response_read_unsupported)return ESP_ERR_NOT_SUPPORTED;}
        else if(family) {++p->family_reads;if(p->family_read_unsupported)return ESP_ERR_NOT_SUPPORTED;}
        else if(metadata) {
            ++p->metadata_reads;
            if(!p->metadata_supported || (p->metadata_fail_enabled&&p->metadata_reads>p->metadata_fail_after))
                return ESP_ERR_NOT_SUPPORTED;
        } else ++p->snapshot_reads;
        const cerberus_dual_ble_snapshot_read_t *r=request;
        assert(r->handle==p->snapshot.handle&&r->index<p->snapshot.count&&r->chunk<(response?31:family?16:11));
        cerberus_dual_ble_snapshot_chunk_t out={.handle=r->handle,.index=r->index,.chunk=r->chunk};
        if(p->busy_read) {--p->busy_read;out.error=CERBERUS_DUAL_BLE_SNAPSHOT_BUSY;}
        else {
            cerberus_dual_ble_snapshot_response_row_t wire={0};
            memcpy(&wire,&p->frozen[r->index],sizeof(p->frozen[r->index]));
            memcpy((uint8_t*)&wire+102,&p->frozen_receive[r->index],sizeof(p->frozen_receive[r->index]));
            wire.family.family=p->frozen_family[r->index];
            wire.response=p->frozen_response[r->index];
            size_t length=response?sizeof(wire):family?158U:metadata?110U:102U;
            if(compact) {
                if(wire.response.state==CERBERUS_DUAL_BLE_RESPONSE_UNKNOWN) {
                    length=160U;wire.response.version=CERBERUS_DUAL_BLE_COMPACT_RESPONSE_VERSION;
                } else if(wire.response.state==CERBERUS_DUAL_BLE_RESPONSE_NONE)length=174U;
                if(p->compact_fault==1)wire.response.version=2;
                else if(p->compact_fault==2)wire.response.state=3;
            }
            size_t offset=r->chunk*10U;assert(offset<length);
            size_t amount=length-offset;
            if(amount>10U)amount=10U;out.byte_count=(uint8_t)amount;
            memcpy(out.data,(const uint8_t *)&wire+offset,amount);
            switch(p->fault) {
            case BAD_READ_HANDLE:--out.handle;break;
            case BAD_READ_INDEX:++out.index;break;
            case BAD_READ_CHUNK:++out.chunk;break;
            case BAD_READ_LENGTH:--out.byte_count;break;
            case BAD_READ_ERROR:out.error=5;break;
            case BAD_READ_ERROR_BODY:out.error=CERBERUS_DUAL_BLE_SNAPSHOT_BUSY;break;
            case BAD_READ_TAIL:if(r->chunk==(response?30:family?15:10))out.data[9]=1;break;
            default:break;
            }
            if(compact) {
                if(p->compact_fault==3&&offset==150U)--out.byte_count;
                else if(p->compact_fault==4&&offset+amount==length)++out.byte_count;
                else if(p->compact_fault==5&&length==174U&&offset==170U)out.data[4]=1;
                else if(p->compact_fault==6&&offset==150U)++out.handle;
                else if(p->compact_fault==7&&offset==150U)++out.index;
                else if(p->compact_fault==8&&offset==150U)++out.chunk;
                else if(p->compact_fault==9||p->compact_fault==10) {
                    out.error=p->compact_fault==9?CERBERUS_DUAL_BLE_SNAPSHOT_STALE:CERBERUS_DUAL_BLE_SNAPSHOT_BUSY;
                    out.byte_count=0;memset(out.data,0,sizeof(out.data));
                } else if(p->compact_fault==11)out.error=CERBERUS_DUAL_BLE_SNAPSHOT_INVALID+1U;
            }
        }
        memcpy(reply,&out,sizeof(out));return ESP_OK;
    }
    cerberus_dual_ble_snapshot_status_t out;
    if(cmd==CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_BEGIN) {
        assert(request_size==20);++p->snapshot_begins;
        const cerberus_dual_ble_snapshot_begin_t *r=request;
        if(p->busy_begin) {
            --p->busy_begin;out=(cerberus_dual_ble_snapshot_status_t){.boot_id=p->boot,.error_code=ESP_ERR_TIMEOUT};
            memcpy(reply,&out,sizeof(out));return ESP_OK;
        }
        if(r->request_id!=p->request_id) {
            assert(r->request_id==p->request_id+1U);p->request_id=r->request_id;p->last_begin_ms=p->now;
            snapshot_freeze(p);
        }
        if(p->lose_begin_replies) {--p->lose_begin_replies;return ESP_ERR_TIMEOUT;}
        out=p->snapshot;
        switch(p->fault) {
        case BAD_BEGIN_BOOT:++out.boot_id;break;
        case BAD_BEGIN_ZERO_HANDLE:out.handle=0;break;
        case BAD_BEGIN_COUNT:out.count=33;break;
        case BAD_BEGIN_FLAGS:out.flags|=3;break;
        case BAD_BEGIN_UNKNOWN_FLAG:out.flags|=0x80;break;
        case BAD_BEGIN_EXTENDED:out.flags&=~CERBERUS_DUAL_BLE_SNAPSHOT_EXTENDED;break;
        default:break;
        }
    } else {
        assert(cmd==CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_STATUS&&request_size==20);++p->snapshot_fences;
        const cerberus_dual_ble_snapshot_binding_t *r=request;assert(r->handle==p->snapshot.handle);
        out=p->snapshot;
        p->last_fence_ms=p->now;
        if(out.flags&CERBERUS_DUAL_BLE_SNAPSHOT_COMPLETE)++p->complete_fences;
        switch(p->fault) {
        case BAD_FENCE_HANDLE:++out.handle;break;
        case BAD_FENCE_BOOT:++out.boot_id;break;
        case BAD_FENCE_COUNT:++out.count;break;
        case BAD_FENCE_FLAGS:out.flags^=3;break;
        default:break;
        }
    }
    memcpy(reply,&out,sizeof(out));return ESP_OK;
}
static esp_err_t exchange(void *ctx, uint8_t cmd, const void *request,
    size_t request_size, void *reply, size_t reply_size)
{
    peer_t *p=ctx; ++p->frames; p->now+=p->cost_ms;
    if(cmd==CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_INVENTORY) {
        ++p->inventory_queries;
        if(!p->inventory_supported)return ESP_ERR_NOT_SUPPORTED;
        assert(request_size==20&&reply_size==22);
        const cerberus_dual_ble_snapshot_binding_t *r=request;
        assert(r->binding.boot_id==p->boot&&r->binding.job_id==p->job&&r->handle==p->snapshot.handle&&p->attached);
        if(p->inventory_timeouts){--p->inventory_timeouts;return ESP_ERR_TIMEOUT;}
        cerberus_dual_ble_snapshot_inventory_t out=p->frozen_inventory;
        switch(p->inventory_fault) {
        case 1:--out.handle;break;
        case 2:++out.count;break;
        case 3:out.flags=0x80;break;
        case 4:out.error_code=ESP_ERR_NOT_FOUND;break;
        case 5:out.flags=0;out.replaced=1;break;
        default:break;
        }
        memcpy(reply,&out,sizeof(out));return ESP_OK;
    }
    if((cmd>=CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_BEGIN&&cmd<=CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_STATUS) ||
        cmd==CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_METADATA ||
        cmd==CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_FAMILY ||
        cmd==CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_RESPONSE ||
        cmd==CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_RESPONSE_COMPACT)
        return snapshot_exchange(p,cmd,request,request_size,reply,reply_size);
    if (cmd == CERBERUS_SPI_COMMAND_DUAL_BLE_INFO) {
        assert(!request_size && reply_size==sizeof(cerberus_dual_ble_info_t));
        if (p->unsupported) return ESP_ERR_NOT_SUPPORTED;
        cerberus_dual_ble_info_t r={.boot_id=p->boot,.next_job_id=p->starts+1U,.version=p->version,.capacity=32,.raw_bytes=31,
            .flags=CERBERUS_DUAL_BLE_READY | CERBERUS_DUAL_BLE_EXTENDED |
                (p->snapshot_capability?CERBERUS_DUAL_BLE_SNAPSHOTS:0) |
                (p->attached || p->busy ? CERBERUS_DUAL_BLE_ATTACHED : 0)};
        memcpy(reply,&r,sizeof(r)); return ESP_OK;
    }
    if (cmd == CERBERUS_SPI_COMMAND_DUAL_BLE_START) {
        assert(request_size==22 && reply_size==22);
        const cerberus_dual_ble_start_t *r=request;
        assert(cerberus_dual_ble_start_valid(r)); assert(r->boot_id==p->boot);
        if(r->mode==CERBERUS_DUAL_BLE_ANALYZER&&!p->analyzer_supported)return ESP_ERR_NOT_SUPPORTED;
        if(r->mode==CERBERUS_DUAL_BLE_FAMILY&&!p->family_supported)return ESP_ERR_NOT_SUPPORTED;
        if(cerberus_dual_ble_response_mode(r->mode)&&!p->response_supported)return ESP_ERR_NOT_SUPPORTED;
        if (!p->attached) { ++p->starts; ++p->epoch; ++p->generation;
            p->attached=true; p->owned=true; p->state=CERBERUS_BLE_SCAN_RUNNING;
            p->job=r->job_id; p->mode=r->mode;p->family_code=r->reserved;
            p->request_id=0;memset(&p->snapshot,0,sizeof(p->snapshot)); }
        assert(r->version==p->version);
        assert(p->job==r->job_id);
        if (p->lose_start) return ESP_ERR_TIMEOUT;
        cerberus_dual_ble_status_t result=status(p); memcpy(reply,&result,sizeof(result)); return ESP_OK;
    }
    if (cmd == CERBERUS_SPI_COMMAND_BLE_SCAN_RESULT) {
        const cerberus_ble_scan_result_request_t *r=request;
        assert(r->generation==p->generation && r->index<p->count && !p->owned);
        cerberus_ble_scan_result_payload_t result={.generation=r->generation,.index=r->index,
            .address={1,2,3,4,5,6}, .address_type=r->index, .name_length=4,
            .advertisement_length=3,.advertisement_event_type=0,.reports_seen=7,.rssi=-20};
        memcpy(reply,&result,sizeof(result)); return ESP_OK;
    }
    if (cmd == CERBERUS_SPI_COMMAND_BLE_SCAN_DATA) {
        const cerberus_ble_scan_data_request_t *r=request;
        cerberus_ble_scan_data_payload_t result={.generation=r->generation,.index=r->index,
            .kind=r->kind,.chunk=r->chunk};
        if (r->kind==CERBERUS_BLE_DATA_NAME) { result.byte_count=4; memcpy(result.bytes,"TEST",4); }
        else { assert(r->kind==CERBERUS_BLE_DATA_RAW_ADVERTISEMENT); result.byte_count=3;
            result.bytes[0]=2;result.bytes[1]=1;result.bytes[2]=6; }
        if (p->corrupt_data) ++result.generation;
        memcpy(reply,&result,sizeof(result)); return ESP_OK;
    }
    const cerberus_dual_ble_binding_t *b=request;
    assert(request_size>=sizeof(*b));
    if(cmd==CERBERUS_SPI_COMMAND_DUAL_BLE_ANALYZER_SAMPLE){
        assert(request_size==12&&reply_size==22&&p->mode==CERBERUS_DUAL_BLE_ANALYZER);
        ++p->analyzer_reads;
        if(p->analyzer_timeouts){--p->analyzer_timeouts;return ESP_ERR_TIMEOUT;}
        cerberus_dual_ble_analyzer_sample_t out={.epoch=p->epoch,.job_id=p->job,
            .reports=p->analyzer_reports,.elapsed_ms=p->analyzer_elapsed,.rssi=127,
            .flags=CERBERUS_DUAL_BLE_ANALYZER_VALID|(p->owned?0:CERBERUS_DUAL_BLE_ANALYZER_FINAL)};
        if(p->analyzer_pending){out.flags=0;out.reports=out.elapsed_ms=0;}
        if(p->analyzer_busy){--p->analyzer_busy;out=(cerberus_dual_ble_analyzer_sample_t){
            .job_id=p->job,.flags=CERBERUS_DUAL_BLE_ANALYZER_BUSY,.rssi=127};}
        switch(p->analyzer_fault){
        case 1:++out.job_id;break;
        case 2:++out.epoch;break;
        case 3:out.flags|=0x80;break;
        case 4:out.rssi=-40;break;
        case 5:out.flags|=CERBERUS_DUAL_BLE_ANALYZER_BUSY;break;
        case 6:out.flags=0;break;
        case 7:out.flags=CERBERUS_DUAL_BLE_ANALYZER_CLOCK_INVALID;out.reports=out.elapsed_ms=0;break;
        case 8:out.flags|=CERBERUS_DUAL_BLE_ANALYZER_SATURATED;break;
        case 9:out.flags|=CERBERUS_DUAL_BLE_ANALYZER_RSSI_VALID;out.rssi=21;break;
        case 10:out.flags=CERBERUS_DUAL_BLE_ANALYZER_BUSY;out.epoch=0;out.reports=out.elapsed_ms=0;out.rssi=0;break;
        case 11:out.epoch=0;out.flags=0;out.reports=out.elapsed_ms=0;break;
        case 12:out.flags|=CERBERUS_DUAL_BLE_ANALYZER_RSSI_VALID;out.rssi=-50;out.reports=0;break;
        case 13:out.flags|=CERBERUS_DUAL_BLE_ANALYZER_FINAL;break;
        case 14:out.reports=UINT32_MAX;break;
        }
        memcpy(reply,&out,sizeof(out));return ESP_OK;
    }
    if (p->reset || b->boot_id!=p->boot || b->job_id!=p->job || !p->attached) {
        cerberus_dual_ble_status_t absent={.job_id=b->job_id,.state=CERBERUS_BLE_SCAN_ERROR,
            .error_code=ESP_ERR_NOT_FOUND,.version=p->version};
        memcpy(reply,&absent,sizeof(absent)); p->attached=false; p->owned=false; return ESP_OK;
    }
    if (cmd == CERBERUS_SPI_COMMAND_DUAL_BLE_ROW_TIME) {
        const cerberus_dual_ble_row_request_t *r=request;
        cerberus_dual_ble_row_time_t stamp={.epoch=p->epoch,.job_id=p->job,.age_ms=7000,
            .index=r->index,.generation=p->generation,.latest_rssi=-80,.flags=3,.reports=7};
        memcpy(reply,&stamp,sizeof(stamp)); return ESP_OK;
    }
    if (cmd == CERBERUS_SPI_COMMAND_DUAL_BLE_TRACK_STATUS) {
        ++p->track_reads;
        cerberus_dual_ble_track_t result={.epoch=p->epoch,.job_id=p->job,.age_ms=40,
            .reports=p->track_reads,.rssi=-75,.flags=3};
        memcpy(reply,&result,sizeof(result)); return ESP_OK;
    }
    if (cmd == CERBERUS_SPI_COMMAND_DUAL_BLE_STOP) {
        ++p->stops;
        if (p->fail_stop) return ESP_ERR_TIMEOUT;
        p->owned=false; p->state=CERBERUS_BLE_SCAN_IDLE;
    } else if (cmd == CERBERUS_SPI_COMMAND_DUAL_BLE_RELEASE) {
        ++p->releases; assert(!p->owned); p->attached=false;p->state=CERBERUS_BLE_SCAN_IDLE;
    } else assert(cmd==CERBERUS_SPI_COMMAND_DUAL_BLE_STATUS);
    cerberus_dual_ble_status_t r=status(p);
    if (p->corrupt_fence && !p->owned && cmd==CERBERUS_SPI_COMMAND_DUAL_BLE_STATUS) ++r.epoch;
    memcpy(reply,&r,sizeof(r)); return ESP_OK;
}
static void init(cerberus_dual_ble_scan_t *s, peer_t *p)
{
    memset(p,0,sizeof(*p));p->boot=123;p->now=100;p->count=2;p->version=1;p->cost_ms=1;
    p->analyzer_supported=true;
    cerberus_dual_ble_ops_t ops={p,exchange,clock_ms};cerberus_dual_ble_init(s,&ops);
}
static void tick(cerberus_dual_ble_scan_t *s, peer_t *p)
{ unsigned before=p->frames;cerberus_dual_ble_tick(s,p->now);assert(p->frames-before<=4);p->now+=100; }
static void start(cerberus_dual_ble_scan_t *s, peer_t *p)
{ assert(cerberus_dual_ble_begin(s,0,NULL,0)==ESP_OK);tick(s,p);assert(p->starts==1); }
static void completed(peer_t *p)
{ p->owned=false;p->state=CERBERUS_BLE_SCAN_COMPLETE; }
static void drain(cerberus_dual_ble_scan_t *s, peer_t *p)
{
    cerberus_dual_ble_stop(s);
    for (unsigned i=0;i<40 && !cerberus_dual_ble_released(s);++i)tick(s,p);
    assert(cerberus_dual_ble_released(s));assert(!p->owned && !p->attached);
}
static void init_live(cerberus_dual_ble_scan_t *s,peer_t *p)
{init(s,p);p->version=2;p->snapshot_capability=true;p->inventory_supported=true;}
static unsigned fast_tick(cerberus_dual_ble_scan_t *s,peer_t *p)
{
    unsigned before=p->frames;cerberus_dual_ble_tick(s,p->now);
    unsigned used=p->frames-before;assert(used<=4);p->now+=20;return used;
}
static void await_snapshots(cerberus_dual_ble_scan_t *s,peer_t *p,unsigned count)
{
    for(unsigned i=0;i<2000&&s->snapshots<count&&!s->error;++i)fast_tick(s,p);
    assert(!s->error&&s->snapshots==count);
}
static void check_live_snapshots(void)
{
    cerberus_dual_ble_scan_t s;peer_t p;
    init_live(&s,&p);p.count=32;
    s.count=1;s.rows[0].rssi=-101; /* Retained previous discovery is not first results. */
    start(&s,&p);assert(!s.has_first_result&&!s.snapshots&&s.count==1&&s.live_updates);
    unsigned peak_calls=0;
    for(unsigned i=0;i<1000&&!s.snapshots;++i) {
        unsigned used=fast_tick(&s,&p);if(used>peak_calls)peak_calls=used;
        if(!s.snapshots)assert(s.count==1&&s.rows[0].rssi==-101&&!s.has_first_result);
    }
    assert(s.snapshots==1&&s.count==32&&s.sweeps==0&&p.owned&&p.attached&&peak_calls==4);
    assert(p.snapshot_reads==32U*11U&&p.snapshot_fences==1&&p.snapshot_begins==1);
    assert(s.has_first_result&&s.first_result_ms<8000&&s.first_result_ms>500);
    assert(s.first_result_ms==p.last_fence_ms-s.discovery_started_ms);
    assert(s.rows[0].rssi==-85&&!strcmp(s.rows[0].name,"SNAP")&&s.rows[0].raw[2]==6);
    assert(s.rows[0].reports==101&&s.rows[0].manufacturer_id==0x004c&&s.rows[0].uuid_length==2);
    for(unsigned i=0;i<32;++i) {
        assert(s.rows[i].request_started_ms==s.snapshot_request_started_ms);
        assert(cerberus_dual_ble_age_upper(s.rows[i].callback_age_ms,s.rows[i].request_started_ms,p.now)>=17);
    }
    uint64_t due=s.next_snapshot_ms;
    unsigned begins=p.snapshot_begins;
    while(p.now+4<due) {fast_tick(&s,&p);assert(p.snapshot_begins==begins);}
    await_snapshots(&s,&p,2);assert(p.last_begin_ms>=due&&!s.sweeps&&p.owned);
    uint32_t first=s.first_result_ms;uint64_t anchor=s.rows[0].request_started_ms;
    /* RF can finish halfway through a frozen RUNNING download. That snapshot
       stays coherent; a distinct COMPLETE snapshot is mandatory before release. */
    for(unsigned i=0;i<1000&&p.request_id<3;++i)fast_tick(&s,&p);
    assert(p.request_id==3);completed(&p);
    await_snapshots(&s,&p,3);assert(!s.sweeps&&s.first_result_ms==first&&s.rows[0].request_started_ms>anchor);
    for(unsigned i=0;i<1000&&!s.sweeps;++i)fast_tick(&s,&p);
    assert(s.sweeps==1&&s.snapshots==4&&p.complete_fences==1);
    assert(s.first_result_ms==first);drain(&s,&p);

    /* Lost BEGIN replies retain one frozen bank and FIRST request anchor. */
    init_live(&s,&p);p.lose_begin_replies=2;start(&s,&p);await_snapshots(&s,&p,1);
    assert(p.snapshot_begins==3&&p.handle_counter==1&&p.request_id==1);
    assert(s.snapshot_request_started_ms+1==p.snapshot_clock_ms);
    assert(s.rows[0].request_started_ms+200<=p.now);
    assert(s.rows[0].reports==101);drain(&s,&p);

    /* Busy operations end this tick and retry the same bound operation. */
    init_live(&s,&p);p.busy_begin=1;p.busy_read=1;start(&s,&p);
    await_snapshots(&s,&p,1);assert(p.snapshot_begins==2&&p.handle_counter==1);drain(&s,&p);
    init_live(&s,&p);start(&s,&p);p.cost_ms=9;
    for(unsigned i=0;i<300&&!s.snapshots;++i)assert(fast_tick(&s,&p)<=1);
    assert(s.snapshots==1);drain(&s,&p);

    /* Empty is a publication, not first results; new nonempty publication
       is measured from public begin even across an internal release/rearm. */
    init_live(&s,&p);p.count=0;start(&s,&p);await_snapshots(&s,&p,1);
    assert(!s.has_first_result&&!s.first_result_ms&&!s.count&&!s.sweeps);
    completed(&p);for(unsigned i=0;i<50&&!s.sweeps;++i)fast_tick(&s,&p);
    assert(s.sweeps==1&&s.snapshots==2&&!s.has_first_result);
    p.count=1;await_snapshots(&s,&p,3);assert(s.has_first_result&&s.first_result_ms>500&&s.sweeps==1);
    completed(&p);for(unsigned i=0;i<50&&s.sweeps<2;++i)fast_tick(&s,&p);
    assert(s.sweeps==2&&s.snapshots==4);
    first=s.first_result_ms;drain(&s,&p);
    assert(cerberus_dual_ble_begin(&s,0,NULL,0)==ESP_OK);
    assert(!s.has_first_result&&!s.first_result_ms&&!s.snapshots&&!s.sweeps&&s.count==1);
    await_snapshots(&s,&p,1);assert(s.has_first_result&&s.first_result_ms<first);drain(&s,&p);

    init_live(&s,&p);p.unknown_age=true;start(&s,&p);await_snapshots(&s,&p,1);
    assert(!s.rows[0].timing_flags&&s.rows[0].callback_age_ms==UINT32_MAX);
    assert(cerberus_dual_ble_age_upper(s.rows[0].callback_age_ms,s.rows[0].request_started_ms,p.now)==UINT32_MAX);
    drain(&s,&p);
    init_live(&s,&p);s.ops.now_ms=NULL;start(&s,&p);await_snapshots(&s,&p,1);
    assert(!s.has_first_result&&!s.first_result_ms);drain(&s,&p);
    init_live(&s,&p);start(&s,&p);p.now+=(uint64_t)UINT32_MAX+1U;
    await_snapshots(&s,&p,1);assert(!s.has_first_result&&!s.first_result_ms&&!s.discovery_clock_valid);
    await_snapshots(&s,&p,2);assert(!s.has_first_result&&!s.first_result_ms);drain(&s,&p);

    cerberus_dual_ble_view_t view;cerberus_dual_ble_view(&s,&view);
    assert(view.snapshots==s.snapshots&&!view.has_first_result&&view.live_updates);
    puts("dual BLE live snapshots: early atomic banks, 32x11 bounded transfer, retry anchor, coalescing, final sweep and first-result metrics passed");
}
static void check_snapshot_faults_and_stop(void)
{
    cerberus_dual_ble_scan_t s;peer_t p;
    for(unsigned fault=1;fault<FAULT_END;++fault) {
        init_live(&s,&p);p.fault=fault;s.count=1;s.rows[0].rssi=-111;
        cerberus_dual_ble_row_t saved=s.rows[0];start(&s,&p);
        for(unsigned i=0;i<200&&!s.error;++i)fast_tick(&s,&p);
        assert(s.error&&s.count==1&&!memcmp(&saved,&s.rows[0],sizeof(saved))&&!s.snapshots&&!s.has_first_result);
        drain(&s,&p);
    }
    /* Stop at every encountered phase/chunk keeps the displayed bank unchanged
       and goes directly through scoped STOP/RELEASE, never another download. */
    init_live(&s,&p);p.count=3;s.count=1;s.rows[0].rssi=-100;
    assert(cerberus_dual_ble_begin(&s,0,NULL,0)==ESP_OK);
    uint32_t phases=0;
    for(unsigned i=0;i<300&&!s.sweeps;++i) {
        phases|=1U<<s.phase;
        cerberus_dual_ble_scan_t stopped=s;peer_t other=p;stopped.ops.context=&other;
        cerberus_dual_ble_row_t saved[32];memcpy(saved,stopped.rows,sizeof(saved));
        unsigned reads=other.snapshot_reads,begins=other.snapshot_begins,fences=other.snapshot_fences;
        drain(&stopped,&other);
        assert(other.snapshot_reads==reads&&other.snapshot_begins==begins&&other.snapshot_fences==fences);
        assert(!memcmp(saved,stopped.rows,sizeof(saved)));
        /* One call per tick exposes BEGIN/READ/FENCE/RELEASE individually. */
        p.cost_ms=9;fast_tick(&s,&p);
        if(s.snapshots)completed(&p);
    }
    assert(s.sweeps==1&&s.snapshots>=2);
    assert((phases&(1U<<11))&&(phases&(1U<<12))&&(phases&(1U<<13))); /* All snapshot phases. */
    drain(&s,&p);

    init_live(&s,&p);start(&s,&p);s.snapshot_request_id=UINT64_MAX;
    for(unsigned i=0;i<40&&!s.error;++i)fast_tick(&s,&p);
    assert(s.error==ESP_ERR_INVALID_STATE&&!p.snapshot_begins);drain(&s,&p);
    /* Negotiated capability and selected status version must agree, including
       the real C5 v2 SNAPSHOTS bit. A legacy peer may not smuggle that bit in. */
    init_live(&s,&p);p.status_remove_flags=CERBERUS_DUAL_BLE_SNAPSHOTS;start(&s,&p);
    assert(s.error==ESP_ERR_INVALID_RESPONSE&&!s.snapshots);p.status_remove_flags=0;drain(&s,&p);
    init(&s,&p);p.status_add_flags=CERBERUS_DUAL_BLE_SNAPSHOTS;start(&s,&p);
    assert(s.error==ESP_ERR_INVALID_RESPONSE&&!s.live_updates);p.status_add_flags=0;drain(&s,&p);
    init(&s,&p);p.snapshot_capability=true;
    assert(cerberus_dual_ble_begin(&s,0,NULL,0)==ESP_OK);tick(&s,&p);
    assert(s.error==ESP_ERR_INVALID_RESPONSE&&!p.starts);
    /* An old READ from a preceding bank cannot replace retained rows,
       even with the same row/chunk position. Transport tokens are not proof. */
    init_live(&s,&p);start(&s,&p);await_snapshots(&s,&p,1);
    cerberus_dual_ble_row_t saved=s.rows[0];p.fault=BAD_READ_HANDLE;
    for(unsigned i=0;i<100&&!s.error;++i)fast_tick(&s,&p);
    assert(s.error==ESP_ERR_INVALID_RESPONSE&&s.snapshots==1&&!memcmp(&saved,&s.rows[0],sizeof(saved)));
    drain(&s,&p);
    puts("dual BLE snapshot validation: 29 malformed/stale/body/fence cases, Stop in every transfer phase and exhausted counter passed");
}
static void assert_provenance(const cerberus_dual_ble_provenance_t *p,
    uint64_t boot, uint64_t epoch, uint8_t generation, uint8_t index)
{
    assert(p->boot_id==boot && p->epoch==epoch && p->generation==generation && p->index==index);
}
static void check_published_provenance(void)
{
    for(unsigned version=1;version<=2;++version) {
        cerberus_dual_ble_scan_t s;peer_t p;init(&s,&p);
        p.version=(uint8_t)version;p.snapshot_capability=version==2;
        p.boot=UINT64_C(0x1234567812345678);p.epoch=UINT64_C(0x100000000);p.generation=255;
        start(&s,&p);
        const uint64_t old_boot=p.boot, old_epoch=p.epoch;
        const uint8_t old_generation=p.generation;
        assert(old_generation==0); /* Wrapped byte is valid, not the identity fence. */
        if(version==1)completed(&p);
        await_snapshots(&s,&p,1);
        cerberus_dual_ble_row_t selected, current;
        cerberus_dual_ble_provenance_t selected_source, current_source;
        unsigned frames=p.frames;
        assert(cerberus_dual_ble_copy_row(&s,1,&selected,&selected_source));
        assert(p.frames==frames);
        assert_provenance(&selected_source,old_boot,old_epoch,old_generation,1);
        drain(&s,&p);
        assert(cerberus_dual_ble_copy_row(&s,1,&current,&current_source));
        assert(!memcmp(&selected,&current,sizeof(selected)));
        assert_provenance(&current_source,old_boot,old_epoch,old_generation,1);

        /* A new boot/job changes mutable START/status immediately. The retained
         * bank's provenance must remain old, including during partial transfer. */
        p.boot+=UINT64_C(0x100000000);p.epoch+=UINT64_C(0x100000000);
        assert(cerberus_dual_ble_begin(&s,0,NULL,0)==ESP_OK);
        tick(&s,&p);
        assert(s.start.boot_id==p.boot && s.start.boot_id!=old_boot);
        assert(cerberus_dual_ble_copy_row(&s,1,&current,&current_source));
        assert_provenance(&current_source,old_boot,old_epoch,old_generation,1);
        if(version==1)completed(&p);
        p.cost_ms=9;
        for(unsigned i=0;i<200&&!s.copied;++i)fast_tick(&s,&p);
        assert(s.copied&&!s.snapshots);
        drain(&s,&p); /* Cancelled staging never publishes its source metadata. */
        assert(cerberus_dual_ble_copy_row(&s,1,&current,&current_source));
        assert_provenance(&current_source,old_boot,old_epoch,old_generation,1);
        assert(!memcmp(&selected,&current,sizeof(selected)));

        p.cost_ms=1;
        assert(cerberus_dual_ble_begin(&s,0,NULL,0)==ESP_OK);tick(&s,&p);
        const uint64_t new_epoch=p.epoch;const uint8_t new_generation=p.generation;
        if(version==1)completed(&p);
        await_snapshots(&s,&p,1);
        assert(cerberus_dual_ble_copy_row(&s,1,&current,&current_source));
        assert_provenance(&current_source,p.boot,new_epoch,new_generation,1);
        assert_provenance(&selected_source,old_boot,old_epoch,old_generation,1);
        assert(selected.request_started_ms<current.request_started_ms);
        /* Invalid copies cannot leak a prior identity or half a row. */
        memset(&current,0xa5,sizeof(current));memset(&current_source,0xa5,sizeof(current_source));
        assert(!cerberus_dual_ble_copy_row(&s,32,&current,&current_source));
        const cerberus_dual_ble_row_t empty={0};
        assert(!memcmp(&current,&empty,sizeof(current)));
        assert_provenance(&current_source,0,0,0,0);
        assert(!cerberus_dual_ble_copy_row(&s,0,&current,NULL));
        drain(&s,&p);
    }
    /* Lost BEGIN replies must not move the frozen row's original anchor. */
    cerberus_dual_ble_scan_t s;peer_t p;init_live(&s,&p);p.lose_begin_replies=2;
    start(&s,&p);await_snapshots(&s,&p,1);
    cerberus_dual_ble_row_t row;cerberus_dual_ble_provenance_t source;
    assert(cerberus_dual_ble_copy_row(&s,0,&row,&source));
    assert(row.request_started_ms+1==p.snapshot_clock_ms && row.request_started_ms+200<=p.now);
    assert_provenance(&source,p.boot,p.epoch,p.generation,0);drain(&s,&p);
    puts("dual BLE publication provenance: v1/v2 full identities, generation wrap, retained/replaced/cancelled banks and lost-BEGIN anchor passed");
}
static void check_inventory_metadata(void)
{
    cerberus_dual_ble_scan_t s;peer_t p;init_live(&s,&p);p.count=32;p.replacements=7;p.dropped=11;
    start(&s,&p);await_snapshots(&s,&p,1);
    assert(s.inventory_known&&s.inventory_replacement_enabled&&s.inventory_replaced==7&&s.inventory_dropped==11);
    cerberus_dual_ble_view_t view;cerberus_dual_ble_view(&s,&view);
    assert(view.inventory_known&&view.inventory_replacement_enabled&&view.inventory_replaced==7&&view.inventory_dropped==11);
    assert(p.inventory_queries==1&&p.snapshot_fences==1);
    /* Counters freeze with BEGIN, not later READ/STATUS. Mutating the live peer
     * during a download cannot attach newer counters to older frozen rows. */
    p.replacements=13;p.dropped=17;p.cost_ms=9;
    for(unsigned i=0;i<200&&p.snapshot_begins<2;++i)fast_tick(&s,&p);
    assert(p.snapshot_begins==2);p.replacements=99;p.dropped=101;
    while(p.inventory_queries<2)fast_tick(&s,&p);
    assert(s.snapshots==1&&s.inventory_replaced==7&&s.inventory_dropped==11);
    await_snapshots(&s,&p,2);
    assert(s.inventory_known&&s.inventory_replaced==13&&s.inventory_dropped==17);
    drain(&s,&p);
    assert(s.inventory_known&&s.inventory_replaced==13);
    assert(cerberus_dual_ble_begin(&s,0,NULL,0)==ESP_OK);tick(&s,&p);
    assert(s.inventory_known&&s.inventory_replaced==13); /* Retained bank, not current START. */
    drain(&s,&p);

    init_live(&s,&p);p.inventory_supported=false;start(&s,&p);await_snapshots(&s,&p,1);
    assert(!s.inventory_known&&!s.inventory_replacement_enabled&&!s.inventory_replaced&&!s.inventory_dropped);
    assert(p.inventory_queries==1);await_snapshots(&s,&p,2);assert(p.inventory_queries==1);drain(&s,&p);
    for(unsigned fault=1;fault<=5;++fault){
        init_live(&s,&p);p.inventory_fault=fault;start(&s,&p);await_snapshots(&s,&p,1);
        assert(!s.error&&!s.inventory_known&&p.inventory_queries==1&&p.snapshot_fences==1);drain(&s,&p);
    }
    init_live(&s,&p);p.inventory_timeouts=20;start(&s,&p);await_snapshots(&s,&p,1);
    assert(!s.error&&!s.inventory_known&&p.inventory_queries==3&&p.snapshot_fences==1);drain(&s,&p);
    init_live(&s,&p);p.inventory_timeouts=2;start(&s,&p);await_snapshots(&s,&p,1);
    assert(s.inventory_known&&p.inventory_queries==3&&p.snapshot_fences==1);drain(&s,&p);
    /* A good optional reply does not bypass a final stale-bank fence. */
    init_live(&s,&p);p.fault=BAD_FENCE_HANDLE;start(&s,&p);
    for(unsigned i=0;i<100&&!s.error;++i)fast_tick(&s,&p);
    assert(s.error&&!s.snapshots&&!s.inventory_known&&p.inventory_queries==1);drain(&s,&p);
    /* Stop after metadata but before final fence cannot publish either half. */
    init_live(&s,&p);p.cost_ms=9;assert(cerberus_dual_ble_begin(&s,0,NULL,0)==ESP_OK);
    for(unsigned i=0;i<100&&!p.inventory_queries;++i)fast_tick(&s,&p);
    assert(p.inventory_queries==1&&!s.snapshots&&!p.snapshot_fences);
    drain(&s,&p);assert(!s.snapshots&&!s.inventory_known&&!p.snapshot_fences);
    puts("dual BLE frozen inventory stats: exact bank, optional old-peer/fault fallback, bounded BUSY, final fence and Stop publication passed");
}
static bool bytes_zero(const void *data,size_t size)
{
    const uint8_t *p=data;
    for(size_t i=0;i<size;++i)if(p[i])return false;
    return true;
}
static void metadata_begin(cerberus_dual_ble_scan_t *s,peer_t *p,uint8_t count)
{
    init_live(s,p);p->metadata_supported=true;p->count=count;
    s->count=1;s->rows[0].rssi=-110;
    s->rows[0].receive=(cerberus_dual_ble_receive_metadata_t){.source_kind=1,.callback_length=3,
        .sid=0xff,.data_status=0xff,.ad_validation=1};
    start(s,p);p->cost_ms=8;
    for(unsigned i=0;i<100&&s->phase!=12;++i)fast_tick(s,p);
    assert(s->phase==12&&!p->metadata_reads&&!p->snapshot_reads&&p->snapshot_begins==1);
}
static void check_receive_metadata(void)
{
    cerberus_dual_ble_scan_t s;peer_t p;
    /* All eleven chunks now carry110 bytes; the final eight bytes remain part
     * of the same frozen row and are copied with its published provenance. */
    metadata_begin(&s,&p,32);
    p.frozen_receive[0]=(cerberus_dual_ble_receive_metadata_t){0};
    p.frozen_receive[1]=(cerberus_dual_ble_receive_metadata_t){.source_kind=2,.properties=1,
        .callback_length=3,.sid=15,.primary_phy=3,.secondary_phy=2,.ad_validation=1};
    p.frozen_receive[2]=(cerberus_dual_ble_receive_metadata_t){.source_kind=2,.properties=0x13,
        .callback_length=3,.sid=0xff,.primary_phy=1,.ad_validation=2};
    /* Full-AD validity and preview parsing are deliberately independent. */
    p.frozen[2].raw_flags=CERBERUS_BLE_ADVERTISEMENT_MALFORMED;
    p.frozen_receive[3]=(cerberus_dual_ble_receive_metadata_t){.source_kind=2,.properties=0,
        .callback_length=255,.sid=0,.primary_phy=1,.secondary_phy=3,.ad_validation=1};
    p.frozen[3].event_type=3;p.frozen[3].raw_length=31;p.frozen[3].raw_flags=3;
    memset(p.frozen[3].raw,0x55,31);
    p.cost_ms=1;await_snapshots(&s,&p,1);
    assert(p.metadata_reads==32U*11U&&!p.snapshot_reads&&p.snapshot_begins==1&&p.snapshot_fences==1);
    assert(s.metadata_read_enabled&&s.count==32&&!s.sweeps&&p.owned);
    for(unsigned i=0;i<32;++i)assert(!memcmp(&s.rows[i].receive,&p.frozen_receive[i],sizeof(s.rows[i].receive)));
    cerberus_dual_ble_row_t selected;cerberus_dual_ble_provenance_t source;
    assert(cerberus_dual_ble_copy_row(&s,3,&selected,&source));
    assert(selected.receive.callback_length==255&&selected.receive.ad_validation==1&&selected.raw_flags==3);
    p.frozen_receive[3].callback_length=31;
    assert(selected.receive.callback_length==255&&s.rows[3].receive.callback_length==255);
    drain(&s,&p);

    /* A NOT_SUPPORTED reply is the only downgrade condition. Even after
     * complete metadata rows were staged, ALL staging is cleared and the same
     * handle is read from row0 through legacy READ, without another BEGIN. */
    const unsigned fallback_positions[]={0,1,10,11,16,351};
    for(unsigned n=0;n<sizeof(fallback_positions)/sizeof(fallback_positions[0]);++n){
        metadata_begin(&s,&p,32);
        cerberus_dual_ble_row_t saved=s.rows[0];
        uint64_t handle=s.snapshot.handle,anchor=s.snapshot_request_started_ms;
        p.metadata_fail_enabled=true;p.metadata_fail_after=fallback_positions[n];
        for(unsigned i=0;i<500&&s.metadata_read_enabled;++i)assert(fast_tick(&s,&p)<=1);
        assert(!s.metadata_read_enabled&&p.metadata_reads==fallback_positions[n]+1);
        assert(s.phase==12&&!s.read_index&&!s.copied&&bytes_zero(s.staging,sizeof(s.staging)));
        assert(bytes_zero(&s.snapshot_metadata,sizeof(s.snapshot_metadata)));
        assert(s.snapshot.handle==handle&&s.snapshot_request_started_ms==anchor&&s.snapshot_anchor_valid);
        assert(p.request_id==1&&p.snapshot_begins==1&&p.starts==1&&!p.stops&&!p.releases);
        assert(!s.snapshots&&!memcmp(&saved,&s.rows[0],sizeof(saved)));
        p.cost_ms=1;await_snapshots(&s,&p,1);
        assert(p.snapshot_reads==32U*11U&&p.snapshot_fences==1);
        for(unsigned i=0;i<32;++i)assert(bytes_zero(&s.rows[i].receive,sizeof(s.rows[i].receive))&&
            s.rows[i].request_started_ms==anchor);
        unsigned attempts=p.metadata_reads;
        await_snapshots(&s,&p,2);assert(p.metadata_reads==attempts&&!s.metadata_read_enabled);
        drain(&s,&p);
        assert(cerberus_dual_ble_begin(&s,0,NULL,0)==ESP_OK&&s.metadata_read_enabled);
        drain(&s,&p);
    }
    /* A lost first BEGIN reply plus a later downgrade cannot refresh age. */
    init_live(&s,&p);p.metadata_supported=true;p.metadata_fail_enabled=true;p.metadata_fail_after=12;
    p.lose_begin_replies=2;start(&s,&p);await_snapshots(&s,&p,1);
    assert(p.snapshot_begins==3&&p.handle_counter==1&&p.metadata_reads==13);
    assert(s.rows[0].request_started_ms+1==p.snapshot_clock_ms&&s.snapshot_request_started_ms+200<p.now);
    assert(bytes_zero(&s.rows[0].receive,sizeof(s.rows[0].receive)));drain(&s,&p);

    /* Validate each envelope dimension, rather than accepting properties or
     * full-AD status inferred from the projected raw prefix. */
    for(unsigned bad=0;bad<22;++bad){
        metadata_begin(&s,&p,1);cerberus_dual_ble_row_t saved=s.rows[0];
        cerberus_dual_ble_receive_metadata_t *m=&p.frozen_receive[0];
        if(bad>=9)*m=(cerberus_dual_ble_receive_metadata_t){.source_kind=2,.properties=1,
            .callback_length=3,.sid=0xff,.primary_phy=1,.ad_validation=1};
        switch(bad){
        case 0:m->source_kind=3;break;
        case 1:m->source_kind=0;break; /* Noncanonical UNKNOWN. */
        case 2:m->properties=1;break;
        case 3:m->callback_length=32;break;
        case 4:m->sid=0;break;
        case 5:m->primary_phy=1;break;
        case 6:m->secondary_phy=1;break;
        case 7:m->data_status=0;break;
        case 8:m->ad_validation=0;break;
        case 9:m->properties=0x80;break;
        case 10:m->properties=8;break; /* Response is not an original ADV. */
        case 11:m->callback_length=2;break;
        case 12:m->callback_length=32;break; /* Preview/truncation mismatch. */
        case 13:m->sid=16;break;
        case 14:m->primary_phy=2;break;
        case 15:m->secondary_phy=4;break;
        case 16:m->data_status=1;break;
        case 17:m->data_status=2;break;
        case 18:m->ad_validation=3;break;
        case 19:m->properties=0;break; /* Mapped event mismatch. */
        case 20:m->properties=0x13;m->sid=1;break;
        case 21:m->properties=0x13;m->secondary_phy=1;break;
        }
        for(unsigned i=0;i<100&&!s.error;++i)fast_tick(&s,&p);
        assert(s.error==ESP_ERR_INVALID_RESPONSE&&!s.snapshots&&s.metadata_read_enabled&&!p.snapshot_reads);
        assert(!memcmp(&saved,&s.rows[0],sizeof(saved)));drain(&s,&p);
    }
    /* Metadata does not weaken transport/semantic or final bank fences. */
    const unsigned faults[]={BAD_READ_HANDLE,BAD_READ_INDEX,BAD_READ_CHUNK,BAD_READ_LENGTH,
        BAD_READ_ERROR,BAD_READ_ERROR_BODY,BAD_FENCE_HANDLE,BAD_FENCE_COUNT,BAD_FENCE_FLAGS};
    for(unsigned i=0;i<sizeof(faults)/sizeof(faults[0]);++i){
        metadata_begin(&s,&p,1);cerberus_dual_ble_row_t saved=s.rows[0];p.fault=faults[i];
        for(unsigned j=0;j<100&&!s.error;++j)fast_tick(&s,&p);
        assert(s.error==ESP_ERR_INVALID_RESPONSE&&!s.snapshots&&!p.snapshot_reads&&s.metadata_read_enabled);
        assert(!memcmp(&saved,&s.rows[0],sizeof(saved)));drain(&s,&p);
    }
    metadata_begin(&s,&p,1);p.busy_read=2;await_snapshots(&s,&p,1);
    assert(s.metadata_read_enabled&&!p.snapshot_reads&&p.metadata_reads==13&&p.snapshot_begins==1);drain(&s,&p);
    /* STOP wins in every row/chunk position and before the final fence. */
    metadata_begin(&s,&p,2);
    for(unsigned i=0;i<30&&!s.snapshots;++i){
        cerberus_dual_ble_scan_t stopped=s;peer_t other=p;stopped.ops.context=&other;
        cerberus_dual_ble_row_t saved=stopped.rows[0];
        unsigned reads=other.metadata_reads,old_reads=other.snapshot_reads,fences=other.snapshot_fences;
        drain(&stopped,&other);
        assert(other.metadata_reads==reads&&other.snapshot_reads==old_reads&&other.snapshot_fences==fences);
        assert(!memcmp(&saved,&stopped.rows[0],sizeof(saved))&&!stopped.snapshots);
        fast_tick(&s,&p);
    }
    assert(s.snapshots==1);drain(&s,&p);
    puts("dual BLE receive metadata: coherent110-byte banks, exact legacy fallback, original age, validation, retry, frozen copies and Stop passed");
}
static void check_analyzer_history(void)
{
    cerberus_dual_ble_analyzer_history_t h={0},saved;
    uint32_t rate=99;
    assert(!cerberus_dual_ble_analyzer_rate(NULL,&rate)&&!rate);
    assert(!cerberus_dual_ble_analyzer_accept(NULL,1,1,0,0,false,false));
    assert(!cerberus_dual_ble_analyzer_accept(&h,0,1,0,0,false,false));
    assert(!cerberus_dual_ble_analyzer_accept(&h,1,0,0,0,false,false));
    assert(!cerberus_dual_ble_analyzer_accept(&h,1,1,UINT32_MAX,10,false,false));
    assert(cerberus_dual_ble_analyzer_accept(&h,1,1,10,100,false,false)&&!h.count);
    assert(cerberus_dual_ble_analyzer_accept(&h,1,1,12,100,false,false)&&!h.count);
    assert(cerberus_dual_ble_analyzer_accept(&h,1,1,15,100,false,false)&&!h.count);
    assert(cerberus_dual_ble_analyzer_accept(&h,1,1,20,200,false,false)&&h.count==1);
    assert(h.history[0].reports==10&&h.history[0].duration_ms==100);
    assert(cerberus_dual_ble_analyzer_rate(&h.history[0],&rate)&&rate==100);
    assert(cerberus_dual_ble_analyzer_accept(&h,1,1,20,200,false,false)&&h.count==1);
    saved=h;assert(!cerberus_dual_ble_analyzer_accept(&h,1,1,19,300,false,false)&&!memcmp(&h,&saved,sizeof(h)));
    assert(!cerberus_dual_ble_analyzer_accept(&h,1,1,20,199,false,false)&&!memcmp(&h,&saved,sizeof(h)));
    assert(cerberus_dual_ble_analyzer_accept(&h,1,1,20,300,false,false));
    assert(cerberus_dual_ble_analyzer_rate(&h.history[1],&rate)&&rate==0); /* Measured silence. */
    cerberus_dual_ble_analyzer_gap(&h);assert(h.count==3&&!h.history[2].valid&&!h.sample_valid);
    cerberus_dual_ble_analyzer_gap(&h);assert(h.count==3); /* Coalesce unknown gap. */
    assert(cerberus_dual_ble_analyzer_accept(&h,1,1,100,2000,false,false)&&h.count==3);
    assert(cerberus_dual_ble_analyzer_accept(&h,1,1,120,2500,false,false)&&h.count==4);
    assert(h.history[3].reports==20&&h.history[3].duration_ms==500);
    assert(cerberus_dual_ble_analyzer_accept(&h,2,2,0,0,false,false)&&h.count==5);
    assert(!h.history[4].valid); /* Never derive across source epoch reset. */
    for(unsigned n=1;n<=80;++n)assert(cerberus_dual_ble_analyzer_accept(&h,2,2,n*3U,n*500U,false,false));
    assert(h.count==32);
    for(unsigned n=0;n<32;++n)assert(h.history[n].valid&&h.history[n].reports==3&&h.history[n].duration_ms==500);
    assert(cerberus_dual_ble_analyzer_accept(&h,2,2,UINT32_MAX,41000,true,false));
    assert(h.saturated&&!h.baseline&&!h.history[(h.head+31)%32].valid);
    saved=h;assert(!cerberus_dual_ble_analyzer_accept(&h,2,2,UINT32_MAX,42000,false,false));
    assert(!memcmp(&h,&saved,sizeof(h)));
    assert(cerberus_dual_ble_analyzer_accept(&h,2,2,UINT32_MAX,42000,true,true)&&h.final);
    assert(!cerberus_dual_ble_analyzer_accept(&h,2,2,UINT32_MAX,43000,true,false));
    cerberus_dual_ble_analyzer_interval_t overflow={UINT32_MAX,1,true};
    assert(!cerberus_dual_ble_analyzer_rate(&overflow,&rate)&&!rate);
    overflow.duration_ms=1000;assert(cerberus_dual_ble_analyzer_rate(&overflow,&rate)&&rate==UINT32_MAX);
    overflow.duration_ms=0;assert(!cerberus_dual_ble_analyzer_rate(&overflow,&rate));
    puts("C5 interval history: source-time deltas, same-ms callback conservation, gaps, saturation, restart and32-entry bounds passed");
}
static void analyzer_start(cerberus_dual_ble_scan_t *s,peer_t *p)
{
    init_live(s,p);p->count=0;p->analyzer_elapsed=100;p->analyzer_reports=10;
    s->count=1;s->rows[0].rssi=-99; /* Ordinary retained bank stays untouched. */
    assert(cerberus_dual_ble_begin(s,CERBERUS_DUAL_BLE_ANALYZER,NULL,0)==ESP_OK);
    for(unsigned n=0;n<20&&!s->analyzer.samples&&!s->error;++n)fast_tick(s,p);
    assert(!s->error&&s->analyzer.samples==1&&!s->analyzer.count);
}
static void analyzer_next(cerberus_dual_ble_scan_t *s,peer_t *p)
{
    uint32_t target=s->analyzer.samples+1;
    for(unsigned n=0;n<1000&&s->analyzer.samples<target&&!s->error;++n)fast_tick(s,p);
    assert(!s->error&&s->analyzer.samples==target);
}
static void check_analyzer_controller(void)
{
    cerberus_dual_ble_scan_t s;peer_t p;cerberus_dual_ble_analyzer_view_t v;
    cerberus_dual_ble_analyzer_view(NULL,&v);assert(!v.enabled&&!v.history_count&&!v.sample_valid);
    analyzer_start(&s,&p);cerberus_dual_ble_row_t retained=s.rows[0];
    unsigned frames=p.frames;cerberus_dual_ble_analyzer_view(&s,&v);
    assert(p.frames==frames&&v.enabled&&v.attached&&v.receiving&&v.extended&&v.sample_valid&&v.reports==10);
    uint64_t due=s.next_poll_ms;while(p.now<due){frames=p.frames;fast_tick(&s,&p);assert(p.frames==frames);}
    p.analyzer_reports=110;p.analyzer_elapsed=600;analyzer_next(&s,&p);
    cerberus_dual_ble_analyzer_view(&s,&v);uint32_t rate;
    assert(v.history_count==1&&v.history[0].reports==100&&v.history[0].duration_ms==500);
    assert(cerberus_dual_ble_analyzer_rate(&v.history[0],&rate)&&rate==200);
    p.analyzer_elapsed=1600;analyzer_next(&s,&p); /* Silence has an actual source duration. */
    assert(s.analyzer.history[1].valid&&!s.analyzer.history[1].reports&&s.analyzer.history[1].duration_ms==1000);
    p.analyzer_busy=1;
    unsigned reads=p.analyzer_reads;
    for(unsigned n=0;n<100&&p.analyzer_reads==reads;++n)fast_tick(&s,&p);
    assert(!s.error&&!s.analyzer.sample_valid&&!s.analyzer.baseline);
    uint8_t entries=s.analyzer.count;p.analyzer_reports=210;p.analyzer_elapsed=2100;analyzer_next(&s,&p);
    assert(s.analyzer.count==entries); /* Re-establish baseline; do not bridge BUSY. */
    p.analyzer_elapsed=2600;p.analyzer_reports=215;analyzer_next(&s,&p);
    assert(s.analyzer.history[(s.analyzer.head+31)%32].reports==5);
    assert(!p.snapshot_begins&&!p.snapshot_reads&&!p.metadata_reads&&!p.inventory_queries&&!s.snapshots);
    assert(s.count==1&&!memcmp(&retained,&s.rows[0],sizeof(retained)));
    completed(&p);p.analyzer_elapsed=8000;p.analyzer_reports=300;
    for(unsigned n=0;n<100&&!s.sweeps&&!s.error;++n)fast_tick(&s,&p);
    assert(!s.error&&s.sweeps==1&&s.analyzer.final&&s.analyzer.reports==300);
    uint64_t old_epoch=s.analyzer.epoch;
    for(unsigned n=0;n<100&&p.starts<2&&!s.error;++n)fast_tick(&s,&p);
    assert(p.starts==2);p.analyzer_elapsed=0;p.analyzer_reports=0;
    for(unsigned n=0;n<100&&s.analyzer.epoch==old_epoch&&!s.error;++n)fast_tick(&s,&p);
    assert(!s.error&&s.analyzer.epoch!=old_epoch&&s.sweeps==1);
    cerberus_dual_ble_analyzer_view(&s,&v);assert(v.history_count&& !v.history[v.history_count-1].valid);
    drain(&s,&p);unsigned stopped_reads=p.analyzer_reads;tick(&s,&p);assert(p.analyzer_reads==stopped_reads);
    assert(cerberus_dual_ble_begin(&s,CERBERUS_DUAL_BLE_ANALYZER,NULL,0)==ESP_OK);
    assert(!s.analyzer.samples&&!s.analyzer.count&&!s.sweeps&&s.count==1);drain(&s,&p);

    /* Analyzer cannot silently fall back to ordinary discovery on old peers. */
    init(&s,&p);assert(cerberus_dual_ble_begin(&s,2,NULL,0)==ESP_OK);fast_tick(&s,&p);
    assert(s.error==ESP_ERR_NOT_SUPPORTED&&!p.starts&&!p.stops&&cerberus_dual_ble_released(&s));
    init_live(&s,&p);p.analyzer_supported=false;
    assert(cerberus_dual_ble_begin(&s,2,NULL,0)==ESP_OK);fast_tick(&s,&p);
    assert(s.error==ESP_ERR_NOT_SUPPORTED&&!p.starts&&!p.stops&&cerberus_dual_ble_released(&s));

    /* Every invalid envelope or invalid final fence suppresses current validity
       without replacing the independent discovery bank. */
    for(unsigned fault=1;fault<=14;++fault){
        analyzer_start(&s,&p);retained=s.rows[0];p.analyzer_fault=fault;
        for(unsigned n=0;n<200&&!s.error;++n)fast_tick(&s,&p);
        assert(s.error==ESP_ERR_INVALID_RESPONSE&&!s.analyzer.sample_valid&&!s.sweeps);
        assert(s.count==1&&!memcmp(&retained,&s.rows[0],sizeof(retained)));drain(&s,&p);
    }
    for(unsigned rewind=0;rewind<2;++rewind){
        analyzer_start(&s,&p);
        if(rewind)p.analyzer_reports=9;else p.analyzer_elapsed=99;
        for(unsigned n=0;n<100&&!s.error;++n)fast_tick(&s,&p);
        assert(s.error==ESP_ERR_INVALID_RESPONSE&&!s.analyzer.sample_valid);drain(&s,&p);
    }
    analyzer_start(&s,&p);p.analyzer_timeouts=3;
    for(unsigned n=0;n<200&&!s.error;++n)fast_tick(&s,&p);
    assert(s.error==ESP_ERR_TIMEOUT&&!s.analyzer.sample_valid);drain(&s,&p);
    analyzer_start(&s,&p);p.cost_ms=9;p.analyzer_elapsed=600;p.analyzer_reports=15;
    for(unsigned n=0;n<100&&s.analyzer.samples<2;++n)assert(fast_tick(&s,&p)<=1);
    assert(s.analyzer.samples==2);drain(&s,&p);
    /* Stop wins before INFO/START/POLL/SAMPLE/FENCE/RELEASE; no sample is read
       after cancellation. Retained history is never an authorization to RF. */
    init_live(&s,&p);p.count=0;p.cost_ms=8;p.analyzer_elapsed=100;
    assert(cerberus_dual_ble_begin(&s,2,NULL,0)==ESP_OK);
    for(unsigned n=0;n<45;++n){
        cerberus_dual_ble_scan_t copy=s;peer_t other=p;copy.ops.context=&other;
        unsigned before=other.analyzer_reads;drain(&copy,&other);assert(other.analyzer_reads==before);
        if(n==25)completed(&p);
        fast_tick(&s,&p);assert(!s.error);
    }
    drain(&s,&p);
    puts("C5 analyzer controller: bounded cadence, source clocks, gaps, old-peer admission, final fence, no inventory, Stop and retained-bank isolation passed");
}
static void family_start(cerberus_dual_ble_scan_t *s,peer_t *p,uint8_t family)
{
    init_live(s,p);p->family_supported=true;p->count=2;
    assert(cerberus_dual_ble_begin_family(s,family)==ESP_OK);
    tick(s,p);assert(p->starts==1&&p->mode==CERBERUS_DUAL_BLE_FAMILY&&p->family_code==family);
}
static void check_family_reader(void)
{
    cerberus_dual_ble_scan_t s;peer_t p;cerberus_dual_ble_view_t view;
    init_live(&s,&p);
    assert(cerberus_dual_ble_begin_family(NULL,1)==ESP_ERR_INVALID_ARG);
    assert(cerberus_dual_ble_begin_family(&s,0)==ESP_ERR_INVALID_ARG);
    assert(cerberus_dual_ble_begin_family(&s,7)==ESP_ERR_INVALID_ARG);
    assert(cerberus_dual_ble_begin(&s,CERBERUS_DUAL_BLE_FAMILY,NULL,0)==ESP_ERR_INVALID_ARG);
    assert(!s.enabled&&!p.frames);
    for(uint8_t family=1;family<=6;++family){
        family_start(&s,&p,family);
        assert(cerberus_dual_ble_begin_family(&s,family)==ESP_ERR_INVALID_STATE);
        cerberus_dual_ble_view(&s,&view);assert(view.requested_family==family&&!view.published_family);
        await_snapshots(&s,&p,1);
        assert(p.family_reads==32&&!p.metadata_reads&&!p.snapshot_reads&&s.live_updates);
        cerberus_dual_ble_view(&s,&view);assert(view.requested_family==family&&view.published_family==family);
        assert(s.rows[0].family.family_code==family&&s.rows[1].family.family_code==family);
        cerberus_dual_ble_row_t saved=s.rows[0];drain(&s,&p);
        assert(cerberus_dual_ble_begin_family(&s,family==6?1:family+1)==ESP_OK);
        cerberus_dual_ble_view(&s,&view);
        assert(view.requested_family!=family&&view.published_family==family&&!memcmp(&saved,&s.rows[0],sizeof(saved)));
        drain(&s,&p);
        assert(cerberus_dual_ble_begin(&s,CERBERUS_DUAL_BLE_DISCOVERY,NULL,0)==ESP_OK);
        await_snapshots(&s,&p,1);
        assert(!s.published_family&&bytes_zero(&s.rows[0].family,sizeof(s.rows[0].family)));
        drain(&s,&p);
    }
    /* Family never silently becomes generic on old capability or command support. */
    for(unsigned old=0;old<3;++old){
        init_live(&s,&p);
        if(old==0){p.version=1;p.snapshot_capability=false;}
        else if(old==1)p.snapshot_capability=false;
        assert(cerberus_dual_ble_begin_family(&s,1)==ESP_OK);fast_tick(&s,&p);
        assert(s.error==ESP_ERR_NOT_SUPPORTED&&!p.starts&&!p.stops&&!p.family_reads&&cerberus_dual_ble_released(&s));
    }
    family_start(&s,&p,1);s.count=1;s.rows[0].rssi=-99;s.published_family=6;
    cerberus_dual_ble_row_t saved=s.rows[0];p.family_read_unsupported=true;
    for(unsigned n=0;n<200&&!s.error;++n)fast_tick(&s,&p);
    assert(s.error==ESP_ERR_NOT_SUPPORTED&&p.family_reads==1&&!p.metadata_reads&&!p.snapshot_reads);
    assert(!s.snapshots&&s.published_family==6&&!memcmp(&saved,&s.rows[0],sizeof(saved)));drain(&s,&p);
    /* Invalid proof, full binding, chunk and final fence errors are fatal.
     * Even a valid raw preview is not an excuse to reinterpret a bad proof. */
    for(unsigned fault=1;fault<=7;++fault){
        family_start(&s,&p,2);s.count=1;s.rows[0].rssi=-101;s.published_family=5;saved=s.rows[0];
        p.family_fault=fault;
        for(unsigned n=0;n<200&&!s.error;++n)fast_tick(&s,&p);
        assert(s.error==ESP_ERR_INVALID_RESPONSE&&!s.snapshots&&!p.metadata_reads&&!p.snapshot_reads);
        assert(s.published_family==5&&!memcmp(&saved,&s.rows[0],sizeof(saved)));drain(&s,&p);
    }
    for(unsigned fault=BAD_READ_HANDLE;fault<FAULT_END;++fault){
        family_start(&s,&p,3);p.fault=fault;s.count=1;s.rows[0].rssi=-103;saved=s.rows[0];
        for(unsigned n=0;n<300&&!s.error;++n)fast_tick(&s,&p);
        assert(s.error==ESP_ERR_INVALID_RESPONSE&&!s.snapshots&&!memcmp(&saved,&s.rows[0],sizeof(saved)));
        drain(&s,&p);
    }
    family_start(&s,&p,4);p.count=32;p.lose_begin_replies=2;
    await_snapshots(&s,&p,1);
    assert(p.family_reads==32U*16U&&p.snapshot_begins==3&&p.handle_counter==1);
    assert(s.rows[31].request_started_ms+1==p.snapshot_clock_ms);
    drain(&s,&p);
    /* Stop has priority at every byte boundary, including the proof suffix. */
    family_start(&s,&p,5);p.count=1;p.cost_ms=8;
    unsigned positions=0;
    for(unsigned n=0;n<200&&!s.snapshots;++n){
        if(s.phase==12){
            cerberus_dual_ble_scan_t copy=s;peer_t other=p;copy.ops.context=&other;
            unsigned reads=other.family_reads;drain(&copy,&other);
            assert(other.family_reads==reads&&!copy.snapshots&&!copy.published_family);++positions;
        }
        fast_tick(&s,&p);
    }
    assert(s.snapshots==1&&positions==16);drain(&s,&p);
    puts("FAMILY158 reader: six selectors, 32x16 bounded transfer, immutable binding, no downgrade, malformed proof/fence, Stop and retained-bank scope passed");
}
static void response_start(cerberus_dual_ble_scan_t *s,peer_t *p,uint8_t family,uint8_t count)
{
    init_live(s,p);p->response_supported=true;p->count=count;
    assert(cerberus_dual_ble_begin_response(s,family)==ESP_OK);
    assert(s->compact_response_read_enabled);
    /* These pre-existing tests intentionally exercise the full310 reader.
     * The separate compact matrix below uses the public begin default. */
    s->compact_response_read_enabled=false;
    tick(s,p);assert(p->starts==1&&cerberus_dual_ble_response_mode(p->mode)&&p->family_code==family);
}
static void response_read_begin(cerberus_dual_ble_scan_t *s,peer_t *p,uint8_t family,uint8_t count)
{
    response_start(s,p,family,count);p->cost_ms=8;
    for(unsigned n=0;n<100&&s->phase!=12;++n)fast_tick(s,p);
    assert(s->phase==12&&!s->copied&&!p->response_reads&&p->snapshot_begins==1);
}
static void check_response_reader(void)
{
    cerberus_dual_ble_scan_t s;peer_t p;cerberus_dual_ble_view_t view;
    init_live(&s,&p);
    assert(cerberus_dual_ble_begin_response(NULL,0)==ESP_ERR_INVALID_ARG);
    assert(cerberus_dual_ble_begin_response(&s,7)==ESP_ERR_INVALID_ARG);
    assert(cerberus_dual_ble_begin(&s,CERBERUS_DUAL_BLE_RESPONSE_DISCOVERY,NULL,0)==ESP_ERR_INVALID_ARG);
    assert(cerberus_dual_ble_begin(&s,CERBERUS_DUAL_BLE_RESPONSE_FAMILY,NULL,0)==ESP_ERR_INVALID_ARG&&!p.frames);
    for(uint8_t family=0;family<=6;++family){
        response_start(&s,&p,family,2);
        assert(cerberus_dual_ble_begin_response(&s,family)==ESP_ERR_INVALID_STATE);
        cerberus_dual_ble_view(&s,&view);
        assert(view.requested_response&&!view.published_response&&view.requested_family==family);
        await_snapshots(&s,&p,1);
        assert(p.response_reads==62&&!p.family_reads&&!p.metadata_reads&&!p.snapshot_reads);
        cerberus_dual_ble_view(&s,&view);
        assert(view.requested_response&&view.published_response&&view.published_family==family);
        cerberus_dual_ble_row_t row;cerberus_dual_ble_provenance_t proof;
        assert(cerberus_dual_ble_copy_row(&s,0,&row,&proof));
        assert(row.response.original_sequence==UINT64_C(0x100000001)&&
            row.response.response_sequence==UINT64_C(0x100000002));
        assert(row.callback_age_ms==17&&row.response.age_ms==9&&row.rssi==-85&&row.response.rssi==-49);
        assert(!strcmp(row.name,"SNAP")&&!memcmp(row.response.name,"HC-05",5)&&row.raw[2]==6);
        cerberus_dual_ble_family_evidence_t evidence;
        assert(cerberus_dual_ble_response_present(&row,&proof));
        assert(cerberus_dual_ble_response_classify(&row,&proof,&evidence)&&evidence.hc_name==CERBERUS_BLE_HC_05);
        uint32_t age=123;
        assert(cerberus_dual_ble_response_age(&row,row.request_started_ms,&age)&&age==9);
        assert(cerberus_dual_ble_response_age(&row,row.request_started_ms+5,&age)&&age==14);
        age=123;assert(!cerberus_dual_ble_response_age(&row,row.request_started_ms-1,&age)&&age==123);
        assert(!cerberus_dual_ble_response_age(&row,UINT64_MAX,&age)&&age==123);
        cerberus_dual_ble_provenance_t bad=proof;bad.epoch=0;
        assert(!cerberus_dual_ble_response_present(&row,&bad)&&
            !cerberus_dual_ble_response_classify(&row,&bad,&evidence)&&evidence.state==CERBERUS_DUAL_BLE_FAMILY_NO_PROVENANCE);
        drain(&s,&p);
        assert(cerberus_dual_ble_begin(&s,CERBERUS_DUAL_BLE_DISCOVERY,NULL,0)==ESP_OK);
        cerberus_dual_ble_view(&s,&view);
        assert(!view.requested_response&&view.published_response&&view.published_family==family);
        assert(!memcmp(&row,&s.rows[0],sizeof(row))&&cerberus_dual_ble_response_present(&row,&proof));
        await_snapshots(&s,&p,1);assert(!s.published_response&&!s.published_family&&
            bytes_zero(&s.rows[0].response,sizeof(s.rows[0].response)));drain(&s,&p);
    }
    /* Explicit mode never silently becomes passive/old metadata on capability
     * or command failure, including a failure after a prior committed bank. */
    for(unsigned old=0;old<3;++old){
        init_live(&s,&p);
        if(old==0){p.version=1;p.snapshot_capability=false;}
        else if(old==1)p.snapshot_capability=false;
        assert(cerberus_dual_ble_begin_response(&s,0)==ESP_OK);fast_tick(&s,&p);
        assert(s.error==ESP_ERR_NOT_SUPPORTED&&!p.starts&&!p.stops&&!p.response_reads&&cerberus_dual_ble_released(&s));
    }
    response_start(&s,&p,6,1);s.count=1;s.rows[0].rssi=-111;s.published_response=false;s.published_family=3;
    cerberus_dual_ble_row_t saved=s.rows[0];p.response_read_unsupported=true;
    for(unsigned n=0;n<200&&!s.error;++n)fast_tick(&s,&p);
    assert(s.error==ESP_ERR_NOT_SUPPORTED&&p.response_reads==1&&!p.family_reads&&!p.metadata_reads&&!p.snapshot_reads);
    assert(!s.snapshots&&!s.published_response&&s.published_family==3&&!memcmp(&saved,&s.rows[0],sizeof(saved)));drain(&s,&p);
    for(unsigned fault=1;fault<=27;++fault){
        response_start(&s,&p,6,1);s.count=1;s.rows[0].rssi=-112;saved=s.rows[0];p.response_fault=fault;
        for(unsigned n=0;n<250&&!s.error;++n)fast_tick(&s,&p);
        assert(s.error==ESP_ERR_INVALID_RESPONSE&&!s.snapshots&&!s.published_response&&
            !memcmp(&saved,&s.rows[0],sizeof(saved)));drain(&s,&p);
    }
    for(unsigned fault=BAD_READ_HANDLE;fault<FAULT_END;++fault){
        response_start(&s,&p,6,2);s.count=1;s.rows[0].rssi=-113;saved=s.rows[0];p.fault=fault;
        for(unsigned n=0;n<300&&!s.error;++n)fast_tick(&s,&p);
        assert(s.error==ESP_ERR_INVALID_RESPONSE&&!s.snapshots&&!memcmp(&saved,&s.rows[0],sizeof(saved)));drain(&s,&p);
    }
    /* An invalid original proof cannot hide behind an otherwise valid response;
     * generic acquisition must carry no selected-family proof in its prefix. */
    for(unsigned family=0;family<=6;family+=6){
        response_read_begin(&s,&p,(uint8_t)family,1);
        p.frozen_family[0].version=2;
        for(unsigned n=0;n<200&&!s.error;++n)fast_tick(&s,&p);
        assert(s.error==ESP_ERR_INVALID_RESPONSE&&!s.snapshots);drain(&s,&p);
    }
    /* Canonical UNKNOWN and proven NONE are different, supported publications.
     * Original clocks and bytes do not get refreshed by either. */
    for(unsigned state=0;state<2;++state){
        response_read_begin(&s,&p,0,1);
        p.frozen_response[0]=(cerberus_dual_ble_response_wire_t){0};
        if(state)p.frozen_response[0]=(cerberus_dual_ble_response_wire_t){.version=1,
            .state=CERBERUS_DUAL_BLE_RESPONSE_NONE,.original_sequence=UINT64_C(0x123456789)};
        await_snapshots(&s,&p,1);assert(s.published_response&&s.rows[0].response.state==state);
        uint32_t age=77;assert(!cerberus_dual_ble_response_age(&s.rows[0],p.now,&age)&&age==77);
        assert(s.rows[0].callback_age_ms==17&&s.rows[0].raw[2]==6);drain(&s,&p);
    }
    /* Valid independent unknown response age does not become the original age. */
    response_read_begin(&s,&p,0,1);p.frozen_response[0].flags&=(uint8_t)~CERBERUS_DUAL_BLE_RESPONSE_AGE_VALID;
    p.frozen_response[0].age_ms=UINT32_MAX;await_snapshots(&s,&p,1);
    uint32_t untouched=73;assert(!cerberus_dual_ble_response_age(&s.rows[0],p.now,&untouched)&&untouched==73);drain(&s,&p);
    for(unsigned upper=0;upper<2;++upper){
        response_read_begin(&s,&p,0,1);p.frozen_response[0].interval_us=8001;
        p.frozen_response[0].age_ms=upper?8:9;await_snapshots(&s,&p,1);drain(&s,&p);
    }
    /* Beyond255 offset progress stays bounded and does not terminate early. */
    response_start(&s,&p,6,32);p.lose_begin_replies=2;
    unsigned peak=0;bool beyond=false;
    for(unsigned n=0;n<1600&&!s.snapshots;++n){unsigned used=fast_tick(&s,&p);if(used>peak)peak=used;
        if(s.copied>255U)beyond=true;}
    assert(s.snapshots==1&&s.count==32&&p.response_reads==32U*31U&&peak==4&&beyond);
    assert(p.snapshot_begins==3&&p.handle_counter==1&&s.rows[31].request_started_ms+1==p.snapshot_clock_ms);
    assert(s.rows[31].response.age_ms==9&&!s.sweeps);
    completed(&p);for(unsigned n=0;n<1500&&!s.sweeps;++n)fast_tick(&s,&p);
    assert(s.sweeps==1&&p.complete_fences==1);drain(&s,&p);
    /* Stop never finishes an in-flight row or fence, including bytes256..309. */
    response_read_begin(&s,&p,0,1);unsigned positions=0;
    for(unsigned n=0;n<200&&!s.snapshots;++n){
        if(s.phase==12){cerberus_dual_ble_scan_t copy=s;peer_t other=p;copy.ops.context=&other;
            unsigned reads=other.response_reads;drain(&copy,&other);
            assert(other.response_reads==reads&&!copy.snapshots&&!copy.published_response);++positions;}
        fast_tick(&s,&p);
    }
    assert(s.snapshots==1&&positions==31);drain(&s,&p);
    /* Lost START still requires bounded owner cleanup; no response bank escapes. */
    init_live(&s,&p);p.response_supported=true;p.lose_start=true;p.fail_stop=true;
    assert(cerberus_dual_ble_begin_response(&s,6)==ESP_OK);
    for(unsigned n=0;n<30&&!s.error;++n)tick(&s,&p);
    assert(s.error==ESP_ERR_TIMEOUT&&p.starts==1&&!s.snapshots&&!cerberus_dual_ble_released(&s));
    p.fail_stop=false;drain(&s,&p);
    puts("Response310 reader: explicit before-RF capability, 64-bit binding, separate hints/ages, 32x31 bounded transfer, frozen bank, UNKNOWN/NONE, Stop and no passive downgrade passed");
}

static void compact_read_begin(cerberus_dual_ble_scan_t *s,peer_t *p,uint8_t family,uint8_t count)
{
    init_live(s,p);p->response_supported=p->compact_supported=true;p->count=count;
    assert(cerberus_dual_ble_begin_response(s,family)==ESP_OK&&s->compact_response_read_enabled);
    tick(s,p);p->cost_ms=8;
    for(unsigned n=0;n<100&&s->phase!=12;++n)fast_tick(s,p);
    assert(s->phase==12&&!s->copied&&!p->compact_reads&&!p->response_reads&&p->snapshot_begins==1);
}
static void compact_state(peer_t *p,uint8_t index,uint8_t state)
{
    assert(state<=2);
    if(state<2) {
        memset(&p->frozen_response[index],0,sizeof(p->frozen_response[index]));
        if(state==1)p->frozen_response[index]=(cerberus_dual_ble_response_wire_t){
            .version=1,.state=CERBERUS_DUAL_BLE_RESPONSE_NONE,.original_sequence=UINT64_C(0x123456789abcdef0)+index};
    }
}
static void assert_compact_bank(const cerberus_dual_ble_scan_t *s,const peer_t *p)
{
    assert(s->count==p->snapshot.count&&s->published_response);
    for(unsigned i=0;i<s->count;++i) {
        assert(!memcmp(&s->rows[i].response,&p->frozen_response[i],sizeof(s->rows[i].response)));
        assert(s->rows[i].request_started_ms==s->snapshot_request_started_ms);
        assert(s->rows[i].raw_length==p->frozen[i].raw_length&&s->rows[i].rssi==p->frozen[i].latest_rssi);
    }
}
static void check_compact_response_reader(void)
{
    cerberus_dual_ble_scan_t s;peer_t p;
    /* All six selectors plus ordinary response acquisition retain the exact
     * original158 prefix and reconstruct the same152-byte response object. */
    for(uint8_t family=0;family<=6;++family) {
        compact_read_begin(&s,&p,family,3);
        for(uint8_t i=0;i<3;++i)compact_state(&p,i,i);
        await_snapshots(&s,&p,1);
        assert(p.compact_reads==16U+18U+31U&&!p.response_reads&&!p.family_reads&&!p.metadata_reads&&!p.snapshot_reads);
        assert_compact_bank(&s,&p);assert(s.published_family==family);drain(&s,&p);
    }
    /* Every read position in a32-row mixed bank can return NOT_SUPPORTED.
     * All completed/partial staging is discarded, then the same frozen handle
     * is reread at row0 using full310. No public bank or age anchor is changed. */
    compact_read_begin(&s,&p,6,32);
    for(uint8_t i=0;i<32;++i)compact_state(&p,i,i%3U);
    s.count=1;s.rows[0].rssi=-117;s.published_family=2;s.published_response=false;
    cerberus_dual_ble_row_t saved=s.rows[0];unsigned positions=0,stop_positions=0;
    const unsigned expected=11U*16U+11U*18U+10U*31U;
    for(unsigned n=0;n<1000&&!s.snapshots;++n) {
        if(s.phase==12) {
            cerberus_dual_ble_scan_t copy=s;peer_t other=p;copy.ops.context=&other;
            uint64_t handle=copy.snapshot.handle,anchor=copy.snapshot_request_started_ms,request=copy.snapshot_request_id;
            other.compact_fail_enabled=true;other.compact_fail_after=other.compact_reads;
            copy.staging[31].rssi=99; /* Even an unused/old suffix is cleared. */
            unsigned begins=other.snapshot_begins,reads=other.compact_reads;
            fast_tick(&copy,&other);
            assert(!copy.compact_response_read_enabled&&!copy.read_index&&!copy.copied);
            assert(bytes_zero(copy.staging,sizeof(copy.staging))&&bytes_zero(&copy.snapshot_response,sizeof(copy.snapshot_response)));
            assert(copy.snapshot.handle==handle&&copy.snapshot_request_id==request&&copy.snapshot_request_started_ms==anchor);
            assert(copy.count==1&&!memcmp(&copy.rows[0],&saved,sizeof(saved))&&!copy.snapshots);
            await_snapshots(&copy,&other,1);
            assert(other.compact_reads==reads+1U&&other.response_reads==32U*31U);
            assert(other.starts==1&&other.snapshot_begins==begins&&other.handle_counter==1);
            assert_compact_bank(&copy,&other);drain(&copy,&other);++positions;
            /* Stop at that same partial state must not download one more chunk. */
            copy=s;other=p;copy.ops.context=&other;reads=other.compact_reads;
            drain(&copy,&other);
            assert(other.compact_reads==reads&&!other.response_reads&&!copy.snapshots&&
                !memcmp(&copy.rows[0],&saved,sizeof(saved)));++stop_positions;
        } else if(s.phase==13||s.phase==17) {
            cerberus_dual_ble_scan_t copy=s;peer_t other=p;copy.ops.context=&other;
            unsigned fences=other.snapshot_fences;drain(&copy,&other);
            assert(other.snapshot_fences==fences&&!copy.snapshots&&!memcmp(&copy.rows[0],&saved,sizeof(saved)));
        }
        unsigned calls=fast_tick(&s,&p);assert(calls<=1); /* Eight-ms local budget. */
        if(!s.snapshots)assert(s.count==1&&!memcmp(&s.rows[0],&saved,sizeof(saved)));
    }
    assert(s.snapshots==1&&p.compact_reads==expected&&positions==expected&&stop_positions==expected);
    assert(!p.response_reads);assert_compact_bank(&s,&p);drain(&s,&p);
    /* Full-read compatibility stays disabled for automatic sweeps, but a new
     * explicit public begin negotiates the optional projection again. */
    compact_read_begin(&s,&p,0,1);p.compact_supported=false;
    uint64_t anchor=s.snapshot_request_started_ms;
    await_snapshots(&s,&p,1);
    assert(!s.compact_response_read_enabled&&p.compact_reads==1&&p.response_reads==31&&s.rows[0].request_started_ms==anchor);
    await_snapshots(&s,&p,2);assert(p.compact_reads==1&&p.response_reads==62);
    completed(&p);for(unsigned n=0;n<1000&&p.starts<2;++n)fast_tick(&s,&p);
    assert(p.starts==2&&!s.compact_response_read_enabled&&p.compact_reads==1);
    drain(&s,&p);assert(cerberus_dual_ble_begin_response(&s,0)==ESP_OK&&s.compact_response_read_enabled);drain(&s,&p);
    /* Invalid header/framing/body/fence never silently chooses the old read. */
    for(unsigned fault=1;fault<=8;++fault)for(uint8_t state=0;state<3;++state) {
        if(fault==5&&state!=1)continue;
        compact_read_begin(&s,&p,0,1);compact_state(&p,0,state);p.compact_fault=fault;
        s.count=1;s.rows[0].rssi=-118;saved=s.rows[0];
        for(unsigned n=0;n<200&&!s.error;++n)fast_tick(&s,&p);
        assert(s.error==ESP_ERR_INVALID_RESPONSE&&!p.response_reads&&!s.snapshots&&s.compact_response_read_enabled);
        assert(!memcmp(&saved,&s.rows[0],sizeof(saved)));drain(&s,&p);
    }
    for(unsigned fault=9;fault<=12;++fault) {
        compact_read_begin(&s,&p,0,1);p.compact_fault=fault;
        s.count=1;s.rows[0].rssi=-120;saved=s.rows[0];
        for(unsigned n=0;n<200&&!s.error;++n)fast_tick(&s,&p);
        esp_err_t expected_error=fault==9?ESP_ERR_NOT_FOUND:fault==11?ESP_ERR_INVALID_RESPONSE:ESP_ERR_TIMEOUT;
        assert(s.error==expected_error&&!p.response_reads&&!s.snapshots&&s.compact_response_read_enabled);
        assert(!memcmp(&saved,&s.rows[0],sizeof(saved)));drain(&s,&p);
    }
    /* NONE's transmitted prefix must still be canonical, including its full
     * nonzero64-bit original sequence; omitted bytes cannot hide a bad prefix. */
    for(unsigned fault=0;fault<3;++fault) {
        compact_read_begin(&s,&p,0,1);compact_state(&p,0,1);
        if(!fault)p.frozen_response[0].original_sequence=0;
        else if(fault==1)p.frozen_response[0].flags=1;
        else p.frozen_response[0].rssi=-40;
        for(unsigned n=0;n<200&&!s.error;++n)fast_tick(&s,&p);
        assert(s.error==ESP_ERR_INVALID_RESPONSE&&!p.response_reads&&!s.snapshots);drain(&s,&p);
    }
    for(unsigned fault=BAD_FENCE_HANDLE;fault<=BAD_FENCE_FLAGS;++fault) {
        compact_read_begin(&s,&p,0,3);compact_state(&p,0,0);compact_state(&p,1,1);
        p.fault=fault;s.count=1;s.rows[0].rssi=-119;saved=s.rows[0];
        for(unsigned n=0;n<200&&!s.error;++n)fast_tick(&s,&p);
        assert(s.error==ESP_ERR_INVALID_RESPONSE&&!s.snapshots&&!p.response_reads&&
            !memcmp(&saved,&s.rows[0],sizeof(saved)));drain(&s,&p);
    }
    /* A canonical BUSY response retriescompact without downgrade; lostBEGIN
     * replies also retain the first request's age anchor and one frozen bank. */
    init_live(&s,&p);p.response_supported=p.compact_supported=true;p.lose_begin_replies=2;p.busy_read=2;
    assert(cerberus_dual_ble_begin_response(&s,0)==ESP_OK);
    await_snapshots(&s,&p,1);
    assert(p.snapshot_begins==3&&p.handle_counter==1&&!p.response_reads&&s.compact_response_read_enabled);
    assert(s.rows[0].request_started_ms+1==p.snapshot_clock_ms);drain(&s,&p);
    puts("Compact response reader: 32 mixed rows, 16/18/31 chunks, all 684 fallback/Stop positions, same-handle anchor, canonical reconstruction, strict wire/fence, per-run downgrade and no passive fallback passed");
}
int main(void)
{
    cerberus_dual_ble_scan_t s;peer_t p;
    init(&s,&p);start(&s,&p);completed(&p);
    for (unsigned i=0;i<50 && !s.sweeps;++i)tick(&s,&p);
    assert(s.sweeps==1 && s.count==2 && s.rows[0].address_type==0 && s.rows[1].address_type==1);
    assert(!memcmp(s.rows[0].address,s.rows[1].address,6)); /* Same bytes, distinct typed identities. */
    assert(s.rows[0].rssi==-80 && !strcmp(s.rows[0].name,"TEST")); /* Latest callback, not -20 peak. */
    assert(s.rows[0].raw_length==3 && s.rows[0].raw[2]==6 && s.rows[0].reports==7);
    assert(bytes_zero(&s.rows[0].receive,sizeof(s.rows[0].receive)));
    assert(cerberus_dual_ble_age_upper(s.rows[0].callback_age_ms,s.rows[0].request_started_ms,p.now)>=7000);
    drain(&s,&p);
    cerberus_dual_ble_row_t saved=s.rows[0];
    assert(cerberus_dual_ble_begin(&s,0,NULL,0)==ESP_OK);tick(&s,&p);completed(&p);p.corrupt_data=true;
    for(unsigned i=0;i<40 && !s.error;++i)tick(&s,&p);
    assert(s.error==ESP_ERR_INVALID_RESPONSE && s.count==2 && !memcmp(&saved,&s.rows[0],sizeof(saved)));
    drain(&s,&p);

    init(&s,&p);start(&s,&p);completed(&p);p.corrupt_fence=true;
    for(unsigned i=0;i<20 && !s.error;++i)tick(&s,&p);
    assert(s.error==ESP_ERR_INVALID_RESPONSE && !s.count);p.corrupt_fence=false;drain(&s,&p);

    init(&s,&p);p.lose_start=true;p.fail_stop=true;assert(cerberus_dual_ble_begin(&s,0,NULL,0)==ESP_OK);
    for(unsigned i=0;i<30 && !s.error;++i)tick(&s,&p);
    assert(s.error==ESP_ERR_TIMEOUT && p.starts==1 && !s.count);
    p.fail_stop=true;for(unsigned i=0;i<8;++i)tick(&s,&p);
    assert(!cerberus_dual_ble_released(&s) && p.owned && !p.releases);
    p.fail_stop=false;drain(&s,&p); /* Lost START replies still authorize only bound cleanup. */

    init(&s,&p);start(&s,&p);p.reset=true;
    for(unsigned i=0;i<8;++i)tick(&s,&p);
    assert(cerberus_dual_ble_released(&s) && s.error && !s.enabled);
    init(&s,&p);p.busy=true;assert(cerberus_dual_ble_begin(&s,0,NULL,0)==ESP_OK);tick(&s,&p);
    assert(s.error==ESP_ERR_INVALID_STATE && !p.starts && !p.stops);
    init(&s,&p);p.unsupported=true;assert(cerberus_dual_ble_begin(&s,0,NULL,0)==ESP_OK);tick(&s,&p);
    assert(s.error==ESP_ERR_NOT_SUPPORTED && !p.starts && !p.stops);

    init(&s,&p);p.count=0;start(&s,&p);completed(&p);
    for(unsigned i=0;i<20 && !s.sweeps;++i)tick(&s,&p);
    assert(s.sweeps==1 && !s.count);drain(&s,&p);

    const uint8_t target[6]={1,2,3,4,5,6};
    init(&s,&p);assert(cerberus_dual_ble_begin(&s,1,target,3)==ESP_OK);
    for(unsigned i=0;i<8 && !s.has_track;++i)tick(&s,&p);
    cerberus_dual_ble_track_view_t v;cerberus_dual_ble_track_view(&s,p.now,&v);
    assert(v.seen && v.fresh && v.rssi==-75 && v.age_upper_ms>=40);
    cerberus_dual_ble_track_view(&s,p.now+5001,&v);assert(v.seen && !v.fresh);
    cerberus_dual_ble_track_view(&s,s.track_request_ms-1,&v);assert(v.age_upper_ms==UINT32_MAX && !v.fresh);
    drain(&s,&p);
    assert(cerberus_dual_ble_age_upper(UINT32_MAX-1,10,12)==UINT32_MAX);
    assert(cerberus_dual_ble_age_upper(2,10,9)==UINT32_MAX);
    assert(cerberus_dual_ble_age_upper(2,10,12)==4);
    check_live_snapshots();check_snapshot_faults_and_stop();check_published_provenance();check_inventory_metadata();
    check_receive_metadata();
    check_analyzer_history();check_analyzer_controller();
    check_family_reader();
    check_response_reader();
    check_compact_response_reader();
    printf("dual BLE reader: owner/replay/fence/lost-reply/age/typed-row checks passed; store=%zu row=%zu wire=%zu scratch=%zu bytes\n",
        sizeof(s),sizeof(s.rows[0]),sizeof(s.rows[0].response),sizeof(s.snapshot_response));
    return 0;
}
