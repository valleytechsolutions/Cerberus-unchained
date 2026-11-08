#include "frame_framebuffer.h"
#include "frame_modes.h"
#include "wifi_frame_session.h"
#include "c5_spi_link.h"
#include "ui_widgets.h"
#include "sentinel_hub.h"
#include "esp_heap_caps.h"

#define UI_BACKGROUND 0x0042
#define UI_PANEL 0x08A4
#define UI_MUTED 0x7CB5
#define UI_TEXT 0xE77F
#define UI_GOOD 0x471B
#define UI_BAD 0xF471
#define TAG "frame-ui"
#define CHECK(x) do { ++checks; assert(x); } while (0)
#define ESP_ERROR_CHECK(x) CHECK((x)==ESP_OK)
static unsigned checks,allocations,freed,starts,stops,releases,old_stops,generic_starts,generic_stops,ignored_stops;
static bool oom,masked,hold_stop,prior_live,prior_dual,prior_capture,c5_online=true;
static esp_err_t start_error,read_error,page_error,release_error;
static uint32_t now_ms;
enum {UI_PAGE_WIFI,UI_PAGE_OTHER,UI_PAGE_FIELD_MENU,SENTINEL_SCAN_NONE,SENTINEL_SCAN_WIFI};
static unsigned current_page=UI_PAGE_WIFI;
static ui_wifi_mode_t wifi_mode=UI_WIFI_MODE_TARGET;
static struct {uint32_t firmware_version;} c5_status={88};
static cerberus_c5_wifi_result_t wifi_selected_result;
static uint8_t wifi_selected_index;
static bool wifi_selection_valid,wifi_attack_selecting,frame_ui_selecting,dual_wifi_return_to_view;
#include "wifi_targets.h"
static unsigned picker_begins;
static uint32_t live_last_poll;
static bool picker_paused;
static void sentinel_scan_session_select_snapshot(int*s,unsigned tool){(void)s;CHECK(tool==SENTINEL_SCAN_WIFI);++picker_begins;}
static void sentinel_scan_session_pause(int*s,bool pause){(void)s;picker_paused=pause;}
static int live_session,live_ble_session;
static struct {const char *tools_label,*hint;} live_view;
static cerberus_s3_frame_status_t model;
static cerberus_s3_frame_row_t rows[16];
static cerberus_s3_capture_status_t generic;
static const uint8_t ap[6]={2,0x10,0x20,0x30,0x40,0x50};
static const cerberus_ui_widget_theme_t theme={UI_BACKGROUND,UI_PANEL,UI_GOOD,UI_TEXT,UI_MUTED};
#define UI_WIDGET_THEME (&theme)
static bool eapol_ui_previous_released(void){return !prior_live&&!prior_dual&&!prior_capture;}
static void dual_wifi_live_cancel_handoff(void){++old_stops;}
static void dual_wifi_ui_leave(void){++old_stops;}
static void sentinel_scan_session_select(int*s,unsigned mode){(void)s;CHECK(mode==SENTINEL_SCAN_NONE);++old_stops;}
void *heap_caps_calloc(size_t n,size_t size,unsigned caps)
{CHECK(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));if(oom)return NULL;void*p=calloc(n,size);CHECK(p);++allocations;return p;}
void heap_caps_free(void*p){if(p){++freed;free(p);}}
static bool privacy_addresses_masked(void){return masked;}
static void cerberus_privacy_format_wifi_identifier(const uint8_t*a,bool mask,char*out,size_t n)
{if(mask)snprintf(out,n,"**:**:**:**:**:**");else snprintf(out,n,"%02X:%02X:%02X:%02X:%02X:%02X",a[0],a[1],a[2],a[3],a[4],a[5]);}
static const char *esp_err_to_name(esp_err_t e)
{return e==ESP_ERR_TIMEOUT?"ESP_ERR_TIMEOUT":e==ESP_ERR_NO_MEM?"ESP_ERR_NO_MEM":e==ESP_ERR_INVALID_STATE?"ESP_ERR_INVALID_STATE":"ESP_FAIL";}
static esp_err_t draw_centered(uint16_t y,const char*text,uint16_t color,uint8_t scale)
{CHECK(strlen(text)*6U*scale<=320U);return cerberus_display_draw_text((uint16_t)((320U-strlen(text)*6U*scale)/2U),y,text,color,UI_BACKGROUND,scale);}
esp_err_t cerberus_s3_frame_start(const cerberus_s3_frame_options_t*o)
{CHECK(!prior_live&&!prior_dual&&!prior_capture&&!model.capture.busy);++starts;
 if(start_error)return start_error;model=(cerberus_s3_frame_status_t){.generation=starts,.channel=o->channel,.mode_mask=o->mode_mask,
 .filter_bssid=o->filter_bssid,.planned=o->plan!=NULL,.capture={.generation=starts,.state=CERBERUS_S3_CAPTURE_RUNNING,
 .busy=true,.save_pcap=o->save_pcap,.channel=o->channel,.mode_mask=o->mode_mask,.planned=o->plan!=NULL}};
 memcpy(model.bssid,o->bssid,6);if(o->plan){CHECK(cerberus_wifi_capture_plan_valid(o->plan)&&o->plan->dwell_ms==(o->mode_mask==CERBERUS_WIFI_CAPTURE_MODE_RAW?100:250));model.plan=*o->plan;}return ESP_OK;}
