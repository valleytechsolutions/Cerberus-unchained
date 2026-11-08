#include "airtag_adapters.h"
#include "ble_archive_ui.h"
#include "ble_archive_io.h"
#include "ble_archive_response_fixture.h"
#define ESP_ERROR_CHECK(x) CHECK((x)==ESP_OK)
#define MALLOC_CAP_SPIRAM 1U
#define MALLOC_CAP_8BIT 2U
#define UI_GOOD UI_TEXT
typedef enum { UI_PAGE_FILES, UI_PAGE_BLE_ARCHIVE, UI_PAGE_BLE, UI_PAGE_MAIN } ui_page_t;
typedef enum { UI_BLE_MODE_MENU, UI_BLE_MODE_DETAIL } ui_ble_mode_t;
static ui_page_t current_page=UI_PAGE_FILES;
static ui_ble_mode_t ble_mode=UI_BLE_MODE_MENU;
static ble_archive_ui_store_t native_view;
static ble_archive_ui_store_t *ble_archive_view;
static bool ble_archive_menu;
static unsigned native_disposes, allocations, frees, loads, saves, exports, copies, releases, cancels, draws;
static bool oom;
static unsigned lookup_entries;
static bool saved_ble_lookup_holds_files(void) { return false; }
static void saved_ble_lookup_open(bool json) { CHECK(json); ++lookup_entries; }
static void *allocated;
static void *heap_caps_calloc(size_t count,size_t size,unsigned caps)
{ CHECK(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));CHECK(!allocated);++allocations;
  if(oom)return NULL;allocated=calloc(count,size);CHECK(allocated);return allocated; }
static void heap_caps_free(void *p) { CHECK(p==allocated);free(p);allocated=NULL;++frees; }
static void ble_archive_dispose(void) { ++native_disposes;ble_archive_view=NULL;ble_archive_menu=false; }
static esp_err_t draw_detail_page(void),draw_current_page(void);
static cerberus_ble_archive_io_status_t io;
static cerberus_ble_archive_t frozen_native;
static cerberus_airtag_json_t worker_doc, expected_doc;
static uint8_t encoded[CERBERUS_AIRTAG_JSON_MAX_BYTES];
static size_t encoded_length;
static uint64_t sequence, canceled_job, released_job;
static esp_err_t start_error, copy_error;
static char copied_path[512],copied_name[256];
static esp_err_t reserve(cerberus_ble_archive_io_operation_t operation,uint64_t *job)
{
    *job=0;if(start_error)return start_error;
    CHECK(!io.worker_active);io=(cerberus_ble_archive_io_status_t){.job_id=++sequence,
        .format=CERBERUS_BLE_ARCHIVE_IO_FORMAT_AIRTAG_JSON,.operation=operation,
        .state=CERBERUS_BLE_ARCHIVE_IO_STARTING,.worker_active=true};*job=sequence;return ESP_OK;
}
esp_err_t cerberus_ble_archive_io_load_airtag_json(const char *path,const char *name,uint64_t *job)
{ ++loads;snprintf(copied_path,sizeof(copied_path),"%s",path);snprintf(copied_name,sizeof(copied_name),"%s",name);
  return reserve(CERBERUS_BLE_ARCHIVE_IO_LOAD_AIRTAG_JSON,job); }
esp_err_t cerberus_ble_archive_io_save_airtag_json(const cerberus_airtag_json_t *doc,uint64_t *job)
{ ++saves;esp_err_t e=reserve(CERBERUS_BLE_ARCHIVE_IO_SAVE_AIRTAG_JSON,job);if(!e)worker_doc=*doc;return e; }
esp_err_t cerberus_ble_archive_io_export_airtag_json(const cerberus_ble_archive_t *doc,uint64_t *job)
{ ++exports;esp_err_t e=reserve(CERBERUS_BLE_ARCHIVE_IO_EXPORT_AIRTAG_JSON,job);if(!e)frozen_native=*doc;return e; }
esp_err_t cerberus_ble_archive_io_copy_airtag_json(uint64_t job,cerberus_airtag_json_t *doc)
{ ++copies;CHECK(job==io.job_id&&!io.worker_active&&io.result_ready);
  if(copy_error)return copy_error;*doc=worker_doc;return ESP_OK; }
