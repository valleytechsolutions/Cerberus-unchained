#undef NDEBUG
#include "wardrive_service.h"
#include "wardrive_log_feedback.h"
#include "wardrive_gps_uart.h"
#include "gps_config.h"
#include "gps_recorder_ui_stub.h"
#include "ui_widgets.h"
#include "display.h"
#include "esp_check.h"
#include "s3_ble_host.h"
#include "s3_ble_discovery_session.h"
#include "sentinel_scan_session.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "display_font.h"

static unsigned checks, calls, fail_at, status_reads, stop_requests, page_draws, recent_opens;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);abort(); } } while (0)
#define ESP_ERROR_CHECK(x) CHECK((x)==ESP_OK)
#define UI_HEADER_HEIGHT 48U
enum { UI_PAGE_BLE=1, UI_PAGE_IEEE802154=2, UI_PAGE_WARDRIVE=3, UI_PAGE_SETTINGS=4 };
enum { UI_SETTINGS_MODE_GPS=1 };
static unsigned current_page;
static const char *TAG="wardrive_host";
static uint16_t pixels[480][320];
static char drawn[8192], lines[480][128], counter_text[2][16];
static cerberus_wardrive_status_t runtime;
esp_err_t cerberus_wardrive_gps_uart_get_state(cerberus_wardrive_gps_uart_state_t *out)
{memset(out,0,sizeof(*out));out->gps=runtime.gps;out->error=runtime.gps_error;out->active_baud=115200;
 out->driver_active=out->reader_running=out->clock_valid=true;return ESP_OK;}
esp_err_t cerberus_wardrive_start_check(bool enabled)
{cerberus_wardrive_gps_uart_state_t s;cerberus_wardrive_gps_uart_get_state(&s);
 return s.error!=ESP_OK?s.error:cerberus_wardrive_gps_start_ready(enabled,&s)?ESP_OK:ESP_ERR_NOT_FINISHED;}
static bool wardrive_start_pending,wardrive_return_pending,wardrive_waiting_for_gps,wardrive_include_expansions;
static uint8_t wardrive_mode;
static esp_err_t gps_preview_error, gps_preview_result;
static bool gps_preview_attached, gps_preview_cleanup;
static uint32_t gps_preview_retry_ms;
static unsigned settings_mode, gps_preview_starts, gps_preview_stops;
static cerberus_gps_config_t gps_settings;
static cerberus_gps_config_t cerberus_settings_gps_get(void){return gps_settings;}
esp_err_t cerberus_wardrive_gps_uart_preview(bool enabled)
{if(enabled)++gps_preview_starts;else ++gps_preview_stops;return gps_preview_result;}
esp_err_t cerberus_wardrive_set_mode(uint8_t mode){wardrive_mode=mode;return ESP_OK;}

static esp_err_t wardrive_start_error;
static char wardrive_blocked_reason[64];
static uint32_t wardrive_start_requested_ms,wardrive_last_start_attempt_ms,wardrive_last_draw_ms;
static int64_t clock_us=1234000;
static sentinel_scan_session_t live_session, live_ble_session;
static uint64_t live_ble_lease_epoch;
static struct { cerberus_ble_discovery_session_t session; } discovery_store, *ble_discovery_live;
static unsigned service_starts, launcher_returns, storage_owner;
static bool recent_is_open, requested_gps, requested_expansions;
static esp_err_t service_start_result;
static cerberus_s3_ble_owner_t host_owner;
enum { CERBERUS_STORAGE_OWNER_USB=2 };
static esp_err_t draw_detail_page(void);
static void wardrive_recent_open(void){++recent_opens;}
static bool wardrive_recent_is_open(void){return recent_is_open;}
static unsigned logs_opened;
static void wardrive_logs_open(void){++logs_opened;}
static bool wardrive_logs_is_open(void){return false;}
static void wardrive_logs_tick(bool asleep){(void)asleep;}
static void wardrive_gps_view_open(void){}
static bool wardrive_gps_view_is_open(void){return false;}
static void wardrive_gps_view_release_if_hidden(uint32_t now_ms,bool asleep){(void)now_ms;(void)asleep;}
static unsigned cerberus_storage_get_owner(void){return storage_owner;}
cerberus_s3_ble_owner_t cerberus_s3_ble_host_owner(void){return host_owner;}
static void return_to_launcher(void){++launcher_returns;current_page=0;}
esp_err_t cerberus_wardrive_start(bool expansions,bool gps)
{++service_starts;requested_expansions=expansions;requested_gps=gps;
 return cerberus_wardrive_start_check(gps)==ESP_OK?service_start_result:ESP_ERR_NOT_FINISHED;}
