/* Production UI + controller + parser, using the existing controller's exact
 * transport fake. No UI helper is substituted for owner state transitions. */
#define main controller_suite
#include "mesh_scan_check.c"
#undef main
#include "mesh_locate_ui_peer.h"
#include "mesh_framebuffer.h"
#include "ble_archive_io.h"
#include "storage.h"
#include "sentinel_hub.h"
#include "field_menu.h"
#define UI_BACKGROUND 0x0042
#define UI_PANEL 0x08A4
#define UI_MUTED 0x7CB5
#define UI_TEXT 0xE77F
#define UI_GOOD 0x471B
#define UI_BAD 0xF471
#define TAG "mesh-ui"
#define MALLOC_CAP_SPIRAM 4U
#define MALLOC_CAP_8BIT 2U
#define ESP_ERROR_CHECK(x) CHECK((x)==ESP_OK)
typedef enum {UI_PAGE_IEEE802154,UI_PAGE_FILES,UI_PAGE_OTHER,UI_PAGE_WIFI,UI_PAGE_BLE_ATTACK,UI_PAGE_FIELD_MENU,UI_PAGE_BLE,UI_PAGE_TOOLS,UI_PAGE_MESH_ARCHIVE} ui_page_t;
typedef enum {UI_IEEE802154_MODE_MENU,UI_IEEE802154_MODE_ENERGY,UI_IEEE802154_MODE_BEACONS,
    UI_IEEE802154_MODE_ACTIVITY,UI_IEEE802154_MODE_ASSESSMENT,UI_IEEE802154_MODE_MESH} ui_ieee802154_mode_t;
static ui_page_t current_page;
static ui_ieee802154_mode_t ieee802154_mode;
enum { UI_WIFI_MODE_DUAL = 1, UI_WIFI_MODE_LIST, UI_BLE_MODE_LIST, UI_WIFI_MODE_CHANNEL_ANALYZER };
enum { CHANNEL_ANALYZER_BACK };
static void channel_analyzer_request(unsigned action){(void)action;CHECK(false);}
static unsigned wifi_mode,ble_mode;
static bool dual_wifi_return_to_view,ble_attack_capture_valid,ui_capability_blocked,learning_tool_active;
static const char *ui_capability_blocked_reason;
static ui_page_t tool_parent_page=UI_PAGE_FIELD_MENU;
static void dual_wifi_ui_leave(void){CHECK(false);}
static esp_err_t draw_current_page(void){return ESP_OK;}
static void open_tool_page(ui_page_t page);
static bool ieee802154_scan_checked,ieee802154_beacon_checked,ieee802154_activity_checked;
static bool oom,masked,prior_released=true;
static unsigned allocations,freed,io_saves,io_cancels,io_releases,file_opens,lessons;
static uint64_t job_sequence;
static struct {uint32_t firmware_version;} c5_status={73};
static char file_path[CERBERUS_STORAGE_PATH_MAX],opened_name[CERBERUS_STORAGE_NAME_MAX];
static cerberus_ble_archive_io_status_t io;
static mesh_archive_t io_copy, io_load_source, io_decoded, io_decode_workspace;
static uint8_t io_map_bytes[MESH_ARCHIVE_MAX_BYTES];
static size_t io_map_length;
static bool io_copy_fail;
static unsigned io_loads, io_exports, listings;
static char io_load_name[CERBERUS_STORAGE_NAME_MAX];
static unsigned file_mode, file_page_offset;
static size_t file_entry_count;
static esp_err_t storage_last_error;
enum { UI_FILE_MODE_LIST, UI_FILE_MODE_TEXT };
static esp_err_t refresh_file_listing(bool remount){CHECK(!remount);++listings;return ESP_OK;}
static mesh_locate_t locate_io_copy;
static esp_err_t io_start_error;
static const cerberus_ui_widget_theme_t theme={.background=UI_BACKGROUND,.panel=UI_PANEL,
    .text=UI_TEXT,.muted=UI_MUTED,.border=UI_GOOD};
#define UI_WIDGET_THEME (&theme)
static bool privacy_addresses_masked(void){return masked;}
static bool dual_wifi_live_released(void){return prior_released;}
static int64_t esp_timer_get_time(void){return peer.clock;}
static void *heap_caps_calloc(size_t n,size_t size,unsigned caps)
{CHECK(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));if(oom)return NULL;void*p=calloc(n,size);CHECK(p);++allocations;return p;}
static void heap_caps_free(void*p){if(p){++freed;free(p);}}
static const char *esp_err_to_name(esp_err_t error)
{return error==ESP_ERR_NO_MEM?"ESP_ERR_NO_MEM":error==ESP_ERR_NOT_SUPPORTED?"ESP_ERR_NOT_SUPPORTED":
 error==ESP_ERR_INVALID_RESPONSE?"ESP_ERR_INVALID_RESPONSE":error==ESP_ERR_TIMEOUT?"ESP_ERR_TIMEOUT":"ESP_FAIL";}
static esp_err_t draw_centered(uint16_t y,const char*text,uint16_t color,uint8_t scale)
{CHECK(strlen(text)*6U*scale<=320U);return cerberus_display_draw_text((320U-strlen(text)*6U*scale)/2U,y,text,color,UI_BACKGROUND,scale);}
static esp_err_t draw_header(const char*text){return draw_centered(20,text,UI_TEXT,2);}
static esp_err_t draw_button(uint16_t x,uint16_t y,uint16_t w,uint16_t h,const char*text)
{return cerberus_ui_widget_draw_button((cerberus_ui_rect_t){x,y,w,h},text,2U,true,UI_WIDGET_THEME);}
static esp_err_t draw_small_button(uint16_t x,uint16_t y,uint16_t w,uint16_t h,const char*text)
{return cerberus_ui_widget_draw_button((cerberus_ui_rect_t){x,y,w,h},text,1U,true,UI_WIDGET_THEME);}
esp_err_t c5_spi_link_mesh_exchange(uint8_t cmd,const void*q,size_t qs,void*r,size_t rs)
{return exchange(&peer,cmd,q,qs,r,rs);}
esp_err_t c5_spi_link_mesh_locate_exchange(uint8_t cmd,const void*q,size_t qs,void*r,size_t rs)
{return locate_peer_exchange(cmd,q,qs,r,rs);}
esp_err_t cerberus_ble_archive_io_save_mesh_archive(const mesh_archive_t*s,uint64_t*job)
{++io_saves;*job=0U;if(io_start_error)return io_start_error;
 CHECK(mesh_archive_valid(s));io_copy=*s;
 CHECK(mesh_archive_encode(s,io_map_bytes,sizeof(io_map_bytes),&io_map_length));
 io=(cerberus_ble_archive_io_status_t){.job_id=++job_sequence,.operation=CERBERUS_BLE_ARCHIVE_IO_SAVE_MESH_ARCHIVE,
    .format=CERBERUS_BLE_ARCHIVE_IO_FORMAT_MESH_ARCHIVE,.state=CERBERUS_BLE_ARCHIVE_IO_SAVING,.worker_active=true};
 *job=job_sequence;return ESP_OK;}
esp_err_t cerberus_ble_archive_io_load_mesh_archive(const char*path,const char*name,uint64_t*job)
{CHECK(path&&name);++io_loads;*job=0U;if(io_start_error)return io_start_error;
 snprintf(io_load_name,sizeof(io_load_name),"%s",name);
 io=(cerberus_ble_archive_io_status_t){.job_id=++job_sequence,.operation=CERBERUS_BLE_ARCHIVE_IO_LOAD_MESH_ARCHIVE,
    .format=CERBERUS_BLE_ARCHIVE_IO_FORMAT_MESH_ARCHIVE,.state=CERBERUS_BLE_ARCHIVE_IO_LOADING,.worker_active=true};
 *job=job_sequence;return ESP_OK;}
