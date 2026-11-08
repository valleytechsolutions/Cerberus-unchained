#include "analyzer_framebuffer.h"
#include "analyzer_modes.h"
#include "wifi_channel_analyzer_scan.h"
#include "wifi_packet_monitor_core.h"
#include "sentinel_hub.h"

#define UI_BACKGROUND 0x0042
#define UI_PANEL 0x08A4
#define UI_MUTED 0x7CB5
#define UI_TEXT 0xE77F
#define UI_GOOD 0x471B
#define UI_BAD 0xF471
#define TAG "analyzer-ui"
#define MALLOC_CAP_SPIRAM 4U
#define MALLOC_CAP_8BIT 2U
#define CHECK(x) do { ++checks; assert(x); } while (0)
#define ESP_ERROR_CHECK(x) CHECK((x)==ESP_OK)
static unsigned checks, allocations, freed, starts, stops, wire_calls, normal_scans;
static uint64_t clock_ms, session=10U;
static uint32_t sequence;
static uint8_t receiver_channel;
static bool held, hold_stop, oom, masked, have_hint, lose_start;
enum {UI_PAGE_WIFI, UI_PAGE_OTHER};
static unsigned current_page=UI_PAGE_WIFI;
static ui_wifi_mode_t wifi_mode=UI_WIFI_MODE_MENU;
static struct { uint32_t firmware_version; } c5_status={82};
static void *heap_caps_calloc(size_t n,size_t size,unsigned caps)
{CHECK(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));if(oom)return NULL;void*p=calloc(n,size);CHECK(p);++allocations;return p;}
static void heap_caps_free(void *p) { if(p){++freed;free(p);} }
static bool privacy_addresses_masked(void) { return masked; }
static void cerberus_privacy_format_wifi_identifier(const uint8_t *a,bool mask,char*out,size_t n)
{ if(mask)snprintf(out,n,"**:**:**:**:**:**");else snprintf(out,n,"%02X:%02X:%02X:%02X:%02X:%02X",a[0],a[1],a[2],a[3],a[4],a[5]); }
static const char *esp_err_to_name(esp_err_t error)
{ return error==ESP_ERR_TIMEOUT?"ESP_ERR_TIMEOUT":error==ESP_ERR_INVALID_STATE?"ESP_ERR_INVALID_STATE":"ESP_FAIL"; }
static esp_err_t draw_centered(uint16_t y,const char *text,uint16_t color,uint8_t scale)
{CHECK(strlen(text)*6U*scale<=320U);return cerberus_display_draw_text((uint16_t)((320U-strlen(text)*6U*scale)/2U),y,text,color,UI_BACKGROUND,scale);}
static esp_err_t draw_button(uint16_t x,uint16_t y,uint16_t w,uint16_t h,const char *text)
{ CHECK(strlen(text)*12U<=w);esp_err_t error=cerberus_display_fill_rect(x,y,w,h,UI_PANEL);
  return error?error:cerberus_display_draw_text((uint16_t)(x+(w-strlen(text)*12U)/2U),y+(h-14U)/2U,text,UI_TEXT,UI_PANEL,2); }
static esp_err_t draw_small_button(uint16_t x,uint16_t y,uint16_t w,uint16_t h,const char *text)
{ CHECK(strlen(text)*6U<=w);esp_err_t error=cerberus_display_fill_rect(x,y,w,h,UI_PANEL);
  return error?error:cerberus_display_draw_text((uint16_t)(x+(w-strlen(text)*6U)/2U),y+(h-7U)/2U,text,UI_TEXT,UI_PANEL,1); }

static void status(cerberus_wifi_channel_analyzer_status_t *r)
{ *r=(cerberus_wifi_channel_analyzer_status_t){.session_id=session,.channel=receiver_channel,
    .latest_sequence=sequence,.stored_count=(uint8_t)(sequence<64U?sequence:64U),.target_ms=100U,
    .flags=CERBERUS_WIFI_CHANNEL_ANALYZER_BOUND|(held?CERBERUS_WIFI_CHANNEL_ANALYZER_HELD:0U),
    .state=held?CERBERUS_WIFI_PACKET_MONITOR_RUNNING:CERBERUS_WIFI_PACKET_MONITOR_IDLE}; }
