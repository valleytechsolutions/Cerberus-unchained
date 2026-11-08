#include "interchange_framebuffer.h"
#include "wifi_marauder.h"
#include "ble_archive_io.h"
#define UI_BACKGROUND 0x0042
#define UI_PANEL 0x08A4
#define UI_MUTED 0x7CB5
#define UI_TEXT 0xE77F
#define UI_BAD 0xF471
#define UI_GOOD UI_TEXT
#define TAG "interchange-ui"
#define MALLOC_CAP_SPIRAM 1U
#define MALLOC_CAP_8BIT 2U
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); abort(); } } while (0)
#define ESP_ERROR_CHECK(x) CHECK((x)==ESP_OK)
static bool privacy_mask,oom;
static bool privacy_addresses_masked(void) { return privacy_mask; }
static void cerberus_privacy_format_wifi_identifier(const uint8_t *a,bool mask,char *out,size_t n)
{ if(mask)snprintf(out,n,"XX:XX:XX:XX:XX:XX");else snprintf(out,n,"%02X:%02X:%02X:%02X:%02X:%02X",a[0],a[1],a[2],a[3],a[4],a[5]); }
static const char *wifi_security_text(uint8_t s) { return s?"WPA2":"OPEN"; }
static const char *esp_err_to_name(esp_err_t e)
{ return e==ESP_ERR_NO_MEM?"ESP_ERR_NO_MEM":e==ESP_ERR_INVALID_RESPONSE?"ESP_ERR_INVALID_RESPONSE":
    e==ESP_ERR_NOT_SUPPORTED?"ESP_ERR_NOT_SUPPORTED":e==ESP_ERR_INVALID_SIZE?"ESP_ERR_INVALID_SIZE":"ESP_FAIL"; }
static esp_err_t draw_centered(uint16_t y,const char *s,uint16_t color,uint8_t scale)
{ CHECK(strlen(s)*6U*scale<=320U);return cerberus_display_draw_text((320U-strlen(s)*6U*scale)/2U,y,s,color,UI_BACKGROUND,scale); }
static esp_err_t draw_header(const char *s) { return draw_centered(20,s,UI_TEXT,2); }
static esp_err_t button(uint16_t x,uint16_t y,uint16_t w,uint16_t h,const char *s,uint8_t scale)
{ CHECK(strlen(s)*6U*scale<=w);esp_err_t e=cerberus_display_fill_rect(x,y,w,h,UI_PANEL);
    return e?e:cerberus_display_draw_text(x+(w-strlen(s)*6U*scale)/2U,y+(h-7U*scale)/2U,s,UI_TEXT,UI_PANEL,scale); }
static esp_err_t draw_button(uint16_t x,uint16_t y,uint16_t w,uint16_t h,const char *s) { return button(x,y,w,h,s,2); }
static esp_err_t draw_small_button(uint16_t x,uint16_t y,uint16_t w,uint16_t h,const char *s) { return button(x,y,w,h,s,1); }
#include "wifi_archive_ui.h"
typedef enum { UI_PAGE_FILES,UI_PAGE_WIFI_ARCHIVE,UI_PAGE_MAIN,UI_PAGE_SSID_LISTS } ui_page_t;
static ui_page_t current_page=UI_PAGE_FILES;
static ui_page_t wifi_archive_return_page=UI_PAGE_FILES;
static int wifi_mode,wifi_archive_return_mode;
static wifi_archive_ui_store_t native_view,*wifi_archive_view;
static unsigned allocations,frees,loads,saves,exports,copies,cancels,releases,draws,native_disposes,native_closes;
static void *allocated;
static void *heap_caps_calloc(size_t n,size_t size,unsigned caps)
{ CHECK(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));CHECK(!allocated);++allocations;if(oom)return NULL;
    allocated=calloc(n,size);CHECK(allocated);return allocated; }
static void heap_caps_free(void *p) { CHECK(p==allocated);free(p);allocated=NULL;++frees; }
/* Native allocation/close boundaries are counted; the actual native renderer,
 * tap mapping and production overlay branches are compiled below. */
