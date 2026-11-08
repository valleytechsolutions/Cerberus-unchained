#include "cerberus_protocol.h"
#include "esp_err.h"
#include "ble_inventory_admission.h"
#include "dual_ble_protocol.h"
#include "ble_analyzer_counter.h"
#include "ble_family_wire.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "ble_response.h"
#define MYNEWT_VAL(x) 1
#define pdTRUE 1
#define CERBERUS_BLE_NAME_MAX_LENGTH 32U
#define CERBERUS_BLE_UUID_MAX_LENGTH 16U
#define CERBERUS_BLE_ADVERTISEMENT_MAX_LENGTH 31U
#define portMAX_DELAY 0xffffffffU
#define BLE_HCI_ADV_RPT_EVTYPE_ADV_IND 0
#define BLE_HCI_ADV_RPT_EVTYPE_DIR_IND 1
#define BLE_HCI_ADV_RPT_EVTYPE_NONCONN_IND 3
#define BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP 4
#define BLE_HCI_ADV_CONN_MASK 1
#define BLE_HCI_ADV_SCAN_RSP_MASK 8
#define BLE_HCI_ADV_LEGACY_MASK 16
#define BLE_GAP_EXT_ADV_DATA_STATUS_COMPLETE 0
enum { BLE_GAP_EVENT_DISC=1, BLE_GAP_EVENT_EXT_DISC, BLE_GAP_EVENT_DISC_COMPLETE };
typedef struct {uint8_t val[6],type;} address_t;
struct ble_gap_disc_desc {address_t addr;int8_t rssi;uint8_t event_type;
    const uint8_t *data;uint8_t length_data;};
/* Scalar callback fields match the installed NimBLE ble_gap.h. In particular,
 * report props and delivered lengths are uint8_t, not wider invented fields. */
struct ble_gap_ext_disc_desc {uint8_t props,data_status,legacy_event_type;address_t addr;
    int8_t rssi,tx_power;uint8_t sid,prim_phy,sec_phy;uint16_t periodic_adv_itvl;
    uint8_t length_data;const uint8_t *data;address_t direct_addr;};
struct ble_gap_event {int type;struct ble_gap_disc_desc disc;struct ble_gap_ext_disc_desc ext_disc;
    struct {int reason;} disc_complete;};
struct test_uuid16 {uint16_t value;};
struct test_uuid32 {uint32_t value;};
struct test_uuid128 {uint8_t value[16];};
struct ble_hs_adv_fields {const uint8_t *name,*mfg_data;uint8_t name_len,mfg_data_len;
 const struct test_uuid16 *uuids16;uint8_t num_uuids16;
 const struct test_uuid32 *uuids32;uint8_t num_uuids32;
 const struct test_uuid128 *uuids128;uint8_t num_uuids128;};
#include "c5_ble_record_type.h"
#include "ble_response_state.h"
typedef struct {int unused;} sentinel_watch_protocol_record_t;
static struct {struct {uint16_t identities;} status;} activity_capture;
static unsigned watch_capture;
static cerberus_ble_record_t scan_results[32];
static cerberus_ble_scan_status_payload_t scan_status;
static uint32_t scan_callback_epoch;
#include "c5_ble_dual_state.h"
static bool host_synced;
static void *scanner_mutex=(void*)1;
static unsigned held, starts, stops, parse_calls, mutex_takes, response_mode_checks;
static uint64_t now_ms;
static bool other_radio_busy, cancel_fails, snapshot_busy, parse_fails;
static unsigned try_locks;
static int64_t esp_timer_get_time(void) {return (int64_t)(now_ms*1000U);}
static int xSemaphoreTake(void *m,unsigned wait)
{(void)m;++mutex_takes;if(!wait){++try_locks;if(snapshot_busy||held)return 0;}assert(!held);held=1;return 1;}
static void xSemaphoreGive(void *m) {(void)m;assert(held);held=0;}
static bool allocation_fails;
static unsigned response_allocations,response_frees;
static void *response_test_calloc(size_t n,size_t size)
{ if(allocation_fails)return NULL;void *p=calloc(n,size);if(p)++response_allocations;return p; }
static void response_test_free(void *p){if(p)++response_frees;free(p);}
#define calloc response_test_calloc
#define free response_test_free
static esp_err_t start_scan(bool companion)
{
    assert(companion && dual_attached && !held);
    if(other_radio_busy||scan_callback_epoch==UINT32_MAX)return ESP_ERR_INVALID_STATE;
    ++starts;++scan_status.generation;dual_generation=scan_status.generation;
    ++scan_callback_epoch;scan_status.state=CERBERUS_BLE_SCAN_RUNNING;
    scan_status.stored_count=0;scan_status.error_code=0;dual_radio_active=true;
    scan_status.dropped_reports=0;
    if(cerberus_dual_ble_family_mode(dual_mode))memset(scan_family,0,sizeof(scan_family));
    if(dual_response)memset(dual_response,0,sizeof(*dual_response));
    memset(scan_results,0,sizeof(scan_results));
    cerberus_ble_inventory_admission_begin(&scan_inventory,
        dual_version==CERBERUS_DUAL_BLE_VERSION&&
            cerberus_dual_ble_snapshot_mode(dual_mode),
        esp_timer_get_time());
    cerberus_ble_analyzer_counter_reset(&dual_analyzer);
    if(dual_mode==CERBERUS_DUAL_BLE_ANALYZER){
        cerberus_ble_analyzer_counter_arm(&dual_analyzer,scan_callback_epoch,esp_timer_get_time());
        cerberus_ble_analyzer_counter_accept(&dual_analyzer,true);
    }
    return ESP_OK;
}
static esp_err_t stop_scan(bool companion)
{
    assert(companion && dual_attached && !held);++stops;
    if(cancel_fails){dual_error=ESP_FAIL;return ESP_FAIL;}
    dual_radio_active=false;scan_status.state=CERBERUS_BLE_SCAN_IDLE;
    cerberus_ble_analyzer_counter_finish(&dual_analyzer,esp_timer_get_time());return ESP_OK;
}
static int ble_hs_adv_parse_fields(struct ble_hs_adv_fields *f,const void *d,unsigned n)
{++parse_calls;if(parse_fails)return 1;const uint8_t *p=d;static struct test_uuid16 uuid;
 for(unsigned i=0;i<n;){unsigned k=p[i++];if(!k)break;if(k>n-i)return 1;
 if((p[i]==8||p[i]==9)&&k>1){f->name=p+i+1;f->name_len=(uint8_t)(k-1);}
 if(p[i]==0xff&&k>=3){f->mfg_data=p+i+1;f->mfg_data_len=(uint8_t)(k-1);}
 if((p[i]==2||p[i]==3)&&k>=3){uuid.value=(uint16_t)p[i+1]|((uint16_t)p[i+2]<<8);f->uuids16=&uuid;f->num_uuids16=1;}
 i+=k;}return 0;}
static void cerberus_ble_activity_observe(void *a,const uint8_t *b,uint8_t t,const void *d,unsigned n,bool valid,uint32_t now)
{(void)a;(void)b;(void)t;(void)d;(void)n;(void)valid;(void)now;}
static bool sentinel_watch_protocol_decode_ble(const void *d,unsigned n,void *p)
{(void)d;(void)n;(void)p;return false;}
static void cerberus_watch_capture_observe_timed(void *a,const void *b,const uint8_t *c,uint8_t t,int8_t r,uint8_t x,uint64_t now)
{(void)a;(void)b;(void)c;(void)t;(void)r;(void)x;(void)now;}
static void cerberus_watch_capture_observe(void *a,const void *b,const uint8_t *c,uint8_t t,int8_t r,uint8_t x)
{(void)a;(void)b;(void)c;(void)t;(void)r;(void)x;}
static int find_record(const address_t *a)
{for(unsigned i=0;i<scan_status.stored_count;++i)if(scan_results[i].address_type==a->type && !memcmp(scan_results[i].address,a->val,6))return (int)i;return -1;}
static size_t bounded_size(size_t value,size_t limit){return value<limit?value:limit;}
#include "c5_ble_copy_fields_impl.h"
/* Count the real mode predicate reached by the production callback, without
 * replacing response handling, its mutex, or the SDK receive switch. */
static bool checked_response_mode(uint8_t mode)
{ ++response_mode_checks; return cerberus_dual_ble_response_mode(mode); }
#define cerberus_dual_ble_response_mode checked_response_mode
#include "c5_ble_callback_impl.h"
static void finish_scan_for_epoch(int reason,uint32_t epoch)
{if(epoch&&epoch==scan_callback_epoch){dual_radio_active=false;
 cerberus_ble_analyzer_counter_finish(&dual_analyzer,esp_timer_get_time());
 scan_status.state=reason?CERBERUS_BLE_SCAN_ERROR:CERBERUS_BLE_SCAN_COMPLETE;response_error_locked();}}
#include "c5_ble_gap_receive_impl.h"
#undef cerberus_dual_ble_response_mode
#include "dual_ble_service_impl.h"

static void command(uint8_t cmd,const void *request,size_t n,void *reply,size_t expected)
{size_t size=0;assert(cerberus_ble_dual_command(cmd,request,n,reply,&size));assert(size==expected);assert(!held);}

static cerberus_dual_ble_snapshot_status_t snapshot_begin(cerberus_dual_ble_binding_t b,uint64_t id)
{cerberus_dual_ble_snapshot_begin_t r={.binding=b,.request_id=id};cerberus_dual_ble_snapshot_status_t out;
 command(CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_BEGIN,&r,sizeof(r),&out,sizeof(out));return out;}
static cerberus_dual_ble_snapshot_status_t snapshot_status(cerberus_dual_ble_binding_t b,uint64_t handle)
{cerberus_dual_ble_snapshot_binding_t r={.binding=b,.handle=handle};cerberus_dual_ble_snapshot_status_t out;
 command(CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_STATUS,&r,sizeof(r),&out,sizeof(out));return out;}
static cerberus_dual_ble_snapshot_inventory_t snapshot_inventory(cerberus_dual_ble_binding_t b,uint64_t handle)
{cerberus_dual_ble_snapshot_binding_t r={.binding=b,.handle=handle};cerberus_dual_ble_snapshot_inventory_t out;
 command(CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_INVENTORY,&r,sizeof(r),&out,sizeof(out));return out;}
static cerberus_dual_ble_snapshot_chunk_t snapshot_chunk(cerberus_dual_ble_binding_t b,uint64_t handle,uint8_t index,uint8_t chunk)
{cerberus_dual_ble_snapshot_read_t r={.binding=b,.handle=handle,.index=index,.chunk=chunk};cerberus_dual_ble_snapshot_chunk_t out;
 command(CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ,&r,sizeof(r),&out,sizeof(out));
 assert(out.handle==handle&&out.index==index&&out.chunk==chunk);
 if(out.error){assert(!out.byte_count);for(unsigned i=0;i<10;++i)assert(!out.data[i]);}return out;}
static cerberus_dual_ble_snapshot_row_t snapshot_row(cerberus_dual_ble_binding_t b,uint64_t handle,uint8_t index)
{cerberus_dual_ble_snapshot_row_t row={0};for(uint8_t i=0;i<CERBERUS_DUAL_BLE_SNAPSHOT_CHUNKS;++i){
 cerberus_dual_ble_snapshot_chunk_t c=snapshot_chunk(b,handle,index,i);assert(!c.error);
 size_t n=i==10?2U:10U;assert(c.byte_count==n);memcpy((uint8_t*)&row+i*10U,c.data,n);
 for(size_t j=n;j<10U;++j)assert(!c.data[j]);}return row;}
static cerberus_dual_ble_snapshot_metadata_row_t snapshot_metadata_row(
    cerberus_dual_ble_binding_t b,uint64_t handle,uint8_t index)
{
    cerberus_dual_ble_snapshot_metadata_row_t row={0};
    for(uint8_t chunk=0;chunk<CERBERUS_DUAL_BLE_SNAPSHOT_METADATA_CHUNKS;++chunk){
        cerberus_dual_ble_snapshot_read_t request={b,handle,index,chunk};
        cerberus_dual_ble_snapshot_chunk_t reply;
        command(CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_METADATA,&request,sizeof(request),&reply,sizeof(reply));
        assert(!reply.error&&reply.handle==handle&&reply.index==index&&reply.chunk==chunk&&reply.byte_count==10);
        memcpy((uint8_t*)&row+chunk*10U,reply.data,10);
    }
    cerberus_dual_ble_snapshot_row_t old=snapshot_row(b,handle,index);
    assert(!memcmp(&row.row,&old,sizeof(old))); /* Old READ remains102 with final8zero padding. */
    assert(cerberus_dual_ble_receive_metadata_valid(&row.receive,row.row.raw_length,row.row.event_type,row.row.raw_flags));
    return row;
}

