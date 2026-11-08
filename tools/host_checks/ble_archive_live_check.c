#include "archive_live_adapters.h"
#include "ble_archive_io.h"
#include "s3_ble_discovery_inventory.h"
#include "s3_findmy_monitor.h"
#include "dual_ble_scan.h"
#include "ble_archive_family_fixture.h"
#include "ble_archive_response_fixture.h"
#define ESP_ERROR_CHECK(x) CHECK((x) == ESP_OK)
#define MALLOC_CAP_SPIRAM 1U
#define MALLOC_CAP_8BIT 2U
typedef enum { UI_PAGE_BLE, UI_PAGE_FILES, UI_PAGE_BLE_ARCHIVE, UI_PAGE_MAIN } ui_page_t;
typedef enum { UI_BLE_MODE_MENU, UI_BLE_MODE_DETAIL, UI_BLE_MODE_C5, UI_BLE_MODE_LIST,
    UI_BLE_MODE_FINDMY_MONITOR, UI_BLE_MODE_FOX_HUNT, UI_BLE_MODE_ANALYZER } ui_ble_mode_t;
enum { UI_FILE_MODE_LIST };
static ui_page_t current_page;
static ui_ble_mode_t ble_mode;
static bool ui_capability_blocked;
static unsigned lookup_entries;
static bool saved_ble_lookup_holds_files(void) { return false; }
static bool saved_ble_lookup_is_open(void) { return false; }
static bool saved_ble_lookup_tick(bool asleep) { (void)asleep; return false; }
static esp_err_t saved_ble_lookup_draw(void) { CHECK(false); return ESP_FAIL; }
static void saved_ble_lookup_tap(uint16_t x,uint16_t y) { (void)x;(void)y;CHECK(false); }
static void saved_ble_lookup_open(bool json) { CHECK(!json); ++lookup_entries; }
enum { UI_BACK_Y=426, UI_BACK_HEIGHT=44 };
static int file_mode;
static bool ble_selection_valid, ble_selected_raw_frozen, ble_selected_original_time64_valid;
static bool ble_selected_from_monitor;
static uint64_t ble_selected_original_ms64, ble_selected_source_ceiling_ms64;
static uint64_t ble_selected_scan_epoch;
static cerberus_c5_ble_result_t ble_selected_result;
static cerberus_findmy_monitor_record_t ble_findmy_selected;
typedef struct { cerberus_s3_ble_discovery_inventory_t inventory; } ble_discovery_live_store_t;
static ble_discovery_live_store_t discovery;
static ble_discovery_live_store_t *ble_discovery_live;
static int64_t now_us;
static unsigned clock_reads;
static int64_t esp_timer_get_time(void) { ++clock_reads;return now_us; }
static cerberus_dual_ble_scan_t companion;
static cerberus_dual_ble_scan_t *dual_ble_ui_store;
static cerberus_dual_ble_row_t dual_ble_ui_selected;
static cerberus_dual_ble_provenance_t dual_ble_ui_selected_provenance;
static bool dual_ble_ui_detail, s3_released, c5_released;
static bool dual_ble_ui_from_monitor, monitor_requested;
static unsigned monitor_stops, monitor_captured_rows;
static void ble_findmy_monitor_return(void) { monitor_requested=false;ble_mode=UI_BLE_MODE_FINDMY_MONITOR; }
static void ble_findmy_request_stop(bool exit, bool select);
static unsigned s3_stop_requests, ordinary_captured_rows, pending_exchanges;
static bool ordinary_requested, ordinary_selection_pending, dual_ble_ui_selection_pending;
static bool mutate_live_on_stop;
static uint8_t dual_ble_ui_painted_count;
#include "archive_pending_cancel.h"
static void ble_discovery_leave(void);
static bool dual_ble_live_discovery_released(void) { return s3_released; }
static bool dual_ble_ui_released(void) { return c5_released; }
static void *allocation;
static unsigned allocations, rejected_allocations, frees, saves, loads, copies, cancels, releases, repaints;
static bool allocation_fails;
static esp_err_t start_error, copy_error;
static uint64_t sequence;
static uint64_t last_canceled_job, last_released_job;
static cerberus_ble_archive_io_status_t io;
static cerberus_ble_archive_t saved;
static size_t saved_length;
static uint8_t file_bytes[CERBERUS_BLE_ARCHIVE_MAX_SIZE];
static size_t file_length;
static bool load_from_file;
static void *heap_caps_calloc(size_t count,size_t size,unsigned caps)
{ CHECK(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));CHECK(!allocation);++allocations;
  if(allocation_fails){++rejected_allocations;return NULL;}allocation=calloc(count,size);CHECK(allocation);return allocation; }
static void heap_caps_free(void *p) { CHECK(p&&p==allocation);++frees;free(p);allocation=NULL; }
static esp_err_t draw_detail_page(void);
static esp_err_t draw_current_page(void);
static void open_tool_page(ui_page_t page) { current_page=page; }
esp_err_t cerberus_ble_archive_io_save(const cerberus_ble_archive_t *doc,uint64_t *job)
{ ++saves;*job=0;if(start_error)return start_error;CHECK(cerberus_ble_archive_validate(doc));
  static uint8_t encoded[CERBERUS_BLE_ARCHIVE_MAX_SIZE];size_t length=0;
  CHECK(cerberus_ble_archive_encode(doc,encoded,sizeof(encoded),&length));
  CHECK(length==32U+432U*doc->count);saved_length=length;
  memcpy(file_bytes,encoded,length);file_length=length;
  CHECK(cerberus_ble_archive_decode(encoded,length,&saved));
  io=(cerberus_ble_archive_io_status_t){.job_id=++sequence,.worker_active=true,.state=CERBERUS_BLE_ARCHIVE_IO_SAVING};*job=sequence;return ESP_OK; }
esp_err_t cerberus_ble_archive_io_load(const char *path,const char *name,uint64_t *job)
{ ++loads;*job=0;CHECK(path&&name);if(start_error)return start_error;
  if(load_from_file)CHECK(cerberus_ble_archive_decode(file_bytes,file_length,&saved));
  io=(cerberus_ble_archive_io_status_t){.job_id=++sequence,.worker_active=true,.state=CERBERUS_BLE_ARCHIVE_IO_LOADING};*job=sequence;return ESP_OK; }
void cerberus_ble_archive_io_get_status(cerberus_ble_archive_io_status_t *out) { *out=io; }
esp_err_t cerberus_ble_archive_io_copy_result(uint64_t job,cerberus_ble_archive_t *out)
{ ++copies;CHECK(job==io.job_id&&!io.worker_active&&io.result_ready);if(copy_error)return copy_error;*out=saved;return ESP_OK; }
void cerberus_ble_archive_io_cancel(uint64_t job) { CHECK(job);++cancels;last_canceled_job=job;if(job==io.job_id)io.cancel_requested=true; }
void cerberus_ble_archive_io_release(uint64_t job) { CHECK(job);++releases;last_released_job=job;if(job==io.job_id)io.result_ready=false; }
/* JSON is a separate offline controller, exercised by check_airtag_json_ui.
 * This native capture fixture never enters it; unexpected dispatch fails. */
static bool airtag_json_is_open(void) { return false; }
static esp_err_t airtag_json_draw(void) { CHECK(false); return ESP_FAIL; }
static void airtag_json_tap(uint16_t x,uint16_t y) { (void)x;(void)y;CHECK(false); }
static void airtag_json_open_export(void) { CHECK(false); }
static bool airtag_json_tick(bool asleep) { (void)asleep;return false; }
#include "ble_archive_live_ui.h"
/* Shared-owner boundary: the dedicated discovery/controller suites exercise
 * its actual lifecycle; here a stop deliberately mutates the source to prove
 * archive capture precedes cancellation and never reads it again. */