static void wifi_archive_dispose(void) { ++native_disposes;wifi_archive_view=NULL; }
static void wifi_archive_close(bool redraw) { (void)redraw;++native_closes;wifi_archive_dispose();current_page=UI_PAGE_FILES; }
static esp_err_t draw_detail_page(void),draw_current_page(void);
static cerberus_ble_archive_io_status_t io;
static cerberus_wifi_archive_t frozen_native;
static cerberus_wifi_marauder_ap_t worker_ap,expected_ap;
static cerberus_wifi_marauder_ssid_t worker_ssid,expected_ssid;
static cerberus_wifi_marauder_ssid_t imported_ssid;
static unsigned ssid_imports;
static esp_err_t ssid_import_error;
static ui_page_t ssid_import_return_page;
static esp_err_t ssid_lists_import_document(const cerberus_wifi_marauder_ssid_t *document,ui_page_t return_page)
{
    ++ssid_imports;CHECK(document&&allocated&&cerberus_wifi_marauder_ssid_validate(document)==CERBERUS_WIFI_MARAUDER_OK);
    if(ssid_import_error)return ssid_import_error;
    imported_ssid=*document;ssid_import_return_page=return_page;current_page=UI_PAGE_SSID_LISTS;return ESP_OK;
}
static uint8_t encoded[CERBERUS_WIFI_MARAUDER_AP_MAX_BYTES];
static size_t encoded_length;
static uint64_t sequence,canceled_job,released_job;
static esp_err_t start_error,copy_error;
static char copied_path[512],copied_name[256];
static esp_err_t reserve(cerberus_ble_archive_io_operation_t op,bool ap,uint64_t *job)
{ *job=0;if(start_error)return start_error;CHECK(!io.worker_active);
    io=(cerberus_ble_archive_io_status_t){.job_id=++sequence,.operation=op,.worker_active=true,
        .format=ap?CERBERUS_BLE_ARCHIVE_IO_FORMAT_AP_JSON:CERBERUS_BLE_ARCHIVE_IO_FORMAT_SSID_TEXT,
        .state=CERBERUS_BLE_ARCHIVE_IO_STARTING};*job=sequence;return ESP_OK; }
esp_err_t cerberus_ble_archive_io_load_ap_json(const char *path,const char *name,uint64_t *job)
{ ++loads;snprintf(copied_path,sizeof(copied_path),"%s",path);snprintf(copied_name,sizeof(copied_name),"%s",name);
    return reserve(CERBERUS_BLE_ARCHIVE_IO_LOAD_AP_JSON,true,job); }
esp_err_t cerberus_ble_archive_io_load_ssid_text(const char *path,const char *name,uint64_t *job)
{ ++loads;snprintf(copied_path,sizeof(copied_path),"%s",path);snprintf(copied_name,sizeof(copied_name),"%s",name);
    return reserve(CERBERUS_BLE_ARCHIVE_IO_LOAD_SSID_TEXT,false,job); }
esp_err_t cerberus_ble_archive_io_save_ap_json(const cerberus_wifi_marauder_ap_t *doc,uint64_t *job)
{ ++saves;esp_err_t e=reserve(CERBERUS_BLE_ARCHIVE_IO_SAVE_AP_JSON,true,job);if(!e)worker_ap=*doc;return e; }
esp_err_t cerberus_ble_archive_io_save_ssid_text(const cerberus_wifi_marauder_ssid_t *doc,uint64_t *job)
{ ++saves;esp_err_t e=reserve(CERBERUS_BLE_ARCHIVE_IO_SAVE_SSID_TEXT,false,job);if(!e)worker_ssid=*doc;return e; }
esp_err_t cerberus_ble_archive_io_export_ap_json(const cerberus_wifi_archive_t *doc,uint64_t *job)
{ ++exports;esp_err_t e=reserve(CERBERUS_BLE_ARCHIVE_IO_EXPORT_AP_JSON,true,job);if(!e)frozen_native=*doc;return e; }
esp_err_t cerberus_ble_archive_io_export_ssid_text(const cerberus_wifi_archive_t *doc,uint64_t *job)
{ ++exports;esp_err_t e=reserve(CERBERUS_BLE_ARCHIVE_IO_EXPORT_SSID_TEXT,false,job);if(!e)frozen_native=*doc;return e; }
esp_err_t cerberus_ble_archive_io_copy_ap_json(uint64_t job,cerberus_wifi_marauder_ap_t *out)
{ ++copies;CHECK(job==io.job_id&&io.result_ready&&!io.worker_active);if(copy_error)return copy_error;*out=worker_ap;return ESP_OK; }
esp_err_t cerberus_ble_archive_io_copy_ssid_text(uint64_t job,cerberus_wifi_marauder_ssid_t *out)
{ ++copies;CHECK(job==io.job_id&&io.result_ready&&!io.worker_active);if(copy_error)return copy_error;*out=worker_ssid;return ESP_OK; }
void cerberus_ble_archive_io_get_status(cerberus_ble_archive_io_status_t *out) { *out=io; }
void cerberus_ble_archive_io_cancel(uint64_t job) { ++cancels;canceled_job=job;if(job==io.job_id)io.cancel_requested=true; }
void cerberus_ble_archive_io_release(uint64_t job) { ++releases;released_job=job;if(job==io.job_id)io.result_ready=false; }
#include "wifi_interchange_live_ui.h"
#include "interchange_native_routes.h"
static void clear_draw(void) { drawn_size=calls=fail_after=0;drawn[0]=0; }
static esp_err_t draw_detail_page(void) { ++draws;clear_draw();return current_page==UI_PAGE_SSID_LISTS?ESP_OK:native_draw(); }
static esp_err_t draw_current_page(void) { return current_page==UI_PAGE_WIFI_ARCHIVE?draw_detail_page():ESP_OK; }
/* Existing non-Wi-Fi Files actions are separate counted boundaries. */
enum { UI_FILE_MODE_LIST,UI_FILE_MODE_DELETE_CONFIRM,UI_FILE_MODE_PACKAGE_INFO,UI_FILE_MODE_ACTIONS,UI_FILE_OPERATION_RENAME };
static int file_mode,file_package_info;
static struct { bool is_directory;char name[256]; } file_selected_entry;
static char file_selected_path[512],file_action_message[64];
static uint16_t file_action_color;
static esp_err_t file_package_error;
#define CERBERUS_STORAGE_FIRMWARE_PATH "/CERBERUS/FIRMWARE"
static unsigned renames,clipboards,inspects,airtag_opens;
static void begin_file_name_input(int op,const char *name) { CHECK(op==UI_FILE_OPERATION_RENAME&&name);++renames; }
static void stage_file_clipboard(bool move) { (void)move;++clipboards; }
static esp_err_t cerberus_storage_inspect_firmware_package(const char *name,int *out) { CHECK(name&&out);++inspects;return ESP_OK; }
static bool airtag_json_is_file(const char *name) { return wifi_interchange_can_open(name,true); }
static void airtag_json_open_file(const char *path,const char *name) { CHECK(path&&name);++airtag_opens; }
#include "interchange_files.h"
#include "interchange_files_parent.h"