static void callback_metadata_checks(void)
{
    _Static_assert(sizeof(((struct ble_gap_ext_disc_desc*)0)->props)==1,"SDK props width");
    _Static_assert(sizeof(((struct ble_gap_ext_disc_desc*)0)->length_data)==1,"SDK report length width");
    dual_attached=dual_radio_active=false;scan_status.state=CERBERUS_BLE_SCAN_IDLE;dual_error=0;now_ms=100000;
    cerberus_dual_ble_start_t start={.boot_id=dual_boot_id,.job_id=dual_job_highwater+1U,.version=2};
    cerberus_dual_ble_status_t owner;
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_START,&start,sizeof(start),&owner,sizeof(owner));
    assert(!owner.error_code&&dual_radio_active);
    cerberus_dual_ble_binding_t binding={dual_boot_id,start.job_id};uint64_t request=0;
    struct ble_gap_event event={.type=BLE_GAP_EVENT_DISC,
        .disc={.addr={{22,2,3,4,5,6},1},.rssi=-37,.event_type=0}};
    uint8_t payload[255]={0};const uint8_t lengths[]={0,31,32,255};
    for(unsigned extended=0;extended<2;++extended)for(unsigned n=0;n<sizeof(lengths);++n){
        uint8_t length=lengths[n];memset(payload,0,sizeof(payload));
        if(length){payload[0]=(uint8_t)(length-1U);payload[1]=0xff;payload[2]=0x4c;}
        event.type=extended?BLE_GAP_EVENT_EXT_DISC:BLE_GAP_EVENT_DISC;
        event.disc.length_data=length;event.disc.data=length?payload:NULL;
        event.ext_disc=(struct ble_gap_ext_disc_desc){.addr=event.disc.addr,.rssi=-37,
            .sid=7,.prim_phy=3,.sec_phy=1,.length_data=length,.data=length?payload:NULL};
        ++now_ms;assert(!gap_event(&event,(void*)(uintptr_t)scan_callback_epoch));
        cerberus_dual_ble_snapshot_status_t status=snapshot_begin(binding,++request);assert(!status.error_code&&status.count==1);
        cerberus_dual_ble_snapshot_metadata_row_t frozen=snapshot_metadata_row(binding,status.handle,0);
        assert(frozen.row.raw_length==(length>31?31:length));
        if(!extended&&length>31){cerberus_dual_ble_receive_metadata_t zero={0};assert(!memcmp(&frozen.receive,&zero,sizeof(zero)));}
        else {
            assert(frozen.receive.source_kind==(extended?CERBERUS_DUAL_BLE_RECEIVE_EXTENDED:CERBERUS_DUAL_BLE_RECEIVE_LEGACY));
            assert(frozen.receive.callback_length==length&&frozen.receive.ad_validation==CERBERUS_DUAL_BLE_AD_VALID);
            assert(frozen.receive.sid==(extended?7:255)&&frozen.receive.data_status==(extended?0:255));
            assert(frozen.receive.primary_phy==(extended?3:0)&&frozen.receive.secondary_phy==(extended?1:0));
        }
        if(length>31)assert(frozen.row.raw_flags==3); /* Cut AD preview; full received AD still independently VALID. */
        else assert(!frozen.row.raw_flags);
    }
    event.type=BLE_GAP_EVENT_EXT_DISC;event.ext_disc.length_data=31;event.ext_disc.data=payload;
    memset(payload,0,sizeof(payload));payload[0]=31;payload[1]=0xff;payload[2]=0x4c;
    ++now_ms;assert(!gap_event(&event,(void*)(uintptr_t)scan_callback_epoch));
    cerberus_dual_ble_snapshot_status_t status=snapshot_begin(binding,++request);
    cerberus_dual_ble_snapshot_metadata_row_t malformed=snapshot_metadata_row(binding,status.handle,0);
    assert(malformed.receive.ad_validation==CERBERUS_DUAL_BLE_AD_INVALID&&malformed.row.raw_flags==2);
    payload[0]=30;
    parse_fails=true;++now_ms;assert(!gap_event(&event,(void*)(uintptr_t)scan_callback_epoch));parse_fails=false;
    status=snapshot_begin(binding,++request);
    malformed=snapshot_metadata_row(binding,status.handle,0);
    assert(malformed.receive.ad_validation==CERBERUS_DUAL_BLE_AD_VALID&&malformed.row.raw_flags==2);
    /* Full-container layout validation stays independent of NimBLE parser failure. */
    const uint8_t props[]={0x10,0x12,0x13,0x15},events[]={3,2,0,1};
    for(unsigned i=0;i<sizeof(props);++i){
        event.ext_disc.props=props[i];event.ext_disc.legacy_event_type=events[i];
        event.ext_disc.sid=255;event.ext_disc.prim_phy=1;event.ext_disc.sec_phy=0;
        ++now_ms;assert(!gap_event(&event,(void*)(uintptr_t)scan_callback_epoch));
        status=snapshot_begin(binding,++request);
        cerberus_dual_ble_snapshot_metadata_row_t row=snapshot_metadata_row(binding,status.handle,0);
        assert(row.receive.source_kind==CERBERUS_DUAL_BLE_RECEIVE_EXTENDED&&row.receive.properties==props[i]);
        assert(row.row.event_type==events[i]&&row.receive.sid==255&&!row.receive.data_status);
    }
    /* Full source metadata unknown/invalid never invents a PHY/SID. Raw
     * admission stays separate and old immutable handle metadata stays fixed. */
    event.ext_disc.props=0;event.ext_disc.prim_phy=3;event.ext_disc.sec_phy=2;event.ext_disc.sid=15;
    ++now_ms;assert(!gap_event(&event,(void*)(uintptr_t)scan_callback_epoch));
    status=snapshot_begin(binding,++request);
    cerberus_dual_ble_snapshot_metadata_row_t painted=snapshot_metadata_row(binding,status.handle,0);
    const uint64_t painted_handle=status.handle;
    for(unsigned fault=0;fault<5;++fault){
        struct ble_gap_event bad=event;
        if(fault==0)bad.ext_disc.sid=16;
        if(fault==1)bad.ext_disc.prim_phy=2;
        if(fault==2)bad.ext_disc.sec_phy=4;
        if(fault==3)bad.ext_disc.props=0x20;
        if(fault==4){bad.ext_disc.props=0x10;bad.ext_disc.legacy_event_type=0;}
        ++now_ms;assert(!gap_event(&bad,(void*)(uintptr_t)scan_callback_epoch));
        cerberus_dual_ble_snapshot_metadata_row_t same=snapshot_metadata_row(binding,painted_handle,0);
        assert(!memcmp(&same,&painted,sizeof(same)));
    }
    status=snapshot_begin(binding,++request);
    cerberus_dual_ble_snapshot_metadata_row_t unknown=snapshot_metadata_row(binding,status.handle,0);
    cerberus_dual_ble_receive_metadata_t zero={0};assert(!memcmp(&unknown.receive,&zero,sizeof(zero)));
    /* Partial/controller-truncated, responses and stale callbacks cannot
     * replace the raw record or turn its metadata into a newer observation. */
    cerberus_ble_record_t saved=scan_results[0];
    for(unsigned fault=0;fault<5;++fault){
        struct ble_gap_event excluded=event;void *token=(void*)(uintptr_t)scan_callback_epoch;
        if(fault<2)excluded.ext_disc.data_status=(uint8_t)(fault+1U);
        if(fault==2)excluded.ext_disc.props=BLE_HCI_ADV_SCAN_RSP_MASK;
        if(fault==3)token=(void*)(uintptr_t)(scan_callback_epoch-1U);
        if(fault==4){excluded.ext_disc.props=0x18;excluded.ext_disc.legacy_event_type=4;}
        ++now_ms;assert(!gap_event(&excluded,token));assert(!memcmp(&saved,&scan_results[0],sizeof(saved)));
    }
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_STOP,&binding,sizeof(binding),&owner,sizeof(owner));
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_RELEASE,&binding,sizeof(binding),&owner,sizeof(owner));
    puts("C5 actual legacy/EXT callback metadata: SDK widths, full AD vs cut preview, legacy-PDU EXT, unknown normalization, exclusions, immutable102/110 wire passed");
}

