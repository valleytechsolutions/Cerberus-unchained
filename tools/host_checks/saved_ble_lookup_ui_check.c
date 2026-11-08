#include "lookup_framebuffer.h"
#include "lookup_owner_fixture.h"
#define UI_BACKGROUND 0x0042
#define UI_PANEL 0x08A4
#define UI_MUTED 0x7CB5
#define UI_TEXT 0xE77F
#define UI_GOOD 0x471B
#define UI_BAD 0xF471
#define TAG "lookup-ui"
#define MALLOC_CAP_SPIRAM 4U
#define MALLOC_CAP_8BIT 2U
#define CHECK(x) do { ++checks; if(!(x)){fprintf(stderr,"UI line %u: %s\n",__LINE__,#x);abort();} } while(0)
#define ESP_ERROR_CHECK(x) CHECK((x)==ESP_OK)
static unsigned checks,allocations,freed,draws,file_cancels,file_releases;
static bool oom,masked,prior_live,prior_wifi;
static bool privacy_addresses_masked(void){return masked;}
static void cerberus_privacy_format_ble_identifier(const uint8_t*a,bool mask,char*out,size_t n)
{if(mask)snprintf(out,n,"**:**:**:**:**:**");else snprintf(out,n,"%02X:%02X:%02X:%02X:%02X:%02X",a[5],a[4],a[3],a[2],a[1],a[0]);}
static const char *esp_err_to_name(esp_err_t e)
{return e==ESP_ERR_NO_MEM?"ESP_ERR_NO_MEM":e==ESP_ERR_TIMEOUT?"ESP_ERR_TIMEOUT":e==ESP_ERR_INVALID_STATE?"ESP_ERR_INVALID_STATE":"ESP_FAIL";}
static esp_err_t draw_centered(uint16_t y,const char*t,uint16_t color,uint8_t scale)
{CHECK(strlen(t)*6U*scale<=320U);return cerberus_display_draw_text((uint16_t)((320U-strlen(t)*6U*scale)/2U),y,t,color,UI_BACKGROUND,scale);}
static esp_err_t draw_header(const char*t){return draw_centered(20,t,UI_TEXT,2);}
static esp_err_t draw_button(uint16_t x,uint16_t y,uint16_t w,uint16_t h,const char*t)
{CHECK(strlen(t)*12U<=w);esp_err_t e=cerberus_display_fill_rect(x,y,w,h,UI_PANEL);
 return e?e:cerberus_display_draw_text((uint16_t)(x+(w-strlen(t)*12U)/2U),y+(h-14U)/2U,t,UI_TEXT,UI_PANEL,2);}
