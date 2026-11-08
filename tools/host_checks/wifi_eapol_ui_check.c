#include "eapol_framebuffer.h"
#include "eapol_modes.h"
#include "wifi_eapol_session.h"
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
#define TAG "eapol-ui"
#define CHECK(x) do { ++checks; assert(x); } while (0)
#define ESP_ERROR_CHECK(x) CHECK((x)==ESP_OK)
static unsigned checks,allocations,freed,starts,stops,releases,old_stops,generic_starts,generic_stops,ignored_stops;
static bool oom,masked,hold_stop,prior_live,prior_dual,prior_capture,c5_online=true;
static esp_err_t start_error,read_error,page_error,release_error;
static uint32_t now_ms;
enum {UI_PAGE_WIFI,UI_PAGE_OTHER,UI_PAGE_FIELD_MENU,SENTINEL_SCAN_NONE,SENTINEL_SCAN_WIFI};
static unsigned current_page=UI_PAGE_WIFI;
static ui_wifi_mode_t wifi_mode=UI_WIFI_MODE_TARGET;
static struct {uint32_t firmware_version;} c5_status={87};
static cerberus_c5_wifi_result_t wifi_selected_result;
static uint8_t wifi_selected_index;
static bool wifi_selection_valid,wifi_attack_selecting,eapol_ui_selecting,dual_wifi_return_to_view;
#include "wifi_targets.h"
static unsigned picker_begins;
static uint32_t live_last_poll;
static bool picker_paused;
static void sentinel_scan_session_select_snapshot(int*s,unsigned tool){(void)s;CHECK(tool==SENTINEL_SCAN_WIFI);++picker_begins;}
static void sentinel_scan_session_pause(int*s,bool pause){(void)s;picker_paused=pause;}
static int live_session,live_ble_session;
static cerberus_s3_eapol_status_t model;
static cerberus_s3_eapol_row_t rows[16];
static cerberus_s3_eapol_context_row_t contexts[16];
static cerberus_s3_capture_status_t generic;
static const uint8_t ap[6]={2,0x10,0x20,0x30,0x40,0x50};
static const cerberus_ui_widget_theme_t theme={UI_BACKGROUND,UI_PANEL,UI_GOOD,UI_TEXT,UI_MUTED};
#define UI_WIDGET_THEME (&theme)
typedef struct {bool busy;} cerberus_dual_wifi_status_t;
static bool dual_wifi_live_released(void){return !prior_live;}
static void cerberus_dual_wifi_scan_get_status(cerberus_dual_wifi_status_t*s){s->busy=prior_dual;}
static bool dual_wifi_ui_busy(const cerberus_dual_wifi_status_t*s){return s->busy;}
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
esp_err_t cerberus_s3_eapol_start_options(const cerberus_s3_eapol_options_t*o)
{CHECK(!prior_live&&!prior_dual&&!prior_capture&&!model.capture.busy);++starts;
 if(start_error)return start_error;model=(cerberus_s3_eapol_status_t){.generation=starts,.channel=o->channel,
 .filter_bssid=o->filter_bssid,.planned=o->plan!=NULL,.capture={.generation=starts,.state=CERBERUS_S3_CAPTURE_RUNNING,
 .busy=true,.save_pcap=o->save_pcap,.channel=o->channel,.planned=o->plan!=NULL}};
 memcpy(model.bssid,o->bssid,6);if(o->plan){CHECK(cerberus_wifi_capture_plan_valid(o->plan)&&o->plan->dwell_ms==2000);model.plan=*o->plan;}return ESP_OK;}
void cerberus_s3_eapol_stop(void)
{++stops;if(ignored_stops){--ignored_stops;return;}if(model.capture.busy){model.capture.state=CERBERUS_S3_CAPTURE_STOPPING;
 if(!hold_stop){model.capture.busy=false;model.capture.state=model.capture.save_pcap?CERBERUS_S3_CAPTURE_SAVED:CERBERUS_S3_CAPTURE_COMPLETE;}}}