static void reset(void)
{ wifi_interchange_dispose();wifi_archive_view=NULL;memset(&native_view,0,sizeof(native_view));
    memset(&io,0,sizeof(io));memset(&worker_ap,0,sizeof(worker_ap));memset(&worker_ssid,0,sizeof(worker_ssid));
    memset(&file_selected_entry,0,sizeof(file_selected_entry));file_action_message[0]=0;
    current_page=UI_PAGE_FILES;wifi_archive_return_page=UI_PAGE_FILES;wifi_mode=wifi_archive_return_mode=0;
    privacy_mask=oom=false;start_error=copy_error=ssid_import_error=ESP_OK;clear_draw(); }
static void finish(void)
{
    CHECK(io.worker_active);bool ap=io.format==CERBERUS_BLE_ARCHIVE_IO_FORMAT_AP_JSON;
    cerberus_wifi_marauder_result_t r=CERBERUS_WIFI_MARAUDER_OK;
    if(io.operation==CERBERUS_BLE_ARCHIVE_IO_EXPORT_AP_JSON)
        r=cerberus_wifi_marauder_ap_from_archive(&frozen_native,&worker_ap,&io.wifi_export_stats);
    if(io.operation==CERBERUS_BLE_ARCHIVE_IO_EXPORT_SSID_TEXT)
        r=cerberus_wifi_marauder_ssid_from_archive(&frozen_native,&worker_ssid,&io.wifi_export_stats);
    io.wifi_result=r;io.worker_active=false;
    if(r!=CERBERUS_WIFI_MARAUDER_OK){io.error=r==CERBERUS_WIFI_MARAUDER_UNREPRESENTABLE?ESP_ERR_NOT_SUPPORTED:
        r==CERBERUS_WIFI_MARAUDER_LIMIT?ESP_ERR_INVALID_SIZE:ESP_ERR_INVALID_RESPONSE;io.state=CERBERUS_BLE_ARCHIVE_IO_ERROR;return;}
    if(ap){uint16_t ignored=worker_ap.ignored_members;
        CHECK(cerberus_wifi_marauder_ap_encode(&worker_ap,encoded,sizeof(encoded),&encoded_length)==CERBERUS_WIFI_MARAUDER_OK);
        memset(&worker_ap,0,sizeof(worker_ap));CHECK(cerberus_wifi_marauder_ap_decode(encoded,encoded_length,&worker_ap)==CERBERUS_WIFI_MARAUDER_OK);
        /* The real worker returns its immutable input/decoded model. Encoding
         * intentionally cannot reconstruct unknown extensions. */
        worker_ap.ignored_members=ignored;}
    else {CHECK(cerberus_wifi_marauder_ssid_encode(&worker_ssid,encoded,sizeof(encoded),&encoded_length)==CERBERUS_WIFI_MARAUDER_OK);
        memset(&worker_ssid,0,sizeof(worker_ssid));CHECK(cerberus_wifi_marauder_ssid_decode(encoded,encoded_length,&worker_ssid)==CERBERUS_WIFI_MARAUDER_OK);}
    io.result_ready=true;io.state=CERBERUS_BLE_ARCHIVE_IO_COMPLETE;io.bytes=(uint32_t)encoded_length;
    snprintf(io.file_name,sizeof(io.file_name),"%s",ap?"P00001.LOG":"S00001.TXT");
}
static void preview(const char *dir,const char *name)
{ char path[1024];snprintf(path,sizeof(path),"%s/%s.ppm",dir,name);FILE *f=fopen(path,"wb");CHECK(f);
    fprintf(f,"P6\n320 480\n255\n");for(size_t i=0;i<sizeof(screen)/sizeof(*screen);++i){uint16_t p=screen[i];
        uint8_t rgb[]={(uint8_t)((p>>11)*255/31),(uint8_t)(((p>>5)&63)*255/63),(uint8_t)((p&31)*255/31)};
        assert(fwrite(rgb,1,3,f)==3);}CHECK(!fclose(f)); }