#include "ble_archive_ui.h"
#include "airtag_json_ui.h"
#include "ble_archive_io.h"
typedef enum{UI_PAGE_BLE_ARCHIVE,UI_PAGE_BLE,UI_PAGE_FILES,UI_PAGE_MAIN,UI_PAGE_BLE_ATTACK}ui_page_t;
typedef enum{UI_BLE_MODE_MENU,UI_BLE_MODE_DETAIL,UI_BLE_MODE_C5,UI_BLE_MODE_FOX_HUNT}ui_ble_mode_t;
enum{UI_FILE_MODE_LIST,SENTINEL_SCAN_NONE,CERBERUS_S3_BLE_OWNER_NONE};
static ui_page_t current_page=UI_PAGE_BLE_ARCHIVE,airtag_json_return_page=UI_PAGE_FILES;
static ui_ble_mode_t ble_mode=UI_BLE_MODE_DETAIL,airtag_json_return_mode=UI_BLE_MODE_DETAIL;
static int file_mode,live_session,live_ble_session;
static bool ble_archive_menu,airtag_json_opened,airtag_json_from_archive;
static esp_err_t ble_archive_open_error,airtag_json_open_error;
static ble_archive_ui_store_t *ble_archive_view;
static airtag_json_ui_store_t *airtag_json_view;
static ble_archive_ui_store_t file_native;
static airtag_json_ui_store_t file_json;
static ble_archive_ui_store_t frozen_native_ui;
static airtag_json_ui_store_t frozen_json_ui;
static void *heap_caps_calloc(size_t n,size_t size,unsigned caps)
{CHECK(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));if(oom)return NULL;void*p=calloc(n,size);CHECK(p);++allocations;return p;}
static void heap_caps_free(void*p){if(!p)return;if(p==&file_native||p==&file_json)return;++freed;free(p);}
static int64_t esp_timer_get_time(void){return (int64_t)peer.now*1000;}
static uint64_t dual_ble_ui_clock(void*ctx){(void)ctx;return peer.now;}
static esp_err_t dual_ble_ui_exchange(void*ctx,uint8_t cmd,const void*in,size_t n,void*out,size_t cap)
{(void)ctx;return exchange(&peer,cmd,in,n,out,cap);}
typedef struct{bool busy;}cerberus_dual_wifi_status_t;
static void cerberus_dual_wifi_scan_get_status(cerberus_dual_wifi_status_t*out){out->busy=prior_wifi;}
static bool dual_wifi_ui_busy(const cerberus_dual_wifi_status_t*s){return s->busy;}
static bool dual_wifi_live_released(void){return !prior_live;}
static unsigned cerberus_s3_ble_host_owner(void){return CERBERUS_S3_BLE_OWNER_NONE;}
static unsigned old_stop_requests;
static void ble_discovery_leave(void){++old_stop_requests;}
static void dual_wifi_live_cancel_handoff(void){++old_stop_requests;}
static void dual_wifi_ui_leave(void){++old_stop_requests;}
static void sentinel_scan_session_select(int*s,unsigned mode){(void)s;CHECK(mode==SENTINEL_SCAN_NONE);++old_stop_requests;}
static void dual_ble_ui_sync(int mode,const uint8_t*a,uint8_t type,uint64_t now)
{CHECK(mode==-1&&!a&&!type&&now==peer.now);++old_stop_requests;}
static bool ble_fox_hunt_exit_pending,ble_fox_hunt_s3_attached,ble_fox_hunt_s3_started;
static bool ble_fox_hunt_from_c5,ble_fox_hunt_start_pending,ble_fox_hunt_retry_requested;
static bool ble_fox_hunt_s3_owned,ble_fox_hunt_s3_receiving_cached,ble_fox_hunt_s3_start_pending,ble_fox_hunt_cleanup_timed_out;
static bool ble_attack_selecting,fox_c5_held;
static uint32_t ble_fox_hunt_start_requested_ms,ble_fox_hunt_exit_started_ms,ble_fox_hunt_s3_start_requested_ms;
static uint32_t ble_fox_hunt_last_tick_ms,ble_fox_hunt_last_draw_ms;
static esp_err_t ble_fox_hunt_error,ble_fox_hunt_barrier_error;
static ui_page_t ble_session_exit_page=UI_PAGE_BLE;
static cerberus_c5_ble_result_t ble_fox_hunt_target;
static bool ble_fox_hunt_c5_released(void){return !fox_c5_held;}
static bool dual_ble_live_discovery_released(void){return !prior_live;}
static void set_ble_action_message(const char*t,unsigned color){(void)t;(void)color;}
static void open_tool_page(ui_page_t p){current_page=p;}
static void return_to_launcher(void){current_page=UI_PAGE_MAIN;}
static bool ble_fox_hunt_open_peer(const cerberus_c5_ble_result_t*,bool);
static esp_err_t draw_detail_page(void),draw_current_page(void);
static bool saved_ble_lookup_holds_files(void);
static bool saved_ble_lookup_is_open(void);
static void saved_ble_lookup_open(bool);
static esp_err_t saved_ble_lookup_draw(void);
static void saved_ble_lookup_tap(uint16_t,uint16_t);
static bool saved_ble_lookup_tick(bool),saved_ble_lookup_fox_return(bool);
void cerberus_ble_archive_io_cancel(uint64_t job){CHECK(job);++file_cancels;}
void cerberus_ble_archive_io_release(uint64_t job){CHECK(job);++file_releases;}
static void airtag_json_open_export(void){CHECK(false);}
static void ble_archive_save(unsigned scope){(void)scope;CHECK(false);}
enum{BLE_ARCHIVE_BOTH,BLE_ARCHIVE_S3_SELECTED};
static void ble_archive_return(void){CHECK(false);}
static bool airtag_json_is_open(void){return airtag_json_opened;}
static esp_err_t airtag_json_draw(void){return airtag_json_ui_draw(airtag_json_view,airtag_json_open_error);}
static bool airtag_json_tick(bool);
static void airtag_json_tap(uint16_t,uint16_t);
static void airtag_json_accept(esp_err_t error,uint64_t job){(void)error;(void)job;CHECK(false);}
esp_err_t cerberus_ble_archive_io_export_airtag_json(const cerberus_ble_archive_t*d,uint64_t*j)
{(void)d;(void)j;CHECK(false);return ESP_FAIL;}
esp_err_t cerberus_ble_archive_io_save_airtag_json(const cerberus_airtag_json_t*d,uint64_t*j)
{(void)d;(void)j;CHECK(false);return ESP_FAIL;}
#include "lookup_dispose.h"
#include "saved_ble_lookup_ui.h"
#include "lookup_fox_routes.h"
#include "lookup_file_routes.h"
static esp_err_t draw_detail_page(void)
{++draws;drawn_size=0;drawn[0]=0;return current_page==UI_PAGE_BLE_ARCHIVE?ble_archive_draw():ESP_OK;}
static esp_err_t draw_current_page(void){return draw_detail_page();}
static void ui_step(unsigned count,bool asleep)
{for(unsigned i=0;i<count;++i){peer.now+=20;ble_archive_tick(asleep);}}
static void ui_seed(bool json,bool chain)
{
 CHECK(!saved_ble_lookup_opened&&!native.owned&&!peer.owned);
 cerberus_dual_ble_scan_t scratch;init_live(&scratch,&peer);peer.count=4;peer.metadata_supported=true;
 memset(&native,0,sizeof(native));memset(native_rows,0,sizeof(native_rows));native_starts=native_stops=native_releases=0;
 native_fail_start=native_fail_stop=native_bad_copy=false;peer.now=1000;
 memset(&file_native,0,sizeof(file_native));memset(&file_json,0,sizeof(file_json));
 memcpy(file_native.selected.address,target,6);file_native.selected.address_type=3;
 file_native.has_document=file_native.selected_valid=true;file_native.view=BLE_ARCHIVE_UI_DETAIL;
 file_native.page=3;file_native.raw_page=2;file_native.source_filter=CERBERUS_BLE_ARCHIVE_LIST_ALL;
 snprintf(file_native.file_name,sizeof(file_native.file_name),"B00042.BLE");
 memcpy(file_json.selected.address,target,6);file_json.selected_index=7;file_json.selected.length=256;
 file_json.has_document=file_json.selected_valid=true;file_json.view=AIRTAG_JSON_RAW;file_json.page=2;file_json.raw_page=1;
 snprintf(file_json.file_name,sizeof(file_json.file_name),"SAVED-TAGS.LOG");
 file_json.document.count=8;file_json.document.records[7]=file_json.selected;
 file_json.document.payload_used=256;memset(file_json.document.payload,0x55,256);
 ble_archive_view=!json||chain?&file_native:NULL;airtag_json_view=json?&file_json:NULL;
 airtag_json_opened=json;airtag_json_from_archive=chain;ble_archive_menu=false;
 current_page=UI_PAGE_BLE_ARCHIVE;ble_mode=UI_BLE_MODE_DETAIL;prior_live=prior_wifi=oom=masked=false;
 ble_fox_hunt_exit_pending=ble_fox_hunt_s3_attached=ble_fox_hunt_s3_started=false;
 ble_fox_hunt_start_pending=fox_c5_held=false;old_stop_requests=0;fail_after=0;
 saved_ble_lookup_sleep_retained=false;
}
static void remember_file(void){frozen_native_ui=file_native;frozen_json_ui=file_json;}
static void check_file(void)
{
 /* Painting invalidates only list hit maps; selected document and navigation
  * state must be byte-identical through radio activity and return. */
 CHECK(!memcmp(&file_native.document,&frozen_native_ui.document,sizeof(file_native.document)));
 CHECK(!memcmp(&file_native.selected,&frozen_native_ui.selected,sizeof(file_native.selected)));
 CHECK(file_native.page==frozen_native_ui.page&&file_native.raw_page==frozen_native_ui.raw_page&&file_native.view==frozen_native_ui.view);
 CHECK(!memcmp(&file_json.document,&frozen_json_ui.document,sizeof(file_json.document)));
 CHECK(!memcmp(&file_json.selected,&frozen_json_ui.selected,sizeof(file_json.selected)));
 CHECK(file_json.page==frozen_json_ui.page&&file_json.raw_page==frozen_json_ui.raw_page&&file_json.view==frozen_json_ui.view);
}
static void enter(bool json,bool chain)
{ui_seed(json,chain);remember_file();ble_archive_tap(json?112:164,json?426:374);
 CHECK(saved_ble_lookup_opened&&saved_ble_lookup_ui&&!saved_ble_lookup_ui_busy()&&!native_starts&&!peer.starts&&!old_stop_requests);
 CHECK(!memcmp(saved_ble_lookup_ui->address,target,6));ui_step(3,false);CHECK(!native_starts&&!peer.starts);}