static void snapshot_checks(void)
{
    memset(&dual_snapshot,0,sizeof(dual_snapshot));dual_snapshot.counter=dual_boot_id;
    cerberus_dual_ble_start_t start={.boot_id=dual_boot_id,.job_id=dual_job_highwater+1U,.version=2};
    cerberus_dual_ble_status_t owner;
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_START,&start,sizeof(start),&owner,sizeof(owner));
    assert(!owner.error_code&&owner.version==2&&(owner.flags&CERBERUS_DUAL_BLE_SNAPSHOTS));
    cerberus_dual_ble_binding_t b={dual_boot_id,start.job_id},wrong=b;
    unsigned start_before=starts,stop_before=stops;
    uint8_t raw[]={2,1,6};
    struct ble_gap_disc_desc p={.addr={{10,2,3,4,5,6},1},.rssi=-30,.event_type=0,.data=raw,.length_data=3};
    now_ms=1000;record_advertisement(&p,scan_callback_epoch,990000);
    memcpy(scan_results[0].name,"Peer",4);scan_results[0].name_length=4;
    scan_results[0].uuid_length=2;scan_results[0].uuid[0]=0xaa;scan_results[0].uuid[1]=0xfe;
    scan_results[0].manufacturer_id=0x4c;
    snapshot_busy=true;
    assert(snapshot_begin(b,1).error_code==ESP_ERR_TIMEOUT&&!dual_snapshot.valid);
    snapshot_busy=false;
    cerberus_dual_ble_snapshot_status_t first=snapshot_begin(b,1);
    assert(!first.error_code&&first.boot_id==b.boot_id&&first.handle==b.boot_id+1&&first.count==1);
    assert(first.flags==(CERBERUS_DUAL_BLE_SNAPSHOT_RUNNING|CERBERUS_DUAL_BLE_SNAPSHOT_EXTENDED));
    cerberus_dual_ble_snapshot_row_t frozen=snapshot_row(b,first.handle,0);
    assert(frozen.latest_rssi==-30&&frozen.age_ms==10&&frozen.timing_flags==3&&frozen.reports==1);
    assert(frozen.name_length==4&&!memcmp(frozen.name,"Peer",4)&&frozen.name[4]==0);
    assert(frozen.raw_length==3&&!memcmp(frozen.raw,raw,3)&&frozen.raw[3]==0);
    /* Real held-mutex admission also leaves the current handle unchanged. */
    held=1;cerberus_dual_ble_snapshot_begin_t contended={.binding=b,.request_id=2};
    cerberus_dual_ble_snapshot_status_t blocked;size_t blocked_size=0;
    assert(cerberus_ble_dual_command(CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_BEGIN,
        &contended,sizeof(contended),&blocked,&blocked_size));
    assert(blocked_size==22&&blocked.error_code==ESP_ERR_TIMEOUT&&held==1&&dual_snapshot.handle==first.handle);held=0;
    cerberus_dual_ble_snapshot_status_t malformed;
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_BEGIN,&contended,sizeof(contended)-1,&malformed,22);
    assert(malformed.error_code==ESP_ERR_INVALID_ARG&&dual_snapshot.handle==first.handle);
    /* A callback can update the live row between ANY read chunks; every frozen
       field, including its first BEGIN age, must remain the original version. */
    raw[2]=7;p.rssi=-90;now_ms=1500;record_advertisement(&p,scan_callback_epoch,1490000);
    cerberus_dual_ble_snapshot_status_t retry=snapshot_begin(b,1);
    assert(!memcmp(&first,&retry,sizeof(first)));
    cerberus_dual_ble_snapshot_row_t reread=snapshot_row(b,first.handle,0);
    assert(!memcmp(&frozen,&reread,sizeof(frozen)));
    assert(snapshot_begin(b,3).error_code==ESP_ERR_INVALID_STATE);
    assert(snapshot_begin(b,0).error_code==ESP_ERR_INVALID_STATE);
    wrong.boot_id++;assert(snapshot_begin(wrong,2).error_code==ESP_ERR_NOT_FOUND);
    wrong=b;wrong.job_id++;assert(snapshot_status(wrong,first.handle).error_code==ESP_ERR_NOT_FOUND);
    assert(snapshot_chunk(wrong,first.handle,0,0).error==CERBERUS_DUAL_BLE_SNAPSHOT_STALE);
    assert(snapshot_chunk(b,first.handle,1,0).error==CERBERUS_DUAL_BLE_SNAPSHOT_RANGE);
    assert(snapshot_chunk(b,first.handle,0,11).error==CERBERUS_DUAL_BLE_SNAPSHOT_RANGE);
    snapshot_busy=true;
    assert(snapshot_status(b,first.handle).error_code==ESP_ERR_TIMEOUT);
    assert(snapshot_chunk(b,first.handle,0,0).error==CERBERUS_DUAL_BLE_SNAPSHOT_BUSY);
    snapshot_busy=false;
    uint64_t epoch=dual_epoch;dual_epoch++;
    assert(snapshot_status(b,first.handle).error_code==ESP_ERR_NOT_FOUND);dual_epoch=epoch;
    cerberus_dual_ble_snapshot_status_t second=snapshot_begin(b,2);
    assert(!second.error_code&&second.handle==first.handle+1);
    assert(snapshot_status(b,first.handle).error_code==ESP_ERR_NOT_FOUND);
    assert(snapshot_begin(b,1).error_code==ESP_ERR_INVALID_STATE);
    reread=snapshot_row(b,second.handle,0);assert(reread.latest_rssi==-90&&reread.reports==2&&reread.raw[2]==7);
    /* Peer aggregate lengths are checked before replacing a valid bank. */
    scan_results[0].name_length=33;assert(snapshot_begin(b,3).error_code==ESP_ERR_INVALID_RESPONSE);
    assert(snapshot_status(b,second.handle).error_code==ESP_OK);scan_results[0].name_length=4;
    scan_status.stored_count=33;assert(snapshot_begin(b,3).error_code==ESP_ERR_INVALID_RESPONSE);scan_status.stored_count=1;
    /* Wrap-safe callback age, unknown time/RSSI, and all32 retained rows. */
    scan_status.stored_count=32;for(unsigned i=1;i<32;++i)scan_results[i]=scan_results[0];
    now_ms=(uint64_t)UINT32_MAX+6U;scan_results[0].original_observed_ms=UINT32_MAX-4U;
    scan_results[1].original_time_valid=false;scan_results[1].latest_rssi=127;
    scan_results[2].advertisement_flags=CERBERUS_BLE_ADVERTISEMENT_MALFORMED|CERBERUS_BLE_ADVERTISEMENT_TRUNCATED;
    cerberus_dual_ble_snapshot_status_t third=snapshot_begin(b,3);assert(!third.error_code&&third.count==32);
    reread=snapshot_row(b,third.handle,0);assert(reread.age_ms==10);
    reread=snapshot_row(b,third.handle,1);assert(reread.age_ms==UINT32_MAX&&!reread.timing_flags);
    reread=snapshot_row(b,third.handle,2);assert(reread.raw_flags==3);
    reread=snapshot_row(b,third.handle,31);assert(reread.name_length==4);
    uint64_t saved_counter=dual_snapshot.counter;dual_snapshot.counter=UINT64_MAX;
    assert(snapshot_begin(b,4).error_code==ESP_ERR_INVALID_STATE);
    assert(!snapshot_begin(b,3).error_code);dual_snapshot.counter=saved_counter;
    uint64_t saved_request=dual_snapshot.request_id;dual_snapshot.request_id=UINT64_MAX;
    assert(!snapshot_begin(b,UINT64_MAX).error_code);
    assert(snapshot_begin(b,1).error_code==ESP_ERR_INVALID_STATE);dual_snapshot.request_id=saved_request;
    assert(starts==start_before&&stops==stop_before); /* Snapshot never changes RF. */
    /* A failed Stop retains the exact RF owner and cannot authorize a foreign
       release. The frozen bank remains private, while reads report its error. */
    cancel_fails=true;
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_STOP,&b,sizeof(b),&owner,sizeof(owner));
    assert(owner.error_code&&(owner.flags&CERBERUS_DUAL_BLE_OWNED)&&dual_attached&&dual_snapshot.valid);
    assert(snapshot_status(b,third.handle).error_code==ESP_FAIL);
    cancel_fails=false;dual_error=0;
    /* A completion is a new immutable snapshot; old handle stays RUNNING until
       replaced and never silently turns into a final bank. */
    dual_radio_active=false;scan_status.state=CERBERUS_BLE_SCAN_COMPLETE;
    assert(snapshot_status(b,third.handle).flags&CERBERUS_DUAL_BLE_SNAPSHOT_RUNNING);
    cerberus_dual_ble_snapshot_status_t final=snapshot_begin(b,4);
    assert(!final.error_code&&(final.flags&CERBERUS_DUAL_BLE_SNAPSHOT_COMPLETE));
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_STOP,&wrong,sizeof(wrong),&owner,sizeof(owner));
    assert(snapshot_status(b,final.handle).error_code==ESP_OK);
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_STOP,&b,sizeof(b),&owner,sizeof(owner));
    assert(snapshot_status(b,final.handle).error_code==ESP_ERR_NOT_FOUND);
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_RELEASE,&b,sizeof(b),&owner,sizeof(owner));
    assert(!dual_attached&&!dual_snapshot.valid);
    /* A subsequent job cannot adopt old handles; completed orphan expiry
       discards a valid snapshot without affecting any active RF ownership. */
    start.job_id=dual_job_highwater+1U;
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_START,&start,sizeof(start),&owner,sizeof(owner));
    b.job_id=start.job_id;scan_status.stored_count=1;
    cerberus_dual_ble_snapshot_status_t orphan=snapshot_begin(b,1);assert(!orphan.error_code);
    assert(snapshot_status(b,final.handle).error_code==ESP_ERR_NOT_FOUND);
    now_ms+=CERBERUS_DUAL_BLE_ORPHAN_MS+1U;
    cerberus_dual_ble_info_t info;
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_INFO,NULL,0,&info,22);
    assert(dual_attached&&dual_radio_active&&dual_snapshot.valid);
    dual_radio_active=false;scan_status.state=CERBERUS_BLE_SCAN_COMPLETE;
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_INFO,NULL,0,&info,22);
    assert(!dual_attached&&!dual_snapshot.valid);
    assert(snapshot_status(b,orphan.handle).error_code==ESP_ERR_NOT_FOUND);
    printf("C5 snapshot memory: %zu frozen rows + %zu metadata = %zu bytes; %u try-locks.\n",
        sizeof(dual_snapshot.rows),sizeof(dual_snapshot)-sizeof(dual_snapshot.rows),sizeof(dual_snapshot),try_locks);
}
static void dense_inventory_checks(void)
{
    const uint8_t flags[]={2,1,6}, bad_width[]={2,3,0x44}, bad_suffix[]={2,1,6,0,1};
    const uint8_t duplicate_name[]={2,9,'A',2,9,'B'}, short_mfg[]={2,0xff,0x4c};
    assert(cerberus_ble_inventory_ad_valid(NULL,0));
    assert(cerberus_ble_inventory_ad_valid(flags,sizeof(flags)));
    assert(!cerberus_ble_inventory_ad_valid(NULL,1));
    assert(!cerberus_ble_inventory_ad_valid(flags,2));
    assert(!cerberus_ble_inventory_ad_valid(bad_width,sizeof(bad_width)));
    assert(!cerberus_ble_inventory_ad_valid(bad_suffix,sizeof(bad_suffix)));
    assert(!cerberus_ble_inventory_ad_valid(duplicate_name,sizeof(duplicate_name)));
    assert(!cerberus_ble_inventory_ad_valid(short_mfg,sizeof(short_mfg)));
    scan_status.state=CERBERUS_BLE_SCAN_IDLE;dual_attached=dual_radio_active=false;dual_error=0;now_ms=5000;
    cerberus_dual_ble_start_t start={.boot_id=dual_boot_id,.job_id=dual_job_highwater+1U,.version=2};
    cerberus_dual_ble_status_t owner;
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_START,&start,sizeof(start),&owner,sizeof(owner));
    assert(!owner.error_code&&scan_inventory.enabled);
    cerberus_dual_ble_binding_t binding={dual_boot_id,start.job_id};
    struct ble_gap_disc_desc p={.addr={{1,2,3,4,5,6},0},.rssi=-30,.event_type=0,
        .data=flags,.length_data=sizeof(flags)};
    int64_t stamp=esp_timer_get_time();
    for(unsigned i=0;i<32;++i){p.addr.val[0]=(uint8_t)(i+1);record_advertisement(&p,scan_callback_epoch,++stamp);}
    assert(scan_status.stored_count==32&&!scan_inventory.replaced&&!scan_inventory.dropped);
    memcpy(scan_results[1].name,"OLD",3);scan_results[1].name_length=3;
    scan_results[1].uuid_length=16;memset(scan_results[1].uuid,0xa5,16);scan_results[1].manufacturer_id=0x1234;
    scan_results[1].reports_seen=99;
    cerberus_dual_ble_snapshot_status_t first=snapshot_begin(binding,1);
    assert(!first.error_code&&first.count==32);
    cerberus_dual_ble_snapshot_row_t original=snapshot_row(binding,first.handle,1);
    /* A valid recent original protects slot0; malformed repeated slot1 does
     * not become more recent for replacement merely through repeated traffic. */
    p.addr.val[0]=1;record_advertisement(&p,scan_callback_epoch,++stamp);
    p.addr.val[0]=2;p.data=bad_suffix;p.length_data=sizeof(bad_suffix);
    record_advertisement(&p,scan_callback_epoch,++stamp);
    assert(scan_inventory.rank[1]<scan_inventory.rank[0]);
    p.addr.val[0]=99;p.data=flags;p.length_data=sizeof(flags);p.event_type=3;p.rssi=-100;
    record_advertisement(&p,scan_callback_epoch,++stamp);
    assert(scan_inventory.replaced==1&&!scan_inventory.dropped&&scan_status.stored_count==32);
    assert(scan_results[0].address[0]==1&&scan_results[1].address[0]==99);
    assert(!scan_results[1].name_length&&!scan_results[1].uuid_length&&!scan_results[1].flags);
    assert(scan_results[1].manufacturer_id==CERBERUS_BLE_MANUFACTURER_UNKNOWN);
    assert(scan_results[1].reports_seen==1&&scan_results[1].rssi==-100&&scan_results[1].latest_rssi==-100);
    cerberus_dual_ble_snapshot_row_t retained=snapshot_row(binding,first.handle,1);
    assert(!memcmp(&retained,&original,sizeof(original)));
    cerberus_dual_ble_snapshot_inventory_t stats=snapshot_inventory(binding,first.handle);
    assert(!stats.error_code&&stats.handle==first.handle&&stats.count==32&&stats.flags==1&&!stats.replaced&&!stats.dropped);
    cerberus_ble_record_t before[32];memcpy(before,scan_results,sizeof(before));
    p.addr.val[0]=100;p.data=bad_width;p.length_data=sizeof(bad_width);
    record_advertisement(&p,scan_callback_epoch,++stamp);
    assert(scan_inventory.dropped==1);
    p.data=flags;p.length_data=sizeof(flags);
    record_advertisement(&p,scan_callback_epoch,-1);
    record_advertisement(&p,scan_callback_epoch,stamp-1);
    assert(scan_inventory.dropped==3);
    p.addr.type=4;record_advertisement(&p,scan_callback_epoch,++stamp);p.addr.type=0;
    p.event_type=4;record_advertisement(&p,scan_callback_epoch,++stamp);p.event_type=3;
    record_advertisement(&p,scan_callback_epoch-1,++stamp);
    assert(scan_inventory.dropped==3&&scan_inventory.replaced==1&&!memcmp(before,scan_results,sizeof(before)));
    cerberus_dual_ble_snapshot_status_t second=snapshot_begin(binding,2);
    stats=snapshot_inventory(binding,second.handle);
    assert(!stats.error_code&&stats.replaced==1&&stats.dropped==3&&stats.count==32);
    assert(snapshot_inventory(binding,first.handle).error_code==ESP_ERR_NOT_FOUND);
    cerberus_dual_ble_binding_t foreign=binding;foreign.boot_id++;
    assert(snapshot_inventory(foreign,second.handle).error_code==ESP_ERR_NOT_FOUND);
    foreign=binding;foreign.job_id++;
    assert(snapshot_inventory(foreign,second.handle).error_code==ESP_ERR_NOT_FOUND);
    uint64_t epoch=dual_epoch;dual_epoch++;
    assert(snapshot_inventory(binding,second.handle).error_code==ESP_ERR_NOT_FOUND);dual_epoch=epoch;
    snapshot_busy=true;stats=snapshot_inventory(binding,second.handle);snapshot_busy=false;
    assert(stats.error_code==ESP_ERR_TIMEOUT&&!stats.replaced&&!stats.dropped&&!stats.count&&!stats.flags);
    /* Entire complete extended container validates independently of its31-byte
     * preview. The prefix remains explicitly truncated, never reassembled. */
    uint8_t extended[255]={254,0xff,0x4c,0};
    assert(cerberus_ble_inventory_ad_valid(extended,sizeof(extended)));
    assert(!cerberus_ble_inventory_ad_valid(extended,sizeof(extended)+1));
    p.data=extended;p.length_data=255;record_advertisement(&p,scan_callback_epoch,++stamp);
    assert(scan_inventory.replaced==2&&scan_results[2].address[0]==100&&scan_results[2].advertisement_length==31);
    assert(scan_results[2].advertisement_flags&CERBERUS_BLE_ADVERTISEMENT_TRUNCATED);
    scan_inventory.sequence=UINT32_MAX;p.addr.val[0]=101;p.data=flags;p.length_data=sizeof(flags);
    record_advertisement(&p,scan_callback_epoch,++stamp);assert(scan_inventory.replaced==2&&scan_inventory.dropped==4);
    scan_inventory.sequence=1000;scan_inventory.replaced=UINT32_MAX;
    record_advertisement(&p,scan_callback_epoch,++stamp);assert(scan_inventory.replaced==UINT32_MAX);
    scan_inventory.dropped=UINT32_MAX;p.addr.val[0]=102;p.data=bad_width;p.length_data=sizeof(bad_width);
    record_advertisement(&p,scan_callback_epoch,++stamp);assert(scan_inventory.dropped==UINT32_MAX);
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_STOP,&binding,sizeof(binding),&owner,sizeof(owner));
    assert(snapshot_inventory(binding,second.handle).error_code==ESP_ERR_NOT_FOUND);
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_RELEASE,&binding,sizeof(binding),&owner,sizeof(owner));
    /* Legacy v1 discovery and v2 exact-address tracking keep stable first32
     * admission. A different typed address is different even with same bytes. */
    for(unsigned mode=0;mode<2;++mode){
        start.job_id=dual_job_highwater+1U;start.version=mode?2:1;start.mode=(uint8_t)mode;
        start.address[0]=mode?1:0;start.address_type=0;
        command(CERBERUS_SPI_COMMAND_DUAL_BLE_START,&start,sizeof(start),&owner,sizeof(owner));
        assert(!owner.error_code&&!scan_inventory.enabled&&!scan_inventory.replaced&&!scan_inventory.dropped);
        p.data=flags;p.length_data=sizeof(flags);p.event_type=0;p.addr.type=0;
        for(unsigned i=0;i<32;++i){p.addr.val[0]=(uint8_t)(i+1);record_advertisement(&p,scan_callback_epoch,++stamp);}
        p.addr.val[0]=1;p.addr.type=1;record_advertisement(&p,scan_callback_epoch,++stamp);
        assert(scan_status.stored_count==32&&!scan_inventory.replaced&&scan_inventory.dropped==1&&scan_results[0].address_type==0);
        binding.job_id=start.job_id;
        command(CERBERUS_SPI_COMMAND_DUAL_BLE_STOP,&binding,sizeof(binding),&owner,sizeof(owner));
        command(CERBERUS_SPI_COMMAND_DUAL_BLE_RELEASE,&binding,sizeof(binding),&owner,sizeof(owner));
    }
    printf("C5 dense inventory: strict complete admission, LRU ranks, immutable snapshots/stats, typed/drop/clock/saturation/legacy checks; state=%zu bytes\n",sizeof(scan_inventory));
}
static cerberus_dual_ble_analyzer_sample_t analyzer_sample(cerberus_dual_ble_binding_t binding)
{
    cerberus_dual_ble_analyzer_sample_t out;
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_ANALYZER_SAMPLE,&binding,sizeof(binding),&out,sizeof(out));
    return out;
}
static void analyzer_checks(void)
{
    dual_attached=dual_radio_active=false;scan_status.state=CERBERUS_BLE_SCAN_IDLE;
    dual_error=0;now_ms=300000;
    cerberus_dual_ble_start_t start={.boot_id=dual_boot_id,.job_id=dual_job_highwater+1U,
        .version=2,.mode=CERBERUS_DUAL_BLE_ANALYZER};
    cerberus_dual_ble_status_t owner;
    for(unsigned defect=0;defect<3;++defect){
        cerberus_dual_ble_start_t bad=start;
        if(defect==0)bad.version=1;if(defect==1)bad.address[0]=1;if(defect==2)bad.address_type=1;
        command(CERBERUS_SPI_COMMAND_DUAL_BLE_START,&bad,sizeof(bad),&owner,sizeof(owner));
        assert(owner.error_code==ESP_ERR_INVALID_ARG&&!dual_attached);
    }
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_START,&start,sizeof(start),&owner,sizeof(owner));
    assert(!owner.error_code&&owner.epoch&&dual_radio_active);
    uint64_t epoch=owner.epoch;unsigned original_starts=starts;
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_START,&start,sizeof(start),&owner,sizeof(owner));
    assert(starts==original_starts&&owner.epoch==epoch);
    cerberus_dual_ble_binding_t b={dual_boot_id,start.job_id};
    cerberus_dual_ble_analyzer_sample_t out=analyzer_sample(b);
    assert(out.epoch==epoch&&out.job_id==b.job_id&&out.flags==1&&!out.reports&&!out.elapsed_ms&&out.rssi==127);
    dual_analyzer.accepted=false;out=analyzer_sample(b);
    assert(out.epoch==epoch&&out.job_id==b.job_id&&!out.flags&&!out.reports&&!out.elapsed_ms&&out.rssi==127);
    dual_analyzer.accepted=true;
    cerberus_ble_record_t unchanged[32];memcpy(unchanged,scan_results,sizeof(unchanged));
    unsigned parsed=parse_calls;
    uint8_t malformed[]={255};
    struct ble_gap_event event={.type=BLE_GAP_EVENT_DISC,.disc={
        .addr={{1,2,3,4,5,6},1},.rssi=-40,.event_type=3,.data=malformed,.length_data=1}};
    for(unsigned i=0;i<40;++i){++now_ms;event.disc.addr.val[0]=(uint8_t)i;
        assert(!gap_event(&event,(void*)(uintptr_t)scan_callback_epoch));}
    event.type=BLE_GAP_EVENT_EXT_DISC;event.ext_disc=(struct ble_gap_ext_disc_desc){
        .addr=event.disc.addr,.rssi=-55,.data=malformed,.length_data=1};
    for(unsigned status=0;status<3;++status){event.ext_disc.data_status=(uint8_t)status;++now_ms;
        assert(!gap_event(&event,(void*)(uintptr_t)scan_callback_epoch));}
    out=analyzer_sample(b);
    assert(out.flags==17&&out.reports==43&&out.elapsed_ms==43&&out.rssi==-55);
    assert(parse_calls==parsed&&!scan_status.stored_count&&!memcmp(unchanged,scan_results,sizeof(unchanged)));
    for(unsigned defect=0;defect<7;++defect){
        struct ble_gap_event bad=event;void *token=(void*)(uintptr_t)scan_callback_epoch;
        if(defect==0)bad.ext_disc.props=BLE_HCI_ADV_SCAN_RSP_MASK;
        if(defect==1)bad.ext_disc.addr.type=4;
        if(defect==2)token=(void*)(uintptr_t)(scan_callback_epoch-1U);
        if(defect==3){bad.type=BLE_GAP_EVENT_DISC;bad.disc.event_type=4;}
        if(defect==4){bad.type=BLE_GAP_EVENT_DISC;bad.disc.event_type=5;}
        if(defect==5)bad.ext_disc.data=NULL;
        if(defect==6){bad.ext_disc.data_status=0;bad.ext_disc.data=NULL;}
        ++now_ms;assert(!gap_event(&bad,token));assert(analyzer_sample(b).reports==43);
    }
    assert(parse_calls==parsed&&!memcmp(unchanged,scan_results,sizeof(unchanged)));
    event.ext_disc.data_status=1;event.ext_disc.rssi=127;++now_ms;
    assert(!gap_event(&event,(void*)(uintptr_t)scan_callback_epoch));out=analyzer_sample(b);
    assert(out.flags==1&&out.reports==44&&out.rssi==127);
    unsigned locks=try_locks;snapshot_busy=true;out=analyzer_sample(b);snapshot_busy=false;
    assert(try_locks==locks+1&&out.flags==2&&!out.epoch&&!out.reports&&!out.elapsed_ms&&out.rssi==127&&out.job_id==b.job_id);
    for(unsigned defect=0;defect<3;++defect){cerberus_dual_ble_binding_t wrong=b;
        if(defect==0)++wrong.boot_id;if(defect==1)++wrong.job_id;
        if(defect==2)dual_mode=CERBERUS_DUAL_BLE_DISCOVERY;
        out=analyzer_sample(wrong);assert(!out.epoch&&!out.flags&&!out.reports&&!out.elapsed_ms&&out.rssi==127);
        dual_mode=CERBERUS_DUAL_BLE_ANALYZER;
    }
    for(size_t length=0;length<sizeof(b);++length){
        command(CERBERUS_SPI_COMMAND_DUAL_BLE_ANALYZER_SAMPLE,&b,length,&out,sizeof(out));
        assert(!out.epoch&&!out.flags&&!out.reports&&!out.elapsed_ms&&out.rssi==127&&!out.job_id);
    }
    uint8_t oversized[sizeof(b)+1U]={0};memcpy(oversized,&b,sizeof(b));
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_ANALYZER_SAMPLE,oversized,sizeof(oversized),&out,sizeof(out));
    assert(!out.epoch&&!out.flags&&!out.job_id&&!out.reports&&out.rssi==127);
    scanner_mutex=NULL;out=analyzer_sample(b);scanner_mutex=(void*)1;
    assert(!out.epoch&&!out.flags&&out.job_id==b.job_id);
    assert(snapshot_begin(b,1).error_code==ESP_ERR_INVALID_STATE); /* Analyzer never transfers an inventory. */
    dual_analyzer.reports=UINT32_MAX-1;event.ext_disc.rssi=-30;++now_ms;
    assert(!gap_event(&event,(void*)(uintptr_t)scan_callback_epoch));out=analyzer_sample(b);
    assert(out.reports==UINT32_MAX&&out.flags==21);
    ++now_ms;assert(!gap_event(&event,(void*)(uintptr_t)scan_callback_epoch));assert(analyzer_sample(b).reports==UINT32_MAX);
    uint32_t callback=scan_callback_epoch;now_ms=308000;finish_scan_for_epoch(0,callback);out=analyzer_sample(b);
    assert(out.flags==29&&out.elapsed_ms==8000&&out.reports==UINT32_MAX);
    now_ms=309000;out=analyzer_sample(b);assert(out.flags==29&&out.elapsed_ms==8000);
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_RELEASE,&b,sizeof(b),&owner,sizeof(owner));
    out=analyzer_sample(b);assert(!out.epoch&&!out.flags);
    start.job_id=dual_job_highwater+1U;now_ms=400000;
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_START,&start,sizeof(start),&owner,sizeof(owner));b.job_id=start.job_id;
    now_ms=400001;assert(!gap_event(&event,(void*)(uintptr_t)callback));assert(!analyzer_sample(b).reports);
    assert(!gap_event(&event,(void*)(uintptr_t)scan_callback_epoch));assert(analyzer_sample(b).reports==1);
    now_ms=400000;out=analyzer_sample(b);assert(out.flags==32&&out.epoch==owner.epoch&&!out.reports&&!out.elapsed_ms&&out.rssi==127);
    now_ms=400010;assert(analyzer_sample(b).flags==32);
    cancel_fails=true;command(CERBERUS_SPI_COMMAND_DUAL_BLE_STOP,&b,sizeof(b),&owner,sizeof(owner));
    assert(dual_attached&&(owner.flags&CERBERUS_DUAL_BLE_OWNED));out=analyzer_sample(b);assert(!out.flags&&!out.reports);
    cancel_fails=false;command(CERBERUS_SPI_COMMAND_DUAL_BLE_STOP,&b,sizeof(b),&owner,sizeof(owner));
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_RELEASE,&b,sizeof(b),&owner,sizeof(owner));
    start.job_id=dual_job_highwater+1U;other_radio_busy=true;
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_START,&start,sizeof(start),&owner,sizeof(owner));other_radio_busy=false;
    assert(owner.error_code&&!dual_attached);
    start.job_id=dual_job_highwater+1U;command(CERBERUS_SPI_COMMAND_DUAL_BLE_START,&start,sizeof(start),&owner,sizeof(owner));
    b.job_id=start.job_id;finish_scan_for_epoch(0,scan_callback_epoch);now_ms+=CERBERUS_DUAL_BLE_ORPHAN_MS;
    out=analyzer_sample(b);assert(!out.epoch&&!out.flags&&!dual_attached);
    uint32_t saved_callback=scan_callback_epoch;
    for(unsigned exhausted=0;exhausted<2;++exhausted){
        start.job_id=dual_job_highwater+1U;
        scan_callback_epoch=UINT32_MAX;
        command(CERBERUS_SPI_COMMAND_DUAL_BLE_START,&start,sizeof(start),&owner,sizeof(owner));
        assert(owner.error_code==ESP_ERR_INVALID_STATE&&!dual_attached&&!dual_radio_active);
    }
    scan_callback_epoch=saved_callback;
    puts("C5 Analyzer actual service/GAP: typed original+partial counts, no parsing/inventory, full binding, BUSY, saturation, frozen finish, clock fault, Stop and orphan passed");
}