void cerberus_s3_frame_stop(void)
{++stops;if(ignored_stops){--ignored_stops;return;}if(model.capture.busy){model.capture.state=CERBERUS_S3_CAPTURE_STOPPING;
 if(!hold_stop){model.capture.busy=false;model.capture.state=model.capture.save_pcap?CERBERUS_S3_CAPTURE_SAVED:CERBERUS_S3_CAPTURE_COMPLETE;}}}
esp_err_t cerberus_s3_frame_status(cerberus_s3_frame_status_t*out)
{memset(out,0,sizeof(*out));if(read_error)return read_error;*out=model;return ESP_OK;}
esp_err_t cerberus_s3_frame_page(uint16_t offset,cerberus_s3_frame_row_t*out,size_t n,cerberus_s3_frame_page_t*page)
{CHECK(n==2);memset(out,0,n*sizeof(*out));*page=(cerberus_s3_frame_page_t){0};if(page_error)return page_error;
 page->generation=model.generation;page->total=model.summary.count;
 for(unsigned i=0;i<n&&offset+i<page->total;++i){out[i]=rows[offset+i];++page->copied;}return ESP_OK;}
esp_err_t cerberus_s3_frame_release(void)
{++releases;if(release_error)return release_error;if(model.capture.busy)return ESP_ERR_INVALID_STATE;memset(&model,0,sizeof(model));return ESP_OK;}
void cerberus_s3_capture_status(cerberus_s3_capture_status_t*out)
{*out=model.capture.busy?model.capture:generic;if(prior_capture)out->busy=true;}
esp_err_t cerberus_s3_capture_start(uint8_t channel,uint8_t mode,const uint8_t*bssid,bool filter)
{CHECK(channel==6&&!memcmp(bssid,ap,6));CHECK(filter==(mode!=CERBERUS_WIFI_CAPTURE_MODE_PROBE));++generic_starts;
 generic=(cerberus_s3_capture_status_t){.state=CERBERUS_S3_CAPTURE_RUNNING,.busy=true,.mode_mask=mode};return ESP_OK;}
