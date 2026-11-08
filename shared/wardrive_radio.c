/* Common passive receive engine. RF callbacks only decode and enqueue; SPI,
 * CSV formatting and SD I/O are consumers and never run in a radio callback. */
#include "wardrive_radio.h"
#include "wardrive_decode.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#if CONFIG_BT_NIMBLE_ENABLED
#include "host/ble_hs.h"
#include "host/util/util.h"
#include <stdatomic.h>
#if CONFIG_IDF_TARGET_ESP32S3
#include "s3_wifi_owner.h"
#include "s3_ble_host.h"
#define WD_QUEUE_CAPACITY 512U
#define WD_QUEUE_MEMORY (MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)
#else
#include "wifi_scanner.h"
#include "ble_scanner.h"
#define WD_QUEUE_CAPACITY 128U
#define WD_QUEUE_MEMORY (MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)
#endif

static portMUX_TYPE wd_lock=portMUX_INITIALIZER_UNLOCKED;
static atomic_flag wd_command=ATOMIC_FLAG_INIT;
static cerberus_wd_ring_t wd_ring;
static cerberus_wd_status_t wd_status;
static uint64_t wd_retired_session;
static uint32_t wd_last_contact,wd_epoch;
static bool wd_worker,wd_stop,wd_accepting;
/* A resume received before the worker handles pause must still cross the RF
 * shutdown barrier. Otherwise a previous BLE callback keeps its old epoch. */