static int64_t esp_timer_get_time(void){return clock_us;}
static const char *esp_err_to_name(esp_err_t e)
{return e==ESP_ERR_TIMEOUT?"ESP_ERR_TIMEOUT":e==ESP_ERR_NO_MEM?"ESP_ERR_NO_MEM":"ESP_FAIL";}
void cerberus_wardrive_get_status(cerberus_wardrive_status_t *out){++status_reads;*out=runtime;}
void cerberus_wardrive_request_stop(void){++stop_requests;}

esp_err_t cerberus_display_fill_rect(uint16_t x,uint16_t y,uint16_t w,uint16_t h,uint16_t color)
{
    CHECK((unsigned)x+w<=320U&&(unsigned)y+h<=480U);
    if(++calls==fail_at)return ESP_FAIL;
    for(unsigned iy=y;iy<(unsigned)y+h;++iy)for(unsigned ix=x;ix<(unsigned)x+w;++ix)pixels[iy][ix]=color;
    return ESP_OK;
}
esp_err_t cerberus_display_draw_text(uint16_t x,uint16_t y,const char *text,uint16_t fg,uint16_t bg,uint8_t scale)
{
    CHECK(scale==1U||scale==2U);
    CHECK(x+strlen(text)*6U*scale<=320U&&(unsigned)y+7U*scale<=480U);
    if(++calls==fail_at)return ESP_FAIL;
    CHECK(strlen(drawn)+strlen(text)+2U<sizeof(drawn));
    strcat(drawn,text);strcat(drawn,"\n");
    snprintf(lines[y],sizeof(lines[y]),"%s",text);
    if(y==176){CHECK(scale==2);snprintf(counter_text[x>=160],16,"%s",text);}
    for(size_t i=0;text[i];++i){const uint8_t *font=font5x7((unsigned char)text[i]);
        for(unsigned cy=0;cy<7U;++cy)for(unsigned cx=0;cx<6U;++cx)
            for(unsigned sy=0;sy<scale;++sy)for(unsigned sx=0;sx<scale;++sx)
                pixels[y+cy*scale+sy][x+(i*6U+cx)*scale+sx]=cx<5U&&(font[cy]&(1U<<(4U-cx)))?fg:bg;
    }
    return ESP_OK;
}
#include "wardrive_ui_impl.h"

