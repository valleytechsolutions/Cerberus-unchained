#include "sae_framebuffer.h"
#include "sae_modes.h"
#include "wifi_sae_session.h"
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
#define TAG "sae-ui"
#define CHECK(x) do { ++checks; assert(x); } while (0)
#define ESP_ERROR_CHECK(x) CHECK((x)==ESP_OK)
static unsigned checks,allocations,freed,starts,stops,releases,old_stops,generic_starts,generic_stops,ignored_stops;
static bool oom,masked,hold_stop,prior_live,prior_dual,prior_capture,c5_online=true;
static esp_err_t start_error,read_error,page_error,release_error;
static uint32_t now_ms;
enum {UI_PAGE_WIFI,UI_PAGE_OTHER,UI_PAGE_FIELD_MENU,SENTINEL_SCAN_NONE,SENTINEL_SCAN_WIFI};
static unsigned current_page=UI_PAGE_WIFI;
static ui_wifi_mode_t wifi_mode=UI_WIFI_MODE_TARGET;
static struct {uint32_t firmware_version;} c5_status={86};
static cerberus_c5_wifi_result_t wifi_selected_result;
static uint8_t wifi_selected_index;
static bool wifi_selection_valid,wifi_attack_selecting,sae_ui_selecting,eapol_ui_selecting,dual_wifi_return_to_view;
static struct {const char *tools_label,*hint;} live_view;
static int live_session,live_ble_session;
static unsigned picker_begins;
static uint32_t live_last_poll;
static bool picker_paused;
static void sentinel_scan_session_select_snapshot(int*s,unsigned tool){(void)s;CHECK(tool==SENTINEL_SCAN_WIFI);++picker_begins;}
static void sentinel_scan_session_pause(int*s,bool pause){(void)s;picker_paused=pause;}
static cerberus_s3_sae_status_t model;
static cerberus_s3_sae_row_t rows[16];
static cerberus_s3_capture_status_t generic;
static const uint8_t ap[6]={2,0x10,0x20,0x30,0x40,0x50};
static const cerberus_ui_widget_theme_t theme={UI_BACKGROUND,UI_PANEL,UI_GOOD,UI_TEXT,UI_MUTED};
#define UI_WIDGET_THEME (&theme)
#include "wifi_targets.h"
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
esp_err_t cerberus_s3_sae_start(const cerberus_s3_sae_options_t*o)
{CHECK(!prior_live&&!prior_dual&&!prior_capture&&!model.capture.busy);++starts;
 if(start_error)return start_error;model=(cerberus_s3_sae_status_t){.generation=starts,.channel=o->channel,
 .filter_bssid=o->filter_bssid,.planned=o->plan!=NULL,.capture={.generation=starts,.state=CERBERUS_S3_CAPTURE_RUNNING,
 .busy=true,.save_pcap=o->save_pcap,.channel=o->channel,.planned=o->plan!=NULL}};
 memcpy(model.bssid,o->bssid,6);if(o->plan){CHECK(cerberus_wifi_capture_plan_valid(o->plan));model.plan=*o->plan;}return ESP_OK;}
void cerberus_s3_sae_stop(void)
{++stops;if(ignored_stops){--ignored_stops;return;}if(model.capture.busy){model.capture.state=CERBERUS_S3_CAPTURE_STOPPING;
 if(!hold_stop){model.capture.busy=false;model.capture.state=model.capture.save_pcap?CERBERUS_S3_CAPTURE_SAVED:CERBERUS_S3_CAPTURE_COMPLETE;}}}
esp_err_t cerberus_s3_sae_status(cerberus_s3_sae_status_t*out)
{memset(out,0,sizeof(*out));if(read_error)return read_error;*out=model;return ESP_OK;}
esp_err_t cerberus_s3_sae_page(uint16_t offset,cerberus_s3_sae_row_t*out,size_t n,cerberus_s3_sae_page_t*page)
{CHECK(n==2);memset(out,0,n*sizeof(*out));*page=(cerberus_s3_sae_page_t){0};if(page_error)return page_error;
 page->generation=model.generation;page->total=model.summary.count;
 for(unsigned i=0;i<n&&offset+i<page->total;++i){out[i]=rows[offset+i];++page->copied;}return ESP_OK;}