static esp_err_t c5_spi_link_wifi_channel_analyzer_info(cerberus_wifi_channel_analyzer_status_t *r)
{ ++wire_calls;*r=(cerberus_wifi_channel_analyzer_status_t){.session_id=session};if(held)status(r);return ESP_OK; }
static esp_err_t c5_spi_link_wifi_channel_analyzer_start(uint64_t id,uint8_t ch,cerberus_wifi_channel_analyzer_status_t *r)
{ ++wire_calls;CHECK(!held&&id>session);++starts;session=id;receiver_channel=ch;sequence=0U;held=true;status(r);
  if(lose_start){lose_start=false;return ESP_ERR_TIMEOUT;}return ESP_OK; }
static esp_err_t c5_spi_link_wifi_channel_analyzer_status(uint64_t id,cerberus_wifi_channel_analyzer_status_t *r)
{ ++wire_calls;CHECK(id==session);status(r);return ESP_OK; }
static esp_err_t c5_spi_link_wifi_channel_analyzer_stop(uint64_t id,cerberus_wifi_channel_analyzer_status_t *r)
{ ++wire_calls;++stops;CHECK(id==session);if(!hold_stop)held=false;status(r);return ESP_OK; }
static esp_err_t c5_spi_link_wifi_channel_analyzer_sample(uint64_t id,uint32_t seq,cerberus_wifi_channel_analyzer_sample_t *r)
{ ++wire_calls;CHECK(id==session&&seq&&seq<=sequence);*r=(cerberus_wifi_channel_analyzer_sample_t){
    .session_id=id,.sequence=seq,.channel=receiver_channel,.frames=seq==1U?0U:seq*10U,.window_us=seq==2U?300000U:100000U};return ESP_OK; }
static esp_err_t c5_spi_link_wifi_channel_analyzer_hint_peek(uint64_t id,cerberus_wifi_channel_analyzer_hint_status_t *r)
{ ++wire_calls;CHECK(id==session);*r=(cerberus_wifi_channel_analyzer_hint_status_t){.session_id=id,.age_ms=UINT32_MAX,.error_code=ESP_ERR_NOT_FOUND};
  if(have_hint)*r=(cerberus_wifi_channel_analyzer_hint_status_t){.session_id=id,.projection=1U,.age_ms=250U,.length=42U,.flags=1U};return ESP_OK; }
static esp_err_t c5_spi_link_wifi_channel_analyzer_hint_read(uint64_t id,uint32_t projection,uint8_t part,cerberus_wifi_channel_analyzer_hint_part_t *r)
{ ++wire_calls;CHECK(id==session&&projection==1U&&part<5U);cerberus_wifi_channel_analyzer_hint_t hint={.bssid={2,0x10,0x20,0x30,0x40,0x50},
    .ssid={'F','i','e','l','d',0,'K','i','t'},.ssid_length=9U,.rssi=-47,.channel=receiver_channel,.flags=1U};
  *r=(cerberus_wifi_channel_analyzer_hint_part_t){.session_id=id,.projection=projection,.part=part};
  unsigned offset=part*9U,length=sizeof(hint)-offset;if(length>9U)length=9U;memcpy(r->bytes,(uint8_t*)&hint+offset,length);return ESP_OK; }

static esp_err_t draw_detail_page(void);
static void dual_wifi_live_open(bool start) { CHECK(start&&!held);++normal_scans;wifi_mode=UI_WIFI_MODE_DUAL; }
#include "wifi_channel_analyzer_ui.h"
#include "analyzer_routes.h"
static esp_err_t draw_detail_page(void)
{ drawn_size=0;drawn[0]=0;esp_err_t error=cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND);
  if(!error)error=draw_centered(20,"WI-FI TOOLS",UI_TEXT,2);
  if(!error)error=route_draw();return error?error:route_footer(); }