esp_err_t cerberus_s3_eapol_status(cerberus_s3_eapol_status_t*out)
{memset(out,0,sizeof(*out));if(read_error)return read_error;*out=model;return ESP_OK;}
esp_err_t cerberus_s3_eapol_page(uint16_t offset,cerberus_s3_eapol_row_t*out,size_t n,cerberus_s3_eapol_page_t*page)
{CHECK(n==2);memset(out,0,n*sizeof(*out));*page=(cerberus_s3_eapol_page_t){0};if(page_error)return page_error;
 page->generation=model.generation;page->total=model.summary.count;
 for(unsigned i=0;i<n&&offset+i<page->total;++i){out[i]=rows[offset+i];++page->copied;}return ESP_OK;}
esp_err_t cerberus_s3_eapol_context_page(uint16_t offset,cerberus_s3_eapol_context_row_t*out,size_t n,cerberus_s3_eapol_page_t*page)
{CHECK(n==2);memset(out,0,n*sizeof(*out));*page=(cerberus_s3_eapol_page_t){0};if(page_error)return page_error;
 page->generation=model.generation;page->total=model.summary.context_count;
 for(unsigned i=0;i<n&&offset+i<page->total;++i){out[i]=contexts[offset+i];++page->copied;}return ESP_OK;}
esp_err_t cerberus_s3_eapol_release(void)
{++releases;if(release_error)return release_error;if(model.capture.busy)return ESP_ERR_INVALID_STATE;memset(&model,0,sizeof(model));return ESP_OK;}
void cerberus_s3_capture_status(cerberus_s3_capture_status_t*out)
{*out=model.capture.busy?model.capture:generic;if(prior_capture)out->busy=true;}
esp_err_t cerberus_s3_capture_start(uint8_t channel,uint8_t mode,const uint8_t*bssid,bool filter)
{CHECK(channel==6&&!memcmp(bssid,ap,6));CHECK(filter==(mode!=CERBERUS_WIFI_CAPTURE_MODE_PROBE));++generic_starts;
 generic=(cerberus_s3_capture_status_t){.state=CERBERUS_S3_CAPTURE_RUNNING,.busy=true,.mode_mask=mode};return ESP_OK;}
void cerberus_s3_capture_stop(void){++generic_stops;generic.busy=false;generic.state=CERBERUS_S3_CAPTURE_SAVED;}
static bool eapol_ui_is_open(void);
static bool eapol_ui_busy(void);
static bool eapol_ui_pending(void);
static void eapol_ui_open(bool menu);
static void eapol_ui_close(bool back);
static void eapol_ui_start(void);
static esp_err_t eapol_ui_draw(void);
static void eapol_ui_tap(uint16_t x,uint16_t y);
static bool eapol_ui_tick(uint32_t now,bool visible);
/* SAE's production monitor has its own route/render suite. This boundary
 * verifies EAPOL releases before dispatching the next capture mode. */
static bool sae_opened,sae_ui_selecting;
static void sae_ui_selection_done(void){CHECK(false);}
static bool sae_ui_is_open(void){return sae_opened;}
static bool sae_ui_busy(void){return false;}
static bool sae_ui_pending(void){return false;}
static void sae_ui_open(bool menu){CHECK(!menu&&!eapol_ui_is_open());sae_opened=true;}
static void sae_ui_close(bool back){CHECK(back);sae_opened=false;wifi_mode=UI_WIFI_MODE_TARGET;}
static void sae_ui_start(void){CHECK(false);}
static esp_err_t sae_ui_draw(void){return ESP_OK;}
static void sae_ui_tap(uint16_t x,uint16_t y){(void)x;(void)y;CHECK(false);}
static bool sae_ui_tick(uint32_t now,bool visible){(void)now;(void)visible;return false;}
#include "eapol_picker_begin.h"
#include "wifi_capture_ui.h"
#include "wifi_eapol_ui.h"
static esp_err_t draw_detail_page(void);
static esp_err_t draw_current_page(void){return draw_detail_page();}
#include "eapol_routes.h"
static esp_err_t draw_detail_page(void)
{drawn_size=0;drawn[0]=0;esp_err_t e=cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND);
 if(!e)e=draw_centered(20,"WI-FI TOOLS",UI_TEXT,2);if(!e&&wifi_mode==UI_WIFI_MODE_CAPTURE)e=capture_draw();
 if(!e&&wifi_mode==UI_WIFI_MODE_AUTH)e=auth_monitor_draw();
 return e?e:cerberus_ui_widget_draw_button((cerberus_ui_rect_t){8,420,304,44},eapol_ui_is_open()?eapol_ui_back_label():route_auth_back_label(),2,true,UI_WIDGET_THEME);}