static void seed_ap(unsigned count)
{ memset(&worker_ap,0,sizeof(worker_ap));worker_ap.count=(uint8_t)count;
    for(unsigned i=0;i<count;++i){cerberus_wifi_marauder_ap_row_t *r=&worker_ap.rows[i];
        r->present=CERBERUS_WIFI_MARAUDER_AP_ALL;r->essid_length=32;r->man_length=64;
        for(unsigned j=0;j<32;++j)r->essid[j]=(uint8_t)('A'+j%26);
        r->essid[2]=0;r->essid[3]='\r';for(unsigned j=0;j<64;++j)r->man[j]=(uint8_t)('a'+j%26);
        r->bssid[0]=2;r->bssid[5]=(uint8_t)i;r->rssi=INT32_MIN;r->channel=255;r->security=255;r->packets=UINT32_MAX;
        r->station_offset=(uint16_t)(i?256:0);r->station_count=(uint16_t)(i?0:256);}
    worker_ap.station_count=count?256:0;for(unsigned i=0;i<worker_ap.station_count;++i){worker_ap.stations[i][0]=4;worker_ap.stations[i][5]=(uint8_t)i;}
    CHECK(cerberus_wifi_marauder_ap_validate(&worker_ap)==CERBERUS_WIFI_MARAUDER_OK); }
static void open_ap(unsigned count)
{ reset();wifi_interchange_open_file("/CERBERUS/WIFI","APs_0.log",true);seed_ap(count);finish();expected_ap=worker_ap;
    CHECK(wifi_interchange_tick(false));CHECK(wifi_interchange_view->has_document);CHECK(!memcmp(&wifi_interchange_view->document.ap,&expected_ap,sizeof(expected_ap))); }
static void open_ssid(const uint8_t *bytes,size_t length)
{ reset();wifi_interchange_open_file("/CERBERUS/WIFI","SSIDs_0.log",false);
    CHECK(cerberus_wifi_marauder_ssid_decode(bytes,length,&worker_ssid)==CERBERUS_WIFI_MARAUDER_OK);expected_ssid=worker_ssid;
    finish();CHECK(wifi_interchange_tick(false));CHECK(!memcmp(&wifi_interchange_view->document.ssid,&expected_ssid,sizeof(expected_ssid))); }
