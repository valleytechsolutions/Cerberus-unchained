#include "ap_survey_foreign_boundary.h"
#include "wardrive_foreign_boundary.h"
#undef NDEBUG
#include "cerberus_protocol.h"
#include "wifi_channel_survey_metrics.h"
#include "esp_err.h"
#include "display.h"
#include "ui_widgets.h"
#include <assert.h>
#include <setjmp.h>
#include <stdatomic.h>
static atomic_bool packet_count_radio_reserved;
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { ++checks; assert(x); } while (0)
#define portMAX_DELAY 0
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
#define WIFI_PROMIS_FILTER_MASK_MGMT 1U
#define WIFI_PROMIS_FILTER_MASK_CTRL 2U
#define WIFI_PROMIS_FILTER_MASK_DATA 4U
#define WIFI_BAND_2G 1
#define WIFI_BAND_5G 2
#define WIFI_SECOND_CHAN_NONE 0
#define WIFI_PKT_MISC 3
typedef unsigned wifi_promiscuous_pkt_type_t;
typedef struct { unsigned filter_mask; } wifi_promiscuous_filter_t;
typedef struct { struct { unsigned rx_state,sig_len,channel; int8_t rssi; } rx_ctrl; } wifi_promiscuous_pkt_t;
static int mutex,task,mux;
static void *scanner_mutex=&mutex,*channel_survey_task_handle=&task;
static bool locked,critical,scanner_ready=true,station_reserved;
static cerberus_wifi_scan_status_payload_t scan_status;
static cerberus_wifi_connection_status_payload_t connection_status;
static cerberus_wifi_defense_status_payload_t defense_status;
static cerberus_ble_scan_status_payload_t ble_status;
static cerberus_ble_connection_status_payload_t ble_connection;
/* Separate passive Probe owner; the actual admission body below checks it. */
static atomic_bool probe_sniff_radio_reserved;
static atomic_bool mac_radio_reserved,csi_radio_reserved,remote_id_radio_reserved,
    capture_radio_reserved,packet_monitor_radio_reserved;
/* Same production storage types, with no duplicate result bank. */
static cerberus_wifi_channel_survey_status_payload_t channel_survey_status;
static cerberus_wifi_channel_survey_result_payload_t channel_survey_results[CERBERUS_WIFI_CHANNEL_SURVEY_MAX_RESULTS];
static uint8_t channel_survey_channels[CERBERUS_WIFI_CHANNEL_SURVEY_MAX_RESULTS];
static int64_t channel_survey_started_us,clock_us;
static atomic_bool channel_survey_cancelled,channel_survey_radio_reserved;
#define channel_survey_mux mux
static bool channel_survey_receiving;
static uint8_t channel_survey_receive_channel;
static uint32_t channel_survey_packet_count;
static int64_t channel_survey_rssi_sum;
static int8_t channel_survey_strongest_rssi;
static unsigned notifications,enable_calls,disable_calls,channel_calls,delay_calls,worker_notifications;
static unsigned cancel_after_delay;
static int filter_error,enable_error,disable_error,channel_error;
static bool sdk_promiscuous,worker_running,run_worker_during_wait,run_worker_on_notify,hold_worker;
static jmp_buf worker_exit;
static void run_worker(void);
static void (*worker_delay_hook)(void);
static uint32_t esp_random(void){return 0x12345678U;}
static void cerberus_wifi_scanner_get_status(cerberus_wifi_scan_status_payload_t *out);
static void xSemaphoreTake(void *m,unsigned t){(void)t;CHECK(m==scanner_mutex&&!locked);locked=true;}
static void xSemaphoreGive(void *m){CHECK(m==scanner_mutex&&locked);locked=false;}
#define portENTER_CRITICAL(m) do { CHECK((m)==&mux&&!critical);critical=true; } while(0)
#define portEXIT_CRITICAL(m) do { CHECK((m)==&mux&&critical);critical=false; } while(0)
static int64_t esp_timer_get_time(void){return clock_us;}
static bool cerberus_c5_test_running(void){return false;}
static bool cerberus_ieee802154_scanner_is_running(void){return false;}
static bool cerberus_wifi_defense_survey_is_running(void){return defense_status.state==CERBERUS_WIFI_DEFENSE_RUNNING;}
static bool cerberus_wifi_station_running(void){return station_reserved;}
static void cerberus_ble_scanner_get_status(cerberus_ble_scan_status_payload_t *out){*out=ble_status;}
static void cerberus_ble_connection_get_status(cerberus_ble_connection_status_payload_t *out){*out=ble_connection;}
static void cerberus_wifi_connection_get_status(cerberus_wifi_connection_status_payload_t *out){*out=connection_status;}
static void xTaskNotifyGive(void *t){CHECK(t==channel_survey_task_handle);++notifications;if(run_worker_on_notify)run_worker();}
static void ulTaskNotifyTake(int clear,unsigned timeout)
{(void)timeout;CHECK(clear==pdTRUE);if(worker_notifications++)longjmp(worker_exit,1);}
static int esp_wifi_set_promiscuous_filter(const wifi_promiscuous_filter_t *f)
{CHECK(f->filter_mask==7U);return filter_error;}
static int esp_wifi_set_promiscuous(bool on)
{CHECK(!locked);if(on){++enable_calls;if(!enable_error)sdk_promiscuous=true;return enable_error;}
 ++disable_calls;if(!disable_error)sdk_promiscuous=false;return disable_error;}
static int esp_wifi_set_band(int band){CHECK(band==1||band==2);return channel_error;}
static int esp_wifi_set_channel(uint8_t channel,int second)
{CHECK(channel&&second==0);++channel_calls;return channel_error;}
static void vTaskDelay(unsigned ms)
{
    CHECK(!locked);clock_us+=(int64_t)ms*1000;
    if(worker_running){
        ++delay_calls;
        if(worker_delay_hook)worker_delay_hook();
        cerberus_wifi_scan_status_payload_t projected;cerberus_wifi_scanner_get_status(&projected);
        CHECK(projected.state==CERBERUS_WIFI_SCAN_RUNNING);
        if(cancel_after_delay&&delay_calls>=cancel_after_delay)
            atomic_store(&channel_survey_cancelled,true);
    }else if(run_worker_during_wait&&!hold_worker)run_worker();
}
#include "survey_admission.h"
#include "wifi_channel_survey_service.h"
static void run_worker(void)
{
    CHECK(!worker_running);worker_running=true;worker_notifications=0;
    if(!setjmp(worker_exit))channel_survey_task(NULL);
    worker_running=false;CHECK(!locked&&!critical);
}

