#undef NDEBUG
#include "cerberus_watch_capture.h"
#include "dual_ble_protocol.h"
#include "ble_analyzer_counter.h"
#include "ble_family_wire.h"
#include "../../c5_firmware/main/ble_inventory_admission.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "ble_response.h"

static unsigned checks;
#define CHECK(x) do { ++checks; assert(x); } while (0)
#define MYNEWT_VAL(x) TEST_EXTENDED
#define portMAX_DELAY 0
#define BLE_SCAN_DURATION_MS 8000U
#define BLE_HS_FOREVER 0
#define BLE_NPL_OK 0
#define BLE_HS_EOS 3
#define BLE_HS_EALREADY 2
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(tag,format,...) do { if(false) printf(format,__VA_ARGS__); } while(0)
enum { ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE, ESP_ERR_NOT_FINISHED };
typedef int esp_err_t;
typedef struct { uint8_t type,val[6]; } ble_addr_t;
struct ble_gap_disc_desc { ble_addr_t addr; int8_t rssi; uint8_t event_type,length_data; const uint8_t *data; };
/* Match the installed SDK's eight-bit callback fields; this fixture does not
 * invent >255-byte reports or silently widen properties. */
struct ble_gap_ext_disc_desc { uint8_t props,data_status,legacy_event_type; ble_addr_t addr;
    int8_t rssi,tx_power; uint8_t sid,prim_phy,sec_phy; uint16_t periodic_adv_itvl;
    uint8_t length_data; const uint8_t *data; ble_addr_t direct_addr; };
enum { BLE_GAP_EVENT_DISC=1,BLE_GAP_EVENT_EXT_DISC,BLE_GAP_EVENT_DISC_COMPLETE };
enum { BLE_HCI_ADV_RPT_EVTYPE_ADV_IND=0,BLE_HCI_ADV_RPT_EVTYPE_DIR_IND=1,
       BLE_HCI_ADV_RPT_EVTYPE_NONCONN_IND=3,BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP=4 };
enum { BLE_HCI_ADV_CONN_MASK=1,BLE_HCI_ADV_SCAN_RSP_MASK=8,BLE_HCI_ADV_LEGACY_MASK=16,
       BLE_GAP_EXT_ADV_DATA_STATUS_COMPLETE=0 };
struct ble_gap_event { int type; struct ble_gap_disc_desc disc; struct ble_gap_ext_disc_desc ext_disc; struct { int reason; } disc_complete; };
struct ble_hs_adv_fields { unsigned unused; };
struct ble_npl_event { void *arg; };
struct ble_npl_callout { unsigned unused; };
struct ble_gap_disc_params { unsigned itvl,window,filter_policy,limited,passive,filter_duplicates; };
struct ble_gap_ext_disc_params { unsigned itvl,window,passive; };

#include "c5_ble_watch_record_type.h"
#include "../../c5_firmware/main/ble_response_state.h"
static int mutex;
static void *scanner_mutex=&mutex;
static bool locked,host_synced=true,scan_cancelled;
static struct ble_npl_event start_event;
static struct ble_npl_callout scan_timeout_callout;
static cerberus_ble_scan_status_payload_t scan_status;
static cerberus_ble_connection_status_payload_t connection_status;
static cerberus_ble_record_t scan_results[CERBERUS_BLE_SCAN_MAX_RESULTS];
static cerberus_watch_capture_t watch_capture;
static cerberus_ble_activity_capture_t activity_capture;
static uint64_t watch_capture_epoch;
static uint32_t scan_callback_epoch;
static uint64_t dual_epoch;
static c5_response_context_t *dual_response;
static struct {bool valid;} dual_snapshot;
/* Ordinary Watch never owns the separate companion protocol. Keep the actual
 * scanner admission and callback branches compiled; companion lifecycle and
 * typed-target behavior are exercised by check_dual_ble.py. */