static void ble_discovery_leave(void)
{
    ++s3_stop_requests;ordinary_requested=ordinary_selection_pending=false;
    dual_ble_ui_cancel_selection();
    ordinary_captured_rows=ble_archive_view?ble_archive_view->incoming.count:0U;
    if(dual_ble_ui_store)cerberus_dual_ble_stop(dual_ble_ui_store);
    if(mutate_live_on_stop){
        memset(&discovery.inventory,0,sizeof(discovery.inventory));
        memset(companion.rows,0,sizeof(companion.rows));
        memset(&dual_ble_ui_selected,0,sizeof(dual_ble_ui_selected));
    }
}
static esp_err_t pending_exchange(void *ctx,uint8_t cmd,const void *req,size_t n,void *out,size_t cap)
{(void)ctx;(void)cmd;(void)req;(void)n;(void)out;(void)cap;++pending_exchanges;return ESP_ERR_TIMEOUT;}
static void ble_findmy_request_stop(bool exit, bool select)
{ CHECK(!select);monitor_requested=false;++monitor_stops;
  if(current_page==UI_PAGE_BLE_ARCHIVE&&ble_archive_view)monitor_captured_rows=ble_archive_view->incoming.count;
  if(exit)ble_mode=UI_BLE_MODE_LIST; }
static void request_selected(void) { ble_archive_save_selected();ble_archive_tick(false); }
static esp_err_t draw_detail_page(void) { drawn_size=0;drawn[0]=0;calls=0;++repaints;return ble_archive_draw(); }
static esp_err_t draw_current_page(void) { ++repaints;return current_page==UI_PAGE_BLE_ARCHIVE?draw_detail_page():ESP_OK; }
#include "archive_c5_footer.h"

static void seed(void)
{
    ble_archive_dispose();memset(&discovery,0,sizeof(discovery));ble_discovery_live=&discovery;
    discovery.inventory.initialized=true;discovery.inventory.count=2;now_us=10000000;
    for(unsigned i=0;i<2;++i){
        cerberus_s3_ble_discovery_entry_t *e=&discovery.inventory.entries[i];
        e->epoch=8+i;e->source_now_ms=9500;e->original_source_ms=9000;
        e->record.peer.address[0]=7;e->record.peer.address_type=i;e->record.peer.rssi=-44;
        strcpy(e->record.peer.name,i?"random typed peer":"public typed peer");
        e->record.peer.manufacturer_id=0xFFFF;e->record.generation=4;e->record.index=i;
        e->record.advertisement.address[0]=7;e->record.advertisement.address_type=i;
        uint8_t data[]={2,1,6};memcpy(e->record.advertisement.data,data,3);e->record.advertisement.length=3;
        e->record.advertisement.reports_seen=9;
    }
    cerberus_s3_ble_discovery_record_t *r=&discovery.inventory.entries[0].record;
    ble_findmy_selected=(cerberus_findmy_monitor_record_t){.peer=r->peer,.advertisement=r->advertisement,
        .generation=r->generation,.index=r->index,.observed_ms=9000};
    ble_selected_result=r->peer;ble_selection_valid=ble_selected_raw_frozen=ble_selected_original_time64_valid=true;
    ble_selected_original_ms64=9000;ble_selected_source_ceiling_ms64=9500;
    ble_selected_scan_epoch=0;
    current_page=UI_PAGE_BLE;ble_mode=UI_BLE_MODE_MENU;allocation_fails=false;start_error=copy_error=ESP_OK;
    io=(cerberus_ble_archive_io_status_t){0};privacy_mask=false;
    memset(&companion,0,sizeof(companion));dual_ble_ui_store=NULL;dual_ble_ui_detail=false;
    dual_ble_ui_from_monitor=ble_selected_from_monitor=monitor_requested=false;monitor_captured_rows=0;
    ordinary_requested=ordinary_selection_pending=dual_ble_ui_selection_pending=false;
    mutate_live_on_stop=false;ordinary_captured_rows=0;dual_ble_ui_painted_count=0;
    memset(&dual_ble_ui_selected,0,sizeof(dual_ble_ui_selected));
    memset(&dual_ble_ui_selected_provenance,0,sizeof(dual_ble_ui_selected_provenance));
    s3_released=c5_released=true;
    ui_capability_blocked=false;
    load_from_file=false;
}
static void complete(void)
{ io.state=CERBERUS_BLE_ARCHIVE_IO_COMPLETE;io.worker_active=false;io.result_ready=true;
  strcpy(io.file_name,"B00001.BLE");ble_archive_tick(false); }