static cerberus_dual_ble_snapshot_family_row_t snapshot_family_row(
    cerberus_dual_ble_binding_t b,uint64_t handle,uint8_t index)
{
    cerberus_dual_ble_snapshot_family_row_t row={0};
    for(uint8_t chunk=0;chunk<16;++chunk){
        cerberus_dual_ble_snapshot_read_t request={b,handle,index,chunk};
        cerberus_dual_ble_snapshot_chunk_t reply;
        command(CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_FAMILY,&request,sizeof(request),&reply,sizeof(reply));
        assert(!reply.error&&reply.handle==handle&&reply.index==index&&reply.chunk==chunk);
        size_t n=chunk==15?8U:10U;assert(reply.byte_count==n);
        memcpy((uint8_t*)&row+chunk*10U,reply.data,n);
        for(size_t j=n;j<10;++j)assert(!reply.data[j]);
    }
    cerberus_dual_ble_snapshot_metadata_row_t old=snapshot_metadata_row(b,handle,index);
    assert(!memcmp(&old,&row.metadata,sizeof(old)));
    cerberus_dual_ble_family_evidence_t e;
    assert(cerberus_dual_ble_family_wire_decode(&row.family,&row.metadata.receive,
        row.metadata.row.raw_length,row.metadata.row.event_type,row.metadata.row.raw_flags,
        row.metadata.row.address,row.metadata.row.address_type,&e));
    return row;
}