void cerberus_s3_capture_stop(void){++generic_stops;generic.busy=false;generic.state=CERBERUS_S3_CAPTURE_SAVED;}
static bool frame_ui_is_open(void);
static bool frame_ui_busy(void);
static bool frame_ui_pending(void);
static void frame_ui_open(bool menu,uint8_t filter_index);
static void frame_ui_close(bool back);
static esp_err_t frame_ui_draw(void);
static void frame_ui_tap(uint16_t x,uint16_t y);
static bool frame_ui_tick(uint32_t now,bool visible);
/* Other monitor families have independent production UI suites. */
static bool sae_opened,sae_ui_selecting,eapol_opened,eapol_ui_selecting;
static void sae_ui_selection_done(void){CHECK(false);}
static void eapol_ui_selection_done(void){CHECK(false);}
static bool sae_ui_is_open(void){return sae_opened;}
static bool sae_ui_busy(void){return false;}
static bool sae_ui_pending(void){return false;}
static void sae_ui_open(bool menu){(void)menu;CHECK(!frame_ui_is_open());sae_opened=true;}
static void sae_ui_close(bool back){CHECK(back);sae_opened=false;wifi_mode=UI_WIFI_MODE_TARGET;}
static esp_err_t sae_ui_draw(void){return ESP_OK;}
static void sae_ui_tap(uint16_t x,uint16_t y){(void)x;(void)y;CHECK(false);}
static bool sae_ui_tick(uint32_t now,bool visible){(void)now;(void)visible;return false;}
static bool eapol_ui_is_open(void){return eapol_opened;}
static bool eapol_ui_busy(void){return false;}
static bool eapol_ui_pending(void){return false;}
static void eapol_ui_open(bool menu){(void)menu;CHECK(!frame_ui_is_open());eapol_opened=true;}
static void eapol_ui_close(bool back){CHECK(back);eapol_opened=false;wifi_mode=UI_WIFI_MODE_TARGET;}
static esp_err_t eapol_ui_draw(void){return ESP_OK;}
static void eapol_ui_tap(uint16_t x,uint16_t y){(void)x;(void)y;CHECK(false);}
static bool eapol_ui_tick(uint32_t now,bool visible){(void)now;(void)visible;return false;}
#include "frame_picker_begin.h"
#include "wifi_capture_ui.h"
#include "wifi_frame_ui.h"
#define SENTINEL_VIEW_VISIBLE_ROWS 4U
enum {SENTINEL_VIEW_ROW0};
#include "frame_picker_types.h"
static live_scan_record_t live_records[SENTINEL_SCAN_WIFI+1];
static live_wifi_paint_map_t live_wifi_built,live_wifi_painted;
static uint64_t wifi_relation_publication;
static uint8_t wifi_relation_generation;
static struct {cerberus_c5_wifi_result_t wifi[32];} picker_bank,*live_store=&picker_bank;
static struct {unsigned painted_count,built_count;} *ble_discovery_live;
static char live_hint[52];
static uint32_t live_frame;
static unsigned picker_refreshes;
static esp_err_t picker_paint_error;
static bool ble_discovery_presenting(void){return false;}
static esp_err_t sentinel_scan_view_draw(const void*v,unsigned frame){(void)v;(void)frame;return picker_paint_error;}
static esp_err_t sentinel_scan_view_update(const void*v,unsigned frame){(void)v;(void)frame;return picker_paint_error;}
static void live_make_view(void){++picker_refreshes;}
#include "frame_picker.h"
static esp_err_t draw_detail_page(void);
static esp_err_t draw_current_page(void){return draw_detail_page();}
#include "frame_routes.h"
static esp_err_t draw_detail_page(void)
{drawn_size=0;drawn[0]=0;esp_err_t e=cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND);
 if(!e)e=draw_centered(20,"WI-FI TOOLS",UI_TEXT,2);if(!e&&wifi_mode==UI_WIFI_MODE_CAPTURE)e=capture_draw();
 if(!e&&wifi_mode==UI_WIFI_MODE_RX_TOOLS)e=rx_tools_draw();
 return e?e:cerberus_ui_widget_draw_button((cerberus_ui_rect_t){8,420,304,44},frame_ui_is_open()?frame_ui_back_label():route_rx_back_label(),2,true,UI_WIDGET_THEME);}
