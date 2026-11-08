#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int esp_err_t;
enum {ESP_OK=0,ESP_FAIL=-1,ESP_ERR_NO_MEM=257,ESP_ERR_INVALID_ARG,ESP_ERR_INVALID_STATE,
    ESP_ERR_TIMEOUT,ESP_ERR_NOT_SUPPORTED,ESP_ERR_NOT_FINISHED};
static unsigned checks,depth,allocations;
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"line %u: %s\n",__LINE__,#x);exit(1);}}while(0)
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) do{(void)x;CHECK(!depth);++depth;}while(0)
#define portEXIT_CRITICAL(x) do{(void)x;CHECK(depth==1);--depth;}while(0)
#ifndef FIXTURE_TICK_MS
#define FIXTURE_TICK_MS 1U
#endif
#define pdMS_TO_TICKS(x) ((x)/FIXTURE_TICK_MS)
#define pdPASS 1
#ifndef FIXTURE_EXT_ADV
#define FIXTURE_EXT_ADV 0
#endif
#define MYNEWT_VAL(x) FIXTURE_EXT_ADV
enum {MALLOC_CAP_INTERNAL=1,MALLOC_CAP_8BIT=2,MALLOC_CAP_SPIRAM=4,
    WIFI_SECOND_CHAN_NONE=0,WIFI_STORAGE_RAM=1,WIFI_MODE_NULL=0,WIFI_PROMIS_FILTER_MASK_MGMT=1,
    BLE_HS_FOREVER=0,BLE_HS_EALREADY=99,BLE_GAP_EVENT_DISC=1,BLE_GAP_EVENT_EXT_DISC=2};
typedef enum {WIFI_PKT_MGMT=0,WIFI_PKT_DATA=1} wifi_promiscuous_pkt_type_t;
typedef struct {unsigned sig_len,rx_state,channel;int rssi;} rx_control_t;
typedef struct {rx_control_t rx_ctrl;uint8_t payload[256];} wifi_promiscuous_pkt_t;
typedef struct {unsigned filter_mask;} wifi_promiscuous_filter_t;
typedef struct {int static_rx_buf_num,dynamic_rx_buf_num,dynamic_tx_buf_num,rx_ba_win,
    ampdu_rx_enable,ampdu_tx_enable,amsdu_tx_enable;} wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){0})
struct ble_gap_disc_params{uint16_t itvl,window;uint8_t passive,filter_duplicates;};
struct ble_gap_ext_disc_params{uint16_t itvl,window;uint8_t passive;};
struct ble_gap_event{int type;struct {uint8_t *data;size_t length_data;
    struct {uint8_t val[6],type;}addr;int rssi;}disc;
    struct {uint8_t *data;size_t length_data;struct {uint8_t val[6],type;}addr;int rssi,data_status;}ext_disc;};