static void tick(unsigned n,bool asleep){for(unsigned i=0;i<n;++i){now_ms+=25;route_tick(now_ms,asleep);}}
static void reset(void)
{CHECK(!eapol_ui_is_open()&&!eapol_ui&&!model.capture.busy);memset(&generic,0,sizeof(generic));memset(&model,0,sizeof(model));memset(rows,0,sizeof(rows));memset(contexts,0,sizeof(contexts));wifi_targets_clear();eapol_ui_selecting=wifi_attack_selecting=false;picker_begins=0;
 oom=masked=hold_stop=prior_live=prior_dual=prior_capture=false;c5_online=true;c5_status.firmware_version=87;
 start_error=read_error=page_error=release_error=ESP_OK;starts=stops=releases=old_stops=generic_starts=generic_stops=ignored_stops=0;now_ms=1000;fail_after=0;
 current_page=UI_PAGE_WIFI;wifi_mode=UI_WIFI_MODE_CAPTURE;wifi_selected_result=(cerberus_c5_wifi_result_t){.ssid="Field Kit",.channel=6,.rssi=-35};memcpy(wifi_selected_result.bssid,ap,6);wifi_selected_index=7;wifi_selection_valid=true;capture_enter();}
static void enter(void)
{reset();for(unsigned i=0;i<3;++i)route_tap(100,160);CHECK(eapol_ui_is_open()&&capture_mode_index==3&&!starts&&!old_stops);tick(2,false);}
static void start(void){route_tap(100,390);tick(2,false);CHECK(starts==1&&model.capture.busy&&!model.capture.save_pcap);}
static void leave(void){bool menu=eapol_ui_from_menu;route_back();tick(3,false);CHECK(!eapol_ui_is_open()&&!eapol_ui&&!model.capture.busy&&wifi_mode==(menu?UI_WIFI_MODE_AUTH:UI_WIFI_MODE_TARGET));}
static void enter_menu(void)
{reset();wifi_mode=UI_WIFI_MODE_MENU;route_menu(270,380);CHECK(wifi_mode==UI_WIFI_MODE_AUTH&&!starts&&!eapol_ui_is_open());
 CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"AUTH MONITORS")&&strstr(drawn,"WIFI MENU"));route_tap(100,170);CHECK(eapol_ui_is_open()&&eapol_ui_from_menu&&!starts&&!old_stops);tick(2,false);}
