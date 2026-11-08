#include "ap_survey_foreign_boundary.h"
#include "wardrive_foreign_boundary.h"
/* Actual C5 lifecycle/companion commands; SDK boundary follows installed NimBLE semantics. */
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
#include <stdatomic.h>
static atomic_bool packet_count_radio_reserved;
#include "ble_response.h"

static unsigned checks;
#define CHECK(x) do { ++checks; assert(x); } while (0)
#define MYNEWT_VAL(x) TEST_EXTENDED
#define portMAX_DELAY 0
#define BLE_SCAN_DURATION_MS 8000U
#define BLE_HS_FOREVER 0
#define BLE_NPL_OK 0
#define BLE_HS_EOS 3
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(tag,format,...) do { if(false) printf(format,__VA_ARGS__); } while(0)
#include "esp_err.h"
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
struct ble_npl_event { void *arg; bool queued; void (*fn)(struct ble_npl_event*); };
struct ble_npl_callout { struct ble_npl_event ev; bool armed; };
struct ble_gap_disc_params { unsigned itvl,window,filter_policy,limited,passive,filter_duplicates; };
struct ble_gap_ext_disc_params { unsigned itvl,window,passive; };

#include "c5_ble_watch_record_type.h"
#include "../../c5_firmware/main/ble_response_state.h"
static int mutex;
static void *scanner_mutex=&mutex;
static bool locked,host_synced=true,scan_cancelled;
static struct ble_npl_event start_event, stop_event, connect_event;
static struct ble_npl_callout scan_timeout_callout;
static cerberus_ble_scan_status_payload_t scan_status;
static cerberus_ble_connection_status_payload_t connection_status;
static cerberus_ble_record_t scan_results[CERBERUS_BLE_SCAN_MAX_RESULTS];
static cerberus_watch_capture_t watch_capture;
static cerberus_ble_activity_capture_t activity_capture;
static uint64_t watch_capture_epoch;
static uint32_t scan_callback_epoch;
#include "c5_ble_dual_state.h"
static uint8_t diagnostic_phase;
static int64_t clock_us,parse_delay;
static unsigned parse_epoch_change,queued,rf_starts,timeout_stops,parse_calls,mutex_takes;

static int infer_error,rf_error,callout_error,cancel_error,preempt_error;
static void (*sdk_hook)(void);
static bool sdk_receiving;
static unsigned cancel_calls, queue_count;
static struct ble_npl_event *queue[16];
enum { BLE_HS_EALREADY = 2 };
static void run_one(void) {
    CHECK(queue_count); struct ble_npl_event *e=queue[0];
    memmove(queue,queue+1,(--queue_count)*sizeof(*queue));
    e->queued=false; e->fn(e);
}

static void *rf_arg;
static int (*rf_callback)(struct ble_gap_event *,void *);
static int64_t esp_timer_get_time(void){return clock_us;}
static int xSemaphoreTake(void *m,unsigned ticks){(void)ticks;++mutex_takes;CHECK(m==scanner_mutex&&!locked);locked=true;return 1;}
static void xSemaphoreGive(void *m){CHECK(m==scanner_mutex&&locked);locked=false;}
static int ble_hs_adv_parse_fields(struct ble_hs_adv_fields *f,const uint8_t *p,unsigned n)
{(void)f;(void)p;(void)n;++parse_calls;clock_us+=parse_delay;if(parse_epoch_change){++scan_callback_epoch;parse_epoch_change=0;}return 0;}
static int find_record(const ble_addr_t *a){(void)a;return -1;}
static void copy_advertisement_fields(cerberus_ble_record_t *r,const struct ble_hs_adv_fields *f){(void)r;(void)f;CHECK(false);}
static void copy_raw_advertisement(cerberus_ble_record_t *r,const struct ble_gap_disc_desc *d,bool p){(void)r;(void)d;(void)p;CHECK(false);}
static void ble_npl_callout_stop(struct ble_npl_callout *c){CHECK(c==&scan_timeout_callout);++timeout_stops;c->armed=false; /* ESP_TIMER stop leaves queued event. */}
static void *ble_npl_event_get_arg(struct ble_npl_event *e){return e->arg;}
static void ble_npl_event_set_arg(struct ble_npl_event *e,void *arg){e->arg=arg;}
static void *nimble_port_get_dflt_eventq(void){return &mutex;}