static void ticks(unsigned n,bool awake)
{ for(unsigned i=0;i<n;++i){clock_ms+=25U;unsigned before=wire_calls;route_tick(clock_ms,!awake);CHECK(wire_calls-before<=1U);} }
static void reset(void)
{ CHECK(!channel_analyzer_ui&&!held);clock_ms=1000U;starts=stops=wire_calls=normal_scans=0;sequence=0;have_hint=hold_stop=lose_start=masked=oom=false;
  c5_status.firmware_version=82U;current_page=UI_PAGE_WIFI;wifi_mode=UI_WIFI_MODE_MENU;fail_after=0; }
static void enter(void)
{ reset();route_menu(30,190);CHECK(channel_analyzer_ui&&wifi_mode==UI_WIFI_MODE_CHANNEL_ANALYZER&&starts==0U);ticks(3,true);
  CHECK(held&&starts==1U&&receiver_channel==1U&&channel_analyzer_ui_busy()); }
static void leave(void)
{ route_back(20,UI_BACK_Y);ticks(80,true);CHECK(!channel_analyzer_ui&&!held&&wifi_mode==UI_WIFI_MODE_MENU); }
static void save(const char *folder,const char *name)
{ char path[1024];snprintf(path,sizeof(path),"%s/%s.ppm",folder,name);FILE*f=fopen(path,"wb");CHECK(f);fprintf(f,"P6\n320 480\n255\n");
  for(unsigned i=0;i<320*480;++i){uint16_t p=screen[i];fputc((p>>11)*255/31,f);fputc(((p>>5)&63)*255/63,f);fputc((p&31)*255/31,f);}CHECK(!fclose(f)); }

