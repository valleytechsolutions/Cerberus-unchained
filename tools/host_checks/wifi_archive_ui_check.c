#include "wifi_framebuffer.h"
#include "wifi_archive_capture.h"
#include "ble_archive_io.h"
#include "s3_wifi_scanner.h"
#include "ssid_display_impl.h"
#define UI_BACKGROUND 0x0042
#define UI_PANEL 0x08A4
#define UI_MUTED 0x7CB5
#define UI_TEXT 0xE77F
#define UI_BAD 0xF471
#define TAG "wifi-ui"
#define MALLOC_CAP_SPIRAM 1U
#define MALLOC_CAP_8BIT 2U
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); abort(); } } while (0)
#define ESP_ERROR_CHECK(x) CHECK((x)==ESP_OK)
static bool privacy_mask, oom;
static unsigned allocations, frees, clock_calls, capture_calls, starts, stops, steps;
static unsigned loads, saves, copies, cancels, releases, draws;
static uint64_t sequence, canceled_job, released_job;
static int64_t now_us;
static esp_err_t start_error, copy_error, capture_error;
static cerberus_dual_wifi_snapshot_t published;
static cerberus_dual_wifi_status_t status;
static cerberus_ble_archive_io_status_t io;
static cerberus_wifi_archive_t worker_doc, original;
static uint8_t wire[CERBERUS_WIFI_ARCHIVE_MAX_SIZE];
static size_t wire_length;
static bool privacy_addresses_masked(void) { return privacy_mask; }
static void cerberus_privacy_format_wifi_identifier(const uint8_t *a,bool mask,char *out,size_t n)
{ if(mask)snprintf(out,n,"XX:XX:XX:XX:XX:XX");else snprintf(out,n,"%02X:%02X:%02X:%02X:%02X:%02X",a[0],a[1],a[2],a[3],a[4],a[5]); }
static const char *wifi_security_text(uint8_t security) { return security ? "WPA2" : "OPEN"; }
static const char *esp_err_to_name(esp_err_t error)
{ return error==ESP_ERR_NO_MEM?"ESP_ERR_NO_MEM":error==ESP_ERR_INVALID_STATE?"ESP_ERR_INVALID_STATE":error==ESP_ERR_INVALID_RESPONSE?"ESP_ERR_INVALID_RESPONSE":"ESP_FAIL"; }
static esp_err_t draw_centered(uint16_t y,const char *text,uint16_t color,uint8_t scale)
{ CHECK(strlen(text)*6U*scale<=320U);return cerberus_display_draw_text((320U-strlen(text)*6U*scale)/2U,y,text,color,UI_BACKGROUND,scale); }
static esp_err_t draw_header(const char *text) { return draw_centered(20,text,UI_TEXT,2); }
static esp_err_t button(uint16_t x,uint16_t y,uint16_t w,uint16_t h,const char *text,uint8_t scale)
{ CHECK(strlen(text)*6U*scale<=w);esp_err_t e=cerberus_display_fill_rect(x,y,w,h,UI_PANEL);
  return e?e:cerberus_display_draw_text(x+(w-strlen(text)*6U*scale)/2U,y+(h-7U*scale)/2U,text,UI_TEXT,UI_PANEL,scale); }
static esp_err_t draw_button(uint16_t x,uint16_t y,uint16_t w,uint16_t h,const char *text)
{ return button(x,y,w,h,text,2); }
static esp_err_t draw_small_button(uint16_t x,uint16_t y,uint16_t w,uint16_t h,const char *text)
{ return button(x,y,w,h,text,1); }
static void *heap_caps_calloc(size_t count,size_t size,unsigned caps)
{ CHECK(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));if(oom)return NULL;void *p=calloc(count,size);CHECK(p);++allocations;return p; }
static void heap_caps_free(void *p) { if(p){free(p);++frees;} }
esp_err_t cerberus_dual_wifi_scan_start(void) { ++starts;status.state=CERBERUS_DUAL_WIFI_RUNNING;return ESP_OK; }
void cerberus_dual_wifi_scan_stop(void) { ++stops;if(status.cleanup_pending)status.state=CERBERUS_DUAL_WIFI_STOPPING; }
void cerberus_dual_wifi_scan_step(void) { ++steps; }
void cerberus_dual_wifi_scan_get_status(cerberus_dual_wifi_status_t *out) { *out=status; }
esp_err_t cerberus_dual_wifi_scan_get_row(uint8_t i,cerberus_dual_wifi_row_t *out)
{ if(i>=published.count)return ESP_ERR_NOT_FOUND;*out=published.rows[i];return ESP_OK; }
esp_err_t cerberus_dual_wifi_scan_capture_archive(int64_t now,uint64_t mask,cerberus_wifi_archive_t *out)
{ ++capture_calls;if(capture_error)return capture_error;
  return cerberus_wifi_archive_capture_rows(&published.publication,published.rows,published.count,now,mask,out)?ESP_OK:ESP_ERR_INVALID_RESPONSE; }
