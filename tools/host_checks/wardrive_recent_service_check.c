#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wardrive_service.h"
#include "c5_spi_link.h"
#include "field_survey.h"
#include "field_survey_stream.h"
#include "s3_ble_host.h"
#include "s3_ble_scanner.h"
#include "s3_wifi_owner.h"
#include "wardrive_wigle.h"
#include "wardrive_gps_uart.h"
#include "wifi_capture_session.h"
#include "wardrive_compact.h"
#include "wardrive_recent_storage_decls.h"

static unsigned checks;
#define CHECK(x) do { ++checks; assert(x); } while (0)
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define MALLOC_CAP_SPIRAM 1U
#define MALLOC_CAP_8BIT 2U
#define MALLOC_CAP_INTERNAL 4U
#define pdPASS 1
#define pdMS_TO_TICKS(x) (x)
static void enter_lock(void);
static void exit_lock(void);
#define portENTER_CRITICAL(p) ((void)(p), enter_lock())
#define portEXIT_CRITICAL(p) ((void)(p), exit_lock())
static int64_t esp_timer_get_time(void);
static void *heap_caps_calloc(size_t, size_t, unsigned);
static void heap_caps_free(void *);
static size_t heap_caps_get_free_size(unsigned);
static void vTaskDelay(unsigned);
static void vTaskDelete(void *);
static int xTaskCreate(void (*)(void *), const char *, unsigned, void *, unsigned, void *);

/* The complete production source, with only its SDK include lines replaced. */
#include "wardrive_recent_service_impl.h"

static int64_t clock_us;
static unsigned lock_depth, allocations, frees, task_creates, sd_calls, sd_appends;
static unsigned rf_starts, status_polls, gps_calls, wifi_reads, ble_copies;
static unsigned gps_stop_calls, gps_stop_failures, gps_cleanup_checks;
static bool inspect_gps_cleanup;
static unsigned fail_alloc_at, stop_after_append, fail_append_at;
static unsigned wifi_start_calls;
static esp_err_t wifi_first_start_error;
static bool host_initialized_value,reset_during_claim,replace_during_claim;
static bool reset_before_release,replace_before_release;
static unsigned host_epoch_value,host_claims,host_releases;
static cerberus_s3_ble_owner_t host_owner_value;
static uint64_t host_next_token;
static cerberus_s3_ble_host_lease_t host_live_lease;
static bool fail_task, immediate_task, inspect_pending, inspect_free, auto_complete, enqueue_on_start;
static esp_err_t status_error, session_error, append_error, begin_error, copy_error;
static bool storage_open[2], storage_owned;
static size_t partial_bytes;
static void (*pending_task)(void *);
static cerberus_wifi_scan_status_payload_t wifi_status;
static cerberus_ble_scan_status_payload_t ble_status;
static cerberus_c5_wifi_result_t wifi_source[32];
static cerberus_c5_ble_result_t ble_source[32];
static cerberus_s3_ble_wardrive_evidence_t ble_evidence[32];
static field_survey_stream_t stream;
static field_survey_snapshot_t field_status;
static cerberus_wardrive_gps_snapshot_t gps_source;
/* Admission and later row snapshots are separate so lifecycle cases can
 * exercise a fix lost after starting without bypassing the Start gate. */
static cerberus_wardrive_gps_uart_state_t gps_admission;
esp_err_t cerberus_wardrive_gps_uart_get_state(cerberus_wardrive_gps_uart_state_t *out)
{ *out=gps_admission;return ESP_OK; }

#include "wardrive_passive_boundary.h"

static void enter_lock(void) { CHECK(lock_depth == 0U); ++lock_depth; }
static void exit_lock(void) { CHECK(lock_depth == 1U); --lock_depth; }
static int64_t esp_timer_get_time(void) { return clock_us; }
static void *heap_caps_calloc(size_t count, size_t size, unsigned caps)
{
    CHECK(!lock_depth && caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ++allocations;
    if (allocations == fail_alloc_at) return NULL;
    return calloc(count, size);
}
static void heap_caps_free(void *ptr)
{
    CHECK(!lock_depth);
    if (!ptr) return;
    if (inspect_free) {
        cerberus_wardrive_recent_snapshot_t view;
        cerberus_wardrive_copy_recent(&view);
        CHECK(!view.available && !view.count && wardrive_recent == NULL);
    }
    ++frees; free(ptr);
}
static size_t heap_caps_get_free_size(unsigned caps)
{ CHECK(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)); return 200000U; }
static void vTaskDelay(unsigned ms) { passive_delay(ms); }
static void vTaskDelete(void *p) { CHECK(p == NULL); }
static int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack,
    void *arg, unsigned priority, void *handle)
{
    CHECK(!lock_depth && !strcmp(name, "wardrive") && stack == 6144 &&
        arg == NULL && priority == 2 && handle == NULL);
    ++task_creates;
    if (fail_task) return 0;
    pending_task = fn;
    if (immediate_task) { stop_requested = true; fn(arg); pending_task = NULL; }
    return pdPASS;
}