static bool wd_pause_requested,wd_pause_pending;
static bool wd_promiscuous,wd_ble_active;
#if CONFIG_IDF_TARGET_ESP32S3
static bool wd_wifi_initialized,wd_wifi_started,wd_wifi_callback;
static bool wd_wifi_claimed;
static cerberus_s3_ble_host_lease_t wd_ble_lease;
#else
static bool wd_wifi_saved;
static wifi_band_mode_t wd_previous_band;
static wifi_second_chan_t wd_previous_second;
static uint8_t wd_previous_channel;
static wifi_promiscuous_filter_t wd_previous_filter;
#endif
static uint32_t wd_now(void){return (uint32_t)(esp_timer_get_time()/1000LL);}
static void wd_error(esp_err_t e)
{
    portENTER_CRITICAL(&wd_lock);
    if(wd_status.error==ESP_OK)wd_status.error=e;
    if(e!=ESP_OK)wd_stop=true;
    portEXIT_CRITICAL(&wd_lock);
}
bool cerberus_wardrive_radio_held(void)
{
    portENTER_CRITICAL(&wd_lock);bool held=wd_status.held!=0;portEXIT_CRITICAL(&wd_lock);return held;
}
static bool wd_cancelled(void)
{
    uint32_t now=wd_now();portENTER_CRITICAL(&wd_lock);
    if((uint32_t)(now-wd_last_contact)>=CERBERUS_WD_LEASE_MS){
        if(wd_status.error==ESP_OK)wd_status.error=ESP_ERR_TIMEOUT;
        wd_stop=true;
    }
    bool stop=wd_stop;portEXIT_CRITICAL(&wd_lock);return stop;
}
static bool wd_pause_needed(void)
{
    portENTER_CRITICAL(&wd_lock);
    bool pause=wd_pause_requested||wd_pause_pending;
    portEXIT_CRITICAL(&wd_lock);return pause;
}
static void wd_receiving(uint8_t phase,uint8_t channel)
{
    portENTER_CRITICAL(&wd_lock);
    if(!wd_stop&&!wd_pause_requested&&!wd_pause_pending){
        wd_status.phase=phase;wd_status.channel=channel;
        wd_status.state=CERBERUS_WD_RUNNING;wd_accepting=true;
    }
    portEXIT_CRITICAL(&wd_lock);
}
static void wd_enqueue(const cerberus_wd_record_t *row,uint32_t epoch)
{
    portENTER_CRITICAL(&wd_lock);
    if(wd_accepting&&!wd_stop&&wd_epoch==epoch&&wd_status.phase==row->kind){
        if(row->kind==1)wd_status.wifi_reports=wd_status.wifi_reports==UINT32_MAX?UINT32_MAX:wd_status.wifi_reports+1U;
        else wd_status.ble_reports=wd_status.ble_reports==UINT32_MAX?UINT32_MAX:wd_status.ble_reports+1U;
        (void)cerberus_wd_ring_push(&wd_ring,row);
        if(wd_ring.next_sequence==UINT32_MAX){wd_status.error=ESP_ERR_INVALID_STATE;wd_stop=true;}
    }
    portEXIT_CRITICAL(&wd_lock);
}
void cerberus_wardrive_radio_wifi_rx(void *buffer,int type)
{
    if(!buffer||type!=WIFI_PKT_MGMT)return;
    portENTER_CRITICAL(&wd_lock);
    uint32_t epoch=wd_epoch;bool receiving=wd_accepting&&!wd_stop&&wd_status.phase==CERBERUS_WD_PHASE_WIFI;
    portEXIT_CRITICAL(&wd_lock);if(!receiving)return;
    const wifi_promiscuous_pkt_t *packet=buffer;
    if(packet->rx_ctrl.rx_state||packet->rx_ctrl.sig_len<4U)return;
    cerberus_wd_record_t row;
    if(!cerberus_wd_wifi_decode(packet->payload,packet->rx_ctrl.sig_len-4U,
        packet->rx_ctrl.channel,packet->rx_ctrl.rssi,wd_now(),&row))return;
    wd_enqueue(&row,epoch);
}
#if CONFIG_IDF_TARGET_ESP32S3
static void wd_wifi_rx(void *buffer,wifi_promiscuous_pkt_type_t type)
{cerberus_wardrive_radio_wifi_rx(buffer,(int)type);}
#endif
static int wd_gap(struct ble_gap_event *event,void *arg)
{
    uint32_t epoch=(uint32_t)(uintptr_t)arg;cerberus_wd_record_t row;
    switch(event->type){
    case BLE_GAP_EVENT_DISC:
        if(cerberus_wd_ble_decode(event->disc.data,event->disc.length_data,event->disc.addr.val,
            event->disc.addr.type,event->disc.rssi,wd_now(),&row))wd_enqueue(&row,epoch);
        break;
#if MYNEWT_VAL(BLE_EXT_ADV)
    case BLE_GAP_EVENT_EXT_DISC:
        /* Incomplete extended fragments are not complete AD field streams. */
        if(event->ext_disc.data_status==0&&cerberus_wd_ble_decode(event->ext_disc.data,
            event->ext_disc.length_data,event->ext_disc.addr.val,event->ext_disc.addr.type,
            event->ext_disc.rssi,wd_now(),&row))wd_enqueue(&row,epoch);
        break;
#endif
    default:break;
    }
    return 0;
}
static esp_err_t wd_ble_begin(void)
{
    portENTER_CRITICAL(&wd_lock);uint32_t epoch=wd_epoch;portEXIT_CRITICAL(&wd_lock);
    uint8_t address_type=0;int rc=ble_hs_id_infer_auto(0,&address_type);
    if(rc)return ESP_ERR_INVALID_STATE;
#if CONFIG_IDF_TARGET_ESP32C5 && MYNEWT_VAL(BLE_EXT_ADV)
    /* C5 covers 1M and coded PHY. S3 adds an independent continuous 1M receiver. */
    const struct ble_gap_ext_disc_params p={.itvl=160,.window=80,.passive=1};
    rc=ble_gap_ext_disc(address_type,0,0,0,0,0,&p,&p,wd_gap,(void *)(uintptr_t)epoch);
#else
    const struct ble_gap_disc_params p={.itvl=64,.window=64,.passive=1,.filter_duplicates=0};
    rc=ble_gap_disc(address_type,BLE_HS_FOREVER,&p,wd_gap,(void *)(uintptr_t)epoch);
#endif
    if(rc)return ESP_FAIL;
    wd_ble_active=true;return ESP_OK;
}
static esp_err_t wd_ble_end(void)
{
    if(!wd_ble_active)return ESP_OK;
    int rc=ble_gap_disc_cancel();
    if(rc!=0&&rc!=BLE_HS_EALREADY)return ESP_FAIL;
    if(ble_gap_disc_active())return ESP_ERR_NOT_FINISHED;
    wd_ble_active=false;return ESP_OK;
}
static esp_err_t wd_quiet(void)
{
    esp_err_t e=wd_ble_end();if(e!=ESP_OK)return e;
    if(wd_promiscuous){e=esp_wifi_set_promiscuous(false);if(e!=ESP_OK)return e;wd_promiscuous=false;}
    return ESP_OK;
}
static esp_err_t wd_pause_wait(void)
{
    /* Keep the initialized drivers, leases and FIFO. Pausing neither publishes
     * a completed recording nor lets another operation claim either radio. */
    esp_err_t e;
    while((e=wd_quiet())==ESP_ERR_NOT_FINISHED&&!wd_cancelled())
        vTaskDelay(pdMS_TO_TICKS(20U)?pdMS_TO_TICKS(20U):1U);
    if(e==ESP_ERR_NOT_FINISHED&&wd_cancelled())return ESP_OK;
    if(e!=ESP_OK)return e;
    portENTER_CRITICAL(&wd_lock);
    wd_pause_pending=false;wd_status.phase=CERBERUS_WD_PHASE_NONE;wd_status.channel=0;
    if(!wd_stop)wd_status.state=CERBERUS_WD_PAUSED;
    portEXIT_CRITICAL(&wd_lock);
    while(!wd_cancelled()&&wd_pause_needed())
        vTaskDelay(pdMS_TO_TICKS(20U)?pdMS_TO_TICKS(20U):1U);
    return ESP_OK;
}
static esp_err_t wd_prepare(uint8_t mode)
{
    esp_err_t e=ESP_OK;
#if CONFIG_IDF_TARGET_ESP32S3
    /* Reserve Wi-Fi even in BLE-only mode so another UI task cannot admit a
     * competing S3 radio operation between host claim and GAP start. */
    e=cerberus_s3_wifi_claim(CERBERUS_S3_WIFI_WARDRIVE);if(e!=ESP_OK)return e;wd_wifi_claimed=true;
    if(mode!=CERBERUS_WD_WIFI){e=cerberus_s3_ble_host_claim(CERBERUS_S3_BLE_OWNER_WARDRIVE,&wd_ble_lease);if(e!=ESP_OK)return e;}
    else if(cerberus_s3_ble_host_owner()!=CERBERUS_S3_BLE_OWNER_NONE)return ESP_ERR_INVALID_STATE;
    if(mode!=CERBERUS_WD_BLE){
        e=cerberus_s3_wifi_prepare(CERBERUS_S3_WIFI_WARDRIVE);if(e!=ESP_OK)return e;
        wifi_init_config_t init=WIFI_INIT_CONFIG_DEFAULT();
        init.static_rx_buf_num=4;init.dynamic_rx_buf_num=8;init.dynamic_tx_buf_num=2;
        init.rx_ba_win=4;init.ampdu_rx_enable=0;init.ampdu_tx_enable=0;init.amsdu_tx_enable=0;
        e=esp_wifi_init(&init);if(e!=ESP_OK)return e;wd_wifi_initialized=true;
        e=esp_wifi_set_storage(WIFI_STORAGE_RAM);if(e!=ESP_OK)return e;
        e=esp_wifi_set_mode(WIFI_MODE_NULL);if(e!=ESP_OK)return e;
        e=esp_wifi_start();if(e!=ESP_OK)return e;wd_wifi_started=true;
        e=esp_wifi_set_promiscuous_rx_cb(wd_wifi_rx);if(e!=ESP_OK)return e;wd_wifi_callback=true;
    }
#else
    if(mode!=CERBERUS_WD_BLE){
        e=esp_wifi_get_band_mode(&wd_previous_band);if(e!=ESP_OK)return e;
        e=esp_wifi_get_channel(&wd_previous_channel,&wd_previous_second);if(e!=ESP_OK)return e;
        e=esp_wifi_get_promiscuous_filter(&wd_previous_filter);if(e!=ESP_OK)return e;
        wd_wifi_saved=true;
        e=esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);if(e!=ESP_OK)return e;
        e=esp_wifi_set_band(WIFI_BAND_5G);if(e!=ESP_OK)return e;
    }