static void family_checks(void)
{
    now_ms=500000;dual_error=0;
    const uint8_t samples[6][31]={
        {26,0xff,0x4c,0,2,0x15,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,0x12,0x34,0xab,0xcd,0xc5},
        {3,3,0x44,0xfd}, {3,3,0x81,0x30}, {15,9,'F','S',' ','E','x','t',' ','B','a','t','t','e','r','y'},
        {3,3,0x5f,0xfd}, {6,9,'H','C','-','0','5'}};
    const uint8_t lengths[]={27,4,4,16,4,7};
    cerberus_dual_ble_start_t start={.boot_id=dual_boot_id,.version=2,.mode=CERBERUS_DUAL_BLE_FAMILY};
    cerberus_dual_ble_status_t owner;
    for(unsigned invalid=0;invalid<6;++invalid){
        cerberus_dual_ble_start_t bad=start;bad.job_id=dual_job_highwater+1;bad.reserved=1;
        if(invalid==0)bad.reserved=0;if(invalid==1)bad.reserved=7;
        if(invalid==2)bad.version=1;if(invalid==3)bad.address[0]=1;
        if(invalid==4)bad.address_type=1;if(invalid==5)bad.mode=CERBERUS_DUAL_BLE_DISCOVERY;
        unsigned count=starts;command(CERBERUS_SPI_COMMAND_DUAL_BLE_START,&bad,sizeof(bad),&owner,sizeof(owner));
        assert(owner.error_code==ESP_ERR_INVALID_ARG&&!dual_attached&&starts==count);
    }
    for(uint8_t code=1;code<=6;++code){
        start.job_id=dual_job_highwater+1;start.reserved=code;
        command(CERBERUS_SPI_COMMAND_DUAL_BLE_START,&start,sizeof(start),&owner,sizeof(owner));
        assert(!owner.error_code&&dual_mode==CERBERUS_DUAL_BLE_FAMILY&&dual_family==code&&scan_inventory.enabled);
        for(size_t i=0;i<sizeof(scan_family);++i)assert(!((uint8_t*)scan_family)[i]);
        unsigned nstarts=starts,nstops=stops;uint64_t epoch=owner.epoch;
        command(CERBERUS_SPI_COMMAND_DUAL_BLE_START,&start,sizeof(start),&owner,sizeof(owner));
        assert(!owner.error_code&&owner.epoch==epoch&&starts==nstarts);
        cerberus_dual_ble_start_t changed=start;changed.reserved=code==6?1:code+1;
        command(CERBERUS_SPI_COMMAND_DUAL_BLE_START,&changed,sizeof(changed),&owner,sizeof(owner));
        assert(owner.error_code==ESP_ERR_INVALID_STATE&&starts==nstarts&&dual_family==code);
        uint8_t payload[255]={0};payload[0]=39;payload[1]=0x30;
        memcpy(payload+40,samples[code-1],lengths[code-1]);
        struct ble_gap_event event={.type=BLE_GAP_EVENT_EXT_DISC,.ext_disc={
            .addr={{20,2,3,4,5,6},1},.rssi=-43,.sid=255,.prim_phy=3,.sec_phy=2,
            .length_data=255,.data=payload}};
        /* More than32 unrelated typed originals must never crowd out a rare
         * matching source, nor count as full-bank drops/replacements. */
        uint8_t unrelated[]={2,1,6};event.ext_disc.data=unrelated;event.ext_disc.length_data=3;
        for(unsigned i=0;i<80;++i){event.ext_disc.addr.val[0]=(uint8_t)i;++now_ms;
            assert(!gap_event(&event,(void*)(uintptr_t)scan_callback_epoch));}
        assert(!scan_status.stored_count&&!scan_inventory.replaced&&!scan_inventory.dropped);
        event.ext_disc.addr.val[0]=20;event.ext_disc.data=payload;event.ext_disc.length_data=255;
        parse_fails=true;++now_ms;assert(!gap_event(&event,(void*)(uintptr_t)scan_callback_epoch));parse_fails=false;
        assert(scan_status.stored_count==1&&scan_family[0].categories==(1U<<(code-1)));
        assert(scan_results[0].advertisement_flags==3&&scan_results[0].receive.ad_validation==1);
        cerberus_dual_ble_binding_t b={dual_boot_id,start.job_id};uint64_t id=1;
        cerberus_dual_ble_snapshot_status_t snap=snapshot_begin(b,id);assert(!snap.error_code&&snap.count==1);
        cerberus_dual_ble_snapshot_family_row_t frozen=snapshot_family_row(b,snap.handle,0);
        assert(frozen.family.family_code==code&&frozen.metadata.receive.callback_length==255);
        cerberus_ble_record_t old=scan_results[0];cerberus_dual_ble_family_wire_t oldproof=scan_family[0];
        uint32_t rank=scan_inventory.rank[0];
        for(unsigned excluded=0;excluded<6;++excluded){struct ble_gap_event bad=event;
            if(excluded==0)bad.ext_disc.props=8;
            if(excluded==1)bad.ext_disc.data_status=1;
            if(excluded==2)bad.ext_disc.data_status=2;
            if(excluded==3)bad.ext_disc.addr.type=4;
            uint32_t callback=excluded==4?scan_callback_epoch-1:scan_callback_epoch;
            uint64_t prior_now=now_ms;if(excluded==5)now_ms=old.original_observed_ms-1U;else ++now_ms;
            assert(!gap_event(&bad,(void*)(uintptr_t)callback));if(excluded==5)now_ms=prior_now;
            assert(!memcmp(&old,&scan_results[0],sizeof(old))&&!memcmp(&oldproof,&scan_family[0],sizeof(oldproof)));
            assert(scan_inventory.rank[0]==rank&&!scan_inventory.replaced&&!scan_inventory.dropped);
        }
        /* Same typed original, new complete nonmatch clears current proof and
         * becomes preferred rank0 while FIRST BEGIN remains immutable. */
        event.ext_disc.data=unrelated;event.ext_disc.length_data=3;++now_ms;
        assert(!gap_event(&event,(void*)(uintptr_t)scan_callback_epoch));
        assert(scan_family[0].flags&1);assert(!scan_family[0].categories&&!scan_inventory.rank[0]);
        cerberus_dual_ble_snapshot_family_row_t same=snapshot_family_row(b,snap.handle,0);assert(!memcmp(&same,&frozen,sizeof(same)));
        cerberus_dual_ble_snapshot_status_t retry=snapshot_begin(b,id);assert(retry.handle==snap.handle);
        same=snapshot_family_row(b,retry.handle,0);assert(!memcmp(&same,&frozen,sizeof(same)));
        snap=snapshot_begin(b,++id);same=snapshot_family_row(b,snap.handle,0);assert(!same.family.categories);
        event.ext_disc.data=payload;event.ext_disc.length_data=255;payload[254]=1;++now_ms;
        assert(!gap_event(&event,(void*)(uintptr_t)scan_callback_epoch));
        assert(scan_family[0].state==CERBERUS_DUAL_BLE_FAMILY_MALFORMED&&!scan_family[0].categories&&!(scan_family[0].flags&1));
        snap=snapshot_begin(b,++id);same=snapshot_family_row(b,snap.handle,0);assert(same.metadata.receive.ad_validation==2);
        payload[254]=0;event.ext_disc.sid=16;++now_ms;assert(!gap_event(&event,(void*)(uintptr_t)scan_callback_epoch));
        snap=snapshot_begin(b,++id);same=snapshot_family_row(b,snap.handle,0);
        assert(same.family.state==CERBERUS_DUAL_BLE_FAMILY_RECEIVE_UNKNOWN&&!same.family.categories&&!same.metadata.receive.source_kind);
        event.ext_disc.sid=255;
        /* Fill all32 with qualifying originals, make most-recent slot lose
         * qualification, then ensure its rank0 wins replacement over oldest. */
        for(unsigned i=0;i<32;++i){event.ext_disc.addr.val[0]=(uint8_t)(20+i);++now_ms;
            assert(!gap_event(&event,(void*)(uintptr_t)scan_callback_epoch));}
        assert(scan_status.stored_count==32);event.ext_disc.addr.val[0]=51;event.ext_disc.data=unrelated;event.ext_disc.length_data=3;
        ++now_ms;assert(!gap_event(&event,(void*)(uintptr_t)scan_callback_epoch));assert(!scan_inventory.rank[31]);
        event.ext_disc.addr.val[0]=99;event.ext_disc.data=payload;event.ext_disc.length_data=255;
        ++now_ms;assert(!gap_event(&event,(void*)(uintptr_t)scan_callback_epoch));
        assert(scan_results[31].address[0]==99&&scan_inventory.replaced==1&&scan_results[0].address[0]==20);
        /* Saturated admission sequence never churns full-bank identities;
         * unrelated filtered callbacks are not counted as admission drops. */
        scan_inventory.sequence=UINT32_MAX;event.ext_disc.addr.val[0]=100;++now_ms;
        assert(!gap_event(&event,(void*)(uintptr_t)scan_callback_epoch));assert(scan_inventory.dropped==1&&scan_inventory.replaced==1);
        event.ext_disc.data=unrelated;event.ext_disc.length_data=3;++now_ms;
        assert(!gap_event(&event,(void*)(uintptr_t)scan_callback_epoch));assert(scan_inventory.dropped==1);
        snap=snapshot_begin(b,++id);cerberus_dual_ble_snapshot_inventory_t stats=snapshot_inventory(b,snap.handle);
        assert(stats.replaced==1&&stats.dropped==1&&stats.count==32&&stats.flags==1);
        /* Full handle/binding/index/chunk fences apply to every new chunk. */
        for(unsigned bad=0;bad<7;++bad){cerberus_dual_ble_snapshot_read_t req={b,snap.handle,0,0};
            if(bad==0)++req.binding.boot_id;if(bad==1)++req.binding.job_id;if(bad==2)++req.handle;
            if(bad==3)req.index=32;if(bad==4)req.chunk=16;if(bad==5)snapshot_busy=true;
            if(bad==6)dual_mode=CERBERUS_DUAL_BLE_DISCOVERY;
            cerberus_dual_ble_snapshot_chunk_t reply;
            command(CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_FAMILY,&req,sizeof(req),&reply,sizeof(reply));
            snapshot_busy=false;dual_mode=CERBERUS_DUAL_BLE_FAMILY;
            assert(reply.error&&!reply.byte_count&&reply.handle==req.handle&&reply.index==req.index&&reply.chunk==req.chunk);
            for(unsigned j=0;j<10;++j)assert(!reply.data[j]);
        }
        assert(starts==nstarts&&stops==nstops); /* Reads never schedule RF. */
        finish_scan_for_epoch(0,scan_callback_epoch);snap=snapshot_begin(b,++id);assert(snap.flags&2);
        command(CERBERUS_SPI_COMMAND_DUAL_BLE_RELEASE,&b,sizeof(b),&owner,sizeof(owner));
        assert(snapshot_status(b,snap.handle).error_code==ESP_ERR_NOT_FOUND&&!dual_attached);
    }
    uint32_t old_callback=scan_callback_epoch;
    for(unsigned exhausted=0;exhausted<2;++exhausted){
        start.job_id=dual_job_highwater+1;start.reserved=6;
        scan_callback_epoch=UINT32_MAX;
        unsigned before=starts;
        command(CERBERUS_SPI_COMMAND_DUAL_BLE_START,&start,sizeof(start),&owner,sizeof(owner));
        assert(owner.error_code==ESP_ERR_INVALID_STATE&&!dual_attached&&!dual_radio_active&&starts==before);
    }
    scan_callback_epoch=old_callback;
    printf("C5 FAMILY actual callback/service: full255 all6, focused capacity, dequalification/rank0, immutable102/110/158, epoch/clock/exclusions/lifecycle passed; live=%zu frozen=%zu bytes\n",sizeof(scan_family),sizeof(dual_snapshot.family));
}

static cerberus_dual_ble_snapshot_response_row_t snapshot_response_row(
    cerberus_dual_ble_binding_t b,uint64_t handle,uint8_t index)
{
    uint8_t bytes[310];
    for(uint8_t chunk=0;chunk<31;++chunk){
        cerberus_dual_ble_snapshot_read_t req={b,handle,index,chunk};cerberus_dual_ble_snapshot_chunk_t out;
        command(CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_RESPONSE,&req,sizeof(req),&out,sizeof(out));
        assert(!out.error&&out.handle==handle&&out.index==index&&out.chunk==chunk&&out.byte_count==10);
        memcpy(bytes+10U*chunk,out.data,10);
    }
    cerberus_dual_ble_snapshot_response_row_t out;
    memcpy(&out.family,bytes,158);cerberus_dual_ble_response_decode_bytes(bytes+158,&out.response);
    assert(cerberus_dual_ble_response_valid(&out.response,&out.family,dual_family));
    cerberus_dual_ble_snapshot_row_t old=snapshot_row(b,handle,index);
    assert(!memcmp(&old,&out.family.metadata.row,sizeof(old)));
    return out;
}