esp_err_t cerberus_s3_sae_release(void)
{++releases;if(release_error)return release_error;if(model.capture.busy)return ESP_ERR_INVALID_STATE;memset(&model,0,sizeof(model));return ESP_OK;}
void cerberus_s3_capture_status(cerberus_s3_capture_status_t*out)
{*out=model.capture.busy?model.capture:generic;if(prior_capture)out->busy=true;}
esp_err_t cerberus_s3_capture_start(uint8_t channel,uint8_t mode,const uint8_t*bssid,bool filter)
{CHECK(channel==6&&!memcmp(bssid,ap,6));CHECK(filter==(mode!=CERBERUS_WIFI_CAPTURE_MODE_PROBE));++generic_starts;
 generic=(cerberus_s3_capture_status_t){.state=CERBERUS_S3_CAPTURE_RUNNING,.busy=true,.mode_mask=mode};return ESP_OK;}
void cerberus_s3_capture_stop(void){++generic_stops;generic.busy=false;generic.state=CERBERUS_S3_CAPTURE_SAVED;}
static bool eapol_ui_is_open(void){return false;}
static bool eapol_ui_busy(void){return false;}
static bool eapol_ui_pending(void){return false;}
static void eapol_ui_open(bool menu){(void)menu;CHECK(false);}
static void eapol_ui_selection_done(void){CHECK(false);}
static void eapol_ui_close(bool back){(void)back;CHECK(false);}
static void eapol_ui_start(void){CHECK(false);}
static esp_err_t eapol_ui_draw(void){CHECK(false);return ESP_FAIL;}
static void eapol_ui_tap(uint16_t x,uint16_t y){(void)x;(void)y;CHECK(false);}
static bool eapol_ui_tick(uint32_t now,bool visible){(void)now;(void)visible;return false;}
static bool sae_ui_is_open(void);
static bool sae_ui_busy(void);
static bool sae_ui_pending(void);
static void sae_ui_open(bool menu);
static void sae_ui_close(bool back);
static void sae_ui_start(void);
static esp_err_t sae_ui_draw(void);
static void sae_ui_tap(uint16_t x,uint16_t y);
#include "sae_picker_begin.h"
#include "wifi_capture_ui.h"
#include "wifi_sae_ui.h"
#define SENTINEL_VIEW_VISIBLE_ROWS 4U
enum {SENTINEL_VIEW_ROW0};
#include "sae_picker_types.h"
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
#include "sae_picker.h"
static esp_err_t draw_detail_page(void);
static esp_err_t draw_current_page(void){return draw_detail_page();}
#include "sae_routes.h"
static esp_err_t draw_detail_page(void)
{drawn_size=0;drawn[0]=0;esp_err_t e=cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND);
 if(!e)e=draw_centered(20,"WI-FI TOOLS",UI_TEXT,2);if(!e&&wifi_mode==UI_WIFI_MODE_CAPTURE)e=capture_draw();if(!e&&wifi_mode==UI_WIFI_MODE_AUTH)e=auth_monitor_draw();
 return e?e:cerberus_ui_widget_draw_button((cerberus_ui_rect_t){8,420,304,44},sae_ui_is_open()?sae_ui_back_label():route_auth_back_label(),2,true,UI_WIDGET_THEME);}
static void tick(unsigned n,bool asleep){for(unsigned i=0;i<n;++i){now_ms+=25;route_tick(now_ms,asleep);}}
static void reset(void)
{CHECK(!sae_ui_is_open()&&!sae_ui&&!model.capture.busy);memset(&generic,0,sizeof(generic));memset(&model,0,sizeof(model));memset(rows,0,sizeof(rows));
 wifi_targets_clear();oom=masked=hold_stop=prior_live=prior_dual=prior_capture=false;c5_online=true;c5_status.firmware_version=86;
 sae_ui_selecting=wifi_attack_selecting=false;start_error=read_error=page_error=release_error=ESP_OK;
 starts=stops=releases=old_stops=generic_starts=generic_stops=ignored_stops=0;now_ms=1000;fail_after=0;
 current_page=UI_PAGE_WIFI;wifi_mode=UI_WIFI_MODE_CAPTURE;wifi_selected_result=(cerberus_c5_wifi_result_t){.ssid="Field Kit",.channel=6,.rssi=-35};
 memcpy(wifi_selected_result.bssid,ap,6);wifi_selected_index=7;wifi_selection_valid=true;capture_enter();}