esp_err_t cerberus_ble_archive_io_copy_mesh_archive(uint64_t job,mesh_archive_t*out)
{CHECK(job==io.job_id&&io.result_ready&&io.format==CERBERUS_BLE_ARCHIVE_IO_FORMAT_MESH_ARCHIVE);
 if(io_copy_fail)return ESP_ERR_INVALID_RESPONSE;
 if(!mesh_archive_decode(io_map_bytes,io_map_length,&io_decoded,&io_decode_workspace))return ESP_ERR_INVALID_RESPONSE;
 *out=io_decoded;return ESP_OK;}
esp_err_t cerberus_ble_archive_io_export_mesh_archive_csv(const mesh_archive_t*s,uint64_t*job)
{++io_exports;*job=0U;if(io_start_error)return io_start_error;CHECK(mesh_archive_valid(s));io_load_source=*s;
 size_t length=0U;CHECK(mesh_archive_report_encode(s,NULL,0U,&length)&&length);
 io=(cerberus_ble_archive_io_status_t){.job_id=++job_sequence,.operation=CERBERUS_BLE_ARCHIVE_IO_EXPORT_MESH_ARCHIVE_CSV,
    .format=CERBERUS_BLE_ARCHIVE_IO_FORMAT_MESH_REPORT,.state=CERBERUS_BLE_ARCHIVE_IO_SAVING,.worker_active=true};
 *job=job_sequence;return ESP_OK;}
esp_err_t cerberus_ble_archive_io_save_mesh_locate(const mesh_locate_t*l,uint64_t*job)
{++io_saves;*job=0U;if(io_start_error)return io_start_error;
 CHECK(l->frozen&&l->final_valid&&!l->faulted&&mesh_locate_released(l));locate_io_copy=*l;
 io=(cerberus_ble_archive_io_status_t){.job_id=++job_sequence,.operation=CERBERUS_BLE_ARCHIVE_IO_SAVE_MESH_LOCATE,
    .format=CERBERUS_BLE_ARCHIVE_IO_FORMAT_MESH_LOCATE_REPORT,.state=CERBERUS_BLE_ARCHIVE_IO_SAVING,.worker_active=true};
 *job=job_sequence;return ESP_OK;}
void cerberus_ble_archive_io_get_status(cerberus_ble_archive_io_status_t*out){*out=io;}
void cerberus_ble_archive_io_cancel(uint64_t job){CHECK(job==io.job_id);++io_cancels;}
void cerberus_ble_archive_io_release(uint64_t job){CHECK(job);++io_releases;}
static void open_text_file(const char*name){++file_opens;snprintf(opened_name,sizeof(opened_name),"%s",name);}
static void open_learning_lesson(unsigned id){CHECK(id==4U);++lessons;}
static esp_err_t draw_detail_page(void);
#include "mesh_ui.h"
#include "mesh_archive_ui.h"
static esp_err_t draw_detail_page(void)
{drawn_size=0;drawn[0]=0;return current_page==UI_PAGE_MESH_ARCHIVE?mesh_archive_draw():
 current_page==UI_PAGE_IEEE802154&&ieee802154_mode==UI_IEEE802154_MODE_MESH?mesh_ui_draw():ESP_OK;}
#include "mesh_routes.h"
static void open_tool_page(ui_page_t page){current_page=page;route_entry();}
static void ui_step(unsigned ms,bool asleep)
{peer.clock+=(int64_t)ms*1000;unsigned before=peer.wire_calls;
 route_tick((uint64_t)peer.clock/1000U,asleep);CHECK(peer.wire_calls-before<=4U);}
static void ui_reset(void)
{CHECK(!mesh_ui&&!mesh_archive_view);reset();memset(&locate_peer,0,sizeof(locate_peer));oom=masked=false;prior_released=true;c5_status.firmware_version=73;
 current_page=UI_PAGE_IEEE802154;ieee802154_mode=UI_IEEE802154_MODE_MENU;fail_after=0;
 io=(cerberus_ble_archive_io_status_t){0};io_start_error=ESP_OK;io_copy_fail=false;opened_name[0]=0;route_entry();route_tools(30,70);}
static void ui_exit(void)
{mesh_ui_close(true);for(unsigned i=0;i<1000U&&mesh_ui;++i)ui_step(20,false);CHECK(!mesh_ui&&!peer.bound&&!locate_peer.bound);}
static void ui_begin(unsigned count)
{ui_reset();CHECK(mesh_ui&&!peer.wire_calls&&!mesh_ui_busy());mesh_ui_start_stop();ui_step(0,false);
 CHECK(peer.starts==1U&&peer.bound&&mesh_ui_busy());
 for(unsigned i=0;i<count;++i){add_row(0U);row_t*r=&peer.rows[(peer.head+peer.count-1U)%16U];
    r->bytes[21]=(uint8_t)(i+1U);r->bytes[22]=(uint8_t)(i+2U);}
 for(unsigned i=0;i<1000U&&mesh_ui->scan.model.counters.records<count;++i)ui_step(20,false);
 CHECK(mesh_ui->scan.model.counters.records==count);CHECK(draw_detail_page()==ESP_OK);}
static void ui_freeze(void)
{mesh_scan_stop(&mesh_ui->scan);for(unsigned i=0;i<1000U&&!mesh_ui->scan.frozen;++i)ui_step(20,false);
 CHECK(mesh_ui->scan.frozen&&mesh_scan_released(&mesh_ui->scan)&&!peer.bound);}