static void tick(unsigned n,bool asleep){for(unsigned i=0;i<n;++i){now_ms+=25;route_tick(now_ms,asleep);}}
static void reset(void)
{CHECK(!frame_ui_is_open()&&!frame_ui&&!model.capture.busy);memset(&generic,0,sizeof(generic));memset(&model,0,sizeof(model));memset(rows,0,sizeof(rows));wifi_targets_clear();frame_ui_selecting=wifi_attack_selecting=false;picker_begins=0;
 oom=masked=hold_stop=prior_live=prior_dual=prior_capture=false;c5_online=true;c5_status.firmware_version=88;
 start_error=read_error=page_error=release_error=ESP_OK;starts=stops=releases=old_stops=generic_starts=generic_stops=ignored_stops=0;now_ms=1000;fail_after=0;
 current_page=UI_PAGE_WIFI;wifi_mode=UI_WIFI_MODE_CAPTURE;wifi_selected_result=(cerberus_c5_wifi_result_t){.ssid="Field Kit",.channel=6,.rssi=-35};memcpy(wifi_selected_result.bssid,ap,6);wifi_selected_index=7;wifi_selection_valid=true;capture_enter();}
static void enter(bool menu,unsigned filter)
{reset();if(menu){wifi_mode=UI_WIFI_MODE_MENU;route_menu(270,380);CHECK(wifi_mode==UI_WIFI_MODE_RX_TOOLS&&!starts&&!frame_ui_is_open());
 CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"RX TOOLS")&&strstr(drawn,"WIFI MENU"));route_tap(100,140);}
 else{capture_mode_index=(uint8_t)filter;CHECK(draw_detail_page()==ESP_OK);}
 CHECK(frame_ui_is_open()&&!starts&&!old_stops&&frame_ui->save);tick(2,false);}