esp_err_t c5_spi_link_get_status(cerberus_c5_status_t *out)
{ ++status_polls; *out = (cerberus_c5_status_t){.capabilities=CERBERUS_C5_CAP_WIFI_2_4_GHZ,.firmware_version=60}; return status_error; }
esp_err_t c5_spi_link_get_wifi_scan_status(cerberus_wifi_scan_status_payload_t *out)
{ ++status_polls; if(auto_complete&&wifi_status.state==CERBERUS_WIFI_SCAN_RUNNING){wifi_status.state=CERBERUS_WIFI_SCAN_COMPLETE;wifi_status.stored_count=2;} *out = wifi_status; return status_error; }
esp_err_t c5_spi_link_wifi_capture_status(cerberus_wifi_capture_status_payload_t *out)
{ ++status_polls; memset(out,0,sizeof(*out)); return ESP_OK; }
esp_err_t c5_spi_link_wifi_channel_survey_status(cerberus_wifi_channel_survey_status_payload_t *out)
{ ++status_polls; memset(out,0,sizeof(*out)); return ESP_OK; }
esp_err_t c5_spi_link_start_wifi_scan(cerberus_wifi_scan_status_payload_t *out)
{
    ++wifi_start_calls;
    esp_err_t error=wifi_start_calls==1U?wifi_first_start_error:ESP_OK;
    if(error!=ESP_ERR_TIMEOUT){++rf_starts;wifi_status.state=CERBERUS_WIFI_SCAN_RUNNING;++wifi_status.generation;}
    *out=wifi_status;return error;
}
esp_err_t c5_spi_link_stop_wifi_scan(cerberus_wifi_scan_status_payload_t *out)
{ wifi_status.state=CERBERUS_WIFI_SCAN_COMPLETE; *out=wifi_status; return ESP_OK; }
esp_err_t c5_spi_link_get_wifi_scan_result(uint8_t generation,uint8_t index,cerberus_c5_wifi_result_t *out)
{ CHECK(generation==wifi_status.generation && index<32); ++wifi_reads; *out=wifi_source[index]; return copy_error; }
esp_err_t c5_spi_link_copy_wifi_probe_results(uint8_t g,uint32_t d,
    cerberus_c5_wifi_probe_bank_t *bank,cerberus_wifi_probe_row_t *rows,size_t cap,
    cerberus_c5_probe_continue_fn fn,void *ctx)
{ (void)g;(void)d;(void)bank;(void)rows;(void)cap; CHECK(fn(ctx)); return ESP_ERR_NOT_SUPPORTED; }
esp_err_t c5_spi_link_copy_wifi_relations(uint8_t g,uint32_t d,
    cerberus_c5_wifi_relation_bank_t *bank,cerberus_wifi_relation_row_t *rows,size_t cap,
    cerberus_c5_relation_continue_fn fn,void *ctx)
{ (void)g;(void)d;(void)bank;(void)rows;(void)cap; CHECK(fn(ctx)); return ESP_ERR_NOT_SUPPORTED; }
esp_err_t c5_spi_link_check_wifi_probe_bank(const cerberus_c5_wifi_probe_bank_t *p,uint32_t d)
{ (void)p;(void)d; return ESP_OK; }
esp_err_t c5_spi_link_check_wifi_relation_bank(const cerberus_c5_wifi_relation_bank_t *p,uint32_t d)
{ (void)p;(void)d; return ESP_OK; }
cerberus_s3_ble_owner_t cerberus_s3_ble_host_owner(void) { return host_owner_value; }
bool cerberus_s3_ble_host_initialized(void) { return host_initialized_value; }
unsigned cerberus_s3_ble_host_epoch(void) { return host_epoch_value; }
esp_err_t cerberus_s3_ble_host_claim(cerberus_s3_ble_owner_t o,
    cerberus_s3_ble_host_lease_t *lease)
{
    ++host_claims;CHECK(o==CERBERUS_S3_BLE_OWNER_SCANNER && host_owner_value==CERBERUS_S3_BLE_OWNER_NONE);
    CHECK(lease!=NULL);memset(lease,0,sizeof(*lease));
    host_initialized_value=true;host_owner_value=o;
    host_live_lease=(cerberus_s3_ble_host_lease_t){.token=++host_next_token,.epoch=host_epoch_value,.owner=o};
    if(reset_during_claim || replace_during_claim){
        ++host_epoch_value;host_owner_value=CERBERUS_S3_BLE_OWNER_NONE;
        host_live_lease=(cerberus_s3_ble_host_lease_t){0};
        if(replace_during_claim){
            host_owner_value=CERBERUS_S3_BLE_OWNER_CENTRAL;
            host_live_lease=(cerberus_s3_ble_host_lease_t){.token=++host_next_token,.epoch=host_epoch_value,.owner=host_owner_value};
        }
        return ESP_ERR_INVALID_STATE;
    }
    *lease=host_live_lease;
    return ESP_OK;
}
bool cerberus_s3_ble_host_lease_valid(const cerberus_s3_ble_host_lease_t *lease)
{return lease&&lease->token&&lease->token==host_live_lease.token&&lease->epoch==host_live_lease.epoch&&lease->owner==host_live_lease.owner;}
esp_err_t cerberus_s3_ble_host_release(const cerberus_s3_ble_host_lease_t *lease)
{
    ++host_releases;
    if(reset_before_release){++host_epoch_value;host_owner_value=CERBERUS_S3_BLE_OWNER_NONE;host_live_lease=(cerberus_s3_ble_host_lease_t){0};}
    if(replace_before_release){
        host_owner_value=CERBERUS_S3_BLE_OWNER_SCANNER;
        host_live_lease=(cerberus_s3_ble_host_lease_t){.token=++host_next_token,.epoch=host_epoch_value,.owner=host_owner_value};
    }
    if(!cerberus_s3_ble_host_lease_valid(lease))return ESP_ERR_INVALID_STATE;
    host_owner_value=CERBERUS_S3_BLE_OWNER_NONE;host_live_lease=(cerberus_s3_ble_host_lease_t){0};return ESP_OK;
}
cerberus_s3_wifi_owner_t cerberus_s3_wifi_owner(void) { return CERBERUS_S3_WIFI_NONE; }
void cerberus_s3_capture_status(cerberus_s3_capture_status_t *out) { memset(out,0,sizeof(*out)); }
esp_err_t cerberus_s3_ble_scanner_get_status(cerberus_ble_scan_status_payload_t *out)
{ ++status_polls; if(auto_complete&&ble_status.state==CERBERUS_BLE_SCAN_RUNNING){ble_status.state=CERBERUS_BLE_SCAN_COMPLETE;ble_status.stored_count=2;} *out=ble_status; return ESP_OK; }
esp_err_t cerberus_s3_ble_scanner_start(cerberus_ble_scan_status_payload_t *out)
{ (void)out; CHECK(false && "generic BLE Start is forbidden in Wardriving"); return ESP_FAIL; }
esp_err_t cerberus_s3_ble_scanner_stop(cerberus_ble_scan_status_payload_t *out)
{ (void)out; CHECK(false && "generic BLE Stop is forbidden in Wardriving"); return ESP_FAIL; }
esp_err_t cerberus_s3_ble_scanner_copy_wardrive(uint8_t g,uint8_t n,
    cerberus_c5_ble_result_t *out,cerberus_s3_ble_wardrive_evidence_t *hints,size_t cap)
{ (void)g;(void)n;(void)out;(void)hints;(void)cap;
  CHECK(false && "generation-only BLE copy is forbidden in Wardriving"); return ESP_FAIL; }