static uint64_t now_us=1000000;
static int64_t esp_timer_get_time(void){return (int64_t)now_us;}
static bool fail_alloc,fail_task,driver_rx,ble_rx,drop_contact,drain,prepare_fail;
static unsigned cleanup_failures,channel,radio_stops,quiet_calls,ble_cancel_pending;
static uint32_t stop_at;
static void (*worker_fn)(void *);
static int (*gap_fn)(struct ble_gap_event *,void *);
static void *gap_arg;
static void *heap_caps_calloc(size_t n,size_t size,unsigned caps)
{CHECK(!depth&&(caps==3||caps==6));if(fail_alloc)return NULL;void *p=calloc(n,size);CHECK(p);++allocations;return p;}
static void heap_caps_free(void *p){CHECK(!depth);if(p){CHECK(allocations);--allocations;free(p);}}
static int xTaskCreate(void (*fn)(void *),const char *name,unsigned stack,void *arg,unsigned priority,void *handle)
{CHECK(!depth&&!strcmp(name,"wd_radio")&&stack==4096&&!arg&&priority==4&&!handle);if(fail_task)return 0;worker_fn=fn;return pdPASS;}
static void vTaskDelete(void *unused){CHECK(!depth&&!unused);++radio_stops;}
static void vTaskDelay(unsigned ticks);
static void pause_control(void);
#if CONFIG_IDF_TARGET_ESP32C5
typedef int wifi_band_mode_t;
typedef int wifi_second_chan_t;
enum {WIFI_BAND_MODE_AUTO=0,WIFI_BAND_MODE_2G_ONLY=1,WIFI_BAND_2G=2,WIFI_BAND_5G=5};
static wifi_band_mode_t current_mode=WIFI_BAND_MODE_2G_ONLY;
static unsigned current_band=WIFI_BAND_2G,current_filter=9;
static esp_err_t esp_wifi_get_band_mode(wifi_band_mode_t *mode){CHECK(!depth);*mode=current_mode;return ESP_OK;}
static esp_err_t esp_wifi_set_band_mode(wifi_band_mode_t mode){CHECK(!depth&&!ble_rx);current_mode=mode;return ESP_OK;}
static esp_err_t esp_wifi_get_channel(uint8_t *ch,wifi_second_chan_t *second){CHECK(!depth);*ch=6;*second=WIFI_SECOND_CHAN_NONE;return ESP_OK;}
static esp_err_t esp_wifi_set_band(unsigned band){CHECK(!depth&&!ble_rx&&current_mode==WIFI_BAND_MODE_AUTO);current_band=band;return ESP_OK;}
static esp_err_t esp_wifi_get_promiscuous_filter(wifi_promiscuous_filter_t *filter){CHECK(!depth);filter->filter_mask=current_filter;return ESP_OK;}
#endif
static esp_err_t esp_wifi_set_channel(unsigned value,int secondary)
{CHECK(!depth&&secondary==WIFI_SECOND_CHAN_NONE&&!ble_rx);
#if CONFIG_IDF_TARGET_ESP32C5
 CHECK(current_band==(value<=14?WIFI_BAND_2G:WIFI_BAND_5G));
#endif
 channel=value;return ESP_OK;}
static esp_err_t esp_wifi_set_promiscuous(bool enabled)
{CHECK(!depth);if(!enabled&&cleanup_failures){--cleanup_failures;return ESP_FAIL;}
 CHECK(!enabled||!ble_rx);if(!enabled&&driver_rx)++quiet_calls;driver_rx=enabled;return ESP_OK;}
static esp_err_t esp_wifi_set_promiscuous_filter(const wifi_promiscuous_filter_t *f)
{CHECK(!depth&&(f->filter_mask==WIFI_PROMIS_FILTER_MASK_MGMT||f->filter_mask==9));
 if(prepare_fail){prepare_fail=false;return ESP_FAIL;}
#if CONFIG_IDF_TARGET_ESP32C5
 current_filter=f->filter_mask;
#endif
 return ESP_OK;}
static int ble_hs_id_infer_auto(int privacy,uint8_t *type){CHECK(!depth&&!privacy);*type=0;return 0;}
#if CONFIG_IDF_TARGET_ESP32C5 && FIXTURE_EXT_ADV
static int ble_gap_ext_disc(unsigned type,int duration,int period,int duplicates,int filter,int limited,
    const struct ble_gap_ext_disc_params *one_m,const struct ble_gap_ext_disc_params *coded,
    int (*fn)(struct ble_gap_event *,void *),void *arg)
{CHECK(!depth&&!driver_rx&&!ble_rx&&!type&&!duration&&!period&&!duplicates&&!filter&&!limited);
 CHECK(one_m&&coded&&one_m->passive&&coded->passive&&one_m->itvl==160&&coded->itvl==160&&one_m->window==80&&coded->window==80);
 gap_fn=fn;gap_arg=arg;ble_rx=true;return 0;}
#else
static int ble_gap_disc(unsigned type,int duration,const struct ble_gap_disc_params *p,
    int (*fn)(struct ble_gap_event *,void *),void *arg)
{CHECK(!depth&&!driver_rx&&!ble_rx&&!type&&duration==BLE_HS_FOREVER&&p->passive&&!p->filter_duplicates&&p->itvl==p->window);gap_fn=fn;gap_arg=arg;ble_rx=true;return 0;}
#endif
static int ble_gap_disc_cancel(void)
{CHECK(!depth);if(cleanup_failures){--cleanup_failures;return 1;}
 if(ble_cancel_pending){--ble_cancel_pending;return 0;}
 if(ble_rx)++quiet_calls;ble_rx=false;return 0;}