static void start(void){route_tap(100,390);tick(2,false);CHECK(starts==1&&model.capture.busy);}
static void leave(void){bool menu=frame_ui_from_menu;route_back();tick(3,false);CHECK(!frame_ui_is_open()&&!frame_ui&&!model.capture.busy&&wifi_mode==(menu?UI_WIFI_MODE_RX_TOOLS:UI_WIFI_MODE_TARGET));}
static void choose_scope(unsigned scope)
{for(unsigned i=0;i<6&&frame_ui->scope!=scope;++i)route_tap(100,250);CHECK(frame_ui->scope==scope);}
static void choose_filter(unsigned filter)
{for(unsigned i=0;i<3&&frame_ui->filter!=filter;++i)route_tap(100,215);CHECK(frame_ui->filter==filter);}
static void tests(void)
{
 for(unsigned filter=0;filter<3;++filter){enter(false,filter);CHECK(frame_ui->scope==FRAME_SCOPE_AP&&frame_ui->filter==filter);start();
 CHECK(model.mode_mask==frame_ui_modes[filter]&&model.capture.save_pcap&&model.channel==6&&model.filter_bssid==(filter!=1));
 CHECK(filter==1||!memcmp(model.bssid,ap,6));leave();}
 for(unsigned filter=0;filter<3;++filter)for(unsigned scope=0;scope<3;++scope){enter(true,0);choose_filter(filter);choose_scope(scope);start();
 CHECK(model.planned&&!model.filter_bssid&&!model.plan.target_count&&model.plan.channel_mask==cerberus_wifi_capture_plan_band_mask(scope));leave();}
 enter(true,0);choose_scope(FRAME_SCOPE_FIXED);route_tap(20,285);CHECK(frame_ui->channel==cerberus_wifi_capture_plan_channel(CERBERUS_WIFI_CAPTURE_PLAN_CHANNELS-1));
 route_tap(250,285);CHECK(frame_ui->channel==1);route_tap(250,285);start();CHECK(!model.planned&&model.channel==2&&!model.filter_bssid);leave();
 for(unsigned filter=0;filter<3;++filter){enter(true,0);choose_filter(filter);choose_scope(FRAME_SCOPE_SELECTED);route_tap(100,390);CHECK(!starts&&frame_ui->error==ESP_ERR_INVALID_ARG);
 route_tap(100,285);CHECK(frame_ui_selecting&&wifi_attack_selecting&&picker_begins==1&&picker_paused&&!live_last_poll&&!starts);tick(3,false);CHECK(frame_ui_is_open()&&!frame_ui->exit);
 cerberus_c5_wifi_result_t a=wifi_selected_result;CHECK(wifi_targets_add(&a,0));a.channel=149;CHECK(wifi_targets_add(&a,1));route_picker_done();CHECK(!frame_ui_selecting&&!wifi_attack_selecting&&wifi_mode==UI_WIFI_MODE_CAPTURE);
 prior_live=true;route_tap(100,390);CHECK(frame_ui->plan.target_count==(filter==1?0:1));CHECK(frame_ui->plan.channel_mask==(cerberus_wifi_capture_plan_channel_bit(6)|cerberus_wifi_capture_plan_channel_bit(149)));
 wifi_targets_clear();frame_ui_open(false,2);tick(3,false);CHECK(!starts&&frame_ui_pending());prior_live=false;tick(3,false);CHECK(starts==1&&model.plan.target_count==(filter==1?0:1));leave();}
 enter(false,0);dual_wifi_return_to_view=true;frame_ui->target_dual_return=true;choose_scope(FRAME_SCOPE_SELECTED);route_tap(100,285);wifi_selected_index=20;wifi_selected_result.channel=149;
 route_picker_back();leave();CHECK(wifi_selected_index==7&&wifi_selected_result.channel==6&&dual_wifi_return_to_view);
 enter(false,0);memset(wifi_selected_result.bssid,0xff,6);wifi_selected_result.channel=149;snprintf(wifi_selected_result.ssid,33,"Changed AP");capture_enter();CHECK(capture_target.channel==6);
 start();CHECK(model.channel==6&&!memcmp(model.bssid,ap,6));CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"Field Kit")&&!strstr(drawn,"Changed AP"));leave();
 enter(true,0);prior_live=prior_dual=prior_capture=true;route_tap(100,390);tick(8,false);CHECK(!starts&&frame_ui_pending());route_back();route_tap(100,390);tick(8,false);CHECK(!starts&&frame_ui_is_open());
 prior_live=prior_dual=prior_capture=false;tick(3,false);CHECK(!starts&&!frame_ui_is_open());
 enter(true,0);start();ignored_stops=1;route_tap(100,390);CHECK(model.capture.busy&&frame_ui_pending());tick(2,false);CHECK(!model.capture.busy&&!frame_ui_pending());leave();
 enter(true,0);start();ignored_stops=2;route_back();tick(1,false);CHECK(model.capture.busy&&frame_ui_is_open());tick(2,false);CHECK(!model.capture.busy&&!frame_ui_is_open());
 enter(true,0);start();hold_stop=true;route_back();tick(5,false);CHECK(frame_ui_is_open()&&model.capture.busy);hold_stop=false;tick(3,false);CHECK(!frame_ui_is_open());
 enter(true,0);start();hold_stop=true;current_page=UI_PAGE_OTHER;tick(5,false);CHECK(frame_ui_is_open());hold_stop=false;tick(3,false);CHECK(!frame_ui_is_open()&&current_page==UI_PAGE_OTHER);
 enter(true,0);start();hold_stop=true;tick(4,true);CHECK(model.capture.busy);hold_stop=false;tick(3,true);CHECK(!model.capture.busy&&frame_ui_is_open());tick(3,false);CHECK(starts==1);leave();
 enter(true,0);prior_live=true;route_tap(100,390);tick(2,true);prior_live=false;tick(3,false);CHECK(!starts&&!frame_ui_pending());leave();
 enter(true,0);choose_scope(FRAME_SCOPE_SELECTED);route_tap(100,285);current_page=UI_PAGE_OTHER;tick(3,false);CHECK(!frame_ui_is_open()&&!frame_ui_selecting&&!wifi_attack_selecting);
 enter(true,0);start();model.summary.received=123;route_tap(100,390);tick(2,false);CHECK(frame_ui->status.summary.received==123&&!model.capture.busy);
 route_tap(100,390);tick(2,false);CHECK(starts==2&&!model.summary.received);leave();
 enter(true,0);route_tap(100,318);CHECK(!frame_ui->save&&draw_detail_page()==ESP_OK&&strstr(drawn,"NO SD CARD REQUIRED"));start();CHECK(!model.capture.save_pcap);
 route_tap(100,318);CHECK(!frame_ui->save);leave();
 enter(false,0);release_error=ESP_ERR_TIMEOUT;route_tap(100,160);tick(3,false);CHECK(frame_ui_is_open()&&capture_mode_index==0);route_back();release_error=ESP_OK;tick(3,false);CHECK(!frame_ui_is_open()&&wifi_mode==UI_WIFI_MODE_TARGET);
 enter(false,1);route_tap(100,160);tick(3,false);CHECK(!frame_ui_is_open()&&capture_mode_index==3&&eapol_opened&&!generic_starts);route_back();CHECK(!eapol_opened);
 enter(true,0);c5_status.firmware_version=87;route_tap(100,390);tick(2,false);CHECK(!starts&&draw_detail_page()==ESP_OK&&strstr(drawn,"CAPABILITY 88"));leave();
 enter(true,0);start_error=ESP_ERR_NO_MEM;route_tap(100,390);tick(2,false);CHECK(!model.capture.busy&&draw_detail_page()==ESP_OK&&strstr(drawn,"ESP_ERR_NO_MEM"));leave();
 reset();oom=true;wifi_mode=UI_WIFI_MODE_RX_TOOLS;route_tap(100,140);CHECK(frame_ui_is_open()&&!frame_ui&&draw_detail_page()==ESP_OK);route_back();CHECK(!frame_ui_is_open());
 reset();oom=true;wifi_mode=UI_WIFI_MODE_RX_TOOLS;route_tap(100,140);oom=false;route_tap(100,390);tick(3,false);CHECK(starts==1);leave();
 enter(true,0);const uint16_t misses[][2]={{11,390},{308,390},{12,381},{12,412},{156,360},{163,360},{12,230},{12,264},{12,298},{12,332}};
 for(unsigned i=0;i<sizeof(misses)/sizeof(misses[0]);++i)route_tap(misses[i][0],misses[i][1]);CHECK(!starts&&!frame_ui_pending()&&frame_ui->filter==0&&frame_ui->scope==FRAME_SCOPE_ALL&&frame_ui->save);leave();
 enter(true,0);start();route_launcher();tick(3,false);CHECK(!frame_ui_is_open()&&wifi_mode==UI_WIFI_MODE_RX_TOOLS);route_rx_back();CHECK(wifi_mode==UI_WIFI_MODE_MENU);CHECK(allocations==freed);
 reset();wifi_mode=UI_WIFI_MODE_RX_TOOLS;const uint16_t menu_miss[][2]={{11,140},{308,140},{12,125},{12,182},{12,221},{12,278},{12,317},{12,374}};
 for(unsigned i=0;i<sizeof(menu_miss)/sizeof(menu_miss[0]);++i)route_tap(menu_miss[i][0],menu_miss[i][1]);CHECK(!frame_ui_is_open()&&!eapol_opened&&!sae_opened&&!starts);
}
static void save(const char*folder,const char*name)
{char path[1024];snprintf(path,sizeof(path),"%s/%s.ppm",folder,name);FILE*f=fopen(path,"wb");CHECK(f);fprintf(f,"P6\n320 480\n255\n");
 for(unsigned i=0;i<320*480;++i){uint16_t p=screen[i];fputc((p>>11)*255/31,f);fputc(((p>>5)&63)*255/63,f);fputc((p&31)*255/31,f);}CHECK(!fclose(f));}