static void navigation(void)
{
    reset();oom=true;route_menu(30,190);CHECK(!channel_analyzer_ui&&channel_analyzer_open_error==ESP_ERR_NO_MEM&&!wire_calls);
    route_back(20,UI_BACK_Y);CHECK(wifi_mode==UI_WIFI_MODE_MENU);oom=false;
    reset();c5_status.firmware_version=81;route_menu(30,190);ticks(2,true);CHECK(channel_analyzer_ui&&channel_analyzer_ui->scan.error==ESP_ERR_NOT_SUPPORTED&&!wire_calls);leave();
    /* Cancellation before INFO or START sends no start. */
    for(unsigned n=0;n<2U;++n){reset();route_menu(30,190);ticks(n,true);route_back(20,UI_BACK_Y);ticks(2,true);CHECK(!starts&&!held&&!channel_analyzer_ui);}
    enter();sequence=3;have_hint=true;ticks(120,true);CHECK(channel_analyzer_ui->scan.history_count==4U&&channel_analyzer_ui->scan.hint_valid);
    /* Empty space and button boundaries are inert. */
    const uint16_t misses[][2]={{7,356},{76,356},{83,356},{152,356},{159,356},{312,356},{20,355},{20,398}};
    for(unsigned i=0;i<sizeof(misses)/sizeof(misses[0]);++i){route_tap(misses[i][0],misses[i][1]);CHECK(channel_analyzer_ui->action==CHANNEL_ANALYZER_NONE);}
    route_back(7,420);route_back(312,420);route_back(20,464);CHECK(channel_analyzer_ui->action==CHANNEL_ANALYZER_NONE);
    /* A held cleanup cannot restart or leave. Back overrides a queued CH+. */
    hold_stop=true;route_tap(84,356);route_tap(84,356);route_tap(200,356);CHECK(channel_analyzer_ui->action==CHANNEL_ANALYZER_RETUNE);
    ticks(25,true);CHECK(starts==1U&&held&&wifi_mode==UI_WIFI_MODE_CHANNEL_ANALYZER);
    route_back(20,420);ticks(25,true);CHECK(channel_analyzer_ui->action==CHANNEL_ANALYZER_BACK&&held&&starts==1U);
    hold_stop=false;ticks(60,true);CHECK(!channel_analyzer_ui&&!held&&wifi_mode==UI_WIFI_MODE_MENU&&starts==1U);
    route_menu(30,80);CHECK(normal_scans==1U&&wifi_mode==UI_WIFI_MODE_DUAL);
    /* Retune preserves history and inserts a visible transition on receipt. */
    enter();sequence=3;ticks(30,true);route_tap(84,356);ticks(30,true);CHECK(held&&starts==2U&&receiver_channel==2U);
    CHECK(channel_analyzer_ui->scan.history_count==4U&&!channel_analyzer_ui->scan.current_valid);
    sequence=2;ticks(30,true);wifi_channel_analyzer_summary_t sum;wifi_channel_analyzer_scan_summary(&channel_analyzer_ui->scan,&sum);
    CHECK(sum.windows==5U&&sum.gap_count==1U&&sum.average_rate==77U); /* 70 frames / 0.9 seconds */
    route_tap(160,356);ticks(30,true);CHECK(!held&&channel_analyzer_ui&&!channel_analyzer_ui_busy());
    unsigned retained=channel_analyzer_ui->scan.history_count;route_tap(160,356);ticks(30,true);CHECK(starts==3U&&held&&channel_analyzer_ui->scan.history_count==retained);
    leave();
    /* Lost START acknowledgement still requires an exact Stop. */
    reset();lose_start=true;route_menu(30,190);ticks(2,true);CHECK(held&&starts==1U);hold_stop=true;route_back(20,420);ticks(30,true);
    CHECK(channel_analyzer_ui&&held&&stops);hold_stop=false;ticks(40,true);CHECK(!channel_analyzer_ui&&!held);
    /* Sleep cancels pending retune, completes release while hidden, and does
     * not restart when the original page becomes visible again. */
    enter();sequence=3;ticks(30,true);hold_stop=true;route_tap(84,356);ticks(10,false);CHECK(held&&starts==1U);
    hold_stop=false;ticks(40,false);CHECK(channel_analyzer_ui&&!held&&channel_analyzer_ui->scan.phase==WIFI_ANALYZER_OFF);
    ticks(30,true);CHECK(starts==1U&&!held);route_tap(160,356);ticks(30,true);CHECK(starts==2U&&held);leave();
    enter();hold_stop=true;route_tap(84,356);CHECK(!channel_analyzer_prepare_sleep());ticks(30,true);
    CHECK(held&&starts==1U&&!channel_analyzer_prepare_sleep());hold_stop=false;ticks(30,true);
    CHECK(!held&&starts==1U&&channel_analyzer_prepare_sleep());leave();
    enter();hold_stop=true;route_launcher();ticks(30,true);CHECK(channel_analyzer_ui&&held&&wifi_mode==UI_WIFI_MODE_CHANNEL_ANALYZER);
    hold_stop=false;ticks(30,true);CHECK(!channel_analyzer_ui&&!held&&wifi_mode==UI_WIFI_MODE_MENU);
    enter();hold_stop=true;current_page=UI_PAGE_OTHER;ticks(30,true);CHECK(channel_analyzer_ui&&held);
    hold_stop=false;ticks(30,true);CHECK(!channel_analyzer_ui&&!held&&current_page==UI_PAGE_OTHER);
    /* Channel stepping wraps between the supported 2.4/5 GHz endpoints. */
    enter();route_tap(8,356);ticks(30,true);CHECK(receiver_channel==165U);route_tap(84,356);ticks(30,true);CHECK(receiver_channel==1U);leave();
    CHECK(allocations==freed);
}