static void enter(bool menu)
{reset();if(menu){wifi_mode=UI_WIFI_MODE_MENU;route_menu(270,380);CHECK(wifi_mode==UI_WIFI_MODE_AUTH&&!sae_ui_is_open()&&!starts);CHECK(draw_detail_page()==ESP_OK);route_tap(100,270);}else{capture_mode_index=4;CHECK(draw_detail_page()==ESP_OK);}
 CHECK(sae_ui_is_open()&&!starts&&!old_stops);tick(2,false);}
static void start(void){route_tap(100,390);tick(2,false);CHECK(starts==1&&model.capture.busy&&!model.capture.save_pcap);}
static void leave(void)
{bool menu=sae_ui_from_menu;route_back();tick(3,false);CHECK(!sae_ui_is_open()&&!sae_ui&&!model.capture.busy&&wifi_mode==(menu?UI_WIFI_MODE_AUTH:UI_WIFI_MODE_TARGET));}
static void choose_scope(unsigned scope)
{for(unsigned i=0;i<6&&sae_ui->scope!=scope;++i)route_tap(100,230);CHECK(sae_ui->scope==scope);}
static void tests(void)
{
 for(unsigned y=364;y<406;++y)for(unsigned x=0;x<320;++x){const int actions[]={6,9,8,7,12};int want=-1;
  if(x>=12&&x<308){unsigned i=(x-12)/60;if((x-12)%60<56)want=actions[i];}CHECK(sentinel_hub_hit_test(SENTINEL_HUB_WIFI,x,y)==want);}
 const int actions[]={0,1,2,11,10,3,4,5};for(unsigned i=0;i<8;++i)CHECK(sentinel_hub_hit_test(SENTINEL_HUB_WIFI,20,70+i*36)==actions[i]);
 enter(true);CHECK(sae_ui->scope==SAE_SCOPE_ALL&&sae_ui_from_menu);CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"SAVE PCAP OFF"));start();
 CHECK(model.planned&&!model.filter_bssid&&model.plan.channel_mask==cerberus_wifi_capture_plan_band_mask(CERBERUS_WIFI_CAPTURE_PLAN_BOTH));leave();
 for(unsigned scope=SAE_SCOPE_2G;scope<=SAE_SCOPE_5G;++scope){enter(true);choose_scope(scope);start();CHECK(model.plan.channel_mask==cerberus_wifi_capture_plan_band_mask(scope));leave();}
 enter(true);choose_scope(SAE_SCOPE_FIXED);route_tap(20,260);CHECK(sae_ui->channel==cerberus_wifi_capture_plan_channel(CERBERUS_WIFI_CAPTURE_PLAN_CHANNELS-1));
 route_tap(250,260);CHECK(sae_ui->channel==1);route_tap(250,260);start();CHECK(!model.planned&&!model.filter_bssid&&model.channel==2);leave();
 enter(false);memset(wifi_selected_result.bssid,0xff,6);wifi_selected_result.channel=149;snprintf(wifi_selected_result.ssid,33,"Changed AP");
 capture_enter();CHECK(sae_ui->target.channel==6&&capture_mode_index==4);start();CHECK(model.filter_bssid&&model.channel==6&&!memcmp(model.bssid,ap,6));
 CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"Field Kit"));leave();CHECK(wifi_selected_index==7&&wifi_selected_result.channel==6&&!memcmp(wifi_selected_result.bssid,ap,6));
 enter(true);choose_scope(SAE_SCOPE_SELECTED);route_tap(100,390);CHECK(!starts&&sae_ui->error==ESP_ERR_INVALID_ARG);route_tap(100,260);
 CHECK(sae_ui_selecting&&wifi_attack_selecting&&wifi_mode==UI_WIFI_MODE_LIST&&!strcmp(route_picker_label(),"DONE"));tick(3,false);CHECK(sae_ui_is_open()&&!sae_ui->exit);
 cerberus_c5_wifi_result_t a=wifi_selected_result;CHECK(wifi_targets_add(&a,0));a.channel=149;CHECK(wifi_targets_add(&a,1));CHECK(wifi_target_count==2);
 route_picker_done();CHECK(!sae_ui_selecting&&!wifi_attack_selecting&&wifi_mode==UI_WIFI_MODE_CAPTURE);prior_live=true;route_tap(100,390);
 CHECK(sae_ui->plan.target_count==1&&sae_ui->plan.channel_mask==(cerberus_wifi_capture_plan_channel_bit(6)|cerberus_wifi_capture_plan_channel_bit(149)));
 wifi_targets_clear();sae_ui_open(true);tick(3,false);CHECK(!starts&&sae_ui_pending());prior_live=false;tick(3,false);CHECK(starts==1&&model.plan.target_count==1);leave();
 enter(true);choose_scope(SAE_SCOPE_SELECTED);route_tap(100,260);route_picker_back();CHECK(wifi_mode==UI_WIFI_MODE_CAPTURE&&!sae_ui_selecting);leave();
 enter(true);choose_scope(SAE_SCOPE_SELECTED);route_tap(100,260);
 picker_bank.wifi[0]=wifi_selected_result;picker_bank.wifi[1]=wifi_selected_result;picker_bank.wifi[1].bssid[5]=0x99;
 live_records[SENTINEL_SCAN_WIFI]=(live_scan_record_t){.captured=true,.saved_generation=7,.count=2};wifi_relation_generation=7;wifi_relation_publication=100;
 live_wifi_built=(live_wifi_paint_map_t){.publication=100,.token=1,.generation=7,.bank_count=2,.count=2,.indexes={1,0},.valid=true};
 picker_paint_error=ESP_OK;CHECK(live_paint(true)==ESP_OK);route_picker_row(0);CHECK(wifi_target_count==1&&wifi_targets[0].ap.bssid[5]==0x99);
 wifi_relation_publication=101;route_picker_row(1);CHECK(wifi_target_count==1&&picker_refreshes>0); /* Reused generation, changed publication. */
 live_wifi_built.publication=101;live_wifi_built.token=2;CHECK(live_paint(true)==ESP_OK);picker_paint_error=ESP_FAIL;
 CHECK(live_paint(false)==ESP_FAIL&&!live_wifi_painted.valid);picker_paint_error=ESP_OK;route_picker_row(1);CHECK(wifi_target_count==1);
 CHECK(live_paint(true)==ESP_OK);route_picker_row(1);CHECK(wifi_target_count==2&&wifi_targets[1].ap.bssid[5]==ap[5]);
 picker_bank.wifi[0].bssid[5]=0x88;route_picker_done();start();CHECK(model.plan.target_count==2&&model.plan.bssids[1][5]==ap[5]);leave();
 reset();dual_wifi_return_to_view=true;capture_mode_index=4;CHECK(draw_detail_page()==ESP_OK);choose_scope(SAE_SCOPE_SELECTED);
 route_tap(100,260);CHECK(!dual_wifi_return_to_view);wifi_selected_result.channel=149;route_picker_back();leave();CHECK(wifi_selected_result.channel==6&&dual_wifi_return_to_view);
 enter(true);choose_scope(SAE_SCOPE_SELECTED);for(unsigned i=0;i<32;++i){a=wifi_selected_result;a.bssid[5]=(uint8_t)i;CHECK(wifi_targets_add(&a,(uint8_t)i));}start();CHECK(model.plan.target_count==32);leave();
 enter(true);prior_live=prior_dual=prior_capture=true;route_tap(100,390);tick(4,false);CHECK(!starts&&sae_ui_pending());route_back();route_tap(100,390);
 prior_live=prior_dual=prior_capture=false;tick(3,false);CHECK(!starts&&!sae_ui_is_open());
 enter(true);start();hold_stop=true;route_tap(100,390);route_back();tick(4,false);CHECK(sae_ui_is_open()&&model.capture.busy);route_tap(100,230);route_tap(100,390);CHECK(starts==1);
 hold_stop=false;cerberus_s3_sae_stop();tick(3,false);CHECK(!sae_ui_is_open()&&wifi_mode==UI_WIFI_MODE_AUTH);
 enter(true);start();model.summary.observations=7;route_tap(100,390);tick(2,false);CHECK(sae_ui->status.summary.observations==7&&!model.capture.busy);route_tap(100,390);tick(2,false);CHECK(starts==2&&!model.summary.observations);leave();
 enter(true);start();ignored_stops=1;route_tap(100,390);CHECK(model.capture.busy&&sae_ui_pending());tick(2,false);CHECK(!model.capture.busy&&!sae_ui_pending());leave();
 enter(true);start();ignored_stops=2;route_back();tick(1,false);CHECK(model.capture.busy&&sae_ui_is_open());tick(2,false);CHECK(!model.capture.busy&&!sae_ui_is_open());
 enter(true);start();hold_stop=true;current_page=UI_PAGE_OTHER;tick(4,false);CHECK(sae_ui_is_open());hold_stop=false;tick(3,false);CHECK(!sae_ui_is_open()&&current_page==UI_PAGE_OTHER);
 enter(true);start();hold_stop=true;tick(4,true);CHECK(model.capture.busy);hold_stop=false;tick(3,true);CHECK(!model.capture.busy&&sae_ui_is_open());tick(3,false);CHECK(starts==1);leave();
 enter(true);prior_live=true;route_tap(100,390);tick(2,true);prior_live=false;tick(3,false);CHECK(!starts&&!sae_ui_pending());leave();
 enter(true);choose_scope(SAE_SCOPE_SELECTED);route_tap(100,260);current_page=UI_PAGE_OTHER;tick(3,false);CHECK(!sae_ui_is_open()&&!sae_ui_selecting&&!wifi_attack_selecting);
 enter(false);release_error=ESP_ERR_TIMEOUT;route_tap(100,160);tick(3,false);CHECK(sae_ui_is_open());route_back();release_error=ESP_OK;tick(3,false);CHECK(!sae_ui_is_open()&&wifi_mode==UI_WIFI_MODE_TARGET);
 enter(false);route_tap(100,160);tick(3,false);CHECK(!sae_ui_is_open()&&capture_mode_index==0);route_tap(100,380);CHECK(generic_starts==1);route_tap(100,380);route_back();
 enter(true);c5_status.firmware_version=85;route_tap(100,390);tick(3,false);CHECK(!starts&&draw_detail_page()==ESP_OK&&strstr(drawn,"CAPABILITY 86"));leave();
 enter(true);start_error=ESP_ERR_NO_MEM;route_tap(100,390);tick(3,false);CHECK(!model.capture.busy&&draw_detail_page()==ESP_OK&&strstr(drawn,"ESP_ERR_NO_MEM"));leave();
 reset();oom=true;route_menu(270,380);route_tap(100,270);CHECK(sae_ui_is_open()&&!sae_ui&&draw_detail_page()==ESP_OK);route_back();CHECK(!sae_ui_is_open());
 reset();oom=true;route_menu(270,380);route_tap(100,270);oom=false;route_tap(100,390);tick(3,false);CHECK(starts==1);leave();
 enter(true);const uint16_t misses[][2]={{11,390},{308,390},{12,381},{12,412},{156,360},{163,360}};
 for(unsigned i=0;i<sizeof(misses)/sizeof(misses[0]);++i)route_tap(misses[i][0],misses[i][1]);CHECK(!starts&&!sae_ui_pending()&&sae_ui->view==SAE_VIEW_OPTIONS);leave();
 enter(true);start();route_launcher();tick(3,false);CHECK(!sae_ui_is_open()&&wifi_mode==UI_WIFI_MODE_AUTH);CHECK(allocations==freed);route_auth_back();CHECK(wifi_mode==UI_WIFI_MODE_MENU&&!sae_ui_is_open());
}
static void save(const char*folder,const char*name)
{char path[1024];snprintf(path,sizeof(path),"%s/%s.ppm",folder,name);FILE*f=fopen(path,"wb");CHECK(f);fprintf(f,"P6\n320 480\n255\n");
 for(unsigned i=0;i<320*480;++i){uint16_t p=screen[i];fputc((p>>11)*255/31,f);fputc(((p>>5)&63)*255/63,f);fputc((p&31)*255/31,f);}CHECK(!fclose(f));}