static esp_err_t draw_detail_page(void)
{
    ++page_draws;drawn[0]='\0';memset(lines,0,sizeof(lines));
    ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND),TAG,"clear");
    ESP_RETURN_ON_ERROR(draw_header("WARDRIVING"),TAG,"header");
    return draw_wardrive();
}
static void reset(void)
{
    memset(&runtime,0,sizeof(runtime));wardrive_start_pending=wardrive_return_pending=false;
    wardrive_waiting_for_gps=wardrive_include_expansions=false;wardrive_start_error=ESP_OK;
    wardrive_blocked_reason[0]='\0';calls=fail_at=status_reads=stop_requests=page_draws=recent_opens=0;
    wardrive_last_draw_ms=999;clock_us=1234000;
    live_session=(sentinel_scan_session_t){0};live_ble_session=(sentinel_scan_session_t){0};
    live_ble_lease_epoch=0;memset(&discovery_store,0,sizeof(discovery_store));ble_discovery_live=NULL;
    service_starts=launcher_returns=storage_owner=0;recent_is_open=requested_gps=requested_expansions=false;
    service_start_result=ESP_OK;host_owner=CERBERUS_S3_BLE_OWNER_NONE;current_page=UI_PAGE_WARDRIVE;
    wardrive_start_requested_ms=wardrive_last_start_attempt_ms=0;
    gps_preview_error=gps_preview_result=ESP_OK;gps_preview_attached=gps_preview_cleanup=false;
    gps_preview_retry_ms=settings_mode=gps_preview_starts=gps_preview_stops=0;
    gps_settings=(cerberus_gps_config_t){0};
}
static void draw(void)
{
    unsigned before=status_reads,stops=stop_requests;
    cerberus_wardrive_status_t frozen=runtime;
    CHECK(draw_detail_page()==ESP_OK);
    CHECK(status_reads==before+1U&&stop_requests==stops&&!memcmp(&frozen,&runtime,sizeof(runtime)));
}
static void ready_gps(void)
{
    runtime.gps=(cerberus_wardrive_gps_snapshot_t){.state=CERBERUS_WARDRIVE_GPS_FRESH,
        .coordinates_valid=true,.utc_valid=true,.utc_from_rmc=true};
}
static void save(const char *prefix,const char *name)
{
    char path[1024];snprintf(path,sizeof(path),"%s-%s.ppm",prefix,name);
    FILE *f=fopen(path,"wb");CHECK(f!=NULL);CHECK(fprintf(f,"P6\n320 480\n255\n")>0);
    for(unsigned y=0;y<480;++y)for(unsigned x=0;x<320;++x){
        uint16_t p=pixels[y][x];uint8_t rgb[]={(uint8_t)(((p>>11)&31)*255/31),
            (uint8_t)(((p>>5)&63)*255/63),(uint8_t)((p&31)*255/31)};
        if(fwrite(rgb,1,3,f)!=3){fclose(f);abort();}
    }
    CHECK(fclose(f)==0);
}
static void check_views(const char *prefix)
{
    reset();draw();CHECK(!strcmp(lines[80],"GPS DATA > WAITING FOR RECEIVER"));
    CHECK(!strcmp(counter_text[0],"0")&&!strcmp(counter_text[1],"0")&&strstr(drawn,"START\n"));save(prefix,"idle-no-gps");
    runtime=(cerberus_wardrive_status_t){.state=CERBERUS_WARDRIVE_RUNNING,.c5_wifi_sweeps=12,.c5_wifi_observations=96,
        .c5_stream={.wifi_reports=96},.s3_stream={.ble_reports=173},.s3_ble_sweeps=9,.s3_ble_observations=173,.s3_ble_flock_observations=23,
        .s3_ble_flock_clue=3,.s3_ble_flock_signature=15,.s3_ble_flock_corroborated=5,
        .c5_probe_state=CERBERUS_WARDRIVE_PROBE_AVAILABLE,.c5_probe_observations=4,
        .skipped_no_fix=269};
    draw();CHECK(!strcmp(counter_text[0],"96"));
    CHECK(!strcmp(counter_text[1],"173")&&strstr(drawn,"STOP\n"));
    CHECK(!strcmp(lines[300],"NOT SAVED: 269 / NO FIX 269"));
    CHECK(!strcmp(lines[324],"SCANNING / CSV NEEDS GPS FIX + UTC"));save(prefix,"running-no-gps");
    runtime.c5_stream.wifi_reports=runtime.c5_stream.ble_reports=UINT32_MAX;draw();
    CHECK(!strcmp(counter_text[0],"4294967295")&&!strcmp(counter_text[1],"4294967468"));save(prefix,"max-counts");
    runtime.state=CERBERUS_WARDRIVE_STOPPING;wardrive_return_pending=true;draw();
    CHECK(!strcmp(lines[105],"STATE: STOPPING / SAVING"));
    CHECK(!strcmp(lines[324],"WAITING FOR RADIOS AND FILES")&&strstr(drawn,"STOP\n"));save(prefix,"stopping");
    runtime.compacting=true;draw();
    CHECK(!strcmp(lines[105],"STATE: SAVING RSSI EXTREMES")&&strstr(drawn,"STOP\n"));save(prefix,"compacting");
    runtime.compacting=false;
    runtime.state=CERBERUS_WARDRIVE_STOPPED;wardrive_return_pending=false;draw();
    CHECK(!strcmp(lines[105],"STATE: STOPPED / NO CSV SAVED")&&strstr(drawn,"START\n"));
    CHECK(!strcmp(lines[280],"LAST FILE: NO CSV CREATED"));
    CHECK(!strcmp(lines[324],"GPS WAS OFF / SCAN ONLY"));save(prefix,"stopped-gps-off");
    runtime.gps_requested=true;draw();
    CHECK(!strcmp(lines[324],"NO VALID GPS FIX + UTC DURING CAPTURE"));save(prefix,"stopped-no-fix");
    /* A later receiver fix must not rewrite the completed session's outcome. */
    runtime.gps.state=CERBERUS_WARDRIVE_GPS_FRESH;runtime.gps.utc_valid=true;draw();
    CHECK(!strcmp(lines[324],"NO VALID GPS FIX + UTC DURING CAPTURE"));
    runtime.skipped_no_fix=0;runtime.skipped_time=10;draw();
    CHECK(!strcmp(lines[324],"OBSERVATION TIMES COULD NOT MATCH GPS"));
    runtime.skipped_time=0;runtime.skipped_format=10;draw();
    CHECK(!strcmp(lines[324],"NO OBSERVATIONS IN SUPPORTED CSV FORMAT"));
    runtime.skipped_format=0;draw();CHECK(!strcmp(lines[324],"NO ELIGIBLE OBSERVATIONS RECEIVED"));
    runtime.files_saved=1;runtime.sd_rows=138;strcpy(runtime.last_file_name,"CERBWardrive001.CSV");draw();
    runtime.csv_rows=2;runtime.csv_devices=1;draw();
    CHECK(!strcmp(lines[260],"CSV: 2 ROWS / 1 DEVICES / 0 KiB"));
    CHECK(!strcmp(lines[105],"STATE: STOPPED")&&!strcmp(lines[324],"CSV SAVED / OPEN LOGS BELOW"));save(prefix,"stopped-saved");
    runtime.state=CERBERUS_WARDRIVE_ERROR;runtime.error_code=ESP_ERR_TIMEOUT;runtime.s3_ble_error=ESP_ERR_TIMEOUT;
    draw();CHECK(!strcmp(lines[324],"S3: ESP_ERR_TIMEOUT"));save(prefix,"error");
    runtime=(cerberus_wardrive_status_t){.state=CERBERUS_WARDRIVE_RUNNING,.gps_requested=true,
        .gps={.state=CERBERUS_WARDRIVE_GPS_FRESH,.utc_valid=true},.expansion_present_mask=0x3f};
    wardrive_include_expansions=true;draw();
    CHECK(!strcmp(lines[80],"GPS DATA > FIX + UTC / WIGLE CSV"));
    CHECK(strstr(drawn,"R1-R6: ON / 6 ONLINE"));save(prefix,"gps-format-preview");
    runtime.gps.utc_valid=false;draw();CHECK(!strcmp(lines[80],"GPS DATA > FIX / UTC UNVERIFIED"));
    strcpy(runtime.last_file_name,"CERBWardrive001.CSV");runtime.files_saved=1;draw();
    CHECK(!strcmp(lines[280],"LAST FILE: CERBWardrive001.CSV"));
    runtime.skipped_no_fix=runtime.skipped_time=runtime.skipped_format=UINT32_MAX;draw();
    CHECK(!strcmp(lines[300],"NOT SAVED: 12884901885 / NO FIX 4294967295"));
    runtime.gps.state=CERBERUS_WARDRIVE_GPS_STALE;draw();CHECK(!strcmp(lines[80],"GPS DATA > FIX STALE"));
    runtime.gps_error=ESP_FAIL;draw();CHECK(!strcmp(lines[80],"GPS DATA > UART ERROR"));
    runtime.error_code=runtime.sd_error=ESP_ERR_INVALID_SIZE;draw();
    CHECK(!strcmp(lines[324],"LOG SIZE LIMIT / START A NEW SESSION"));
}
static void check_routes(void)
{
    reset();wardrive_mode=0;wardrive_handle_tap(80,130);CHECK(wardrive_mode==CERBERUS_WD_WIFI);
    wardrive_handle_tap(80,130);CHECK(wardrive_mode==CERBERUS_WD_BLE);
    wardrive_handle_tap(24,346);CHECK(!wardrive_waiting_for_gps&&!wardrive_include_expansions);
    wardrive_handle_tap(80,130);CHECK(wardrive_mode==CERBERUS_WD_COMBINED);
    wardrive_handle_tap(24,346);CHECK(wardrive_include_expansions&&!wardrive_waiting_for_gps);
    wardrive_handle_tap(164,346);CHECK(!wardrive_include_expansions);
    wardrive_handle_tap(164,346);CHECK(wardrive_include_expansions);
    unsigned old=status_reads;wardrive_handle_tap(7,390);wardrive_handle_tap(312,390);CHECK(status_reads==old);
    unsigned draws=page_draws;wardrive_handle_tap(200,178);CHECK(page_draws==draws&&!stop_requests);
    ready_gps();wardrive_start_error=ESP_FAIL;strcpy(wardrive_blocked_reason,"old failure");
    wardrive_handle_tap(80,380);CHECK(wardrive_start_pending&&!stop_requests&&!wardrive_start_error&&!wardrive_blocked_reason[0]);
    CHECK(wardrive_start_requested_ms==1234&&wardrive_last_start_attempt_ms==984&&!wardrive_last_draw_ms);
    wardrive_handle_tap(24,346);CHECK(wardrive_include_expansions); /* No configuration changes while pending. */
    wardrive_handle_tap(80,411);CHECK(!wardrive_start_pending&&!stop_requests);
    for(unsigned state=CERBERUS_WARDRIVE_STARTING;state<=CERBERUS_WARDRIVE_STOPPING;++state){
        runtime.state=(cerberus_wardrive_state_t)state;unsigned stops=stop_requests;
        wardrive_handle_tap(80,380);CHECK(stop_requests==stops+1&&!wardrive_start_pending);
        wardrive_handle_tap(164,346);CHECK(wardrive_include_expansions);
    }
    old=status_reads;draws=page_draws;
    wardrive_handle_tap(104,390);wardrive_handle_tap(211,390);
    CHECK(status_reads==old&&page_draws==draws&&!recent_opens);
    unsigned stops=stop_requests;
    wardrive_handle_tap(112,380);wardrive_handle_tap(207,411);
    CHECK(recent_opens==2&&status_reads==old&&stop_requests==stops);
    wardrive_handle_tap(216,380);wardrive_handle_tap(311,411);CHECK(logs_opened==2);
    /* A failed paint is returned, not reported as successful. */
    reset();fail_at=1;CHECK(draw_wardrive()==ESP_FAIL);
}
static void check_lifecycle(const char *prefix)
{
    /* A held full Recon token blocks admission even when both schedulers are idle. */
    reset();ready_gps();wardrive_handle_tap(80,380);live_ble_lease_epoch=UINT64_C(0x100000007);
    for(unsigned i=0;i<13;++i)wardrive_tick(1500+i*250,true);
    CHECK(wardrive_start_pending&&!service_starts&&!launcher_returns);
    live_ble_lease_epoch=0;wardrive_tick(5000,true);
    CHECK(service_starts==1&&!wardrive_start_pending&&requested_gps&&!requested_expansions);
    wardrive_tick(5250,true);CHECK(service_starts==1);
    /* A scheduler request and a discovery attachment remain separate barriers. */
    reset();ready_gps();wardrive_handle_tap(80,380);live_ble_session.pending.kind=SENTINEL_SCAN_COMMAND_STOP;
    wardrive_tick(1500,true);CHECK(!service_starts&&wardrive_start_pending);
    live_ble_session.pending.kind=SENTINEL_SCAN_COMMAND_NONE;
    ble_discovery_live=&discovery_store;discovery_store.session.epoch=UINT64_C(0x200000007);
    wardrive_tick(1750,true);CHECK(!service_starts&&wardrive_start_pending);
    discovery_store.session.epoch=0;wardrive_tick(2000,true);CHECK(service_starts==1);
    /* Back must wait through the old nine-second cleanup deadline and longer. */
    reset();runtime.state=CERBERUS_WARDRIVE_STOPPING;wardrive_start_pending=true;
    wardrive_back();CHECK(wardrive_return_pending&&!wardrive_start_pending&&stop_requests==1&&!launcher_returns);
    for(unsigned i=0;i<20;++i)wardrive_tick(2000+i*1000,true);
    CHECK(wardrive_return_pending&&!launcher_returns&&!service_starts);
    runtime.error_code=runtime.s3_ble_error=ESP_ERR_TIMEOUT;wardrive_tick(22000,false);
    CHECK(!launcher_returns&&wardrive_return_pending&&strstr(drawn,"STATE: STOPPING / SAVING"));
    CHECK(strstr(drawn,"S3: ESP_ERR_TIMEOUT")&&strstr(drawn,"STOP\n"));save(prefix,"held-release-error");
    runtime.state=CERBERUS_WARDRIVE_ERROR;wardrive_tick(23000,true);
    CHECK(launcher_returns==1&&!wardrive_return_pending&&!service_starts);
    wardrive_tick(24000,true);CHECK(launcher_returns==1);
    /* Successful release, cancellation before admission, and inactive Back. */
    reset();runtime.state=CERBERUS_WARDRIVE_RUNNING;wardrive_back();
    CHECK(wardrive_return_pending&&stop_requests==1);
    runtime.state=CERBERUS_WARDRIVE_STOPPED;wardrive_tick(1500,true);CHECK(launcher_returns==1);
    reset();ready_gps();wardrive_handle_tap(80,380);wardrive_back();wardrive_tick(2000,true);
    CHECK(!wardrive_start_pending&&!service_starts&&launcher_returns==1&&!stop_requests);
    /* Explicit admission faults never become a synthetic running state. */
    reset();ready_gps();wardrive_handle_tap(80,380);storage_owner=CERBERUS_STORAGE_OWNER_USB;
    wardrive_tick(1500,true);CHECK(!service_starts&&!wardrive_start_pending&&wardrive_start_error==ESP_ERR_INVALID_STATE);
    CHECK(!strcmp(wardrive_blocked_reason,"RETURN SD FROM USB DRIVE MODE"));
    reset();ready_gps();wardrive_handle_tap(80,380);live_ble_lease_epoch=UINT64_MAX;
    wardrive_tick(1500,true);wardrive_tick(16500,true);CHECK(!service_starts&&!wardrive_start_pending&&wardrive_start_error==ESP_ERR_TIMEOUT);
    reset();ready_gps();wardrive_include_expansions=true;wardrive_handle_tap(80,380);
    service_start_result=ESP_ERR_INVALID_STATE;wardrive_tick(1500,true);
    CHECK(service_starts==1&&wardrive_start_pending&&requested_gps&&requested_expansions);
    wardrive_tick(1600,true);CHECK(service_starts==1);
    service_start_result=ESP_OK;wardrive_tick(1750,true);CHECK(service_starts==2&&!wardrive_start_pending);
}
int main(int argc,char **argv)
{
    CHECK(argc==2);
    /* Wardriving owns preview even with the saved Settings preview disabled.
     * Sleeping the display must not interrupt acquisition; leaving releases it. */
    reset();gps_preview_poll(300,true);
    CHECK(gps_preview_attached&&gps_preview_starts==1&&!gps_preview_stops);
    gps_preview_poll(1000,false);CHECK(gps_preview_starts==1);
    current_page=UI_PAGE_SETTINGS;settings_mode=UI_SETTINGS_MODE_GPS;gps_preview_poll(1300,false);
    CHECK(!gps_preview_attached&&gps_preview_stops==1);
    gps_settings.enabled=1;gps_preview_poll(1600,false);CHECK(gps_preview_attached&&gps_preview_starts==2);
    gps_preview_poll(1900,true);CHECK(!gps_preview_attached&&gps_preview_stops==2);
    /* A failed attach is released before the next retry. */
    reset();gps_preview_result=ESP_ERR_NO_MEM;gps_preview_poll(300,false);
    CHECK(gps_preview_cleanup&&!gps_preview_attached&&gps_preview_starts==1);
    gps_preview_result=ESP_OK;gps_preview_poll(600,false);CHECK(!gps_preview_cleanup&&gps_preview_stops==1);
    gps_preview_poll(900,false);CHECK(gps_preview_attached&&gps_preview_starts==2);
    reset();wardrive_handle_tap(80,380);
    CHECK(wardrive_start_pending&&wardrive_waiting_for_gps&&!service_starts);
    for(unsigned t=1500;t<=240000;t+=1000)wardrive_tick(t,true);
    CHECK(wardrive_start_pending&&!service_starts&&!wardrive_start_error);
    draw();CHECK(!strcmp(lines[105],"STATE: WAITING FOR GPS")&&strstr(drawn,"STOP\n"));
    CHECK(!strstr(drawn,"GPS: ON")&&!strstr(drawn,"GPS: OFF"));save(argv[1],"waiting-gps");
    ready_gps();wardrive_tick(241000,true);
    CHECK(!wardrive_start_pending&&service_starts==1&&requested_gps);
    wardrive_tick(242000,true);CHECK(service_starts==1);
    reset();wardrive_handle_tap(80,380);wardrive_handle_tap(80,380);
    ready_gps();wardrive_tick(2000,true);CHECK(!wardrive_start_pending&&!service_starts);
    reset();wardrive_handle_tap(80,380);wardrive_back();
    ready_gps();wardrive_tick(2000,true);CHECK(!wardrive_start_pending&&!service_starts);
    reset();ready_gps();wardrive_handle_tap(80,380);service_start_result=ESP_ERR_NOT_FINISHED;
    wardrive_tick(1500,true);CHECK(wardrive_start_pending&&wardrive_waiting_for_gps&&service_starts==1);
    runtime.gps.utc_valid=false;wardrive_tick(90000,true);
    CHECK(wardrive_start_pending&&service_starts==1&&!wardrive_start_error);
    ready_gps();service_start_result=ESP_OK;wardrive_tick(91000,true);
    CHECK(!wardrive_start_pending&&service_starts==2);
    reset();wardrive_handle_tap(80,380);runtime.gps_error=ESP_FAIL;wardrive_tick(1500,true);
    CHECK(wardrive_start_pending&&!service_starts&&wardrive_start_error==ESP_FAIL);
    runtime.gps_error=ESP_OK;ready_gps();wardrive_tick(1750,true);
    CHECK(!wardrive_start_pending&&service_starts==1&&!wardrive_start_error&&!wardrive_blocked_reason[0]);
    reset();wardrive_handle_tap(80,380);runtime.gps_error=ESP_FAIL;wardrive_tick(1500,true);
    wardrive_handle_tap(80,380);CHECK(!wardrive_start_pending&&!wardrive_waiting_for_gps&&!wardrive_start_error);
    check_views(argv[1]);check_routes();check_lifecycle(argv[1]);
    printf("Wardriving production UI: %u checks; GPS wait/autostart/cancel, preview ownership, large radio counts, Stop/config routes and bounded rendering pass.\n",checks);
}
