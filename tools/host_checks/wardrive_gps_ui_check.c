#undef NDEBUG
#include "wardrive_service.h"
#include "wardrive_log_feedback.h"
#include "wardrive_gps_uart.h"
#include "ui_widgets.h"
#include "display.h"
#include "esp_check.h"
#include "s3_ble_host.h"
#include "gps_recorder_ui_stub.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "display_font.h"

static unsigned checks,calls,fail_at,page_draws,status_reads,uart_state_reads;
static unsigned service_starts,stop_requests,launcher_returns,recent_opens,recent_closes;
static unsigned trace_begins,trace_reads,trace_ends,allocations,frees,masked_reads;
static bool fail_alloc,held_recon,recent_opened;
static esp_err_t begin_error,read_error,end_error,state_error;
static uint64_t live_token,next_token;
static cerberus_wardrive_status_t runtime;
static cerberus_wardrive_gps_uart_state_t uart_state;
static cerberus_wardrive_gps_trace_snapshot_t producer_trace;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);abort(); } } while (0)
#define ESP_ERROR_CHECK(x) CHECK((x)==ESP_OK)
#define UI_HEADER_HEIGHT 48U
#define MALLOC_CAP_SPIRAM 1U
#define MALLOC_CAP_8BIT 2U
enum { UI_PAGE_BLE=1,UI_PAGE_IEEE802154=2,UI_PAGE_WARDRIVE=3,CERBERUS_STORAGE_OWNER_USB=2 };
static unsigned current_page,storage_owner;
static const char *TAG="gps_ui_host";
static uint16_t pixels[480][320];
static char drawn[16384],lines[480][128];
static bool wardrive_start_pending,wardrive_return_pending,wardrive_waiting_for_gps,wardrive_include_expansions;
static uint8_t wardrive_mode;
static esp_err_t gps_preview_error;
static void gps_preview_poll(uint32_t now,bool asleep){(void)now;(void)asleep;}
esp_err_t cerberus_wardrive_set_mode(uint8_t mode){wardrive_mode=mode;return ESP_OK;}