static void seed_response(void)
{
    seed();
    cerberus_s3_ble_discovery_record_t *r=&discovery.inventory.entries[0].record;
    r->observed_ms=9000;
    cerberus_s3_ble_discovery_evidence_t *e=&r->focus.discovery;
    memcpy(e->address,r->peer.address,6);e->address_type=r->peer.address_type;e->generation=r->generation;
    e->advertisement_sequence=e->raw_advertisement_sequence=1;e->scan_response_sequence=2;
    e->advertisement_observed_ms=9000;e->scan_response_observed_ms=9100;
    e->flags=CERBERUS_S3_BLE_EVIDENCE_ADV_COMPLETE|CERBERUS_S3_BLE_EVIDENCE_ADV_VALID|
        CERBERUS_S3_BLE_EVIDENCE_RAW_COMPLETE|CERBERUS_S3_BLE_EVIDENCE_RESPONSE_VALID;
    e->scan_response.length=e->scan_response.original_length=5;e->scan_response.event_type=4;
    memcpy(e->scan_response.data,"\004\011RSP",5);
    ble_findmy_selected.focus=r->focus;ble_selected_scan_epoch=discovery.inventory.entries[0].epoch;
}
static void test_response_capture(void)
{
    seed_response();ble_archive_open_menu();CHECK(ble_archive_capture(BLE_ARCHIVE_BOTH)==ESP_OK);
    const cerberus_ble_archive_row_t *row=&ble_archive_view->incoming.rows[0];
    CHECK(row->response_state==CERBERUS_BLE_ARCHIVE_RESPONSE_PRESENT&&row->response_source_uptime_ms==9100);
    CHECK(row->original_source_uptime_ms==9000&&row->response_length==5&&!memcmp(row->response,"\004\011RSP",5));
    CHECK(ble_archive_view->incoming.rows[1].response_state==CERBERUS_BLE_ARCHIVE_RESPONSE_UNKNOWN);
    memset(&discovery.inventory.entries[0].record.focus,0,sizeof(discovery.inventory.entries[0].record.focus));
    CHECK(ble_archive_capture(BLE_ARCHIVE_S3_SELECTED)==ESP_OK&&ble_archive_view->incoming.count==1);
    CHECK(row->scan_epoch_valid&&row->scan_epoch==8&&row->response_state==CERBERUS_BLE_ARCHIVE_RESPONSE_PRESENT);
    request_selected();CHECK(saved.rows[0].response_state==CERBERUS_BLE_ARCHIVE_RESPONSE_PRESENT);
    CHECK(saved.rows[0].response_sequence==2&&saved.rows[0].response_source_uptime_ms==9100);
    complete();CHECK(ble_archive_view->document.rows[0].response_length==5);
    /* Missing full session provenance cannot be borrowed from a later list. */
    seed_response();ble_archive_open_menu();ble_selected_scan_epoch=0;
    CHECK(ble_archive_capture(BLE_ARCHIVE_S3_SELECTED)==ESP_OK&&
        ble_archive_view->incoming.rows[0].response_state==CERBERUS_BLE_ARCHIVE_RESPONSE_UNKNOWN);
    seed_response();ble_archive_open_menu();
    ble_findmy_selected.focus.discovery.advertisement_sequence=3;
    ble_findmy_selected.focus.discovery.scan_response_sequence=4;
    CHECK(ble_archive_capture(BLE_ARCHIVE_S3_SELECTED)==ESP_OK&&
        ble_archive_view->incoming.rows[0].response_state==CERBERUS_BLE_ARCHIVE_RESPONSE_UNKNOWN);
    CHECK(ble_archive_view->incoming.rows[0].advertisement_length==3&&
        !memcmp(ble_archive_view->incoming.rows[0].advertisement,"\002\001\006",3));
}
static void test_capture(void)
{
    seed();ble_archive_open_menu();CHECK(ble_archive_view&&ble_archive_menu);CHECK(strstr(drawn,"SAVE BOTH RECEIVERS"));
    CHECK(ble_archive_capture(false)==ESP_OK&&ble_archive_view->incoming.count==2);
    CHECK(ble_archive_view->incoming.rows[0].scan_epoch==8&&ble_archive_view->incoming.rows[1].address_type==1);
    CHECK(ble_archive_view->incoming.rows[0].original_source_uptime_ms==9000);
    now_us=-1;CHECK(ble_archive_capture(false)==ESP_ERR_INVALID_STATE);now_us=10000000;
    discovery.inventory.entries[0].source_now_ms=11000;CHECK(ble_archive_capture(false)==ESP_ERR_INVALID_RESPONSE);
    discovery.inventory.entries[0].source_now_ms=9500;
    discovery.inventory.entries[1].record.advertisement.address[0]=8;CHECK(ble_archive_capture(false)==ESP_ERR_INVALID_RESPONSE);
    discovery.inventory.entries[1].record.advertisement.address[0]=7;
    ble_selected_original_time64_valid=false;CHECK(ble_archive_capture(true)==ESP_OK);
    CHECK(!ble_archive_view->incoming.rows[0].source_time_valid&&!ble_archive_view->incoming.rows[0].copy_ceiling_ms);
    CHECK(!ble_archive_view->incoming.rows[0].scan_epoch_valid);
    ble_selected_raw_frozen=false;CHECK(ble_archive_capture(true)==ESP_ERR_NOT_FOUND);
    ble_selected_raw_frozen=true;ble_selected_result.address_type=3;CHECK(ble_archive_capture(true)==ESP_ERR_NOT_FOUND);
    cerberus_ble_archive_row_t row,before;memset(&row,0xA5,sizeof(row));before=row;
    cerberus_c5_ble_result_t peer=ble_findmy_selected.peer;cerberus_c5_ble_advertisement_t raw=ble_findmy_selected.advertisement;
    peer.uuid_length=3;CHECK(!cerberus_ble_archive_capture_row(&row,&peer,&raw,0,0,0,false,0,0,false));CHECK(!memcmp(&row,&before,sizeof(row)));
    peer.uuid_length=0;peer.rssi=126;CHECK(cerberus_ble_archive_capture_row(&row,&peer,&raw,0,0,0,false,0,0,false));CHECK(!row.rssi_valid&&row.rssi==127);
    memset(peer.name,'X',sizeof(peer.name));CHECK(!cerberus_ble_archive_capture_row(&row,&peer,&raw,0,0,0,false,0,0,false));
    ble_archive_return();CHECK(!allocation&&current_page==UI_PAGE_BLE&&ble_mode==UI_BLE_MODE_MENU);
}
static void test_jobs(void)
{
    seed();ble_mode=UI_BLE_MODE_DETAIL;unsigned old_saves=saves;request_selected();
    CHECK(saves==old_saves+1&&ble_archive_view->job_pending&&current_page==UI_PAGE_BLE_ARCHIVE);
    CHECK(saved.count==1&&!strcmp(saved.rows[0].name,"public typed peer"));
    strcpy(ble_findmy_selected.peer.name,"later changed");complete();
    CHECK(ble_archive_view->has_document&&!ble_archive_view->job_pending&&strstr(drawn,"HISTORICAL"));
    CHECK(!strcmp(ble_archive_view->document.rows[0].name,"public typed peer"));
    ble_archive_tap(20,160);CHECK(ble_archive_view->selected_valid);ble_archive_tap(64,426);
    CHECK(ble_archive_view&&ble_archive_view->view==BLE_ARCHIVE_UI_LIST);ble_archive_tap(64,426);
    CHECK(!allocation&&current_page==UI_PAGE_BLE&&ble_mode==UI_BLE_MODE_DETAIL);
    seed();ble_archive_open_menu();ble_archive_tap(40,160);ble_archive_tick(false);CHECK(saved.count==2&&ble_archive_view->job_pending);
    uint64_t wanted=io.job_id;io.job_id=wanted+100;io.worker_active=false;io.state=CERBERUS_BLE_ARCHIVE_IO_ERROR;
    ble_archive_tick(false);CHECK(ble_archive_view->job_pending);io.job_id=wanted;io.worker_active=true;
    unsigned canceled=cancels;ble_archive_tap(64,426);CHECK(cancels==canceled+1&&!allocation&&current_page==UI_PAGE_BLE);
    complete();CHECK(!allocation); /* A late worker outcome cannot reopen the page. */
    seed();current_page=UI_PAGE_FILES;ble_archive_open_file("/moved","old.BLE");CHECK(ble_archive_view->job_pending);
    copy_error=ESP_FAIL;complete();CHECK(ble_archive_view->view==BLE_ARCHIVE_UI_ERROR&&!ble_archive_view->has_document);
    ble_archive_tap(64,426);CHECK(!allocation&&current_page==UI_PAGE_FILES);
    seed();request_selected();unsigned paint=repaints;io.worker_active=false;io.state=CERBERUS_BLE_ARCHIVE_IO_ERROR;io.error=ESP_ERR_INVALID_RESPONSE;
    ble_archive_tick(true);CHECK(repaints==paint&&!ble_archive_view->job_pending);ble_archive_return();
    seed();start_error=ESP_ERR_INVALID_STATE;request_selected();CHECK(ble_archive_view->view==BLE_ARCHIVE_UI_ERROR&&!ble_archive_view->job_pending);ble_archive_return();
    seed();allocation_fails=true;ble_archive_open_menu();CHECK(!ble_archive_view&&strstr(drawn,"ARCHIVE UNAVAILABLE"));ble_archive_tap(64,426);CHECK(current_page==UI_PAGE_BLE);
    seed();ble_archive_open_menu();ble_archive_tap(40,290);CHECK(current_page==UI_PAGE_FILES&&file_mode==UI_FILE_MODE_LIST&&!allocation);
    seed();request_selected();current_page=UI_PAGE_MAIN;ble_archive_tick(false);CHECK(!allocation);
    CHECK(ble_archive_is_file("B00001.BLE")&&ble_archive_is_file("old.ble")&&!ble_archive_is_file("x.TMP")&&!ble_archive_is_file(NULL)&&!ble_archive_is_file(".BLE"));
}
static void test_accepted_job_rejected_by_view(void)
{
    for(unsigned operation=0;operation<2;++operation) {
        seed();request_selected();complete();
        cerberus_ble_archive_t previous=ble_archive_view->document;
        /* Inject an unexpectedly reused job identity at the IO boundary. The
         * actual viewer begin rejects it; both controller entry points must
         * release the newly accepted job and retain the previous document. */
        const uint64_t accepted_job=sequence+1U;
        ble_archive_view->job_id=accepted_job;
        unsigned canceled=cancels,released=releases,copied=copies;
        if(operation==0)request_selected();
        else ble_archive_open_file("/moved","another.BLE");
        CHECK(io.job_id==accepted_job&&io.cancel_requested);
        CHECK(cancels==canceled+1U&&releases==released+1U);
        CHECK(last_canceled_job==accepted_job&&last_released_job==accepted_job);
        CHECK(ble_archive_open_error==ESP_ERR_INVALID_STATE);
        CHECK(ble_archive_view->view==BLE_ARCHIVE_UI_ERROR&&!ble_archive_view->job_pending);
        CHECK(ble_archive_view->has_document&&!memcmp(&previous,&ble_archive_view->document,sizeof(previous)));
        CHECK(strstr(drawn,"PREVIOUS SNAPSHOT RETAINED"));
        ble_archive_tick(false);CHECK(copies==copied&&releases==released+1U);
        ble_archive_return();CHECK(!allocation&&current_page==UI_PAGE_BLE);
    }
}
static void test_complete_without_result(void)
{
    seed();request_selected();complete();
    cerberus_ble_archive_t previous=ble_archive_view->document;
    ble_archive_open_file("/moved","missing-result.BLE");
    const uint64_t job=io.job_id;
    unsigned copied=copies,released=releases;
    io.worker_active=false;io.state=CERBERUS_BLE_ARCHIVE_IO_COMPLETE;
    io.result_ready=false;io.error=ESP_OK;
    ble_archive_tick(false);
    CHECK(copies==copied&&releases==released+1U&&last_released_job==job);
    CHECK(!ble_archive_view->job_pending&&ble_archive_view->view==BLE_ARCHIVE_UI_ERROR);
    CHECK(ble_archive_view->error==ESP_ERR_INVALID_STATE);
    CHECK(ble_archive_view->has_document&&!memcmp(&previous,&ble_archive_view->document,sizeof(previous)));
    CHECK(strstr(drawn,"PREVIOUS SNAPSHOT RETAINED"));
    ble_archive_tick(false);CHECK(copies==copied&&releases==released+1U);
    ble_archive_return();CHECK(!allocation&&current_page==UI_PAGE_BLE);
}
static void seed_both(void)
{
    seed_response();dual_ble_ui_store=&companion;companion.count=32;
    companion.published_boot_id=123;companion.published_epoch=456;companion.published_generation=0;
    discovery.inventory.count=32;
    for(unsigned i=0;i<32;++i){
        discovery.inventory.entries[i]=discovery.inventory.entries[0];
        cerberus_s3_ble_discovery_record_t *s=&discovery.inventory.entries[i].record;
        s->peer.address[0]=(uint8_t)i;s->advertisement.address[0]=(uint8_t)i;
        s->peer.address_type=s->advertisement.address_type=0;s->index=(uint8_t)i;
        memcpy(s->focus.discovery.address,s->peer.address,6);s->focus.discovery.address_type=0;
        cerberus_dual_ble_row_t *c=&companion.rows[i];
        c->address[0]=(uint8_t)i;c->address_type=0;c->rssi=-61;c->timing_flags=3;
        strcpy(c->name,"Frozen C5 row");c->request_started_ms=9900;c->callback_age_ms=5;
        c->raw_length=3;c->raw[0]=2;c->raw[1]=1;c->raw[2]=6;
        if(i%3==1)c->receive=(cerberus_dual_ble_receive_metadata_t){
            .source_kind=CERBERUS_DUAL_BLE_RECEIVE_LEGACY,.callback_length=3,
            .sid=0xff,.data_status=0xff,.ad_validation=CERBERUS_DUAL_BLE_AD_VALID};
        if(i%3==2||i==31){
            c->raw_length=31;c->event_type=3;c->raw_flags=3;
            CHECK(archive_test_family_original(c->raw,&c->receive,&c->family,c->address,c->address_type,'5'));
        }
    }
    memset(companion.staging,0xEE,sizeof(companion.staging));
    ble_mode=UI_BLE_MODE_C5;
}
static void seed_both_responses(uint8_t family)
{
    static cerberus_ble_archive_row_t fixture;
    seed_both();
    companion.start.mode=family?CERBERUS_DUAL_BLE_RESPONSE_FAMILY:CERBERUS_DUAL_BLE_RESPONSE_DISCOVERY;
    companion.start.reserved=family;companion.published_family=family;companion.published_response=true;
    for(unsigned i=0;i<32;++i){
        cerberus_dual_ble_row_t *r=&companion.rows[i];
        memset(&fixture,0,sizeof(fixture));memcpy(fixture.address,r->address,6);fixture.address_type=r->address_type;
        CHECK(archive_test_response_row(&fixture,(uint8_t)(i%6+1),family));
        memset(r->raw,0,sizeof(r->raw));memcpy(r->raw,fixture.advertisement,fixture.advertisement_length);
        r->raw_length=fixture.advertisement_length;r->event_type=fixture.advertisement_event_type;
        r->raw_flags=fixture.advertisement_flags;r->receive=fixture.receive;r->family=fixture.family;
        r->response=fixture.c5_response;r->callback_age_ms=125;
    }
}
static void test_both_and_c5_footer(void)
{
    seed_both();cerberus_c5_ble_result_t selected_before=ble_selected_result;
    CHECK(archive_root_footer_draw()==ESP_OK&&strstr(drawn,"SAVE BOTH"));
    unsigned before=saves,stopped=s3_stop_requests;
    s3_released=c5_released=false;archive_root_footer_tap(220,440);
    CHECK(current_page==UI_PAGE_BLE_ARCHIVE&&ble_archive_view->release_pending&&saves==before);
    CHECK(s3_stop_requests==stopped+1&&strstr(drawn,"SD WRITE NOT STARTED"));
    CHECK(ble_archive_view->incoming.count==64&&ble_archive_view->incoming.saved_uptime_ms==10000);
    CHECK(ble_archive_view->incoming.rows[0].receiver==CERBERUS_BLE_ARCHIVE_RECEIVER_S3);
    CHECK(ble_archive_view->incoming.rows[32].receiver==CERBERUS_BLE_ARCHIVE_RECEIVER_C5&&
        ble_archive_view->incoming.rows[32].age_upper_at_save_ms==105);
    cerberus_dual_ble_receive_metadata_t frozen_receive=companion.rows[31].receive;
    cerberus_dual_ble_family_wire_t frozen_family=companion.rows[31].family;
    CHECK(!memcmp(&ble_archive_view->incoming.rows[63].receive,&frozen_receive,sizeof(frozen_receive)));
    CHECK(!memcmp(&ble_archive_view->incoming.rows[63].family,&frozen_family,sizeof(frozen_family)));
    CHECK(ble_archive_view->incoming.rows[0].response_state==CERBERUS_BLE_ARCHIVE_RESPONSE_PRESENT);
    memset(companion.rows,0,sizeof(companion.rows));companion.published_epoch=999;
    discovery.inventory.entries[0].record.peer.address[0]=99;now_us+=2000000;
    s3_released=true;ble_archive_tick(false);CHECK(saves==before);
    c5_released=true;ble_archive_tick(false);
    CHECK(saves==before+1&&saved.count==64&&saved.rows[32].scan_epoch==456&&
        saved.rows[32].receiver_boot_id==123&&saved.rows[0].address[0]==0&&
        saved.rows[32].age_upper_at_save_ms==105); /* Not refreshed after release. */
    CHECK(saved_length==27680U&&!memcmp(&saved.rows[63].receive,&frozen_receive,sizeof(frozen_receive)));
    CHECK(!memcmp(&saved.rows[63].family,&frozen_family,sizeof(frozen_family)));
    CHECK(saved.rows[0].response_state==CERBERUS_BLE_ARCHIVE_RESPONSE_PRESENT&&saved.rows[0].response_source_uptime_ms==9100);
    CHECK(!memcmp(&selected_before,&ble_selected_result,sizeof(selected_before)));
    complete();CHECK(strstr(drawn,"64/64 OBS / P1/16 / S3 32 C5 32 L0"));
    ble_archive_tap(64,426);CHECK(current_page==UI_PAGE_BLE&&ble_mode==UI_BLE_MODE_C5);

    seed_both();dual_ble_ui_detail=true;
    CHECK(cerberus_dual_ble_copy_row(&companion,31,&dual_ble_ui_selected,&dual_ble_ui_selected_provenance));
    frozen_receive=dual_ble_ui_selected.receive;
    frozen_family=dual_ble_ui_selected.family;
    companion.published_epoch=999;companion.published_boot_id=888;companion.rows[31].address[0]=99;
    memset(&companion.rows[31].receive,0,sizeof(companion.rows[31].receive));
    memset(&companion.rows[31].family,0,sizeof(companion.rows[31].family));
    companion.start.mode=CERBERUS_DUAL_BLE_FAMILY;companion.start.reserved=1;companion.published_family=1;
    CHECK(archive_root_footer_draw()==ESP_OK&&strstr(drawn,"SAVE SD"));
    archive_root_footer_tap(220,440);ble_archive_tick(false);
    CHECK(saved.count==1&&saved.rows[0].receiver==CERBERUS_BLE_ARCHIVE_RECEIVER_C5&&
        saved.rows[0].address[0]==31&&saved.rows[0].index==31&&saved.rows[0].scan_epoch==456&&
        saved.rows[0].receiver_boot_id==123&&saved.rows[0].generation==0);
    CHECK(!memcmp(&saved.rows[0].receive,&frozen_receive,sizeof(frozen_receive))&&saved.rows[0].advertisement_length==31);
    CHECK(!memcmp(&saved.rows[0].family,&frozen_family,sizeof(frozen_family))&&saved.rows[0].family.family_code==6);
    complete();ble_archive_return();CHECK(ble_mode==UI_BLE_MODE_C5&&dual_ble_ui_detail);
    archive_root_footer_tap(156,440);CHECK(ble_mode==UI_BLE_MODE_C5); /* Footer gap. */
    archive_root_footer_tap(30,440);CHECK(ble_mode==UI_BLE_MODE_LIST);

    seed_both();ble_discovery_live=NULL;ble_archive_save_c5(false);ble_archive_tick(false);CHECK(saved.count==32);
    seed();ble_archive_save(BLE_ARCHIVE_BOTH);ble_archive_tick(false);CHECK(saved.count==2);
    seed();ble_discovery_live=NULL;ble_archive_save(BLE_ARCHIVE_BOTH);
    CHECK(ble_archive_open_error==ESP_ERR_NOT_FOUND&&!ble_archive_view->release_pending);
    seed_both();companion.count=33;ble_archive_save_c5(false);CHECK(ble_archive_open_error==ESP_ERR_INVALID_SIZE);
    ble_archive_return();
}
static void test_release_wait(void)
{
    seed();s3_released=false;c5_released=false;unsigned before=saves;
    ble_archive_save_selected();CHECK(ble_archive_view->release_pending&&saves==before);
    ble_archive_tap(64,426);CHECK(!allocation&&current_page==UI_PAGE_BLE&&saves==before);
    s3_released=c5_released=true;ble_archive_tick(false);CHECK(saves==before); /* No late submission. */
    for(unsigned fault=0;fault<4;++fault){
        seed();s3_released=false;ble_archive_save_selected();
        if(fault==0)now_us+=12000000;else if(fault==1)now_us=-1;else if(fault==2)now_us-=1000;
        ble_archive_tick(fault==3);
        CHECK(!ble_archive_view->release_pending&&!ble_archive_view->job_pending&&saves==before);
        CHECK(ble_archive_view->error==(fault==0?ESP_ERR_TIMEOUT:ESP_ERR_INVALID_STATE));
        s3_released=true;ble_archive_tick(false);CHECK(saves==before);
        ble_archive_return();
    }
    seed();s3_released=false;ble_archive_save_selected();
    ble_archive_save_selected();CHECK(saves==before); /* Repeated tap cannot replace frozen incoming. */
    s3_released=true;ble_archive_tick(false);CHECK(saves==before+1);complete();ble_archive_return();
}
static void test_receive_metadata_jobs(void)
{
    static cerberus_ble_archive_t frozen;
    /* A selected envelope is frozen before owner drain, along with its bytes
     * and provenance. Repeated save taps cannot substitute a later selection. */
    seed_both();dual_ble_ui_detail=true;
    CHECK(cerberus_dual_ble_copy_row(&companion,31,&dual_ble_ui_selected,&dual_ble_ui_selected_provenance));
    s3_released=c5_released=false;unsigned before=saves;
    ble_archive_save_c5(true);CHECK(ble_archive_view->release_pending&&saves==before);
    frozen=ble_archive_view->incoming;
    memset(&dual_ble_ui_selected.receive,0,sizeof(dual_ble_ui_selected.receive));
    memset(&dual_ble_ui_selected.family,0,sizeof(dual_ble_ui_selected.family));
    memset(companion.rows,0,sizeof(companion.rows));
    ble_archive_save_c5(true);CHECK(!memcmp(&frozen,&ble_archive_view->incoming,sizeof(frozen)));
    s3_released=true;ble_archive_tick(false);CHECK(saves==before&&!memcmp(&frozen,&ble_archive_view->incoming,sizeof(frozen)));
    c5_released=true;ble_archive_tick(false);CHECK(saves==before+1&&!memcmp(&frozen,&saved,sizeof(frozen)));
    complete();CHECK(!memcmp(&frozen,&ble_archive_view->document,sizeof(frozen)));
    /* A failed later load leaves the previously published v5 document intact. */
    ble_archive_open_file("/COPIED","bad.BLE");io.worker_active=false;
    io.state=CERBERUS_BLE_ARCHIVE_IO_ERROR;io.error=ESP_ERR_INVALID_RESPONSE;
    ble_archive_tick(false);CHECK(ble_archive_view->has_document&&!memcmp(&frozen,&ble_archive_view->document,sizeof(frozen)));
    CHECK(strstr(drawn,"PREVIOUS SNAPSHOT RETAINED"));ble_archive_return();

    /* Cancellation before submit starts no worker; an already submitted job
     * keeps its frozen input and cannot reopen the page on late completion. */
    for(unsigned submitted=0;submitted<2;++submitted){
        seed_both();s3_released=c5_released=false;before=saves;
        ble_archive_save_c5(false);frozen=ble_archive_view->incoming;
        CHECK(frozen.count==64&&frozen.rows[63].receive.callback_length==255);
        if(submitted){s3_released=c5_released=true;ble_archive_tick(false);CHECK(!memcmp(&frozen,&saved,sizeof(frozen)));}
        else CHECK(!memcmp(&frozen,&ble_archive_view->incoming,sizeof(frozen)));
        memset(companion.rows,0,sizeof(companion.rows));
        ble_archive_tap(64,426);CHECK(!allocation&&current_page==UI_PAGE_BLE);
        s3_released=c5_released=true;ble_archive_tick(false);CHECK(saves==before+submitted);
        if(submitted){CHECK(!memcmp(&frozen,&saved,sizeof(frozen)));complete();CHECK(!allocation);}
    }
    /* A release timeout keeps captured evidence reviewable and never retries
     * a background save after ownership eventually clears. */
    seed_both();s3_released=c5_released=false;before=saves;
    ble_archive_save_c5(false);frozen=ble_archive_view->incoming;now_us+=12000000;
    ble_archive_tick(false);CHECK(ble_archive_view->error==ESP_ERR_TIMEOUT&&saves==before);
    CHECK(!memcmp(&frozen,&ble_archive_view->incoming,sizeof(frozen)));
    s3_released=c5_released=true;ble_archive_tick(false);CHECK(saves==before);ble_archive_return();
}
static void test_invalid_family_capture(void)
{
    seed_both();dual_ble_ui_detail=true;
    CHECK(cerberus_dual_ble_copy_row(&companion,31,&dual_ble_ui_selected,&dual_ble_ui_selected_provenance));
    dual_ble_ui_selected.family.version=2;
    unsigned before=saves,stopped=s3_stop_requests;
    ble_archive_save_c5(true);
    CHECK(ble_archive_open_error==ESP_ERR_INVALID_RESPONSE&&ble_archive_view->view==BLE_ARCHIVE_UI_ERROR);
    CHECK(!ble_archive_view->release_pending&&!ble_archive_view->job_pending&&saves==before&&s3_stop_requests==stopped+1);
    CHECK(companion.stop_requested); /* Invalid evidence starts no IO; leaving ordinary acquisition still cancels intent. */
    ble_archive_return();
}
static void test_saved_family_reopen(void)
{
    static cerberus_ble_archive_t frozen;
    seed_both();dual_ble_ui_detail=true;
    CHECK(cerberus_dual_ble_copy_row(&companion,31,&dual_ble_ui_selected,&dual_ble_ui_selected_provenance));
    s3_released=c5_released=false;unsigned before=saves;
    ble_archive_save_c5(true);frozen=ble_archive_view->incoming;
    CHECK(frozen.rows[0].family.hc_name==CERBERUS_BLE_HC_05&&frozen.rows[0].family.family_code==6);
    CHECK(frozen.rows[0].receive.callback_length==255&&frozen.rows[0].advertisement_length==31);
    CHECK(archive_test_family_original(dual_ble_ui_selected.raw,&dual_ble_ui_selected.receive,
        &dual_ble_ui_selected.family,dual_ble_ui_selected.address,dual_ble_ui_selected.address_type,'6'));
    companion.published_boot_id=999;companion.published_epoch=1000;companion.published_family=1;
    companion.start.mode=CERBERUS_DUAL_BLE_FAMILY;companion.start.reserved=1;
    memset(companion.rows,0,sizeof(companion.rows));memset(companion.staging,0xEE,sizeof(companion.staging));
    now_us+=3000000;ble_archive_save_c5(true);
    CHECK(!memcmp(&frozen,&ble_archive_view->incoming,sizeof(frozen))&&saves==before);
    s3_released=c5_released=true;ble_archive_tick(false);complete();
    CHECK(!memcmp(&frozen,&ble_archive_view->document,sizeof(frozen))&&file_length==464);
    ble_archive_return();CHECK(!allocation);

    /* Reopen the actual encoded bytes after the loading boot's clock and live
     * receiver bank have changed. Source clocks/bounds come only from file. */
    now_us=0;load_from_file=true;current_page=UI_PAGE_FILES;
    unsigned clocks=clock_reads,stopped=s3_stop_requests,save_count=saves;
    ble_archive_open_file("/MOVED","family.BLE");complete();
    CHECK(!memcmp(&frozen,&ble_archive_view->document,sizeof(frozen)));
    ble_archive_tap(20,160);CHECK(ble_archive_view->selected_valid);
    ble_archive_tap(64,390);CHECK(ble_archive_view->view==BLE_ARCHIVE_UI_RAW);
    ble_archive_tap(250,65);CHECK(ble_archive_view->raw_hints&&strstr(drawn,"SAVED C5 HINTS"));
    CHECK(strstr(drawn,"HISTORICAL / NO LIVE RADIO")&&strstr(drawn,"AGE AT SAVE: <= 105 ms"));
    CHECK(ble_archive_view->selected.receiver_boot_id==123&&ble_archive_view->selected.scan_epoch==456);
    CHECK(ble_archive_view->selected.family.hc_name==CERBERUS_BLE_HC_05&&ble_archive_view->hint_pages>=2);
    uint8_t pages=ble_archive_view->hint_pages;
    for(uint8_t page=1;page<pages;++page)ble_archive_tap(220,390);
    CHECK(ble_archive_view->hint_page==pages-1&&strstr(drawn,"HC-05"));
    CHECK(!strstr(drawn,"HC-06")&&clock_reads==clocks&&s3_stop_requests==stopped&&saves==save_count);
    CHECK(ble_archive_view->selected_saved_uptime_ms==10000&&ble_archive_view->selected_age_ms==105);
    ble_archive_tap(64,426);CHECK(ble_archive_view->view==BLE_ARCHIVE_UI_DETAIL);
    ble_archive_tap(64,426);CHECK(ble_archive_view->view==BLE_ARCHIVE_UI_LIST);
    ble_archive_tap(64,426);CHECK(!allocation&&current_page==UI_PAGE_FILES);
    load_from_file=false;
}
static void test_c5_response_jobs(void)
{
    static cerberus_ble_archive_t frozen;
    seed_both_responses(0);s3_released=c5_released=false;
    unsigned before=saves;
    ble_archive_save_c5(false);frozen=ble_archive_view->incoming;
    CHECK(frozen.count==64&&ble_archive_view->release_pending&&saves==before);
    for(unsigned i=32;i<64;++i){
        const cerberus_ble_archive_row_t *r=&frozen.rows[i];
        CHECK(r->c5_response.state==CERBERUS_DUAL_BLE_RESPONSE_PRESENT&&
            r->c5_response.receive.callback_length==255&&r->c5_response.raw_length==31);
        CHECK(r->advertisement_length==3&&r->age_upper_at_save_ms==225&&
            r->c5_response_age_upper_at_save_ms==223&&r->c5_response_age_upper_valid);
        CHECK(!r->source_time_valid&&!r->original_source_uptime_ms&&!r->copy_ceiling_ms);
    }
    memset(companion.rows,0,sizeof(companion.rows));companion.published_boot_id=999;
    companion.published_epoch=1000;companion.published_family=1;companion.published_response=false;
    memset(companion.staging,0xFF,sizeof(companion.staging));now_us+=1000000;
    ble_archive_save_c5(false);CHECK(!memcmp(&frozen,&ble_archive_view->incoming,sizeof(frozen)));
    s3_released=true;ble_archive_tick(false);CHECK(saves==before);
    c5_released=true;ble_archive_tick(false);
    CHECK(saves==before+1&&saved_length==27680&&file_length==27680&&!memcmp(&frozen,&saved,sizeof(frozen)));
    complete();CHECK(!memcmp(&frozen,&ble_archive_view->document,sizeof(frozen)));ble_archive_return();

    for(unsigned submitted=0;submitted<2;++submitted){
        seed_both_responses(6);s3_released=c5_released=false;before=saves;
        ble_archive_save_c5(false);frozen=ble_archive_view->incoming;
        memset(companion.rows,0,sizeof(companion.rows));
        if(submitted){s3_released=c5_released=true;ble_archive_tick(false);CHECK(!memcmp(&frozen,&saved,sizeof(frozen)));}
        else CHECK(!memcmp(&frozen,&ble_archive_view->incoming,sizeof(frozen)));
        ble_archive_tap(64,426);CHECK(!allocation&&current_page==UI_PAGE_BLE);
        s3_released=c5_released=true;ble_archive_tick(false);CHECK(saves==before+submitted);
        if(submitted){CHECK(!memcmp(&frozen,&saved,sizeof(frozen)));complete();CHECK(!allocation);}
    }
    seed_both_responses(6);s3_released=c5_released=false;before=saves;
    ble_archive_save_c5(false);frozen=ble_archive_view->incoming;now_us+=12000000;
    ble_archive_tick(false);CHECK(ble_archive_view->error==ESP_ERR_TIMEOUT&&saves==before);
    CHECK(!memcmp(&frozen,&ble_archive_view->incoming,sizeof(frozen)));
    s3_released=c5_released=true;ble_archive_tick(false);CHECK(saves==before);ble_archive_return();
}
static void test_saved_c5_response_reopen(void)
{
    static cerberus_ble_archive_t frozen;
    const char *labels[]={"IBEACON","FIND MY","FLIPPER","FLOCK","META","HC NAME"};
    for(uint8_t family=1;family<=6;++family){
        seed_both_responses(family);dual_ble_ui_detail=true;
        CHECK(cerberus_dual_ble_copy_row(&companion,(uint8_t)(family-1),
            &dual_ble_ui_selected,&dual_ble_ui_selected_provenance));
        s3_released=c5_released=false;unsigned before=saves;
        ble_archive_save_c5(true);frozen=ble_archive_view->incoming;
        CHECK(frozen.count==1&&frozen.rows[0].c5_response.family.family_code==family&&
            frozen.rows[0].c5_response.receive.callback_length==255&&frozen.rows[0].advertisement_length==3);
        CHECK(!strcmp(frozen.rows[0].name,"Frozen C5 row")&&frozen.rows[0].age_upper_at_save_ms==225&&
            frozen.rows[0].c5_response_age_upper_at_save_ms==223);
        memset(&dual_ble_ui_selected.response,0,sizeof(dual_ble_ui_selected.response));
        memset(&dual_ble_ui_selected.family,0,sizeof(dual_ble_ui_selected.family));
        dual_ble_ui_selected.raw[0]=99;dual_ble_ui_selected_provenance.boot_id=999;
        memset(companion.rows,0,sizeof(companion.rows));memset(companion.staging,0xEE,sizeof(companion.staging));
        companion.published_boot_id=999;companion.published_epoch=1000;companion.published_family=1;
        companion.start.mode=CERBERUS_DUAL_BLE_DISCOVERY;companion.published_response=false;
        now_us+=3000000;ble_archive_save_c5(true);
        CHECK(!memcmp(&frozen,&ble_archive_view->incoming,sizeof(frozen))&&saves==before);
        s3_released=c5_released=true;ble_archive_tick(false);complete();
        CHECK(!memcmp(&frozen,&ble_archive_view->document,sizeof(frozen))&&file_length==464);
        ble_archive_return();CHECK(!allocation);

        now_us=0;load_from_file=true;current_page=UI_PAGE_FILES;
        unsigned clocks=clock_reads,stopped=s3_stop_requests,save_count=saves;
        ble_archive_open_file("/MOVED","response.BLE");complete();
        CHECK(!memcmp(&frozen,&ble_archive_view->document,sizeof(frozen)));
        ble_archive_tap(20,160);ble_archive_tap(64,390);
        CHECK(ble_archive_view->view==BLE_ARCHIVE_UI_RAW&&!ble_archive_view->c5_response);
        ble_archive_tap(200,94);CHECK(ble_archive_view->c5_response&&strstr(drawn,"SAVED C5 RESPONSE"));
        CHECK(!memcmp(&ble_archive_view->selected.c5_response,&frozen.rows[0].c5_response,sizeof(frozen.rows[0].c5_response)));
        ble_archive_tap(160,65);CHECK(ble_archive_view->raw_response&&strstr(drawn,"SAVED RSP RX INFO"));
        CHECK(strstr(drawn,"RSP AGE AT SAVE: <= 223 ms")&&strstr(drawn,"SAVED RESPONSE RSSI -47 dBm"));
        ble_archive_tap(250,65);CHECK(ble_archive_view->raw_hints&&strstr(drawn,"SAVED C5 RSP HINTS"));
        CHECK(strstr(drawn,labels[family-1])&&strstr(drawn,"AGE AT SAVE: <= 223 ms"));
        CHECK(strstr(drawn,"MATCHED RESPONSE / ORIGINAL KEPT SEPARATE")&&strstr(drawn,"HISTORICAL / NO LIVE RADIO"));
        CHECK(ble_archive_view->selected.receiver_boot_id==123&&ble_archive_view->selected.scan_epoch==456&&
            ble_archive_view->selected_saved_uptime_ms==10000&&ble_archive_view->selected_age_ms==225);
        ble_archive_tap(40,94);CHECK(!ble_archive_view->c5_response&&strstr(drawn,"NO SUPPORTED FAMILY HINT"));
        CHECK(strstr(drawn,"AGE AT SAVE: <= 225 ms")&&clock_reads==clocks&&s3_stop_requests==stopped&&saves==save_count);
        ble_archive_tap(64,426);ble_archive_tap(64,426);ble_archive_tap(64,426);
        CHECK(!allocation&&current_page==UI_PAGE_FILES);load_from_file=false;
    }
}
static void test_invalid_c5_response_capture(void)
{
    for(unsigned fault=0;fault<5;++fault){
        seed_both_responses(6);dual_ble_ui_detail=true;
        CHECK(cerberus_dual_ble_copy_row(&companion,5,&dual_ble_ui_selected,&dual_ble_ui_selected_provenance));
        if(fault==0)dual_ble_ui_selected.response.reserved_tail=1;
        if(fault==1)dual_ble_ui_selected.response.family.family_code=1;
        if(fault==2)dual_ble_ui_selected.response.age_ms=126;
        if(fault==3)dual_ble_ui_selected.response.receive.sid=8;
        if(fault==4)dual_ble_ui_selected.response.response_sequence=dual_ble_ui_selected.response.original_sequence;
        unsigned before=saves,stopped=s3_stop_requests;
        ble_archive_save_c5(true);
        CHECK(ble_archive_open_error==ESP_ERR_INVALID_RESPONSE&&!ble_archive_view->release_pending&&
            !ble_archive_view->job_pending&&saves==before&&s3_stop_requests==stopped+1&&companion.stop_requested);
        ble_archive_return();
    }
}
static void test_monitor_save_return(void)
{
    /* The real monitor footer captures both retained banks, including a
     * response-only Find My proof, before stopping either owner's intent. */
    seed_both_responses(2);ble_mode=UI_BLE_MODE_FINDMY_MONITOR;monitor_requested=true;
    s3_released=c5_released=false;
    unsigned before=saves, stopped=monitor_stops;
    CHECK(archive_monitor_footer_draw()==ESP_OK&&strstr(drawn,"SAVE BOTH"));
    archive_monitor_footer_tap(220,440);
    CHECK(current_page==UI_PAGE_BLE_ARCHIVE&&ble_archive_return_mode==UI_BLE_MODE_FINDMY_MONITOR);
    CHECK(!monitor_requested&&monitor_stops==stopped+1&&monitor_captured_rows==64);
    CHECK(ble_archive_view->release_pending&&saves==before);
    static cerberus_ble_archive_t frozen;frozen=ble_archive_view->incoming;
    CHECK(frozen.saved_uptime_ms==10000&&frozen.rows[0].source_time_valid);
    CHECK(frozen.rows[32].receiver==CERBERUS_BLE_ARCHIVE_RECEIVER_C5&&
        frozen.rows[32].c5_response.state==CERBERUS_DUAL_BLE_RESPONSE_PRESENT);
    memset(discovery.inventory.entries,0,sizeof(discovery.inventory.entries));
    memset(companion.rows,0,sizeof(companion.rows));companion.published_epoch=999;
    now_us+=2000000;s3_released=true;ble_archive_tick(false);CHECK(saves==before);
    c5_released=true;ble_archive_tick(false);
    CHECK(saves==before+1&&!memcmp(&frozen,&saved,sizeof(frozen)));
    complete();ble_archive_return();
    CHECK(current_page==UI_PAGE_BLE&&ble_mode==UI_BLE_MODE_FINDMY_MONITOR&&!monitor_requested);
    load_from_file=true;ble_archive_open_file("/sdcard/cerberus/ble/B00001.BLE","B00001.BLE");
    complete();CHECK(!memcmp(&frozen,&ble_archive_view->document,sizeof(frozen)));
    ble_archive_return();CHECK(ble_mode==UI_BLE_MODE_FINDMY_MONITOR&&!monitor_requested);

    /* Allocation/capture failure must also clear pre-INFO acquisition intent. */
    seed();ble_mode=UI_BLE_MODE_FINDMY_MONITOR;monitor_requested=true;
    ble_discovery_live=NULL;before=saves;ble_archive_save_c5(false);
    CHECK(ble_archive_open_error==ESP_ERR_NOT_FOUND&&!monitor_requested&&saves==before);
    ble_archive_return();CHECK(ble_mode==UI_BLE_MODE_FINDMY_MONITOR&&!monitor_requested);

    /* Frozen C5 detail retains the monitor destination through the root Back
     * control; it never restarts the ordinary companion on the return. */
    seed_both();dual_ble_ui_detail=dual_ble_ui_from_monitor=true;
    CHECK(archive_root_footer_draw()==ESP_OK&&strstr(drawn,"FIND MY MONITOR"));
    archive_root_footer_tap(40,440);
    CHECK(ble_mode==UI_BLE_MODE_FINDMY_MONITOR&&!monitor_requested);
}