static bool dual_attached,dual_radio_active,dual_track_seen;
static uint8_t dual_mode,dual_address[6],dual_address_type,dual_generation,dual_version,dual_family;
static cerberus_ble_inventory_admission_t scan_inventory;
static cerberus_dual_ble_family_wire_t scan_family[CERBERUS_DUAL_BLE_CAPACITY];
static uint32_t dual_track_ms,dual_track_reports;
static int8_t dual_track_rssi;
static int32_t dual_error;
static cerberus_ble_analyzer_counter_t dual_analyzer;
#include "c5_ble_control_state.h"
static void dual_expire_locked(void){CHECK(locked);}
static uint8_t diagnostic_phase;
static int64_t clock_us,parse_delay;
static unsigned parse_epoch_change,queued,rf_starts,timeout_stops,parse_calls,mutex_takes;
static int infer_error,rf_error,callout_error;
static void *rf_arg;
static int (*rf_callback)(struct ble_gap_event *,void *);
static int64_t esp_timer_get_time(void){return clock_us;}
static void xSemaphoreTake(void *m,unsigned ticks){(void)ticks;++mutex_takes;CHECK(m==scanner_mutex&&!locked);locked=true;}
static void xSemaphoreGive(void *m){CHECK(m==scanner_mutex&&locked);locked=false;}
static int ble_hs_adv_parse_fields(struct ble_hs_adv_fields *f,const uint8_t *p,unsigned n)
{(void)f;(void)p;(void)n;++parse_calls;clock_us+=parse_delay;if(parse_epoch_change){++scan_callback_epoch;parse_epoch_change=0;}return 0;}
static int find_record(const ble_addr_t *a){(void)a;return -1;}
static void copy_advertisement_fields(cerberus_ble_record_t *r,const struct ble_hs_adv_fields *f){(void)r;(void)f;CHECK(false);}
static void copy_raw_advertisement(cerberus_ble_record_t *r,const struct ble_gap_disc_desc *d,bool p){(void)r;(void)d;(void)p;CHECK(false);}
static void ble_npl_callout_stop(struct ble_npl_callout *c){CHECK(c==&scan_timeout_callout);++timeout_stops;}
static void *ble_npl_event_get_arg(struct ble_npl_event *e){return e->arg;}
static void ble_npl_event_set_arg(struct ble_npl_event *e,void *arg){e->arg=arg;}
static void *nimble_port_get_dflt_eventq(void){return &mutex;}
static void ble_npl_eventq_put(void *q,struct ble_npl_event *e){CHECK(q==&mutex&&e==&start_event);++queued;}
static int ble_hs_id_infer_auto(int privacy,uint8_t *type){CHECK(!privacy);*type=0;return infer_error;}
static int ble_gap_disc_cancel(void){return 0;}
static int ble_gap_disc_active(void){return dual_radio_active;}
static unsigned ble_npl_time_ms_to_ticks32(unsigned ms){return ms;}
static int ble_npl_callout_reset(struct ble_npl_callout *c,unsigned ticks){CHECK(c==&scan_timeout_callout&&ticks==8000);return callout_error;}
static bool cerberus_c5_test_running(void){return false;}
static bool another_radio_operation_is_active(void){return false;}
static esp_err_t cerberus_wifi_channel_survey_preempt(void){return ESP_OK;}
#if TEST_EXTENDED
static int ble_gap_ext_disc(uint8_t type,unsigned a,unsigned b,unsigned c,unsigned d,unsigned e,
    const struct ble_gap_ext_disc_params *one,const struct ble_gap_ext_disc_params *coded,
    int (*cb)(struct ble_gap_event *,void *),void *arg)
{CHECK(!type&&!a&&!b&&!c&&!d&&!e&&coded->passive==1&&one->passive==(cerberus_dual_ble_response_mode(dual_mode)&&dual_response?0U:1U)&&one->itvl==160&&one->window==80);
 ++rf_starts;rf_callback=cb;rf_arg=arg;return rf_error;}
#else
static int ble_gap_disc(uint8_t type,int forever,const struct ble_gap_disc_params *p,
    int (*cb)(struct ble_gap_event *,void *),void *arg)
{CHECK(!type&&!forever&&p->passive==(cerberus_dual_ble_response_mode(dual_mode)&&dual_response?0U:1U)&&!p->filter_duplicates&&p->itvl==(p->passive?0U:160U)&&p->window==(p->passive?0U:80U));
 ++rf_starts;rf_callback=cb;rf_arg=arg;return rf_error;}
#endif
#include "../../c5_firmware/main/ble_response_impl.h"
#include "c5_ble_watch_timing_impl.h"