static void choose_scope(unsigned scope)
{for(unsigned i=0;i<6&&eapol_ui->scope!=scope;++i)route_tap(100,230);CHECK(eapol_ui->scope==scope);}
static void scope_tests(void)
{
 enter_menu();CHECK(eapol_ui->scope==EAPOL_SCOPE_ALL);start();CHECK(model.planned&&!model.filter_bssid&&model.plan.channel_mask==cerberus_wifi_capture_plan_band_mask(0));leave();
 for(unsigned scope=EAPOL_SCOPE_2G;scope<=EAPOL_SCOPE_5G;++scope){enter_menu();choose_scope(scope);start();CHECK(model.plan.channel_mask==cerberus_wifi_capture_plan_band_mask(scope));leave();}
 enter_menu();choose_scope(EAPOL_SCOPE_FIXED);route_tap(20,260);CHECK(eapol_ui->channel==cerberus_wifi_capture_plan_channel(CERBERUS_WIFI_CAPTURE_PLAN_CHANNELS-1));
 route_tap(250,260);CHECK(eapol_ui->channel==1);route_tap(250,260);start();CHECK(!model.planned&&!model.filter_bssid&&model.channel==2);leave();
 enter_menu();choose_scope(EAPOL_SCOPE_SELECTED);route_tap(100,390);CHECK(!starts&&eapol_ui->error==ESP_ERR_INVALID_ARG);route_tap(100,260);
 CHECK(eapol_ui_selecting&&wifi_attack_selecting&&wifi_mode==UI_WIFI_MODE_LIST&&picker_begins==1&&picker_paused&&!live_last_poll);tick(3,false);CHECK(eapol_ui_is_open()&&!eapol_ui->exit);
 cerberus_c5_wifi_result_t a=wifi_selected_result;CHECK(wifi_targets_add(&a,0));a.channel=149;CHECK(wifi_targets_add(&a,1));
 route_picker_done();CHECK(!eapol_ui_selecting&&!wifi_attack_selecting&&wifi_mode==UI_WIFI_MODE_CAPTURE);prior_live=true;route_tap(100,390);
 CHECK(eapol_ui->plan.target_count==1&&eapol_ui->plan.dwell_ms==2000&&eapol_ui->plan.channel_mask==(cerberus_wifi_capture_plan_channel_bit(6)|cerberus_wifi_capture_plan_channel_bit(149)));
 wifi_targets_clear();eapol_ui_open(true);tick(3,false);CHECK(!starts&&eapol_ui_pending());prior_live=false;tick(3,false);CHECK(starts==1&&model.plan.target_count==1);leave();
 enter();dual_wifi_return_to_view=true;eapol_ui->target_dual_return=true;choose_scope(EAPOL_SCOPE_SELECTED);route_tap(100,260);wifi_selected_index=20;wifi_selected_result.channel=149;
 route_picker_back();leave();CHECK(wifi_selected_index==7&&wifi_selected_result.channel==6&&dual_wifi_return_to_view);
 enter_menu();choose_scope(EAPOL_SCOPE_SELECTED);route_tap(100,260);current_page=UI_PAGE_OTHER;tick(3,false);CHECK(!eapol_ui_is_open()&&!eapol_ui_selecting&&!wifi_attack_selecting&&current_page==UI_PAGE_OTHER);
 reset();wifi_mode=UI_WIFI_MODE_AUTH;const uint16_t miss[][2]={{11,170},{308,170},{12,147},{12,204},{12,257},{12,314}};
 for(unsigned i=0;i<sizeof(miss)/sizeof(miss[0]);++i)route_tap(miss[i][0],miss[i][1]);CHECK(!eapol_ui_is_open()&&!sae_opened&&!starts);route_auth_back();CHECK(wifi_mode==UI_WIFI_MODE_MENU&&!starts);
}
static void tests(void)
{
 enter();CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"SAVE PCAP OFF")&&strstr(drawn,"EAPOL MONITOR"));
 memset(wifi_selected_result.bssid,0xff,6);wifi_selected_result.channel=149;start();CHECK(!memcmp(capture_target.bssid,ap,6)&&capture_target.channel==6);leave();
 enter();prior_live=true;route_tap(100,390);current_page=UI_PAGE_OTHER;wifi_selected_result.channel=149;memset(wifi_selected_result.bssid,0xff,6);snprintf(wifi_selected_result.ssid,sizeof(wifi_selected_result.ssid),"Changed AP");
 capture_enter();CHECK(capture_mode_index==3&&capture_target.channel==6&&eapol_ui->target.channel==6);
 capture_target=wifi_selected_result;snprintf(capture_ssid,sizeof(capture_ssid),"Changed AP");current_page=UI_PAGE_WIFI;prior_live=false;tick(2,false);
 CHECK(starts==1&&model.capture.busy&&eapol_ui->target.channel==6&&!memcmp(eapol_ui->target.bssid,ap,6));CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"Field Kit")&&!strstr(drawn,"Changed AP"));leave();
 enter();prior_live=prior_dual=prior_capture=true;route_tap(100,390);tick(8,false);CHECK(!starts&&eapol_ui_pending());
 route_back();route_tap(100,390);tick(8,false);CHECK(!starts&&eapol_ui_is_open());prior_live=prior_dual=prior_capture=false;tick(3,false);CHECK(!starts&&!eapol_ui_is_open());
 enter();start();hold_stop=true;route_tap(100,390);tick(8,false);CHECK(model.capture.busy&&eapol_ui_is_open());route_back();route_tap(100,390);tick(8,false);CHECK(starts==1&&model.capture.busy);
 hold_stop=false;cerberus_s3_eapol_stop();tick(3,false);CHECK(!eapol_ui_is_open()&&wifi_mode==UI_WIFI_MODE_TARGET);
 enter();start();model.summary.phase[0]=12;route_tap(100,390);tick(2,false);CHECK(!model.capture.busy&&eapol_ui_is_open()&&eapol_ui->status.summary.phase[0]==12);
 CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"RESULTS RETAINED"));route_tap(100,390);tick(2,false);CHECK(starts==2&&model.capture.busy&&!model.summary.phase[0]);leave();
 enter();start();ignored_stops=1;route_tap(100,390);CHECK(model.capture.busy&&eapol_ui_pending());tick(2,false);CHECK(!model.capture.busy&&!eapol_ui_pending());leave();
 enter();start();ignored_stops=2;route_back();tick(1,false);CHECK(model.capture.busy&&eapol_ui_is_open());tick(2,false);CHECK(!model.capture.busy&&!eapol_ui_is_open());
 enter();start();hold_stop=true;current_page=UI_PAGE_OTHER;tick(8,false);CHECK(model.capture.busy&&eapol_ui_is_open());hold_stop=false;tick(3,false);CHECK(!eapol_ui_is_open()&&current_page==UI_PAGE_OTHER);
 current_page=UI_PAGE_WIFI;CHECK(draw_detail_page()==ESP_OK&&eapol_ui_is_open());route_tap(100,390);tick(2,false);CHECK(starts==2&&!generic_starts&&model.capture.busy);leave();
 enter();start();hold_stop=true;tick(4,true);CHECK(model.capture.busy);hold_stop=false;tick(3,true);CHECK(!model.capture.busy&&eapol_ui_is_open());tick(4,false);CHECK(starts==1);leave();
 enter();prior_live=true;route_tap(100,390);tick(2,true);prior_live=false;tick(3,false);CHECK(!starts&&!eapol_ui_pending());leave();
 enter();release_error=ESP_ERR_TIMEOUT;route_tap(100,160);tick(3,false);CHECK(eapol_ui_is_open()&&capture_mode_index==3);route_back();release_error=ESP_OK;tick(2,false);CHECK(!eapol_ui_is_open()&&wifi_mode==UI_WIFI_MODE_TARGET);
 enter();route_tap(100,160);tick(2,false);CHECK(!eapol_ui_is_open()&&capture_mode_index==4&&sae_opened&&!generic_starts);route_back();CHECK(!sae_opened);
 enter();route_tap(50,300);CHECK(eapol_ui->save);route_tap(100,390);tick(2,false);CHECK(model.capture.save_pcap);route_tap(50,300);CHECK(eapol_ui->save);leave();
 enter();c5_status.firmware_version=86;route_tap(100,390);tick(2,false);CHECK(!starts&&eapol_ui->error==ESP_ERR_NOT_SUPPORTED);CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"CAPABILITY 87"));leave();
 enter();start_error=ESP_ERR_NO_MEM;route_tap(100,390);tick(2,false);CHECK(!model.capture.busy);CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"ESP_ERR_NO_MEM"));leave();
 reset();oom=true;for(unsigned i=0;i<3;++i)route_tap(100,160);CHECK(eapol_ui_is_open()&&!eapol_ui);CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"ESP_ERR_NO_MEM"));route_back();CHECK(!eapol_ui_is_open());
 enter();const uint16_t misses[][2]={{11,390},{308,390},{12,381},{12,412},{108,360},{111,360},{208,360},{211,360}};
 for(unsigned i=0;i<sizeof(misses)/sizeof(misses[0]);++i)route_tap(misses[i][0],misses[i][1]);CHECK(!starts&&!eapol_ui_pending()&&!eapol_ui->save&&eapol_ui->view==EAPOL_VIEW_OPTIONS);leave();
 enter();start();route_launcher();tick(3,false);CHECK(!eapol_ui_is_open()&&wifi_mode==UI_WIFI_MODE_TARGET);CHECK(allocations==freed);
 const uint8_t legacy[]={0,1,2};for(unsigned i=0;i<sizeof(legacy);++i){reset();capture_mode_index=legacy[i];route_tap(100,380);CHECK(generic_starts==1&&generic.mode_mask==capture_modes[legacy[i]]);route_tap(100,380);CHECK(generic_stops==1);route_back();CHECK(wifi_mode==UI_WIFI_MODE_TARGET);}
}
static void save(const char*folder,const char*name)
{char path[1024];snprintf(path,sizeof(path),"%s/%s.ppm",folder,name);FILE*f=fopen(path,"wb");CHECK(f);fprintf(f,"P6\n320 480\n255\n");
 for(unsigned i=0;i<320*480;++i){uint16_t p=screen[i];fputc((p>>11)*255/31,f);fputc(((p>>5)&63)*255/63,f);fputc((p&31)*255/31,f);}CHECK(!fclose(f));}