static void run_lookup(void)
{ble_archive_tap(84,366);ui_step(2,false);CHECK(native.attached);feed_native();
 for(unsigned i=0;i<800;++i){ui_step(1,false);cerberus_saved_ble_lookup_view_t v;cerberus_saved_ble_lookup_view(&saved_ble_lookup_ui->lookup,peer.now,&v);if(v.count==8)break;}
 cerberus_saved_ble_lookup_view_t v;cerberus_saved_ble_lookup_view(&saved_ble_lookup_ui->lookup,peer.now,&v);CHECK(v.count==8&&native.owned&&peer.owned);CHECK(draw_detail_page()==ESP_OK);}
static void drain_lookup(void)
{native_fail_stop=peer.fail_stop=false;for(unsigned i=0;i<1200&&saved_ble_lookup_opened;++i)ui_step(1,false);
 CHECK(!saved_ble_lookup_opened&&!native.attached&&!peer.attached);}
static void fox_finish(bool asleep)
{ble_fox_hunt_exit_pending=true;ble_fox_hunt_s3_attached=true;fox_c5_held=true;
 actual_fox_release_tick((uint32_t)peer.now,asleep);CHECK(saved_ble_lookup_opened&&current_page==UI_PAGE_BLE);
 ble_fox_hunt_s3_attached=false;actual_fox_release_tick((uint32_t)peer.now,asleep);CHECK(saved_ble_lookup_opened);
 fox_c5_held=false;actual_fox_release_tick((uint32_t)peer.now,asleep);CHECK(!saved_ble_lookup_opened&&current_page==UI_PAGE_BLE_ARCHIVE);check_file();}