void cerberus_ble_archive_io_get_status(cerberus_ble_archive_io_status_t *out) { *out=io; }
void cerberus_ble_archive_io_cancel(uint64_t job) { ++cancels;canceled_job=job;if(job==io.job_id)io.cancel_requested=true; }
void cerberus_ble_archive_io_release(uint64_t job) { ++releases;released_job=job;if(job==io.job_id)io.result_ready=false; }
#include "airtag_json_live_ui.h"
#include "airtag_archive_routes.h"
static esp_err_t draw_detail_page(void) { ++draws;drawn_size=calls=fail_after=0;drawn[0]=0;return archive_page_draw(); }
static esp_err_t draw_current_page(void) { return current_page==UI_PAGE_BLE_ARCHIVE?draw_detail_page():ESP_OK; }
enum { UI_FILE_MODE_LIST, UI_FILE_MODE_DELETE_CONFIRM, UI_FILE_MODE_PACKAGE_INFO, UI_FILE_OPERATION_RENAME };
static int file_mode;
static struct { bool is_directory; char name[256]; } file_selected_entry;
static char file_selected_path[512],file_action_message[64];
static uint16_t file_action_color;
static int file_package_info;
static esp_err_t file_package_error;
#define CERBERUS_STORAGE_FIRMWARE_PATH "/CERBERUS/FIRMWARE"
static unsigned rename_calls,clipboard_calls,inspect_calls;
static void begin_file_name_input(int op,const char *name) { CHECK(op==UI_FILE_OPERATION_RENAME&&name);++rename_calls; }
static void stage_file_clipboard(bool move) { (void)move;++clipboard_calls; }
static esp_err_t cerberus_storage_inspect_firmware_package(const char *name,int *out)
{ CHECK(name&&out);++inspect_calls;return ESP_OK; }
/* Explicit Wi-Fi parser choices have their actual route/renderer suite. The
 * existing AirTag fixture must never dispatch into either unrelated format. */
static bool wifi_interchange_can_open(const char *name,bool ap_json) { (void)name;(void)ap_json;return false; }
static void wifi_interchange_open_file(const char *path,const char *name,bool ap_json)
{ (void)path;(void)name;(void)ap_json;CHECK(false); }
#include "airtag_file_routes.h"