static void painted_selection(void)
{
 enter(true,0);choose_scope(FRAME_SCOPE_SELECTED);route_tap(100,285);
 picker_bank.wifi[0]=wifi_selected_result;picker_bank.wifi[1]=wifi_selected_result;picker_bank.wifi[1].bssid[5]=0x99;
 live_records[SENTINEL_SCAN_WIFI]=(live_scan_record_t){.captured=true,.saved_generation=7,.count=2};wifi_relation_generation=7;wifi_relation_publication=100;
 live_wifi_built=(live_wifi_paint_map_t){.publication=100,.token=1,.generation=7,.bank_count=2,.count=2,.indexes={1,0},.valid=true};
 picker_paint_error=ESP_OK;CHECK(live_paint(true)==ESP_OK);route_picker_row(0);CHECK(wifi_target_count==1&&wifi_targets[0].ap.bssid[5]==0x99);
 wifi_relation_publication=101;route_picker_row(1);CHECK(wifi_target_count==1&&picker_refreshes>0);
 live_wifi_built.publication=101;live_wifi_built.token=2;picker_paint_error=ESP_FAIL;CHECK(live_paint(true)==ESP_FAIL);picker_paint_error=ESP_OK;route_picker_row(1);CHECK(wifi_target_count==1);
 picker_paint_error=ESP_OK;CHECK(live_paint(true)==ESP_OK);route_picker_row(1);CHECK(wifi_target_count==2);
 route_picker_done();start();CHECK(model.plan.target_count==2&&model.plan.bssids[0][5]==0x99&&model.plan.bssids[1][5]==0x50);leave();
}
static void ownership_edges(void)
{
 reset();eapol_opened=true;frame_ui_open(true,0);CHECK(!frame_ui_is_open()&&!frame_ui);eapol_opened=false;
 sae_opened=true;frame_ui_open(false,1);CHECK(!frame_ui_is_open()&&!frame_ui);sae_opened=false;frame_ui_open(true,3);CHECK(!frame_ui_is_open());
 enter(true,0);start();hold_stop=true;route_back();wifi_mode=UI_WIFI_MODE_RX_TOOLS;route_tap(100,240);CHECK(!eapol_opened&&frame_ui_is_open());
 tick(3,false);CHECK(frame_ui_is_open());hold_stop=false;tick(3,false);CHECK(!frame_ui_is_open()&&wifi_mode==UI_WIFI_MODE_RX_TOOLS&&!eapol_opened);
 enter(true,0);start();model.summary.received=81;route_tap(100,390);tick(3,false);route_tap(50,360);choose_filter(2);start_error=ESP_ERR_NO_MEM;route_tap(100,390);tick(3,false);
 CHECK(frame_ui->status.summary.received==81&&frame_ui->run_filter==0&&!model.capture.busy);leave();
}
static void previews(const char*folder)
{
 reset();wifi_mode=UI_WIFI_MODE_RX_TOOLS;CHECK(draw_detail_page()==ESP_OK);save(folder,"rx-tools-menu");
 enter(true,0);CHECK(draw_detail_page()==ESP_OK);save(folder,"frame-raw-options");choose_filter(1);choose_scope(FRAME_SCOPE_SELECTED);CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"SELECTED CHANNELS"));save(folder,"frame-probe-selected");leave();
 enter(false,1);CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"AP CHANNEL")&&!strstr(drawn,"COPIED AP BSSID"));save(folder,"frame-probe-ap-channel");leave();
 enter(true,0);start();model.capture.hop_valid=true;model.capture.channel=149;model.capture.completed_sweeps=2;model.capture.attempted_mask=15;model.capture.applied_mask=7;
 model.capture.received=100;model.capture.written=60;model.capture.dropped_full=3;model.capture.dropped_other=2;model.capture.pending=2;
 model.summary=(cerberus_wifi_frame_summary_t){.received=70,.management=40,.data=20,.control=8,.other=2,.probe_requests=3,.probe_responses=4,.deauth=5,.disassoc=6,.beacons=7,.eapol_headers=8,.extrema_valid=true,.min_rssi=-82,.max_rssi=-31,.count=3,.event_replaced=2};
 tick(10,false);CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"C5 ADMITTED 100")&&strstr(drawn,"S3 DELIVERED 70 / SD WRITTEN 60")&&strstr(drawn,"TUNED 3 / TRIED 4 / SKIPPED 1")&&strstr(drawn,"DELIVERED RSSI LOW -82 / HIGH -31"));save(folder,"frame-raw-summary");
 for(unsigned i=0;i<3;++i){rows[i]=(cerberus_s3_frame_row_t){.generation=model.generation,.last_received_age_ms=1234,.evidence={.bssid={2,0x10,0x20,0x30,0x40,0x50},.transmitter={2,0xaa,0xbb,0xcc,0xdd,(uint8_t)i},.receiver={0xff,0xff,0xff,0xff,0xff,0xff},.subtype=12,.channel=149,.reason=7,.reason_known=true,.rssi=-40,.rssi_valid=true,.frames=5,.retries=2}};}
 route_tap(200,360);CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"REASON 7")&&strstr(drawn,"DELIVERY 1.234S AGO")&&strstr(drawn,"REPLACED 2"));save(folder,"frame-disconnect-events");
 route_tap(250,330);CHECK(frame_ui->offset==2);route_tap(20,330);CHECK(!frame_ui->offset);rows[0].evidence.protected_frame=true;rows[1].evidence.fragmented=true;rows[1].evidence.subtype=10;
 masked=true;rows[0].last_received_age_ms=UINT32_MAX;rows[0].evidence.rssi_valid=false;CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"UNKNOWN / PROTECTED")&&strstr(drawn,"UNKNOWN / FRAGMENTED")&&!strstr(drawn,"REASON 7")&&strstr(drawn,"DELIVERY AGE UNKNOWN")&&!strstr(drawn,"02:AA"));save(folder,"frame-protected-private");rows[0].evidence.protected_frame=false;rows[0].evidence.reason_known=false;CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"UNDECODED BODY")&&!strstr(drawn,"SHORT BODY"));
 page_error=ESP_ERR_TIMEOUT;CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"EVENT COPY UNAVAILABLE")&&!strstr(drawn,"REASON"));page_error=ESP_OK;++model.generation;CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"SESSION CHANGED"));--model.generation;
 route_tap(200,360);model.capture.hop_error=ESP_ERR_TIMEOUT;tick(10,false);CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"TELEMETRY UNAVAILABLE")&&!strstr(drawn,"TUNED 3"));save(folder,"frame-coverage-error");
 model.capture.hop_error=ESP_OK;model.capture.channel=0;tick(10,false);CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"TUNING / NO CURRENT RX CHANNEL"));
 model.capture.received=model.summary.received=model.capture.written=UINT32_MAX;model.summary.management=model.summary.data=model.summary.deauth=model.summary.disassoc=UINT32_MAX;tick(10,false);CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"S3 DELIVERED >=4294967295 / SD WRITTEN >=4294967295"));save(folder,"frame-saturated");
 hold_stop=true;read_error=ESP_ERR_TIMEOUT;route_back();tick(10,false);CHECK(frame_ui_is_open()&&draw_detail_page()==ESP_OK&&strstr(drawn,"ESP_ERR_TIMEOUT"));save(folder,"frame-stop-error");read_error=ESP_OK;hold_stop=false;tick(3,false);CHECK(!frame_ui_is_open());
 enter(true,0);start();snprintf(model.capture.file_name,sizeof(model.capture.file_name),"WCAP0042.PCAPNG");model.capture.written=42;route_tap(100,390);tick(3,false);CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"WCAP0042.PCAPNG")&&strstr(drawn,"RESULTS RETAINED"));save(folder,"frame-saved");leave();
 enter(true,0);unsigned before=calls;CHECK(draw_detail_page()==ESP_OK);unsigned count=calls-before;for(unsigned i=1;i<=count;++i){fail_after=calls+i;CHECK(draw_detail_page()==ESP_FAIL);fail_after=0;}leave();CHECK(allocations==freed);
}
int main(int argc,char**argv)
{CHECK(argc==2);tests();painted_selection();ownership_edges();previews(argv[1]);printf("Wi-Fi frame UI: %u checks; optional PSRAM %zu bytes\n",checks,sizeof(frame_ui_store_t));return 0;}