static void save_preview(const char*folder,const char*name)
{char path[1024];snprintf(path,sizeof(path),"%s/%s.ppm",folder,name);FILE*f=fopen(path,"wb");CHECK(f);fprintf(f,"P6\n320 480\n255\n");
 for(unsigned i=0;i<320*480;++i){uint16_t p=screen[i];fputc((p>>11)*255/31,f);fputc(((p>>5)&63)*255/63,f);fputc((p&31)*255/31,f);}CHECK(!fclose(f));}
static void navigation(const char*folder)
{
 for(unsigned json=0;json<2;++json){enter(json,false);CHECK(draw_detail_page()==ESP_OK);save_preview(folder,json?"lookup-json-ready":"lookup-native-ready");
  const uint16_t misses[][2]={{7,426},{312,426},{8,470},{76,366},{83,366},{236,366},{243,366},{8,408}};
  for(unsigned i=0;i<sizeof(misses)/sizeof(misses[0]);++i){ble_archive_tap(misses[i][0],misses[i][1]);CHECK(!saved_ble_lookup_ui->start_pending&&!saved_ble_lookup_ui->closing);}
  run_lookup();CHECK(strstr(drawn,"SWEEP PEAK")&&strstr(drawn,"TYPE 0 PUBLIC"));save_preview(folder,"lookup-s3-candidates");
  ble_archive_tap(244,366);CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"C5 / TYPE 0 PUBLIC")&&strstr(drawn,"ORIGINAL"));save_preview(folder,"lookup-c5-candidates");
  unsigned frames=peer.frames,starts=native_starts;CHECK(draw_detail_page()==ESP_OK&&peer.frames==frames&&native_starts==starts);
  cerberus_saved_ble_lookup_candidate_t selected=saved_ble_lookup_ui->painted[2];
  native_fail_stop=peer.fail_stop=true;ble_archive_tap(20,236);ui_step(4,false);CHECK(saved_ble_lookup_ui->handoff&&current_page==UI_PAGE_BLE_ARCHIVE);
  CHECK(!memcmp(&saved_ble_lookup_ui->selected,&selected,sizeof(selected)));check_file();
  ble_archive_dispose();airtag_json_dispose();CHECK(ble_archive_view==(json?NULL:&file_native)&&airtag_json_view==(json?&file_json:NULL));
  native_fail_stop=peer.fail_stop=false;for(unsigned i=0;i<300&&current_page!=UI_PAGE_BLE;++i)ui_step(1,false);
  CHECK(saved_ble_lookup_ui->fox&&current_page==UI_PAGE_BLE&&ble_mode==UI_BLE_MODE_FOX_HUNT&&!native.attached&&!peer.attached);
  CHECK(ble_fox_hunt_from_c5&&ble_fox_hunt_target.address_type==2&&!memcmp(ble_fox_hunt_target.address,target,6)&&ble_fox_hunt_target.rssi==127);
  ui_step(4,false);CHECK(saved_ble_lookup_opened);fox_finish(false);
 }
 enter(true,true);run_lookup();ble_archive_tap(20,144);for(unsigned i=0;i<300&&current_page!=UI_PAGE_BLE;++i)ui_step(1,false);
 CHECK(saved_ble_lookup_ui->fox&&ble_archive_view==&file_native&&airtag_json_view==&file_json&&!ble_fox_hunt_from_c5);fox_finish(true);
 ui_step(5,true);CHECK(ble_archive_view==&file_native&&airtag_json_view==&file_json&&airtag_json_opened);ui_step(1,false);check_file();
 enter(true,false);prior_live=prior_wifi=true;ble_archive_tap(84,366);ui_step(5,false);CHECK(!native_starts&&!peer.starts&&saved_ble_lookup_ui->barrier);
 ble_archive_tap(20,426);ui_step(4,false);CHECK(saved_ble_lookup_opened&&!native_starts);prior_live=false;ui_step(2,false);CHECK(saved_ble_lookup_opened);
 prior_wifi=false;ui_step(2,false);CHECK(!saved_ble_lookup_opened&&!native_starts&&!peer.starts);check_file();
 enter(false,false);run_lookup();native_fail_stop=peer.fail_stop=true;ble_archive_tap(20,144);ui_step(300,false);
 CHECK(saved_ble_lookup_ui->handoff&&current_page==UI_PAGE_BLE_ARCHIVE);native_fail_stop=peer.fail_stop=false;ui_step(150,false);
 CHECK(saved_ble_lookup_opened&&!saved_ble_lookup_ui->handoff&&!saved_ble_lookup_ui->fox&&strstr(drawn,"OBSERVATION EXPIRED"));
 ble_archive_tap(20,426);drain_lookup();check_file();
 enter(true,true);run_lookup();native_fail_stop=peer.fail_stop=true;ble_archive_tap(20,144);ble_archive_tap(20,426);ui_step(5,false);
 CHECK(saved_ble_lookup_ui->closing&&!saved_ble_lookup_ui->handoff);drain_lookup();CHECK(current_page==UI_PAGE_BLE_ARCHIVE);check_file();
 enter(true,true);run_lookup();native_fail_stop=peer.fail_stop=true;ui_step(5,true);CHECK(saved_ble_lookup_opened&&saved_ble_lookup_ui->closing);
 native_fail_stop=peer.fail_stop=false;ui_step(100,true);CHECK(!saved_ble_lookup_opened&&airtag_json_opened&&airtag_json_view==&file_json&&ble_archive_view==&file_native);
 ui_step(1,false);check_file();
 enter(false,false);run_lookup();native_fail_stop=peer.fail_stop=true;current_page=UI_PAGE_MAIN;ui_step(5,false);CHECK(saved_ble_lookup_opened);
 drain_lookup();CHECK(current_page==UI_PAGE_MAIN);ui_step(1,false);CHECK(!ble_archive_view);
 enter(true,false);run_lookup();ble_archive_tap(20,144);for(unsigned i=0;i<300&&current_page!=UI_PAGE_BLE;++i)ui_step(1,false);
 CHECK(saved_ble_lookup_ui->fox);current_page=UI_PAGE_MAIN;ble_fox_hunt_exit_pending=true;ble_fox_hunt_s3_attached=false;fox_c5_held=false;
 actual_fox_release_tick((uint32_t)peer.now,false);CHECK(!saved_ble_lookup_opened&&current_page==UI_PAGE_MAIN);ui_step(1,false);CHECK(!airtag_json_view);
 ui_seed(true,false);oom=true;ble_archive_tap(112,426);CHECK(saved_ble_lookup_opened&&!saved_ble_lookup_ui&&strstr(drawn,"WORKSPACE UNAVAILABLE"));
 ble_archive_tap(20,426);CHECK(!saved_ble_lookup_opened&&airtag_json_view==&file_json);oom=false;
 CHECK(allocations==freed);
}
static void render_edges(const char*folder)
{
 enter(false,false);run_lookup();masked=true;CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"**:**:**:**:**:**"));save_preview(folder,"lookup-private");
 saved_ble_lookup_ui->lookup.candidates[0].peer.rssi=127;CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"RSSI UNAVAILABLE")&&!strstr(drawn,"127 DBM"));
 saved_ble_lookup_ui->lookup.candidates[0].age_at_anchor_ms=UINT32_MAX;CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"STALE"));
 saved_ble_lookup_ui->lookup.c5_begin_error=ESP_ERR_NOT_SUPPORTED;CHECK(draw_detail_page()==ESP_OK);save_preview(folder,"lookup-partial-error");
 unsigned start=calls;CHECK(draw_detail_page()==ESP_OK);unsigned total=calls-start;
 for(unsigned i=1;i<=total;++i){fail_after=calls+i;CHECK(draw_detail_page()==ESP_FAIL);CHECK(!saved_ble_lookup_ui->painted_valid);fail_after=0;}
 ble_archive_tap(20,426);drain_lookup();CHECK(allocations==freed);
}
int main(int argc,char**argv)
{CHECK(argc==2);CHECK(lookup_component_main()==0);navigation(argv[1]);render_edges(argv[1]);
 printf("Saved BLE lookup UI: %u checks; optional PSRAM %zu bytes\n",checks,sizeof(saved_ble_lookup_ui_store_t));return 0;}
