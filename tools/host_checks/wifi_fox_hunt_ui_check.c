#include "fox_framebuffer.h"
#include "fox_modes.h"
#include "wifi_fox_hunt_scan.h"
#include "wifi_packet_monitor_core.h"
#include "wifi_beacon_analysis.h"
#include "fox_label.h"

#define UI_BACKGROUND 0x0042
#define UI_PANEL 0x08A4
#define UI_MUTED 0x7CB5
#define UI_TEXT 0xE77F
#define UI_GOOD 0x471B
#define UI_BAD 0xF471
#define TAG "fox-ui"
#define MALLOC_CAP_SPIRAM 4U
#define MALLOC_CAP_8BIT 2U
#define CHECK(x) do { ++checks; assert(x); } while (0)
#define ESP_ERROR_CHECK(x) CHECK((x)==ESP_OK)
static unsigned checks,allocations,freed,starts,stops,wire_calls,old_stops,draws;
static uint64_t clock_ms,session=10U;
static uint32_t sequence,signal_age;
static uint8_t receiver_channel,signal_channel,band;
static int8_t signal_rssi;
static bool held,hold_stop,oom,masked,lose_start,searching,skipped,c5_online=true;
static bool prior_live,prior_dual,prior_pine,prior_multi;
static esp_err_t stop_error,stop_transport_error;
enum {UI_PAGE_WIFI,UI_PAGE_OTHER,SENTINEL_SCAN_NONE};
static unsigned current_page=UI_PAGE_WIFI;
static ui_wifi_mode_t wifi_mode=UI_WIFI_MODE_MENU;
static struct {uint32_t firmware_version;} c5_status={83};
static int live_session,live_ble_session;
typedef struct {bool busy;} cerberus_dual_wifi_status_t;
static void dual_wifi_live_cancel_handoff(void) {++old_stops;}
static void dual_wifi_ui_leave(void) {++old_stops;}
static void wifi_relation_cancel(void) {++old_stops;}
static void sentinel_scan_session_select(int *s,unsigned mode) {(void)s;CHECK(mode==SENTINEL_SCAN_NONE);++old_stops;}
static void pinescan_request_stop(void) {++old_stops;}
static void multissid_request_stop(void) {++old_stops;}
static bool pinescan_busy(void) {return prior_pine;}
static bool multissid_busy(void) {return prior_multi;}
static bool dual_wifi_live_released(void) {return !prior_live;}
static void cerberus_dual_wifi_scan_get_status(cerberus_dual_wifi_status_t *s) {s->busy=prior_dual;}
static bool dual_wifi_ui_busy(const cerberus_dual_wifi_status_t *s) {return s->busy;}
static void *heap_caps_calloc(size_t n,size_t size,unsigned caps)
{CHECK(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));if(oom)return NULL;void*p=calloc(n,size);CHECK(p);++allocations;return p;}
static void heap_caps_free(void*p) {if(p){++freed;free(p);}}
static bool privacy_addresses_masked(void) {return masked;}
static void cerberus_privacy_format_wifi_identifier(const uint8_t*a,bool mask,char*out,size_t n)
{if(mask)snprintf(out,n,"**:**:**:**:**:**");else snprintf(out,n,"%02X:%02X:%02X:%02X:%02X:%02X",a[0],a[1],a[2],a[3],a[4],a[5]);}
static const char *esp_err_to_name(esp_err_t e)
{return e==ESP_ERR_TIMEOUT?"ESP_ERR_TIMEOUT":e==ESP_ERR_INVALID_STATE?"ESP_ERR_INVALID_STATE":"ESP_FAIL";}
static esp_err_t draw_centered(uint16_t y,const char*text,uint16_t color,uint8_t scale)
{CHECK(strlen(text)*6U*scale<=320U);return cerberus_display_draw_text((uint16_t)((320U-strlen(text)*6U*scale)/2U),y,text,color,UI_BACKGROUND,scale);}
static esp_err_t draw_header(const char *text) {return draw_centered(20,text,UI_TEXT,2);}
static esp_err_t draw_button(uint16_t x,uint16_t y,uint16_t w,uint16_t h,const char*text)
{CHECK(strlen(text)*12U<=w);esp_err_t e=cerberus_display_fill_rect(x,y,w,h,UI_PANEL);
 return e?e:cerberus_display_draw_text((uint16_t)(x+(w-strlen(text)*12U)/2U),y+(h-14U)/2U,text,UI_TEXT,UI_PANEL,2);}