#endif
    if(mode!=CERBERUS_WD_BLE){
        wifi_promiscuous_filter_t filter={.filter_mask=WIFI_PROMIS_FILTER_MASK_MGMT};
        e=esp_wifi_set_promiscuous_filter(&filter);
    }
    return e;
}
static esp_err_t wd_cleanup(void)
{
    esp_err_t e=wd_quiet();if(e!=ESP_OK)return e;
#if CONFIG_IDF_TARGET_ESP32S3
    if(wd_wifi_callback){e=esp_wifi_set_promiscuous_rx_cb(NULL);if(e!=ESP_OK)return e;wd_wifi_callback=false;}
    if(wd_wifi_started){e=esp_wifi_stop();if(e!=ESP_OK)return e;wd_wifi_started=false;}
    if(wd_wifi_initialized){e=esp_wifi_deinit();if(e!=ESP_OK)return e;wd_wifi_initialized=false;}
    if(wd_ble_lease.token){
        if(cerberus_s3_ble_host_lease_valid(&wd_ble_lease)){
            e=cerberus_s3_ble_host_release(&wd_ble_lease);if(e!=ESP_OK)return e;
        }
        memset(&wd_ble_lease,0,sizeof(wd_ble_lease));
    }
    if(wd_wifi_claimed){e=cerberus_s3_wifi_release(CERBERUS_S3_WIFI_WARDRIVE);if(e!=ESP_OK)return e;wd_wifi_claimed=false;}
#else
    if(wd_wifi_saved){
        e=esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);if(e!=ESP_OK)return e;
        if(wd_previous_channel){
            e=esp_wifi_set_band(wd_previous_channel<=14U?WIFI_BAND_2G:WIFI_BAND_5G);if(e!=ESP_OK)return e;
            e=esp_wifi_set_channel(wd_previous_channel,wd_previous_second);if(e!=ESP_OK)return e;
        }
        e=esp_wifi_set_promiscuous_filter(&wd_previous_filter);if(e!=ESP_OK)return e;
        e=esp_wifi_set_band_mode(wd_previous_band);if(e!=ESP_OK)return e;
        wd_wifi_saved=false;
    }