static esp_err_t wardrive_start_error;
static char wardrive_blocked_reason[64];
static uint32_t wardrive_start_requested_ms,wardrive_last_start_attempt_ms,wardrive_last_draw_ms;
static int64_t clock_us=1234000;
static esp_err_t draw_detail_page(void);
static void wardrive_recent_open(void){++recent_opens;recent_opened=true;}
static void wardrive_recent_close(void){++recent_closes;recent_opened=false;}
static bool wardrive_recent_is_open(void){return recent_opened;}
/* Log-manager lifetime and controls have their own production UI suite. */
static bool wardrive_logs_is_open(void){return false;}
static void wardrive_logs_tick(bool asleep){(void)asleep;}
static void wardrive_logs_open(void){CHECK(false);}
static void wardrive_areas_open(void){}
static void wardrive_areas_close(void){}
static bool wardrive_areas_is_open(void){return false;}
static void wardrive_areas_release_if_hidden(bool asleep){(void)asleep;}
static esp_err_t wardrive_areas_start_check(void){return ESP_OK;}
static bool wardrive_recent_active(cerberus_wardrive_state_t state)
{return state==CERBERUS_WARDRIVE_STARTING||state==CERBERUS_WARDRIVE_RUNNING||state==CERBERUS_WARDRIVE_STOPPING;}
static bool recon_sessions_idle_for_takeover(void){return !held_recon;}
static unsigned cerberus_storage_get_owner(void){return storage_owner;}
cerberus_s3_ble_owner_t cerberus_s3_ble_host_owner(void){return CERBERUS_S3_BLE_OWNER_NONE;}
static void return_to_launcher(void){++launcher_returns;current_page=0;}
esp_err_t cerberus_wardrive_start(bool expansions,bool gps)
{(void)expansions;(void)gps;++service_starts;return ESP_OK;}
esp_err_t cerberus_wardrive_start_check(bool enabled)
{return cerberus_wardrive_gps_start_ready(enabled,&uart_state)?ESP_OK:ESP_ERR_NOT_FINISHED;}
static int64_t esp_timer_get_time(void){return clock_us;}
static const char *esp_err_to_name(esp_err_t e)
{return e==ESP_ERR_TIMEOUT?"ESP_ERR_TIMEOUT":e==ESP_ERR_NO_MEM?"ESP_ERR_NO_MEM":e==ESP_ERR_NOT_FOUND?"ESP_ERR_NOT_FOUND":"ESP_FAIL";}
void cerberus_wardrive_get_status(cerberus_wardrive_status_t *out){++status_reads;*out=runtime;}
void cerberus_wardrive_get_recent_status(cerberus_wardrive_recent_status_t *out)
{++status_reads;memset(out,0,sizeof(*out));out->state=runtime.state;}
void cerberus_wardrive_request_stop(void){++stop_requests;}
static void *heap_caps_calloc(size_t n,size_t size,unsigned caps)
{CHECK(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));++allocations;return fail_alloc?NULL:calloc(n,size);}
static void heap_caps_free(void *p){if(p)++frees;free(p);}
esp_err_t cerberus_wardrive_gps_uart_get_state(cerberus_wardrive_gps_uart_state_t *out)
{++uart_state_reads;*out=uart_state;return state_error;}
esp_err_t cerberus_wardrive_gps_uart_trace_begin(uint64_t *token)
{
    ++trace_begins;*token=0;
    if(begin_error)return begin_error;
    if(!uart_state.reader_running||uart_state.starting||uart_state.stopping)return ESP_ERR_NOT_FINISHED;
    CHECK(!live_token);live_token=++next_token;*token=live_token;return ESP_OK;
}
esp_err_t cerberus_wardrive_gps_uart_trace_read(uint64_t token,bool mask,cerberus_wardrive_gps_trace_snapshot_t *out)
{
    ++trace_reads;memset(out,0,sizeof(*out));
    if(token!=live_token)return ESP_ERR_NOT_FOUND;
    if(read_error)return read_error;
    *out=producer_trace;out->observer_token=token;
    if(mask){++masked_reads;for(unsigned i=0;i<8;++i){memset(out->rows[i].payload,0,sizeof(out->rows[i].payload));out->rows[i].retained_length=0;out->rows[i].flags|=CERBERUS_WARDRIVE_GPS_TRACE_MASKED;}}
    return ESP_OK;
}
esp_err_t cerberus_wardrive_gps_uart_trace_end(uint64_t token)
{++trace_ends;if(end_error)return end_error;if(token!=live_token)return ESP_ERR_NOT_FOUND;live_token=0;return ESP_OK;}
/* Any accidental acquisition/cleanup from a viewing path is a hard failure. */
esp_err_t cerberus_wardrive_gps_uart_start(void){CHECK(false);return ESP_FAIL;}
esp_err_t cerberus_wardrive_gps_uart_stop(void){CHECK(false);return ESP_FAIL;}

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
    CHECK(strlen(drawn)+strlen(text)+2U<sizeof(drawn));strcat(drawn,text);strcat(drawn,"\n");
    snprintf(lines[y],sizeof(lines[y]),"%s",text);
    for(size_t i=0;text[i];++i){const uint8_t *font=font5x7((unsigned char)text[i]);
        for(unsigned cy=0;cy<7U;++cy)for(unsigned cx=0;cx<6U;++cx)
            for(unsigned sy=0;sy<scale;++sy)for(unsigned sx=0;sx<scale;++sx)
                pixels[y+cy*scale+sy][x+(i*6U+cx)*scale+sx]=cx<5U&&(font[cy]&(1U<<(4U-cx)))?fg:bg;
    }return ESP_OK;
}
#include "wardrive_gps_common.h"
#include "wardrive_gps_live_ui.h"
#include "wardrive_gps_routes.h"
static esp_err_t draw_detail_page(void)
{
    ++page_draws;drawn[0]='\0';memset(lines,0,sizeof(lines));
    if(wardrive_gps_view_is_open())return wardrive_gps_view_draw();
    ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND),TAG,"clear");
    ESP_RETURN_ON_ERROR(draw_header("WARDRIVING"),TAG,"header");return draw_wardrive();
}
static void reset(void)
{
    memset(&gps_log_runtime,0,sizeof(gps_log_runtime));gps_log_starts=gps_log_stops=gps_log_marks=0;gps_log_result=ESP_OK;
    wardrive_gps_log_mode=CERBERUS_GPX_TRACK;wardrive_gps_log_error=ESP_OK;
    end_error=ESP_OK;wardrive_gps_view_close();memset(&wardrive_gps_view,0,sizeof(wardrive_gps_view));
    memset(&runtime,0,sizeof(runtime));memset(&uart_state,0,sizeof(uart_state));memset(&producer_trace,0,sizeof(producer_trace));
    uart_state.gps.sentence_age_ms=uart_state.gps.fix_age_ms=uart_state.gps.gga_age_ms=uart_state.gps.utc_age_ms=UINT32_MAX;
    calls=fail_at=page_draws=status_reads=uart_state_reads=0;service_starts=stop_requests=launcher_returns=0;
    recent_opens=recent_closes=trace_begins=trace_reads=trace_ends=allocations=frees=masked_reads=0;
    fail_alloc=held_recon=recent_opened=false;begin_error=read_error=end_error=state_error=ESP_OK;live_token=0;next_token=UINT64_C(0x100000000);
    current_page=UI_PAGE_WARDRIVE;storage_owner=0;clock_us=1234000;
    wardrive_start_pending=wardrive_return_pending=wardrive_waiting_for_gps=wardrive_include_expansions=false;
    wardrive_start_error=ESP_OK;wardrive_blocked_reason[0]='\0';wardrive_last_draw_ms=999;
    wardrive_start_requested_ms=wardrive_last_start_attempt_ms=0;
}
static void save(const char *prefix,const char *name)
{
    char path[1024];snprintf(path,sizeof(path),"%s-%s.ppm",prefix,name);FILE *f=fopen(path,"wb");CHECK(f!=NULL);
    CHECK(fprintf(f,"P6\n320 480\n255\n")>0);
    for(unsigned y=0;y<480;++y)for(unsigned x=0;x<320;++x){uint16_t p=pixels[y][x];uint8_t rgb[]={
        (uint8_t)(((p>>11)&31)*255/31),(uint8_t)(((p>>5)&63)*255/63),(uint8_t)((p&31)*255/31)};
        if(fwrite(rgb,1,3,f)!=3)abort();}CHECK(fclose(f)==0);
}
static void run_reader(void)
{uart_state.config.rx_gpio=1;uart_state.active_baud=115200;uart_state.driver_active=uart_state.reader_running=true;uart_state.clock_valid=true;uart_state.receiver_epoch=UINT64_C(0x200000007);runtime.state=CERBERUS_WARDRIVE_RUNNING;runtime.gps_requested=true;producer_trace.capturing=true;producer_trace.receiver_epoch=uart_state.receiver_epoch;}
static void line_row(unsigned index,unsigned result,const char *text)
{
    CHECK(index<8);cerberus_wardrive_gps_trace_row_t *r=&producer_trace.rows[index];memset(r,0,sizeof(*r));
    r->sequence=UINT64_C(0x100000001)+index;r->receiver_epoch=producer_trace.receiver_epoch;r->received_ms=1000+index;
    r->result=(uint8_t)result;r->termination=CERBERUS_WARDRIVE_GPS_TERMINATION_CRLF;
    r->flags=CERBERUS_WARDRIVE_GPS_TRACE_RECEIPT_VALID|CERBERUS_WARDRIVE_GPS_TRACE_PAYLOAD_COMPLETE;
    r->total_length=(uint32_t)strlen(text);r->retained_length=(uint8_t)(r->total_length>127?127:r->total_length);
    memcpy(r->payload,text,r->retained_length);producer_trace.count=(uint8_t)(index+1);
}
static void entry_and_data(const char *prefix)
{
    reset();actual_open_wardrive();CHECK(!wardrive_waiting_for_gps&&!wardrive_gps_view_is_open());
    actual_tap(80,80);CHECK(wardrive_gps_view_is_open()&&wardrive_gps_view.hidden&&!allocations&&!trace_begins&&!service_starts);
    CHECK(strstr(drawn,"S3 GPS UART OFF")&&strstr(drawn,"NO RECENT NMEA")&&!strstr(drawn,"ABSENT"));save(prefix,"off-data");
    actual_tap(200,65);CHECK(wardrive_gps_view.nmea&&trace_begins==1&&!allocations&&!live_token);
    CHECK(strstr(drawn,"NMEA VIEW IDLE")&&strstr(drawn,"NO RECENT NMEA"));save(prefix,"off-nmea");
    uart_state.historical_error=ESP_FAIL;CHECK(draw_detail_page()==ESP_OK);
    CHECK(strstr(drawn,"LAST UART ERROR")&&!strstr(drawn,"WARDRIVING OWNS GPS"));save(prefix,"off-prior-error");
    uart_state.historical_error=ESP_OK;
    run_reader();unsigned begins=trace_begins;wardrive_tick(2000,false);CHECK(trace_begins==begins&&!live_token); /* No auto-attach. */
    uart_state.config.rx_gpio=2;uart_state.active_baud=115200;CHECK(draw_detail_page()==ESP_OK);
    CHECK(strstr(drawn,"RX GPIO2 / 115200 / RECEIVER ACTIVE")&&!strstr(drawn,"9600"));
    uart_state.detecting_baud=true;CHECK(draw_detail_page()==ESP_OK);CHECK(strstr(drawn,"115200 / DETECTING"));
    uart_state.detecting_baud=false;
    actual_tap(80,65);uart_state.gps=(cerberus_wardrive_gps_snapshot_t){.state=CERBERUS_WARDRIVE_GPS_FRESH,
        .sentence_age_ms=2,.fix_age_ms=10,.gga_age_ms=4900,.utc_age_ms=10,.coordinates_valid=true,.latitude=-90,.longitude=180,
        .altitude_valid=true,.altitude_m=-123.4f,.satellites_valid=true,.satellites=255,.hdop_valid=true,.hdop=999.99f,
        .utc_valid=true,.utc_from_rmc=true,.valid_sentences=UINT32_MAX,.bad_checksums=UINT32_MAX,.malformed_sentences=UINT32_MAX,.overflow_lines=UINT32_MAX};
    strcpy(uart_state.gps.utc,"2026-11-01T00:00:00.000Z");CHECK(draw_detail_page()==ESP_OK);
    CHECK(!strstr(drawn,"-90.000000")&&!strstr(drawn,"180.000000")&&!strstr(drawn,"-123.4"));save(prefix,"fresh-hidden");
    actual_tap(160,398);CHECK(!wardrive_gps_view.hidden&&strstr(drawn,"-90.000000")&&strstr(drawn,"180.000000"));
    CHECK(strstr(drawn,"GGA FIELD AGE: 4.9s")&&strstr(drawn,"HDOP 999.99 (NOT METERS)"));save(prefix,"fresh-shown");
    uart_state.gps.satellites_valid=uart_state.gps.hdop_valid=false;uart_state.gps.gga_age_ms=5001;CHECK(draw_detail_page()==ESP_OK);
    CHECK(strstr(drawn,"SATS UNKNOWN / HDOP UNKNOWN")&&strstr(drawn,"FIX: FRESH"));
    uart_state.gps.state=CERBERUS_WARDRIVE_GPS_STALE;uart_state.gps.coordinates_valid=uart_state.gps.altitude_valid=uart_state.gps.utc_valid=false;
    CHECK(draw_detail_page()==ESP_OK);CHECK(strstr(drawn,"FIX: STALE")&&strstr(drawn,"LATITUDE: UNAVAILABLE"));save(prefix,"stale-data");
    actual_tap(80,450);CHECK(!wardrive_gps_view_is_open()&&!stop_requests&&!service_starts);
    actual_tap(80,80);CHECK(wardrive_gps_view.hidden);actual_tap(80,450);
}
static void trace_and_privacy(const char *prefix)
{
    reset();run_reader();for(unsigned i=0;i<8;++i)line_row(i,i,"$GPRMC,PRIVATE,5130.000,N,00006.000,W*00");
    actual_tap(80,80);actual_tap(200,65);CHECK(live_token&&allocations==1&&masked_reads==1&&trace_begins==1);
    CHECK(wardrive_gps_view.pages==4&&!strstr(drawn,"PRIVATE")&&strstr(drawn,"RAW LINE HIDDEN"));save(prefix,"nmea-hidden");
    for(unsigned i=0;i<8;++i){CHECK(!wardrive_gps_view.trace->rows[i].retained_length);for(unsigned j=0;j<127;++j)CHECK(!wardrive_gps_view.trace->rows[i].payload[j]);}
    actual_tap(160,398);CHECK(strstr(drawn,"PRIVATE")&&strstr(drawn,"OTHER / CHECKSUM OK"));save(prefix,"nmea-shown");
    unsigned reads=trace_reads;uint64_t copied_ms=wardrive_gps_view.copy_ms;
    memset(producer_trace.rows[2].payload,'Z',producer_trace.rows[2].retained_length);clock_us=9234000;
    actual_tap(260,398);CHECK(wardrive_gps_view.page==1&&wardrive_gps_view.paused&&trace_reads==reads);
    CHECK(strstr(drawn,"INVALID FIELDS")&&strstr(drawn,"PRIVATE")&&!strstr(drawn,"ZZZZ"));
    CHECK(wardrive_gps_view.copy_ms==copied_ms&&strstr(drawn,"AGES AT COPY"));save(prefix,"nmea-paused");
    actual_tap(260,398);CHECK(wardrive_gps_view.page==2&&strstr(drawn,"BAD CHECKSUM")&&strstr(drawn,"INVALID FRAME"));save(prefix,"nmea-invalid");
    actual_tap(260,398);CHECK(wardrive_gps_view.page==3&&strstr(drawn,"OVERFLOW / PREFIX")&&strstr(drawn,"CONTROL BYTE"));
    producer_trace.rows[6].total_length=UINT32_MAX;producer_trace.rows[6].retained_length=127;
    memset(producer_trace.rows[6].payload,'X',127);producer_trace.rows[7].payload[0]=1;producer_trace.rows[7].payload[1]=0xff;
    producer_trace.rows[6].flags&=(uint8_t)~CERBERUS_WARDRIVE_GPS_TRACE_PAYLOAD_COMPLETE;
    producer_trace.rows[7].flags=0;actual_tap(160,156);CHECK(!wardrive_gps_view.paused&&trace_reads==reads+1);
    CHECK(strstr(drawn,"127/4294967295B")&&strstr(drawn,"RX UNKNOWN")&&strstr(drawn,"RETAINED PREFIX"));save(prefix,"nmea-overflow");
    /* Monotonic-clock invalidation wins even while now remains above old receipt. */
    clock_us=2000000;uart_state.clock_valid=false;CHECK(draw_detail_page()==ESP_OK);CHECK(strstr(drawn,"RX UNKNOWN"));
    uart_state.clock_valid=true;
    clock_us=-1;CHECK(draw_detail_page()==ESP_OK);CHECK(strstr(drawn,"RX UNKNOWN"));clock_us=1;CHECK(draw_detail_page()==ESP_OK);CHECK(strstr(drawn,"RX UNKNOWN"));
    producer_trace.evicted_lines=producer_trace.discarded_lines=producer_trace.overflow_lines=UINT32_MAX;
    producer_trace.sequence_exhausted=true;producer_trace.capturing=false;CHECK(draw_detail_page()==ESP_OK);
    CHECK(strstr(drawn,"EVICT 4294967295 / DROP 4294967295 / OVF 4294967295")&&strstr(drawn,"SEQUENCE LIMIT"));save(prefix,"nmea-limit");
    actual_tap(160,156);CHECK(wardrive_gps_view.paused);reads=trace_reads;
    actual_tap(160,398);CHECK(wardrive_gps_view.hidden&&!strstr(drawn,"XXXX")&&!strstr(drawn,"PRIVATE")&&trace_reads==reads);
    for(unsigned i=0;i<8;++i){CHECK(!wardrive_gps_view.trace->rows[i].retained_length);for(unsigned j=0;j<127;++j)CHECK(!wardrive_gps_view.trace->rows[i].payload[j]);}
    actual_tap(160,398);CHECK(!wardrive_gps_view.hidden&&strstr(drawn,"RAW HIDDEN IN COPY")&&!strstr(drawn,"XXXX")&&trace_reads==reads);
    actual_tap(160,156);CHECK(!wardrive_gps_view.paused&&strstr(drawn,"XXXX")&&trace_reads==reads+1);
    /* A failed display never grants a new page/privacy operation. Back still works. */
    fail_at=calls+1;CHECK(draw_detail_page()==ESP_FAIL&&!wardrive_gps_view.paint_valid);fail_at=0;
    bool hidden=wardrive_gps_view.hidden;actual_tap(160,398);CHECK(wardrive_gps_view.hidden==hidden);
    actual_tap(80,450);CHECK(!live_token&&frees==1&&!stop_requests&&!service_starts);
}
static void failures_and_lifecycle(const char *prefix)
{
    reset();run_reader();actual_tap(80,80);fail_alloc=true;actual_tap(200,65);
    CHECK(!live_token&&!wardrive_gps_view.trace&&trace_ends==1&&wardrive_gps_view.error==ESP_ERR_NO_MEM);
    CHECK(strstr(drawn,"ESP_ERR_NO_MEM")&&!stop_requests);save(prefix,"nmea-oom");
    fail_alloc=false;actual_tap(200,65);CHECK(live_token&&wardrive_gps_view.trace);
    unsigned began=trace_begins,ended=trace_ends;uint64_t token=live_token;
    actual_tap(200,65);CHECK(trace_begins==began&&live_token==token);
    read_error=ESP_ERR_TIMEOUT;CHECK(draw_detail_page()==ESP_OK);CHECK(strstr(drawn,"NO RECENT NMEA")&&!wardrive_gps_view.trace->count);
    actual_tap(200,65);CHECK(live_token==token&&trace_begins==began&&trace_ends==ended);
    read_error=ESP_ERR_NOT_FOUND;CHECK(draw_detail_page()==ESP_OK);live_token=0;read_error=ESP_OK;
    actual_tap(200,65);CHECK(live_token!=token&&trace_begins==began+1&&trace_ends==ended+1);
    began=trace_begins;ended=trace_ends;
    line_row(0,CERBERUS_WARDRIVE_GPS_LINE_RMC,"$GPRMC,FROZEN*00");
    CHECK(draw_detail_page()==ESP_OK);CHECK(wardrive_gps_view.trace->count==1);
    producer_trace.rows[0].payload[0]='!';CHECK(wardrive_gps_view.trace->rows[0].payload[0]==0); /* Masked value copy. */
    actual_tap(80,65);CHECK(!live_token&&!wardrive_gps_view.trace&&trace_ends==ended+1);
    actual_tap(200,65);CHECK(live_token);ended=trace_ends;wardrive_tick(2000,true);
    CHECK(!wardrive_gps_view.opened&&!live_token&&trace_ends==ended+1&&!stop_requests&&runtime.state==CERBERUS_WARDRIVE_RUNNING);
    wardrive_tick(2600,false);CHECK(!wardrive_gps_view.opened&&trace_begins==began+1); /* Wake does not attach. */
    actual_tap(80,80);actual_tap(200,65);CHECK(live_token);current_page=UI_PAGE_BLE;wardrive_tick(3000,false);
    CHECK(!live_token&&!wardrive_gps_view.opened&&!service_starts&&!stop_requests);
    current_page=UI_PAGE_WARDRIVE;actual_tap(80,80);actual_tap(200,65);CHECK(live_token);
    end_error=ESP_ERR_TIMEOUT;actual_tap(80,65);CHECK(live_token&&wardrive_gps_view.detach_pending&&!wardrive_gps_view.nmea);
    ended=trace_ends;wardrive_tick(1235,false);CHECK(trace_ends==ended); /* Paced retained detach on DATA. */
    end_error=ESP_OK;wardrive_tick(1600,false);CHECK(!live_token&&!wardrive_gps_view.trace&&wardrive_gps_view.opened);
    actual_tap(200,65);CHECK(live_token);
    end_error=ESP_ERR_TIMEOUT;actual_tap(80,450);CHECK(live_token&&!wardrive_gps_view.opened&&wardrive_gps_view.token);
    end_error=ESP_OK;wardrive_tick(3500,false);CHECK(!live_token&&!wardrive_gps_view.trace); /* Exact cleanup retained, no UART Stop. */
    actual_tap(80,80);actual_tap(200,65);runtime.state=CERBERUS_WARDRIVE_STOPPING;uart_state.reader_running=false;uart_state.stopping=true;
    uart_state.error=ESP_ERR_TIMEOUT;CHECK(draw_detail_page()==ESP_OK);CHECK(strstr(drawn,"S3 GPS UART STOPPING")&&strstr(drawn,"STOPPING"));save(prefix,"gps-stopping");
    actual_tap(230,450);CHECK(stop_requests==1&&wardrive_gps_view.opened);actual_tap(80,450);CHECK(!live_token&&!wardrive_gps_view.opened);
    actual_summary_back();CHECK(wardrive_return_pending&&stop_requests==2&&!launcher_returns);
    for(unsigned i=0;i<20;++i)wardrive_tick(4000+i*1000,true);
    CHECK(wardrive_return_pending&&!launcher_returns&&!service_starts);
    runtime.state=CERBERUS_WARDRIVE_STOPPED;wardrive_tick(25000,true);CHECK(launcher_returns==1&&!wardrive_return_pending);
}
static void gpx_controls(const char *prefix)
{
    reset();run_reader();actual_tap(80,80);actual_tap(200,65);
    CHECK(live_token);actual_tap(260,65);
    CHECK(wardrive_gps_view.logging&&!wardrive_gps_view.nmea&&!live_token&&!gps_log_starts);
    CHECK(strstr(drawn,"START GPX")&&strstr(drawn,"MODE: TRACK"));save(prefix,"gpx-ready");
    actual_tap(160,398);CHECK(gps_log_starts==1&&gps_log_runtime.busy&&!service_starts);
    CHECK(strstr(drawn,"WAITING FOR GPS FIX + UTC")&&strstr(drawn,"STOP GPX"));save(prefix,"gpx-waiting");
    actual_tap(160,150);CHECK(wardrive_gps_log_mode==CERBERUS_GPX_TRACK); /* Busy mode is fixed. */
    actual_tap(160,398);CHECK(gps_log_stops==1&&gps_log_runtime.busy&&strstr(drawn,"SAVING"));
    gps_log_runtime.busy=false;gps_log_runtime.state=CERBERUS_GPS_LOG_STOPPED;
    CHECK(draw_detail_page()==ESP_OK);actual_tap(160,150);
    CHECK(wardrive_gps_log_mode==CERBERUS_GPX_POINTS&&strstr(drawn,"MODE: POINTS OF INTEREST"));
    actual_tap(160,398);gps_log_runtime.fix_ready=true;gps_log_runtime.state=CERBERUS_GPS_LOG_RECORDING;
    CHECK(draw_detail_page()==ESP_OK);actual_tap(160,278);
    CHECK(gps_log_marks==1&&gps_log_runtime.queued==1&&!service_starts&&!stop_requests);save(prefix,"gpx-poi");
    actual_tap(80,450);CHECK(!wardrive_gps_view.opened&&gps_log_runtime.busy&&strstr(drawn,"GPX ACTIVE"));
    actual_summary_back();CHECK(wardrive_return_pending&&gps_log_stops==2&&!launcher_returns);
    wardrive_tick(6000,true);CHECK(!launcher_returns&&wardrive_return_pending);
    gps_log_runtime.busy=false;gps_log_runtime.state=CERBERUS_GPS_LOG_STOPPED;
    runtime.state=CERBERUS_WARDRIVE_STOPPED;
    wardrive_tick(7000,true);CHECK(launcher_returns==1&&!wardrive_return_pending);
    reset();run_reader();actual_tap(80,80);actual_tap(260,65);
    gps_log_result=ESP_ERR_NO_MEM;actual_tap(160,398);
    CHECK(!gps_log_runtime.busy&&strstr(drawn,"ESP_ERR_NO_MEM"));save(prefix,"gpx-error");
    gps_log_result=ESP_OK;actual_tap(160,398);gps_log_runtime.state=CERBERUS_GPS_LOG_RECORDING;
    gps_log_runtime.fix_ready=true;gps_log_runtime.written=1234;gps_log_runtime.track.distance_m=12345;
    gps_log_runtime.track.speed_mps=4.5;gps_log_runtime.track.segments=2;
    CHECK(draw_detail_page()==ESP_OK);save(prefix,"gpx-track");
    gps_log_runtime.busy=false;gps_log_runtime.state=CERBERUS_GPS_LOG_STOPPED;
    strcpy(gps_log_runtime.file_name,"GPS-T00001.GPX");CHECK(draw_detail_page()==ESP_OK);
    CHECK(strstr(drawn,"GPS-T00001.GPX")&&strstr(drawn,"START GPX"));save(prefix,"gpx-saved");
    gps_log_runtime.written=UINT32_MAX;gps_log_runtime.rejected=gps_log_runtime.discarded=UINT32_MAX;
    gps_log_runtime.track.segments=UINT32_MAX;CHECK(draw_detail_page()==ESP_OK);save(prefix,"gpx-large-counts");
    gps_log_runtime.busy=true;current_page=UI_PAGE_BLE;
    unsigned stopped=gps_log_stops;wardrive_tick(9000,false);
    CHECK(gps_log_stops==stopped+1&&gps_log_runtime.busy&&gps_log_runtime.state==CERBERUS_GPS_LOG_STOPPING);
    gps_log_runtime.busy=false;
}
int main(int argc,char **argv)
{
    CHECK(argc==2);entry_and_data(argv[1]);trace_and_privacy(argv[1]);failures_and_lifecycle(argv[1]);gpx_controls(argv[1]);
    printf("Wardriving GPS production UI: %u checks; DATA/NMEA no acquisition, masked raw copies, independent ages, PSRAM/OFF failures, tabs/paging, sleep/Back observer cleanup and retained Wardrive STOPPING pass. UI state %zu bytes, optional trace copy %zu bytes.\n",checks,sizeof(wardrive_gps_view_t),sizeof(cerberus_wardrive_gps_trace_snapshot_t));
}