static void status(cerberus_wifi_fox_hunt_status_t*r)
{*r=(cerberus_wifi_fox_hunt_status_t){.session_id=session,.latest_sequence=sequence,.current_channel=held?receiver_channel:0,.last_target_channel=sequence?signal_channel:0,
 .state=held?CERBERUS_WIFI_PACKET_MONITOR_RUNNING:CERBERUS_WIFI_PACKET_MONITOR_IDLE,.sweeps=3,
 .flags=CERBERUS_WIFI_FOX_HUNT_BOUND|(held?CERBERUS_WIFI_FOX_HUNT_HELD|CERBERUS_WIFI_FOX_HUNT_RECEIVING:0U)|
 (searching?CERBERUS_WIFI_FOX_HUNT_SEARCHING:0U)|(skipped?CERBERUS_WIFI_FOX_HUNT_CHANNELS_SKIPPED:0U),
 .error_code=skipped?ESP_ERR_INVALID_STATE:ESP_OK};}
static esp_err_t c5_spi_link_wifi_fox_hunt_info(cerberus_wifi_fox_hunt_status_t*r)
{++wire_calls;*r=(cerberus_wifi_fox_hunt_status_t){.session_id=session};if(held)status(r);return ESP_OK;}
static esp_err_t c5_spi_link_wifi_fox_hunt_start(const cerberus_wifi_fox_hunt_start_t*q,cerberus_wifi_fox_hunt_status_t*r)
{++wire_calls;CHECK(!held&&q->session_id>session&&cerberus_wifi_fox_hunt_start_valid(q));CHECK(!prior_live&&!prior_dual&&!prior_pine&&!prior_multi);
 ++starts;session=q->session_id;receiver_channel=q->initial_channel;band=q->search_band;sequence=0;held=true;status(r);
 if(lose_start){lose_start=false;return ESP_ERR_TIMEOUT;}return ESP_OK;}
static esp_err_t c5_spi_link_wifi_fox_hunt_status(uint64_t id,cerberus_wifi_fox_hunt_status_t*r)
{++wire_calls;CHECK(id==session);status(r);return ESP_OK;}
static esp_err_t c5_spi_link_wifi_fox_hunt_stop(uint64_t id,cerberus_wifi_fox_hunt_status_t*r)
{++wire_calls;++stops;CHECK(id==session);if(stop_transport_error)return stop_transport_error;if(!hold_stop)held=false;status(r);
 if(stop_error){r->state=CERBERUS_WIFI_PACKET_MONITOR_ERROR;r->error_code=stop_error;}return ESP_OK;}
static esp_err_t c5_spi_link_wifi_fox_hunt_signal(uint64_t id,cerberus_wifi_fox_hunt_signal_t*r)
{++wire_calls;CHECK(id==session);*r=(cerberus_wifi_fox_hunt_signal_t){.session_id=id,.sequence=sequence,
 .age_ms=sequence?signal_age:UINT32_MAX,.rssi=sequence?signal_rssi:INT8_MIN,.channel=sequence?signal_channel:0U,.observed_ms=sequence?1000U:0U};return ESP_OK;}
static esp_err_t draw_detail_page(void);
#include "wifi_fox_hunt_ui.h"
#include "fox_routes.h"
static esp_err_t draw_detail_page(void) {++draws;drawn_size=0;drawn[0]=0;return route_draw();}
static void ticks(unsigned n,bool awake)
{for(unsigned i=0;i<n;++i){clock_ms+=25U;unsigned before=wire_calls;route_tick(clock_ms,!awake);CHECK(wire_calls-before<=1U);}}
static const uint8_t target[6]={2,0x10,0x20,0x30,0x40,0x50};
static void reset(void)
{CHECK(!wifi_fox_hunt_ui&&!held);clock_ms=1000U;starts=stops=wire_calls=old_stops=draws=0;sequence=signal_age=0;
 signal_rssi=-48;signal_channel=6;oom=masked=lose_start=hold_stop=searching=skipped=false;
 prior_live=prior_dual=prior_pine=prior_multi=false;stop_error=stop_transport_error=ESP_OK;
 c5_online=true;c5_status.firmware_version=83;current_page=UI_PAGE_WIFI;wifi_mode=UI_WIFI_MODE_TARGET;fail_after=0;}
static void enter(unsigned origin,uint8_t channel)
{reset();uint8_t bytes[]={'F','i','e','l','d',0,'K','i','t'};uint8_t address[6];memcpy(address,target,6);
 CHECK(wifi_fox_hunt_open(address,channel,bytes,sizeof(bytes),(uint8_t)origin)==ESP_OK);
 memset(address,0xff,6);memset(bytes,0xff,sizeof(bytes));CHECK(wifi_fox_hunt_ui&&wifi_mode==UI_WIFI_MODE_FOX_HUNT&&!wire_calls);
 CHECK(!memcmp(wifi_fox_hunt_ui->target,target,6)&&wifi_fox_hunt_ui->label[0]=='F'&&wifi_fox_hunt_holds_source((uint8_t)origin));
 ticks(2,true);CHECK(!wire_calls&&!wifi_fox_hunt_ui_busy());}