#include "dual_wifi_ui.h"
typedef enum { UI_PAGE_FILES, UI_PAGE_WIFI, UI_PAGE_WIFI_ARCHIVE, UI_PAGE_MAIN } ui_page_t;
typedef enum { UI_WIFI_MODE_MENU, UI_WIFI_MODE_DUAL } ui_wifi_mode_t;
static ui_page_t current_page=UI_PAGE_FILES;
static ui_wifi_mode_t wifi_mode=UI_WIFI_MODE_MENU;
static esp_err_t draw_detail_page(void),draw_current_page(void);
static int64_t esp_timer_get_time(void) { ++clock_calls;return now_us; }
static void dual_wifi_live_cancel_handoff(void) { }
static void dual_wifi_live_open(bool start)
{ current_page=UI_PAGE_WIFI;wifi_mode=UI_WIFI_MODE_DUAL;(void)dual_wifi_ui_enter(start); }
static esp_err_t reserve(cerberus_ble_archive_io_operation_t operation,uint64_t *job)
{ *job=0;if(start_error)return start_error;CHECK(!io.worker_active);io=(cerberus_ble_archive_io_status_t){
    .job_id=++sequence,.format=CERBERUS_BLE_ARCHIVE_IO_FORMAT_WIFI,.operation=operation,
    .state=CERBERUS_BLE_ARCHIVE_IO_STARTING,.worker_active=true};*job=sequence;return ESP_OK; }
esp_err_t cerberus_ble_archive_io_save_wifi(const cerberus_wifi_archive_t *doc,uint64_t *job)
{ ++saves;esp_err_t e=reserve(CERBERUS_BLE_ARCHIVE_IO_SAVE_WIFI,job);if(!e)worker_doc=*doc;return e; }
esp_err_t cerberus_ble_archive_io_load_wifi(const char *path,const char *name,uint64_t *job)
{ CHECK(path&&name);++loads;return reserve(CERBERUS_BLE_ARCHIVE_IO_LOAD_WIFI,job); }
esp_err_t cerberus_ble_archive_io_copy_wifi(uint64_t job,cerberus_wifi_archive_t *out)
{ ++copies;CHECK(job==io.job_id&&!io.worker_active&&io.result_ready);if(copy_error)return copy_error;*out=worker_doc;return ESP_OK; }
void cerberus_ble_archive_io_get_status(cerberus_ble_archive_io_status_t *out) { *out=io; }
void cerberus_ble_archive_io_cancel(uint64_t job) { ++cancels;canceled_job=job; }
void cerberus_ble_archive_io_release(uint64_t job) { ++releases;released_job=job;if(job==io.job_id)io.result_ready=false; }
/* Optional interchange overlay is tested by check_wifi_interchange_ui.py.
 * This native .WFI controller fixture keeps that independent page closed. */