esp_err_t field_survey_set_idle_probe_enabled(bool enabled) { (void)enabled; return ESP_OK; }
void field_survey_snapshot(field_survey_snapshot_t *out) { *out=field_status; }
esp_err_t field_survey_set_expansion_plan(const cerberus_wardrive_plan_t *p) { (void)p; return ESP_OK; }
esp_err_t field_survey_set_running(bool run)
{
    field_status.requested=run; field_status.worker_seen_epoch=++field_status.request_epoch;
    field_status.scanning_mask=run?field_status.present_mask:0;
    if(run&&enqueue_on_start){
        expansion_record_t row={.radio=1,.bssid={2,3,4,5,6,123},.channel=6,.rssi=-60};
        CHECK(field_survey_stream_push(&stream,&row));
    }
    return ESP_OK;
}
esp_err_t field_survey_stream_open(void) { memset(&stream,0,sizeof(stream)); return ESP_OK; }
void field_survey_stream_close(void) { memset(&stream,0,sizeof(stream)); }
size_t field_survey_stream_read(expansion_record_t *out,size_t cap,uint32_t *overflow,
    uint32_t *invalid,uint16_t *pending,uint32_t by_slot[6])
{ size_t n=field_survey_stream_pop(&stream,out,cap); *overflow=*invalid=0; *pending=stream.count; memset(by_slot,0,6*sizeof(*by_slot)); return n; }
esp_err_t cerberus_wardrive_gps_uart_start(void) { ++gps_calls; return ESP_OK; }
esp_err_t cerberus_wardrive_gps_uart_stop(void)
{
    ++gps_calls; ++gps_stop_calls;
    if (inspect_gps_cleanup) {
        CHECK(!lock_depth && worker_active && workspace);
        CHECK(wardrive_status.state==CERBERUS_WARDRIVE_STOPPING);
        CHECK(!storage_owned && !storage_open[0] && !storage_open[1]);
        CHECK(!passive_source.attached);
        CHECK(cerberus_wardrive_start(false,true)==ESP_ERR_INVALID_STATE);
        if (gps_stop_calls>1U) CHECK(wardrive_status.gps_error!=ESP_OK);
        ++gps_cleanup_checks;
    }
    if (gps_stop_failures) { --gps_stop_failures; return ESP_ERR_TIMEOUT; }
    return ESP_OK;
}
void cerberus_wardrive_gps_uart_snapshot(cerberus_wardrive_gps_snapshot_t *out,esp_err_t *error)
{ ++gps_calls; *out=gps_source; if(error)*error=ESP_OK; }
esp_err_t cerberus_storage_wardrive_session_begin(void)
{ ++sd_calls; if(session_error)return session_error; CHECK(!storage_owned); storage_owned=true; return ESP_OK; }
esp_err_t cerberus_storage_wardrive_session_finish(void)
{ ++sd_calls; CHECK(!storage_open[0]&&!storage_open[1]); storage_owned=false; return ESP_OK; }
static void check_preview(void)
{
    if (!inspect_pending) return;
    cerberus_wardrive_recent_snapshot_t view;
    cerberus_wardrive_copy_recent(&view);
    CHECK(view.available && view.count && view.rows[0].outcome==CERBERUS_WARDRIVE_RECENT_PENDING);
}
static esp_err_t sd_begin(unsigned which,const char *header,size_t len)
{ ++sd_calls; CHECK(!lock_depth && header && len); check_preview(); if(begin_error)return begin_error; CHECK(!storage_open[which]); storage_open[which]=true; return ESP_OK; }
static esp_err_t sd_append(unsigned which,const char *row,size_t len)
{
    ++sd_calls; ++sd_appends; CHECK(!lock_depth && storage_open[which] && len==strlen(row));
    check_preview(); if(append_error&&(!fail_append_at||sd_appends==fail_append_at)){
        partial_bytes+=len/2; return append_error;
    }
    if(stop_after_append && sd_appends==stop_after_append)cerberus_wardrive_request_stop();
    return ESP_OK;
}
static esp_err_t sd_finish(unsigned which,bool keep,char *name,size_t cap,uint64_t *bytes)
{ ++sd_calls; (void)keep; CHECK(storage_open[which]); storage_open[which]=false; snprintf(name,cap,"TEST.CSV"); if(bytes)*bytes=100; return ESP_OK; }
esp_err_t cerberus_storage_wardrive_begin(const char *h,size_t n){return sd_begin(0,h,n);}
esp_err_t cerberus_storage_wardrive_wigle_begin(const char *h,size_t n){return sd_begin(1,h,n);}
esp_err_t cerberus_storage_wardrive_append(const char *r,size_t n){return sd_append(0,r,n);}
esp_err_t cerberus_storage_wardrive_wigle_append(const char *r,size_t n){return sd_append(1,r,n);}
esp_err_t cerberus_storage_wardrive_finish(bool k,char *n,size_t c,uint64_t *b){return sd_finish(0,k,n,c,b);}
esp_err_t cerberus_storage_wardrive_wigle_finish(bool k,char *n,size_t c,uint64_t *b,cerberus_wardrive_compact_stats_t *stats){memset(stats,0,sizeof(*stats));return sd_finish(1,k,n,c,b);}
esp_err_t cerberus_storage_wardrive_sync(void){++sd_calls;return ESP_OK;}
esp_err_t cerberus_storage_wardrive_wigle_sync(void){++sd_calls;return ESP_OK;}