static void start(void) {route_tap(20,366);ticks(4,true);CHECK(held&&starts==1&&wifi_fox_hunt_ui_busy());}
static void leave(unsigned origin) {route_global_tap(20,420);ticks(50,true);CHECK(!wifi_fox_hunt_ui&&!held&&wifi_mode==origin);}
static void navigation(void)
{
 reset();oom=true;CHECK(wifi_fox_hunt_open(target,6,NULL,0,UI_WIFI_MODE_TARGET)==ESP_ERR_NO_MEM&&!old_stops&&!wire_calls);oom=false;
 c5_status.firmware_version=82;CHECK(wifi_fox_hunt_open(target,6,NULL,0,UI_WIFI_MODE_TARGET)==ESP_ERR_NOT_SUPPORTED&&!old_stops);
 reset();CHECK(wifi_fox_hunt_open(target,0,NULL,0,UI_WIFI_MODE_TARGET)==ESP_ERR_INVALID_ARG);
 CHECK(wifi_fox_hunt_open(target,6,NULL,1,UI_WIFI_MODE_TARGET)==ESP_ERR_INVALID_ARG);
 const unsigned origins[]={UI_WIFI_MODE_TARGET,UI_WIFI_MODE_OBSERVED_CLIENTS,UI_WIFI_MODE_PINESCAN,UI_WIFI_MODE_MULTISSID};
 for(unsigned i=0;i<4;++i){enter(origins[i],i&1?36:6);start();CHECK(band==(i==1?CERBERUS_WIFI_FOX_HUNT_BAND_BOTH:i&1?CERBERUS_WIFI_FOX_HUNT_BAND_5G:CERBERUS_WIFI_FOX_HUNT_BAND_2G));leave(origins[i]);}
 enter(UI_WIFI_MODE_TARGET,6);prior_live=prior_dual=true;route_tap(20,366);ticks(10,true);CHECK(!wire_calls&&wifi_fox_hunt_ui->start_pending&&!wifi_fox_hunt_ui_busy());
 prior_live=false;ticks(2,true);CHECK(!wire_calls);route_global_tap(20,420);ticks(2,true);CHECK(wifi_fox_hunt_ui&&!wire_calls);
 prior_dual=false;ticks(2,true);CHECK(!wifi_fox_hunt_ui&&!starts&&wifi_mode==UI_WIFI_MODE_TARGET);
 for(unsigned i=2;i<4;++i){enter(origins[i],6);if(i==2)prior_pine=true;else prior_multi=true;route_tap(20,366);ticks(10,true);CHECK(!wire_calls);
  route_launcher();ticks(2,true);CHECK(wifi_fox_hunt_ui&&!starts);prior_pine=prior_multi=false;ticks(2,true);CHECK(!wifi_fox_hunt_ui&&!starts&&wifi_mode==origins[i]);}
 enter(UI_WIFI_MODE_TARGET,6);
 const uint16_t misses[][2]={{7,366},{312,366},{8,365},{8,408},{7,420},{312,420},{8,419},{8,464}};
 for(unsigned i=0;i<sizeof(misses)/sizeof(misses[0]);++i){route_global_tap(misses[i][0],misses[i][1]);CHECK(!wifi_fox_hunt_ui->start_pending&&!wifi_fox_hunt_ui->closing);}
 start();sequence=1;ticks(24,true);CHECK(wifi_fox_hunt_ui->scan.have_signal);
 hold_stop=true;route_tap(20,366);ticks(12,true);CHECK(held);route_global_tap(20,420);route_tap(20,366);ticks(12,true);
 CHECK(wifi_fox_hunt_ui->closing&&held&&starts==1);hold_stop=false;ticks(50,true);CHECK(!held&&!wifi_fox_hunt_ui&&wifi_mode==UI_WIFI_MODE_TARGET);
 enter(UI_WIFI_MODE_TARGET,6);start();sequence=1;ticks(20,true);route_tap(20,366);ticks(30,true);CHECK(!held&&wifi_fox_hunt_ui);
 unsigned retained=wifi_fox_hunt_ui->scan.history_count;CHECK(retained);route_tap(20,366);ticks(5,true);CHECK(starts==2&&held&&wifi_fox_hunt_ui->scan.history_count>=retained);leave(UI_WIFI_MODE_TARGET);
 enter(UI_WIFI_MODE_TARGET,6);lose_start=true;route_tap(20,366);ticks(2,true);CHECK(held&&starts==1);route_global_tap(20,420);ticks(50,true);CHECK(!held&&!wifi_fox_hunt_ui);
 enter(UI_WIFI_MODE_TARGET,6);start();hold_stop=true;CHECK(!wifi_fox_hunt_prepare_sleep());ticks(10,false);CHECK(held&&starts==1);
 hold_stop=false;ticks(40,false);CHECK(!held&&wifi_fox_hunt_ui&&wifi_fox_hunt_prepare_sleep());ticks(20,true);CHECK(starts==1);leave(UI_WIFI_MODE_TARGET);
 enter(UI_WIFI_MODE_TARGET,6);prior_live=true;route_tap(20,366);CHECK(!wifi_fox_hunt_prepare_sleep());prior_live=false;ticks(20,true);CHECK(!starts&&!wifi_fox_hunt_ui->start_pending);leave(UI_WIFI_MODE_TARGET);
 enter(UI_WIFI_MODE_TARGET,6);start();hold_stop=true;current_page=UI_PAGE_OTHER;ticks(10,true);CHECK(held&&wifi_fox_hunt_ui);hold_stop=false;ticks(40,true);CHECK(!held&&!wifi_fox_hunt_ui&&current_page==UI_PAGE_OTHER);
 enter(UI_WIFI_MODE_TARGET,6);start();hold_stop=true;stop_error=ESP_FAIL;route_launcher();ticks(30,true);CHECK(held&&wifi_fox_hunt_ui&&strstr(drawn,"RELEASE RETRY: ESP_FAIL"));
 stop_error=ESP_OK;hold_stop=false;ticks(40,true);CHECK(!wifi_fox_hunt_ui&&!held);
 enter(UI_WIFI_MODE_TARGET,6);start();stop_transport_error=ESP_ERR_TIMEOUT;route_launcher();ticks(30,true);CHECK(held&&wifi_fox_hunt_ui&&strstr(drawn,"RELEASE RETRY: ESP_ERR_TIMEOUT"));
 stop_transport_error=ESP_OK;ticks(40,true);CHECK(!wifi_fox_hunt_ui&&!held);CHECK(allocations==freed);
}
static void save(const char*folder,const char*name)
{char path[1024];snprintf(path,sizeof(path),"%s/%s.ppm",folder,name);FILE*f=fopen(path,"wb");CHECK(f);fprintf(f,"P6\n320 480\n255\n");
 for(unsigned i=0;i<320*480;++i){uint16_t p=screen[i];fputc((p>>11)*255/31,f);fputc(((p>>5)&63)*255/63,f);fputc((p&31)*255/31,f);}CHECK(!fclose(f));}