static void response_checks(void)
{
    const uint8_t samples[6][31]={
        {26,0xff,0x4c,0,2,0x15,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,0x12,0x34,0xab,0xcd,0xc5},
        {3,3,0x44,0xfd},{3,3,0x81,0x30},{15,9,'F','S',' ','E','x','t',' ','B','a','t','t','e','r','y'},
        {3,3,0x5f,0xfd},{6,9,'H','C','-','0','5'}};
    const uint8_t lengths[]={27,4,4,16,4,7};
    cerberus_dual_ble_start_t start={.boot_id=dual_boot_id,.version=2,.mode=CERBERUS_DUAL_BLE_RESPONSE_DISCOVERY};
    cerberus_dual_ble_status_t status;
    assert(!dual_attached&&!dual_response);now_ms=700000;dual_error=0;
    unsigned prior_starts=starts,prior_frees=response_frees;
    allocation_fails=true;start.job_id=dual_job_highwater+1;
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_START,&start,sizeof(start),&status,sizeof(status));
    assert(status.error_code==ESP_ERR_NO_MEM&&!dual_attached&&!dual_response&&starts==prior_starts);
    allocation_fails=false;other_radio_busy=true;
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_START,&start,sizeof(start),&status,sizeof(status));
    assert(status.error_code==ESP_ERR_INVALID_STATE&&!dual_response&&starts==prior_starts&&response_frees==prior_frees+1);
    other_radio_busy=false;
    for(uint8_t code=0;code<=6;++code){
        start.job_id=dual_job_highwater+1;start.mode=code?CERBERUS_DUAL_BLE_RESPONSE_FAMILY:CERBERUS_DUAL_BLE_RESPONSE_DISCOVERY;start.reserved=code;
        command(CERBERUS_SPI_COMMAND_DUAL_BLE_START,&start,sizeof(start),&status,sizeof(status));
        assert(!status.error_code&&dual_response);c5_response_context_t *allocation=dual_response;
        unsigned allocated=response_allocations;
        command(CERBERUS_SPI_COMMAND_DUAL_BLE_START,&start,sizeof(start),&status,sizeof(status));
        assert(!status.error_code&&allocation==dual_response&&response_allocations==allocated);
        uint8_t original[]={2,1,6};uint8_t response[255]={39,0x30};
        memcpy(response+40,samples[code?code-1:5],lengths[code?code-1:5]);
        struct ble_gap_event e={.type=BLE_GAP_EVENT_EXT_DISC,.ext_disc={.props=2,.addr={{90,2,3,4,5,6},1},
            .rssi=-70,.sid=7,.prim_phy=1,.sec_phy=1,.length_data=3,.data=original}};
        if(code){
            for(unsigned i=0;i<80;++i){e.ext_disc.addr.val[0]=(uint8_t)i;++now_ms;gap_event(&e,(void*)(uintptr_t)scan_callback_epoch);}
            assert(!scan_status.stored_count&&!scan_inventory.replaced&&!scan_inventory.dropped);
            unsigned pending=0;for(unsigned i=0;i<8;++i)pending+=dual_response->candidates[i].sequence!=0;
            assert(pending==8);e.ext_disc.props=10;e.ext_disc.addr.val[0]=0;e.ext_disc.data=response;e.ext_disc.length_data=255;
            ++now_ms;gap_event(&e,(void*)(uintptr_t)scan_callback_epoch);assert(!scan_status.stored_count); /* evicted candidate */
            e.ext_disc.props=2;e.ext_disc.data=original;e.ext_disc.length_data=3;
        }
        e.ext_disc.addr.val[0]=90;++now_ms;gap_event(&e,(void*)(uintptr_t)scan_callback_epoch);
        assert(scan_status.stored_count==(code?0:1));
        e.ext_disc.props=10;e.ext_disc.data=response;e.ext_disc.length_data=255;e.ext_disc.rssi=-25;
        ++now_ms;gap_event(&e,(void*)(uintptr_t)scan_callback_epoch);
        assert(scan_status.stored_count==1&&scan_results[0].latest_rssi==-70&&scan_results[0].reports_seen==1);
        assert(dual_response->live[0].wire.state==CERBERUS_DUAL_BLE_RESPONSE_PRESENT);
        assert(dual_response->live[0].wire.family.categories==(1U<<(code?code-1:5)));
        assert(dual_response->live[0].wire.family.family_code==code&&response_allocations==allocated);
        cerberus_dual_ble_binding_t b={dual_boot_id,start.job_id};uint64_t id=1;
        ++now_ms;cerberus_dual_ble_snapshot_status_t snap=snapshot_begin(b,id);assert(!snap.error_code);
        cerberus_dual_ble_snapshot_response_row_t frozen=snapshot_response_row(b,snap.handle,0);
        assert(frozen.response.raw_length==31&&frozen.response.receive.callback_length==255&&frozen.response.raw_flags==1);
        assert(frozen.response.age_ms==1&&frozen.response.interval_us==1000&&frozen.family.metadata.row.age_ms==2);
        assert(frozen.response.family.categories==(1U<<(code?code-1:5)));
        if(code==6||!code)assert(frozen.response.name_length==5&&!memcmp(frozen.response.name,"HC-05",5));
        /* A partial newer original revokes pairing, preserves historical ADV,
         * and cannot alter an already frozen FIRST BEGIN or accepted retry. */
        e.ext_disc.props=2;e.ext_disc.data_status=1;e.ext_disc.data=original;e.ext_disc.length_data=3;
        ++now_ms;gap_event(&e,(void*)(uintptr_t)scan_callback_epoch);
        assert(!dual_response->live[0].wire.version&&scan_results[0].reports_seen==1);
        e.ext_disc.props=10;e.ext_disc.data_status=0;e.ext_disc.data=response;e.ext_disc.length_data=255;
        ++now_ms;gap_event(&e,(void*)(uintptr_t)scan_callback_epoch);assert(!dual_response->live[0].wire.version);
        cerberus_dual_ble_snapshot_response_row_t retry=snapshot_response_row(b,snap.handle,0);assert(!memcmp(&retry,&frozen,sizeof(retry)));
        assert(snapshot_begin(b,id).handle==snap.handle);
        snap=snapshot_begin(b,++id);retry=snapshot_response_row(b,snap.handle,0);assert(!retry.response.version);
        /* Rebind a fresh complete original; wrong SID/type, incomplete/invalid
         * responses and stale epoch cannot change NONE or original counts. */
        e.ext_disc.props=2;e.ext_disc.data=original;e.ext_disc.length_data=3;++now_ms;gap_event(&e,(void*)(uintptr_t)scan_callback_epoch);
        assert(dual_response->live[0].wire.state==1&&dual_response->live[0].wire.original_sequence);
        for(unsigned fault=0;fault<7;++fault){struct ble_gap_event bad=e;bad.ext_disc.props=10;bad.ext_disc.data=response;bad.ext_disc.length_data=255;
            uint32_t epoch=scan_callback_epoch;
            if(fault==0)bad.ext_disc.sid=8;if(fault==1)bad.ext_disc.addr.type=0;if(fault==2)bad.ext_disc.data_status=1;
            if(fault==3)bad.ext_disc.prim_phy=3;if(fault==4)--epoch;if(fault==5)bad.ext_disc.sid=255;if(fault==6)response[254]=1;
            ++now_ms;gap_event(&bad,(void*)(uintptr_t)epoch);response[254]=0;
            assert(dual_response->live[0].wire.state==1&&scan_results[0].reports_seen==2);
        }
        e.ext_disc.props=10;e.ext_disc.data=response;e.ext_disc.length_data=255;now_ms+=2001;
        gap_event(&e,(void*)(uintptr_t)scan_callback_epoch);assert(dual_response->live[0].wire.state==1);
        dual_response->sequence=UINT64_MAX;e.ext_disc.props=2;e.ext_disc.data=original;e.ext_disc.length_data=3;
        ++now_ms;gap_event(&e,(void*)(uintptr_t)scan_callback_epoch);assert(!dual_response->live[0].wire.version&&scan_results[0].reports_seen==2);
        cancel_fails=true;command(CERBERUS_SPI_COMMAND_DUAL_BLE_STOP,&b,sizeof(b),&status,sizeof(status));
        assert(status.error_code&&dual_response==allocation&&dual_attached);
        command(CERBERUS_SPI_COMMAND_DUAL_BLE_RELEASE,&b,sizeof(b),&status,sizeof(status));assert(status.error_code&&dual_response);
        cancel_fails=false;command(CERBERUS_SPI_COMMAND_DUAL_BLE_STOP,&b,sizeof(b),&status,sizeof(status));assert(dual_response&&!dual_radio_active);
        command(CERBERUS_SPI_COMMAND_DUAL_BLE_RELEASE,&b,sizeof(b),&status,sizeof(status));assert(!dual_response&&!dual_attached);
        command(CERBERUS_SPI_COMMAND_DUAL_BLE_RELEASE,&b,sizeof(b),&status,sizeof(status));assert(!dual_response);
    }
    start.mode=CERBERUS_DUAL_BLE_RESPONSE_DISCOVERY;start.reserved=0;start.job_id=dual_job_highwater+1;
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_START,&start,sizeof(start),&status,sizeof(status));assert(dual_response);
    finish_scan_for_epoch(0,scan_callback_epoch);now_ms+=CERBERUS_DUAL_BLE_ORPHAN_MS;
    cerberus_dual_ble_info_t info;command(CERBERUS_SPI_COMMAND_DUAL_BLE_INFO,NULL,0,&info,sizeof(info));
    assert(!dual_response&&!dual_attached&&response_allocations==response_frees);
    printf("C5 response actual GAP/service: all6 response-only full255 promotion, bounded8 pending, immutable310/old102, invalidation, source/clock/exhaustion, allocationfailure/Stop/orphan cleanup passed; optionalcontext=%zu live=%zu candidate=%zu bytes\n",sizeof(c5_response_context_t),sizeof(c5_response_live_t),sizeof(c5_response_candidate_t));
}

static void response_dense_clock_checks(void)
{
    now_ms=900000;dual_error=0;
    cerberus_dual_ble_start_t start={.boot_id=dual_boot_id,.job_id=dual_job_highwater+1,.version=2,
        .mode=CERBERUS_DUAL_BLE_RESPONSE_FAMILY,.reserved=6};
    cerberus_dual_ble_status_t status;command(CERBERUS_SPI_COMMAND_DUAL_BLE_START,&start,sizeof(start),&status,sizeof(status));
    assert(!status.error_code&&dual_response);
    uint8_t ordinary[]={2,1,6},hc[]={6,9,'H','C','-','0','5'};
    struct ble_gap_event e={.type=BLE_GAP_EVENT_EXT_DISC,.ext_disc={.addr={{1,2,3,4,5,6},1},.rssi=-75,.sid=7,.prim_phy=1,.sec_phy=1}};
    for(unsigned i=0;i<32;++i){
        e.ext_disc.addr.val[0]=(uint8_t)(i+1);e.ext_disc.props=2;e.ext_disc.data=ordinary;e.ext_disc.length_data=3;
        ++now_ms;gap_event(&e,(void*)(uintptr_t)scan_callback_epoch);
        e.ext_disc.props=10;e.ext_disc.data=hc;e.ext_disc.length_data=7;
        ++now_ms;gap_event(&e,(void*)(uintptr_t)scan_callback_epoch);
        assert(scan_status.stored_count==i+1&&scan_results[i].reports_seen==1&&scan_inventory.rank[i]);
    }
    e.ext_disc.props=2;e.ext_disc.data=ordinary;e.ext_disc.length_data=3;
    ++now_ms;gap_event(&e,(void*)(uintptr_t)scan_callback_epoch);assert(!scan_inventory.rank[31]);
    e.ext_disc.props=10;e.ext_disc.data=hc;e.ext_disc.length_data=7;
    ++now_ms;gap_event(&e,(void*)(uintptr_t)scan_callback_epoch);assert(scan_inventory.rank[31]&&scan_results[31].reports_seen==2);
    e.ext_disc.data=ordinary;e.ext_disc.length_data=3;
    ++now_ms;gap_event(&e,(void*)(uintptr_t)scan_callback_epoch);assert(!scan_inventory.rank[31]&&scan_results[31].reports_seen==2);
    e.ext_disc.addr.val[0]=99;e.ext_disc.props=2;
    ++now_ms;gap_event(&e,(void*)(uintptr_t)scan_callback_epoch);
    e.ext_disc.props=10;e.ext_disc.data=hc;e.ext_disc.length_data=7;
    ++now_ms;gap_event(&e,(void*)(uintptr_t)scan_callback_epoch);
    assert(scan_results[31].address[0]==99&&scan_results[31].reports_seen==1&&scan_results[0].address[0]==1&&scan_inventory.replaced==1);
    cerberus_dual_ble_binding_t b={dual_boot_id,start.job_id};
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_STOP,&b,sizeof(b),&status,sizeof(status));
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_RELEASE,&b,sizeof(b),&status,sizeof(status));assert(!dual_response);

    now_ms=(UINT64_C(1)<<32)+5U;uint64_t observed=now_ms;
    start.mode=CERBERUS_DUAL_BLE_RESPONSE_DISCOVERY;start.reserved=0;start.job_id=dual_job_highwater+1;b.job_id=start.job_id;
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_START,&start,sizeof(start),&status,sizeof(status));assert(!status.error_code);
    uint8_t adv[]={3,3,0x34,0x12,3,0xff,0xcd,0xab,4,9,'A','D','V'};
    struct ble_gap_event legacy={.type=BLE_GAP_EVENT_DISC,.disc={.addr={{90,2,3,4,5,6},1},.event_type=0,.rssi=-80,.data=adv,.length_data=sizeof(adv)}};
    gap_event(&legacy,(void*)(uintptr_t)scan_callback_epoch);
    assert(scan_results[0].name_length==3&&!memcmp(scan_results[0].name,"ADV",3)&&scan_results[0].manufacturer_id==0xabcd&&scan_results[0].uuid_length==2);
    assert(scan_results[0].uuid[0]==0x34&&scan_results[0].uuid[1]==0x12);
    legacy.disc.event_type=4;legacy.disc.rssi=-20;legacy.disc.data=hc;legacy.disc.length_data=sizeof(hc);
    gap_event(&legacy,(void*)(uintptr_t)scan_callback_epoch);
    assert(scan_results[0].reports_seen==1&&scan_results[0].latest_rssi==-80&&scan_results[0].name_length==3);
    assert(dual_response->live[0].wire.response_sequence>dual_response->live[0].wire.original_sequence&&dual_response->live[0].wire.interval_us==0);
    uint64_t id=1;cerberus_dual_ble_snapshot_status_t snap=snapshot_begin(b,id);
    cerberus_dual_ble_snapshot_response_row_t frozen=snapshot_response_row(b,snap.handle,0);
    assert(!frozen.response.age_ms&&!frozen.family.metadata.row.age_ms);
    now_ms+=3;snap=snapshot_begin(b,++id);frozen=snapshot_response_row(b,snap.handle,0);
    assert(frozen.response.age_ms==3&&frozen.family.metadata.row.age_ms==3);
    legacy.disc.event_type=0;legacy.disc.data=adv;legacy.disc.length_data=sizeof(adv);now_ms=observed-1;
    gap_event(&legacy,(void*)(uintptr_t)scan_callback_epoch);assert(!dual_response->live[0].wire.version&&scan_results[0].reports_seen==1);
    snap=snapshot_begin(b,++id);frozen=snapshot_response_row(b,snap.handle,0);
    assert(frozen.family.metadata.row.age_ms==UINT32_MAX&&!(frozen.family.metadata.row.timing_flags&1));
    now_ms=observed+4;snap=snapshot_begin(b,++id);frozen=snapshot_response_row(b,snap.handle,0);
    assert(!frozen.response.version&&frozen.family.metadata.row.age_ms==4);
    /* Confirmed RF completion error frees optional memory but keeps error
     * lease until RELEASE; a repeated cleanup cannot free it twice. */
    finish_scan_for_epoch(9,scan_callback_epoch);assert(!dual_response);
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_RELEASE,&b,sizeof(b),&status,sizeof(status));assert(!dual_attached);
    assert(response_allocations==response_frees);
    puts("C5 response dense/source regression:32 promotions, no-hint rank0/renewal, replacement, exact original SDK projection, same-ms sequences, full64 clock and backward freeze, quiescent error free passed");
}