static void ble_npl_eventq_put(void *q,struct ble_npl_event *e){
    CHECK(q==&mutex); if(e->queued)return; CHECK(queue_count<16);
    e->queued=true; queue[queue_count++]=e; ++queued;
}
static int ble_hs_id_infer_auto(int privacy,uint8_t *type){CHECK(!privacy);*type=0;return infer_error;}

static int ble_gap_disc_cancel(void){
    ++cancel_calls; if(sdk_hook){void (*hook)(void)=sdk_hook;sdk_hook=NULL;hook();}
    if(cancel_error)return cancel_error; if(!sdk_receiving)return BLE_HS_EALREADY;
    sdk_receiving=false; return 0; /* Successful cancel emits no DISC_COMPLETE. */
}
static int ble_gap_disc_active(void){return sdk_receiving;}
static unsigned ble_npl_time_ms_to_ticks32(unsigned ms){return ms;}
static int ble_npl_callout_reset(struct ble_npl_callout *c,unsigned ticks){CHECK(c==&scan_timeout_callout&&ticks==8000);c->armed=callout_error==0;return callout_error;}
static bool cerberus_c5_test_running(void){return false;}
static bool another_radio_operation_is_active(void){return false;}
static esp_err_t cerberus_wifi_channel_survey_preempt(void){return preempt_error;}
#if TEST_EXTENDED
static int ble_gap_ext_disc(uint8_t type,unsigned a,unsigned b,unsigned c,unsigned d,unsigned e,
    const struct ble_gap_ext_disc_params *one,const struct ble_gap_ext_disc_params *coded,
    int (*cb)(struct ble_gap_event *,void *),void *arg)
{CHECK(!type&&!a&&!b&&!c&&!d&&!e&&coded->passive==1&&one->passive==(cerberus_dual_ble_response_mode(dual_mode)&&dual_response?0U:1U)&&one->itvl==160&&one->window==80);
 ++rf_starts;rf_callback=cb;rf_arg=arg;if(!rf_error)sdk_receiving=true;if(sdk_hook){void (*hook)(void)=sdk_hook;sdk_hook=NULL;hook();}return rf_error;}
#else
static int ble_gap_disc(uint8_t type,int forever,const struct ble_gap_disc_params *p,
    int (*cb)(struct ble_gap_event *,void *),void *arg)
{CHECK(!type&&!forever&&p->passive==(cerberus_dual_ble_response_mode(dual_mode)&&dual_response?0U:1U)&&!p->filter_duplicates&&p->itvl==(p->passive?0U:160U)&&p->window==(p->passive?0U:80U));
 ++rf_starts;rf_callback=cb;rf_arg=arg;if(!rf_error)sdk_receiving=true;if(sdk_hook){void (*hook)(void)=sdk_hook;sdk_hook=NULL;hook();}return rf_error;}
#endif
#include "../../c5_firmware/main/ble_response_impl.h"

#define pdTRUE 1
#define ESP_LOGE(...) ((void)0)
#define BLE_CONNECTION_HANDLE_NONE 0xffffU
#define BLE_ADDR_RANDOM_ID 3
static bool disconnect_requested;
static cerberus_ble_gatt_service_result_payload_t gatt_services[CERBERUS_BLE_GATT_SERVICE_MAX_RESULTS];
static cerberus_ble_gatt_characteristic_result_payload_t gatt_characteristics[CERBERUS_BLE_GATT_CHARACTERISTIC_MAX_RESULTS];
static cerberus_ble_gatt_service_status_payload_t gatt_service_status;
static cerberus_ble_gatt_characteristic_status_payload_t gatt_characteristic_status;
static int ble_hs_util_ensure_addr(int privacy){CHECK(!privacy);return 0;}
static void reset_gatt_read_locked(uint8_t state,int32_t error){(void)state;(void)error;CHECK(locked);}
#include "actual_lifecycle.h"
#include "dual_ble_service_impl.h"
/* Separate passive Probe owner; the actual admission body below checks it. */
static atomic_bool probe_sniff_radio_reserved;
static atomic_bool mac_radio_reserved,csi_radio_reserved,remote_id_radio_reserved,
    capture_radio_reserved,packet_monitor_radio_reserved;