static void previews(const char*folder)
{
 enter(UI_WIFI_MODE_TARGET,6);CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"READY")&&strstr(drawn,"Field\\x00Kit")&&strstr(drawn,"-- DBM"));save(folder,"fox-ready");
 start();for(unsigned i=1;i<=20;++i){sequence=i;signal_rssi=(int8_t)(-85+(int)i*2);ticks(4,true);}
 CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"TRACKING")&&strstr(drawn,"-45 DBM")&&strstr(drawn,"AGE <="));save(folder,"fox-tracking");
 searching=true;receiver_channel=11;signal_age=2000;ticks(30,true);CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"SEARCHING")&&strstr(drawn,"-- DBM")&&strstr(drawn,"LAST -45 DBM"));save(folder,"fox-searching");
 searching=false;receiver_channel=signal_channel=11;sequence++;signal_age=0;ticks(10,true);CHECK(strstr(drawn,"TRACKING"));
 skipped=true;ticks(30,true);CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"CHANNELS SKIPPED"));save(folder,"fox-warning");
 route_tap(20,366);ticks(40,true);CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"STOPPED")&&strstr(drawn,"-- DBM"));save(folder,"fox-stopped");
 memset(wifi_fox_hunt_ui->label,0xff,32);wifi_fox_hunt_ui->label_length=32;masked=true;wifi_fox_hunt_ui->now_ms=UINT64_C(5000000000);
 CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"\\xFF")&&strstr(drawn,"**:**:**:**:**:**"));save(folder,"fox-binary-old");
 unsigned count=calls;CHECK(draw_detail_page()==ESP_OK);count=calls-count;
 for(unsigned i=1;i<=count;++i){fail_after=calls+i;CHECK(draw_detail_page()==ESP_FAIL);fail_after=0;}
 leave(UI_WIFI_MODE_TARGET);CHECK(allocations==freed);
}
int main(int argc,char**argv)
{CHECK(argc==2);navigation();previews(argv[1]);printf("Wi-Fi Fox Hunt UI: %u checks; optional PSRAM %zu bytes\n",checks,sizeof(wifi_fox_hunt_ui_store_t));return 0;}