#endif
    return ESP_OK;
}
static void wd_wait_window(uint32_t started,uint32_t duration)
{
    /* A 5 ms delay rounds to zero with the board's 100 Hz scheduler. Always
     * block for at least one tick so GPS, SPI and the SD writer can run. */
    while(!wd_cancelled()&&!wd_pause_needed()&&(uint32_t)(wd_now()-started)<duration)
        vTaskDelay(pdMS_TO_TICKS(5U) ? pdMS_TO_TICKS(5U) : 1U);
}
static void wd_run(void *unused)
{
    (void)unused;uint8_t mode=wd_status.mode;esp_err_t e=wd_prepare(mode);
#if CONFIG_IDF_TARGET_ESP32S3
    static const uint8_t channels[]={1,6,11,2,7,3,8,4,9,5,10};
    bool mixed=mode==CERBERUS_WD_COMBINED;
#else
    static const uint8_t channels[]={36,40,44,48,149,153,157,161,165,52,56,60,64,
        100,104,108,112,116,120,124,128,132,136,140,144};
    bool mixed=false; /* C5 stays on 5 GHz in combined mode. */
#endif
    while(e==ESP_OK&&!wd_cancelled()){
        if(wd_pause_needed()){
            e=wd_pause_wait();if(e!=ESP_OK||wd_cancelled())break;
        }
        if(mode!=CERBERUS_WD_BLE){
            unsigned tuned=0;
            for(unsigned i=0;i<sizeof(channels)&&!wd_cancelled()&&!wd_pause_needed();++i){
                portENTER_CRITICAL(&wd_lock);wd_accepting=false;wd_status.channel=0;portEXIT_CRITICAL(&wd_lock);
                e=esp_wifi_set_channel(channels[i],WIFI_SECOND_CHAN_NONE);
                if(e!=ESP_OK){
                    portENTER_CRITICAL(&wd_lock);wd_status.channel_rejects=wd_status.channel_rejects==UINT32_MAX?UINT32_MAX:wd_status.channel_rejects+1U;portEXIT_CRITICAL(&wd_lock);
                    e=ESP_OK;continue; /* Driver enforces regulatory support. */
                }
                ++tuned;
                if(!wd_promiscuous){e=esp_wifi_set_promiscuous(true);if(e!=ESP_OK)break;wd_promiscuous=true;}
                wd_receiving(CERBERUS_WD_PHASE_WIFI,channels[i]);
                wd_wait_window(wd_now(),CERBERUS_WD_DWELL_MS);
            }
            if(!tuned&&!wd_cancelled()&&!wd_pause_needed())e=ESP_ERR_NOT_SUPPORTED;
            if(e==ESP_OK&&!wd_cancelled()&&!wd_pause_needed()){
                portENTER_CRITICAL(&wd_lock);wd_status.valid_channels=(uint8_t)tuned;
                wd_status.wifi_sweeps=wd_status.wifi_sweeps==UINT32_MAX?UINT32_MAX:wd_status.wifi_sweeps+1U;portEXIT_CRITICAL(&wd_lock);
            }
        }
        if(e==ESP_OK&&!wd_cancelled()&&!wd_pause_needed()&&(mixed||mode==CERBERUS_WD_BLE)){
            portENTER_CRITICAL(&wd_lock);wd_accepting=false;wd_status.channel=0;portEXIT_CRITICAL(&wd_lock);
            if(wd_promiscuous){e=esp_wifi_set_promiscuous(false);if(e!=ESP_OK)break;wd_promiscuous=false;}
            e=wd_ble_begin();if(e!=ESP_OK)break;
            wd_receiving(CERBERUS_WD_PHASE_BLE,0);
            uint32_t began=wd_now(),accounted=began;
            do{
                wd_wait_window(wd_now(),mixed?CERBERUS_WD_BLE_WINDOW_MS:100U);
                uint32_t now=wd_now();portENTER_CRITICAL(&wd_lock);
                uint32_t elapsed=now-accounted;
                wd_status.ble_ms=UINT32_MAX-wd_status.ble_ms<elapsed?UINT32_MAX:wd_status.ble_ms+elapsed;
                portEXIT_CRITICAL(&wd_lock);accounted=now;
                if(!wd_cancelled()&&!wd_pause_needed()&&!ble_gap_disc_active()){e=ESP_FAIL;break;}
            }while(!mixed&&!wd_cancelled()&&!wd_pause_needed());
            (void)began;
            portENTER_CRITICAL(&wd_lock);wd_accepting=false;portEXIT_CRITICAL(&wd_lock);
            if(e==ESP_OK&&!wd_pause_needed())e=wd_ble_end();
        }
    }
    wd_error(e);
    portENTER_CRITICAL(&wd_lock);wd_accepting=false;wd_status.state=CERBERUS_WD_STOPPING;wd_status.phase=0;wd_status.channel=0;
    portEXIT_CRITICAL(&wd_lock);
    while((e=wd_cleanup())!=ESP_OK){wd_error(e);vTaskDelay(pdMS_TO_TICKS(50));}
    portENTER_CRITICAL(&wd_lock);wd_status.state=CERBERUS_WD_STOPPED;wd_status.held=0;wd_worker=false;portEXIT_CRITICAL(&wd_lock);
    vTaskDelete(NULL);
}
esp_err_t cerberus_wardrive_radio_exchange(uint8_t command,const cerberus_wd_request_t *q,void *reply)
{
    if(!q||!reply||!q->session||q->reserved[0]||q->reserved[1]||q->mode>CERBERUS_WD_BLE||q->discard>1||
        command<CERBERUS_WD_START||command>CERBERUS_WD_RESUME)return ESP_ERR_INVALID_ARG;
    memset(reply,0,54U);
    if(atomic_flag_test_and_set_explicit(&wd_command,memory_order_acquire))return ESP_ERR_NOT_FINISHED;
    esp_err_t e=ESP_OK;cerberus_wd_record_t *dispose=NULL;
    if(command==CERBERUS_WD_START){
        portENTER_CRITICAL(&wd_lock);bool same=wd_status.session==q->session;
        bool available=!wd_status.session&&!wd_worker&&q->session!=wd_retired_session&&wd_epoch!=UINT32_MAX;
        portEXIT_CRITICAL(&wd_lock);
        if(!same&&!available)e=ESP_ERR_INVALID_STATE;
        if(e==ESP_OK&&!same){
#if !CONFIG_IDF_TARGET_ESP32S3
            if(!cerberus_wifi_wardrive_ready()||!cerberus_ble_wardrive_ready())e=ESP_ERR_INVALID_STATE;
#endif
            cerberus_wd_record_t *rows=e==ESP_OK?heap_caps_calloc(WD_QUEUE_CAPACITY,sizeof(*rows),WD_QUEUE_MEMORY):NULL;
            if(e==ESP_OK&&!rows)e=ESP_ERR_NO_MEM;
            if(e==ESP_OK){
                portENTER_CRITICAL(&wd_lock);memset(&wd_status,0,sizeof(wd_status));memset(&wd_ring,0,sizeof(wd_ring));
                wd_status.session=q->session;wd_status.mode=q->mode;wd_status.state=CERBERUS_WD_STARTING;wd_status.held=1;
                wd_ring.rows=rows;wd_ring.capacity=WD_QUEUE_CAPACITY;wd_stop=false;wd_accepting=false;
                wd_pause_requested=false;wd_pause_pending=false;
                wd_last_contact=wd_now();++wd_epoch;wd_worker=true;portEXIT_CRITICAL(&wd_lock);
                if(xTaskCreate(wd_run,"wd_radio",4096,NULL,4,NULL)!=pdPASS){
                    portENTER_CRITICAL(&wd_lock);wd_worker=false;wd_status.held=0;wd_status.state=CERBERUS_WD_STOPPED;
                    wd_status.error=ESP_ERR_NO_MEM;portEXIT_CRITICAL(&wd_lock);e=ESP_ERR_NO_MEM;
                }
            }
        }
    }
    portENTER_CRITICAL(&wd_lock);
    if(e==ESP_OK&&wd_status.session!=q->session)e=ESP_ERR_INVALID_STATE;
    if(e==ESP_OK){
        wd_last_contact=wd_now();
        if(!cerberus_wd_ring_ack(&wd_ring,q->ack))e=ESP_ERR_INVALID_ARG;
        else if(command==CERBERUS_WD_STOP){wd_stop=true;wd_accepting=false;if(wd_worker)wd_status.state=CERBERUS_WD_STOPPING;}
        else if(command==CERBERUS_WD_PAUSE||command==CERBERUS_WD_RESUME){
            if(!wd_worker||wd_stop)e=ESP_ERR_INVALID_STATE;
            else if(command==CERBERUS_WD_RESUME)wd_pause_requested=false;
            else if(!wd_pause_requested){
                if(!wd_pause_pending){
                    if(wd_epoch==UINT32_MAX){wd_status.error=e=ESP_ERR_INVALID_STATE;wd_stop=true;}
                    else {++wd_epoch;wd_pause_pending=true;}
                }
                wd_pause_requested=true;wd_accepting=false;
                if(!wd_stop)wd_status.state=CERBERUS_WD_PAUSING;
            }
        }
        else if(command==CERBERUS_WD_RELEASE){
            if(wd_worker||wd_status.held||(wd_ring.count&&!q->discard))e=ESP_ERR_INVALID_STATE;
            else {dispose=wd_ring.rows;wd_ring.rows=NULL;wd_ring.count=0;}
        }
    }
    if(command==CERBERUS_WD_READ&&e==ESP_OK)(void)cerberus_wd_ring_read(&wd_ring,wd_now(),reply);
    else if(command==CERBERUS_WD_READ)((cerberus_wd_record_t *)reply)->kind=255U;
    else if(command!=CERBERUS_WD_READ){
        cerberus_wd_status_t *out=reply;*out=wd_status;out->session=q->session;
        out->pending=wd_ring.count;out->high_water=wd_ring.high_water;out->last_ack=wd_ring.last_ack;
        out->dropped=wd_ring.dropped;
        if(wd_status.session!=q->session){memset(out,0,sizeof(*out));out->session=q->session;}
        if(e!=ESP_OK)out->error=e;
    }
    if(dispose){wd_retired_session=wd_status.session;wd_status.session=0;}
    portEXIT_CRITICAL(&wd_lock);heap_caps_free(dispose);
    atomic_flag_clear_explicit(&wd_command,memory_order_release);return e;
}
#else
/* The temporary USB bridge has Bluetooth disabled and never runs a survey. */
bool cerberus_wardrive_radio_held(void){return false;}
void cerberus_wardrive_radio_wifi_rx(void *buffer,int type){(void)buffer;(void)type;}
esp_err_t cerberus_wardrive_radio_exchange(uint8_t command,const cerberus_wd_request_t *query,void *reply)
{(void)command;(void)query;if(reply)memset(reply,0,54U);return ESP_ERR_NOT_SUPPORTED;}
#endif