/* Actual main dispatch and S3 transport validators, with only frame exchange
 * and link mutex boundaries replaced. A lost reply can occur after mutation. */
static uint8_t response_bytes[22],response_command,response_sequence;
static size_t response_size;
static void prepare_frame_response(cerberus_spi_command_t command,uint8_t sequence,const void *data,size_t size)
{CHECK(size<=sizeof(response_bytes));memcpy(response_bytes,data,size);response_size=size;response_command=command;response_sequence=sequence;}
typedef struct { uint8_t command,sequence,payload_length,payload[22]; } test_request_t;
static void dispatch(const test_request_t *request)
{switch(request->command){
#include "survey_dispatch.h"
default:CHECK(false);}}
static int link_mutex_object;
static void *link_mutex=&link_mutex_object;
static bool link_locked;
static uint8_t drop_after_command,fail_before_command;
static unsigned starts,stops,status_queries,reads;
static int take_link_mutex(unsigned timeout){CHECK((timeout==250||timeout==20)&&!link_locked);link_locked=true;return pdTRUE;}
static void give_link_mutex(void){CHECK(link_locked);link_locked=false;}
static esp_err_t exchange_v2_locked(cerberus_spi_command_t command,const void *request,size_t length,void *out,size_t size)
{
    CHECK(link_locked&&length<=22);if(fail_before_command==command){fail_before_command=0;return ESP_ERR_TIMEOUT;}
    if(command==CERBERUS_SPI_COMMAND_WIFI_CHANNEL_SURVEY_START)++starts;
    if(command==CERBERUS_SPI_COMMAND_WIFI_CHANNEL_SURVEY_STOP)++stops;
    if(command==CERBERUS_SPI_COMMAND_WIFI_CHANNEL_SURVEY_STATUS)++status_queries;
    if(command==CERBERUS_SPI_COMMAND_WIFI_CHANNEL_SURVEY_RESULT)++reads;
    test_request_t q={.command=command,.sequence=17,.payload_length=(uint8_t)length};
    if(length)memcpy(q.payload,request,length);response_size=0;dispatch(&q);
    if(drop_after_command==command){drop_after_command=0;return ESP_ERR_TIMEOUT;}
    CHECK(response_command==command&&response_sequence==17&&response_size==size);
    memcpy(out,response_bytes,size);return ESP_OK;
}
typedef struct { uint32_t firmware_version; } cerberus_c5_status_t;
static cerberus_c5_status_t c5_status={.firmware_version=71};
static esp_err_t c5_spi_link_get_status(cerberus_c5_status_t *out){*out=c5_status;return ESP_OK;}
static uint8_t sequence;
static uint32_t peer_firmware_version;
static unsigned device_queries,scoped_starts,scoped_stops,scoped_statuses,scoped_reads;
static uint8_t reboot_before,reboot_after,corrupt_command,corrupt_part,corrupt_kind;
static uint8_t fail_read_part;
static bool supersede_read_pin;
static unsigned wire_calls;
static int64_t wire_cost_us;
static void reset_c5_epoch(uint64_t epoch)
{
    channel_survey_session_init_locked();
    CHECK(channel_survey_session_epoch != 0U);
    memset(&channel_survey_status,0,sizeof(channel_survey_status));
    channel_survey_status.dwell_ms=100;
    memset(channel_survey_results,0,sizeof(channel_survey_results));
    channel_survey_session_epoch=epoch;channel_survey_session_id=0;
    channel_survey_lease_deadline_us=channel_survey_lease_clock_us=0;
    channel_survey_control_busy=false;channel_survey_pin_counter=channel_survey_pin=0;
    channel_survey_pin_session=0;channel_survey_pin_kind=channel_survey_pin_index=0;
    memset(channel_survey_pin_bytes,0,sizeof(channel_survey_pin_bytes));
    atomic_store(&channel_survey_cancelled,false);atomic_store(&channel_survey_radio_reserved,false);
    sdk_promiscuous=false;
}
static esp_err_t get_status_v2_passive_locked(uint8_t token,uint32_t deadline,cerberus_c5_status_t *out)
{(void)token;(void)deadline;CHECK(link_locked);++device_queries;++wire_calls;*out=c5_status;clock_us+=wire_cost_us;return ESP_OK;}
static esp_err_t spi_request_response_passive(const cerberus_spi_frame_t *request,uint8_t *discarded,
    cerberus_spi_frame_t *response,size_t size,uint32_t deadline)
{
    (void)discarded;(void)deadline;CHECK(link_locked&&size==sizeof(*request));++wire_calls;
    uint8_t cmd=request->command;clock_us+=wire_cost_us;
    if(fail_before_command==cmd){fail_before_command=0;return ESP_ERR_TIMEOUT;}
    if(cmd==CERBERUS_SPI_COMMAND_WIFI_CHANNEL_SURVEY_SESSION_READ){
        cerberus_wifi_channel_survey_session_read_t query;memcpy(&query,request->payload,sizeof(query));
        if(query.part==fail_read_part){fail_read_part=255;return ESP_ERR_TIMEOUT;}
    }
    if(reboot_before==cmd){reboot_before=0;reset_c5_epoch(1000);}
    if(cmd==CERBERUS_SPI_COMMAND_WIFI_CHANNEL_SURVEY_SESSION_START)++scoped_starts;
    if(cmd==CERBERUS_SPI_COMMAND_WIFI_CHANNEL_SURVEY_SESSION_STOP)++scoped_stops;
    if(cmd==CERBERUS_SPI_COMMAND_WIFI_CHANNEL_SURVEY_SESSION_STATUS)++scoped_statuses;
    if(cmd==CERBERUS_SPI_COMMAND_WIFI_CHANNEL_SURVEY_SESSION_READ)++scoped_reads;
    test_request_t q={.command=cmd,.sequence=request->sequence,.payload_length=request->payload_length};
    memcpy(q.payload,request->payload,q.payload_length);response_size=0;dispatch(&q);
    CHECK(response_size==22&&response_command==cmd&&response_sequence==request->sequence);
    if(cmd==CERBERUS_SPI_COMMAND_WIFI_CHANNEL_SURVEY_SESSION_READ&&supersede_read_pin){
        supersede_read_pin=false;
        cerberus_wifi_channel_survey_session_read_t other={.session_id=channel_survey_session_id};
        cerberus_wifi_channel_survey_session_chunk_t ignored;
        CHECK(cerberus_wifi_channel_survey_session_read(&other,&ignored)==ESP_OK);
    }
    if(reboot_after==cmd){reboot_after=0;reset_c5_epoch(2000);}
    if(drop_after_command==cmd){drop_after_command=0;return ESP_ERR_TIMEOUT;}
    bool corrupt=corrupt_command==cmd;
    if(cmd==CERBERUS_SPI_COMMAND_WIFI_CHANNEL_SURVEY_SESSION_READ){
        cerberus_wifi_channel_survey_session_read_t query;memcpy(&query,q.payload,sizeof(query));
        corrupt=corrupt&&query.part==corrupt_part;
    }
    if(corrupt){
        corrupt_command=0;
        if(corrupt_kind==1)response_bytes[0]^=1; /* session echo */
        else if(corrupt_kind==2)response_bytes[8]^=1; /* pin */
        else if(corrupt_kind==3)response_bytes[12]^=1; /* part */
        else if(corrupt_kind==4)response_bytes[21]=1; /* terminal zero tail */
        else if(corrupt_kind==5)response_size=21;
        else if(corrupt_kind==6)response_command^=1;
        else if(corrupt_kind==7)response_sequence^=1;
    }
    CHECK(cerberus_protocol_prepare_frame(response,response_command,response_sequence,response_bytes,response_size));
    return ESP_OK;
}
#include "survey_link.h"
enum { UI_WIFI_MODE_MENU,UI_WIFI_MODE_CHANNELS_24,UI_WIFI_MODE_CHANNELS_5 };
enum { UI_PAGE_WIFI,UI_PAGE_OTHER,UI_PAGE_BLE,UI_PAGE_IEEE802154 };
static unsigned wifi_mode,current_page,draws;
static bool screen_asleep;
#define ESP_ERROR_CHECK(x) CHECK((x)==ESP_OK)
static esp_err_t draw_detail_page(void){++draws;return ESP_OK;}
#include "survey_ui.h"
_Static_assert(UI_WIFI_VISIBLE_ROWS == 6, "fixture must exercise production six-row survey pages");
_Static_assert(UI_WIFI_SURVEY_METRICS_MS == 250U && UI_WIFI_SURVEY_RETRY_MS == 700U,
    "fixture must exercise production healthy and retry cadence");