static bool cerberus_ieee802154_scanner_is_running(void){return false;}
static void cerberus_ble_connection_get_status(cerberus_ble_connection_status_payload_t *out){*out=connection_status;}
#include "actual_wifi_admission.h"

static void command(uint8_t cmd,const void *request,size_t n,void *reply,size_t expected)
{
    size_t size=0;CHECK(cerberus_ble_dual_command(cmd,request,n,reply,&size));
    CHECK(size==expected&&!locked);
}
static cerberus_dual_ble_binding_t new_job(uint8_t mode)
{
    cerberus_dual_ble_start_t r={.boot_id=dual_boot_id,.job_id=dual_job_highwater+1U,.version=2,.mode=mode,
        .reserved=mode==CERBERUS_DUAL_BLE_FAMILY?2:0};
    cerberus_dual_ble_status_t s;
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_START,&r,sizeof(r),&s,sizeof(s));
    CHECK(!s.error_code&&s.flags&CERBERUS_DUAL_BLE_OWNED);
    return (cerberus_dual_ble_binding_t){r.boot_id,r.job_id};
}
static cerberus_dual_ble_status_t binding_command(uint8_t command_id,cerberus_dual_ble_binding_t b)
{
    cerberus_dual_ble_status_t s;
    command(command_id,&b,sizeof(b),&s,sizeof(s));return s;
}
static void release_job(cerberus_dual_ble_binding_t b)
{
    cerberus_dual_ble_status_t s=binding_command(CERBERUS_SPI_COMMAND_DUAL_BLE_RELEASE,b);
    CHECK(!s.error_code&&!s.flags&&!dual_attached);
}
static void reset_case(void)
{
    CHECK(!locked); if(dual_response){free(dual_response);dual_response=NULL;}
    memset(&scan_status,0,sizeof(scan_status));memset(&connection_status,0,sizeof(connection_status));
    memset(&dual_snapshot,0,sizeof(dual_snapshot));
    memset(&watch_capture,0,sizeof(watch_capture));memset(&activity_capture,0,sizeof(activity_capture));
    memset(&dual_analyzer,0,sizeof(dual_analyzer));
    start_event=(struct ble_npl_event){.fn=start_scan_event};
    stop_event=(struct ble_npl_event){.fn=stop_scan_event};
    scan_timeout_callout=(struct ble_npl_callout){.ev={.fn=scan_timeout_event}};
    dual_boot_id=0x123456789abcdef0ULL; dual_epoch_counter=dual_epoch=0;
    dual_job_id=dual_job_highwater=dual_last_contact_ms=0;
    dual_attached=dual_radio_active=dual_track_seen=false;dual_error=0;
    scan_callback_epoch=0;scan_cancelled=false;
    host_synced=true;sdk_receiving=false;queue_count=0;queued=rf_starts=cancel_calls=0;
    callout_error=rf_error=infer_error=cancel_error=preempt_error=0;clock_us=1000000;
    scan_start_pending_epoch=scan_stop_pending_epoch=scan_sdk_epoch=0;scan_deadline_us=0;scan_epoch_retired=false;sdk_hook=NULL;
    memset(&connect_event,0,sizeof(connect_event));
}
static void queue_timeout(void)
{
    CHECK(scan_timeout_callout.armed);scan_timeout_callout.armed=false;
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(),&scan_timeout_callout.ev);
}
static void sdk_completion(void)
{
    CHECK(sdk_receiving);sdk_receiving=false;
    struct ble_gap_event done={.type=BLE_GAP_EVENT_DISC_COMPLETE};
    CHECK(!rf_callback(&done,rf_arg));
}