static void routes(const char *dir)
{
    reset();CHECK(wifi_interchange_can_open("APs_0.LoG",true)&&wifi_interchange_can_open("AP.JSON",true));
    CHECK(wifi_interchange_can_open("SSIDs.LOG",false)&&wifi_interchange_can_open("SSID.Txt",false));
    const char *no[]={NULL,"",".log","x.WFI","x.TMP","x.log.tmp"};
    for(unsigned i=0;i<sizeof(no)/sizeof(*no);++i){CHECK(!wifi_interchange_can_open(no[i],true));CHECK(!wifi_interchange_can_open(no[i],false));}
    CHECK(!wifi_interchange_can_open("x.TXT",true)&&!wifi_interchange_can_open("x.JSON",false));
    strcpy(file_selected_path,"/logs");strcpy(file_selected_entry.name,"ambiguous.LOG");file_mode=UI_FILE_MODE_ACTIONS;
    CHECK(files_page_draw()==ESP_OK);CHECK(strstr(drawn,"OPEN AP JSON")&&strstr(drawn,"OPEN SSID TEXT")&&strstr(drawn,"OPEN AIRTAG JSON"));preview(dir,"files-explicit-formats");
    unsigned before=loads;handle_file_actions_tap(20,315);CHECK(loads==before);handle_file_actions_tap(60,315);
    CHECK(loads==before+1&&io.operation==CERBERUS_BLE_ARCHIVE_IO_LOAD_AP_JSON);
    CHECK(!strcmp(copied_path,"/logs")&&!strcmp(copied_name,"ambiguous.LOG"));
    uint64_t job=io.job_id;wifi_interchange_tap(50,440);CHECK(current_page==UI_PAGE_FILES&&canceled_job==job&&released_job==job&&file_mode==UI_FILE_MODE_ACTIONS);
    files_actions_back();CHECK(file_mode==UI_FILE_MODE_LIST);file_mode=UI_FILE_MODE_ACTIONS;
    io.worker_active=false;handle_file_actions_tap(60,360);CHECK(io.operation==CERBERUS_BLE_ARCHIVE_IO_LOAD_SSID_TEXT);
    wifi_interchange_tap(50,440);io.worker_active=false;handle_file_actions_tap(60,270);CHECK(airtag_opens==1);
    strcpy(file_selected_path,CERBERUS_STORAGE_FIRMWARE_PATH);before=inspects;handle_file_actions_tap(60,270);CHECK(inspects==before+1&&!wifi_interchange_is_open());
    strcpy(file_selected_path,"/logs");file_selected_entry.is_directory=true;before=loads;handle_file_actions_tap(60,315);CHECK(loads==before);
    file_selected_entry.is_directory=false;handle_file_actions_tap(20,130);handle_file_actions_tap(200,130);CHECK(renames&&clipboards);
}
static void ap_pages(const char *dir)
{
    open_ap(64);CHECK(strstr(drawn,"AP JSON / 64 RECORDS"));preview(dir,"ap-list-64");
    for(unsigned p=1;p<16;++p){wifi_interchange_tap(200,395);CHECK(wifi_interchange_view->page==p);}
    CHECK(wifi_interchange_view->painted[3]==63);wifi_interchange_tap(20,315);CHECK(wifi_interchange_view->selected_index==63);
    CHECK(strstr(drawn,"STORED RSSI: -2147483648 / AGE UNKNOWN")&&strstr(drawn,"CHANNEL: 255"));
    CHECK(strstr(drawn,"4294967295")&&strstr(drawn,"SECURITY CODE: 255"));preview(dir,"ap-stored-fields");
    wifi_interchange_tap(200,395);CHECK(strstr(drawn,"41 42 00 0D 45")&&strstr(drawn,"69 6A 6B 6C"));preview(dir,"ap-full-bytes");
    wifi_interchange_tap(50,440);CHECK(wifi_interchange_view->page==15);
    open_ap(1);wifi_interchange_tap(20,185);wifi_interchange_tap(200,395);wifi_interchange_tap(200,395);
    CHECK(strstr(drawn,"1  04:00:00:00:00:00"));
    for(unsigned i=3;i<34;++i)wifi_interchange_tap(200,395);
    CHECK(wifi_interchange_view->detail_page==33&&strstr(drawn,"256  04:00:00:00:00:FF"));preview(dir,"ap-stations-last");
    privacy_mask=true;CHECK(draw_detail_page()==ESP_OK);CHECK(strstr(drawn,"XX:XX:XX:XX:XX:XX")&&!strstr(drawn,"04:00:00"));preview(dir,"ap-stations-masked");
    wifi_interchange_view->detail_page=0;CHECK(draw_detail_page()==ESP_OK);CHECK(!strstr(drawn,"02:00:00"));
    wifi_interchange_tap(50,440);CHECK(!strstr(drawn,"02:00:00"));
    open_ap(1);memset(&wifi_interchange_view->document.ap,0,sizeof(wifi_interchange_view->document.ap));wifi_interchange_view->document.ap.count=1;
    CHECK(draw_detail_page()==ESP_OK);wifi_interchange_tap(20,185);CHECK(strstr(drawn,"SSID NOT PROVIDED")&&strstr(drawn,"RSSI: NOT PROVIDED")&&strstr(drawn,"WPS: NOT PROVIDED"));
    preview(dir,"ap-missing-fields");wifi_interchange_view->selected.present=CERBERUS_WIFI_MARAUDER_AP_WPS|CERBERUS_WIFI_MARAUDER_AP_RSSI|CERBERUS_WIFI_MARAUDER_AP_CHANNEL;
    CHECK(draw_detail_page()==ESP_OK);CHECK(strstr(drawn,"WPS: FALSE")&&strstr(drawn,"STORED RSSI: 0")&&strstr(drawn,"CHANNEL: 0"));
    open_ap(0);CHECK(strstr(drawn,"EMPTY FILE LIST"));CHECK(wifi_interchange_ui_tap(wifi_interchange_view,20,185)==WIFI_INTERCHANGE_NONE);
    reset();wifi_interchange_open_file("/","extensions.LOG",true);
    const char *extensions="[{\"essid\":\"name\",\"vendorExtension\":{\"a\":1}}]";
    CHECK(cerberus_wifi_marauder_ap_decode((const uint8_t *)extensions,strlen(extensions),&worker_ap)==CERBERUS_WIFI_MARAUDER_OK);
    CHECK(worker_ap.ignored_members>0);finish();CHECK(wifi_interchange_tick(false));
    CHECK(strstr(drawn,"UNRETAINED JSON FIELDS: 1"));preview(dir,"ap-discarded-extension");
    wifi_interchange_tap(200,440);CHECK(worker_ap.ignored_members==1);finish();CHECK(wifi_interchange_tick(false));
    CHECK(strstr(drawn,"UNRETAINED JSON FIELDS: 1"));
    CHECK(cerberus_wifi_marauder_ap_decode(encoded,encoded_length,&expected_ap)==CERBERUS_WIFI_MARAUDER_OK&&expected_ap.ignored_members==0);
}
static void ssid_pages(const char *dir)
{
    static const uint8_t bytes[]={'a',0,'\r',255,'\n','\n','a',0,'\r',255};
    open_ssid(bytes,sizeof(bytes));CHECK(worker_ssid.count==3&&!worker_ssid.final_lf);
    wifi_interchange_tap(20,185);CHECK(strstr(drawn,"61 00 0D FF")&&strstr(drawn,"NO FINAL LF"));preview(dir,"ssid-exact-bytes");
    privacy_mask=true;CHECK(draw_detail_page()==ESP_OK);CHECK(strstr(drawn,"61 00 0D FF"));
    wifi_interchange_tap(50,440);wifi_interchange_tap(20,230);CHECK(strstr(drawn,"EMPTY NAME"));
    wifi_interchange_tap(200,440);expected_ssid=worker_ssid;finish();CHECK(wifi_interchange_tick(false));
    CHECK(!wifi_interchange_view->exporting&&!memcmp(&wifi_interchange_view->document.ssid,&expected_ssid,sizeof(expected_ssid)));
    CHECK(encoded_length==sizeof(bytes)&&!memcmp(encoded,bytes,sizeof(bytes)));
    static uint8_t max[2112];for(unsigned i=0;i<64;++i){memset(max+i*33,'Z',32);max[i*33+32]='\n';}
    open_ssid(max,sizeof(max));for(unsigned i=1;i<16;++i)wifi_interchange_tap(200,395);
    wifi_interchange_tap(20,315);CHECK(wifi_interchange_view->selected_index==63&&strstr(drawn,"32 NAME BYTES"));preview(dir,"ssid-64-last");
    open_ssid((const uint8_t *)"",0);CHECK(strstr(drawn,"EMPTY FILE LIST"));wifi_interchange_tap(200,440);finish();CHECK(encoded_length==0);CHECK(wifi_interchange_tick(false));
    open_ssid((const uint8_t *)"\n",1);CHECK(worker_ssid.count==1&&worker_ssid.final_lf);wifi_interchange_tap(20,185);CHECK(strstr(drawn,"FILE ENDS WITH LF"));
}
static void seed_native(unsigned count)
{
    memset(&native_view,0,sizeof(native_view));cerberus_wifi_archive_t *d=&native_view.document;
    d->count=(uint8_t)count;d->saved_time_valid=true;d->saved_us=1000000;
    d->publication=(cerberus_wifi_archive_publication_t){.received_us=900000,.c5_session_id=7,.c5_relation_epoch=9,.generation=42,
        .s3_count=(uint8_t)(count/2),.c5_count=(uint8_t)(count-count/2),.s3_total=32,.c5_total=32,.s3_coverage=1,.c5_coverage=2,.split=true};
    for(unsigned i=0;i<count;++i){cerberus_wifi_archive_row_t *r=&d->rows[i];bool s3=i<count/2;
        r->source=s3?1:2;r->source_index=(uint8_t)(s3?i:i-count/2);r->source_generation=s3?123:0;
        r->received_ms=900;r->bssid[0]=2;r->bssid[5]=(uint8_t)(i%32);r->channel=s3?6:149;r->security=i?9:3;
        r->rssi=-40;r->rssi_valid=i!=1;if(!r->rssi_valid)r->rssi=127;
        snprintf(r->ssid,sizeof(r->ssid),"Network %u",i%16);}
    CHECK(cerberus_wifi_archive_validate(d));native_view.has_document=true;native_view.view=WIFI_ARCHIVE_LIST;
    native_view.groups=true;native_view.source=2;native_view.sort=1;native_view.page=3;wifi_archive_view=&native_view;current_page=UI_PAGE_WIFI_ARCHIVE;
}
static void export_workflow(const char *dir)
{
    reset();seed_native(64);CHECK(draw_detail_page()==ESP_OK);unsigned before=exports;
    native_tap(260,440);CHECK(wifi_interchange_view->view==WIFI_INTERCHANGE_CHOICE&&exports==before);
    CHECK(strstr(drawn,"SOURCE, TIMING AND COVERAGE ARE OMITTED"));preview(dir,"wifi-export-choice");
    wifi_interchange_tap(50,440);CHECK(wifi_archive_view==&native_view&&native_view.groups&&native_view.source==2&&native_view.sort==1&&native_view.page==3);
    native_tap(260,440);wifi_interchange_tap(100,285);CHECK(exports==before+1&&io.worker_active);
    strcpy(native_view.document.rows[0].ssid,"CHANGED AFTER REQUEST");finish();CHECK(wifi_interchange_tick(false));
    CHECK(wifi_interchange_view->document.ap.count==64&&wifi_interchange_view->document.ap.rows[0].essid_length==9);
    CHECK(wifi_interchange_view->stats.records==64&&wifi_interchange_view->stats.security_unmapped==63&&wifi_interchange_view->stats.rssi_omitted==1);
    CHECK(strstr(drawn,"UNKNOWN SEC 63 / RSSI OMITTED 1"));preview(dir,"wifi-ap-export-result");
    wifi_interchange_tap(200,440);CHECK(!wifi_interchange_view->exporting&&strstr(drawn,"SAVING FILE COPY"));finish();CHECK(wifi_interchange_tick(false));
    CHECK(!wifi_interchange_view->stats_valid&&wifi_interchange_view->document.ap.count==64);
    wifi_interchange_tap(50,440);CHECK(wifi_archive_view==&native_view);
    reset();seed_native(4);strcpy(native_view.document.rows[2].ssid,native_view.document.rows[0].ssid);
    native_tap(260,440);wifi_interchange_tap(100,350);finish();CHECK(wifi_interchange_tick(false));
    CHECK(wifi_interchange_view->document.ssid.count==4&&wifi_interchange_view->document.ssid.final_lf);
    CHECK(!memcmp(wifi_interchange_view->document.ssid.rows[0].bytes,wifi_interchange_view->document.ssid.rows[2].bytes,9));
    reset();seed_native(2);native_view.document.rows[0].ssid[0]=(char)0xff;native_tap(260,440);wifi_interchange_tap(100,285);finish();CHECK(wifi_interchange_tick(false));
    CHECK(strstr(drawn,"NOT VALID UTF-8")&&wifi_archive_view==&native_view);preview(dir,"wifi-unrepresentable-json");
    reset();seed_native(2);strcpy(native_view.document.rows[0].ssid,"bad\nname");native_tap(260,440);wifi_interchange_tap(100,350);finish();CHECK(wifi_interchange_tick(false));
    CHECK(strstr(drawn,"CONTAINS A LINE BREAK"));preview(dir,"wifi-unrepresentable-text");
    reset();seed_native(0);native_tap(260,440);wifi_interchange_tap(100,350);finish();CHECK(wifi_interchange_tick(false)&&encoded_length==0);
    reset();seed_native(2);oom=true;before=exports;native_tap(260,440);CHECK(exports==before&&!wifi_interchange_view&&strstr(drawn,"ESP_ERR_NO_MEM"));
    wifi_interchange_tap(50,440);CHECK(wifi_archive_view==&native_view&&native_view.groups&&native_view.source==2);
}
static void lifetime(void)
{
    open_ap(4);clear_draw();fail_after=8;CHECK(wifi_interchange_draw()==ESP_FAIL&&!wifi_interchange_view->paint_valid);
    CHECK(wifi_interchange_ui_tap(wifi_interchange_view,20,185)==WIFI_INTERCHANGE_NONE);CHECK(draw_detail_page()==ESP_OK);
    wifi_interchange_tap(20,185);cerberus_wifi_marauder_ap_row_t frozen=wifi_interchange_view->selected;
    memset(&worker_ap,0,sizeof(worker_ap));CHECK(draw_detail_page()==ESP_OK);CHECK(!memcmp(&frozen,&wifi_interchange_view->selected,sizeof(frozen)));
    wifi_interchange_tap(200,440);CHECK(!wifi_interchange_view->selected_valid&&!wifi_interchange_view->paint_valid);expected_ap=worker_ap;
    finish();copy_error=ESP_ERR_TIMEOUT;CHECK(wifi_interchange_tick(false));CHECK(!memcmp(&expected_ap,&wifi_interchange_view->document.ap,sizeof(expected_ap)));
    CHECK(!strcmp(wifi_interchange_view->file_name,"P00001.LOG"));wifi_interchange_tap(100,395);copy_error=ESP_OK;
    wifi_interchange_tap(200,440);finish();io.format=CERBERUS_BLE_ARCHIVE_IO_FORMAT_SSID_TEXT;unsigned before=copies;
    CHECK(wifi_interchange_tick(false)&&copies==before&&wifi_interchange_view->error==ESP_ERR_INVALID_RESPONSE);
    wifi_interchange_tap(100,395);wifi_interchange_tap(200,440);uint64_t job=io.job_id;finish();++io.job_id;
    CHECK(wifi_interchange_tick(false)&&wifi_interchange_view->job_pending);before=cancels;
    CHECK(wifi_interchange_tick(true)&&cancels==before+1&&canceled_job==job&&released_job==job&&!allocated);
    CHECK(!wifi_interchange_tick(false));
    reset();wifi_interchange_open_file("/","x.LOG",true);job=io.job_id;current_page=UI_PAGE_MAIN;
    CHECK(!wifi_interchange_tick(false)&&!allocated&&current_page==UI_PAGE_MAIN&&released_job==job);
    reset();oom=true;before=loads;wifi_interchange_open_file("/","x.LOG",true);CHECK(loads==before&&!wifi_interchange_view);wifi_interchange_tap(50,440);
    reset();start_error=ESP_ERR_INVALID_STATE;wifi_interchange_open_file("/","x.LOG",true);CHECK(!wifi_interchange_view->job_pending);
    reset();seed_native(2);native_tap(260,440);wifi_interchange_tap(100,285);before=native_closes;job=io.job_id;
    CHECK(wifi_interchange_tick(true)&&native_closes==before+1&&released_job==job&&!wifi_archive_view);
    open_ap(1);expected_ap=wifi_interchange_view->document.ap;wifi_interchange_tap(200,440);
    io.worker_active=false;io.state=CERBERUS_BLE_ARCHIVE_IO_ERROR;io.error=ESP_FAIL;strcpy(io.file_name,"P00002.TMP");
    CHECK(wifi_interchange_tick(false));CHECK(strstr(drawn,"FAILED SAVE MAY LEAVE")&&strstr(drawn,"P00002.TMP"));
    CHECK(!strcmp(wifi_interchange_view->file_name,"P00001.LOG")&&!memcmp(&expected_ap,&wifi_interchange_view->document.ap,sizeof(expected_ap)));
}
static void edit_copy_routes(void)
{
    static const uint8_t bytes[]={'A',0,'\r',255,'\n','\n','A',0,'\r',255};
    open_ssid(bytes,sizeof(bytes));CHECK(strstr(drawn,"EDIT COPY"));
    cerberus_wifi_marauder_ssid_t original=wifi_interchange_view->document.ssid;
    wifi_interchange_ui_store_t *source=wifi_interchange_view;
    unsigned imports=ssid_imports,disposed=frees;
    CHECK(wifi_interchange_ui_tap(source,215,395)==WIFI_INTERCHANGE_NONE);
    CHECK(wifi_interchange_ui_tap(source,312,395)==WIFI_INTERCHANGE_NONE);
    CHECK(wifi_interchange_ui_tap(source,250,418)==WIFI_INTERCHANGE_NONE);
    ssid_import_error=ESP_ERR_NO_MEM;wifi_interchange_tap(216,374);
    CHECK(ssid_imports==imports+1U&&wifi_interchange_view==source&&current_page==UI_PAGE_WIFI_ARCHIVE&&frees==disposed);
    CHECK(source->has_document&&source->view==WIFI_INTERCHANGE_ERROR&&source->error==ESP_ERR_NO_MEM);
    CHECK(!memcmp(&source->document.ssid,&original,sizeof(original)));
    wifi_interchange_tap(100,395);ssid_import_error=ESP_OK;wifi_interchange_tap(311,417);
    CHECK(ssid_imports==imports+2U&&!wifi_interchange_view&&!allocated&&frees==disposed+1U);
    CHECK(current_page==UI_PAGE_SSID_LISTS&&ssid_import_return_page==UI_PAGE_FILES);
    CHECK(!memcmp(&imported_ssid,&original,sizeof(original)));
    memset(&worker_ssid,0,sizeof(worker_ssid));CHECK(!memcmp(&imported_ssid,&original,sizeof(original)));

    static uint8_t many[64U*2U];for(unsigned i=0;i<64U;++i){many[i*2U]='n';many[i*2U+1U]='\n';}
    open_ssid(many,sizeof(many));imports=ssid_imports;
    wifi_interchange_tap(112,374);CHECK(wifi_interchange_view->page==1U);
    wifi_interchange_tap(207,417);CHECK(wifi_interchange_view->page==2U);
    CHECK(wifi_interchange_ui_tap(wifi_interchange_view,208,395)==WIFI_INTERCHANGE_NONE);
    CHECK(wifi_interchange_ui_tap(wifi_interchange_view,104,395)==WIFI_INTERCHANGE_NONE);
    wifi_interchange_tap(103,395);CHECK(wifi_interchange_view->page==1U&&ssid_imports==imports);
    open_ap(8);imports=ssid_imports;wifi_interchange_tap(216,395);
    CHECK(wifi_interchange_view->page==1U&&ssid_imports==imports&&!strstr(drawn,"EDIT COPY"));

    reset();wifi_interchange_open_file("/CERBERUS/WIFI","busy.TXT",false);imports=ssid_imports;
    wifi_interchange_tap(250,395);CHECK(ssid_imports==imports&&wifi_interchange_view->job_pending);
    open_ssid((const uint8_t *)"",0);wifi_interchange_tap(250,395);
    CHECK(current_page==UI_PAGE_SSID_LISTS&&imported_ssid.count==0U&&!imported_ssid.final_lf);

    reset();seed_native(2);wifi_archive_return_page=UI_PAGE_MAIN;wifi_archive_return_mode=7;wifi_mode=9;
    native_tap(260,440);wifi_interchange_tap(100,350);finish();CHECK(wifi_interchange_tick(false));
    original=wifi_interchange_view->document.ssid;unsigned native_before=native_disposes;
    ssid_import_error=ESP_ERR_NO_MEM;wifi_interchange_tap(250,395);
    CHECK(wifi_archive_view==&native_view&&wifi_interchange_view&&native_disposes==native_before&&wifi_mode==9);
    wifi_interchange_tap(100,395);ssid_import_error=ESP_OK;wifi_interchange_tap(250,395);
    CHECK(!wifi_archive_view&&!wifi_interchange_view&&native_disposes==native_before+1U);
    CHECK(ssid_import_return_page==UI_PAGE_MAIN&&wifi_mode==7&&!memcmp(&imported_ssid,&original,sizeof(original)));
}
int main(int argc,char **argv)
{
    /* The unchanged native IO error path has its own complete controller suite. */
    (void)wifi_archive_ui_error;
    CHECK(argc==2);routes(argv[1]);ap_pages(argv[1]);ssid_pages(argv[1]);export_workflow(argv[1]);lifetime();edit_copy_routes();reset();
    CHECK(!allocated);printf("Wi-Fi interchange UI/routes: %u checks PASS; viewer=%zu, AP=%zu, SSID=%zu bytes.\n",checks,
        sizeof(wifi_interchange_ui_store_t),sizeof(cerberus_wifi_marauder_ap_t),sizeof(cerberus_wifi_marauder_ssid_t));return 0;
}