static void lifecycle(void)
{
 ui_reset();ui_exit();oom=true;current_page=UI_PAGE_IEEE802154;route_entry();route_tools(30,70);CHECK(!mesh_ui&&!peer.wire_calls);
 CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"NO PSRAM"));route_back();CHECK(ieee802154_mode==UI_IEEE802154_MODE_MENU);oom=false;
 ui_reset();c5_status.firmware_version=71;mesh_ui_start_stop();ui_step(20,false);
 CHECK(!peer.starts&&!peer.wire_calls&&mesh_ui->error==ESP_ERR_NOT_SUPPORTED);ui_exit();
 ui_reset();prior_released=false;mesh_ui_start_stop();ui_step(20,false);
 CHECK(mesh_ui->start_pending&&!mesh_ui_busy()&&!peer.wire_calls);mesh_ui_tap(20,80);CHECK(mesh_ui->mode==CERBERUS_MESH_OBSERVATION_SWEEP);
 mesh_ui_start_stop();prior_released=true;ui_step(20,false);CHECK(!peer.starts&&!mesh_ui->start_pending);
 mesh_ui_start_stop();route_back();ui_step(20,false);CHECK(!mesh_ui&&!peer.starts);
 ui_begin(4U);CHECK(mesh_ui->painted_count==1U);
 mesh_ui_tap(130,120);CHECK(draw_detail_page()==ESP_OK&&mesh_ui->painted_count==4U);
 mesh_ui_painted_t painted=mesh_ui->painted[0];
 mesh_ui->scan.model.addresses[0].key.value=0xdeadU;mesh_ui_tap(20,155);
 CHECK(mesh_ui->detail&&!memcmp(&mesh_ui->selected,&painted,sizeof(painted)));
 unsigned commands=peer.wire_calls;CHECK(draw_detail_page()==ESP_OK&&peer.wire_calls==commands&&strstr(drawn,"SOURCE SEQ"));
 route_back();CHECK(!mesh_ui->detail);CHECK(draw_detail_page()==ESP_OK);
 fail_after=calls+8U;CHECK(draw_detail_page()!=ESP_OK&&!mesh_ui->painted_count);mesh_ui_tap(20,155);CHECK(!mesh_ui->detail);fail_after=0;
 mesh_ui_tap(250,120);CHECK(draw_detail_page()==ESP_OK);painted=mesh_ui->painted[0];
 mesh_ui->scan.model.addresses[0].key.value=0xbeefU;mesh_ui_tap(20,155);
 CHECK(mesh_ui->detail&&!memcmp(&mesh_ui->selected,&painted,sizeof(painted)));
 CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"NOT A ROUTE"));route_back();
 peer.stop_hold=30U;mesh_ui_start_stop();for(unsigned i=0;i<20U;++i)ui_step(700,false);
 CHECK(mesh_ui_busy()&&mesh_ui->scan.stop_requested);unsigned starts=peer.starts;mesh_ui_start_stop();CHECK(peer.starts==starts);
 peer.stop_hold=0;ui_freeze();CHECK(mesh_ui->scan.final_valid);ui_exit();
 ui_begin(1U);peer.stop_hold=30U;route_back();for(unsigned i=0;i<20U;++i)ui_step(700,true);
 CHECK(mesh_ui&&mesh_ui->closing&&peer.bound);peer.stop_hold=0;
 for(unsigned i=0;i<1000U&&mesh_ui;++i)ui_step(20,true);CHECK(!mesh_ui&&ieee802154_mode==UI_IEEE802154_MODE_MESH);
 ui_begin(1U);current_page=UI_PAGE_OTHER;ui_step(20,false);CHECK(!mesh_ui||mesh_ui->closing);
 for(unsigned i=0;i<1000U&&mesh_ui;++i)ui_step(20,false);CHECK(!mesh_ui&&current_page==UI_PAGE_OTHER);
 ui_begin(0U);add_row(0U);mutate_record(peer.rows[peer.head].bytes,1U);
 for(unsigned i=0;i<1000U&&!mesh_ui->scan.faulted;++i)ui_step(20,false);
 CHECK(mesh_ui->scan.faulted&&peer.bound);unsigned saves=io_saves;
 mesh_ui_tap(260,400);CHECK(!mesh_ui->save_pending&&io_saves==saves);
 CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"CLOSE REQUIRED"));route_back();
 for(unsigned i=0;i<1000U&&mesh_ui;++i)ui_step(20,false);CHECK(!mesh_ui&&!peer.bound);
 CHECK(allocations==freed);
}
static void saving(void)
{
 ui_begin(3U);unsigned saves=io_saves;mesh_ui_tap(260,400);CHECK(mesh_ui->save_pending);
 for(unsigned i=0;i<1000U&&!mesh_ui->job;++i)ui_step(20,false);
 CHECK(mesh_ui->job&&io_saves==saves+1U&&!peer.bound&&io_copy.model.session==mesh_ui->scan.model.session);
 unsigned starts=peer.starts;mesh_ui_start_stop();mesh_ui_tap(20,80);CHECK(peer.starts==starts&&mesh_ui->mode==CERBERUS_MESH_OBSERVATION_SWEEP);
 io.state=CERBERUS_BLE_ARCHIVE_IO_ERROR;io.error=ESP_FAIL;io.worker_active=false;ui_step(20,false);
 CHECK(!mesh_ui->job&&!mesh_ui->saved_name[0]);mesh_ui_tap(260,400);ui_step(20,false);
 CHECK(mesh_ui->job&&io_saves==saves+2U&&peer.starts==starts);
 io.worker_active=false;io.state=CERBERUS_BLE_ARCHIVE_IO_COMPLETE;io.result_ready=true;io.format=CERBERUS_BLE_ARCHIVE_IO_FORMAT_WIFI;
 ui_step(20,false);CHECK(mesh_ui->error==ESP_ERR_INVALID_RESPONSE&&!mesh_ui->saved_name[0]);
 mesh_ui_tap(260,400);ui_step(20,false);CHECK(mesh_ui->job);
 io.worker_active=false;io.state=CERBERUS_BLE_ARCHIVE_IO_COMPLETE;io.result_ready=true;io.error=ESP_FAIL;
 ui_step(20,false);CHECK(!mesh_ui->saved_name[0]&&mesh_ui->error==ESP_FAIL);
 mesh_ui_tap(260,400);ui_step(20,false);CHECK(mesh_ui->job);
 io.worker_active=false;io.state=CERBERUS_BLE_ARCHIVE_IO_COMPLETE;io.result_ready=false;
 ui_step(20,false);CHECK(!mesh_ui->saved_name[0]&&mesh_ui->error==ESP_ERR_INVALID_RESPONSE);
 mesh_ui_tap(260,400);ui_step(20,false);CHECK(mesh_ui->job);
 io.worker_active=false;io.state=CERBERUS_BLE_ARCHIVE_IO_COMPLETE;io.result_ready=true;snprintf(io.file_name,sizeof(io.file_name),"M00001.MSH");
 ui_step(20,false);CHECK(!strcmp(mesh_ui->saved_name,"M00001.MSH"));
 mesh_ui_tap(260,400);CHECK(current_page==UI_PAGE_MESH_ARCHIVE&&!strcmp(file_path,"/CERBERUS/MESH")&&!strcmp(io_load_name,"M00001.MSH"));
 for(unsigned i=0;i<1000U&&mesh_ui;++i)ui_step(20,false);CHECK(!mesh_ui);
 mesh_archive_back();CHECK(!mesh_archive_view&&current_page==UI_PAGE_FILES);
 ui_begin(1U);mesh_ui_tap(260,400);for(unsigned i=0;i<1000U&&!mesh_ui->job;++i)ui_step(20,false);CHECK(mesh_ui->job);
 unsigned cancels=io_cancels,opens=file_opens;route_back();CHECK(io_cancels==cancels+1U);
 for(unsigned i=0;i<20U;++i)ui_step(700,true);CHECK(mesh_ui&&mesh_ui->job&&io_cancels==cancels+1U);
 io.worker_active=false;io.state=CERBERUS_BLE_ARCHIVE_IO_COMPLETE;io.result_ready=true;ui_step(20,true);
 CHECK(!mesh_ui&&file_opens==opens&&allocations==freed);
 ui_begin(1U);ui_freeze();mesh_ui_start_stop();CHECK(mesh_ui->suppress_old);CHECK(draw_detail_page()==ESP_OK&&!mesh_ui->painted_count);
 prior_released=false;ui_step(20,false);CHECK(!mesh_ui->painted_count);mesh_ui_start_stop();ui_exit();
}
static void ppm(const char*folder,const char*name)
{char path[1024];snprintf(path,sizeof(path),"%s/%s.ppm",folder,name);FILE*f=fopen(path,"wb");CHECK(f);
 fprintf(f,"P6\n320 480\n255\n");for(size_t i=0;i<320U*480U;++i){unsigned v=screen[i];
 fputc(((v>>11)&31U)*255U/31U,f);fputc(((v>>5)&63U)*255U/63U,f);fputc((v&31U)*255U/31U,f);}CHECK(!fclose(f));}