static void context_previews(const char*folder)
{
 enter_menu();CHECK(draw_detail_page()==ESP_OK);save(folder,"eapol-all-options");
 choose_scope(EAPOL_SCOPE_SELECTED);CHECK(draw_detail_page()==ESP_OK);save(folder,"eapol-selected-options");
 choose_scope(EAPOL_SCOPE_ALL);start();model.capture.hop_valid=true;model.capture.channel=149;model.capture.completed_sweeps=2;
 model.capture.attempted_mask=15;model.capture.applied_mask=7;tick(10,false);
 CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"RX CHANNEL 149")&&strstr(drawn,"TUNED 3 / TRIED 4 / SKIPPED 1"));save(folder,"eapol-hop-summary");
 model.capture.hop_error=ESP_ERR_TIMEOUT;tick(10,false);CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"TELEMETRY UNAVAILABLE")&&!strstr(drawn,"TUNED 3"));
 save(folder,"eapol-telemetry-error");model.capture.hop_error=ESP_OK;model.capture.channel=0;tick(10,false);CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"TUNING / NO CURRENT RX CHANNEL"));
 model.summary.context_count=3;model.summary.beacon_malformed=2;model.summary.beacon_dropped=4;model.summary.count=2;
 for(unsigned i=0;i<3;++i){contexts[i]=(cerberus_s3_eapol_context_row_t){.generation=model.generation,.last_received_age_ms=25U+i,
 .evidence={.bssid={2,0x10,0x20,0x30,0x40,(uint8_t)(0x50+i)},.ssid={'A',0,13,255,'B'},.ssid_length=5,.channel=149,.advertised_channel=36,.capabilities=0x431,.rssi=-43,.rssi_valid=true,.frames=3}};}
 for(unsigned i=0;i<2;++i){rows[i]=(cerberus_s3_eapol_row_t){.generation=model.generation,.last_received_age_ms=7890U,
 .evidence={.bssid={2,0x10,0x20,0x30,0x40,(uint8_t)(0x50+i)},.station={2,0xaa,0xbb,0xcc,0xdd,0xee},.channel=6,.last_phase=2,.rssi=-50,.rssi_valid=true}};}
 tick(10,false);route_tap(250,360);CHECK(eapol_ui->view==EAPOL_VIEW_CONTEXT);
 CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"UNAUTHENTICATED")&&strstr(drawn,"A\\x00\\x0D\\xFFB")&&strstr(drawn,"BEACON DELIVERY 0.025S AGO")&&strstr(drawn,"RX CH 149 / ADVERTISED CH 36")&&strstr(drawn,"BAD BEACONS 2 / LIMIT 4"));save(folder,"eapol-beacon-context");
 route_tap(250,330);CHECK(eapol_ui->context_offset==2&&!eapol_ui->offset);route_tap(20,330);CHECK(!eapol_ui->context_offset);
 route_tap(150,360);CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"AP 02:10:20:30:40:50")&&strstr(drawn,"AP 02:10:20:30:40:51")&&strstr(drawn,"KEY DELIVERY 7.890S AGO")&&!strstr(drawn,"0.025S"));save(folder,"eapol-distinct-ap-pairs");
 route_tap(250,360);masked=true;contexts[0].last_received_age_ms=UINT32_MAX;contexts[0].evidence.rssi_valid=false;contexts[0].evidence.advertised_channel=0;
 CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"SSID HIDDEN BY PRIVACY")&&strstr(drawn,"BEACON DELIVERY AGE UNKNOWN")&&strstr(drawn,"RSSI UNKNOWN")&&strstr(drawn,"ADVERTISED CH ?")&&!strstr(drawn,"02:10:20")&&!strstr(drawn,"xFF"));save(folder,"eapol-beacon-private");
 masked=false;contexts[0].evidence.ssid_length=32;memset(contexts[0].evidence.ssid,0xff,32);CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"..."));
 contexts[0].evidence.ssid_length=0;CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"SSID EMPTY / HIDDEN-LIKE"));
 page_error=ESP_ERR_TIMEOUT;CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"OBSERVATION COPY UNAVAILABLE")&&!strstr(drawn,"BEACON AP"));page_error=ESP_OK;
 ++model.generation;CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"SESSION CHANGED")&&!strstr(drawn,"BEACON AP"));--model.generation;
 route_tap(250,360);route_tap(100,390);model.capture.channel=0;tick(10,false);CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"COVERAGE RETAINED")&&!strstr(drawn,"CHANNEL 0"));
 save(folder,"eapol-planned-stopped");leave();reset();wifi_mode=UI_WIFI_MODE_AUTH;CHECK(draw_detail_page()==ESP_OK);save(folder,"auth-monitor-menu");
}
static void previews(const char*folder)
{
 enter();CHECK(draw_detail_page()==ESP_OK);save(folder,"eapol-ready");start();
 model.summary=(cerberus_wifi_eapol_summary_t){.received=24,.phase={8,6,5,5},.exchanges=5,.retries=4,.pmkid_frames=3,.count=3};
 model.capture.received=24;model.capture.dropped_full=2;
 for(unsigned i=0;i<3;++i){rows[i].generation=model.generation;rows[i].evidence=(cerberus_wifi_eapol_record_t){.station={2,0xaa,0xbb,0xcc,0xdd,(uint8_t)i},.bssid={2,0x10,0x20,0x30,0x40,0x50},.last_phase=4,.rssi=-42,.rssi_valid=true,.channel=6,.exchanges=2,.pmkid_frames=1};rows[i].last_received_age_ms=1234;}
 tick(10,false);CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"OBSERVED EXCHANGES 5")&&strstr(drawn,"PMKID KDE FRAMES 3"));save(folder,"eapol-summary");
 route_tap(200,360);CHECK(eapol_ui->view==EAPOL_VIEW_ROWS);CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"KEY DELIVERY 1.234S AGO")&&strstr(drawn,"C5 CH 6 / -42 DBM"));save(folder,"eapol-peers");
 route_tap(250,330);CHECK(eapol_ui->offset==2);route_tap(20,330);CHECK(eapol_ui->offset==0);page_error=ESP_ERR_TIMEOUT;
 CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"OBSERVATION COPY UNAVAILABLE"));page_error=ESP_OK;masked=true;rows[0].last_received_age_ms=UINT32_MAX;rows[0].evidence.rssi_valid=false;
 CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"RSSI UNKNOWN")&&strstr(drawn,"DELIVERY AGE UNKNOWN")&&!strstr(drawn,"02:AA:BB"));save(folder,"eapol-private");
 rows[0].evidence.last_phase=CERBERUS_WIFI_EAPOL_NONE;CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"LAST KEY")&&!strstr(drawn,"M0"));
 route_tap(200,360);model.summary.saturated=true;for(unsigned i=0;i<4;++i)model.summary.phase[i]=UINT32_MAX;model.summary.exchanges=UINT32_MAX;tick(10,false);
 CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,">=4294967295"));save(folder,"eapol-saturated");
 hold_stop=true;read_error=ESP_ERR_TIMEOUT;route_back();tick(10,false);CHECK(eapol_ui_is_open());CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"ESP_ERR_TIMEOUT"));save(folder,"eapol-stop-error");
 read_error=ESP_OK;hold_stop=false;cerberus_s3_eapol_stop();tick(3,false);CHECK(!eapol_ui_is_open());
 enter();route_tap(50,300);route_tap(100,390);tick(2,false);model.capture.written=19;model.capture.bytes_written=10240;snprintf(model.capture.file_name,sizeof(model.capture.file_name),"WCAP0019.PCAPNG");
 route_tap(100,390);tick(2,false);CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"PCAP HAS NO TIMES")&&strstr(drawn,"SD WRITTEN 19")&&strstr(drawn,"WCAP0019.PCAPNG"));save(folder,"eapol-saved");leave();
 enter();unsigned before=calls;CHECK(draw_detail_page()==ESP_OK);unsigned count=calls-before;
 for(unsigned i=1;i<=count;++i){fail_after=calls+i;CHECK(draw_detail_page()==ESP_FAIL);fail_after=0;}leave();CHECK(allocations==freed);
}
int main(int argc,char**argv)
{CHECK(argc==2);tests();scope_tests();context_previews(argv[1]);previews(argv[1]);printf("Wi-Fi EAPOL UI: %u checks; optional PSRAM %zu bytes\n",checks,sizeof(eapol_ui_store_t));return 0;}
