#include "s3_wifi_packet_count.h"
#include "s3_wifi_owner.h"
#include "s3_ble_host.h"
#include "wifi_packet_count_protocol.h"
#include "wifi_capture_ring.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <string.h>

typedef struct {
    cerberus_packet_count_begin_t request;
    cerberus_wifi_packet_count_target_t targets[CERBERUS_WIFI_PACKET_COUNT_LIMIT];
    cerberus_wifi_packet_count_table_t table;
    cerberus_wifi_capture_ring_t *raw;
    uint8_t pin_bytes[CERBERUS_PACKET_COUNT_SUMMARY_BYTES];
    uint8_t raw_pin_bytes[CERBERUS_PACKET_COUNT_RAW_META_BYTES];
    uint64_t deadline_us,clock_floor;
    uint32_t pin_counter,pin,raw_pin,last_ack,acked,inflight;
    uint16_t configured,pin_size;
    uint8_t state,channel,next_channel;
    esp_err_t error;
    bool held,accepting,stop,release,data_ready,initialized,started,callback,disable_pending,worker;
} s3_packet_count_context_t;

static portMUX_TYPE count_lock=portMUX_INITIALIZER_UNLOCKED;
static s3_packet_count_context_t *count_context;
/* A local caller resets with the S3; this ordinal never repeats in this boot. */
static uint64_t count_epoch=1U;
static bool count_launching,count_command;
static atomic_uint count_admission;
static uint32_t count_admission_counter;