static void large_model(const char *folder)
{
 ui_begin(0U);
 mesh_observation_input_t input={.session=peer.epoch,.length=125U,.flags=7U,.rssi=-128,.lqi=255U};
 memset(input.mac,0xaa,sizeof(input.mac));input.mac[0]=0x41U;input.mac[1]=0xccU;input.mac[2]=0U;
 input.mac[3]=0x34U;input.mac[4]=0x12U;
 for(unsigned i=0;i<80U;++i){
    input.sequence=i+1U;input.callback_us=UINT64_MAX-10000U+i;input.channel=(uint8_t)(11U+i%16U);
    le(input.mac+5U,UINT64_C(0xffff000011110000),8U);
    le(input.mac+13U,UINT64_C(0xabcdef0011223300)+i,8U);
    CHECK(mesh_observation_ingest(&mesh_ui->scan.model,&input)==MESH_OBSERVATION_OK);
 }
 CHECK(mesh_ui->scan.model.pan_count==16U&&mesh_ui->scan.model.address_count==64U&&mesh_ui->scan.model.witness_count==64U);
 CHECK(mesh_ui->scan.model.counters.address_omissions&&mesh_ui->scan.model.counters.witness_omissions==16U);
 mesh_ui_tap(130,120);CHECK(draw_detail_page()==ESP_OK&&mesh_ui->count==64U);
 for(unsigned i=0;i<15U;++i){mesh_ui_tap(90,400);CHECK(draw_detail_page()==ESP_OK);}
 CHECK(mesh_ui->page==15U&&mesh_ui->painted_count==4U);mesh_ui_tap(90,400);CHECK(mesh_ui->page==15U);
 ppm(folder,"mesh-last-address-page");mesh_ui_tap(250,120);CHECK(draw_detail_page()==ESP_OK);
 CHECK(strstr(drawn,"E64 ABCDEF0011223300 -> E64 FFFF000011110000"));ppm(folder,"mesh-eui-traffic");
 mesh_ui_tap(20,155);CHECK(mesh_ui->detail&&mesh_ui->selected.value.witness_valid);
 CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"NEXT RAW PAGE")&&strstr(drawn,"18446744073709541"));
 ppm(folder,"mesh-eui-detail");mesh_ui_tap(120,390);CHECK(mesh_ui->raw_page==1U);
 CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"RAW PAGE 2 / 2"));ppm(folder,"mesh-raw-last-page");
 mesh_ui_painted_t frozen=mesh_ui->selected;
 masked=true;CHECK(draw_detail_page()==ESP_OK&&!strstr(drawn,"ABCDEF")&&!strstr(drawn,"AA AA")&&strstr(drawn,"HIDDEN"));
 CHECK(!memcmp(&frozen,&mesh_ui->selected,sizeof(frozen)));masked=false;route_back();
 input.sequence=81U;input.callback_us=UINT64_MAX-9000U;input.channel=11U;
 le(input.mac+13U,UINT64_C(0xabcdef0011223300),8U);
 CHECK(mesh_observation_ingest(&mesh_ui->scan.model,&input)==MESH_OBSERVATION_OK);
 CHECK(draw_detail_page()==ESP_OK);mesh_ui_tap(20,155);
 CHECK(!mesh_ui->selected.value.witness_valid&&draw_detail_page()==ESP_OK&&strstr(drawn,"NOT RETAINED"));
 ui_exit();
}
static void render(const char*folder)
{
 ui_reset();CHECK(draw_detail_page()==ESP_OK);ppm(folder,"mesh-idle");ui_exit();
 ui_begin(8U);CHECK(draw_detail_page()==ESP_OK);ppm(folder,"mesh-pans");
 mesh_ui_tap(130,120);CHECK(draw_detail_page()==ESP_OK);ppm(folder,"mesh-addresses");
 mesh_ui_tap(90,400);CHECK(draw_detail_page()==ESP_OK&&mesh_ui->page==1U);mesh_ui_tap(90,400);
 CHECK(draw_detail_page()==ESP_OK&&mesh_ui->page==2U);mesh_ui_tap(20,400);CHECK(mesh_ui->page==1U);
 mesh_ui_tap(250,120);CHECK(draw_detail_page()==ESP_OK);ppm(folder,"mesh-traffic");
 mesh_ui_tap(20,155);CHECK(draw_detail_page()==ESP_OK);ppm(folder,"mesh-detail");
 masked=true;CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"MASKED")&&!strstr(drawn,"41 88"));ppm(folder,"mesh-private-detail");masked=false;route_back();
 peer.stop_hold=5U;mesh_ui_start_stop();ui_step(20,false);CHECK(draw_detail_page()==ESP_OK);ppm(folder,"mesh-stopping");peer.stop_hold=0;ui_freeze();
 mesh_ui->scan.model.counters.pan_omissions=UINT32_MAX;mesh_ui->scan.model.counters.address_omissions=UINT32_MAX;
 mesh_ui->scan.model.counters.edge_omissions=UINT32_MAX;mesh_ui->scan.model.counters.witness_omissions=UINT32_MAX;
 mesh_ui->scan.model.counters.malformed=UINT32_MAX;mesh_ui->scan.model.counters.unsupported=UINT32_MAX;
 CHECK(draw_detail_page()==ESP_OK);ppm(folder,"mesh-limits");ui_exit();
 ui_reset();ui_exit();oom=true;route_entry();route_tools(30,70);CHECK(draw_detail_page()==ESP_OK);ppm(folder,"mesh-no-memory");route_back();oom=false;
 CHECK(cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND)==ESP_OK&&draw_header("Radio tools")==ESP_OK);
 CHECK(sentinel_hub_draw(SENTINEL_HUB_RADIO,UI_WIDGET_THEME)==ESP_OK);ppm(folder,"mesh-tools");
 for(unsigned i=0;i<5U;++i){ieee802154_mode=UI_IEEE802154_MODE_MENU;route_tools(30,(uint16_t)(70U+i*54U));
    CHECK(ieee802154_mode==(i==0U?UI_IEEE802154_MODE_MESH:i==1U?UI_IEEE802154_MODE_ENERGY:
        i==2U?UI_IEEE802154_MODE_BEACONS:i==3U?UI_IEEE802154_MODE_ACTIVITY:UI_IEEE802154_MODE_ASSESSMENT));
    if(i==0U)ui_exit();}
 ieee802154_mode=UI_IEEE802154_MODE_MENU;route_tools(30,380);CHECK(lessons==1U);
 CHECK(allocations==freed);
}
static void navigation(const char *folder)
{
 CHECK(!mesh_ui);reset();memset(&locate_peer,0,sizeof(locate_peer));
 current_page=UI_PAGE_FIELD_MENU;tool_parent_page=UI_PAGE_OTHER;
 cerberus_field_menu_t menu={0};drawn_size=0;drawn[0]=0;
 CHECK(cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND)==ESP_OK);
 CHECK(cerberus_field_menu_draw(&menu,UI_WIDGET_THEME)==ESP_OK);
 CHECK(strstr(drawn,"MESH DETECTION")&&!strstr(drawn,"RADIO SENSE"));ppm(folder,"mesh-scan-entry");
 cerberus_field_action_t action=cerberus_field_menu_tap(&menu,30,232);
 CHECK(action==CERBERUS_FIELD_RADIO);unsigned before=allocations;
 route_field(action);CHECK(current_page==UI_PAGE_IEEE802154&&ieee802154_mode==UI_IEEE802154_MODE_MENU&&tool_parent_page==UI_PAGE_FIELD_MENU);
 CHECK(!mesh_ui&&!peer.wire_calls&&allocations==before);
 drawn_size=0;drawn[0]=0;
 CHECK(cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND)==ESP_OK&&draw_header("Mesh Detection")==ESP_OK);
 CHECK(sentinel_hub_draw(SENTINEL_HUB_RADIO,UI_WIDGET_THEME)==ESP_OK);
 CHECK(strstr(drawn,"Mesh Detection")&&strstr(drawn,"Channel energy")&&strstr(drawn,"Zigbee beacons")&&
    strstr(drawn,"Frame activity")&&strstr(drawn,"Compare snapshots"));
 CHECK(route_draw_footer()==ESP_OK&&strstr(drawn,"BACK TO SCAN"));ppm(folder,"mesh-tools-menu");
 for(unsigned i=0;i<5U;++i){
    route_tools(30,(uint16_t)(70U+i*54U));
    CHECK(ieee802154_mode==(i==0U?UI_IEEE802154_MODE_MESH:i==1U?UI_IEEE802154_MODE_ENERGY:
        i==2U?UI_IEEE802154_MODE_BEACONS:i==3U?UI_IEEE802154_MODE_ACTIVITY:UI_IEEE802154_MODE_ASSESSMENT));
    CHECK(!peer.wire_calls);
    if(i){drawn_size=0;drawn[0]=0;CHECK(route_draw_footer()==ESP_OK&&strstr(drawn,"MESH MENU")&&!strstr(drawn,"HOME"));}
    route_footer();
    if(mesh_ui)ui_step(20,false);
    CHECK(current_page==UI_PAGE_IEEE802154&&ieee802154_mode==UI_IEEE802154_MODE_MENU&&!mesh_ui);
 }
 route_footer();CHECK(current_page==UI_PAGE_FIELD_MENU&&!mesh_ui&&!peer.wire_calls);
 CHECK(allocations==freed);
}
static mesh_scan_t preserved_survey;
static mesh_ui_painted_t preserved_target;
static void locate_address(void)
{
 mesh_ui_tap(130,120);CHECK(draw_detail_page()==ESP_OK);mesh_ui_tap(20,155);
 CHECK(mesh_ui->detail&&mesh_ui->selected.value.kind==MESH_OBSERVATION_SELECT_ADDRESS);
 preserved_target=mesh_ui->selected;
}
static void locate_begin_ui(void)
{
 ui_begin(3U);locate_address();mesh_ui_tap(30,390);
 CHECK(mesh_ui->locate&&mesh_ui->locate_pending&&mesh_ui->locate_view&&!locate_peer.starts);
 for(unsigned i=0;i<1000U&&!locate_peer.bound;++i)ui_step(20,false);
 CHECK(locate_peer.bound&&!peer.bound&&mesh_ui->scan.frozen&&mesh_ui->scan.final_valid);
 CHECK(mesh_ui->locate->target.value==preserved_target.value.record.address.key.value&&
    mesh_ui->locate->origin_session==preserved_target.value.session);
 preserved_survey=mesh_ui->scan;
}
static void locate_until_sample(uint32_t sequence)
{
 for(unsigned i=0;i<1000U&&(!mesh_ui->locate->has_snapshot||mesh_ui->locate->snapshot.match_sequence!=sequence);++i)ui_step(20,false);
 CHECK(mesh_ui->locate->has_snapshot&&mesh_ui->locate->snapshot.match_sequence==sequence);
 CHECK(!memcmp(&preserved_survey,&mesh_ui->scan,sizeof(preserved_survey)));
}
static void locate_return_ui(void)
{
 route_back();for(unsigned i=0;i<1000U&&mesh_ui->locate_view;++i)ui_step(20,false);
 CHECK(!mesh_ui->locate&&!mesh_ui->locate_view&&mesh_ui->detail&&!locate_peer.bound);
 CHECK(!memcmp(&preserved_survey,&mesh_ui->scan,sizeof(preserved_survey))&&
    !memcmp(&preserved_target,&mesh_ui->selected,sizeof(preserved_target)));
}
static void locate_lifecycle(const char *folder)
{
 ui_begin(1U);locate_address();oom=true;unsigned stops=peer.stops;
 mesh_ui_tap(30,390);CHECK(!mesh_ui->locate&&peer.stops==stops&&peer.running&&mesh_ui->error==ESP_ERR_NO_MEM);
 CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"ESP_ERR_NO_MEM"));ppm(folder,"mesh-locate-no-memory");oom=false;ui_exit();
 ui_begin(1U);locate_address();c5_status.firmware_version=72;mesh_ui_tap(30,390);
 CHECK(!mesh_ui->locate&&peer.running);ui_exit();
 ui_begin(1U);locate_address();peer.stop_hold=30U;mesh_ui_tap(30,390);
 CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"STOPPING SURVEY"));ppm(folder,"mesh-locate-handoff");
 for(unsigned i=0;i<20U;++i)ui_step(700,false);CHECK(mesh_ui->locate_pending&&!locate_peer.starts&&mesh_ui_busy());
 mesh_ui_tap(30,400);CHECK(!mesh_ui->locate_pending);peer.stop_hold=0;ui_freeze();
 for(unsigned i=0;i<5U;++i)ui_step(700,false);CHECK(!locate_peer.starts);ui_exit();
 ui_begin(1U);locate_address();mesh_ui_tap(30,390);current_page=UI_PAGE_OTHER;
 for(unsigned i=0;i<1000U&&mesh_ui;++i)ui_step(20,false);
 CHECK(!mesh_ui&&!locate_peer.starts&&!peer.bound&&current_page==UI_PAGE_OTHER);
 ui_begin(1U);ui_freeze();locate_address();mesh_ui_tap(30,390);peer.step_us=9000;ui_step(0,false);
 CHECK(mesh_ui->locate&&!locate_peer.starts);route_back();peer.step_us=0;
 for(unsigned i=0;i<1000U&&mesh_ui->locate_view;++i)ui_step(20,false);
 CHECK(!locate_peer.starts&&!mesh_ui->locate&&mesh_ui->detail);ui_exit();
 ui_begin(1U);ui_freeze();locate_address();locate_peer.lose_start=true;mesh_ui_tap(30,390);ui_step(0,false);
 CHECK(locate_peer.bound&&locate_peer.starts==1U);route_back();
 for(unsigned i=0;i<1000U&&mesh_ui->locate_view;++i)ui_step(20,false);
 CHECK(!locate_peer.bound&&locate_peer.starts==1U&&!mesh_ui->locate);ui_exit();
 ui_begin(0U);add_row(0U);row_t *unknown=&peer.rows[peer.head];unknown->bytes[11]=5U;
 memset(unknown->bytes+14U,0,125U);const uint8_t unknown_mac[]={0x41,0xa0,7,1,2};
 memcpy(unknown->bytes+14U,unknown_mac,sizeof(unknown_mac));
 for(unsigned i=0;i<1000U&&!mesh_ui->scan.model.address_count;++i)ui_step(20,false);
 locate_address();CHECK(!preserved_target.value.record.address.key.pan.known);mesh_ui_tap(30,390);
 for(unsigned i=0;i<1000U&&!locate_peer.bound;++i)ui_step(20,false);CHECK(locate_peer.bound);
 preserved_survey=mesh_ui->scan;locate_peer_sample(-71,1U);locate_until_sample(1U);
 CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"UNKNOWN PAN / AMBIGUOUS"));ppm(folder,"mesh-locate-unknown-pan");
 locate_return_ui();ui_exit();
 ui_begin(1U);mesh_ui_tap(130,120);CHECK(draw_detail_page()==ESP_OK);mesh_ui_tap(20,197);
 CHECK(mesh_ui->detail&&!mesh_ui->selected.value.record.address.source_frames&&
    mesh_ui->selected.value.record.address.destination_frames);
 preserved_target=mesh_ui->selected;mesh_ui_tap(30,390);
 for(unsigned i=0;i<1000U&&!locate_peer.bound;++i)ui_step(20,false);CHECK(locate_peer.bound);
 preserved_survey=mesh_ui->scan;
 CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"NOT YET HEARD AS SOURCE"));
 locate_peer_sample(-48,1U);locate_until_sample(1U);CHECK(mesh_ui->locate->snapshot.address==preserved_target.value.record.address.key.value);
 locate_return_ui();ui_exit();
 ui_begin(0U);add_row(0U);row_t *extended=&peer.rows[peer.head];extended->bytes[11]=15U;extended->bytes[15]=0xc8U;
 le(extended->bytes+21U,UINT64_C(0xfedcba9876543210),8U);
 for(unsigned i=0;i<1000U&&!mesh_ui->scan.model.address_count;++i)ui_step(20,false);
 locate_address();mesh_ui_tap(30,390);
 for(unsigned i=0;i<1000U&&!locate_peer.bound;++i)ui_step(20,false);CHECK(locate_peer.bound);
 preserved_survey=mesh_ui->scan;locate_peer_sample(-128,1U);locate_until_sample(1U);
 CHECK(mesh_ui->locate->target.value==UINT64_C(0xfedcba9876543210));
 CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"FEDCBA9876543210")&&strstr(drawn,"-128 DBM"));ppm(folder,"mesh-locate-ext64");
 masked=true;CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"EXT64 MASKED")&&!strstr(drawn,"FEDCBA9876543210")&&!strstr(drawn,"1234"));
 masked=false;for(unsigned i=0;i<34U;++i)ui_step(100,false);
 mesh_ui_locate_toggle();for(unsigned i=0;i<1000U&&!mesh_ui->locate->frozen;++i)ui_step(20,false);
 CHECK(mesh_ui->locate->frozen&&!locate_peer.bound&&draw_detail_page()==ESP_OK&&
    strstr(drawn,"RETAINED EVIDENCE")&&strstr(drawn,"LAST SOURCE SAMPLE IS STALE"));
 unsigned original_starts=peer.starts;mesh_ui_locate_toggle();ui_step(0,false);
 CHECK(peer.starts==original_starts&&locate_peer.starts==2U&&!mesh_ui->locate->has_snapshot);
 locate_return_ui();ui_exit();
 locate_begin_ui();CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"NOT YET HEARD AS SOURCE")&&strstr(drawn,"RSSI --"));
 ppm(folder,"mesh-locate-waiting");locate_peer_sample(-50,1U);locate_until_sample(1U);
 CHECK(mesh_ui->locate->history_count==1U&&draw_detail_page()==ESP_OK&&strstr(drawn,"-50 DBM"));
 uint64_t age1=0,age2=0;CHECK(mesh_locate_age_upper(mesh_ui->locate,peer.clock,&age1));
 for(unsigned i=0;i<50U;++i)ui_step(100,false);
 CHECK(mesh_ui->locate->history_count==1U&&mesh_locate_age_upper(mesh_ui->locate,peer.clock,&age2)&&age2>age1);
 CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"FRESHNESS UNCONFIRMED")&&!strstr(drawn,"IS STALE"));ppm(folder,"mesh-locate-stale");
 for(unsigned i=0;i<35U;++i){locate_peer_sample((int8_t)(-90+(int)i),i==0U?5U:1U);locate_until_sample(locate_peer.current.match_sequence);}
 CHECK(mesh_ui->locate->history_count==32U&&mesh_ui->locate->coalesced_matches==4U);
 CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"LAST 32 DELIVERED SAMPLES"));ppm(folder,"mesh-locate-signal");
 masked=true;CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"SHORT16 MASKED")&&!strstr(drawn,"0201"));ppm(folder,"mesh-locate-masked");masked=false;
 locate_peer_sample(-60,1U);locate_peer.current.flags&=(uint8_t)~(CERBERUS_MESH_LOCATE_RSSI_VALID|CERBERUS_MESH_LOCATE_LQI_VALID);
 locate_peer.current.rssi=0;locate_peer.current.lqi=0;locate_until_sample(locate_peer.current.match_sequence);
 CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"RSSI --")&&strstr(drawn,"LQI UNKNOWN"));
 locate_peer_sample(-55,1U);locate_peer.current.flags&=(uint8_t)~CERBERUS_MESH_LOCATE_MATCH_TIME_VALID;
 locate_peer.current.last_match_us=0;locate_peer.invalid_clock=true;locate_until_sample(locate_peer.current.match_sequence);
 CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"SOURCE AGE UNKNOWN"));ppm(folder,"mesh-locate-unknown-age");
 locate_peer.stop_hold=30U;route_back();for(unsigned i=0;i<20U;++i)ui_step(700,false);
 CHECK(mesh_ui->locate_view&&locate_peer.bound&&mesh_ui_busy());locate_peer.stop_hold=0;locate_return_ui();ui_exit();
 locate_begin_ui();locate_peer.bad_part=true;
 for(unsigned i=0;i<1000U&&!mesh_ui->locate->faulted;++i)ui_step(20,false);
 CHECK(mesh_ui->locate->faulted&&locate_peer.bound);CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"FAULT / CLOSE REQUIRED"));
 ppm(folder,"mesh-locate-fault");locate_return_ui();ui_exit();
 locate_begin_ui();locate_peer.release_hold=30U;ui_step(20,true);
 for(unsigned i=0;i<20U;++i)ui_step(700,true);CHECK(mesh_ui&&mesh_ui->closing&&locate_peer.bound);
 locate_peer.release_hold=0;for(unsigned i=0;i<1000U&&mesh_ui;++i)ui_step(20,true);
 CHECK(!mesh_ui&&!locate_peer.bound&&allocations==freed);
}
static void locate_saving(const char *folder)
{
 locate_begin_ui();snprintf(mesh_ui->saved_name,sizeof(mesh_ui->saved_name),"M00001.MSH");
 locate_peer_sample(-63,2U);locate_until_sample(2U);unsigned starts=locate_peer.starts;
 mesh_ui_tap(220,400);CHECK(mesh_ui->locate_save_pending);
 for(unsigned i=0;i<1000U&&!mesh_ui->job;++i)ui_step(20,false);
 CHECK(mesh_ui->job&&mesh_ui->job_locate&&locate_io_copy.frozen&&!locate_peer.bound);
 mesh_ui_tap(30,400);CHECK(locate_peer.starts==starts);
 io.worker_active=false;io.state=CERBERUS_BLE_ARCHIVE_IO_ERROR;io.error=ESP_FAIL;ui_step(20,false);
 CHECK(!mesh_ui->job&&!mesh_ui->locate_saved_name[0]);mesh_ui_tap(220,400);ui_step(20,false);CHECK(mesh_ui->job);
 io.worker_active=false;io.state=CERBERUS_BLE_ARCHIVE_IO_COMPLETE;io.result_ready=true;io.format=CERBERUS_BLE_ARCHIVE_IO_FORMAT_MESH_REPORT;
 snprintf(io.file_name,sizeof(io.file_name),"WRONG.CSV");ui_step(20,false);
 CHECK(!mesh_ui->locate_saved_name[0]&&mesh_ui->error==ESP_ERR_INVALID_RESPONSE);
 mesh_ui_tap(220,400);ui_step(20,false);CHECK(mesh_ui->job);
 io.worker_active=false;io.state=CERBERUS_BLE_ARCHIVE_IO_COMPLETE;io.result_ready=true;snprintf(io.file_name,sizeof(io.file_name),"M00002.CSV");ui_step(20,false);
 CHECK(!strcmp(mesh_ui->locate_saved_name,"M00002.CSV")&&!strcmp(mesh_ui->saved_name,"M00001.MSH"));
 CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"OPEN CSV"));ppm(folder,"mesh-locate-saved");
 mesh_ui_tap(220,400);CHECK(current_page==UI_PAGE_FILES&&!strcmp(opened_name,"M00002.CSV"));
 for(unsigned i=0;i<1000U&&mesh_ui;++i)ui_step(20,false);CHECK(!mesh_ui);
 locate_begin_ui();mesh_ui_tap(220,400);for(unsigned i=0;i<1000U&&!mesh_ui->job;++i)ui_step(20,false);
 unsigned opened=file_opens,cancelled=io_cancels;route_back();CHECK(io_cancels==cancelled+1U);
 for(unsigned i=0;i<20U;++i)ui_step(700,false);CHECK(mesh_ui->locate&&mesh_ui->locate_return);
 io.worker_active=false;io.state=CERBERUS_BLE_ARCHIVE_IO_COMPLETE;io.result_ready=true;ui_step(20,false);
 CHECK(!mesh_ui->locate&&mesh_ui->detail&&file_opens==opened);ui_exit();CHECK(allocations==freed);
}
static void archive_complete_load(void)
{
 io.state=CERBERUS_BLE_ARCHIVE_IO_COMPLETE;io.result_ready=true;io.worker_active=false;io.result_ready=true;
 snprintf(io.file_name,sizeof(io.file_name),"%s",io_load_name);ui_step(20,false);
}
static void archive_close(void)
{
 if(mesh_archive_view&&mesh_archive_view->detail)mesh_archive_tap(30,450);
 mesh_archive_tap(30,450);CHECK(!mesh_archive_view&&current_page==UI_PAGE_FILES);
}
static void archive_workflow(const char *folder)
{
 /* The actual live controller supplies a complete released map with more than
  * 64 delivered witnesses; the codec roundtrip is used by the IO copy boundary. */
 ui_begin(0U);
 for(unsigned i=0U;i<80U;++i){
    add_row(0U);row_t*r=&peer.rows[peer.head];r->bytes[11]=125U;
    r->bytes[21]=(uint8_t)(i+1U);r->bytes[22]=(uint8_t)(i+2U);
    memset(r->bytes+23U,0xa5,116U);
    for(unsigned n=0;n<1000U&&mesh_ui->scan.model.counters.records<i+1U;++n)ui_step(20,false);
    CHECK(mesh_ui->scan.model.counters.records==i+1U);
 }
 peer.source_error=ESP_FAIL;ui_freeze();CHECK(mesh_ui->scan.model.witness_count==64U&&mesh_ui->scan.model.counters.witness_omissions==16U);
 unsigned wire=peer.wire_calls,starts=peer.starts;
 oom=true;mesh_ui_tap(260,400);ui_step(20,false);
 CHECK(mesh_ui->error==ESP_ERR_NO_MEM&&!mesh_ui->job&&mesh_ui->scan.frozen&&peer.wire_calls==wire);oom=false;
 mesh_ui_tap(260,400);ui_step(20,false);CHECK(mesh_ui->job&&io_copy.model.counters.records==80U);
 io.worker_active=false;io.state=CERBERUS_BLE_ARCHIVE_IO_COMPLETE;io.result_ready=true;
 snprintf(io.file_name,sizeof(io.file_name),"M00042.MSH");ui_step(20,false);
 CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"OPEN MAP"));ppm(folder,"mesh-map-saved");
 mesh_ui_tap(260,400);CHECK(current_page==UI_PAGE_MESH_ARCHIVE&&mesh_archive_view&&mesh_archive_view->job);
 ui_step(20,false);CHECK(!mesh_ui);archive_complete_load();
 CHECK(mesh_archive_view->has_document&&!mesh_archive_view->job&&mesh_archive_view->document.model.counters.records==80U);
 CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"HISTORICAL")&&strstr(drawn,"RAW FIRST 64/64")&&!strstr(drawn,"START")&&!strstr(drawn,"LOCATE"));
 CHECK(strstr(drawn,"SOURCE ERROR")&&mesh_archive_view->document.final_summary.error_code==ESP_FAIL);
 ppm(folder,"mesh-map-pans");
 uint8_t source_flags=mesh_archive_view->document.final_summary.flags;
 mesh_archive_view->document.final_summary.flags|=CERBERUS_MESH_OBSERVATION_COUNTERS_SATURATED;
 CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"C5 >= DROP")&&strstr(drawn,"VALUES ARE BOUNDS"));
 ppm(folder,"mesh-map-source-bounds");mesh_archive_view->document.final_summary.flags=source_flags;
 mesh_observation_counters_t saved_counters=mesh_archive_view->document.model.counters;
 mesh_archive_view->document.model.counters.pan_omissions=UINT32_MAX;
 mesh_archive_view->document.model.counters.address_omissions=UINT32_MAX;
 mesh_archive_view->document.model.counters.edge_omissions=UINT32_MAX;
 mesh_archive_view->document.model.counters.witness_omissions=UINT32_MAX;
 mesh_archive_view->document.model.counters.malformed=UINT32_MAX;
 mesh_archive_view->document.model.counters.unsupported=UINT32_MAX;
 CHECK(draw_detail_page()==ESP_OK);ppm(folder,"mesh-map-limits");
 mesh_archive_view->document.model.counters=saved_counters;
 mesh_archive_tap(130,120);CHECK(mesh_archive_view->count==64U&&mesh_archive_view->painted_count==4U);
 ppm(folder,"mesh-map-addresses");
 mesh_ui_painted_t proof=mesh_archive_view->painted[0];
 uint64_t old=mesh_archive_view->document.model.addresses[0].key.value;
 mesh_archive_view->document.model.addresses[0].key.value=0xdeadU;
 mesh_archive_tap(20,155);CHECK(mesh_archive_view->detail&&!memcmp(&proof,&mesh_archive_view->selected,sizeof(proof)));
 mesh_archive_view->document.model.addresses[0].key.value=old;
 CHECK(strstr(drawn,"HISTORICAL SELECTED")&&!strstr(drawn,"LOCATE"));ppm(folder,"mesh-map-detail");
 mesh_archive_tap(120,390);CHECK(mesh_archive_view->raw_page==1U&&strstr(drawn,"RAW PAGE 2 / 2"));ppm(folder,"mesh-map-raw-last-page");
 masked=true;CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"HIDDEN")&&!strstr(drawn,"A5 A5"));
 CHECK(!strstr(drawn,"LOCATE"));ppm(folder,"mesh-map-private");masked=false;
 mesh_archive_tap(30,450);CHECK(!mesh_archive_view->detail);
 fail_after=calls+8U;CHECK(draw_detail_page()!=ESP_OK&&!mesh_archive_view->painted_count);fail_after=0;
 mesh_archive_tap(20,155);CHECK(!mesh_archive_view->detail);CHECK(draw_detail_page()==ESP_OK);
 for(unsigned i=0;i<15U;++i)mesh_archive_tap(90,400);
 CHECK(mesh_archive_view->page==15U&&mesh_archive_view->painted_count==4U);ppm(folder,"mesh-map-last-address-page");
 mesh_archive_tap(250,120);CHECK(strstr(drawn,"TRAFFIC"));ppm(folder,"mesh-map-traffic");
 /* Every failure keeps the last published historical map and its name. */
 io_load_source=mesh_archive_view->document;
 const unsigned wrong_formats[]={CERBERUS_BLE_ARCHIVE_IO_FORMAT_MESH_REPORT,CERBERUS_BLE_ARCHIVE_IO_FORMAT_MESH_LOCATE_REPORT};
 for(unsigned i=0;i<2U;++i){mesh_archive_open_file(file_path,"WRONG.MSH");io.format=wrong_formats[i];archive_complete_load();
    CHECK(mesh_archive_view->error==ESP_ERR_INVALID_RESPONSE&&!memcmp(&io_load_source,&mesh_archive_view->document,sizeof(io_load_source)));
    CHECK(!strcmp(mesh_archive_view->name,"M00042.MSH"));}
 mesh_archive_open_file(file_path,"BAD.MSH");io_map_bytes[0]^=1U;archive_complete_load();io_map_bytes[0]^=1U;
 CHECK(mesh_archive_view->error==ESP_ERR_INVALID_RESPONSE&&!memcmp(&io_load_source,&mesh_archive_view->document,sizeof(io_load_source)));
 ppm(folder,"mesh-map-load-error");
 mesh_archive_open_file(file_path,"COPY.MSH");io_copy_fail=true;archive_complete_load();io_copy_fail=false;
 CHECK(mesh_archive_view->has_document&&!memcmp(&io_load_source,&mesh_archive_view->document,sizeof(io_load_source)));
 mesh_archive_open_file(file_path,"MISSING.MSH");io.job_id++;ui_step(20,false);
 CHECK(!mesh_archive_view->job&&mesh_archive_view->error==ESP_ERR_NOT_FOUND);
 io_start_error=ESP_FAIL;mesh_archive_open_file(file_path,"FAIL.MSH");CHECK(!mesh_archive_view->job&&mesh_archive_view->has_document);io_start_error=ESP_OK;
 /* Export is a separate file result; it cannot publish a typed map. */
 mesh_archive_tap(220,400);CHECK(mesh_archive_view->job&&mesh_archive_view->exporting);
 CHECK(!memcmp(&io_load_source,&mesh_archive_view->document,sizeof(io_load_source)));
 io.worker_active=false;io.state=CERBERUS_BLE_ARCHIVE_IO_ERROR;io.error=ESP_FAIL;ui_step(20,false);
 CHECK(!mesh_archive_view->csv_name[0]&&mesh_archive_view->has_document);
 mesh_archive_tap(220,400);io.worker_active=false;io.state=CERBERUS_BLE_ARCHIVE_IO_COMPLETE;io.result_ready=true;
 io.format=CERBERUS_BLE_ARCHIVE_IO_FORMAT_MESH_ARCHIVE;ui_step(20,false);CHECK(!mesh_archive_view->csv_name[0]);
 mesh_archive_tap(220,400);io.worker_active=false;io.state=CERBERUS_BLE_ARCHIVE_IO_COMPLETE;io.result_ready=true;
 snprintf(io.file_name,sizeof(io.file_name),"M00043.CSV");ui_step(20,false);
 CHECK(!strcmp(mesh_archive_view->csv_name,"M00043.CSV")&&strstr(drawn,"OPEN CSV"));ppm(folder,"mesh-map-exported");
 mesh_archive_tap(220,400);CHECK(!mesh_archive_view&&current_page==UI_PAGE_FILES&&!strcmp(opened_name,"M00043.CSV"));
 CHECK(peer.wire_calls==wire&&peer.starts==starts&&!peer.bound&&!locate_peer.bound);
 /* Real Files dispatch, no-radio OOM, exact pending-job cancel on Back/hide/sleep. */
 unsigned loads=io_loads;
 route_file("not-a-map.csv");CHECK(io_loads==loads&&!mesh_archive_view);
 snprintf(file_path,sizeof(file_path),"/CERBERUS/MESH");oom=true;route_file("M00042.mSh");
 CHECK(!mesh_archive_view&&current_page==UI_PAGE_MESH_ARCHIVE&&strstr(drawn,"NO PSRAM"));
 mesh_archive_tap(30,450);CHECK(current_page==UI_PAGE_FILES);oom=false;
 for(unsigned how=0;how<3U;++how){
    route_file("M00042.MSH");CHECK(mesh_archive_view&&mesh_archive_view->job);
    unsigned cancels=io_cancels,releases=io_releases,opened=file_opens;
    if(how==0U)mesh_archive_tap(30,450);
    else{if(how==1U)current_page=UI_PAGE_OTHER;ui_step(20,how==2U);}
    CHECK(!mesh_archive_view&&io_cancels==cancels+1U&&io_releases==releases+1U);
    io.worker_active=false;io.state=CERBERUS_BLE_ARCHIVE_IO_COMPLETE;io.result_ready=true;ui_step(20,false);
    CHECK(!mesh_archive_view&&file_opens==opened&&current_page!=(ui_page_t)UI_PAGE_MESH_ARCHIVE);
 }
 route_file("M00042.MSH");archive_complete_load();mesh_archive_tap(220,400);CHECK(mesh_archive_view->job);
 archive_close();CHECK(!mesh_archive_view);
 /* SAVED is disabled while a receiver or SD job is owned, and remains useful
  * from the stopped initial shell without starting a radio. */
 ui_reset();unsigned listed=listings;mesh_ui_tap(270,80);
 CHECK(current_page==UI_PAGE_FILES&&listings==listed+1U&&!strcmp(file_path,"/CERBERUS/MESH"));
 /* The real empty-list renderer has no selectable fake file. Errors and all
  * other paths suppress this first-use message; actual refresh logic is tested
  * separately by the directory-memory fixture against the storage boundary. */
 drawn_size=0;drawn[0]=0;file_entry_count=0;storage_last_error=ESP_OK;
 CHECK(cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND)==ESP_OK);
 CHECK(draw_header("FILES")==ESP_OK);
 CHECK(draw_mesh_archive_empty_list()==ESP_OK&&strstr(drawn,"NO SAVED MAPS")&&
    strstr(drawn,"SAVE A MESH SURVEY TO KEEP IT HERE"));
 CHECK(route_draw_footer()==ESP_OK);ppm(folder,"mesh-map-empty-saved");
 drawn_size=0;drawn[0]=0;file_entry_count=1;
 CHECK(draw_mesh_archive_empty_list()==ESP_OK&&!drawn_size);
 file_entry_count=0;storage_last_error=ESP_FAIL;
 CHECK(draw_mesh_archive_empty_list()==ESP_OK&&!drawn_size);
 storage_last_error=ESP_OK;snprintf(file_path,sizeof(file_path),"/CERBERUS/MESH/CHILD");
 CHECK(draw_mesh_archive_empty_list()==ESP_OK&&!drawn_size);
 snprintf(file_path,sizeof(file_path),"%s",CERBERUS_STORAGE_MESH_ARCHIVE_PATH);
 fail_after=calls+1U;CHECK(draw_mesh_archive_empty_list()==ESP_FAIL);fail_after=0;
 tool_parent_page=UI_PAGE_FIELD_MENU;listed=listings;route_footer();
 CHECK(current_page==UI_PAGE_FIELD_MENU&&listings==listed&&!peer.wire_calls);
 ui_step(20,false);CHECK(!mesh_ui&&!peer.wire_calls);
 ui_begin(1U);mesh_ui_tap(270,80);CHECK(current_page==UI_PAGE_IEEE802154&&mesh_ui_busy());ui_exit();
 /* A valid empty failed capture is not displayed as a clean empty network.
  * Source saturation/clock flags remain separate from successful file loading. */
 ui_begin(0U);peer.source_error=ESP_FAIL;peer.no_start_time=true;ui_freeze();
 mesh_ui->scan.final_summary.tuned_mask=1U;
 mesh_ui->scan.final_summary.callbacks=UINT32_MAX;
 mesh_ui->scan.final_summary.invalid_callbacks=UINT32_MAX;
 mesh_ui->scan.final_summary.flags|=CERBERUS_MESH_OBSERVATION_COUNTERS_SATURATED|CERBERUS_MESH_OBSERVATION_SUMMARY_CLOCK_INVALID;
 CHECK(mesh_report_valid(&mesh_ui->scan)&&!mesh_ui->scan.model.counters.saturated);
 mesh_ui_tap(260,400);ui_step(20,false);CHECK(mesh_ui->job);
 io.worker_active=false;io.state=CERBERUS_BLE_ARCHIVE_IO_COMPLETE;io.result_ready=true;
 snprintf(io.file_name,sizeof(io.file_name),"M00044.MSH");ui_step(20,false);mesh_ui_tap(260,400);
 ui_step(20,false);CHECK(!mesh_ui);archive_complete_load();
 CHECK(mesh_archive_view->has_document&&!mesh_archive_view->document.model.counters.records&&
    mesh_archive_view->error==ESP_OK&&strstr(drawn,"SOURCE ERROR")&&strstr(drawn,"C5 CLOCK FAULT")&&
    strstr(drawn,"PARTIAL CHANNELS")&&strstr(drawn,"VALUES ARE BOUNDS")&&strstr(drawn,"C5 >= DROP"));
 ppm(folder,"mesh-map-empty-source-error");archive_close();
 CHECK(allocations==freed);
}
int main(int argc,char**argv)
{CHECK(argc==2);lifecycle();saving();render(argv[1]);large_model(argv[1]);navigation(argv[1]);locate_lifecycle(argv[1]);locate_saving(argv[1]);archive_workflow(argv[1]);
 printf("Mesh UI: %u checks PASS; workspace %zu / painted %zu / optional Locate %zu / historical %zu bytes\n",checks,
    sizeof(mesh_ui_store_t),sizeof(mesh_ui_painted_t),sizeof(mesh_locate_t),sizeof(mesh_archive_ui_store_t));return 0;}