static void reset(void)
{
    airtag_json_dispose();ble_archive_view=NULL;memset(&native_view,0,sizeof(native_view));
    memset(&io,0,sizeof(io));memset(&worker_doc,0,sizeof(worker_doc));
    current_page=UI_PAGE_FILES;ble_mode=UI_BLE_MODE_DETAIL;oom=privacy_mask=false;
    start_error=copy_error=ESP_OK;drawn_size=calls=fail_after=0;drawn[0]=0;
}
static void seed_json(unsigned count,unsigned length)
{
    memset(&worker_doc,0,sizeof(worker_doc));worker_doc.count=(uint8_t)count;
    CHECK(count<=64&&count*length<=8192);
    for(unsigned i=0;i<count;++i){
        cerberus_airtag_json_record_t *r=&worker_doc.records[i];
        r->address[0]=0x56;r->address[1]=0x34;r->address[2]=0x12;r->address[5]=0xC0;
        r->offset=(uint16_t)(i*length);r->length=(uint16_t)length;
        for(unsigned j=0;j<length;++j)worker_doc.payload[r->offset+j]=(uint8_t)(j+i);
    }
    worker_doc.payload_used=(uint16_t)(count*length);
    CHECK(cerberus_airtag_json_validate(&worker_doc)==CERBERUS_AIRTAG_JSON_OK);
}
static void finish(void)
{
    CHECK(io.worker_active);
    if(io.operation==CERBERUS_BLE_ARCHIVE_IO_EXPORT_AIRTAG_JSON){
        cerberus_airtag_json_result_t result=cerberus_airtag_json_from_archive(&frozen_native,&worker_doc,&io.export_stats);
        if(result!=CERBERUS_AIRTAG_JSON_OK){
            io.error=result==CERBERUS_AIRTAG_JSON_LIMIT?ESP_ERR_INVALID_SIZE:
                result==CERBERUS_AIRTAG_JSON_NO_ELIGIBLE?ESP_ERR_NOT_FOUND:ESP_ERR_INVALID_RESPONSE;
            io.state=CERBERUS_BLE_ARCHIVE_IO_ERROR;io.worker_active=false;return;
        }
    }
    CHECK(cerberus_airtag_json_encode(&worker_doc,encoded,sizeof(encoded),&encoded_length)==CERBERUS_AIRTAG_JSON_OK);
    memset(&worker_doc,0,sizeof(worker_doc));
    CHECK(cerberus_airtag_json_decode(encoded,encoded_length,&worker_doc)==CERBERUS_AIRTAG_JSON_OK);
    io.worker_active=false;io.result_ready=true;io.state=CERBERUS_BLE_ARCHIVE_IO_COMPLETE;
    snprintf(io.file_name,sizeof(io.file_name),"A00001.LOG");io.count=worker_doc.count;io.bytes=(uint32_t)encoded_length;
}
static void preview(const char *dir,const char *name)
{
    char path[1024];snprintf(path,sizeof(path),"%s/%s.ppm",dir,name);FILE *f=fopen(path,"wb");CHECK(f);
    fprintf(f,"P6\n320 480\n255\n");
    for(size_t i=0;i<sizeof(screen)/sizeof(*screen);++i){uint16_t p=screen[i];
        uint8_t rgb[]={(uint8_t)((p>>11)*255/31),(uint8_t)(((p>>5)&63)*255/63),(uint8_t)((p&31)*255/31)};
        assert(fwrite(rgb,1,3,f)==3);}
    CHECK(!fclose(f));
}
static void open_loaded(unsigned count,unsigned length)
{
    reset();airtag_json_open_file("/logs","Airtags_0.log");CHECK(airtag_json_view&&airtag_json_view->job_pending);
    seed_json(count,length);expected_doc=worker_doc;finish();CHECK(airtag_json_tick(false));
    CHECK(airtag_json_view->has_document&&!airtag_json_view->job_pending);
    CHECK(!memcmp(&airtag_json_view->document,&expected_doc,sizeof(expected_doc)));
}
static void file_routes(void)
{
    reset();const char *yes[]={"Airtags_0.log","x.JSON","a.LoG","one.json"};
    const char *no[]={".log","a.logx","x.BLE","x.json.tmp","","a","a.txt"};
    for(unsigned i=0;i<sizeof(yes)/sizeof(*yes);++i)CHECK(airtag_json_is_file(yes[i]));
    for(unsigned i=0;i<sizeof(no)/sizeof(*no);++i)CHECK(!airtag_json_is_file(no[i]));CHECK(!airtag_json_is_file(NULL));
    strcpy(file_selected_path,"/logs");strcpy(file_selected_entry.name,"Airtags_0.log");
    CHECK(draw_file_actions()==ESP_OK);CHECK(strstr(drawn,"OPEN AIRTAG JSON"));
    unsigned before=loads;handle_file_actions_tap(20,270);CHECK(loads==before);
    handle_file_actions_tap(60,270);CHECK(loads==before+1&&current_page==UI_PAGE_BLE_ARCHIVE);
    strcpy(file_selected_path,"/changed");strcpy(file_selected_entry.name,"changed.log");
    CHECK(!strcmp(copied_path,"/logs")&&!strcmp(copied_name,"Airtags_0.log"));
    uint64_t job=io.job_id;airtag_json_tap(50,440);CHECK(!airtag_json_is_open()&&current_page==UI_PAGE_FILES);
    CHECK(canceled_job==job&&released_job==job);
    reset();strcpy(file_selected_path,CERBERUS_STORAGE_FIRMWARE_PATH);strcpy(file_selected_entry.name,"x.LOG");
    before=inspect_calls;handle_file_actions_tap(60,270);CHECK(inspect_calls==before+1&&file_mode==UI_FILE_MODE_PACKAGE_INFO);
    CHECK(!airtag_json_is_open());
    file_selected_entry.is_directory=true;strcpy(file_selected_path,"/logs");before=loads;
    handle_file_actions_tap(60,270);CHECK(loads==before);file_selected_entry.is_directory=false;
    handle_file_actions_tap(20,130);handle_file_actions_tap(200,130);CHECK(rename_calls&&clipboard_calls);
}
static void list_raw_privacy(const char *dir)
{
    open_loaded(64,128);CHECK(airtag_json_view->paint_valid&&airtag_json_view->painted_count==4);
    CHECK(strstr(drawn,"64/64 RECORDS / 8192/8192 B"));preview(dir,"json-list-64");
    for(unsigned p=1;p<16;++p){airtag_json_tap(200,395);CHECK(airtag_json_view->page==p);}
    CHECK(airtag_json_view->painted_indexes[3]==63);airtag_json_tap(20,310);
    CHECK(airtag_json_view->selected_index==63&&airtag_json_view->selected_valid);
    CHECK(strstr(drawn,"TYPE / SOURCE / TIME / RSSI UNKNOWN"));
    CHECK(strstr(drawn,"3F 40 41 42"));preview(dir,"json-raw-128");
    privacy_mask=true;CHECK(draw_detail_page()==ESP_OK);CHECK(strstr(drawn,"RAW PAYLOAD HIDDEN"));
    CHECK(!strstr(drawn,"C0:00:00:12:34:56")&&!strstr(drawn,"3F 40 41 42"));preview(dir,"json-raw-masked");
    privacy_mask=false;airtag_json_tap(50,440);CHECK(airtag_json_view->view==AIRTAG_JSON_LIST);
    CHECK(airtag_json_view->page==15);airtag_json_tap(50,440);CHECK(!allocated);
    open_loaded(1,8192);airtag_json_tap(20,160);
    for(unsigned p=1;p<64;++p){airtag_json_tap(200,395);CHECK(airtag_json_view->raw_page==p);}
    CHECK(strstr(drawn,"OFFSET 8064 / PAGE 64/64"));CHECK(strstr(drawn,"F0 F1 F2 F3"));preview(dir,"json-raw-last-page");
    airtag_json_tap(200,395);CHECK(airtag_json_view->raw_page==63);airtag_json_tap(30,395);CHECK(airtag_json_view->raw_page==62);
    open_loaded(2,31);airtag_json_tap(20,160);CHECK(strstr(drawn,"31 BYTES"));preview(dir,"json-raw-31");
    open_loaded(1,0);airtag_json_tap(20,160);CHECK(strstr(drawn,"EMPTY PAYLOAD RETAINED"));
    open_loaded(0,0);CHECK(strstr(drawn,"EMPTY JSON ARRAY"));preview(dir,"json-empty");
    CHECK(airtag_json_ui_tap(airtag_json_view,20,160)==AIRTAG_JSON_NONE);
}
static void atomicity_and_lifetime(void)
{
    open_loaded(4,32);unsigned before=copies;uint64_t job=airtag_json_view->job_id;
    /* Failed full paint never authorizes selection. */
    drawn_size=calls=0;drawn[0]=0;fail_after=8;
    CHECK(airtag_json_draw()==ESP_FAIL);CHECK(!airtag_json_view->paint_valid);
    CHECK(airtag_json_ui_tap(airtag_json_view,20,160)==AIRTAG_JSON_NONE);fail_after=0;
    CHECK(draw_detail_page()==ESP_OK);airtag_json_tap(20,160);
    const cerberus_airtag_json_record_t frozen=airtag_json_view->selected;
    memset(&worker_doc,0,sizeof(worker_doc));CHECK(!memcmp(&airtag_json_view->selected,&frozen,sizeof(frozen)));
    CHECK(airtag_json_view->document.payload[1]==1);CHECK(copies==before);
    airtag_json_tap(50,440);airtag_json_tap(200,440);CHECK(airtag_json_view->job_pending);
    CHECK(!airtag_json_view->paint_valid&&!airtag_json_view->selected_valid);
    CHECK(io.job_id>job);job=io.job_id;expected_doc=worker_doc;
    finish();copy_error=ESP_ERR_TIMEOUT;CHECK(airtag_json_tick(false));CHECK(airtag_json_view->has_document);
    CHECK(!memcmp(&expected_doc,&airtag_json_view->document,sizeof(expected_doc)));
    CHECK(airtag_json_view->view==AIRTAG_JSON_ERROR&&released_job==job);
    CHECK(!strcmp(airtag_json_view->file_name,"A00001.LOG"));
    airtag_json_tap(100,395);CHECK(airtag_json_view->view==AIRTAG_JSON_LIST);
    copy_error=0;airtag_json_tap(200,440);job=io.job_id;finish();
    io.format=CERBERUS_BLE_ARCHIVE_IO_FORMAT_BLE;before=copies;CHECK(airtag_json_tick(false));CHECK(copies==before);
    CHECK(airtag_json_view->error==ESP_ERR_INVALID_RESPONSE);
    airtag_json_tap(100,395);airtag_json_tap(200,440);job=io.job_id;
    ++io.job_id;io.worker_active=false;io.state=CERBERUS_BLE_ARCHIVE_IO_COMPLETE;io.result_ready=true;
    CHECK(airtag_json_tick(false)&&airtag_json_view->job_pending);CHECK(copies==before);
    before=cancels;CHECK(airtag_json_tick(true));CHECK(!allocated&&!airtag_json_is_open());
    CHECK(cancels==before+1&&canceled_job==job&&current_page==UI_PAGE_FILES);
    CHECK(!airtag_json_tick(false));
    reset();oom=true;before=loads;airtag_json_open_file("/","x.LOG");CHECK(!airtag_json_view&&loads==before);
    CHECK(strstr(drawn,"ESP_ERR_NO_MEM")&&!strstr(drawn,"FAILED SAVE"));airtag_json_tap(50,440);CHECK(current_page==UI_PAGE_FILES);
    reset();airtag_json_open_file("/","x.LOG");job=io.job_id;current_page=UI_PAGE_MAIN;
    CHECK(!airtag_json_tick(false)&&!allocated&&released_job==job&&current_page==UI_PAGE_MAIN);
    reset();start_error=ESP_ERR_INVALID_STATE;airtag_json_open_file("/","x.LOG");CHECK(airtag_json_view->view==AIRTAG_JSON_ERROR);
    CHECK(!airtag_json_view->job_pending);airtag_json_tap(50,440);
}
static void seed_native(void)
{
    memset(&native_view,0,sizeof(native_view));native_view.document.count=1;native_view.document.saved_uptime_ms=1000;
    cerberus_ble_archive_row_t *r=&native_view.document.rows[0];
    r->receiver=CERBERUS_BLE_ARCHIVE_RECEIVER_C5;r->address[0]=1;r->address[5]=0xC0;r->address_type=1;
    r->rssi=-42;r->rssi_valid=true;r->rssi_kind=CERBERUS_BLE_ARCHIVE_RSSI_LATEST_ADV;
    r->manufacturer_id=0xffff;r->scan_epoch_valid=true;r->scan_epoch=99;r->receiver_boot_id=123;
    r->age_upper_valid=true;r->age_upper_at_save_ms=10;
    /* Compact original independently qualifies, followed by a complete matched
     * response. Both have genuine saved source proof; JSON drops that proof. */
    const uint8_t bytes[31]={30,0xff,0x4c,0,0x12,0x19};
    memcpy(r->advertisement,bytes,sizeof(bytes));r->advertisement_length=sizeof(bytes);r->advertisement_event_type=0;
    r->receive=(cerberus_dual_ble_receive_metadata_t){.source_kind=CERBERUS_DUAL_BLE_RECEIVE_LEGACY,
        .callback_length=sizeof(bytes),.sid=255,.data_status=255,.ad_validation=CERBERUS_DUAL_BLE_AD_VALID};
    cerberus_dual_ble_response_wire_t *rsp=&r->c5_response;
    rsp->version=1;rsp->state=CERBERUS_DUAL_BLE_RESPONSE_PRESENT;rsp->raw_length=4;rsp->event_type=4;
    rsp->rssi=127;rsp->age_ms=UINT32_MAX;rsp->original_sequence=9;rsp->response_sequence=10;rsp->interval_us=1000;
    rsp->receive=r->receive;rsp->receive.callback_length=4;memcpy(rsp->raw,(uint8_t[]){3,3,0x44,0xfd},4);
    cerberus_dual_ble_family_evidence_t evidence;
    CHECK(cerberus_dual_ble_family_parse_ad(rsp->raw,4,r->address,r->address_type,&evidence));
    CHECK(cerberus_dual_ble_response_family_encode(&evidence,0,&rsp->family));
    CHECK(cerberus_ble_archive_validate(&native_view.document));
    native_view.has_document=true;native_view.view=BLE_ARCHIVE_UI_LIST;ble_archive_view=&native_view;
    current_page=UI_PAGE_BLE_ARCHIVE;ble_archive_menu=false;
}
static void export_workflow(const char *dir)
{
    reset();seed_native();CHECK(draw_detail_page()==ESP_OK);CHECK(strstr(drawn,"EXPORT FINDMY JSON"));
    unsigned before=exports;archive_page_tap(200,440);CHECK(airtag_json_view->view==AIRTAG_JSON_CONFIRM&&exports==before);
    CHECK(strstr(drawn,"LOSES SOURCE, ADDRESS TYPE, TIME, RSSI"));preview(dir,"json-export-confirm");
    airtag_json_tap(50,440);CHECK(!allocated&&ble_archive_view==&native_view&&current_page==UI_PAGE_BLE_ARCHIVE);
    archive_page_tap(200,440);airtag_json_tap(200,440);CHECK(exports==before+1&&io.worker_active);
    memset(native_view.document.rows[0].advertisement,0,sizeof(native_view.document.rows[0].advertisement));
    finish();CHECK(airtag_json_tick(false));CHECK(airtag_json_view->has_document&&airtag_json_view->document.count==2);
    CHECK(airtag_json_view->document.payload[4]==0x12&&airtag_json_view->stats.eligible==2);
    CHECK(airtag_json_view->document.records[1].offset==31&&airtag_json_view->document.payload[33]==0x44);
    CHECK(!memcmp(airtag_json_view->document.records[0].address,airtag_json_view->document.records[1].address,6));
    preview(dir,"json-export-result");
    airtag_json_tap(200,440);CHECK(!airtag_json_view->exporting&&airtag_json_view->saving);
    CHECK(strstr(drawn,"WRITING JSON COPY")&&!strstr(drawn,"EXPORTING"));
    finish();CHECK(airtag_json_tick(false));CHECK(airtag_json_view->document.count==2);
    CHECK(!strstr(drawn,"PACKETS 0")&&strstr(drawn,"FILE ORDER / DUPLICATES PRESERVED"));
    expected_doc=airtag_json_view->document;airtag_json_tap(200,440);
    io.worker_active=false;io.state=CERBERUS_BLE_ARCHIVE_IO_ERROR;io.error=ESP_FAIL;
    strcpy(io.file_name,"A00002.TMP");CHECK(airtag_json_tick(false));
    CHECK(strstr(drawn,"A00002.TMP")&&strstr(drawn,"FAILED SAVE"));
    CHECK(!memcmp(&expected_doc,&airtag_json_view->document,sizeof(expected_doc)));
    CHECK(!strcmp(airtag_json_view->file_name,"A00001.LOG"));
    airtag_json_tap(100,395);CHECK(airtag_json_view->view==AIRTAG_JSON_LIST);
    airtag_json_tap(50,440);CHECK(ble_archive_view==&native_view);
    /* Eligibility failures leave the native snapshot untouched and show exact
     * worker packet statistics, not a silently truncated success. */
    reset();seed_native();native_view.document.rows[0].advertisement[4]=0x13;
    memset(&native_view.document.rows[0].c5_response,0,sizeof(native_view.document.rows[0].c5_response));
    archive_page_tap(200,440);airtag_json_tap(200,440);finish();CHECK(airtag_json_tick(false));
    CHECK(airtag_json_view->error==ESP_ERR_NOT_FOUND&&strstr(drawn,"NO COMPLETE ELIGIBLE"));preview(dir,"json-no-eligible");
    airtag_json_tap(50,440);CHECK(native_view.document.rows[0].advertisement[4]==0x13);
    reset();seed_native();archive_page_tap(200,440);airtag_json_tap(200,440);
    io.worker_active=false;io.state=CERBERUS_BLE_ARCHIVE_IO_ERROR;io.error=ESP_ERR_INVALID_SIZE;
    io.export_stats=(cerberus_airtag_json_export_stats_t){.eligible=65,.filtered=32,.incomplete=31};
    CHECK(airtag_json_tick(false));CHECK(strstr(drawn,"EXPORT LIMIT / NO PARTIAL FILE"));
    CHECK(strstr(drawn,"ELIGIBLE 65 / FILTERED 32 / INCOMPLETE 31"));preview(dir,"json-limit");
    airtag_json_tap(50,440);CHECK(ble_archive_view==&native_view);
}
int main(int argc,char **argv)
{
    CHECK(argc==2);file_routes();list_raw_privacy(argv[1]);atomicity_and_lifetime();export_workflow(argv[1]);reset();
    CHECK(!allocated);printf("AirTag JSON UI/routes: %u checks PASS; viewer=%zu, model=%zu bytes.\n",checks,
        sizeof(airtag_json_ui_store_t),sizeof(cerberus_airtag_json_t));return 0;
}