static void dispatch_once(struct ble_gap_event *event,uint32_t epoch,unsigned expected_locks)
{
    unsigned checks_before=response_mode_checks,locks_before=mutex_takes;
    c5_response_context_t *allocation=dual_response;
    assert(!gap_event(event,(void*)(uintptr_t)epoch));
    assert(response_mode_checks==checks_before+1U);
    assert(mutex_takes==locks_before+expected_locks && !held);
    assert(dual_attached && dual_radio_active && dual_response==allocation);
}

static void response_dispatch_once_checks(void)
{
    uint8_t original[]={6,9,'H','C','-','0','5'};
    uint8_t response[]={6,9,'H','C','-','0','6'};
    uint8_t malformed[]={5,9,'X'};
    now_ms=800000;
    for(unsigned extended=0;extended<2;++extended)for(unsigned mode_case=0;mode_case<3;++mode_case){
        const bool active=mode_case!=0;
        cerberus_dual_ble_start_t start={.boot_id=dual_boot_id,.job_id=dual_job_highwater+1U,
            .version=2,.mode=mode_case==2?CERBERUS_DUAL_BLE_RESPONSE_FAMILY:
                active?CERBERUS_DUAL_BLE_RESPONSE_DISCOVERY:CERBERUS_DUAL_BLE_DISCOVERY,
            .reserved=mode_case==2?6:0};
        cerberus_dual_ble_status_t status;
        command(CERBERUS_SPI_COMMAND_DUAL_BLE_START,&start,sizeof(start),&status,sizeof(status));
        assert(!status.error_code && (!!dual_response)==active);
        cerberus_dual_ble_binding_t binding={dual_boot_id,start.job_id};
        struct ble_gap_event event={.type=extended?BLE_GAP_EVENT_EXT_DISC:BLE_GAP_EVENT_DISC,
            .disc={.addr={{99,2,3,4,5,6},1},.rssi=-70,.event_type=0,.data=original,.length_data=sizeof(original)},
            .ext_disc={.addr={{99,2,3,4,5,6},1},.rssi=-70,.props=2,.sid=7,.prim_phy=1,.sec_phy=1,
                .data=original,.length_data=sizeof(original)}};
        ++now_ms;dispatch_once(&event,scan_callback_epoch,active?2U:3U);
        assert(scan_status.stored_count==1 && scan_results[0].reports_seen==1);
        if(active)assert(dual_response->live[0].wire.state==CERBERUS_DUAL_BLE_RESPONSE_NONE);
        struct ble_gap_event reply=event;
        reply.disc.event_type=4;reply.disc.data=response;reply.ext_disc.props=10;reply.ext_disc.data=response;
        ++now_ms;dispatch_once(&reply,scan_callback_epoch,active?2U:1U);
        assert(scan_results[0].reports_seen==1 && !memcmp(scan_results[0].advertisement,original,sizeof(original)));
        if(active)assert(dual_response->live[0].wire.state==CERBERUS_DUAL_BLE_RESPONSE_PRESENT);

        /* An old callback token cannot invalidate the currently matched pair. */
        cerberus_ble_record_t retained=scan_results[0];
        cerberus_dual_ble_response_wire_t proof={0};if(active)proof=dual_response->live[0].wire;
        ++now_ms;dispatch_once(&event,scan_callback_epoch-1U,active?1U:3U);
        assert(!memcmp(&retained,&scan_results[0],sizeof(retained)));
        if(active)assert(!memcmp(&proof,&dual_response->live[0].wire,sizeof(proof)));

        /* Both legacy and EXT invalid payloads must reach response invalidation
         * before the original copier's null-data early return. */
        struct ble_gap_event bad=event;bad.disc.data=NULL;bad.ext_disc.data=NULL;
        ++now_ms;dispatch_once(&bad,scan_callback_epoch,1U);
        assert(scan_results[0].reports_seen==1);
        if(active)assert(!dual_response->live[0].wire.version);
        ++now_ms;dispatch_once(&reply,scan_callback_epoch,active?2U:1U);
        if(active)assert(!dual_response->live[0].wire.version);
        ++now_ms;dispatch_once(&event,scan_callback_epoch,active?2U:3U);
        ++now_ms;dispatch_once(&reply,scan_callback_epoch,active?2U:1U);
        if(active)assert(dual_response->live[0].wire.state==CERBERUS_DUAL_BLE_RESPONSE_PRESENT);

        if(extended){
            bad=event;bad.ext_disc.data_status=1;
            unsigned reports=scan_results[0].reports_seen;
            ++now_ms;dispatch_once(&bad,scan_callback_epoch,active?1U:3U);
            assert(scan_results[0].reports_seen==reports);
            if(active)assert(!dual_response->live[0].wire.version);
            ++now_ms;dispatch_once(&reply,scan_callback_epoch,active?2U:1U);
            if(active)assert(!dual_response->live[0].wire.version);
            ++now_ms;dispatch_once(&event,scan_callback_epoch,active?2U:3U);
            ++now_ms;dispatch_once(&reply,scan_callback_epoch,active?2U:1U);
        }
        bad=event;bad.disc.data=malformed;bad.disc.length_data=sizeof(malformed);
        bad.ext_disc.data=malformed;bad.ext_disc.length_data=sizeof(malformed);
        ++now_ms;dispatch_once(&bad,scan_callback_epoch,active?2U:3U);
        if(active)assert(!dual_response->live[0].wire.version);
        assert(scan_results[0].advertisement_flags&CERBERUS_BLE_ADVERTISEMENT_MALFORMED);
        ++now_ms;dispatch_once(&reply,scan_callback_epoch,active?2U:1U);
        if(active)assert(!dual_response->live[0].wire.version);
        command(CERBERUS_SPI_COMMAND_DUAL_BLE_STOP,&binding,sizeof(binding),&status,sizeof(status));
        command(CERBERUS_SPI_COMMAND_DUAL_BLE_RELEASE,&binding,sizeof(binding),&status,sizeof(status));
        assert(!dual_attached && !dual_response);
    }
    unsigned checks_before=response_mode_checks,locks_before=mutex_takes;
    record_advertisement(NULL,scan_callback_epoch,0);
    assert(response_mode_checks==checks_before && mutex_takes==locks_before);
    assert(response_allocations==response_frees);
    puts("C5 actual callback dispatch: legacy/EXT mode predicate once, passive3-lock/active2-lock originals, response ownership, partial/null/malformed invalidation and stale-token retention passed");
}

static cerberus_dual_ble_snapshot_chunk_t compact_chunk(
    cerberus_dual_ble_snapshot_read_t request, uint8_t error)
{
    cerberus_dual_ble_snapshot_chunk_t out;
    memset(&out,0xa5,sizeof(out));
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_RESPONSE_COMPACT,
        &request,sizeof(request),&out,sizeof(out));
    assert(out.handle==request.handle && out.index==request.index && out.chunk==request.chunk && out.error==error);
    if(error){assert(!out.byte_count);for(unsigned i=0;i<sizeof(out.data);++i)assert(!out.data[i]);}
    return out;
}