/* Canonical 31-byte BLE Service Data carrying BasicID, independently encoded. */
static uint8_t advertisement[64]={30,0x16,0xfa,0xff,0x0d,7,0x02,0x10,'T','E','S','T','-','A'};
static struct ble_gap_event report;
static void start(void)
{
    unsigned count=rf_starts;CHECK(cerberus_ble_scanner_start()==ESP_OK);
    CHECK(scan_status.state==CERBERUS_BLE_SCAN_RUNNING&&queued==count+1);
    start_scan_event(&start_event);CHECK(rf_starts==count+1&&rf_arg);
    /* The Watch path must work even when ordinary inventory is already full. */
    scan_status.stored_count=CERBERUS_BLE_SCAN_MAX_RESULTS;
    memset(&report,0,sizeof(report));report.type=BLE_GAP_EVENT_DISC;
    report.disc=(struct ble_gap_disc_desc){.addr={.type=1,.val={2,4,6,8,10,12}},.rssi=-40,
        .event_type=3,.length_data=31,.data=advertisement};
}
static void receive(void){CHECK(rf_callback(&report,rf_arg)==0&&!locked);}
static void complete(void){struct ble_gap_event e={.type=BLE_GAP_EVENT_DISC_COMPLETE};CHECK(rf_callback(&e,rf_arg)==0);}
static cerberus_watch_scan_timing_payload_t query(void)
{
    cerberus_watch_scan_timing_request_t q={CERBERUS_WATCH_RADIO_BLE,scan_status.generation,0};
    cerberus_watch_scan_timing_payload_t out;
    CHECK(cerberus_ble_watch_get_timing(&q,&out)==ESP_OK&&!locked);return out;
}
static void entry_clock_and_qualification(void)
{
    scan_status.generation=255;clock_us=0;start();CHECK(scan_status.generation==0&&watch_capture.capture_epoch==1);
    parse_delay=5000;receive();CHECK(clock_us==5000&&watch_capture.qualifying_valid==1&&watch_capture.qualifying_us[0]==0);
    parse_delay=0;clock_us=9000;report.disc.rssi=-90;receive();CHECK(watch_capture.qualifying_us[0]==9000&&watch_capture.records[0].rssi==-90);
    clock_us=10000;report.disc.length_data=30;receive();CHECK(watch_capture.qualifying_us[0]==9000);
    report.disc.length_data=31;report.disc.event_type=4;receive();CHECK(watch_capture.qualifying_us[0]==9000);
    report.disc.event_type=3;report.disc.addr.type=255;receive();CHECK(watch_capture.qualifying_us[0]==9000);
    complete();clock_us=10001;cerberus_watch_scan_timing_payload_t out=query();CHECK(out.flags==1&&out.source_age_ms==2&&out.capture_epoch==1);
    clock_us=-1;out=query();CHECK(!out.flags&&!out.source_age_ms&&out.capture_epoch==1);
    cerberus_watch_scan_timing_request_t wrong={CERBERUS_WATCH_RADIO_WIFI,0,0};memset(&out,0xa5,sizeof(out));
    CHECK(cerberus_ble_watch_get_timing(&wrong,&out)==ESP_ERR_INVALID_STATE&&!out.capture_epoch);
    clock_us=100000;start();receive();CHECK(watch_capture.qualifying_valid==1);
    clock_us=-1;receive();CHECK(!watch_capture.qualifying_valid);complete();out=query();CHECK(!out.flags);
}
static void stale_delivery_and_saturation(void)
{
    void *old=rf_arg;clock_us=200000;scan_status.generation=255;start();CHECK(scan_status.generation==0&&rf_arg!=old);
    unsigned stopped=timeout_stops;struct ble_gap_event done={.type=BLE_GAP_EVENT_DISC_COMPLETE};
    CHECK(gap_event(&report,old)==0&&!watch_capture.status.reports);
    CHECK(gap_event(&done,old)==0&&scan_status.state==CERBERUS_BLE_SCAN_RUNNING&&timeout_stops==stopped);
    CHECK(gap_event(&done,NULL)==0&&scan_status.state==CERBERUS_BLE_SCAN_RUNNING);
    struct ble_npl_event stale={.arg=old};unsigned runs=rf_starts;start_scan_event(&stale);CHECK(rf_starts==runs);
    parse_epoch_change=1;receive();CHECK(!watch_capture.status.reports);--scan_callback_epoch;
    receive();CHECK(watch_capture.status.stored_count==1);scan_cancelled=true;finish_scan_for_epoch(0,scan_callback_epoch);
    CHECK(!watch_capture.qualifying_valid&&scan_status.state==CERBERUS_BLE_SCAN_IDLE);
    watch_capture_epoch=UINT64_MAX-1;start();receive();complete();CHECK(query().capture_epoch==UINT64_MAX);
    start();CHECK(!watch_capture.capture_epoch&&watch_capture_epoch==UINT64_MAX);receive();complete();
    CHECK(watch_capture.status.stored_count==1&&!watch_capture.qualifying_valid);
    cerberus_watch_scan_timing_request_t q={CERBERUS_WATCH_RADIO_BLE,scan_status.generation,0};cerberus_watch_scan_timing_payload_t out;
    CHECK(cerberus_ble_watch_get_timing(&q,&out)==ESP_ERR_INVALID_STATE&&!out.capture_epoch);
}
static void extended_admission(void)
{
#if TEST_EXTENDED
    watch_capture_epoch=500;clock_us=800000;start();
    report.type=BLE_GAP_EVENT_EXT_DISC;report.ext_disc=(struct ble_gap_ext_disc_desc){
        .addr=report.disc.addr,.rssi=-50,.length_data=31,.data=advertisement};
    parse_delay=7000;receive();parse_delay=0;CHECK(watch_capture.qualifying_valid==1&&watch_capture.qualifying_us[0]==800000);
    clock_us=900000;report.ext_disc.data_status=1;receive();CHECK(watch_capture.qualifying_us[0]==800000);
    report.ext_disc.data_status=0;report.ext_disc.props=BLE_HCI_ADV_SCAN_RSP_MASK;receive();CHECK(watch_capture.qualifying_us[0]==800000);
    report.ext_disc.props=0;report.ext_disc.rssi=-90;receive();CHECK(watch_capture.qualifying_us[0]==900000);
    struct ble_gap_event done={.type=BLE_GAP_EVENT_DISC_COMPLETE,.disc_complete={.reason=1}};
    CHECK(gap_event(&done,rf_arg)==0&&scan_status.state==CERBERUS_BLE_SCAN_ERROR&&!watch_capture.qualifying_valid);
#endif
}
static const uint8_t motion_location[25]={0x12,7,180,254,0,0,0,0,0,0,0,0,0,0,0,0,0,255,255};
static const uint8_t motion_basic[25]={0x02,0x10,'T','E','S','T','-','A'};
static void motion_start(void)
{
    memcpy(advertisement+6,motion_basic,25);start();
#if TEST_EXTENDED
    report.type=BLE_GAP_EVENT_EXT_DISC;
    report.ext_disc=(struct ble_gap_ext_disc_desc){.addr=report.disc.addr,.rssi=-40,.length_data=31,.data=advertisement};
#endif
}
static void motion_receive(const uint8_t message[25],int64_t at)
{memcpy(advertisement+6,message,25);clock_us=at;receive();}
static void motion_wire(void)
{
    watch_capture_epoch=1000;clock_us=0;motion_start();receive();
    parse_delay=9000;motion_receive(motion_location,1000);parse_delay=0;
    CHECK(clock_us==10000&&watch_capture.motion_us[0]==1000);
    motion_receive(motion_basic,20000);
    uint8_t malformed[25];memcpy(malformed,motion_location,25);malformed[24]=1;motion_receive(malformed,30000);
    memcpy(advertisement+6,motion_location,25);clock_us=40000;
#if TEST_EXTENDED
    report.ext_disc.data_status=1;receive();report.ext_disc.data_status=0;
    report.ext_disc.props=BLE_HCI_ADV_SCAN_RSP_MASK;receive();report.ext_disc.props=0;
    report.ext_disc.length_data=32;advertisement[31]=1;receive();report.ext_disc.length_data=31;
#else
    report.disc.event_type=4;receive();report.disc.event_type=3;
    report.disc.length_data=32;advertisement[31]=1;receive();report.disc.length_data=31;
#endif
    CHECK(watch_capture.qualifying_us[0]==20000&&watch_capture.motion_us[0]==1000);
    complete();clock_us=40001;
    cerberus_watch_scan_data_request_t q={CERBERUS_WATCH_RADIO_BLE,scan_status.generation,0,0};
    cerberus_watch_scan_motion_payload_t data;cerberus_watch_scan_timing_payload_t age;
    CHECK(cerberus_ble_watch_get_motion(&q,&data)==ESP_OK);
    CHECK(data.radio==q.radio&&data.generation==q.generation&&!data.index&&data.capture_epoch==1001);
    CHECK(!data.reserved0&&!data.reserved[0]&&!data.reserved[1]);
    CHECK(data.motion.flags==31&&data.motion.speed_qms==1017&&data.motion.direction_deg==360&&data.motion.height_encoded==65535&&!data.motion.reserved);
    CHECK(cerberus_ble_watch_get_motion_timing(&q,&age)==ESP_OK&&age.flags==1&&age.source_age_ms==40);
    CHECK(age.capture_epoch==data.capture_epoch&&age.radio==q.radio&&age.generation==q.generation&&!age.index);
    CHECK(query().source_age_ms==21); /* Basic ID receipt cannot freshen motion. */
    sentinel_watch_motion_t retained=data.motion;
    clock_us=999;CHECK(cerberus_ble_watch_get_motion_timing(&q,&age)==ESP_OK&&!age.flags&&!age.source_age_ms);
    clock_us=-1;CHECK(cerberus_ble_watch_get_motion_timing(&q,&age)==ESP_OK&&!age.flags&&!age.source_age_ms&&age.capture_epoch==1001);
    CHECK(cerberus_ble_watch_get_motion(&q,&data)==ESP_OK&&!memcmp(&data.motion,&retained,sizeof(retained)));
    for(unsigned defect=0;defect<9;++defect){
        cerberus_watch_scan_data_request_t wrong=q;
        if(defect==0)wrong.radio=CERBERUS_WATCH_RADIO_WIFI;
        if(defect==1)++wrong.generation;
        if(defect==2)wrong.index=1;
        if(defect==3)wrong.chunk=1;
        if(defect==5)watch_capture.status.state=CERBERUS_WATCH_RUNNING;
        if(defect==6)watch_capture.capture_epoch=0;
        if(defect==7)watch_capture.motion[0].reserved=1;
        if(defect==8)scanner_mutex=NULL;
        memset(&data,0xa5,sizeof(data));memset(&age,0xa5,sizeof(age));
        CHECK(cerberus_ble_watch_get_motion(defect==4?NULL:&wrong,&data)!=ESP_OK);
        CHECK(cerberus_ble_watch_get_motion_timing(defect==4?NULL:&wrong,&age)!=ESP_OK);
        for(size_t i=0;i<sizeof(data);++i)CHECK(!((uint8_t*)&data)[i]);
        for(size_t i=0;i<sizeof(age);++i)CHECK(!((uint8_t*)&age)[i]);
        watch_capture.status.state=CERBERUS_WATCH_COMPLETE;watch_capture.capture_epoch=1001;watch_capture.motion[0]=retained;scanner_mutex=&mutex;
    }
    CHECK(cerberus_ble_watch_get_motion(&q,NULL)==ESP_ERR_INVALID_ARG);
    CHECK(cerberus_ble_watch_get_motion_timing(&q,NULL)==ESP_ERR_INVALID_ARG);
    clock_us=0;motion_start();motion_receive(motion_location,0);complete();q.generation=scan_status.generation;clock_us=INT64_MAX;
    CHECK(cerberus_ble_watch_get_motion_timing(&q,&age)==ESP_OK&&age.flags==1&&age.source_age_ms==UINT64_C(9223372036854776));
    clock_us=0;motion_start();motion_receive(motion_location,1000);
    uint8_t unavailable[25]={0x12,3,181,255};motion_receive(unavailable,2000);complete();q.generation=scan_status.generation;clock_us=2001;
    CHECK(cerberus_ble_watch_get_motion(&q,&data)==ESP_OK&&data.motion.flags==1&&!data.motion.speed_qms&&!data.motion.direction_deg&&!data.motion.height_encoded);
    CHECK(cerberus_ble_watch_get_motion_timing(&q,&age)==ESP_OK&&age.flags==1&&age.source_age_ms==1);
    motion_start();motion_receive(motion_location,-1);complete();q.generation=scan_status.generation;clock_us=1000;
    CHECK(cerberus_ble_watch_get_motion(&q,&data)==ESP_OK&&data.motion.flags==31);
    CHECK(cerberus_ble_watch_get_motion_timing(&q,&age)==ESP_OK&&!age.flags&&!age.source_age_ms);
    motion_start();motion_receive(motion_basic,0);motion_receive(motion_location,1000);
    uint8_t replacement[25];memcpy(replacement,motion_basic,25);replacement[7]='B';motion_receive(replacement,2000);
    complete();q.generation=scan_status.generation;clock_us=3000;
    CHECK(cerberus_ble_watch_get_motion(&q,&data)==ESP_OK&&!data.motion.flags);
    CHECK(cerberus_ble_watch_get_motion_timing(&q,&age)==ESP_OK&&!age.flags&&!age.source_age_ms);
    motion_start();receive();complete();q.generation=scan_status.generation;
    CHECK(cerberus_ble_watch_get_motion(&q,&data)==ESP_OK&&data.capture_epoch==watch_capture.capture_epoch);
    for(size_t i=0;i<sizeof(data.motion);++i)CHECK(!((uint8_t*)&data.motion)[i]);
    CHECK(cerberus_ble_watch_get_motion_timing(&q,&age)==ESP_OK&&!age.flags&&!age.source_age_ms&&age.capture_epoch==data.capture_epoch);
}
static void analyzer_actual_lifecycle(void)
{
    dual_attached=true;dual_mode=CERBERUS_DUAL_BLE_ANALYZER;dual_version=2;dual_error=0;
    dual_radio_active=false;scan_status.state=CERBERUS_BLE_SCAN_IDLE;
    clock_us=100000;CHECK(start_scan(true)==ESP_OK);
    cerberus_dual_ble_analyzer_sample_t sample;
    cerberus_ble_analyzer_counter_sample(&dual_analyzer,clock_us,&sample);CHECK(!sample.flags);
    clock_us=500000;start_scan_event(&start_event);
    CHECK(dual_radio_active&&dual_analyzer.started_us==500000&&dual_analyzer.accepted);
    memset(&report,0,sizeof(report));report.type=BLE_GAP_EVENT_DISC;
    report.disc=(struct ble_gap_disc_desc){.addr={.type=1,.val={1,2,3,4,5,6}},.rssi=-44,
        .event_type=3,.length_data=31,.data=advertisement};
    unsigned parses=parse_calls;
    clock_us=501000;receive();
    CHECK(dual_analyzer.reports==1&&parse_calls==parses&&!scan_status.stored_count&&
        !watch_capture.status.reports&&!activity_capture.status.reports);
    cerberus_ble_analyzer_counter_sample(&dual_analyzer,clock_us,&sample);
    CHECK(sample.flags==17&&sample.elapsed_ms==1&&sample.reports==1);
    void *old=rf_arg;clock_us=8500000;complete();
    CHECK(dual_analyzer.finished&&!dual_radio_active);
    cerberus_ble_analyzer_counter_sample(&dual_analyzer,9000000,&sample);
    CHECK(sample.flags==25&&sample.elapsed_ms==8000);
    /* Actual admission and SDK-event path reset the new sweep; stale callback
     * and stale queued start cannot arm or increment the replacement. */
    clock_us=10000000;CHECK(start_scan(true)==ESP_OK);struct ble_npl_event stale={.arg=old};
    start_scan_event(&stale);CHECK(!dual_analyzer.armed);
    CHECK(gap_event(&report,old)==0&&!dual_analyzer.reports);start_scan_event(&start_event);
    clock_us+=1000;receive();CHECK(dual_analyzer.reports==1);scan_cancelled=true;finish_scan_for_epoch(0,scan_callback_epoch);
    CHECK(dual_analyzer.finished&&scan_status.state==CERBERUS_BLE_SCAN_IDLE);
    for(unsigned fault=0;fault<3;++fault){
        dual_radio_active=false;dual_error=0;clock_us+=100000;
        CHECK(start_scan(true)==ESP_OK);
        infer_error=fault==0?9:0;rf_error=fault==1?8:0;callout_error=fault==2?7:0;
        start_scan_event(&start_event);
        CHECK(scan_status.state==CERBERUS_BLE_SCAN_ERROR&&dual_error&&!dual_analyzer.armed);
        cerberus_ble_analyzer_counter_sample(&dual_analyzer,clock_us,&sample);
        CHECK(!sample.flags&&!sample.reports&&!sample.elapsed_ms&&sample.rssi==127);
        infer_error=rf_error=callout_error=0;
    }
    dual_radio_active=false;dual_error=0;scan_status.state=CERBERUS_BLE_SCAN_IDLE;
    scan_callback_epoch=UINT32_MAX;unsigned queued_before=queued;
    CHECK(start_scan(true)==ESP_ERR_INVALID_STATE&&queued==queued_before);
    /* All receive modes fail closed before a callback token is reused. */
    dual_attached=false;CHECK(cerberus_ble_scanner_start()==ESP_ERR_INVALID_STATE);
    CHECK(scan_callback_epoch==UINT32_MAX&&queued==queued_before);
    dual_attached=true;CHECK(start_scan(true)==ESP_ERR_INVALID_STATE);
    dual_attached=false;dual_radio_active=false;
}