static cerberus_dual_ble_status_t try_next_job(void)
{
    cerberus_dual_ble_start_t r={.boot_id=dual_boot_id,.job_id=dual_job_highwater+1U,.version=2};
    cerberus_dual_ble_status_t s;
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_START,&r,sizeof(r),&s,sizeof(s));return s;
}
static void check_busy(void)
{
    cerberus_ble_scan_status_payload_t status;
    cerberus_ble_scanner_get_status(&status);CHECK(status.state==CERBERUS_BLE_SCAN_RUNNING);
    CHECK(wifi_another_radio_operation_is_active());
    CHECK(cerberus_ble_scanner_start()==ESP_ERR_INVALID_STATE);
    cerberus_ble_connect_request_t request={.address_type=1};
    CHECK(cerberus_ble_connection_start(&request)==ESP_ERR_INVALID_STATE);
}
static void check_blocked_release(cerberus_dual_ble_binding_t b)
{
    cerberus_dual_ble_status_t s=binding_command(CERBERUS_SPI_COMMAND_DUAL_BLE_RELEASE,b);
    CHECK(s.error_code==ESP_ERR_INVALID_STATE&&(s.flags&CERBERUS_DUAL_BLE_OWNED));
    CHECK(try_next_job().error_code==ESP_ERR_INVALID_STATE);check_busy();
}
static void stale_timeout_preserves_replacement(void)
{
    reset_case();cerberus_dual_ble_binding_t a=new_job(CERBERUS_DUAL_BLE_DISCOVERY);run_one();
    (void)binding_command(CERBERUS_SPI_COMMAND_DUAL_BLE_STOP,a);
    queue_timeout();CHECK(queue_count==2&&queue[0]==&stop_event);
    run_one();CHECK(!sdk_receiving&&!dual_radio_active&&scan_status.state==CERBERUS_BLE_SCAN_IDLE);
    release_job(a);cerberus_dual_ble_binding_t b=new_job(CERBERUS_DUAL_BLE_DISCOVERY);
    unsigned cancels=cancel_calls;run_one(); /* Old wake before B's SDK Start. */
    CHECK(cancel_calls==cancels&&scan_status.state==CERBERUS_BLE_SCAN_RUNNING&&!dual_error);
    run_one();CHECK(rf_starts==2&&sdk_receiving&&scan_sdk_epoch==scan_callback_epoch);
    int64_t deadline=scan_deadline_us;
    for(unsigned early=0;early<3;++early){
        clock_us=early==0?-1:early==1?100:deadline-1;
        scan_timeout_event(&scan_timeout_callout.ev);
        CHECK(cancel_calls==cancels&&sdk_receiving&&scan_timeout_callout.armed&&scan_deadline_us==deadline);
    }
    clock_us=deadline;scan_timeout_event(&scan_timeout_callout.ev);
    CHECK(cancel_calls==cancels+1&&!sdk_receiving&&!dual_radio_active&&scan_status.state==CERBERUS_BLE_SCAN_COMPLETE);
    release_job(b);
}
static void pending_stop_blocks_replacement(bool reset)
{
    reset_case();cerberus_dual_ble_binding_t a=new_job(CERBERUS_DUAL_BLE_DISCOVERY);run_one();
    uint32_t old_epoch=scan_callback_epoch;
    (void)binding_command(CERBERUS_SPI_COMMAND_DUAL_BLE_STOP,a);
    sdk_completion();if(reset){host_reset(77);host_sync();}
    check_blocked_release(a);
    clock_us+=((int64_t)CERBERUS_DUAL_BLE_ORPHAN_MS+1)*1000;
    (void)binding_command(CERBERUS_SPI_COMMAND_DUAL_BLE_STATUS,a);
    CHECK(dual_attached); /* A pending control cannot be orphan-expired. */
    unsigned cancels=cancel_calls;run_one();CHECK(cancel_calls==cancels&&!scan_stop_pending_epoch);
    release_job(a);cerberus_dual_ble_binding_t b=new_job(CERBERUS_DUAL_BLE_DISCOVERY);run_one();
    struct ble_npl_event stale={.arg=(void*)(uintptr_t)old_epoch};
    stop_scan_event(&stale);start_scan_event(&stale);
    CHECK(cancel_calls==cancels&&rf_starts==2&&sdk_receiving&&!dual_error);
    struct ble_gap_event done={.type=BLE_GAP_EVENT_DISC_COMPLETE};
    CHECK(!gap_event(&done,(void*)(uintptr_t)old_epoch)&&sdk_receiving&&dual_radio_active);
    (void)binding_command(CERBERUS_SPI_COMMAND_DUAL_BLE_STOP,b);run_one();release_job(b);
}
static void reset_pending_start_and_duplicate_stop(void)
{
    reset_case();cerberus_dual_ble_binding_t a=new_job(CERBERUS_DUAL_BLE_RESPONSE_DISCOVERY);
    void *original_arg=start_event.arg;c5_response_context_t *context=dual_response;
    host_reset(77);host_sync();check_blocked_release(a);
    CHECK(context==dual_response&&start_event.arg==original_arg&&queue_count==1);
    run_one();CHECK(!rf_starts&&!scan_start_pending_epoch);release_job(a);CHECK(!dual_response);
    cerberus_dual_ble_binding_t b=new_job(CERBERUS_DUAL_BLE_DISCOVERY);
    for(unsigned i=0;i<4;++i)(void)binding_command(CERBERUS_SPI_COMMAND_DUAL_BLE_STOP,b);
    CHECK(queue_count==2&&scan_stop_pending_epoch==scan_callback_epoch);
    run_one();CHECK(!rf_starts&&!sdk_receiving&&scan_status.state==CERBERUS_BLE_SCAN_IDLE);
    check_blocked_release(b);run_one();CHECK(!cancel_calls);release_job(b);
}
static void timer_arm_failure_all_modes(void)
{
    const uint8_t modes[]={CERBERUS_DUAL_BLE_DISCOVERY,CERBERUS_DUAL_BLE_TRACK,
        CERBERUS_DUAL_BLE_ANALYZER,CERBERUS_DUAL_BLE_FAMILY,CERBERUS_DUAL_BLE_RESPONSE_DISCOVERY};
    for(unsigned i=0;i<sizeof(modes);++i){
        reset_case();callout_error=1;cerberus_dual_ble_binding_t b=new_job(modes[i]);run_one();
        CHECK(!sdk_receiving&&!dual_radio_active&&!scan_sdk_epoch&&!scan_start_pending_epoch);
        CHECK(scan_status.state==CERBERUS_BLE_SCAN_ERROR&&dual_error==BLE_HS_EOS);
        release_job(b);CHECK(!dual_response);
    }
    for(unsigned bad=0;bad<2;++bad){
        reset_case();clock_us=bad?INT64_MAX-1:-1;
        cerberus_dual_ble_binding_t b=new_job(CERBERUS_DUAL_BLE_DISCOVERY);run_one();
        CHECK(!sdk_receiving&&!dual_radio_active&&scan_status.state==CERBERUS_BLE_SCAN_ERROR);release_job(b);
    }
}
static void failed_cancel_retains_then_releases(bool companion,bool timeout)
{
    reset_case();cerberus_dual_ble_binding_t b={0};
    if(companion)b=new_job(CERBERUS_DUAL_BLE_RESPONSE_DISCOVERY);
    else CHECK(cerberus_ble_scanner_start()==ESP_OK);
    run_one();cancel_error=17;
    if(timeout){clock_us=scan_deadline_us;scan_timeout_event(&scan_timeout_callout.ev);}
    else {CHECK(stop_scan(companion)==ESP_OK);run_one();}
    CHECK(sdk_receiving&&dual_radio_active&&scan_sdk_epoch==scan_callback_epoch&&scan_status.state==CERBERUS_BLE_SCAN_ERROR);
    if(companion){CHECK(dual_response);check_blocked_release(b);}else check_busy();
    cancel_error=BLE_HS_EALREADY;CHECK(stop_scan(companion)==ESP_OK);run_one();
    CHECK(sdk_receiving&&dual_radio_active); /* EALREADY alone is not quiescence. */
    cancel_error=0;CHECK(stop_scan(companion)==ESP_OK);run_one();
    CHECK(!sdk_receiving&&!dual_radio_active&&!scan_radio_owned_locked());
    CHECK(!wifi_another_radio_operation_is_active());
    if(companion){CHECK(!dual_response);release_job(b);}
    CHECK(cerberus_ble_scanner_start()==ESP_OK);run_one();sdk_completion();
}
static void already_inactive_and_callback_recovery(void)
{
    for(unsigned callback=0;callback<2;++callback){
        reset_case();cerberus_dual_ble_binding_t b=new_job(CERBERUS_DUAL_BLE_DISCOVERY);run_one();
        cancel_error=17;CHECK(stop_scan(true)==ESP_OK);run_one();
        if(callback)sdk_completion();
        else {sdk_receiving=false;cancel_error=BLE_HS_EALREADY;CHECK(stop_scan(true)==ESP_OK);run_one();}
        CHECK(!dual_radio_active&&!scan_sdk_epoch&&!scan_radio_owned_locked());release_job(b);
    }
}
static void failed_timer_cancel_retains_context(void)
{
    for(unsigned companion=0;companion<2;++companion){
        reset_case();callout_error=1;cancel_error=17;
        cerberus_dual_ble_binding_t b={0};
        if(companion)b=new_job(CERBERUS_DUAL_BLE_RESPONSE_DISCOVERY);
        else CHECK(cerberus_ble_scanner_start()==ESP_OK);
        run_one();CHECK(sdk_receiving&&dual_radio_active&&!scan_timeout_callout.armed);
        CHECK(scan_status.state==CERBERUS_BLE_SCAN_ERROR&&!scan_start_pending_epoch);check_busy();
        if(companion){CHECK(dual_response);check_blocked_release(b);}
        clock_us+=((int64_t)CERBERUS_DUAL_BLE_ORPHAN_MS+1)*1000;
        if(companion){(void)binding_command(CERBERUS_SPI_COMMAND_DUAL_BLE_STATUS,b);CHECK(dual_attached&&dual_response);}
        cancel_error=0;CHECK(stop_scan(companion!=0)==ESP_OK);run_one();
        CHECK(!sdk_receiving&&!scan_radio_owned_locked()&&!wifi_another_radio_operation_is_active());
        if(companion){CHECK(!dual_response);release_job(b);}
    }
}
static cerberus_dual_ble_binding_t hook_binding;
static void during_sdk_call(void)
{
    check_blocked_release(hook_binding);
    CHECK(scan_start_pending_epoch||scan_stop_pending_epoch);
}
static void reset_during_sdk_call(void)
{
    during_sdk_call();sdk_receiving=false;host_reset(77);host_sync();
    check_blocked_release(hook_binding);
}
static void complete_during_sdk_call(void)
{ during_sdk_call();sdk_completion();check_blocked_release(hook_binding); }
static void stop_during_timeout_dispatch(void)
{
    CHECK(scan_stop_pending_epoch==scan_callback_epoch&&!queue_count);
    CHECK(stop_scan(true)==ESP_OK&&stop_scan(true)==ESP_OK&&!queue_count);
    check_blocked_release(hook_binding);
}
static void dispatch_reservation_and_reset(void)
{
    for(unsigned reset=0;reset<2;++reset){
        reset_case();hook_binding=new_job(CERBERUS_DUAL_BLE_DISCOVERY);
        sdk_hook=reset?reset_during_sdk_call:during_sdk_call;run_one();
        if(reset){CHECK(!sdk_receiving&&!dual_radio_active&&!scan_sdk_epoch);release_job(hook_binding);}
        else {
            CHECK(sdk_receiving);sdk_hook=during_sdk_call;
            CHECK(stop_scan(true)==ESP_OK);run_one();release_job(hook_binding);
        }
    }
    reset_case();hook_binding=new_job(CERBERUS_DUAL_BLE_DISCOVERY);sdk_hook=complete_during_sdk_call;run_one();
    CHECK(!sdk_receiving&&!dual_radio_active&&!scan_timeout_callout.armed&&scan_status.state==CERBERUS_BLE_SCAN_COMPLETE);
    release_job(hook_binding);
    reset_case();hook_binding=new_job(CERBERUS_DUAL_BLE_DISCOVERY);run_one();
    sdk_hook=reset_during_sdk_call;CHECK(stop_scan(true)==ESP_OK);run_one();
    CHECK(!sdk_receiving&&!dual_radio_active&&!scan_radio_owned_locked());release_job(hook_binding);
    reset_case();hook_binding=new_job(CERBERUS_DUAL_BLE_DISCOVERY);run_one();
    clock_us=scan_deadline_us;sdk_hook=stop_during_timeout_dispatch;scan_timeout_event(&scan_timeout_callout.ev);
    CHECK(scan_status.state==CERBERUS_BLE_SCAN_IDLE&&!sdk_receiving&&!scan_stop_pending_epoch);
    release_job(hook_binding);
}
static void failure_admission_and_exhaustion(void)
{
    reset_case();preempt_error=ESP_ERR_TIMEOUT;
    CHECK(cerberus_ble_scanner_start()==ESP_ERR_TIMEOUT&&!queue_count&&!scan_start_pending_epoch);
    cerberus_ble_connect_request_t request={.address_type=1};
    CHECK(cerberus_ble_connection_start(&request)==ESP_ERR_TIMEOUT&&!queue_count);
    cerberus_dual_ble_start_t response_request={.boot_id=dual_boot_id,.job_id=1,.version=2,
        .mode=CERBERUS_DUAL_BLE_RESPONSE_DISCOVERY};
    cerberus_dual_ble_status_t response_status;
    command(CERBERUS_SPI_COMMAND_DUAL_BLE_START,&response_request,sizeof(response_request),
        &response_status,sizeof(response_status));
    CHECK(response_status.error_code==ESP_ERR_TIMEOUT&&!dual_attached&&!dual_response&&!queue_count);
    preempt_error=0;scan_callback_epoch=UINT32_MAX;
    CHECK(cerberus_ble_scanner_start()==ESP_ERR_INVALID_STATE&&!queue_count&&scan_callback_epoch==UINT32_MAX);
    reset_case();scan_callback_epoch=UINT32_MAX-1;
    CHECK(cerberus_ble_scanner_start()==ESP_OK);run_one();CHECK(scan_sdk_epoch==UINT32_MAX);
    sdk_completion();CHECK(cerberus_ble_scanner_start()==ESP_ERR_INVALID_STATE&&!queue_count&&scan_callback_epoch==UINT32_MAX);
    reset_case();infer_error=9;cerberus_dual_ble_binding_t a=new_job(CERBERUS_DUAL_BLE_DISCOVERY);run_one();
    CHECK(!rf_starts&&!scan_radio_owned_locked()&&scan_status.error_code==9);release_job(a);
    reset_case();rf_error=9;a=new_job(CERBERUS_DUAL_BLE_DISCOVERY);run_one();
    CHECK(!sdk_receiving&&!scan_radio_owned_locked()&&scan_status.error_code==9);release_job(a);
}
int main(void)
{
    reset_case();atomic_store(&probe_sniff_radio_reserved,true);
    CHECK(wifi_another_radio_operation_is_active());
    atomic_store(&probe_sniff_radio_reserved,false);
    { atomic_store(&packet_count_radio_reserved,true);
    CHECK(wifi_another_radio_operation_is_active());
    atomic_store(&packet_count_radio_reserved,false); }

    stale_timeout_preserves_replacement();
    pending_stop_blocks_replacement(false);pending_stop_blocks_replacement(true);
    reset_pending_start_and_duplicate_stop();timer_arm_failure_all_modes();
    for(unsigned companion=0;companion<2;++companion)for(unsigned timeout=0;timeout<2;++timeout)
        failed_cancel_retains_then_releases(companion!=0,timeout!=0);
    already_inactive_and_callback_recovery();failed_timer_cancel_retains_context();dispatch_reservation_and_reset();
    failure_admission_and_exhaustion();
    printf("C5 actual receive lifecycle checks passed: %u assertions (extended=%d).\n",checks,TEST_EXTENDED);
    return 0;
}