static void response_compact_checks(void)
{
    uint8_t original[]={6,9,'H','C','-','0','5'};
    uint8_t response[]={6,9,'H','C','-','0','6'};
    const unsigned lengths[]={160,174,310};
    now_ms=1000000;
    for(unsigned focused=0;focused<2;++focused){
        cerberus_dual_ble_start_t start={.boot_id=dual_boot_id,.job_id=dual_job_highwater+1U,.version=2,
            .mode=focused?CERBERUS_DUAL_BLE_RESPONSE_FAMILY:CERBERUS_DUAL_BLE_RESPONSE_DISCOVERY,
            .reserved=focused?6:0};
        cerberus_dual_ble_status_t owner;
        command(CERBERUS_SPI_COMMAND_DUAL_BLE_START,&start,sizeof(start),&owner,sizeof(owner));
        assert(!owner.error_code && dual_response);
        cerberus_dual_ble_binding_t binding={dual_boot_id,start.job_id};
        struct ble_gap_event event={.type=BLE_GAP_EVENT_EXT_DISC,.ext_disc={.props=2,
            .addr={{90,2,3,4,5,6},1},.rssi=-70,.sid=7,.prim_phy=1,.sec_phy=1,
            .length_data=sizeof(original),.data=original}};
        /* Use real callbacks to create UNKNOWN (a partial original invalidates
         * correlation), NONE (eligible original), and PRESENT in one bank. */
        for(unsigned index=0;index<3;++index){
            event.ext_disc.addr.val[0]=(uint8_t)(90U+index);
            event.ext_disc.props=2;event.ext_disc.data_status=0;event.ext_disc.data=original;
            ++now_ms;assert(!gap_event(&event,(void*)(uintptr_t)scan_callback_epoch));
            if(index!=1){
                if(!index)event.ext_disc.data_status=1;
                else {event.ext_disc.props=10;event.ext_disc.data=response;event.ext_disc.rssi=-25;}
                ++now_ms;assert(!gap_event(&event,(void*)(uintptr_t)scan_callback_epoch));
            }
        }
        assert(scan_status.stored_count==3);
        ++now_ms;cerberus_dual_ble_snapshot_status_t frozen=snapshot_begin(binding,1);
        assert(!frozen.error_code && frozen.count==3);
        const unsigned starts_before=starts,stops_before=stops,allocations=response_allocations;
        uint8_t old[3][310];
        for(unsigned index=0;index<3;++index){
            for(unsigned part=0;part<31;++part){
                cerberus_dual_ble_snapshot_read_t request={binding,frozen.handle,(uint8_t)index,(uint8_t)part};
                cerberus_dual_ble_snapshot_chunk_t full;
                command(CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_RESPONSE,&request,sizeof(request),&full,sizeof(full));
                assert(!full.error && full.byte_count==10);
                memcpy(old[index]+10U*part,full.data,10);
            }
            assert(old[index][159]==index && old[index][158]==(index?1:0));
            /* Verify the omitted full-read bytes really are canonical zero.
             * NONE must preserve every byte of the nonzero LE64 sequence. */
            for(unsigned i=lengths[index];i<310;++i)assert(!old[index][i]);
            if(index==1){uint64_t sequence=0;
                for(unsigned i=0;i<8;++i)sequence|=(uint64_t)old[index][166U+i]<<(8U*i);
                assert(sequence==dual_response->live[1].wire.original_sequence && sequence);
            }
        }
        unsigned reads=0;
        for(unsigned index=0;index<3;++index){
            const unsigned count=(lengths[index]+9U)/10U;
            for(unsigned part=0;part<count;++part){
                /* Update the corresponding live source between every chunk.
                 * The projection and its original FIRST BEGIN stay frozen. */
                event.ext_disc.addr.val[0]=(uint8_t)(90U+index);
                event.ext_disc.props=2;event.ext_disc.data_status=0;event.ext_disc.data=original;
                ++now_ms;assert(!gap_event(&event,(void*)(uintptr_t)scan_callback_epoch));
                cerberus_dual_ble_snapshot_read_t request={binding,frozen.handle,(uint8_t)index,(uint8_t)part};
                cerberus_dual_ble_snapshot_chunk_t compact=compact_chunk(request,CERBERUS_DUAL_BLE_SNAPSHOT_OK);
                const unsigned remaining=lengths[index]-part*10U, amount=remaining<10U?remaining:10U;
                assert(compact.byte_count==amount);++reads;
                for(unsigned j=0;j<10;++j){
                    unsigned offset=part*10U+j;
                    uint8_t expected=j>=amount?0:offset==158U?1:old[index][offset];
                    assert(compact.data[j]==expected);
                }
            }
            for(unsigned part=count;part<=UINT8_MAX;++part){
                cerberus_dual_ble_snapshot_read_t request={binding,frozen.handle,(uint8_t)index,(uint8_t)part};
                compact_chunk(request,CERBERUS_DUAL_BLE_SNAPSHOT_RANGE);
            }
            /* The new command must not mutate the old full310 representation. */
            for(unsigned part=0;part<31;++part){
                cerberus_dual_ble_snapshot_read_t request={binding,frozen.handle,(uint8_t)index,(uint8_t)part};
                cerberus_dual_ble_snapshot_chunk_t full;
                command(CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_RESPONSE,&request,sizeof(request),&full,sizeof(full));
                assert(!full.error && full.byte_count==10 && !memcmp(full.data,old[index]+part*10U,10));
            }
        }
        assert(reads==65 && starts==starts_before && stops==stops_before && response_allocations==allocations);
        cerberus_dual_ble_snapshot_status_t retry=snapshot_begin(binding,1);
        assert(!memcmp(&frozen,&retry,sizeof(frozen)));
        cerberus_dual_ble_snapshot_status_t fence=snapshot_status(binding,frozen.handle);
        assert(!memcmp(&frozen,&fence,sizeof(frozen)));

        /* Corrupt each omitted/canonical field independently. No shortened
         * reply may conceal corruption, even when only prefix chunk0 is read. */
        for(unsigned index=0;index<2;++index){
            cerberus_dual_ble_snapshot_read_t request={binding,frozen.handle,(uint8_t)index,0};
            for(unsigned byte=0;byte<152;++byte){
                if(index && byte>=8U && byte<16U)continue;
                uint8_t saved=dual_response->frozen[index][byte];
                dual_response->frozen[index][byte]^=0x80U;
                compact_chunk(request,CERBERUS_DUAL_BLE_SNAPSHOT_INVALID);
                dual_response->frozen[index][byte]=saved;
            }
            compact_chunk(request,CERBERUS_DUAL_BLE_SNAPSHOT_OK);
        }
        uint8_t sequence[8];memcpy(sequence,dual_response->frozen[1]+8,8);
        memset(dual_response->frozen[1]+8,0,8);
        cerberus_dual_ble_snapshot_read_t request={binding,frozen.handle,1,0};
        compact_chunk(request,CERBERUS_DUAL_BLE_SNAPSHOT_INVALID);
        memcpy(dual_response->frozen[1]+8,sequence,8);
        request.index=2;
        for(unsigned byte=0;byte<2;++byte){
            uint8_t saved=dual_response->frozen[2][byte];dual_response->frozen[2][byte]=255;
            compact_chunk(request,CERBERUS_DUAL_BLE_SNAPSHOT_INVALID);dual_response->frozen[2][byte]=saved;
        }
        for(unsigned bad_index=3;bad_index<=UINT8_MAX;++bad_index){
            request.index=(uint8_t)bad_index;compact_chunk(request,CERBERUS_DUAL_BLE_SNAPSHOT_RANGE);
        }
        request.index=0;
        snapshot_busy=true;compact_chunk(request,CERBERUS_DUAL_BLE_SNAPSHOT_BUSY);snapshot_busy=false;
        for(unsigned fault=0;fault<4;++fault){
            cerberus_dual_ble_snapshot_read_t wrong=request;uint64_t epoch=dual_epoch;
            if(fault==0)++wrong.binding.boot_id;if(fault==1)++wrong.binding.job_id;
            if(fault==2)++wrong.handle;if(fault==3)++dual_epoch;
            compact_chunk(wrong,CERBERUS_DUAL_BLE_SNAPSHOT_STALE);dual_epoch=epoch;
        }
        uint8_t saved_mode=dual_mode;dual_mode=CERBERUS_DUAL_BLE_DISCOVERY;
        compact_chunk(request,CERBERUS_DUAL_BLE_SNAPSHOT_INVALID);dual_mode=saved_mode;
        c5_response_context_t *context=dual_response;dual_response=NULL;
        compact_chunk(request,CERBERUS_DUAL_BLE_SNAPSHOT_INVALID);dual_response=context;
        void *saved_mutex=scanner_mutex;scanner_mutex=NULL;
        compact_chunk(request,CERBERUS_DUAL_BLE_SNAPSHOT_INVALID);scanner_mutex=saved_mutex;
        uint8_t malformed[sizeof(request)+1];memcpy(malformed,&request,sizeof(request));malformed[sizeof(request)]=0;
        for(unsigned length=0;length<=sizeof(malformed);++length)if(length!=sizeof(request)){
            cerberus_dual_ble_snapshot_chunk_t out;memset(&out,0xa5,sizeof(out));
            command(CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_RESPONSE_COMPACT,malformed,length,&out,sizeof(out));
            assert(out.error==CERBERUS_DUAL_BLE_SNAPSHOT_INVALID && !out.handle && !out.index && !out.chunk && !out.byte_count);
            for(unsigned i=0;i<sizeof(out.data);++i)assert(!out.data[i]);
        }
        cerberus_dual_ble_snapshot_status_t replacement=snapshot_begin(binding,2);
        assert(!replacement.error_code && replacement.handle!=frozen.handle);
        compact_chunk(request,CERBERUS_DUAL_BLE_SNAPSHOT_STALE);
        request.handle=replacement.handle;compact_chunk(request,CERBERUS_DUAL_BLE_SNAPSHOT_OK);
        command(CERBERUS_SPI_COMMAND_DUAL_BLE_STOP,&binding,sizeof(binding),&owner,sizeof(owner));
        compact_chunk(request,CERBERUS_DUAL_BLE_SNAPSHOT_STALE);
        command(CERBERUS_SPI_COMMAND_DUAL_BLE_RELEASE,&binding,sizeof(binding),&owner,sizeof(owner));
        compact_chunk(request,CERBERUS_DUAL_BLE_SNAPSHOT_STALE);
        assert(!dual_attached && !dual_response && response_allocations==response_frees);
    }
    puts("C5 compact response actual callbacks/service: UNKNOWN160/NONE174/PRESENT310, 65 versus93 reads, frozen byte equivalence, all index/chunk bounds, canonical omission faults, busy/binding/malformed/lifecycle rejection passed");
}

int main(void)
{
    dual_boot_id=123;host_synced=true;now_ms=100;
    cerberus_dual_ble_info_t info;command(CERBERUS_SPI_COMMAND_DUAL_BLE_INFO,NULL,0,&info,22);
    assert(info.boot_id==123 && info.next_job_id==1 && (info.flags&CERBERUS_DUAL_BLE_READY));
    cerberus_dual_ble_start_t start={.boot_id=123,.job_id=1,.mode=1,.address={1,2,3,4,5,6},.address_type=1,.version=1};
    cerberus_dual_ble_status_t status;
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_START,&start,22,&status,22);
    assert(!status.error_code && status.epoch==1 && dual_attached && dual_radio_active && starts==1);
    cerberus_dual_ble_start_t changed_version=start;changed_version.version=2;
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_START,&changed_version,22,&status,22);assert(status.error_code&&dual_version==1);
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_START,&start,22,&status,22);assert(starts==1);
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_INFO,NULL,0,&info,22);assert(info.flags&CERBERUS_DUAL_BLE_ATTACHED);
    cerberus_dual_ble_start_t other=start;other.address_type=0;
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_START,&other,22,&status,22);assert(status.error_code && starts==1);
    cerberus_dual_ble_binding_t b={123,1},foreign={123,2};
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_STOP,&foreign,12,&status,22);assert(status.error_code==ESP_ERR_NOT_FOUND && !stops);
    struct ble_gap_disc_desc packet={.addr={{1,2,3,4,5,6},1},.rssi=-35,.event_type=0};
    record_advertisement(&packet,scan_callback_epoch,100000);
    packet.rssi=-90;record_advertisement(&packet,scan_callback_epoch,150000);
    assert(dual_track_seen && dual_track_rssi==-90 && dual_track_reports==2 && scan_results[0].rssi==-35);
    assert(scan_results[0].latest_rssi==-90 && scan_results[0].original_observed_ms==150);
    packet.addr.type=0;record_advertisement(&packet,scan_callback_epoch,170000);
    assert(dual_track_reports==2 && scan_status.stored_count==2);
    packet.addr.type=1;packet.rssi=127;record_advertisement(&packet,scan_callback_epoch,180000);
    assert(dual_track_reports==2 && dual_track_ms==150);
    packet.rssi=-60;packet.event_type=4;record_advertisement(&packet,scan_callback_epoch,190000);
    assert(dual_track_reports==2); /* Scan responses never masquerade as original ADV. */
    packet.event_type=0;record_advertisement(&packet,scan_callback_epoch-1,190000);assert(dual_track_reports==2);
    scan_status.stored_count=32;memset(scan_results,0,sizeof(scan_results));
    record_advertisement(&packet,scan_callback_epoch,200000);assert(dual_track_reports==3 && dual_track_rssi==-60);
    now_ms=250;cerberus_dual_ble_track_t track;
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_TRACK_STATUS,&b,12,&track,22);
    assert(track.age_ms==50 && track.reports==3 && track.rssi==-60 && track.flags==3);
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_RELEASE,&b,12,&status,22);assert(status.error_code && dual_attached);
    cancel_fails=true;command(CERBERUS_SPI_COMMAND_DUAL_BLE_STOP,&b,12,&status,22);
    assert(status.error_code && (status.flags&CERBERUS_DUAL_BLE_OWNED) && dual_attached);
    cancel_fails=false;command(CERBERUS_SPI_COMMAND_DUAL_BLE_STOP,&b,12,&status,22);
    assert(!(status.flags&CERBERUS_DUAL_BLE_OWNED) && dual_attached);
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_RELEASE,&b,12,&status,22);assert(!dual_attached && !status.flags);
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_RELEASE,&b,12,&status,22);
    assert(status.error_code==ESP_ERR_NOT_FOUND&&status.version==1); /* Lost v1 RELEASE resolves. */
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_START,&start,22,&status,22);assert(status.error_code && starts==1 && !dual_attached);
    other=start;other.boot_id=456;
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_START,&other,22,&status,22);assert(status.error_code==ESP_ERR_NOT_FOUND && starts==1);
    other=start;other.job_id=2;other.mode=0;memset(other.address,0,6);other.address_type=0;
    other_radio_busy=true;command(CERBERUS_SPI_COMMAND_DUAL_BLE_START,&other,22,&status,22);
    assert(status.error_code && !dual_attached && starts==1);other_radio_busy=false;
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_INFO,NULL,0,&info,22);assert(info.next_job_id==3);
    other.job_id=info.next_job_id;
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_START,&other,22,&status,22);assert(starts==2 && dual_attached);
    now_ms+=13000;command(CERBERUS_SPI_COMMAND_DUAL_BLE_INFO,NULL,0,&info,22);assert(dual_attached); /* Running RF is never assumed gone. */
    dual_radio_active=false;scan_status.state=CERBERUS_BLE_SCAN_COMPLETE;
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_INFO,NULL,0,&info,22);assert(!dual_attached); /* Completed orphan expires. */
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_START,&other,22,&status,22);assert(status.error_code && !dual_attached && starts==2);
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_START,&start,22,&status,22);assert(status.error_code && !dual_attached && starts==2);
    snapshot_checks();
    dense_inventory_checks();
    callback_metadata_checks();
    analyzer_checks();
    family_checks();
    response_checks();
    response_dense_clock_checks();
    response_dispatch_once_checks();
    response_compact_checks();
    puts("dual BLE C5 owner/callback: exact typed tracker, latest RSSI, capacity, idempotency, foreign ownership, orphan checks passed");
    return 0;
}