static bool ble_gap_disc_active(void){CHECK(!depth);return ble_rx;}
#if CONFIG_IDF_TARGET_ESP32S3
#include "s3_wifi_owner.h"
#include "s3_ble_host.h"
static bool wifi_owned,ble_owned,wifi_initialized,wifi_started;
cerberus_s3_ble_owner_t cerberus_s3_ble_host_owner(void){return ble_owned?CERBERUS_S3_BLE_OWNER_WARDRIVE:CERBERUS_S3_BLE_OWNER_NONE;}
esp_err_t cerberus_s3_wifi_claim(cerberus_s3_wifi_owner_t owner)
{CHECK(!depth&&owner==CERBERUS_S3_WIFI_WARDRIVE&&!wifi_owned);wifi_owned=true;return ESP_OK;}
esp_err_t cerberus_s3_wifi_release(cerberus_s3_wifi_owner_t owner)
{CHECK(!depth&&owner==CERBERUS_S3_WIFI_WARDRIVE&&wifi_owned&&!wifi_initialized&&!ble_owned);wifi_owned=false;return ESP_OK;}
esp_err_t cerberus_s3_wifi_prepare(cerberus_s3_wifi_owner_t owner)
{CHECK(!depth&&wifi_owned&&owner==CERBERUS_S3_WIFI_WARDRIVE);return ESP_OK;}
esp_err_t cerberus_s3_ble_host_claim(cerberus_s3_ble_owner_t owner,cerberus_s3_ble_host_lease_t *lease)
{CHECK(!depth&&wifi_owned&&!ble_owned&&owner==CERBERUS_S3_BLE_OWNER_WARDRIVE);ble_owned=true;lease->token=1;return ESP_OK;}
bool cerberus_s3_ble_host_lease_valid(const cerberus_s3_ble_host_lease_t *lease){return lease->token&&ble_owned;}
esp_err_t cerberus_s3_ble_host_release(const cerberus_s3_ble_host_lease_t *lease)
{CHECK(!depth&&lease->token&&ble_owned&&!ble_rx);ble_owned=false;return ESP_OK;}
static esp_err_t esp_wifi_init(const wifi_init_config_t *init)
{CHECK(!depth&&!wifi_initialized&&init->static_rx_buf_num==4&&init->dynamic_tx_buf_num==2);wifi_initialized=true;return ESP_OK;}
static esp_err_t esp_wifi_set_storage(int mode){CHECK(!depth&&mode==WIFI_STORAGE_RAM);return ESP_OK;}
static esp_err_t esp_wifi_set_mode(int mode){CHECK(!depth&&mode==WIFI_MODE_NULL);return ESP_OK;}
static esp_err_t esp_wifi_start(void){CHECK(!depth&&wifi_initialized);wifi_started=true;return ESP_OK;}
static esp_err_t esp_wifi_set_promiscuous_rx_cb(void (*fn)(void *,wifi_promiscuous_pkt_type_t))
{CHECK(!depth);(void)fn;return ESP_OK;}
static esp_err_t esp_wifi_stop(void){CHECK(!depth&&!driver_rx);wifi_started=false;return ESP_OK;}
static esp_err_t esp_wifi_deinit(void){CHECK(!depth&&!wifi_started);wifi_initialized=false;return ESP_OK;}
#else
static bool cerberus_wifi_wardrive_ready(void){return true;}
static bool cerberus_ble_wardrive_ready(void){return true;}
#endif
#include "../../shared/wardrive_radio.c"
static cerberus_wd_request_t query;
static void vTaskDelay(unsigned ticks)
{
    CHECK(!depth&&ticks);now_us+=(uint64_t)ticks*FIXTURE_TICK_MS*1000U;CHECK(now_us<UINT64_C(1000000000));
    CHECK(cerberus_wardrive_radio_held()); /* Cleanup failure must retain ownership. */
    cerberus_wd_status_t status;
    if(!drop_contact)CHECK(cerberus_wardrive_radio_exchange(CERBERUS_WD_STATUS,&query,&status)==ESP_OK);
    if(driver_rx){
        wifi_promiscuous_pkt_t frame={.rx_ctrl={.sig_len=43,.channel=channel,.rssi=-45}};
        frame.payload[0]=0x80;memset(frame.payload+4,255,6);frame.payload[10]=2;frame.payload[16]=4;
        frame.payload[36]=0;frame.payload[37]=1;frame.payload[38]='X';
        cerberus_wardrive_radio_wifi_rx(&frame,WIFI_PKT_MGMT);
    }
    if(ble_rx){
        uint8_t ad[]={2,9,'B'};struct ble_gap_event event={.type=BLE_GAP_EVENT_DISC,
            .disc={.data=ad,.length_data=3,.addr={.val={1,2,3,4,5,6}},.rssi=-50}};
        CHECK(gap_fn(&event,gap_arg)==0);
#if FIXTURE_EXT_ADV
        event.type=BLE_GAP_EVENT_EXT_DISC;event.ext_disc.data=ad;event.ext_disc.length_data=3;
        event.ext_disc.addr.val[0]=2;event.ext_disc.rssi=-51;event.ext_disc.data_status=1;
        uint32_t reports=wd_status.ble_reports;CHECK(gap_fn(&event,gap_arg)==0&&wd_status.ble_reports==reports);
        event.ext_disc.data_status=0;CHECK(gap_fn(&event,gap_arg)==0);
#endif
    }
    if(drain&&!drop_contact){
        cerberus_wd_record_t row;CHECK(cerberus_wardrive_radio_exchange(CERBERUS_WD_READ,&query,&row)==ESP_OK);
        if(row.sequence){CHECK(row.sequence>query.ack);query.ack=row.sequence;}
    }
    pause_control();
    if(!drop_contact&&wd_now()>=stop_at)CHECK(cerberus_wardrive_radio_exchange(CERBERUS_WD_STOP,&query,&status)==ESP_OK);
}
enum { PAUSE_CYCLE=1,PAUSE_STOP,PAUSE_LOSE_HOST,PAUSE_EARLY_RESUME,
       PAUSE_BEFORE_START,PAUSE_ASYNC_BLE,PAUSE_QUIET_ERROR,PAUSE_EPOCH_EXHAUSTED };