static void family_actual_lifecycle(void)
{
    dual_attached=true;dual_mode=CERBERUS_DUAL_BLE_FAMILY;dual_family=6;dual_version=2;
    scan_callback_epoch=100;
    dual_radio_active=false;dual_error=0;scan_status.state=CERBERUS_BLE_SCAN_IDLE;
    memset(scan_family,0xa5,sizeof(scan_family));clock_us=10000000;
    CHECK(start_scan(true)==ESP_OK&&scan_inventory.enabled);
    for(size_t i=0;i<sizeof(scan_family);++i)CHECK(!((uint8_t*)scan_family)[i]);
    start_scan_event(&start_event);CHECK(dual_radio_active);
    complete();CHECK(!dual_radio_active);
    for(unsigned fault=0;fault<3;++fault){
        dual_error=0;clock_us+=100000;CHECK(start_scan(true)==ESP_OK);
        infer_error=fault==0?9:0;rf_error=fault==1?8:0;callout_error=fault==2?7:0;
        start_scan_event(&start_event);CHECK(scan_status.state==CERBERUS_BLE_SCAN_ERROR&&dual_error&&!scan_status.stored_count);
        infer_error=rf_error=callout_error=0;dual_radio_active=false;
    }
    scan_status.state=CERBERUS_BLE_SCAN_IDLE;dual_error=0;unsigned queued_before=queued;
    for(unsigned exhausted=0;exhausted<2;++exhausted){
        scan_callback_epoch=UINT32_MAX;
        CHECK(start_scan(true)==ESP_ERR_INVALID_STATE&&queued==queued_before&&!dual_radio_active);
    }
    dual_attached=false;
}
static void ordinary_dispatch_once(void)
{
    /* Ordinary Watch has no companion owner. Exercise the real registered SDK
     * callback and assert one response-mode lock plus its existing two locks. */
    CHECK(!dual_attached&&!dual_response);clock_us=19000000;start();
    unsigned before=mutex_takes;receive();CHECK(mutex_takes==before+3U);
    report.disc.event_type=4;before=mutex_takes;receive();CHECK(mutex_takes==before+1U);
    report.disc.event_type=3;report.disc.data=NULL;
    before=mutex_takes;receive();CHECK(mutex_takes==before+1U);
#if TEST_EXTENDED
    report.type=BLE_GAP_EVENT_EXT_DISC;
    report.ext_disc=(struct ble_gap_ext_disc_desc){.addr={.type=1,.val={2,4,6,8,10,12}},
        .rssi=-40,.sid=7,.prim_phy=1,.sec_phy=1,.length_data=31,.data=advertisement};
    before=mutex_takes;receive();CHECK(mutex_takes==before+3U);
    report.ext_disc.props=BLE_HCI_ADV_SCAN_RSP_MASK;
    before=mutex_takes;receive();CHECK(mutex_takes==before+1U);
    report.ext_disc.props=0;report.ext_disc.data_status=1;
    before=mutex_takes;receive();CHECK(mutex_takes==before+3U);
    report.ext_disc.data_status=0;report.ext_disc.data=NULL;
    before=mutex_takes;receive();CHECK(mutex_takes==before+1U);
#endif
    CHECK(!dual_attached&&!dual_response);complete();
}