static bool wifi_interchange_is_open(void) { return false; }
static bool wifi_interchange_tick(bool asleep) { (void)asleep;return false; }
static esp_err_t wifi_interchange_draw(void) { CHECK(false);return ESP_FAIL; }
static void wifi_interchange_tap(uint16_t x,uint16_t y) { (void)x;(void)y;CHECK(false); }
static void wifi_interchange_open_export(void) { CHECK(false); }
#include "wifi_archive_live_ui.h"
#include "wifi_page_routes.h"
static void clear_draw(void) { calls=drawn_size=fail_after=0;drawn[0]=0; }
static esp_err_t draw_detail_page(void) { ++draws;clear_draw();return current_page==UI_PAGE_WIFI_ARCHIVE?page_draw():dual_wifi_ui_draw(); }
static esp_err_t draw_current_page(void)
{ return current_page==UI_PAGE_WIFI_ARCHIVE||current_page==UI_PAGE_WIFI?draw_detail_page():ESP_OK; }
static struct { char name[256]; } file_entries[4];
static char file_path[512]="/CERBERUS/WIFI";
static unsigned ble_opens,text_opens,image_opens;
static bool ble_archive_is_file(const char *name) { return strstr(name,".BLE")!=NULL; }
static void ble_archive_open_file(const char *path,const char *name) { CHECK(path&&name);++ble_opens; }
static bool cerberus_bmp_is_file_name(const char *name) { return strstr(name,".BMP")!=NULL; }
static void open_image_file(const char *name) { CHECK(name);++image_opens; }
static void open_text_file(const char *name) { CHECK(name);++text_opens; }
#include "wifi_file_route.h"
static void reset(void)
{
    wifi_archive_dispose();dual_wifi_ui_leave();CHECK(allocations==frees);
    memset(&io,0,sizeof(io));memset(&published,0,sizeof(published));memset(&status,0,sizeof(status));
    current_page=UI_PAGE_FILES;wifi_mode=UI_WIFI_MODE_MENU;privacy_mask=oom=false;
    dual_wifi_ui_set_error(ESP_OK);
    cerberus_dual_wifi_row_t unavailable;
    CHECK(dual_wifi_ui_copy_selected(&unavailable)==ESP_ERR_INVALID_STATE);
    start_error=copy_error=capture_error=ESP_OK;now_us=UINT64_C(0x100000010)*1000+2000;clear_draw();
}
static void seed(unsigned count)
{
    memset(&published,0,sizeof(published));published.count=(uint8_t)count;
    published.publication=(cerberus_wifi_archive_publication_t){.received_us=(uint64_t)now_us-2000,
        .c5_session_id=UINT64_C(0x100000003),.c5_relation_epoch=UINT64_C(0x200000005),.generation=19,
        .elapsed_ms=4011,.s3_count=(uint8_t)(count/2),.c5_count=(uint8_t)(count-count/2),
        .s3_total=49,.c5_total=61,.s3_coverage=1,.c5_coverage=2,.split=true};
    for(unsigned i=0;i<count;++i){cerberus_dual_wifi_row_t *r=&published.rows[i];bool s3=i<count/2;
        r->source=s3?1:2;r->source_index=(uint8_t)(s3?i:i-count/2);r->source_generation=s3?12345:0;
        r->received_ms=(uint32_t)(published.publication.received_us/1000);r->ap.bssid[0]=2;r->ap.bssid[5]=(uint8_t)(i%32);
        snprintf(r->ap.ssid,sizeof(r->ap.ssid),"Network %02u",i%8);r->ap.channel=s3?6:149;r->ap.security=3;
        r->ap.rssi=(int8_t)(-30-(i%60));
        if(!s3)r->observation=(cerberus_s3_wifi_observation_t){.valid=true,
            .request_started_us=published.publication.received_us-1000,.source_age_ms=6};
    }
    status=(cerberus_dual_wifi_status_t){.generation=19,.count=(uint8_t)count,.state=CERBERUS_DUAL_WIFI_COMPLETE,.split=true};
}
static void finish(void)
{
    CHECK(io.worker_active);CHECK(cerberus_wifi_archive_encode(&worker_doc,wire,sizeof(wire),&wire_length));
    memset(&worker_doc,0,sizeof(worker_doc));CHECK(cerberus_wifi_archive_decode(wire,wire_length,&worker_doc));
    io.worker_active=false;io.result_ready=true;io.state=CERBERUS_BLE_ARCHIVE_IO_COMPLETE;
    snprintf(io.file_name,sizeof(io.file_name),"W00001.WFI");io.count=worker_doc.count;io.bytes=(uint32_t)wire_length;
}
static void load(unsigned count)
{
    reset();seed(count);CHECK(cerberus_wifi_archive_capture(&published,now_us,&worker_doc));original=worker_doc;
    strcpy(file_entries[0].name,"W00001.WFI");open_selected_file(0);CHECK(wifi_archive_view&&wifi_archive_view->job_pending);
    finish();wifi_archive_tick(false);CHECK(wifi_archive_view->has_document&&!wifi_archive_view->job_pending);
    CHECK(!memcmp(&original,&wifi_archive_view->document,sizeof(original)));
}
static void preview(const char *dir,const char *name)
{
    char path[1024];snprintf(path,sizeof(path),"%s/%s.ppm",dir,name);FILE *f=fopen(path,"wb");CHECK(f);
    fprintf(f,"P6\n320 480\n255\n");for(size_t i=0;i<sizeof(screen)/sizeof(*screen);++i){uint16_t p=screen[i];
        uint8_t rgb[]={(uint8_t)((p>>11)*255/31),(uint8_t)(((p>>5)&63)*255/63),(uint8_t)((p&31)*255/31)};
        assert(fwrite(rgb,1,3,f)==3);}CHECK(!fclose(f));
}
static void routes_and_views(const char *dir)
{
    load(64);CHECK(wifi_archive_view->paint_valid&&wifi_archive_view->painted_count==4);preview(dir,"wifi-saved-64");
    unsigned clocks=clock_calls,radio=starts+stops+steps,old_loads=loads,old_copies=copies;
    for(unsigned p=1;p<16;++p){page_tap(200,390);CHECK(wifi_archive_view->page==p);}
    CHECK(wifi_archive_view->painted[3]==63);page_tap(20,315);CHECK(wifi_archive_view->selected.source_index==31);
    CHECK(strstr(drawn,"C5 FRAME AGE AT SAVE <= 9 ms"));preview(dir,"wifi-saved-c5");
    privacy_mask=true;CHECK(draw_detail_page()==ESP_OK);CHECK(strstr(drawn,"XX:XX:XX:XX:XX:XX"));
    CHECK(!strstr(drawn,"02:00:00:00:00:1F"));preview(dir,"wifi-saved-masked");privacy_mask=false;
    page_tap(30,440);page_tap(200,108);CHECK(wifi_archive_view->groups&&wifi_archive_view->count==8);
    CHECK(strstr(drawn,"8 OBSERVATIONS / S3 4 / C5 4"));preview(dir,"wifi-saved-ssid-groups");
    page_tap(20,190);CHECK(wifi_archive_view->group_focus&&wifi_archive_view->count==8);
    page_tap(20,190);CHECK(wifi_archive_view->view==WIFI_ARCHIVE_DETAIL);
    CHECK(strstr(drawn,"S3 FRAME PRESENCE / RF AGE UNKNOWN"));preview(dir,"wifi-saved-s3");
    page_tap(30,440);CHECK(wifi_archive_view->group_focus);page_tap(30,440);CHECK(!wifi_archive_view->group_focus);
    page_tap(30,146);CHECK(wifi_archive_view->source==1);CHECK(strstr(drawn,"4 OBSERVATIONS / S3 4 / C5 0"));
    page_tap(150,146);page_tap(150,146);CHECK(wifi_archive_view->band==2&&!wifi_archive_view->count);
    CHECK(strstr(drawn,"NO OBSERVATIONS"));preview(dir,"wifi-saved-filter-empty");
    CHECK(clock_calls==clocks&&starts+stops+steps==radio&&loads==old_loads&&copies==old_copies);
    page_tap(30,440);CHECK(current_page==UI_PAGE_FILES&&!wifi_archive_opened);
    strcpy(file_entries[0].name,"B00001.BLE");open_selected_file(0);CHECK(ble_opens==1);
    strcpy(file_entries[0].name,"other.LOG");open_selected_file(0);CHECK(text_opens==1);
    strcpy(file_entries[0].name,"x.BMP");open_selected_file(0);CHECK(image_opens==1);
    CHECK(wifi_archive_is_file("a.wFi")&&!wifi_archive_is_file(".WFI")&&!wifi_archive_is_file("x.WFI.TMP")&&!wifi_archive_is_file(NULL));
}
static void capture_selection_and_drain(const char *dir)
{
    reset();seed(64);dual_wifi_live_open(false);CHECK(draw_detail_page()==ESP_OK);preview(dir,"wifi-live-save-controls");
    CHECK(dual_wifi_ui_tap(290,172)==DUAL_WIFI_UI_REDRAW);CHECK(dual_wifi_ui_store->save_mask==1);
    CHECK(draw_detail_page()==ESP_OK);CHECK(dual_wifi_ui_tap(100,390)==DUAL_WIFI_UI_REDRAW);
    CHECK(draw_detail_page()==ESP_OK);const uint8_t second=dual_wifi_ui_store->painted_indexes[0];
    CHECK(dual_wifi_ui_tap(290,172)==DUAL_WIFI_UI_REDRAW);
    CHECK(dual_wifi_ui_store->save_mask==(UINT64_C(1)|(UINT64_C(1)<<second)));CHECK(draw_detail_page()==ESP_OK);
    CHECK(dual_wifi_ui_tap(250,390)==DUAL_WIFI_UI_SAVE_SELECTED);
    unsigned old_saves=saves;status.cleanup_pending=true;status.state=CERBERUS_DUAL_WIFI_RUNNING;
    wifi_archive_save_live(true);CHECK(wifi_archive_view->release_pending&&saves==old_saves&&!dual_wifi_ui_store);
    CHECK(wifi_archive_view->document.count==2&&wifi_archive_view->document.subset);
    original=wifi_archive_view->document;seed(2);status.cleanup_pending=true;status.state=CERBERUS_DUAL_WIFI_STOPPING;
    for(unsigned i=0;i<20;++i)wifi_archive_tick(false);CHECK(saves==old_saves);
    preview(dir,"wifi-save-draining");CHECK(!memcmp(&original,&wifi_archive_view->document,sizeof(original)));
    status.cleanup_pending=false;status.state=CERBERUS_DUAL_WIFI_IDLE;wifi_archive_tick(false);
    CHECK(saves==old_saves+1&&wifi_archive_view->job_pending&&!memcmp(&worker_doc,&original,sizeof(original)));
    finish();wifi_archive_tick(false);CHECK(wifi_archive_view->document.count==2);
    page_tap(20,190);CHECK(wifi_archive_view->selected.source_index==0);
    unsigned old_starts=starts;page_tap(30,390);CHECK(!wifi_archive_opened&&current_page==UI_PAGE_WIFI);
    CHECK(dual_wifi_ui_store->pending_start&&starts==old_starts); /* Drawing did not start radio. */
    CHECK(dual_wifi_ui_tick(100,true,true)==DUAL_WIFI_UI_REDRAW&&starts==old_starts+1);
    reset();seed(64);dual_wifi_live_open(false);CHECK(draw_detail_page()==ESP_OK);
    CHECK(dual_wifi_ui_tap(175,390)==DUAL_WIFI_UI_SAVE_ALL);wifi_archive_save_live(false);wifi_archive_tick(false);
    CHECK(worker_doc.count==64&&!worker_doc.subset);finish();wifi_archive_tick(false);
    CHECK(wifi_archive_view->document.count==64);page_tap(30,440);
    CHECK(current_page==UI_PAGE_WIFI&&dual_wifi_ui_store&&!dual_wifi_ui_store->pending_start);
    /* A newly published bank cannot be substituted for the old painted save. */
    reset();seed(4);dual_wifi_live_open(false);CHECK(draw_detail_page()==ESP_OK);published.publication.generation++;
    old_saves=saves;wifi_archive_save_live(false);wifi_archive_tick(false);
    CHECK(!wifi_archive_view->has_document&&wifi_archive_view->error==ESP_ERR_INVALID_STATE&&saves==old_saves);
}
static void failures_and_lifetime(const char *dir)
{
    load(4);unsigned old_copies=copies;clear_draw();fail_after=8;
    CHECK(wifi_archive_draw()==ESP_FAIL&&!wifi_archive_view->paint_valid);
    CHECK(wifi_archive_ui_tap(wifi_archive_view,20,190)==WIFI_ARCHIVE_NONE);clear_draw();CHECK(draw_detail_page()==ESP_OK);
    page_tap(20,190);cerberus_wifi_archive_row_t frozen=wifi_archive_view->selected;
    seed(64);now_us+=9999999;CHECK(draw_detail_page()==ESP_OK);
    CHECK(!memcmp(&frozen,&wifi_archive_view->selected,sizeof(frozen))&&copies==old_copies);
    page_tap(180,440);CHECK(wifi_archive_view->job_pending&&!wifi_archive_view->selected_valid);
    copy_error=ESP_FAIL;finish();wifi_archive_tick(false);CHECK(wifi_archive_view->view==WIFI_ARCHIVE_ERROR);
    CHECK(!memcmp(&original,&wifi_archive_view->document,sizeof(original)));preview(dir,"wifi-save-error");
    page_tap(100,390);CHECK(wifi_archive_view->view==WIFI_ARCHIVE_LIST);
    copy_error=ESP_OK;page_tap(180,440);uint64_t owned=io.job_id;
    io.job_id++;io.worker_active=false;wifi_archive_tick(false);CHECK(wifi_archive_view->job_pending);
    page_tap(30,440);CHECK(!wifi_archive_opened&&canceled_job==owned&&released_job==owned);
    unsigned old_draws=draws;io.job_id=owned;io.state=CERBERUS_BLE_ARCHIVE_IO_COMPLETE;io.result_ready=true;
    wifi_archive_tick(false);CHECK(draws==old_draws&&!wifi_archive_opened);
    load(0);CHECK(strstr(drawn,"NO OBSERVATIONS"));preview(dir,"wifi-saved-empty");
    wifi_archive_tick(true);CHECK(!wifi_archive_opened&&current_page==UI_PAGE_FILES);
    reset();oom=true;unsigned old_loads=loads;wifi_archive_open_file("/","x.WFI");
    CHECK(!wifi_archive_view&&wifi_archive_open_error==ESP_ERR_NO_MEM&&loads==old_loads);page_tap(30,440);
    reset();seed(4);dual_wifi_live_open(false);CHECK(draw_detail_page()==ESP_OK);oom=true;
    wifi_archive_save_live(false);CHECK(!wifi_archive_view&&!dual_wifi_ui_store);oom=false;page_tap(30,440);
    CHECK(dual_wifi_ui_store&&!dual_wifi_ui_store->pending_start);
    reset();seed(4);dual_wifi_live_open(false);CHECK(draw_detail_page()==ESP_OK);status.cleanup_pending=true;
    wifi_archive_save_live(false);unsigned old_saves=saves;wifi_archive_tick(true);
    CHECK(!wifi_archive_opened&&saves==old_saves&&!dual_wifi_ui_store->pending_start);
    reset();wifi_archive_open_file("/","x.WFI");owned=io.job_id;current_page=UI_PAGE_MAIN;wifi_archive_tick(false);
    CHECK(!wifi_archive_view&&canceled_job==owned&&released_job==owned);
    load(4);page_tap(180,440);io.format=CERBERUS_BLE_ARCHIVE_IO_FORMAT_BLE;finish();old_copies=copies;wifi_archive_tick(false);
    CHECK(copies==old_copies&&wifi_archive_view->view==WIFI_ARCHIVE_ERROR);
}
static void grouping_edges(void)
{
    load(6);wifi_archive_view->document.rows[0].ssid[0]=0;wifi_archive_view->document.rows[1].ssid[0]=0;
    memset(wifi_archive_view->document.rows[2].ssid,'X',32);wifi_archive_view->document.rows[2].ssid[32]=0;
    wifi_archive_view->document.rows[3].ssid[0]=(char)0x80;
    wifi_archive_view->document.rows[4].rssi_valid=false;wifi_archive_view->document.rows[4].rssi=127;
    wifi_archive_view->document.rows[5].rssi_valid=true;wifi_archive_view->document.rows[5].rssi=-127;
    page_tap(200,108);CHECK(wifi_archive_view->count==6); /* Hidden identities never collapse. */
    page_tap(250,146);page_tap(250,146);CHECK(wifi_archive_view->sort==2);
    CHECK(wifi_archive_view->indexes[5]==4); /* Unknown RSSI last, even positive stored value. */
    page_tap(30,108);page_tap(20,190);CHECK(wifi_archive_view->selected_valid);
}
static void selected_detail_copy_and_age(void)
{
    /* Detail selection is the original retained index, including bit 63. */
    reset();seed(64);dual_wifi_live_open(false);
    CHECK(draw_detail_page()==ESP_OK);
    unsigned position=0;while(position<64&&dual_wifi_ui_store->indexes[position]!=63)++position;CHECK(position<64);
    for(unsigned p=0;p<position/4;++p){CHECK(dual_wifi_ui_tap(100,390)==DUAL_WIFI_UI_REDRAW);CHECK(draw_detail_page()==ESP_OK);}
    CHECK(dual_wifi_ui_store->painted_indexes[position%4]==63);
    CHECK(dual_wifi_ui_tap(20,(uint16_t)(172+54*(position%4)))==DUAL_WIFI_UI_REDRAW);CHECK(draw_detail_page()==ESP_OK);
    CHECK(dual_wifi_ui_store->selected_row==63);
    CHECK(dual_wifi_ui_tap(20,390)==DUAL_WIFI_UI_REDRAW);CHECK(draw_detail_page()==ESP_OK);
    CHECK(dual_wifi_ui_store->save_mask==(UINT64_C(1)<<63));
    CHECK(dual_wifi_ui_tap(200,390)==DUAL_WIFI_UI_SAVE_SELECTED);
    wifi_archive_save_live(true);wifi_archive_tick(false);CHECK(worker_doc.count==1&&worker_doc.subset);
    CHECK(worker_doc.rows[0].source==2&&worker_doc.rows[0].source_index==31);
    CHECK(worker_doc.publication.s3_count==32&&worker_doc.publication.c5_count==32);
    finish();wifi_archive_tick(false);original=wifi_archive_view->document;
    page_tap(20,190);page_tap(200,440);CHECK(wifi_archive_view->job_pending);
    finish();wifi_archive_tick(false);CHECK(wifi_archive_view->view==WIFI_ARCHIVE_LIST);
    CHECK(!wifi_archive_view->selected_valid&&!memcmp(&original,&wifi_archive_view->document,sizeof(original)));
    /* Unknown and maximal bounds remain historical, with no current clock. */
    wifi_archive_view->document.rows[0].frame_age_upper_valid=false;
    wifi_archive_view->document.rows[0].frame_age_upper_at_save_ms=0;
    CHECK(draw_detail_page()==ESP_OK);page_tap(20,190);
    CHECK(strstr(drawn,"C5 FRAME AGE AT SAVE UNKNOWN"));
    page_tap(30,440);wifi_archive_view->document.rows[0].frame_age_upper_valid=true;
    wifi_archive_view->document.rows[0].frame_age_upper_at_save_ms=UINT64_MAX;
    CHECK(draw_detail_page()==ESP_OK);page_tap(20,190);
    CHECK(strstr(drawn,"C5 FRAME AGE AT SAVE <= 18446744073709551615 ms"));
    /* A newly published generation clears a historical save mask. */
    reset();seed(4);dual_wifi_live_open(false);CHECK(draw_detail_page()==ESP_OK);
    CHECK(dual_wifi_ui_tap(290,172)==DUAL_WIFI_UI_REDRAW);CHECK(dual_wifi_ui_store->save_mask);
    ++status.generation;++published.publication.generation;CHECK(draw_detail_page()==ESP_OK);
    CHECK(!dual_wifi_ui_store->save_mask);
}
int main(int argc,char **argv)
{
    CHECK(argc==2);routes_and_views(argv[1]);capture_selection_and_drain(argv[1]);
    failures_and_lifetime(argv[1]);grouping_edges();selected_detail_copy_and_age();reset();
    printf("Saved Wi-Fi UI: %u checks passed; viewer=%zu bytes, actual codec/capture, frozen selection, routes, privacy and lifetime\n",checks,sizeof(wifi_archive_ui_store_t));
    return 0;
}