static void count_first_error(s3_packet_count_context_t *c,esp_err_t error)
{if(c->error==ESP_OK&&error!=ESP_OK)c->error=error;}
static void count_close_window_locked(s3_packet_count_context_t *c,int64_t now)
{
    c->accepting=false;c->channel=0;
    atomic_store_explicit(&count_admission,0U,memory_order_release);
    if(c->raw)cerberus_wifi_capture_ring_stop(c->raw);
    if(c->data_ready&&c->table.window_open)(void)cerberus_wifi_packet_count_window_end(&c->table,now);
}
static void count_stop_locked(s3_packet_count_context_t *c,int64_t now)
{
    c->stop=true;count_close_window_locked(c,now);
    if(c->held)c->state=CERBERUS_PACKET_COUNT_STOPPING;
}
static bool count_clock_locked(s3_packet_count_context_t *c,int64_t now)
{
    if(now<0||now>INT64_MAX-(int64_t)CERBERUS_PACKET_COUNT_RETENTION_MS*1000||(uint64_t)now<c->clock_floor){
        c->table.summary.flags|=CERBERUS_WIFI_PACKET_COUNT_CLOCK_INVALID;
        count_first_error(c,ESP_ERR_INVALID_STATE);count_stop_locked(c,now);return false;
    }
    c->clock_floor=(uint64_t)now;return true;
}
static void count_expire_locked(s3_packet_count_context_t *c,int64_t now)
{
    if(!count_clock_locked(c,now))return;
    if((uint64_t)now>=c->deadline_us){
        if(c->held){count_first_error(c,ESP_ERR_TIMEOUT);count_stop_locked(c,now);}
        else c->release=true;
    }
}
static void count_control_locked(const s3_packet_count_context_t *c,cerberus_packet_count_control_t *out)
{
    memset(out,0,sizeof(*out));out->session_id=c->request.session_id;
    out->sequence=c->table.summary.revision;out->error_code=c->error;
    out->configured=c->configured;out->state=c->state;out->channel=c->channel;out->count=c->request.target_count;
    out->flags=CERBERUS_PACKET_COUNT_BOUND;
    if(c->held)out->flags|=CERBERUS_PACKET_COUNT_HELD;
    if(c->accepting)out->flags|=CERBERUS_PACKET_COUNT_RX_ACTIVE;
    if(c->data_ready&&!c->release)out->flags|=CERBERUS_PACKET_COUNT_DATA_READY;
    if(c->request.record)out->flags|=CERBERUS_PACKET_COUNT_RECORDING;
    if(c->table.summary.flags&CERBERUS_WIFI_PACKET_COUNT_CLOCK_INVALID)out->flags|=CERBERUS_PACKET_COUNT_CLOCK_INVALID;
    if(c->table.summary.flags&CERBERUS_WIFI_PACKET_COUNT_SATURATED)out->flags|=CERBERUS_PACKET_COUNT_SATURATED;
    if(c->pin_counter>=CERBERUS_PACKET_COUNT_SEQUENCE_MAX-1U||c->table.summary.revision==UINT32_MAX||
       (c->raw&&atomic_load_explicit(&c->raw->next_sequence,memory_order_acquire)>=CERBERUS_PACKET_COUNT_SEQUENCE_MAX))
        out->flags|=CERBERUS_PACKET_COUNT_EXHAUSTED;
}
static void count_raw_stats(const s3_packet_count_context_t *c,cerberus_packet_count_record_stats_t *out)
{
    memset(out,0,sizeof(*out));if(!c->raw)return;
    cerberus_wifi_capture_ring_stats_t s;cerberus_wifi_capture_ring_stats(c->raw,&s);
    out->accepted=s.accepted;out->acked=c->acked;out->pending=s.pending;out->dropped_full=s.dropped_full;
    uint64_t other=(uint64_t)s.dropped_stopped+s.dropped_stale+s.dropped_oversize+s.dropped_truncated+s.dropped_invalid;
    out->dropped_other=other>UINT32_MAX?UINT32_MAX:(uint32_t)other;
    out->last_sequence=atomic_load_explicit(&c->raw->next_sequence,memory_order_acquire);
}
static void count_receive(void *buffer,wifi_promiscuous_pkt_type_t type)
{
    uint32_t admission=atomic_load_explicit(&count_admission,memory_order_acquire);
    int64_t entered=esp_timer_get_time();
    if(!buffer||!admission)return;
    const wifi_promiscuous_pkt_t *packet=buffer;
    portENTER_CRITICAL(&count_lock);
    s3_packet_count_context_t *c=count_context;
    if(!c||!c->accepting||c->stop||admission!=atomic_load_explicit(&count_admission,memory_order_acquire)||
       !count_clock_locked(c,esp_timer_get_time())){
        portEXIT_CRITICAL(&count_lock);return;
    }
    if(entered<0){(void)count_clock_locked(c,entered);portEXIT_CRITICAL(&count_lock);return;}
    if((uint64_t)entered<c->table.window_started_us){portEXIT_CRITICAL(&count_lock);return;}
    ++c->inflight;
    unsigned kind=type==WIFI_PKT_MGMT?CERBERUS_WIFI_PACKET_COUNT_MGMT:
        type==WIFI_PKT_DATA?CERBERUS_WIFI_PACKET_COUNT_DATA:CERBERUS_WIFI_PACKET_COUNT_OTHER;
    size_t length=packet->rx_ctrl.sig_len>=4U?packet->rx_ctrl.sig_len-4U:0U;
    bool metadata=packet->rx_ctrl.rx_state==0U&&packet->rx_ctrl.sig_len>=4U;
    uint16_t matched=cerberus_wifi_packet_count_observe(&c->table,packet->payload,length,kind,
        metadata,packet->rx_ctrl.channel,packet->rx_ctrl.rssi,entered);
    if((c->table.summary.flags&CERBERUS_WIFI_PACKET_COUNT_CLOCK_INVALID)||c->table.summary.revision==UINT32_MAX){count_first_error(c,ESP_ERR_INVALID_STATE);count_stop_locked(c,entered);}
    cerberus_wifi_capture_ring_t *raw=matched?c->raw:NULL;
    bool exhausted=raw&&atomic_load_explicit(&raw->next_sequence,memory_order_acquire)>=CERBERUS_PACKET_COUNT_SEQUENCE_MAX;
    if(exhausted){count_first_error(c,ESP_ERR_INVALID_STATE);count_stop_locked(c,entered);raw=NULL;}
    portEXIT_CRITICAL(&count_lock);
    if(raw){
        cerberus_wifi_pcap_metadata_t meta={.channel=packet->rx_ctrl.channel,
            .rssi_dbm=packet->rx_ctrl.rssi,.rssi_known=packet->rx_ctrl.rssi>=-127&&packet->rx_ctrl.rssi<=0,
            .time_kind=CERBERUS_WIFI_PCAP_TIME_MONOTONIC,.capture_time_us=(uint64_t)entered};
        (void)cerberus_wifi_capture_ring_push(raw,1U,&meta,packet->payload,length,length);
    }
    portENTER_CRITICAL(&count_lock);--c->inflight;portEXIT_CRITICAL(&count_lock);
}
static bool count_cancelled(s3_packet_count_context_t *c)
{
    portENTER_CRITICAL(&count_lock);count_expire_locked(c,esp_timer_get_time());
    if(!c->stop&&(c->table.summary.revision==UINT32_MAX||
       (c->raw&&atomic_load_explicit(&c->raw->next_sequence,memory_order_acquire)>=CERBERUS_PACKET_COUNT_SEQUENCE_MAX))){
        count_first_error(c,ESP_ERR_INVALID_STATE);count_stop_locked(c,esp_timer_get_time());}
    bool stop=c->stop;portEXIT_CRITICAL(&count_lock);return stop;
}
/* SDK calls remain outside critical sections. Each failed cleanup step stays
 * pending and is retried by this same worker; no later run inherits it. */