static void response_actual_lifecycle(void)
{
    for(unsigned mode=4;mode<=5;++mode){
        dual_attached=true;dual_mode=(uint8_t)mode;dual_family=mode==5?6:0;dual_version=2;
        scan_callback_epoch=100;dual_radio_active=false;dual_error=0;
        scan_status.state=CERBERUS_BLE_SCAN_IDLE;
        CHECK(start_scan(true)==ESP_ERR_INVALID_STATE); /* No allocation, no active RF. */
        dual_response=calloc(1,sizeof(*dual_response));CHECK(dual_response);clock_us=20000000;
        CHECK(start_scan(true)==ESP_OK&&scan_inventory.enabled);start_scan_event(&start_event);
        CHECK(dual_radio_active);complete();CHECK(!dual_radio_active&&dual_response);
        for(unsigned fault=0;fault<3;++fault){
            if(!dual_response)dual_response=calloc(1,sizeof(*dual_response));CHECK(dual_response);
            dual_error=0;clock_us+=100000;CHECK(start_scan(true)==ESP_OK);
            infer_error=fault==0?9:0;rf_error=fault==1?8:0;callout_error=fault==2?7:0;
            start_scan_event(&start_event);CHECK(scan_status.state==CERBERUS_BLE_SCAN_ERROR&&dual_error&&!dual_radio_active&&!dual_response);
            infer_error=rf_error=callout_error=0;
        }
        dual_response=calloc(1,sizeof(*dual_response));CHECK(dual_response);
        scan_status.state=CERBERUS_BLE_SCAN_IDLE;scan_callback_epoch=UINT32_MAX;unsigned before=queued;
        CHECK(start_scan(true)==ESP_ERR_INVALID_STATE&&queued==before&&!dual_radio_active);
        dual_attached=false;response_free_locked();CHECK(!dual_response);
    }
}
static void reacquire_actual_lifecycle(void)
{
    dual_attached=true;dual_mode=CERBERUS_DUAL_BLE_REACQUIRE;dual_family=0;dual_version=2;
    memcpy(dual_address,(uint8_t[]){1,2,3,4,5,6},6);dual_address_type=CERBERUS_DUAL_BLE_ANY_OBSERVED_TYPE;
    scan_callback_epoch=100;dual_radio_active=false;dual_error=0;
    scan_status.state=CERBERUS_BLE_SCAN_IDLE;clock_us=30000000;
    CHECK(!dual_response && start_scan(true)==ESP_OK && scan_inventory.enabled);
    start_scan_event(&start_event);CHECK(dual_radio_active && !dual_response);
    /* The actual SDK stub above verifies passive 1M/coded parameters; the
     * actual callback excludes unrelated traffic before parser/accounting. */
    uint8_t data[]={2,1,6};
    struct ble_gap_event unrelated={.type=BLE_GAP_EVENT_DISC,.disc={
        .addr={.type=1,.val={9,2,3,4,5,6}},.rssi=-40,.event_type=0,.length_data=3,.data=data}};
    unsigned parsed_before=parse_calls;
    CHECK(gap_event(&unrelated,rf_arg)==0 && parse_calls==parsed_before && !scan_status.stored_count);
    complete();CHECK(!dual_radio_active && !dual_response);
    dual_attached=false;
}

int main(void)
{
    entry_clock_and_qualification();stale_delivery_and_saturation();extended_admission();motion_wire();ordinary_dispatch_once();analyzer_actual_lifecycle();family_actual_lifecycle();response_actual_lifecycle();reacquire_actual_lifecycle();
    CHECK(!locked);printf("C5 BLE timing (%s): %u checks; actual passive start/GAP/getters, atomic motion and own Location age, partial/response/malformed exclusion, stale callback/completion, zero/negative/full-width time, replacement and epoch saturation passed.\n",TEST_EXTENDED?"extended":"legacy",checks);
    return 0;
}