#define UI_HEADER_HEIGHT 48U
#define ESP_RETURN_ON_ERROR(x,tag,message) do { esp_err_t e=(x);if(e!=ESP_OK)return e; } while(0)
static char drawn[4096];
static uint16_t pixels[480][320];
#include "display_font.h"
static void text(uint16_t x,uint16_t y,const char *s)
{CHECK(x<320&&y<480&&strlen(drawn)+strlen(s)+2<sizeof(drawn));strcat(drawn,s);strcat(drawn,"\n");}
static const char *esp_err_to_name(esp_err_t e){return e==ESP_FAIL?"ESP_FAIL":e==ESP_ERR_TIMEOUT?"ESP_ERR_TIMEOUT":"ESP_ERR_INVALID_STATE";}
esp_err_t cerberus_display_fill_rect(uint16_t x,uint16_t y,uint16_t w,uint16_t h,uint16_t c)
{CHECK(x+w<=320&&y+h<=480);for(unsigned iy=y;iy<y+h;++iy)for(unsigned ix=x;ix<x+w;++ix)pixels[iy][ix]=c;return ESP_OK;}
esp_err_t cerberus_display_draw_text(uint16_t x,uint16_t y,const char *s,uint16_t fg,uint16_t bg,uint8_t scale)
{
    CHECK((scale==1||scale==2)&&x+strlen(s)*6*scale<=320&&y+7*scale<=480);text(x,y,s);
    for(size_t i=0;s[i];++i){const uint8_t *font=font5x7((unsigned char)s[i]);
        for(unsigned cy=0;cy<7;++cy)for(unsigned cx=0;cx<6;++cx)
            for(unsigned sy=0;sy<scale;++sy)for(unsigned sx=0;sx<scale;++sx)
                pixels[y+cy*scale+sy][x+(i*6+cx)*scale+sx]=cx<5&&(font[cy]&(1U<<(4-cx)))?fg:bg;
    }
    return ESP_OK;
}
#include "survey_ui_layout.h"
#include "survey_ui_draw.h"
static void draw_expect(const char *state,const char *button)
{drawn[0]=0;unsigned before=status_queries;CHECK(draw_wifi_channel_survey_live()==ESP_OK);
 char exact_button[64];snprintf(exact_button,sizeof(exact_button),"\n%s\n",button);
 CHECK(strstr(drawn,state)&&strstr(drawn,exact_button)&&status_queries==before);}
static void ui_tick(uint32_t now_ms){
#include "survey_ui_tick.h"
}
static void ui_back(void){
#include "survey_ui_back.h"
}