static struct {
    unsigned kind,stage,completed,quiet_before;
    uint32_t since,wifi,ble,epoch;
    void *queue,*old_ble_arg;
    int (*old_ble_callback)(struct ble_gap_event *,void *);
    bool saw_paused;
} pause_case;
static void check_stale_receive(void)
{
    uint32_t wifi=wd_status.wifi_reports,ble=wd_status.ble_reports;
    cerberus_wd_record_t old={.kind=wd_status.phase,.age_ms=wd_now()};
    wd_enqueue(&old,pause_case.epoch); /* Callback already decoding at PAUSE. */
    CHECK(wd_status.wifi_reports==wifi&&wd_status.ble_reports==ble);
    if(pause_case.old_ble_callback){
        uint8_t ad[]={2,9,'Z'};
        struct ble_gap_event event={.type=BLE_GAP_EVENT_DISC,
            .disc={.data=ad,.length_data=sizeof(ad),.addr={.val={2,2,2,2,2,2}},.rssi=-30}};
        CHECK(pause_case.old_ble_callback(&event,pause_case.old_ble_arg)==0);
        CHECK(wd_status.wifi_reports==wifi&&wd_status.ble_reports==ble);
    }
}
static void request_pause(void)
{
    cerberus_wd_status_t status;
    cerberus_wd_request_t bad=query;bad.ack=UINT32_MAX;
    CHECK(cerberus_wardrive_radio_exchange(CERBERUS_WD_PAUSE,&bad,&status)==ESP_ERR_INVALID_ARG);
    CHECK(!wd_pause_requested&&!wd_pause_pending);
    bad=query;++bad.session;
    CHECK(cerberus_wardrive_radio_exchange(CERBERUS_WD_PAUSE,&bad,&status)==ESP_ERR_INVALID_STATE);
    pause_case.epoch=wd_epoch;pause_case.queue=wd_ring.rows;pause_case.quiet_before=quiet_calls;
    pause_case.old_ble_callback=ble_rx?gap_fn:NULL;pause_case.old_ble_arg=gap_arg;
    if(pause_case.kind==PAUSE_ASYNC_BLE)ble_cancel_pending=3;
    if(pause_case.kind==PAUSE_QUIET_ERROR)cleanup_failures=2;
    if(pause_case.kind==PAUSE_EPOCH_EXHAUSTED)wd_epoch=UINT32_MAX;
    esp_err_t result=cerberus_wardrive_radio_exchange(CERBERUS_WD_PAUSE,&query,&status);
    if(pause_case.kind==PAUSE_EPOCH_EXHAUSTED){
        CHECK(result==ESP_ERR_INVALID_STATE&&wd_stop);pause_case.stage=4;return;
    }
    CHECK(result==ESP_OK&&status.state==CERBERUS_WD_PAUSING&&status.held&&!wd_accepting);
    CHECK(wd_epoch==pause_case.epoch+1);
    CHECK(cerberus_wardrive_radio_exchange(CERBERUS_WD_PAUSE,&query,&status)==ESP_OK);
    CHECK(wd_epoch==pause_case.epoch+1); /* Lost reply / repeated PAUSE. */
    CHECK(cerberus_wardrive_radio_exchange(CERBERUS_WD_START,&query,&status)==ESP_OK);
    CHECK(wd_pause_requested); /* A retried START is not RESUME. */
    pause_case.stage=1;pause_case.wifi=wd_status.wifi_reports;pause_case.ble=wd_status.ble_reports;
    check_stale_receive();
    if(pause_case.kind==PAUSE_EARLY_RESUME){
        CHECK(cerberus_wardrive_radio_exchange(CERBERUS_WD_RESUME,&query,&status)==ESP_OK);
        CHECK(wd_pause_pending&&!wd_pause_requested&&!wd_accepting);
        pause_case.stage=2;
    }
}
static void pause_control(void)
{
    if(!pause_case.kind||wd_stop||drop_contact)return;
    cerberus_wd_status_t status;
    if(pause_case.stage==0&&wd_status.state==CERBERUS_WD_RUNNING){
        /* The mixed S3 case must pause its BLE window, not only its Wi-Fi pass. */
#if CONFIG_IDF_TARGET_ESP32S3
        if(query.mode==CERBERUS_WD_COMBINED&&wd_status.phase!=CERBERUS_WD_PHASE_BLE)return;
#endif
        request_pause();return;
    }
    if(pause_case.stage==1){
        CHECK(!wd_accepting&&wd_status.wifi_reports==pause_case.wifi&&wd_status.ble_reports==pause_case.ble);
        check_stale_receive();
        if(wd_status.state==CERBERUS_WD_PAUSING)return;
        CHECK(wd_status.state==CERBERUS_WD_PAUSED&&!driver_rx&&!ble_rx);
        CHECK(wd_status.held&&wd_ring.rows==pause_case.queue&&allocations==1);
        CHECK(wd_status.phase==CERBERUS_WD_PHASE_NONE&&!wd_status.channel);
#if CONFIG_IDF_TARGET_ESP32S3
        CHECK(wifi_owned&&(query.mode==CERBERUS_WD_WIFI||ble_owned));
#endif
        CHECK(cerberus_wardrive_radio_exchange(CERBERUS_WD_RELEASE,&query,&status)==ESP_ERR_INVALID_STATE);
        if(!pause_case.saw_paused){pause_case.saw_paused=true;pause_case.since=wd_now();}
        if(pause_case.kind==PAUSE_LOSE_HOST){drop_contact=true;return;}
        if(pause_case.kind==PAUSE_STOP){
            CHECK(cerberus_wardrive_radio_exchange(CERBERUS_WD_STOP,&query,&status)==ESP_OK);
            CHECK(cerberus_wardrive_radio_exchange(CERBERUS_WD_RESUME,&query,&status)==ESP_ERR_INVALID_STATE);
            pause_case.stage=4;return;
        }
        uint32_t hold=pause_case.kind==PAUSE_CYCLE?CERBERUS_WD_LEASE_MS+50U:100U;
        if((uint32_t)(wd_now()-pause_case.since)<hold)return;
        CHECK(cerberus_wardrive_radio_exchange(CERBERUS_WD_RESUME,&query,&status)==ESP_OK);
        CHECK(cerberus_wardrive_radio_exchange(CERBERUS_WD_RESUME,&query,&status)==ESP_OK);
        CHECK(!wd_pause_requested&&!wd_accepting);pause_case.stage=2;return;
    }
    if(pause_case.stage==2&&wd_status.state==CERBERUS_WD_RUNNING){
        CHECK(wd_accepting&&wd_ring.rows==pause_case.queue&&allocations==1);
        if(pause_case.kind!=PAUSE_BEFORE_START)CHECK(quiet_calls>pause_case.quiet_before);
        check_stale_receive();
        ++pause_case.completed;
        if(pause_case.completed==2||pause_case.kind!=PAUSE_CYCLE){
            CHECK(cerberus_wardrive_radio_exchange(CERBERUS_WD_STOP,&query,&status)==ESP_OK);
            pause_case.stage=4;
        }else {pause_case.stage=0;pause_case.saw_paused=false;}
    }
}
static void pause_scenario(uint8_t mode,unsigned kind)
{
    static uint64_t session=10000;
    memset(&pause_case,0,sizeof(pause_case));pause_case.kind=kind;
    query=(cerberus_wd_request_t){.session=++session,.mode=mode};
    drain=true;drop_contact=fail_task=false;cleanup_failures=ble_cancel_pending=0;stop_at=UINT32_MAX;
    cerberus_wd_status_t status;
    CHECK(cerberus_wardrive_radio_exchange(CERBERUS_WD_START,&query,&status)==ESP_OK);
    if(kind==PAUSE_BEFORE_START)request_pause();
    CHECK(worker_fn);worker_fn(NULL);worker_fn=NULL;
    CHECK(!cerberus_wardrive_radio_held()&&!driver_rx&&!ble_rx&&allocations==1);
    CHECK(cerberus_wardrive_radio_exchange(CERBERUS_WD_STATUS,&query,&status)==ESP_OK);
    CHECK(status.state==CERBERUS_WD_STOPPED&&!status.held);
    esp_err_t expected=kind==PAUSE_LOSE_HOST?ESP_ERR_TIMEOUT:kind==PAUSE_QUIET_ERROR?ESP_FAIL:
        kind==PAUSE_EPOCH_EXHAUSTED?ESP_ERR_INVALID_STATE:ESP_OK;
    CHECK(status.error==expected);
    CHECK(cerberus_wardrive_radio_exchange(CERBERUS_WD_RESUME,&query,&status)==ESP_ERR_INVALID_STATE);
    CHECK(cerberus_wardrive_radio_exchange(CERBERUS_WD_PAUSE,&query,&status)==ESP_ERR_INVALID_STATE);
    if(kind==PAUSE_CYCLE)CHECK(pause_case.completed==2);
    else if(expected==ESP_OK&&kind!=PAUSE_STOP)CHECK(pause_case.completed==1);
    query.discard=1;CHECK(cerberus_wardrive_radio_exchange(CERBERUS_WD_RELEASE,&query,&status)==ESP_OK);
    CHECK(!allocations);memset(&pause_case,0,sizeof(pause_case));drop_contact=false;
    if(kind==PAUSE_EPOCH_EXHAUSTED)wd_epoch=1000; /* Isolate the exhaustion case. */
}
static void scenario(uint8_t mode,bool overflow,bool lost_host,bool fail_prepare,bool fail_create)
{
    static uint64_t session=100;query=(cerberus_wd_request_t){.session=++session,.mode=mode};
    drain=!overflow;drop_contact=lost_host;prepare_fail=fail_prepare;fail_task=fail_create;
    bool fail_cleanup=cleanup_failures!=0;
    stop_at=wd_now()+7000;cerberus_wd_status_t status;
    esp_err_t e=cerberus_wardrive_radio_exchange(CERBERUS_WD_START,&query,&status);
    CHECK(e==(fail_create?ESP_ERR_NO_MEM:ESP_OK));
    if(!fail_create){CHECK(worker_fn);worker_fn(NULL);worker_fn=NULL;}
    CHECK(!cerberus_wardrive_radio_held()&&!driver_rx&&!ble_rx);
    CHECK(cerberus_wardrive_radio_exchange(CERBERUS_WD_STATUS,&query,&status)==ESP_OK);
    CHECK(status.state==CERBERUS_WD_STOPPED&&!status.held);
    if(lost_host)CHECK(status.error==ESP_ERR_TIMEOUT);
    else if(fail_prepare)CHECK(status.error==ESP_FAIL);
    else if(fail_create)CHECK(status.error==ESP_ERR_NO_MEM);
    else if(fail_cleanup)CHECK(status.error==ESP_FAIL&&cleanup_failures==0);
    else {
        CHECK(status.error==ESP_OK);
        if(mode==CERBERUS_WD_BLE)CHECK(!status.wifi_reports&&status.ble_reports>1);
        else CHECK(status.wifi_reports>1&&status.wifi_sweeps>0);
#if CONFIG_IDF_TARGET_ESP32S3
        if(mode==CERBERUS_WD_COMBINED)CHECK(status.ble_reports>1);
#endif
        if(overflow)CHECK(status.dropped>0&&status.pending==WD_QUEUE_CAPACITY);
    }
    cerberus_wd_request_t stale=query;stale.session+=1000;
    CHECK(cerberus_wardrive_radio_exchange(CERBERUS_WD_RELEASE,&stale,&status)==ESP_ERR_INVALID_STATE);
    CHECK(allocations==1);
    if(overflow){CHECK(cerberus_wardrive_radio_exchange(CERBERUS_WD_RELEASE,&query,&status)==ESP_ERR_INVALID_STATE);}
    query.discard=1;CHECK(cerberus_wardrive_radio_exchange(CERBERUS_WD_RELEASE,&query,&status)==ESP_OK);
    CHECK(!allocations);CHECK(cerberus_wardrive_radio_exchange(CERBERUS_WD_START,&query,&status)==ESP_ERR_INVALID_STATE);
#if CONFIG_IDF_TARGET_ESP32S3
    CHECK(!wifi_owned&&!ble_owned&&!wifi_initialized&&!wifi_started);
#else
    CHECK(!wd_wifi_saved&&current_mode==WIFI_BAND_MODE_2G_ONLY&&current_band==WIFI_BAND_2G&&current_filter==9);
#endif
}
int main(void)
{
    scenario(CERBERUS_WD_WIFI,false,false,false,false);
    scenario(CERBERUS_WD_BLE,false,false,false,false);
    scenario(CERBERUS_WD_COMBINED,false,false,false,false);
    scenario(CERBERUS_WD_WIFI,true,false,false,false);
    scenario(CERBERUS_WD_BLE,true,true,false,false);
    scenario(CERBERUS_WD_WIFI,false,false,true,false);
    scenario(CERBERUS_WD_WIFI,false,false,false,true);
    cleanup_failures=2;scenario(CERBERUS_WD_WIFI,false,false,false,false);
    cleanup_failures=2;scenario(CERBERUS_WD_BLE,false,false,false,false);
    for(uint8_t mode=0;mode<=CERBERUS_WD_BLE;++mode){
        pause_scenario(mode,PAUSE_CYCLE);
        pause_scenario(mode,PAUSE_STOP);
        pause_scenario(mode,PAUSE_LOSE_HOST);
        pause_scenario(mode,PAUSE_EARLY_RESUME);
        pause_scenario(mode,PAUSE_BEFORE_START);
        pause_scenario(mode,PAUSE_QUIET_ERROR);
        pause_scenario(mode,PAUSE_EPOCH_EXHAUSTED);
    }
    pause_scenario(CERBERUS_WD_BLE,PAUSE_ASYNC_BLE);
    printf("Wardrive native receiver: %u checks; modes, pause/resume, stale callbacks, Stop, lease loss, overflow and failed cleanup passed\n",checks);
    return 0;
}