static void reset(void)
{
    worker_active=start_in_progress=false;
    CHECK(cerberus_wardrive_clear_recent()==ESP_OK);
    if(workspace)release_workspace();
    memset(&wardrive_status,0,sizeof(wardrive_status));
    memset(&wifi_status,0,sizeof(wifi_status)); memset(&ble_status,0,sizeof(ble_status));
    memset(&field_status,0,sizeof(field_status)); memset(&stream,0,sizeof(stream));
    memset(&gps_source,0,sizeof(gps_source)); memset(storage_open,0,sizeof(storage_open));
    gps_admission=(cerberus_wardrive_gps_uart_state_t){.driver_active=true,.reader_running=true,
        .clock_valid=true,.gps={.state=CERBERUS_WARDRIVE_GPS_FRESH,.coordinates_valid=true,
        .utc_valid=true,.utc_from_rmc=true}};
    memset(wifi_source,0,sizeof(wifi_source)); memset(ble_source,0,sizeof(ble_source));
    memset(ble_evidence,0,sizeof(ble_evidence));
    allocations=frees=task_creates=sd_calls=sd_appends=rf_starts=status_polls=gps_calls=0;
    gps_stop_calls=gps_stop_failures=gps_cleanup_checks=0;inspect_gps_cleanup=false;
    wifi_reads=ble_copies=fail_alloc_at=stop_after_append=fail_append_at=wifi_start_calls=0;
    fail_task=immediate_task=inspect_pending=inspect_free=storage_owned=auto_complete=enqueue_on_start=false;
    status_error=session_error=append_error=begin_error=copy_error=ESP_OK;
    wifi_first_start_error=ESP_OK;
    partial_bytes=0; pending_task=NULL; recent_session_id=recent_sequence=0;
    clock_us=10000000; stop_requested=false;
    host_initialized_value=true;reset_during_claim=replace_during_claim=false;
    reset_before_release=replace_before_release=false;host_next_token=0;host_live_lease=(cerberus_s3_ble_host_lease_t){0};
    host_epoch_value=8;host_claims=host_releases=0;host_owner_value=CERBERUS_S3_BLE_OWNER_NONE;
    passive_boundary_reset();
    for(unsigned i=0;i<32;++i){
        wifi_source[i]=(cerberus_c5_wifi_result_t){.bssid={2,1,2,3,4,(uint8_t)i},.channel=6,.rssi=-50,.ssid="Lab"};
        ble_source[i]=(cerberus_c5_ble_result_t){.address={2,1,2,3,4,(uint8_t)i},.address_type=1,.rssi=-60,.name="Tag",.manufacturer_id=76};
    }
}
static void prepare_capture(void)
{
    CHECK(cerberus_wardrive_start(false,true)==ESP_OK && worker_active && pending_task);
    recent_begin_session(); /* Same worker-entry function, then exercise captured banks. */
    wifi_status=(cerberus_wifi_scan_status_payload_t){.state=CERBERUS_WIFI_SCAN_COMPLETE,.generation=255,.stored_count=2};
    passive_seed_completed(255U,255U,2U);
    wardrive_status.state=CERBERUS_WARDRIVE_RUNNING;
}
static void capture_wifi_ok(bool final)
{ char radio='?'; CHECK(capture_wifi(255,2,final,now_ms()+1000,&radio)==ESP_OK && radio=='S'); }
static cerberus_wardrive_recent_snapshot_t snapshot(void)
{ cerberus_wardrive_recent_snapshot_t out; cerberus_wardrive_copy_recent(&out); return out; }