static esp_err_t count_cleanup(s3_packet_count_context_t *c)
{
    if(cerberus_s3_wifi_owner()!=CERBERUS_S3_WIFI_COUNT)return ESP_ERR_INVALID_STATE;
    esp_err_t e;
    if(c->disable_pending){e=esp_wifi_set_promiscuous(false);if(e!=ESP_OK)return e;c->disable_pending=false;}
    if(c->callback){e=esp_wifi_set_promiscuous_rx_cb(NULL);if(e!=ESP_OK)return e;c->callback=false;}
    if(c->started){e=esp_wifi_stop();if(e!=ESP_OK&&e!=ESP_ERR_WIFI_NOT_STARTED)return e;c->started=false;}
    if(c->initialized){e=esp_wifi_deinit();if(e!=ESP_OK)return e;c->initialized=false;}
    return cerberus_s3_wifi_release(CERBERUS_S3_WIFI_COUNT);
}
static void count_worker(void *arg)
{
    s3_packet_count_context_t *c=arg;
    for(;;){
        portENTER_CRITICAL(&count_lock);count_expire_locked(c,esp_timer_get_time());
        bool waiting=c->state==CERBERUS_PACKET_COUNT_STAGING&&!c->stop;
        portEXIT_CRITICAL(&count_lock);
        if(!waiting){
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    esp_err_t e=ESP_OK;
    if(!count_cancelled(c))e=cerberus_s3_wifi_prepare(CERBERUS_S3_WIFI_COUNT);
    wifi_init_config_t init=WIFI_INIT_CONFIG_DEFAULT();
    init.static_rx_buf_num=4;init.dynamic_rx_buf_num=8;init.dynamic_tx_buf_num=4;
    init.rx_ba_win=4;init.ampdu_rx_enable=0;init.ampdu_tx_enable=0;init.amsdu_tx_enable=0;
    if(e==ESP_OK&&!count_cancelled(c)){e=esp_wifi_init(&init);c->initialized=e==ESP_OK;}
    if(e==ESP_OK&&c->initialized&&!count_cancelled(c))e=esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if(e==ESP_OK&&c->initialized&&!count_cancelled(c))e=esp_wifi_set_mode(WIFI_MODE_NULL);
    if(e==ESP_OK&&c->initialized&&!count_cancelled(c)){e=esp_wifi_start();c->started=e==ESP_OK;}
    wifi_promiscuous_filter_t filter={.filter_mask=WIFI_PROMIS_FILTER_MASK_MGMT|WIFI_PROMIS_FILTER_MASK_DATA};
    if(e==ESP_OK&&c->started&&!count_cancelled(c))e=esp_wifi_set_promiscuous_filter(&filter);
    if(e==ESP_OK&&c->started&&!count_cancelled(c)){e=esp_wifi_set_promiscuous_rx_cb(count_receive);c->callback=e==ESP_OK;}
    unsigned failures=0,planned=0;
    for(unsigned i=0;i<14U;++i)planned+=(c->request.channel_mask&(UINT64_C(1)<<i))!=0U;
    while(e==ESP_OK&&c->callback&&!count_cancelled(c)){
        unsigned chosen=0;
        for(unsigned i=0;i<14U;++i){unsigned index=(c->next_channel+i)%14U;
            if(c->request.channel_mask&(UINT64_C(1)<<index)){chosen=index;break;}}
        c->next_channel=(uint8_t)((chosen+1U)%14U);uint8_t channel=cerberus_wifi_packet_count_channel(chosen);
        portENTER_CRITICAL(&count_lock);
        c->accepting=false;c->channel=0;
        atomic_store_explicit(&count_admission,0U,memory_order_release);
        if(c->table.window_open)(void)cerberus_wifi_packet_count_window_end(&c->table,esp_timer_get_time());
        portEXIT_CRITICAL(&count_lock);
        e=esp_wifi_set_channel(channel,WIFI_SECOND_CHAN_NONE);
        portENTER_CRITICAL(&count_lock);cerberus_wifi_packet_count_attempt(&c->table,channel,e==ESP_OK);
        if(c->table.summary.revision==UINT32_MAX)e=ESP_ERR_INVALID_STATE;
        if(e!=ESP_OK)count_first_error(c,e);
        portEXIT_CRITICAL(&count_lock);
        if(e!=ESP_OK){
            if(++failures>=planned)break;
            e=ESP_OK;vTaskDelay(pdMS_TO_TICKS(25));continue;
        }
        failures=0;
        if(!c->disable_pending&&!count_cancelled(c)){c->disable_pending=true;e=esp_wifi_set_promiscuous(true);}
        portENTER_CRITICAL(&count_lock);
        int64_t now=esp_timer_get_time();
        if(e==ESP_OK&&!c->stop&&count_clock_locked(c,now)){
            if(count_admission_counter!=UINT32_MAX&&cerberus_wifi_packet_count_window_begin(&c->table,channel,now)){
                c->channel=channel;c->accepting=true;c->state=CERBERUS_PACKET_COUNT_RUNNING;
                atomic_store_explicit(&count_admission,++count_admission_counter,memory_order_release);
            }else e=ESP_ERR_INVALID_STATE;
        }
        uint64_t until=now>=0?(uint64_t)now+(uint64_t)c->request.dwell_ms*1000U:0U;
        portEXIT_CRITICAL(&count_lock);
        while(e==ESP_OK&&!count_cancelled(c)){
            int64_t sample=esp_timer_get_time();
            bool hopping=(c->request.channel_mask&(c->request.channel_mask-1U))!=0U;
            if(sample<0||(hopping&&(uint64_t)sample>=until))break;
            vTaskDelay(pdMS_TO_TICKS(25));
        }
    }
    portENTER_CRITICAL(&count_lock);count_first_error(c,e);count_stop_locked(c,esp_timer_get_time());portEXIT_CRITICAL(&count_lock);
    for(;;){
        portENTER_CRITICAL(&count_lock);bool inflight=c->inflight!=0;portEXIT_CRITICAL(&count_lock);
        if(!inflight){
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    do{
        e=count_cleanup(c);
        if(e!=ESP_OK){portENTER_CRITICAL(&count_lock);count_first_error(c,e);portEXIT_CRITICAL(&count_lock);vTaskDelay(pdMS_TO_TICKS(250));}
    }while(e!=ESP_OK);
    portENTER_CRITICAL(&count_lock);
    int64_t now=esp_timer_get_time();
    if(c->data_ready)cerberus_wifi_packet_count_finish(&c->table,now);
    c->held=false;c->channel=0;c->state=c->error==ESP_OK?CERBERUS_PACKET_COUNT_COMPLETE:CERBERUS_PACKET_COUNT_ERROR;
    c->deadline_us=now>=0?(uint64_t)now+(uint64_t)CERBERUS_PACKET_COUNT_RETENTION_MS*1000U:c->clock_floor;
    portEXIT_CRITICAL(&count_lock);
    for(;;){
        portENTER_CRITICAL(&count_lock);count_expire_locked(c,esp_timer_get_time());
        bool dispose=c->release&&!count_command&&!c->inflight;
        if(dispose)count_context=NULL;
        portEXIT_CRITICAL(&count_lock);
        if(dispose){
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    heap_caps_free(c->raw);heap_caps_free(c);vTaskDelete(NULL);
}
static bool count_begin_valid(const cerberus_packet_count_begin_t *b)
{return b&&b->session_id&&b->target_count&&b->target_count<=CERBERUS_WIFI_PACKET_COUNT_LIMIT&&
    b->channel_mask&&!(b->channel_mask>>14U)&&b->dwell_ms>=CERBERUS_WIFI_PACKET_COUNT_DWELL_MIN_MS&&
    b->dwell_ms<=CERBERUS_WIFI_PACKET_COUNT_DWELL_MAX_MS&&b->record<=1U&&!b->reserved;}
static esp_err_t count_begin(const cerberus_packet_count_begin_t *b,cerberus_packet_count_control_t *reply)
{
    if(!count_begin_valid(b))return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&count_lock);
    if(count_context){
        s3_packet_count_context_t *c=count_context;
        bool same=b->session_id==c->request.session_id;
        if(same)count_control_locked(c,reply);
        bool exact=same&&!memcmp(b,&c->request,sizeof(*b));
        portEXIT_CRITICAL(&count_lock);return exact?ESP_OK:ESP_ERR_INVALID_STATE;
    }
    if(count_launching||count_epoch==UINT64_MAX||b->session_id!=count_epoch+1U){
        portEXIT_CRITICAL(&count_lock);return b->session_id==count_epoch?ESP_ERR_NOT_FOUND:ESP_ERR_INVALID_STATE;}
    count_launching=true;portEXIT_CRITICAL(&count_lock);
    /* Allocate before reserving the radio, so allocation failure owns nothing.
     * The launch flag prevents another COUNT caller from racing admission. */
    esp_err_t e=ESP_OK;bool held=false;
    if(cerberus_s3_ble_host_owner()!=CERBERUS_S3_BLE_OWNER_NONE)e=ESP_ERR_INVALID_STATE;
    if(e==ESP_OK&&heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)<65536U)e=ESP_ERR_NO_MEM;
    s3_packet_count_context_t *c=NULL;
    if(e==ESP_OK){c=heap_caps_calloc(1,sizeof(*c),MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);if(!c)e=ESP_ERR_NO_MEM;}
    if(e==ESP_OK&&b->record){c->raw=heap_caps_calloc(1,sizeof(*c->raw),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);if(!c->raw)e=ESP_ERR_NO_MEM;}
    int64_t now=esp_timer_get_time();
    if(e==ESP_OK&&(now<0||now>INT64_MAX-(int64_t)CERBERUS_PACKET_COUNT_RETENTION_MS*1000))e=ESP_ERR_INVALID_STATE;
    if(e==ESP_OK){e=cerberus_s3_wifi_claim(CERBERUS_S3_WIFI_COUNT);held=e==ESP_OK;}
    if(e==ESP_OK&&cerberus_s3_ble_host_owner()!=CERBERUS_S3_BLE_OWNER_NONE)e=ESP_ERR_INVALID_STATE;
    if(e!=ESP_OK){
        if(held){esp_err_t release=cerberus_s3_wifi_release(CERBERUS_S3_WIFI_COUNT);held=release!=ESP_OK;if(held)e=release;}
        if(!held){
            if(c){heap_caps_free(c->raw);heap_caps_free(c);}
            portENTER_CRITICAL(&count_lock);count_launching=false;portEXIT_CRITICAL(&count_lock);return e;
        }
    }
    c->request=*b;c->state=CERBERUS_PACKET_COUNT_STAGING;c->held=true;c->clock_floor=(uint64_t)now;
    c->deadline_us=(uint64_t)now+(uint64_t)CERBERUS_PACKET_COUNT_LEASE_MS*1000U;
    if(c->raw)cerberus_wifi_capture_ring_init(c->raw);
    portENTER_CRITICAL(&count_lock);count_context=c;count_epoch=b->session_id;count_launching=false;portEXIT_CRITICAL(&count_lock);
    c->worker=e==ESP_OK;
    if(e!=ESP_OK||xTaskCreate(count_worker,"s3_wifi_count",4096,c,3,NULL)!=pdPASS){
        c->worker=false;
        e=cerberus_s3_wifi_release(CERBERUS_S3_WIFI_COUNT);
        if(e==ESP_OK){
            portENTER_CRITICAL(&count_lock);count_context=NULL;portEXIT_CRITICAL(&count_lock);
            heap_caps_free(c->raw);heap_caps_free(c);return ESP_ERR_NO_MEM;
        }
        /* This owner-only release cannot normally fail without foreign state;
         * retain the bound context rather than claiming a clean rejection. */
        portENTER_CRITICAL(&count_lock);c->error=e;c->stop=true;c->state=CERBERUS_PACKET_COUNT_STOPPING;
        count_control_locked(c,reply);portEXIT_CRITICAL(&count_lock);return e;
    }
    portENTER_CRITICAL(&count_lock);count_control_locked(c,reply);portEXIT_CRITICAL(&count_lock);return ESP_OK;
}
static uint32_t count_crc(const uint8_t *p,size_t n)
{
    uint32_t c=UINT32_MAX;for(size_t i=0;i<n;++i){c^=p[i];for(unsigned b=0;b<8U;++b)c=(c>>1U)^(UINT32_C(0xedb88320)&(0U-(c&1U)));}return ~c;
}
static esp_err_t count_peek_locked(s3_packet_count_context_t *c,const cerberus_packet_count_peek_t *q,int64_t now)
{
    if(!c->data_ready||c->release)return ESP_ERR_INVALID_STATE;
    if(c->pin_counter>=CERBERUS_PACKET_COUNT_SEQUENCE_MAX-1U){
        count_first_error(c,ESP_ERR_INVALID_STATE);count_stop_locked(c,now);return ESP_ERR_INVALID_STATE;}
    bool ok=false;uint16_t size=0;
    if(q->kind==CERBERUS_PACKET_COUNT_READ_SUMMARY&&q->index==0U){
        if(c->raw&&c->inflight)return ESP_ERR_NOT_FINISHED;
        cerberus_packet_count_record_stats_t raw;count_raw_stats(c,&raw);
        ok=cerberus_packet_count_encode_summary(&c->table,now,c->state,c->channel,c->request.record,c->error,c->configured,&raw,c->pin_bytes);
        size=CERBERUS_PACKET_COUNT_SUMMARY_BYTES;
    }else if(q->kind==CERBERUS_PACKET_COUNT_READ_ROW){
        ok=cerberus_packet_count_encode_row(&c->table,q->index,now,c->pin_bytes);size=CERBERUS_PACKET_COUNT_ROW_BYTES;
    }else if(q->kind==CERBERUS_PACKET_COUNT_READ_CHANNEL){
        ok=cerberus_packet_count_encode_channel(&c->table,q->index,now,c->pin_bytes);size=CERBERUS_PACKET_COUNT_CHANNEL_BYTES;
    }else if(q->kind==CERBERUS_PACKET_COUNT_READ_RAW_META&&q->index==0U&&c->raw){
        const cerberus_wifi_capture_record_t *r=cerberus_wifi_capture_ring_peek(c->raw);
        if(!r)return ESP_ERR_NOT_FOUND;
        memset(c->raw_pin_bytes,0,CERBERUS_PACKET_COUNT_RAW_META_BYTES);
        cerberus_packet_count_put(c->raw_pin_bytes,r->sequence,4U);cerberus_packet_count_put(c->raw_pin_bytes+4,r->captured_length,2U);
        cerberus_packet_count_put(c->raw_pin_bytes+6,r->original_length,2U);cerberus_packet_count_put(c->raw_pin_bytes+8,r->metadata.capture_time_us,8U);
        /* The serialized command keeps this head and context alive. The RX
         * producer may fill the other slot; do not hold interrupts for CRC. */
        portEXIT_CRITICAL(&count_lock);
        uint32_t crc=count_crc(r->frame,r->captured_length);
        portENTER_CRITICAL(&count_lock);
        cerberus_packet_count_put(c->raw_pin_bytes+16,crc,4U);
        c->raw_pin_bytes[20]=r->metadata.channel;c->raw_pin_bytes[21]=r->metadata.rssi_known?(uint8_t)r->metadata.rssi_dbm:0U;
        c->raw_pin_bytes[22]=1U|(r->metadata.rssi_known?2U:0U);ok=true;size=CERBERUS_PACKET_COUNT_RAW_META_BYTES;
    }
    if(!ok)return ESP_ERR_INVALID_ARG;
    uint32_t ordinal=++c->pin_counter;
    bool raw_meta=q->kind==CERBERUS_PACKET_COUNT_READ_RAW_META;
    if((ordinal&1U)!=(raw_meta?0U:1U))ordinal=++c->pin_counter;
    uint32_t pin=CERBERUS_PACKET_COUNT_PIN_BASE|ordinal;
    if(q->kind==CERBERUS_PACKET_COUNT_READ_RAW_META)c->raw_pin=pin;
    else{c->pin=pin;c->pin_size=size;}
    return ESP_OK;
}
bool cerberus_s3_wifi_packet_count_active(void)
{
    portENTER_CRITICAL(&count_lock);bool active=count_launching||(count_context&&count_context->held);portEXIT_CRITICAL(&count_lock);return active;
}
esp_err_t cerberus_s3_wifi_packet_count_exchange(uint8_t command,const void *input,size_t n,void *output,size_t size)
{
    bool raw_read=command==CERBERUS_PACKET_COUNT_COMMAND_RAW_READ;
    if(!output||size!=(raw_read?sizeof(cerberus_packet_count_raw_chunk_t):sizeof(cerberus_packet_count_control_t)))return ESP_ERR_INVALID_ARG;
    if(n&&input){uintptr_t a=(uintptr_t)input,b=(uintptr_t)output;
        if(a>UINTPTR_MAX-n||b>UINTPTR_MAX-size||(a<b+size&&b<a+n))return ESP_ERR_INVALID_ARG;}
    memset(output,0,size);
    size_t needed=command==CERBERUS_PACKET_COUNT_COMMAND_INFO?0U:
        command==CERBERUS_PACKET_COUNT_COMMAND_BEGIN?sizeof(cerberus_packet_count_begin_t):
        command==CERBERUS_PACKET_COUNT_COMMAND_TARGET?sizeof(cerberus_packet_count_target_t):
        command==CERBERUS_PACKET_COUNT_COMMAND_PEEK?sizeof(cerberus_packet_count_peek_t):
        command==CERBERUS_PACKET_COUNT_COMMAND_READ||raw_read?sizeof(cerberus_packet_count_read_t):
        command==CERBERUS_PACKET_COUNT_COMMAND_RAW_ACK?sizeof(cerberus_packet_count_ack_t):sizeof(cerberus_packet_count_request_t);
    if(command<CERBERUS_PACKET_COUNT_COMMAND_INFO||command>CERBERUS_PACKET_COUNT_COMMAND_RAW_ACK||n!=needed||(n&&!input))return ESP_ERR_INVALID_ARG;
    uint64_t session=0;if(n)memcpy(&session,input,8U);
    cerberus_packet_count_control_t reply={.session_id=session};
    portENTER_CRITICAL(&count_lock);
    if(count_command){portEXIT_CRITICAL(&count_lock);return ESP_ERR_INVALID_STATE;}
    count_command=true;
    s3_packet_count_context_t *c=count_context;int64_t now=esp_timer_get_time();
    if(c)count_expire_locked(c,now);
    portEXIT_CRITICAL(&count_lock);
    /* Task creation failed before any SDK setup. Keep its exact reservation
     * visible until a later command can release it; never touch foreign SDK. */
    if(c&&!c->worker&&c->held){
        esp_err_t cleanup=count_cleanup(c);
        portENTER_CRITICAL(&count_lock);
        if(cleanup==ESP_OK){c->held=false;c->state=CERBERUS_PACKET_COUNT_ERROR;}
        else count_first_error(c,cleanup);
        portEXIT_CRITICAL(&count_lock);
    }
    esp_err_t e=ESP_OK;
    if(command==CERBERUS_PACKET_COUNT_COMMAND_BEGIN){cerberus_packet_count_begin_t q;memcpy(&q,input,sizeof(q));e=count_begin(&q,&reply);}
    else{
        portENTER_CRITICAL(&count_lock);c=count_context;
        /* RX can advance the model between command admission and this lock. */
        now=esp_timer_get_time();if(c)count_expire_locked(c,now);
        if(command==CERBERUS_PACKET_COUNT_COMMAND_INFO){
            if(c)count_control_locked(c,&reply);
            else{reply.session_id=count_epoch;if(count_launching||cerberus_s3_wifi_owner()!=CERBERUS_S3_WIFI_NONE){reply.flags=CERBERUS_PACKET_COUNT_HELD;reply.state=CERBERUS_PACKET_COUNT_QUEUED;}}
        }else if(!session||!c||session!=c->request.session_id){
            e=command==CERBERUS_PACKET_COUNT_COMMAND_RELEASE&&session==count_epoch?ESP_OK:ESP_ERR_NOT_FOUND;
        }else{
            if(command==CERBERUS_PACKET_COUNT_COMMAND_STATUS){
                if(!c->stop&&!c->release&&now>=0)c->deadline_us=(uint64_t)now+(uint64_t)CERBERUS_PACKET_COUNT_LEASE_MS*1000U;
            }else if(command==CERBERUS_PACKET_COUNT_COMMAND_STOP){count_stop_locked(c,now);}
            else if(command==CERBERUS_PACKET_COUNT_COMMAND_RELEASE){c->release=true;c->pin=c->raw_pin=0;count_stop_locked(c,now);}
            else if(command==CERBERUS_PACKET_COUNT_COMMAND_TARGET){
                cerberus_packet_count_target_t q;memcpy(&q,input,sizeof(q));
                cerberus_wifi_packet_count_target_t target={.role=q.role,.channel_hint=q.channel_hint};memcpy(target.mac,q.mac,6U);
                if(q.reserved||q.index>=c->request.target_count||!cerberus_wifi_packet_count_target_valid(&target))e=ESP_ERR_INVALID_ARG;
                else if(c->configured&(1U<<q.index)){if(memcmp(c->targets+q.index,&target,sizeof(target)))e=ESP_ERR_INVALID_STATE;}
                else if(c->state!=CERBERUS_PACKET_COUNT_STAGING||c->stop)e=ESP_ERR_INVALID_STATE;
                else{
                    for(unsigned i=0;i<c->request.target_count;++i)if((c->configured&(1U<<i))&&target.role==c->targets[i].role&&!memcmp(target.mac,c->targets[i].mac,6U))e=ESP_ERR_INVALID_ARG;
                    if(e==ESP_OK){c->targets[q.index]=target;c->configured|=(uint16_t)(1U<<q.index);}
                }
            }else if(command==CERBERUS_PACKET_COUNT_COMMAND_START){
                if(c->data_ready){} /* exact accepted replay never rearms */
                else if(c->state!=CERBERUS_PACKET_COUNT_STAGING||c->stop||c->configured!=(uint16_t)((1U<<c->request.target_count)-1U))e=ESP_ERR_INVALID_STATE;
                else if(!cerberus_wifi_packet_count_init(&c->table,c->targets,c->request.target_count,c->request.channel_mask,c->request.dwell_ms))e=ESP_ERR_INVALID_ARG;
                else{
                    c->data_ready=true;c->state=CERBERUS_PACKET_COUNT_QUEUED;
                    if(c->raw&&!cerberus_wifi_capture_ring_start(c->raw,1U)){e=ESP_ERR_INVALID_STATE;count_first_error(c,e);count_stop_locked(c,now);}
                }
            }else if(command==CERBERUS_PACKET_COUNT_COMMAND_PEEK){cerberus_packet_count_peek_t q;memcpy(&q,input,sizeof(q));e=count_peek_locked(c,&q,now);}
            else if(command==CERBERUS_PACKET_COUNT_COMMAND_RAW_ACK){
                cerberus_packet_count_ack_t q;memcpy(&q,input,sizeof(q));
                if(!q.sequence||!c->raw||c->release)e=ESP_ERR_INVALID_ARG;
                else if(q.sequence!=c->last_ack){
                    if(cerberus_wifi_capture_ring_ack(c->raw,1U,q.sequence)){c->last_ack=q.sequence;if(c->acked!=UINT32_MAX)++c->acked;}
                    else e=ESP_ERR_INVALID_STATE;
                }
            }
            count_control_locked(c,&reply);
            if(command==CERBERUS_PACKET_COUNT_COMMAND_PEEK){cerberus_packet_count_peek_t q;memcpy(&q,input,sizeof(q));
                reply.sequence=e==ESP_OK?(q.kind==CERBERUS_PACKET_COUNT_READ_RAW_META?c->raw_pin:c->pin):0U;}
            if(command==CERBERUS_PACKET_COUNT_COMMAND_RAW_ACK)reply.sequence=e==ESP_OK?c->last_ack:0U;
        }
        if(command==CERBERUS_PACKET_COUNT_COMMAND_READ||raw_read){
            cerberus_packet_count_read_t q;memcpy(&q,input,sizeof(q));
            uint8_t *data=(uint8_t *)output+14U;size_t width=raw_read?CERBERUS_PACKET_COUNT_RAW_CHUNK_BYTES:CERBERUS_PACKET_COUNT_CHUNK_BYTES;
            memcpy(output,&session,8U);memcpy((uint8_t *)output+12U,&q.part,2U);
            if(e==ESP_OK&&c&&session==c->request.session_id&&!c->release){
                const uint8_t *bytes=c->pin_bytes;size_t bytes_count=c->pin_size;
                if(raw_read){const cerberus_wifi_capture_record_t *r=c->raw?cerberus_wifi_capture_ring_peek(c->raw):NULL;
                    if(!r||q.pin!=r->sequence)e=ESP_ERR_INVALID_STATE;else{bytes=r->frame;bytes_count=r->captured_length;}}
                else if(q.pin&&q.pin==c->raw_pin){bytes=c->raw_pin_bytes;bytes_count=CERBERUS_PACKET_COUNT_RAW_META_BYTES;}
                else if(!q.pin||q.pin!=c->pin)e=ESP_ERR_INVALID_STATE;
                size_t offset=(size_t)q.part*width;
                if(e==ESP_OK&&offset>=bytes_count)e=ESP_ERR_INVALID_ARG;
                if(e==ESP_OK){size_t take=bytes_count-offset;if(take>width)take=width;memcpy(data,bytes+offset,take);memcpy((uint8_t *)output+8U,&q.pin,4U);}
            }else if(e==ESP_OK)e=ESP_ERR_INVALID_STATE;
            if(e!=ESP_OK)cerberus_packet_count_put(data,(uint32_t)e,4U);
        }
        if(c&&session==c->request.session_id&&e==ESP_OK&&!c->held&&!c->release&&now>=0&&
           (command==CERBERUS_PACKET_COUNT_COMMAND_STATUS||command==CERBERUS_PACKET_COUNT_COMMAND_PEEK||
            command==CERBERUS_PACKET_COUNT_COMMAND_READ||raw_read))
            c->deadline_us=(uint64_t)now+(uint64_t)CERBERUS_PACKET_COUNT_RETENTION_MS*1000U;
        portEXIT_CRITICAL(&count_lock);
    }
    if(command!=CERBERUS_PACKET_COUNT_COMMAND_READ&&!raw_read){if(e!=ESP_OK)reply.error_code=e;memcpy(output,&reply,sizeof(reply));}
    portENTER_CRITICAL(&count_lock);count_command=false;
    c=count_context;bool dispose=c&&!c->worker&&!c->held&&c->release;
    if(dispose)count_context=NULL;
    portEXIT_CRITICAL(&count_lock);
    if(dispose){heap_caps_free(c->raw);heap_caps_free(c);}
    return ESP_OK;
}