static void previews(const char *folder)
{
    enter();CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"NOW --")&&strstr(drawn,"NO COMPLETED WINDOWS"));save(folder,"analyzer-waiting");
    sequence=1;ticks(30,true);CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"NOW 0/S / 0 FR"));
    sequence=3;have_hint=true;ticks(100,true);CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"Field\\x00Kit")&&strstr(drawn,"AVG 100/S"));save(folder,"analyzer-live");
    route_tap(84,356);ticks(30,true);sequence=70;ticks(120,true);CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"CH2"));save(folder,"analyzer-retuned");
    route_tap(160,356);ticks(100,true);CHECK(!held&&draw_detail_page()==ESP_OK&&strstr(drawn,"STOPPED"));save(folder,"analyzer-stopped");
    wifi_channel_analyzer_scan_t *scan=&channel_analyzer_ui->scan;
    scan->history_count=scan->history_next=2;scan->current_valid=true;
    scan->history[0]=(wifi_channel_analyzer_entry_t){.kind=WIFI_ANALYZER_CHANNEL,.channel=165};
    scan->history[1]=(wifi_channel_analyzer_entry_t){.kind=WIFI_ANALYZER_WINDOW,.channel=165,.frames=UINT32_MAX,.window_us=UINT32_MAX,.flags=1U};
    scan->channel=165;scan->hint_valid=true;scan->hint_age_at_peek_ms=UINT32_MAX;scan->hint.rssi=-127;scan->hint.channel=165;scan->hint.ssid_length=32;
    memset(scan->hint.ssid,0xff,32);masked=true;CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"AGE UNKNOWN")&&strstr(drawn,"**:**:**:**:**:**")&&strstr(drawn,"AVG >=")&&strstr(drawn,"\\xFF"));save(folder,"analyzer-binary-saturated");
    scan->hint.ssid_length=0;CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"<HIDDEN SSID>"));
    scan->hint_age_at_peek_ms=10000U;scan->hint_peek_ms=channel_analyzer_ui->now_ms;CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"STALE"));
    /* Every injected render failure must propagate out of the real drawing
     * route, including its graph, hint and button calls. */
    calls=0;CHECK(draw_detail_page()==ESP_OK);unsigned rendered=calls;
    for(unsigned failure=1;failure<=rendered;++failure){calls=0;fail_after=failure;CHECK(draw_detail_page()==ESP_FAIL);}fail_after=0;
    leave();reset();oom=true;route_menu(30,190);CHECK(draw_detail_page()==ESP_OK);save(folder,"analyzer-no-memory");route_back(20,420);oom=false;
    reset();c5_status.firmware_version=81;route_menu(30,190);CHECK(draw_detail_page()==ESP_OK&&strstr(drawn,"UNSUPPORTED"));save(folder,"analyzer-old-peer");leave();
    static const cerberus_ui_widget_theme_t theme={.background=UI_BACKGROUND,.panel=UI_PANEL,.text=UI_TEXT,.muted=UI_MUTED,.border=UI_GOOD};
    drawn_size=0;drawn[0]=0;CHECK(cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND)==ESP_OK&&draw_centered(20,"WI-FI TOOLS",UI_TEXT,2)==ESP_OK);
    CHECK(sentinel_hub_draw(SENTINEL_HUB_WIFI,&theme)==ESP_OK&&draw_button(8,420,304,44,"BACK")==ESP_OK);save(folder,"analyzer-wifi-menu");
    const int actions[]={0,1,2,11,10,3,4,5};
    for(unsigned i=0;i<8U;++i){CHECK(sentinel_hub_hit_test(SENTINEL_HUB_WIFI,12,70U+i*36U)==actions[i]);CHECK(sentinel_hub_hit_test(SENTINEL_HUB_WIFI,307,101U+i*36U)==actions[i]);
        CHECK(sentinel_hub_hit_test(SENTINEL_HUB_WIFI,30,102U+i*36U)==-1);CHECK(sentinel_hub_hit_test(SENTINEL_HUB_WIFI,11,70U+i*36U)==-1);CHECK(sentinel_hub_hit_test(SENTINEL_HUB_WIFI,308,70U+i*36U)==-1);}
}
int main(int argc,char **argv)
{ CHECK(argc==2);navigation();previews(argv[1]);CHECK(allocations==freed);printf("Channel Analyzer UI: %u checks; optional PSRAM %zu bytes.\n",checks,sizeof(channel_analyzer_ui_store_t));return 0; }