#include "wardrive_passive_worker_check.h"

int main(void)
{
    for (unsigned fault=0;fault<12;++fault) {
        reset();
        switch(fault) {
        case 1:gps_admission.gps.state=CERBERUS_WARDRIVE_GPS_NO_FIX;break;
        case 2:gps_admission.gps.coordinates_valid=false;break;
        case 3:gps_admission.gps.utc_valid=false;break;
        case 4:gps_admission.gps.utc_from_rmc=false;break;
        case 5:gps_admission.gps.fix_age_ms=CERBERUS_WARDRIVE_GPS_FIX_MAX_AGE_MS+1;break;
        case 6:gps_admission.gps.utc_age_ms=CERBERUS_WARDRIVE_GPS_FIX_MAX_AGE_MS+1;break;
        case 7:gps_admission.driver_active=false;break;
        case 8:gps_admission.reader_running=false;break;
        case 9:gps_admission.clock_valid=false;break;
        case 10:gps_admission.stopping=true;break;
        case 11:gps_admission.error=ESP_FAIL;break;
        }
        CHECK(cerberus_wardrive_start(false,fault!=0)==(fault==11?ESP_FAIL:ESP_ERR_NOT_FINISHED));
        CHECK(!worker_active&&!workspace&&!task_creates&&!sd_calls&&!rf_starts&&!allocations);
    }
    reset();
    cerberus_wardrive_recent_snapshot_t view=snapshot();
    CHECK(!view.available && !view.count && !view.session_id && view.state==CERBERUS_WARDRIVE_IDLE);
    cerberus_wardrive_copy_recent(NULL); cerberus_wardrive_get_recent_status(NULL);
    prepare_capture(); inspect_pending=true; capture_wifi_ok(false);
    view=snapshot(); CHECK(view.count==2 && view.rows[0].sequence==2 && view.rows[0].receipt_ms==10000);
    CHECK(view.rows[0].outcome==CERBERUS_WARDRIVE_RECENT_APPENDED_UNLOCATED && sd_appends==2);
    unsigned sd_before=sd_calls,polls_before=status_polls,rf_before=rf_starts,alloc_before=allocations;
    uint64_t revision=view.revision;
    for(unsigned i=0;i<100;++i){
        cerberus_wardrive_recent_status_t status; cerberus_wardrive_get_recent_status(&status);
        view=snapshot(); CHECK(status.count==view.count && status.session_id==view.session_id &&
            status.revision==view.revision && status.events==2 && status.retained_events==2);
    }
    CHECK(sd_calls==sd_before && status_polls==polls_before && rf_starts==rf_before && allocations==alloc_before);
    CHECK(cerberus_wardrive_clear_recent()==ESP_ERR_INVALID_STATE);
    clock_us+=1000000; capture_wifi_ok(true); view=snapshot();
    CHECK(view.revision==revision && view.rows[0].receipt_ms==10000 && sd_appends==2);
    reset_wifi_capture(); capture_wifi_ok(false); view=snapshot();
    CHECK(view.count==2 && view.rows[0].sequence==4 && view.rows[0].receipt_ms==11000 && sd_appends==2);
    CHECK(view.rows[0].outcome==CERBERUS_WARDRIVE_RECENT_DUPLICATE);
    CHECK(capture_ble(255,2,false)==ESP_OK); view=snapshot();
    CHECK(view.count==4 && view.rows[0].observation.type==CERBERUS_WARDRIVE_BLE &&
        view.rows[0].observation.address[0]==1 && view.rows[0].observation.address[5]==2);
    revision=view.revision; clock_us+=1000000; CHECK(capture_ble(255,2,true)==ESP_OK);
    CHECK(snapshot().revision==revision && ble_copies==1);
    expansion_record_t expansion={.radio=1,.bssid={2,1,2,3,4,99},.channel=36,.rssi=-70,.ssid="Remote",.ssid_length=6};
    CHECK(field_survey_stream_push(&stream,&expansion)); CHECK(capture_expansions(false)==ESP_OK);
    view=snapshot(); CHECK(view.rows[0].observation.radio==CERBERUS_WARDRIVE_RADIO_R1 && view.rows[0].sequence==7);
    revision=view.revision; CHECK(capture_expansions(true)==ESP_OK && snapshot().revision==revision);
    inspect_pending=false; stop_scans_and_close_file(false,0,false,0,255U,false); release_workspace();
    worker_active=false; wardrive_status.state=CERBERUS_WARDRIVE_STOPPED;
    view=snapshot(); CHECK(view.available && view.count==5 && !view.worker_active && view.revision==revision);

    /* Failed admissions and task creation retain the old model and identity. */
    uint64_t session=view.session_id;
    status_error=ESP_ERR_TIMEOUT; CHECK(cerberus_wardrive_start(false,true)==status_error);
    CHECK(snapshot().session_id==session && snapshot().revision==revision);
    status_error=ESP_OK; wifi_status.state=0; ble_status.state=0; fail_task=true;
    CHECK(cerberus_wardrive_start(false,true)==ESP_ERR_NO_MEM && !workspace && !worker_active);
    CHECK(snapshot().session_id==session && snapshot().revision==revision);
    enqueue_on_start=true; field_status.present_mask=1;
    unsigned previous_appends=sd_appends;
    CHECK(cerberus_wardrive_start(true,true)==ESP_ERR_NO_MEM && !workspace && !worker_active);
    CHECK(sd_appends==previous_appends && !stream.count && !storage_owned);
    CHECK(snapshot().session_id==session && snapshot().revision==revision && !recent_worker_session);
    enqueue_on_start=false;
    fail_task=false; immediate_task=true;
    CHECK(cerberus_wardrive_start(false,true)==ESP_OK && !workspace && !worker_active);
    view=snapshot(); CHECK(view.available && !view.count && view.session_id==session+1 && view.state==CERBERUS_WARDRIVE_STOPPED);
    inspect_free=true; CHECK(cerberus_wardrive_clear_recent()==ESP_OK); inspect_free=false;
    CHECK(!snapshot().available && snapshot().session_id==session+1);

    /* Optional model OOM never rejects the existing logger or falls back to internal RAM. */
    reset(); fail_alloc_at=2; immediate_task=true;
    CHECK(cerberus_wardrive_start(false,true)==ESP_OK);
    view=snapshot(); CHECK(!view.available && view.view_error==ESP_ERR_NO_MEM && view.state==CERBERUS_WARDRIVE_STOPPED);
    CHECK(allocations==2 && frees==1 && !workspace && !storage_owned);
    fail_alloc_at=0; CHECK(cerberus_wardrive_start(false,true)==ESP_OK && snapshot().available);
    reset(); fail_alloc_at=2; prepare_capture(); capture_wifi_ok(false);
    CHECK(!snapshot().available && snapshot().view_error==ESP_ERR_NO_MEM && sd_appends==2);
    reset(); session_error=ESP_FAIL;
    CHECK(cerberus_wardrive_start(false,true)==ESP_FAIL && !workspace && !worker_active && !pending_task);
    CHECK(!recent_session_id && !wardrive_recent && allocations==1 && frees==1);

    /* Run the actual worker through both completed radios and asynchronous Stop. */
    reset(); CHECK(cerberus_wardrive_start(false,true)==ESP_OK);
    stop_after_append=4; auto_complete=true;
    void (*worker)(void *)=pending_task; pending_task=NULL; worker(NULL);
    view=snapshot(); CHECK(!worker_active && !workspace && !storage_owned && view.count==4);
    /* Existing independent scheduling starts the next C5 sweep before the
     * first BLE bank requests Stop; cleanup drains that already owned sweep. */
    CHECK(view.state==CERBERUS_WARDRIVE_STOPPED && view.available && sd_appends==4 && rf_starts==3);
    CHECK(gps_calls>0 && task_creates==1 && allocations==2 && frees==1);

    reset(); prepare_capture(); begin_error=ESP_FAIL; inspect_pending=true;
    char failed_radio='?';
    CHECK(capture_wifi(255,2,false,now_ms()+1000,&failed_radio)==ESP_FAIL && !sd_appends);
    CHECK(snapshot().count==1 && snapshot().rows[0].outcome==CERBERUS_WARDRIVE_RECENT_ERROR);
    unsigned begin_calls=sd_calls; begin_error=ESP_OK;
    CHECK(capture_wifi(255,2,true,now_ms()+1000,&failed_radio)==ESP_FAIL && sd_calls==begin_calls);

    /* Actual partial-write fault followed by cleanup: one attempt, retained ERROR. */
    for(unsigned kind=0;kind<2;++kind){
        reset(); prepare_capture(); inspect_pending=true; append_error=ESP_FAIL;
        char radio='?'; esp_err_t error=kind?capture_ble(255,2,false):capture_wifi(255,2,false,now_ms()+1000,&radio);
        CHECK(error==ESP_FAIL && sd_appends==1 && partial_bytes>0);
        view=snapshot(); CHECK(view.count==1 && view.rows[0].outcome==CERBERUS_WARDRIVE_RECENT_ERROR);
        uint64_t receipt=view.rows[0].receipt_ms; revision=view.revision;
        append_error=ESP_OK; clock_us+=1000000; cerberus_wardrive_request_stop();
        error=kind?capture_ble(255,2,true):capture_wifi(255,2,true,now_ms()+1000,&radio);
        CHECK(error==ESP_FAIL && sd_appends==1 && snapshot().revision==revision);
        CHECK(snapshot().rows[0].receipt_ms==receipt);
        inspect_pending=false;
        stop_scans_and_close_file(!kind,255,false,0,kind?255U:0U,kind!=0);
        CHECK(sd_appends==1 && snapshot().revision==revision && !storage_owned);
        release_workspace(); worker_active=false; wardrive_status.state=CERBERUS_WARDRIVE_ERROR;
        CHECK(snapshot().rows[0].outcome==CERBERUS_WARDRIVE_RECENT_ERROR);
    }
    /* Stop between rows resumes only the unprocessed suffix; source failure publishes nothing. */
    reset(); prepare_capture(); copy_error=ESP_ERR_TIMEOUT;
    char radio='?'; CHECK(capture_wifi(255,2,false,now_ms()+1000,&radio)==ESP_ERR_TIMEOUT && !snapshot().count && !sd_appends);
    copy_error=ESP_OK; stop_after_append=1; capture_wifi_ok(false);
    view=snapshot(); CHECK(view.count==1 && wifi_capture_next==1);
    uint64_t first_receipt=view.rows[0].receipt_ms; clock_us+=500000;
    capture_wifi_ok(true); view=snapshot(); CHECK(view.count==2 && view.rows[1].receipt_ms==first_receipt && sd_appends==2);
    /* Invalid raw rows are excluded, and the display receipt may be unavailable. */
    reset(); prepare_capture(); wifi_source[0].channel=0; capture_wifi_ok(false);
    CHECK(snapshot().count==1 && wardrive_status.invalid_sightings==1);
    reset_wifi_capture(); clock_us=-1; wifi_source[0].channel=6;
    wardrive_recent_attempt_t attempt={0};
    cerberus_wardrive_observation_t ap={.type=CERBERUS_WARDRIVE_WIFI,.radio=0,.address={2,3,4,5,6,7},.channel=1,.rssi_dbm=-40};
    CHECK(append_recent_observation(&ap,&attempt)==ESP_OK);
    view=snapshot(); CHECK(!view.rows[0].receipt_valid && view.rows[0].receipt_ms==0);
    cerberus_wardrive_recent_status_t compact; cerberus_wardrive_get_recent_status(&compact); CHECK(compact.clock_faults==1);
    clock_us=12000000; attempt=(wardrive_recent_attempt_t){0}; ap.address[5]++;
    wardrive_status.gps_requested=true;
    gps_source=(cerberus_wardrive_gps_snapshot_t){.state=CERBERUS_WARDRIVE_GPS_FRESH,.coordinates_valid=true,
        .latitude=31,.longitude=-80,.utc_valid=true,.utc_from_rmc=true,.utc="2026-10-30T03:00:00.000Z"};
    CHECK(append_recent_observation(&ap,&attempt)==ESP_OK && snapshot().rows[0].outcome==CERBERUS_WARDRIVE_RECENT_APPENDED_WIGLE);
    /* Probe/relation evidence keeps its existing routing and never enters this AP/BLE list. */
    revision=snapshot().revision;
    cerberus_wardrive_observation_t excluded=ap; excluded.type=CERBERUS_WARDRIVE_WIFI_PROBE;
    attempt=(wardrive_recent_attempt_t){0}; recent_preview(&excluded,&attempt);
    excluded.type=CERBERUS_WARDRIVE_WIFI_RELATION; attempt=(wardrive_recent_attempt_t){0}; recent_preview(&excluded,&attempt);
    CHECK(snapshot().revision==revision);
    reset(); start_in_progress=true; CHECK(cerberus_wardrive_clear_recent()==ESP_ERR_INVALID_STATE); start_in_progress=false;
    prepare_capture(); recent_sequence=UINT64_MAX;
    attempt=(wardrive_recent_attempt_t){0}; recent_preview(&ap,&attempt);
    CHECK(recent_sequence==UINT64_MAX && snapshot().count==0 && snapshot().view_error==ESP_ERR_INVALID_STATE);
    reset();
    prepare_capture(); recent_session_id=UINT64_MAX; recent_begin_session();
    CHECK(!snapshot().available && snapshot().view_error==ESP_ERR_INVALID_STATE);
    run_passive_worker_cases();
    reset();
    printf("Wardriving recent service: %u checks; actual whole-service admission, retained PSRAM, immutable cursors, outcomes and cleanup pass\n",checks);
    return 0;
}