static void reset(void)
{
    CHECK(!locked&&!critical&&!link_locked&&!worker_running);
    memset(&scan_status,0,sizeof(scan_status));memset(&connection_status,0,sizeof(connection_status));
    memset(&ble_status,0,sizeof(ble_status));memset(&ble_connection,0,sizeof(ble_connection));
    memset(&defense_status,0,sizeof(defense_status));
    memset(&channel_survey_status,0,sizeof(channel_survey_status));
    channel_survey_status.dwell_ms=100;memset(channel_survey_results,0,sizeof(channel_survey_results));
    atomic_store(&channel_survey_cancelled,false);atomic_store(&channel_survey_radio_reserved,false);
    scanner_ready=true;station_reserved=false;notifications=enable_calls=disable_calls=channel_calls=delay_calls=0;
    filter_error=enable_error=disable_error=channel_error=0;sdk_promiscuous=false;
    cancel_after_delay=11;run_worker_during_wait=true;run_worker_on_notify=false;hold_worker=false;clock_us=1000000;
    starts=stops=status_queries=reads=draws=0;fail_before_command=drop_after_command=0;
    memset(&wifi_channel_survey_status,0,sizeof(wifi_channel_survey_status));
    memset(wifi_channel_survey_rows,0,sizeof(wifi_channel_survey_rows));
    wifi_channel_survey_attempted=wifi_channel_survey_claimed=wifi_channel_survey_cleanup_requested=false;
    wifi_channel_survey_cleanup_polled=false;wifi_channel_survey_claim_generation=wifi_channel_survey_claim_band=0;
    wifi_channel_survey_cleanup_last_poll=wifi_channel_survey_last_poll=0;wifi_channel_survey_error=ESP_ERR_INVALID_STATE;
    wifi_channel_survey_page=0;wifi_mode=UI_WIFI_MODE_CHANNELS_24;current_page=UI_PAGE_WIFI;screen_asleep=false;
    wifi_channel_survey_graph=false;
    c5_status.firmware_version=CERBERUS_WIFI_CHANNEL_SURVEY_SESSION_MIN_FIRMWARE;
    peer_firmware_version=0;sequence=0;device_queries=scoped_starts=scoped_stops=scoped_statuses=scoped_reads=wire_calls=0;
    reboot_before=reboot_after=corrupt_command=corrupt_part=corrupt_kind=0;wire_cost_us=0;worker_delay_hook=NULL;
    fail_read_part=255;supersede_read_pin=false;
    wifi_channel_survey_claim_session=0;wifi_channel_survey_metrics_next=wifi_channel_survey_metrics_page=wifi_channel_survey_rows_valid=0;
    wifi_channel_survey_cancelled=wifi_channel_survey_radio_held=false;
    reset_c5_epoch(100);
}
static void native_lifecycle(void)
{
    reset();CHECK(cerberus_wifi_channel_survey_start(1)==ESP_OK&&notifications==1);
    CHECK(ble_other_radio_active()&&!atomic_load(&channel_survey_radio_reserved));
    CHECK(cerberus_wifi_channel_survey_preempt()==ESP_OK);
    CHECK(!enable_calls&&!ble_other_radio_active()&&channel_survey_status.state==CERBERUS_WIFI_CHANNEL_SURVEY_IDLE);
    CHECK(cerberus_wifi_channel_survey_start(2)==ESP_OK);disable_error=ESP_FAIL;run_worker();
    CHECK(enable_calls==1&&channel_calls>=2&&sdk_promiscuous);
    CHECK(channel_survey_status.state==CERBERUS_WIFI_CHANNEL_SURVEY_ERROR&&atomic_load(&channel_survey_radio_reserved));
    CHECK(ble_other_radio_active()&&cerberus_wifi_channel_survey_start(1)==ESP_ERR_INVALID_STATE);
    unsigned before=disable_calls;CHECK(cerberus_wifi_channel_survey_preempt()==ESP_FAIL&&disable_calls==before+1);
    CHECK(ble_other_radio_active()&&sdk_promiscuous);
    disable_error=0;CHECK(cerberus_wifi_channel_survey_preempt()==ESP_OK);
    CHECK(!ble_other_radio_active()&&!sdk_promiscuous&&!atomic_load(&channel_survey_radio_reserved));
    CHECK(cerberus_wifi_channel_survey_start(1)==ESP_OK);filter_error=ESP_FAIL;run_worker();
    CHECK(channel_survey_status.state==CERBERUS_WIFI_CHANNEL_SURVEY_ERROR&&!ble_other_radio_active());
    CHECK(cerberus_wifi_channel_survey_preempt()==ESP_OK); /* No held cleanup remains. */
    CHECK(channel_survey_status.state==CERBERUS_WIFI_CHANNEL_SURVEY_IDLE&&!channel_survey_status.error_code);
    reset();CHECK(cerberus_wifi_channel_survey_start(1)==ESP_OK);hold_worker=true;
    int64_t before_time=clock_us;CHECK(cerberus_wifi_channel_survey_preempt()==ESP_ERR_TIMEOUT);
    CHECK(clock_us-before_time==1000000&&ble_other_radio_active());hold_worker=false;
    CHECK(cerberus_wifi_channel_survey_preempt()==ESP_OK&&!ble_other_radio_active());
    /* Actual receive callback remains bounded to the current channel. */
    wifi_promiscuous_pkt_t packet={.rx_ctrl={.sig_len=24,.channel=6,.rssi=-40}};
    channel_survey_receiving=true;channel_survey_receive_channel=6;channel_survey_packet_count=0;
    channel_survey_receive(&packet,0);CHECK(channel_survey_packet_count==1);
    packet.rx_ctrl.channel=11;channel_survey_receive(&packet,0);CHECK(channel_survey_packet_count==1);
    channel_survey_receiving=false;
}
static void tick_after(unsigned ms)
{clock_us+=(int64_t)ms*1000;ui_tick((uint32_t)(clock_us/1000));}
static void settle_stop(void)
{
    CHECK(!wifi_channel_survey_stop_ui()&&wifi_channel_survey_claimed);
    draw_expect("RELEASING C5 RADIO","STOPPING");
    CHECK(!strstr(drawn,"C5 SURVEY")&&!strstr(drawn,"RETRY STOP"));
    run_worker();CHECK(wifi_channel_survey_stop_ui()&&!wifi_channel_survey_claimed);
    CHECK(!sdk_promiscuous&&!ble_other_radio_active()&&!stops);
}
static void ui_uncertain_and_replacement(void)
{
    reset();reboot_before=CERBERUS_SPI_COMMAND_WIFI_CHANNEL_SURVEY_SESSION_START;
    wifi_channel_survey_begin_ui();CHECK(scoped_starts==1&&!notifications&&!wifi_channel_survey_claimed);
    CHECK(wifi_channel_survey_error==ESP_ERR_INVALID_STATE&&!ble_other_radio_active());
    CHECK(wifi_channel_survey_stop_ui()&&!scoped_stops&&!stops);
    tick_after(700);CHECK(scoped_starts==1); /* Rejection cannot auto-start. */

    reset();drop_after_command=CERBERUS_SPI_COMMAND_WIFI_CHANNEL_SURVEY_SESSION_START;
    wifi_channel_survey_begin_ui();CHECK(wifi_channel_survey_error==ESP_ERR_TIMEOUT&&wifi_channel_survey_claimed);
    CHECK(notifications==1&&ble_other_radio_active());
    tick_after(700);CHECK(wifi_channel_survey_error==ESP_OK&&wifi_channel_survey_claim_generation==1);
    CHECK(scoped_starts==1);settle_stop();

    reset();fail_before_command=CERBERUS_SPI_COMMAND_WIFI_CHANNEL_SURVEY_SESSION_START;
    wifi_channel_survey_begin_ui();CHECK(wifi_channel_survey_claimed&&!notifications);
    CHECK(wifi_channel_survey_stop_ui()&&!wifi_channel_survey_claimed&&!scoped_stops);

    reset();reboot_after=CERBERUS_SPI_COMMAND_WIFI_CHANNEL_SURVEY_SESSION_START;
    wifi_channel_survey_begin_ui();CHECK(wifi_channel_survey_claimed&&notifications==1);
    CHECK(wifi_channel_survey_stop_ui()&&!wifi_channel_survey_claimed&&!scoped_stops);

    reset();wifi_channel_survey_begin_ui();uint64_t old=wifi_channel_survey_claim_session;
    CHECK(cerberus_wifi_channel_survey_preempt()==ESP_OK);
    channel_survey_status.generation=0; /* Same generation byte on new full identity. */
    cerberus_wifi_channel_survey_session_start_t next={old+1,2};
    cerberus_wifi_channel_survey_session_status_t reply;
    CHECK(cerberus_wifi_channel_survey_session_start(&next,&reply)==ESP_OK&&reply.generation==1);
    CHECK(wifi_channel_survey_stop_ui()&&!wifi_channel_survey_claimed&&!scoped_stops);
    CHECK(channel_survey_session_id==next.session_id&&ble_other_radio_active());
    wifi_channel_survey_begin_ui();CHECK(scoped_starts==1&&!wifi_channel_survey_claimed);
    CHECK(cerberus_wifi_channel_survey_preempt()==ESP_OK);
    wifi_channel_survey_begin_ui();CHECK(scoped_starts==2&&wifi_channel_survey_claimed);settle_stop();
}
static unsigned cleanup_hook_calls;
static void cleanup_delay_observer(void)
{
    if(!atomic_load(&channel_survey_cancelled))return;
    ++cleanup_hook_calls;
    tick_after(700);CHECK(wifi_channel_survey_claimed&&ble_other_radio_active());
    CHECK(wifi_channel_survey_status.state==CERBERUS_WIFI_CHANNEL_SURVEY_RUNNING);
    if(cleanup_hook_calls==1){ui_back();CHECK(wifi_channel_survey_claimed&&wifi_channel_survey_cleanup_requested);
        draw_expect("STOPPING","RETRY STOP");}
    CHECK(!wifi_channel_survey_stop_ui()&&wifi_channel_survey_claimed);
    unsigned before=scoped_starts;wifi_channel_survey_begin_ui();CHECK(scoped_starts==before);
    if(cleanup_hook_calls==3)disable_error=ESP_OK;
}
static void ui_failed_cleanup_and_hidden(void)
{
    reset();wifi_channel_survey_begin_ui();disable_error=ESP_FAIL;
    cleanup_hook_calls=0;worker_delay_hook=cleanup_delay_observer;run_worker();worker_delay_hook=NULL;
    CHECK(cleanup_hook_calls==3&&disable_calls==4&&!sdk_promiscuous);
    tick_after(700);CHECK(!wifi_channel_survey_claimed&&scoped_starts==1);
    draw_expect("PAUSED","AP LIST");ui_back();CHECK(wifi_mode==UI_WIFI_MODE_MENU);

    wifi_mode=UI_WIFI_MODE_CHANNELS_5;wifi_channel_survey_begin_ui();CHECK(scoped_starts==2);
    screen_asleep=true;ui_tick((uint32_t)(clock_us/1000));CHECK(wifi_channel_survey_claimed&&scoped_stops);
    run_worker();tick_after(700);CHECK(!wifi_channel_survey_claimed);
    screen_asleep=false;tick_after(700);CHECK(scoped_starts==2);
    wifi_channel_survey_begin_ui();CHECK(scoped_starts==3);
    current_page=UI_PAGE_OTHER;tick_after(700);CHECK(wifi_channel_survey_claimed);
    run_worker();tick_after(700);CHECK(!wifi_channel_survey_claimed&&scoped_starts==3);
}
static void ui_lost_ack_and_long_cleanup(void)
{
    reset();channel_survey_status.generation=255;wifi_channel_survey_begin_ui();
    CHECK(wifi_channel_survey_claim_generation==1&&wifi_channel_survey_claim_session==101);
    drop_after_command=CERBERUS_SPI_COMMAND_WIFI_CHANNEL_SURVEY_SESSION_STOP;
    CHECK(!wifi_channel_survey_stop_ui()&&wifi_channel_survey_claimed);
    run_worker();CHECK(wifi_channel_survey_stop_ui()&&!wifi_channel_survey_claimed&&scoped_stops==1);
    cerberus_wifi_channel_survey_session_status_t replay;
    unsigned before=notifications;
    CHECK(c5_spi_link_wifi_channel_survey_session_start(101,1,&replay)==ESP_OK);
    CHECK(replay.state==CERBERUS_WIFI_CHANNEL_SURVEY_IDLE&&notifications==before&&!ble_other_radio_active());

    reset();wifi_channel_survey_begin_ui();current_page=UI_PAGE_OTHER;
    for(unsigned i=0;i<20;++i){tick_after(700);CHECK(wifi_channel_survey_claimed&&wifi_channel_survey_cleanup_requested);}
    CHECK(scoped_starts==1&&scoped_stops==20&&notifications==1);
    run_worker();tick_after(700);CHECK(!wifi_channel_survey_claimed&&!ble_other_radio_active());
    current_page=UI_PAGE_WIFI;tick_after(700);CHECK(scoped_starts==1);

    reset();wifi_channel_survey_begin_ui();clock_us+=11000000;
    tick_after(700);CHECK(atomic_load(&channel_survey_cancelled));
    draw_expect("RELEASING C5 RADIO","STOPPING");
    CHECK(!strstr(drawn,"C5 SURVEY"));run_worker();tick_after(700);
    before=notifications;CHECK(c5_spi_link_wifi_channel_survey_session_start(101,1,&replay)==ESP_OK);
    CHECK(notifications==before&&replay.state==CERBERUS_WIFI_CHANNEL_SURVEY_IDLE);
    /* One explicit restart tap may retire a confirmed terminal job and start. */
    wifi_channel_survey_begin_ui();CHECK(scoped_starts==3&&wifi_channel_survey_claim_session==102);
    settle_stop();

    reset();wifi_channel_survey_begin_ui();filter_error=ESP_FAIL;run_worker();tick_after(700);
    CHECK(wifi_channel_survey_status.state==CERBERUS_WIFI_CHANNEL_SURVEY_ERROR);
    CHECK(!wifi_channel_survey_radio_held&&wifi_channel_survey_claimed);
    filter_error=ESP_OK;wifi_channel_survey_begin_ui();
    CHECK(scoped_starts==2&&wifi_channel_survey_claim_session==102);settle_stop();
}
static void projection_atomicity_and_bounds(void)
{
    reset();wifi_channel_survey_begin_ui();uint64_t id=wifi_channel_survey_claim_session;
    for(unsigned i=0;i<channel_survey_status.result_count;++i)channel_survey_results[i]=(cerberus_wifi_channel_survey_result_payload_t){
        .generation=1,.index=(uint8_t)i,.channel=(uint8_t)(i+1),.seen=1,.dwell_ms=100,
        .last_packets=3,.total_packets=17+i,.rate_per_sec=30,.average_rssi=-50,.strongest_rssi=-40,.sample_count=2};
    channel_survey_status.total_packets=123;channel_survey_status.elapsed_ms=456;
    int64_t rotation_started=clock_us;
    for(unsigned i=0;i<UI_WIFI_VISIBLE_ROWS+1U;++i){
        unsigned before=wire_calls;tick_after(UI_WIFI_SURVEY_METRICS_MS-1U);CHECK(wire_calls==before);
        tick_after(1U);CHECK(wire_calls-before==4);
    }
    CHECK(clock_us-rotation_started==(UI_WIFI_VISIBLE_ROWS+1U)*UI_WIFI_SURVEY_METRICS_MS*1000U);
    CHECK(wifi_channel_survey_rows_valid==((1U<<UI_WIFI_VISIBLE_ROWS)-1U)&&wifi_channel_survey_status.total_packets==123);
    CHECK(wifi_channel_survey_rows[4].index==4&&wifi_channel_survey_rows[4].channel==5&&wifi_channel_survey_rows[4].total_packets==21);
    CHECK(wifi_channel_survey_rows[5].index==5&&wifi_channel_survey_rows[5].channel==6&&wifi_channel_survey_rows[5].total_packets==22);
    draw_expect("CH 6","STOP");CHECK(strstr(drawn,"CH 5"));
    cerberus_wifi_channel_survey_result_payload_t prior=wifi_channel_survey_rows[0],out;
    channel_survey_results[0].dwell_ms=120;
    CHECK(c5_spi_link_wifi_channel_survey_session_result(id,1,0,&out)==ESP_OK&&out.dwell_ms==120);
    channel_survey_results[0].dwell_ms=0;memset(&out,0xa5,sizeof(out));
    CHECK(c5_spi_link_wifi_channel_survey_session_result(id,1,0,&out)==ESP_ERR_INVALID_RESPONSE);
    const uint8_t *empty=(const uint8_t *)&out;for(unsigned i=0;i<sizeof(out);++i)CHECK(!empty[i]);
    channel_survey_results[0].dwell_ms=100;
    for(unsigned part=0;part<3;++part){
        fail_read_part=(uint8_t)part;memset(&out,0xa5,sizeof(out));
        CHECK(c5_spi_link_wifi_channel_survey_session_result(id,1,0,&out)==ESP_ERR_TIMEOUT);
        const uint8_t *bytes=(const uint8_t *)&out;for(unsigned i=0;i<sizeof(out);++i)CHECK(!bytes[i]);
    }
    supersede_read_pin=true;
    CHECK(c5_spi_link_wifi_channel_survey_session_result(id,1,0,&out)==ESP_ERR_NOT_FOUND);
    CHECK(!out.channel&&!out.total_packets);
    for(unsigned kind=1;kind<=7;++kind){
        for(unsigned part=0;part<3;++part){
            if((kind==2&&part==0)||(kind==4&&part!=2))continue;
            corrupt_command=CERBERUS_SPI_COMMAND_WIFI_CHANNEL_SURVEY_SESSION_READ;
            corrupt_part=(uint8_t)part;corrupt_kind=(uint8_t)kind;memset(&out,0xa5,sizeof(out));
            CHECK(c5_spi_link_wifi_channel_survey_session_result(id,1,0,&out)==ESP_ERR_INVALID_RESPONSE);
            const uint8_t *bytes=(const uint8_t *)&out;for(unsigned i=0;i<sizeof(out);++i)CHECK(!bytes[i]);
        }
    }
    wifi_channel_survey_metrics_next=1;fail_before_command=CERBERUS_SPI_COMMAND_WIFI_CHANNEL_SURVEY_SESSION_READ;
    tick_after(UI_WIFI_SURVEY_METRICS_MS);CHECK(!memcmp(&prior,&wifi_channel_survey_rows[0],sizeof(prior)));
    unsigned before_retry=wire_calls;tick_after(UI_WIFI_SURVEY_RETRY_MS-1U);CHECK(wire_calls==before_retry);
    tick_after(1U);CHECK(wire_calls-before_retry==4&&wifi_channel_survey_error==ESP_OK);
    /* Failed next-page status never paints prior-page metrics as new rows. */
    wifi_channel_survey_page=1;fail_before_command=CERBERUS_SPI_COMMAND_WIFI_CHANNEL_SURVEY_SESSION_STATUS;
    tick_after(700);CHECK(!wifi_channel_survey_rows_valid);draw_expect("CHANNEL METRICS PENDING","STOP");
    for(unsigned i=0;i<UI_WIFI_VISIBLE_ROWS;++i){unsigned before=wire_calls;
        tick_after(i ? UI_WIFI_SURVEY_METRICS_MS : UI_WIFI_SURVEY_RETRY_MS);CHECK(wire_calls-before==4);}
    CHECK(wifi_channel_survey_rows_valid==((1U<<UI_WIFI_VISIBLE_ROWS)-1U));
    CHECK(wifi_channel_survey_rows[4].index==10&&wifi_channel_survey_rows[4].channel==11);
    CHECK(wifi_channel_survey_rows[5].index==11&&wifi_channel_survey_rows[5].channel==12);
    draw_expect("CH 12","STOP");CHECK(strstr(drawn,"CH 11"));
    wifi_channel_survey_page=2;
    for(unsigned i=0;i<UI_WIFI_VISIBLE_ROWS+1U;++i){unsigned before=wire_calls;
        tick_after(UI_WIFI_SURVEY_METRICS_MS);CHECK(wire_calls-before<=4);}
    CHECK(wifi_channel_survey_rows_valid==3U);
    CHECK(wifi_channel_survey_rows[0].index==12&&wifi_channel_survey_rows[1].index==13);
    draw_expect("CH 14","STOP");CHECK(!strstr(drawn,"CH 12"));
    settle_stop();wifi_mode=UI_WIFI_MODE_CHANNELS_5;
    fail_before_command=CERBERUS_SPI_COMMAND_WIFI_CHANNEL_SURVEY_SESSION_INFO;
    wifi_channel_survey_begin_ui();CHECK(!wifi_channel_survey_rows_valid&&!wifi_channel_survey_status.result_count);
    CHECK(wifi_channel_survey_error==ESP_ERR_TIMEOUT&&!wifi_channel_survey_claimed);

    reset();wifi_channel_survey_begin_ui();id=wifi_channel_survey_claim_session;
    wire_cost_us=7000;CHECK(c5_spi_link_wifi_channel_survey_session_result(id,1,0,&out)==ESP_ERR_TIMEOUT);
    CHECK(!out.channel);wire_cost_us=0;
    reboot_after=CERBERUS_SPI_COMMAND_WIFI_CHANNEL_SURVEY_SESSION_READ;
    CHECK(c5_spi_link_wifi_channel_survey_session_result(id,1,0,&out)==ESP_ERR_NOT_FOUND);
    CHECK(!out.channel&&!out.total_packets);
    CHECK(wifi_channel_survey_stop_ui()&&!scoped_stops);

    reset();wire_cost_us=10000;wifi_channel_survey_begin_ui();
    CHECK(wifi_channel_survey_error==ESP_ERR_TIMEOUT&&!scoped_starts&&!wifi_channel_survey_claimed);
}
static void old_peer_gate(void)
{
    const unsigned versions[]={0,46,68,69};
    for(unsigned i=0;i<sizeof(versions)/sizeof(versions[0]);++i){
        reset();c5_status.firmware_version=versions[i];wifi_channel_survey_begin_ui();
        CHECK(wifi_channel_survey_error==ESP_ERR_NOT_SUPPORTED&&!wifi_channel_survey_claimed);
        CHECK(!scoped_starts&&!scoped_stops&&!wire_calls);draw_expect("C5 FIRMWARE 70 REQUIRED","AP LIST");
        ui_tick((uint32_t)(clock_us/1000));ui_back();CHECK(!starts&&!stops&&!wire_calls);
        cerberus_wifi_channel_survey_session_status_t out;
        CHECK(c5_spi_link_wifi_channel_survey_session_start(101,1,&out)==ESP_ERR_NOT_SUPPORTED);
        CHECK(!scoped_starts&&!scoped_stops&&device_queries==1);
    }
}
static void view_controls(void)
{
    reset();wifi_channel_survey_begin_ui();unsigned before=wire_calls;
    CHECK(wifi_channel_survey_view_tap(164,78)&&wifi_channel_survey_graph);
    draw_expect("NOMINAL 100MS", "STOP");
    CHECK(strstr(drawn,"PENDING")&&!strstr(drawn,"\n0MS\n"));
    c5_status.firmware_version=CERBERUS_WIFI_CHANNEL_SURVEY_MEASURED_MIN_FIRMWARE;
    draw_expect("C5 MEASURED RX", "STOP");
    CHECK(wifi_channel_survey_view_tap(155,115)&&!wifi_channel_survey_graph);
    CHECK(!wifi_channel_survey_view_tap(156,90)&&!wifi_channel_survey_graph);
    CHECK(!wifi_channel_survey_view_tap(163,90)&&!wifi_channel_survey_graph);
    CHECK(!wifi_channel_survey_view_tap(164,77)&&!wifi_channel_survey_graph);
    CHECK(!wifi_channel_survey_view_tap(164,116)&&!wifi_channel_survey_graph);
    CHECK(!wifi_channel_survey_view_tap(312,90)&&!wifi_channel_survey_graph);
    CHECK(wifi_channel_survey_view_tap(311,115)&&wifi_channel_survey_graph);
    wifi_mode=UI_WIFI_MODE_MENU;
    CHECK(!wifi_channel_survey_view_tap(8,78)&&wifi_channel_survey_graph);
    CHECK(wire_calls==before&&scoped_starts==1&&!scoped_stops);
    wifi_mode=UI_WIFI_MODE_CHANNELS_24;settle_stop();
}
static void preview(const char *prefix,const char *name)
{
    drawn[0]=0;CHECK(cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND)==ESP_OK);
    CHECK(draw_header("CHANNEL SURVEY")==ESP_OK);CHECK(draw_wifi_channel_survey_live()==ESP_OK);
    CHECK(draw_small_button(8,430,304,42,"BACK")==ESP_OK);
    char path[1024];snprintf(path,sizeof(path),"%s-%s-%s.ppm",prefix,name,wifi_channel_survey_graph?"graph":"list");
    FILE *file=fopen(path,"wb");CHECK(file);CHECK(fprintf(file,"P6\n320 480\n255\n")>0);
    for(unsigned y=0;y<480;++y)for(unsigned x=0;x<320;++x){uint16_t p=pixels[y][x];
        const uint8_t rgb[]={(uint8_t)(((p>>11)&31)*255/31),(uint8_t)(((p>>5)&63)*255/63),(uint8_t)((p&31)*255/31)};
        if(fwrite(rgb,1,3,file)!=3)abort();
    }
    CHECK(fclose(file)==0);
}
static void previews(const char *prefix)
{
    static const char *names[]={"running","held-error","released","releasing"};
    reset();wifi_channel_survey_begin_ui();
    wifi_channel_survey_rows_valid=(uint8_t)((1U<<UI_WIFI_VISIBLE_ROWS)-1U);wifi_channel_survey_status.total_sweeps=7;wifi_channel_survey_status.total_packets=314;
    for(unsigned i=0;i<UI_WIFI_VISIBLE_ROWS;++i)wifi_channel_survey_rows[i]=(cerberus_wifi_channel_survey_result_payload_t){
        .channel=(uint8_t)(1+i*2),.seen=1,.rate_per_sec=(uint16_t)(12+i*4),.average_rssi=-65,
        .total_packets=70+i,.last_packets=(uint16_t)(4+i),.strongest_rssi=-42,.dwell_ms=(uint16_t)(100+i)};
    c5_status.firmware_version=CERBERUS_WIFI_CHANNEL_SURVEY_MEASURED_MIN_FIRMWARE;
    for(unsigned state=0;state<sizeof(names)/sizeof(names[0]);++state){
        if(state==1){wifi_channel_survey_cleanup_requested=true;wifi_channel_survey_error=ESP_FAIL;}
        if(state==2){wifi_channel_survey_claimed=false;wifi_channel_survey_status.state=CERBERUS_WIFI_CHANNEL_SURVEY_IDLE;wifi_channel_survey_error=ESP_OK;}
        if(state==3){wifi_channel_survey_claimed=true;wifi_channel_survey_cleanup_requested=true;
            wifi_channel_survey_status.state=CERBERUS_WIFI_CHANNEL_SURVEY_RUNNING;wifi_channel_survey_error=ESP_ERR_NOT_FINISHED;}
        for(unsigned view=0;view<2;++view){wifi_channel_survey_graph=view;preview(prefix,names[state]);}
    }
    wifi_channel_survey_claimed=false;wifi_channel_survey_cleanup_requested=false;
    wifi_channel_survey_error=ESP_OK;wifi_channel_survey_status.state=CERBERUS_WIFI_CHANNEL_SURVEY_IDLE;
    wifi_channel_survey_status.total_sweeps=UINT16_MAX;wifi_channel_survey_status.total_packets=UINT32_MAX;
    wifi_channel_survey_rows[0].total_packets=UINT32_MAX;wifi_channel_survey_rows[0].last_packets=UINT16_MAX;
    wifi_channel_survey_rows[0].average_rssi=-127;wifi_channel_survey_rows[0].strongest_rssi=-127;
    wifi_channel_survey_rows[0].rate_per_sec=UINT16_MAX;wifi_channel_survey_rows[0].dwell_ms=UINT16_MAX;
    wifi_channel_survey_rows[1].seen=false;wifi_channel_survey_rows_valid&=~(1U<<2);
    wifi_channel_survey_rows[3].rate_per_sec=0;
    for(unsigned view=0;view<2;++view){wifi_channel_survey_graph=view;preview(prefix,"limits");}
    CHECK(strstr(drawn,">=65535")&&strstr(drawn,"65535MS")&&strstr(drawn,"N/A")&&strstr(drawn,"PENDING"));
    CHECK(strstr(drawn,"RETAINED VALUES / CHECK STATUS ABOVE"));
    wifi_mode=UI_WIFI_MODE_CHANNELS_5;wifi_channel_survey_page=wifi_channel_survey_metrics_page=4;
    wifi_channel_survey_status.result_count=25;wifi_channel_survey_rows_valid=1;
    wifi_channel_survey_rows[0].channel=165;
    preview(prefix,"last-page");CHECK(strstr(drawn,"CH 165")&&!strstr(drawn,"CH 3"));
    wifi_channel_survey_rows[0].dwell_ms=100;c5_status.firmware_version=80;
    preview(prefix,"legacy");CHECK(strstr(drawn,"NOMINAL 100MS"));
}
int main(int argc,char **argv)
{
    CHECK(argc==2);
    native_lifecycle();ui_failed_cleanup_and_hidden();ui_uncertain_and_replacement();ui_lost_ack_and_long_cleanup();projection_atomicity_and_bounds();old_peer_gate();view_controls();
    previews(argv[1]);
    printf("Channel Survey lifecycle: %u checks; actual worker/admission/SPI transport/UI cleanup, queued/dwell/disable failure/retry, uncertain Start, observed replacement, Back/sleep/hidden and reentry passed.\n",checks);
    return 0;
}