static void previews(const char*folder)
{
 enter(true);CHECK(draw_detail_page()==ESP_OK);save(folder,"sae-ready");choose_scope(SAE_SCOPE_FIXED);CHECK(draw_detail_page()==ESP_OK);save(folder,"sae-fixed");
 choose_scope(SAE_SCOPE_SELECTED);CHECK(draw_detail_page()==ESP_OK);save(folder,"sae-selected");choose_scope(SAE_SCOPE_ALL);start();
 model.summary=(cerberus_wifi_sae_summary_t){.observations=24,.commits=14,.token_requests=5,.h2e=2,.sae_pk=1,.group_rejections=2,.count=3};
 model.capture.received=24;model.capture.hop_valid=true;model.capture.channel=149;model.capture.completed_sweeps=2;model.capture.attempted_mask=7;model.capture.applied_mask=3;
 for(unsigned i=0;i<3;++i){rows[i].generation=model.generation;rows[i].evidence=(cerberus_wifi_sae_record_t){.bssid={2,0x10,0x20,0x30,0x40,0x50},.transmitter={2,0xaa,0xbb,0xcc,0xdd,(uint8_t)i},
 .receiver={2,0x10,0x20,0x30,0x40,0x50},.group=19,.group_present=true,.frame_length=128,.body_length=98,.status=76,.observations=8,.rssi=-42,.rssi_valid=true,.channel=6};rows[i].last_received_age_ms=1234;}
 tick(10,false);CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"TUNED 2 / TRIED 3 / SKIPPED 1"));save(folder,"sae-summary");
 model.capture.channel=0;tick(10,false);CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"NO CURRENT RX CHANNEL")&&!strstr(drawn,"RX CHANNEL 0"));
 model.capture.busy=false;tick(10,false);CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"COVERAGE RETAINED")&&!strstr(drawn,"RX CHANNEL 0"));
 model.capture.busy=true;model.capture.channel=149;
 model.capture.hop_valid=false;model.capture.hop_error=ESP_ERR_TIMEOUT;tick(10,false);CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"TELEMETRY UNAVAILABLE")&&!strstr(drawn,"TUNED 2"));save(folder,"sae-telemetry-error");
 route_tap(200,360);CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"S3 DELIVERY 1.234S AGO")&&strstr(drawn,"C5 CH 6 / -42 DBM"));save(folder,"sae-rows");
 route_tap(250,330);CHECK(sae_ui->offset==2);route_tap(20,330);CHECK(sae_ui->offset==0);page_error=ESP_ERR_TIMEOUT;CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"COPY UNAVAILABLE"));page_error=ESP_OK;
 masked=true;rows[0].last_received_age_ms=UINT32_MAX;rows[0].evidence.rssi_valid=false;rows[0].evidence.group_present=false;
 CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"GROUP UNKNOWN")&&strstr(drawn,"RSSI UNKNOWN")&&strstr(drawn,"DELIVERY AGE UNKNOWN")&&!strstr(drawn,"02:AA:BB"));save(folder,"sae-private");
 route_tap(200,360);model.summary.observations=UINT32_MAX;model.summary.commits=UINT32_MAX;model.summary.token_requests=UINT32_MAX;tick(10,false);
 CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,">=4294967295"));save(folder,"sae-saturated");
 hold_stop=true;read_error=ESP_ERR_TIMEOUT;route_back();tick(10,false);CHECK(sae_ui_is_open()&&draw_detail_page()==ESP_OK&&strstr(drawn,"ESP_ERR_TIMEOUT"));save(folder,"sae-stop-error");
 read_error=ESP_OK;hold_stop=false;cerberus_s3_sae_stop();tick(3,false);CHECK(!sae_ui_is_open());
 enter(false);route_tap(50,300);route_tap(100,390);tick(2,false);CHECK(model.capture.save_pcap);model.capture.written=19;model.capture.bytes_written=10240;snprintf(model.capture.file_name,sizeof(model.capture.file_name),"WCAP0019.PCAPNG");
 route_tap(100,390);tick(2,false);CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"PCAP HAS NO TIMES")&&strstr(drawn,"SD WRITTEN 19")&&strstr(drawn,"WCAP0019.PCAPNG"));save(folder,"sae-saved");leave();
 enter(true);unsigned before=calls;CHECK(draw_detail_page()==ESP_OK);unsigned count=calls-before;
 for(unsigned i=1;i<=count;++i){fail_after=calls+i;CHECK(draw_detail_page()==ESP_FAIL);fail_after=0;}leave();CHECK(allocations==freed);
 drawn_size=0;drawn[0]=0;CHECK(cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND)==ESP_OK);CHECK(sentinel_hub_draw(SENTINEL_HUB_WIFI,UI_WIDGET_THEME)==ESP_OK);save(folder,"sae-menu");
}
int main(int argc,char**argv)
{CHECK(argc==2);tests();previews(argv[1]);wifi_targets_clear();printf("Wi-Fi SAE UI: %u checks; optional PSRAM %zu bytes\n",checks,sizeof(sae_ui_store_t));return 0;}