static void arm_pending_info(void)
{
    const cerberus_dual_ble_ops_t ops={.exchange=pending_exchange};
    cerberus_dual_ble_init(&companion,&ops);dual_ble_ui_store=&companion;
    CHECK(cerberus_dual_ble_begin(&companion,CERBERUS_DUAL_BLE_DISCOVERY,NULL,0)==ESP_OK);
    CHECK(companion.enabled&&cerberus_dual_ble_released(&companion));
}
static void test_ordinary_failed_save_back(void)
{
    const ui_ble_mode_t origins[]={UI_BLE_MODE_LIST,UI_BLE_MODE_C5,UI_BLE_MODE_DETAIL};
    const esp_err_t errors[]={ESP_ERR_NOT_FOUND,ESP_ERR_INVALID_RESPONSE,ESP_ERR_INVALID_STATE,ESP_ERR_NO_MEM};
    for(unsigned origin=0;origin<3;++origin)for(unsigned fault=0;fault<4;++fault){
        seed();ble_mode=origins[origin];arm_pending_info();
        ordinary_requested=ordinary_selection_pending=dual_ble_ui_selection_pending=true;
        dual_ble_ui_painted_count=4;
        if(fault==0)ble_discovery_live=NULL;
        else if(fault==1)discovery.inventory.entries[0].record.advertisement.address[0]^=1;
        else if(fault==2)now_us=-1;
        else {ble_discovery_live=NULL;allocation_fails=true;}
        unsigned before=saves,stopped=s3_stop_requests,exchanges=pending_exchanges;
        ble_archive_save(BLE_ARCHIVE_BOTH);
        CHECK(ble_archive_open_error==errors[fault]&&saves==before&&s3_stop_requests==stopped+1);
        CHECK(!ordinary_requested&&!ordinary_selection_pending&&!dual_ble_ui_selection_pending&&!dual_ble_ui_painted_count);
        CHECK(!companion.enabled&&companion.stop_requested&&cerberus_dual_ble_released(&companion));
        CHECK(!ble_archive_view||(!ble_archive_view->release_pending&&!ble_archive_view->job_pending));
        /* Back before any normal owner tick cannot leave a deferred INFO arm. */
        ble_archive_tap(64,426);
        CHECK(current_page==UI_PAGE_BLE&&ble_mode==origins[origin]&&!allocation);
        cerberus_dual_ble_tick(&companion,10000);cerberus_dual_ble_tick(&companion,30000);
        ble_archive_tick(false);
        CHECK(pending_exchanges==exchanges&&!companion.enabled&&!companion.start.job_id&&saves==before);
        CHECK(!ordinary_requested&&!ordinary_selection_pending&&!dual_ble_ui_selection_pending);
    }
}
static void test_ordinary_capture_then_cancel(void)
{
    static cerberus_ble_archive_t frozen;
    for(unsigned without_s3=0;without_s3<2;++without_s3){
        seed_both_responses(2);if(without_s3)ble_discovery_live=NULL;
        ordinary_requested=ordinary_selection_pending=dual_ble_ui_selection_pending=true;
        dual_ble_ui_painted_count=4;mutate_live_on_stop=true;
        /* Exact radio leases stay held independently of canceled intent. */
        companion.start.job_id=91;companion.start.boot_id=123;companion.enabled=true;
        s3_released=c5_released=false;unsigned before=saves,stopped=s3_stop_requests;
        ble_archive_save_c5(false);
        CHECK(ble_archive_view->release_pending&&saves==before&&s3_stop_requests==stopped+1);
        CHECK(ordinary_captured_rows==(without_s3?32:64));frozen=ble_archive_view->incoming;
        CHECK(!ordinary_requested&&!ordinary_selection_pending&&!dual_ble_ui_selection_pending);
        CHECK(!companion.enabled&&companion.stop_requested&&companion.start.job_id==91);
        CHECK(!discovery.inventory.count&&!companion.rows[0].raw_length);
        CHECK(frozen.rows[frozen.count-1].c5_response.state==CERBERUS_DUAL_BLE_RESPONSE_PRESENT);
        s3_released=true;ble_archive_tick(false);CHECK(saves==before);
        CHECK(!memcmp(&frozen,&ble_archive_view->incoming,sizeof(frozen)));
        c5_released=true;ble_archive_tick(false);CHECK(saves==before+1&&!memcmp(&frozen,&saved,sizeof(frozen)));
        complete();ble_archive_return();
        CHECK(!ordinary_requested&&!dual_ble_ui_selection_pending&&ble_mode==UI_BLE_MODE_C5);
    }
    /* A pending selection canceled by Save never applies after a timeout or
       after the user returns before either lease has finished releasing. */
    seed_both();ordinary_requested=ordinary_selection_pending=dual_ble_ui_selection_pending=true;
    s3_released=c5_released=false;unsigned before=saves;
    ble_archive_save_c5(false);now_us+=13000000;ble_archive_tick(false);
    CHECK(ble_archive_open_error==ESP_ERR_TIMEOUT&&!ble_archive_view->release_pending&&saves==before);
    ble_archive_return();CHECK(!ordinary_requested&&!ordinary_selection_pending&&!dual_ble_ui_selection_pending);
    s3_released=c5_released=true;ble_archive_tick(false);CHECK(saves==before&&ble_mode==UI_BLE_MODE_C5);
}
static void test_archive_cleanup_origin(void)
{
    const ui_ble_mode_t modes[]={UI_BLE_MODE_MENU,UI_BLE_MODE_FOX_HUNT,UI_BLE_MODE_ANALYZER,
        UI_BLE_MODE_C5,UI_BLE_MODE_DETAIL,UI_BLE_MODE_LIST,UI_BLE_MODE_LIST};
    for(unsigned i=0;i<sizeof(modes)/sizeof(modes[0]);++i){
        seed_both();ble_mode=modes[i];ordinary_requested=true;
        if(i==3)dual_ble_ui_from_monitor=true;
        if(i==4)ble_selected_from_monitor=true;
        if(i==5)current_page=UI_PAGE_FILES;
        if(i==6)current_page=UI_PAGE_MAIN;
        const ui_page_t origin=current_page;
        companion.start.mode=CERBERUS_DUAL_BLE_TRACK;companion.start.job_id=92;
        companion.start.boot_id=123;companion.enabled=true;
        unsigned stopped=s3_stop_requests,mon_stopped=monitor_stops,before=saves;
        ble_archive_save(BLE_ARCHIVE_BOTH);
        CHECK(ble_archive_view->release_pending&&saves==before);
        CHECK(s3_stop_requests==stopped&&monitor_stops==mon_stopped);
        CHECK(companion.enabled&&!companion.stop_requested&&companion.start.job_id==92);
        ble_archive_return();CHECK(current_page==origin&&ble_mode==modes[i]);
        CHECK(companion.enabled&&!companion.stop_requested&&ordinary_requested);
    }
}

int main(void)
{
    test_capture();test_jobs();test_accepted_job_rejected_by_view();test_complete_without_result();
    test_both_and_c5_footer();test_release_wait();test_response_capture();test_receive_metadata_jobs();
    test_invalid_family_capture();
    test_saved_family_reopen();
    test_c5_response_jobs();test_saved_c5_response_reopen();test_invalid_c5_response_capture();
    test_monitor_save_return();
    test_ordinary_failed_save_back();test_ordinary_capture_then_cancel();test_archive_cleanup_origin();
    ble_archive_dispose();CHECK(frees+rejected_allocations==allocations);
    printf("%u archive capture/controller checks passed; saves=%u loads=%u copies=%u releases=%u; host viewer=%zu document=%zu row=%zu bytes.\n",
        checks,saves,loads,copies,releases,sizeof(ble_archive_ui_store_t),sizeof(cerberus_ble_archive_t),sizeof(cerberus_ble_archive_row_t));return 0;
}
