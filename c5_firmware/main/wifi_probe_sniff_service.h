/* Private include for wifi_scanner.c. SDK and heap calls are outside
 * scanner_mutex/receive critical sections. Existing scanner task is sole
 * worker and sole context disposer; commands only reserve/cancel/read. */
#include "probe_sniff_core.h"

typedef struct {
    cerberus_probe_sniff_table_t table;
    uint8_t pin_bytes[CERBERUS_PROBE_SNIFF_SUMMARY_BYTES];
    uint32_t pin,pin_counter;
    uint8_t pin_kind,pin_index,pin_length;
    uint64_t lease_deadline_us,retention_deadline_us,clock_high_water_us,next_hop_us;
    atomic_bool cancelled;
    bool release_requested,retiring;
    uint32_t inflight;
    uint8_t receive_channel,hop_index;
    /* SDK setup fields are worker-mutated. The armed flag is published under
     * scanner_mutex and the receive lock before command readers observe it. */
    uint8_t phase,backoff;
    bool saved,disable_pending,armed;
    wifi_band_mode_t previous_mode;
    uint8_t previous_channel;
    wifi_second_chan_t previous_second;
    wifi_promiscuous_filter_t previous_filter;
    esp_err_t terminal_error;
} probe_sniff_context_t;
static probe_sniff_context_t *probe_sniff_context;
static uint64_t probe_sniff_epoch,probe_sniff_session;
static cerberus_probe_sniff_start_t probe_sniff_request;
static uint8_t probe_sniff_state;
static esp_err_t probe_sniff_error;

enum { PROBE_SETUP=0,PROBE_ACTIVE,PROBE_DISABLE,PROBE_DRAIN,PROBE_RESTORE };

static bool probe_sniff_clock(uint64_t *out)
{
    int64_t now=esp_timer_get_time();
    if(now<0)return false;
    *out=(uint64_t)now;
    return true;
}
static void probe_sniff_boot_init(void)
{
    /* Called once at scanner boot. Collision-risk discriminator, never an
     * authentication or eternal cross-power-cycle uniqueness claim. */
    probe_sniff_epoch=((uint64_t)esp_random()<<32U)|esp_random();
    if(!probe_sniff_epoch||probe_sniff_epoch==UINT64_MAX)probe_sniff_epoch=UINT64_MAX/2U;
}
static bool probe_sniff_mode_valid(const cerberus_probe_sniff_start_t *q)
{
    return q&&q->session_id&&q->mode<=CERBERUS_PROBE_SNIFF_HOP_BOTH&&
        (q->mode==CERBERUS_PROBE_SNIFF_FIXED?cerberus_probe_sniff_channel_valid(q->channel):q->channel==0U);
}
static void probe_sniff_close(probe_sniff_context_t *c)
{
    portENTER_CRITICAL(&probe_sniff_mux);
    c->receive_channel=0U; /* Already admitted callbacks drain into this epoch. */
    portEXIT_CRITICAL(&probe_sniff_mux);
}
/* scanner_mutex held; expired/invalid clocks cannot revive either deadline. */
static bool probe_sniff_expire_locked(probe_sniff_context_t *c,uint64_t now,bool clock_valid)
{
    if(!c)return false;
    bool owned=atomic_load_explicit(&probe_sniff_radio_reserved,memory_order_acquire);
    if(!clock_valid||now<c->clock_high_water_us||
        (owned?now>=c->lease_deadline_us:now>=c->retention_deadline_us)){
        if(owned){atomic_store_explicit(&c->cancelled,true,memory_order_release);probe_sniff_close(c);
            probe_sniff_state=CERBERUS_PROBE_SNIFF_STOPPING;}
        else {c->retiring=true;c->release_requested=true;}
        return true;
    }
    c->clock_high_water_us=now;return false;
}
static void probe_sniff_reply_locked(uint64_t token,bool info,esp_err_t error,cerberus_probe_sniff_control_t *out)
{
    memset(out,0,sizeof(*out));out->session_id=info?probe_sniff_epoch:token;out->error_code=error;
    bool bound=probe_sniff_context&&probe_sniff_session&&(info||token==probe_sniff_session);
    if(!info&&!bound)return;
    out->capacity=CERBERUS_PROBE_SNIFF_CAPACITY;out->state=probe_sniff_state;
    out->mode=bound?probe_sniff_request.mode:0U;
    if(bound)out->flags|=CERBERUS_PROBE_SNIFF_BOUND;
    if(error==ESP_OK)out->error_code=probe_sniff_error;
    if(atomic_load_explicit(&probe_sniff_radio_reserved,memory_order_acquire))out->flags|=CERBERUS_PROBE_SNIFF_OWNED;
    probe_sniff_context_t *c=probe_sniff_context;
    if(c){
        portENTER_CRITICAL(&probe_sniff_mux);
        out->revision=c->table.summary.revision;out->count=c->table.summary.count;out->channel=c->receive_channel;
        if(c->table.accepting&&c->receive_channel)out->flags|=CERBERUS_PROBE_SNIFF_RECEIVING;
        if(c->table.summary.capacity_dropped_frames)out->flags|=CERBERUS_PROBE_SNIFF_OVERFLOW;
        if(c->table.summary.flags&CERBERUS_PROBE_SNIFF_COUNTERS_SATURATED)out->flags|=CERBERUS_PROBE_SNIFF_SATURATED;
        if(!(out->flags&CERBERUS_PROBE_SNIFF_OWNED)&&c->armed&&!c->retiring&&
            (c->table.summary.flags&CERBERUS_PROBE_SNIFF_STOPPED))out->flags|=CERBERUS_PROBE_SNIFF_DATA_READY;
        portEXIT_CRITICAL(&probe_sniff_mux);
    }
}
void cerberus_wifi_probe_sniff_info(cerberus_probe_sniff_control_t *out)
{
    if(!out)return;
    memset(out,0,sizeof(*out));
    if(!scanner_mutex){out->error_code=ESP_ERR_INVALID_STATE;return;}
    xSemaphoreTake(scanner_mutex,portMAX_DELAY);probe_sniff_reply_locked(0U,true,ESP_OK,out);xSemaphoreGive(scanner_mutex);
}
esp_err_t cerberus_wifi_probe_sniff_start(const cerberus_probe_sniff_start_t *q,cerberus_probe_sniff_control_t *out)
{
    if(!q||!out)return ESP_ERR_INVALID_ARG;
    memset(out,0,sizeof(*out));out->session_id=q->session_id;
    if(!scanner_mutex||!scanner_task_handle){out->error_code=ESP_ERR_INVALID_STATE;return ESP_ERR_INVALID_STATE;}
    xSemaphoreTake(scanner_mutex,portMAX_DELAY);
    if(q->session_id==probe_sniff_session&&probe_sniff_session){
        esp_err_t error=memcmp(q,&probe_sniff_request,sizeof(*q))?ESP_ERR_INVALID_ARG:
            probe_sniff_context?ESP_OK:ESP_ERR_NOT_FOUND;
        probe_sniff_reply_locked(q->session_id,false,error,out);xSemaphoreGive(scanner_mutex);return error;
    }
    bool allowed=probe_sniff_mode_valid(q)&&!probe_sniff_context&&probe_sniff_epoch!=UINT64_MAX&&q->session_id==probe_sniff_epoch+1U;
    xSemaphoreGive(scanner_mutex);
    esp_err_t error=allowed?ESP_OK:ESP_ERR_INVALID_STATE;
    probe_sniff_context_t *candidate=NULL;
    if(error==ESP_OK){
        size_t size=sizeof(*candidate);
        if(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)<size)error=ESP_ERR_NO_MEM;
        else {candidate=heap_caps_calloc(1U,size,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);if(!candidate)error=ESP_ERR_NO_MEM;}
    }
    /* SDK/native source status checks are admission-only, never proof of a
     * claim. Recheck all this scanner's reservations under its mutex below. */
    if(error==ESP_OK&&(cerberus_c5_test_running()||another_radio_operation_is_active()))error=ESP_ERR_INVALID_STATE;
    xSemaphoreTake(scanner_mutex,portMAX_DELAY);
    uint64_t now=0U;bool valid_clock=probe_sniff_clock(&now);
    if(error==ESP_OK&&(!valid_clock||now>INT64_MAX-(uint64_t)CERBERUS_PROBE_SNIFF_LEASE_MS*1000U||
        atomic_load_explicit(&ap_survey_radio_reserved,memory_order_acquire)||probe_sniff_context||probe_sniff_epoch==UINT64_MAX||q->session_id!=probe_sniff_epoch+1U||!scanner_ready||
        station_reserved||wifi_connection_is_active_locked()||scan_status.state==CERBERUS_WIFI_SCAN_RUNNING||
        defense_status.state==CERBERUS_WIFI_DEFENSE_RUNNING||channel_survey_status.state==CERBERUS_WIFI_CHANNEL_SURVEY_RUNNING||
        atomic_load_explicit(&channel_survey_radio_reserved,memory_order_acquire)||
        atomic_load_explicit(&mac_radio_reserved,memory_order_acquire)||atomic_load_explicit(&csi_radio_reserved,memory_order_acquire)||
        atomic_load_explicit(&capture_radio_reserved,memory_order_acquire)||atomic_load_explicit(&remote_id_radio_reserved,memory_order_acquire)||
        atomic_load_explicit(&packet_monitor_radio_reserved,memory_order_acquire)))error=ESP_ERR_INVALID_STATE;
    if(error==ESP_OK){
        probe_sniff_epoch=probe_sniff_session=q->session_id;probe_sniff_request=*q;
        candidate->lease_deadline_us=now+(uint64_t)CERBERUS_PROBE_SNIFF_LEASE_MS*1000U;
        candidate->clock_high_water_us=now;
        cerberus_probe_sniff_table_begin(&candidate->table,q->session_id,now);
        candidate->table.accepting=false;
        candidate->table.summary.started_source_us=0U;
        candidate->table.summary.flags=0U;
        portENTER_CRITICAL(&probe_sniff_mux);probe_sniff_context=candidate;portEXIT_CRITICAL(&probe_sniff_mux);candidate=NULL;
        probe_sniff_state=CERBERUS_PROBE_SNIFF_QUEUED;probe_sniff_error=ESP_OK;
        atomic_store_explicit(&probe_sniff_radio_reserved,true,memory_order_release);
    }
    probe_sniff_reply_locked(q->session_id,false,error,out);xSemaphoreGive(scanner_mutex);
    if(candidate)heap_caps_free(candidate);
    if(error==ESP_OK)xTaskNotifyGive(scanner_task_handle);
    return error;
}
static esp_err_t probe_sniff_control(uint64_t token,unsigned action,cerberus_probe_sniff_control_t *out)
{
    if(!out)return ESP_ERR_INVALID_ARG;
    memset(out,0,sizeof(*out));out->session_id=token;
    if(!scanner_mutex){out->error_code=ESP_ERR_INVALID_STATE;return ESP_ERR_INVALID_STATE;}
    xSemaphoreTake(scanner_mutex,portMAX_DELAY);
    uint64_t now=0U;bool valid_clock=probe_sniff_clock(&now);
    esp_err_t error=token&&token==probe_sniff_session&&(probe_sniff_context||action==2U)?ESP_OK:ESP_ERR_NOT_FOUND;
    probe_sniff_context_t *c=error==ESP_OK?probe_sniff_context:NULL;
    if(c){
        bool expired=probe_sniff_expire_locked(c,now,valid_clock);
        bool owned=atomic_load_explicit(&probe_sniff_radio_reserved,memory_order_acquire);
        if(action){atomic_store_explicit(&c->cancelled,true,memory_order_release);probe_sniff_close(c);
            if(owned)probe_sniff_state=CERBERUS_PROBE_SNIFF_STOPPING;
            c->pin=0U;
            if(action==2U){c->release_requested=c->retiring=true;}
        }else if(!expired&&!c->retiring){
            if(owned&&!atomic_load_explicit(&c->cancelled,memory_order_acquire))
                c->lease_deadline_us=now+(uint64_t)CERBERUS_PROBE_SNIFF_LEASE_MS*1000U;
            else if(!owned)c->retention_deadline_us=now+(uint64_t)CERBERUS_PROBE_SNIFF_RETENTION_MS*1000U;
        }
    }
    probe_sniff_reply_locked(token,false,error,out);xSemaphoreGive(scanner_mutex);
    if(c&&action)xTaskNotifyGive(scanner_task_handle);
    return error;
}
esp_err_t cerberus_wifi_probe_sniff_status(uint64_t token,cerberus_probe_sniff_control_t *out)
{return probe_sniff_control(token,0U,out);}
esp_err_t cerberus_wifi_probe_sniff_stop(uint64_t token,cerberus_probe_sniff_control_t *out)
{return probe_sniff_control(token,1U,out);}
esp_err_t cerberus_wifi_probe_sniff_release(uint64_t token,cerberus_probe_sniff_control_t *out)
{return probe_sniff_control(token,2U,out);}

/* RX callback observes an epoch under the short receive lock, parses outside
 * it, then revalidates that epoch before publication. No heap or SDK calls. */
static void probe_sniff_receive(const wifi_promiscuous_pkt_t *packet,wifi_promiscuous_pkt_type_t type,int64_t callback_us)
{
    /* ESP-IDF v6.0.1: esp_wifi_types_native.h and the simple_sniffer example
     * use sig_len for the callback payload, excluding four FCS bytes. Require
     * one payload byte before the subtype read; dump_len is not an extra gate. */
    if(!packet||type!=WIFI_PKT_MGMT||packet->rx_ctrl.rx_state||packet->rx_ctrl.sig_len<5U)return;
    uint64_t token=0U;
    portENTER_CRITICAL(&probe_sniff_mux);
    probe_sniff_context_t *c=probe_sniff_context;
    if(c&&c->table.accepting&&c->receive_channel==packet->rx_ctrl.channel&&packet->payload[0]==0x40U){
        token=c->table.session_id;++c->inflight;
    }
    portEXIT_CRITICAL(&probe_sniff_mux);
    if(!token)return;
    cerberus_probe_sniff_row_t row;
    cerberus_probe_sniff_decode_result_t result=CERBERUS_PROBE_SNIFF_DECODE_UNSUPPORTED;
    size_t length=(size_t)packet->rx_ctrl.sig_len-4U;
    if(length<=CERBERUS_PROBE_SNIFF_MAX_FRAME_BYTES&&callback_us>=0)
        result=cerberus_probe_sniff_decode(packet->payload,length,packet->rx_ctrl.channel,
            packet->rx_ctrl.rssi,(uint64_t)callback_us,&row);
    portENTER_CRITICAL(&probe_sniff_mux);
    c=probe_sniff_context;
    if(c&&c->table.session_id==token){
        if(result==CERBERUS_PROBE_SNIFF_DECODE_VALID)
            (void)cerberus_probe_sniff_table_commit(&c->table,token,&row,(uint64_t)callback_us);
        else cerberus_probe_sniff_table_rejected(&c->table,token,result,callback_us>=0?(uint64_t)callback_us:UINT64_MAX);
        --c->inflight;
    }
    portEXIT_CRITICAL(&probe_sniff_mux);
}

/* Return false while waiting for existing admitted callbacks; never free a
 * context or retune until the SDK disable and callback drain both complete. */
static bool probe_sniff_drained(probe_sniff_context_t *c)
{
    portENTER_CRITICAL(&probe_sniff_mux);bool empty=c->inflight==0U;portEXIT_CRITICAL(&probe_sniff_mux);return empty;
}
static void probe_sniff_worker_error(probe_sniff_context_t *c,esp_err_t error)
{
    c->terminal_error=error;atomic_store_explicit(&c->cancelled,true,memory_order_release);probe_sniff_close(c);
    xSemaphoreTake(scanner_mutex,portMAX_DELAY);probe_sniff_error=error;probe_sniff_state=CERBERUS_PROBE_SNIFF_STOPPING;xSemaphoreGive(scanner_mutex);
    c->phase=PROBE_DISABLE;
}
static esp_err_t probe_sniff_tune(probe_sniff_context_t *c)
{
    unsigned attempts=probe_sniff_request.mode==CERBERUS_PROBE_SNIFF_FIXED?1U:CERBERUS_PROBE_SNIFF_CHANNEL_COUNT;
    esp_err_t first_error=ESP_OK;
    for(unsigned n=0;n<attempts;++n){
        uint8_t index=c->hop_index,channel=cerberus_probe_sniff_channel_at(index);
        c->hop_index=(uint8_t)((index+1U)%CERBERUS_PROBE_SNIFF_CHANNEL_COUNT);
        if(probe_sniff_request.mode==CERBERUS_PROBE_SNIFF_FIXED){
            channel=probe_sniff_request.channel;
            for(index=0;cerberus_probe_sniff_channel_at(index)!=channel;++index){}
        }else if((probe_sniff_request.mode==CERBERUS_PROBE_SNIFF_HOP_24&&channel>14U)||
            (probe_sniff_request.mode==CERBERUS_PROBE_SNIFF_HOP_5&&channel<=14U))continue;
        portENTER_CRITICAL(&probe_sniff_mux);c->table.summary.attempted_channels|=UINT64_C(1)<<index;portEXIT_CRITICAL(&probe_sniff_mux);
        esp_err_t error=esp_wifi_set_band(channel<=14U?WIFI_BAND_2G:WIFI_BAND_5G);
        if(error==ESP_OK)error=esp_wifi_set_channel(channel,WIFI_SECOND_CHAN_NONE);
        if(error!=ESP_OK){
            if(first_error==ESP_OK)first_error=error;
            portENTER_CRITICAL(&probe_sniff_mux);
            if(c->table.summary.channel_failures<UINT32_MAX)++c->table.summary.channel_failures;
            if(c->table.summary.channel_failures==UINT32_MAX)c->table.summary.flags|=CERBERUS_PROBE_SNIFF_COUNTERS_SATURATED;
            portEXIT_CRITICAL(&probe_sniff_mux);continue;
        }
        portENTER_CRITICAL(&probe_sniff_mux);c->table.summary.successful_channels|=UINT64_C(1)<<index;portEXIT_CRITICAL(&probe_sniff_mux);
        xSemaphoreTake(scanner_mutex,portMAX_DELAY);
        uint64_t before_enable=0U;bool before_clock=probe_sniff_clock(&before_enable);
        (void)probe_sniff_expire_locked(c,before_enable,before_clock);
        bool cancelled=atomic_load_explicit(&c->cancelled,memory_order_acquire);
        xSemaphoreGive(scanner_mutex);
        if(cancelled)return ESP_ERR_NOT_FINISHED;
        c->disable_pending=true;error=esp_wifi_set_promiscuous(true);
        if(error!=ESP_OK)return error;
        uint64_t now=0;
        xSemaphoreTake(scanner_mutex,portMAX_DELAY);
        bool clock_valid=probe_sniff_clock(&now);
        bool expired=probe_sniff_expire_locked(c,now,clock_valid);
        if(expired){xSemaphoreGive(scanner_mutex);return ESP_ERR_INVALID_STATE;}
        portENTER_CRITICAL(&probe_sniff_mux);
        if(!c->armed){
            c->table.armed_us=c->table.clock_high_water_us=now;
            c->table.summary.started_source_us=now;
            c->table.summary.flags|=CERBERUS_PROBE_SNIFF_START_TIME_VALID;c->armed=true;
        }
        if(!atomic_load_explicit(&c->cancelled,memory_order_acquire)){
            c->table.accepting=true;c->receive_channel=channel;
            probe_sniff_state=CERBERUS_PROBE_SNIFF_RUNNING;
        }
        portEXIT_CRITICAL(&probe_sniff_mux);
        c->next_hop_us=now+(uint64_t)CERBERUS_PROBE_SNIFF_DWELL_MS*1000U;c->phase=PROBE_ACTIVE;
        xSemaphoreGive(scanner_mutex);return ESP_OK;
    }
    return first_error==ESP_OK?ESP_ERR_NOT_SUPPORTED:first_error;
}
static void probe_sniff_worker_tick(void)
{
    xSemaphoreTake(scanner_mutex,portMAX_DELAY);
    uint64_t now=0;bool valid_clock=probe_sniff_clock(&now);
    probe_sniff_context_t *c=probe_sniff_context;
    if(!c){xSemaphoreGive(scanner_mutex);return;}
    (void)probe_sniff_expire_locked(c,now,valid_clock);
    bool owned=atomic_load_explicit(&probe_sniff_radio_reserved,memory_order_acquire);
    bool retire=!owned&&c->release_requested;
    if(retire&&probe_sniff_drained(c)){
        portENTER_CRITICAL(&probe_sniff_mux);probe_sniff_context=NULL;portEXIT_CRITICAL(&probe_sniff_mux);
        probe_sniff_state=CERBERUS_PROBE_SNIFF_IDLE;probe_sniff_error=ESP_OK;
        xSemaphoreGive(scanner_mutex);heap_caps_free(c);return;
    }
    xSemaphoreGive(scanner_mutex);
    if(!owned)return;
    if(c->backoff){--c->backoff;return;}
    bool stop=atomic_load_explicit(&c->cancelled,memory_order_acquire);
    if(stop&&c->phase<PROBE_DISABLE){probe_sniff_close(c);c->phase=PROBE_DISABLE;}
    if(c->phase==PROBE_SETUP){
        esp_err_t error=esp_wifi_get_band_mode(&c->previous_mode);
        if(error==ESP_OK)error=esp_wifi_get_channel(&c->previous_channel,&c->previous_second);
        if(error==ESP_OK)error=esp_wifi_get_promiscuous_filter(&c->previous_filter);
        if(error==ESP_OK)c->saved=true;
        if(error==ESP_OK)error=esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
        wifi_promiscuous_filter_t filter={.filter_mask=WIFI_PROMIS_FILTER_MASK_MGMT};
        if(error==ESP_OK)error=esp_wifi_set_promiscuous_filter(&filter);
        if(error==ESP_OK)error=probe_sniff_tune(c);
        if(error!=ESP_OK)probe_sniff_worker_error(c,error);
        return;
    }
    if(c->phase==PROBE_ACTIVE){
        bool exhausted;
        portENTER_CRITICAL(&probe_sniff_mux);exhausted=(c->table.summary.flags&CERBERUS_PROBE_SNIFF_REVISION_EXHAUSTED)!=0;portEXIT_CRITICAL(&probe_sniff_mux);
        if(exhausted){probe_sniff_worker_error(c,ESP_ERR_INVALID_STATE);return;}
        if(probe_sniff_request.mode==CERBERUS_PROBE_SNIFF_FIXED||now<c->next_hop_us)return;
        probe_sniff_close(c);c->phase=PROBE_DISABLE;
    }
    if(c->phase==PROBE_DISABLE){
        if(c->disable_pending){
            esp_err_t error=esp_wifi_set_promiscuous(false);
            if(error!=ESP_OK){
                probe_sniff_worker_error(c,error);c->backoff=13U;return;
            }
            c->disable_pending=false;
        }
        c->phase=PROBE_DRAIN;
    }
    if(c->phase==PROBE_DRAIN){
        if(!probe_sniff_drained(c))return;
        if(!atomic_load_explicit(&c->cancelled,memory_order_acquire)){
            esp_err_t error=probe_sniff_tune(c);if(error!=ESP_OK)probe_sniff_worker_error(c,error);return;
        }
        portENTER_CRITICAL(&probe_sniff_mux);
        if(c->armed)cerberus_probe_sniff_table_finish(&c->table,probe_sniff_session,valid_clock?(int64_t)now:-1);
        portEXIT_CRITICAL(&probe_sniff_mux);c->phase=PROBE_RESTORE;
    }
    if(c->phase==PROBE_RESTORE){
        esp_err_t error=ESP_OK;
        if(c->saved){
            error=esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
            if(error==ESP_OK&&c->previous_channel){
                error=esp_wifi_set_band(c->previous_channel<=14U?WIFI_BAND_2G:WIFI_BAND_5G);
                if(error==ESP_OK)error=esp_wifi_set_channel(c->previous_channel,c->previous_second);
            }
            if(error==ESP_OK)error=esp_wifi_set_promiscuous_filter(&c->previous_filter);
            if(error==ESP_OK)error=esp_wifi_set_band_mode(c->previous_mode);
        }
        xSemaphoreTake(scanner_mutex,portMAX_DELAY);
        if(error!=ESP_OK){probe_sniff_error=error;c->backoff=13U;xSemaphoreGive(scanner_mutex);return;}
        atomic_store_explicit(&probe_sniff_radio_reserved,false,memory_order_release);
        probe_sniff_error=c->terminal_error;probe_sniff_state=c->terminal_error==ESP_OK?CERBERUS_PROBE_SNIFF_COMPLETE:CERBERUS_PROBE_SNIFF_ERROR;
        valid_clock=probe_sniff_clock(&now);
        if(valid_clock&&now>=c->clock_high_water_us&&now<=INT64_MAX-(uint64_t)CERBERUS_PROBE_SNIFF_RETENTION_MS*1000U)
            c->retention_deadline_us=now+(uint64_t)CERBERUS_PROBE_SNIFF_RETENTION_MS*1000U;
        else c->retiring=c->release_requested=true;
        xSemaphoreGive(scanner_mutex);
    }
}

esp_err_t cerberus_wifi_probe_sniff_read(const cerberus_probe_sniff_read_t *q,cerberus_probe_sniff_chunk_t *out)
{
    if(!q||!out)return ESP_ERR_INVALID_ARG;
    memset(out,0,sizeof(*out));out->session_id=q->session_id;out->part=q->part;
    esp_err_t error=ESP_OK;
    if(!scanner_mutex)error=ESP_ERR_INVALID_STATE;
    else {
        xSemaphoreTake(scanner_mutex,portMAX_DELAY);
        probe_sniff_context_t *c=probe_sniff_context;
        uint64_t now=0U;bool clock_valid=probe_sniff_clock(&now);
        if(!q->session_id||q->session_id!=probe_sniff_session||!c||c->retiring)error=ESP_ERR_NOT_FOUND;
        else if(!c->armed||q->kind>CERBERUS_PROBE_SNIFF_READ_ROW||
            (q->kind==CERBERUS_PROBE_SNIFF_READ_SUMMARY&&(q->index||q->part>=CERBERUS_PROBE_SNIFF_SUMMARY_PARTS))||
            (q->kind==CERBERUS_PROBE_SNIFF_READ_ROW&&q->part>=CERBERUS_PROBE_SNIFF_ROW_PARTS))error=ESP_ERR_INVALID_ARG;
        else {
            bool owned=atomic_load_explicit(&probe_sniff_radio_reserved,memory_order_acquire);
            portENTER_CRITICAL(&probe_sniff_mux);uint8_t count=c->table.summary.count;portEXIT_CRITICAL(&probe_sniff_mux);
            bool query_valid=q->part==0U?(!q->pin&&(q->kind==CERBERUS_PROBE_SNIFF_READ_SUMMARY||q->index<count)):
                (q->pin&&q->pin==c->pin&&q->kind==c->pin_kind&&q->index==c->pin_index);
            if(!query_valid)error=ESP_ERR_INVALID_ARG;
            else if(!owned&&probe_sniff_expire_locked(c,now,clock_valid))error=ESP_ERR_NOT_FOUND;
            else if(q->part==0U){
                if(q->pin)error=ESP_ERR_INVALID_ARG;
                else if(c->pin_counter==UINT32_MAX)error=ESP_ERR_INVALID_STATE;
                else {
                    union {cerberus_probe_sniff_summary_t summary;cerberus_probe_sniff_projection_t row;} frozen;
                    bool valid;
                    portENTER_CRITICAL(&probe_sniff_mux);
                    if(q->kind==CERBERUS_PROBE_SNIFF_READ_SUMMARY)
                        valid=cerberus_probe_sniff_table_copy_summary(&c->table,q->session_id,
                            clock_valid?(int64_t)now:-1,&frozen.summary);
                    else valid=cerberus_probe_sniff_table_copy_row(&c->table,q->session_id,q->index,
                        clock_valid?(int64_t)now:-1,&frozen.row);
                    portEXIT_CRITICAL(&probe_sniff_mux);
                    if(!valid)error=ESP_ERR_INVALID_ARG;
                    else {c->pin=++c->pin_counter;c->pin_kind=q->kind;c->pin_index=q->index;
                        c->pin_length=q->kind==CERBERUS_PROBE_SNIFF_READ_SUMMARY?CERBERUS_PROBE_SNIFF_SUMMARY_BYTES:CERBERUS_PROBE_SNIFF_PROJECTION_BYTES;
                        memcpy(c->pin_bytes,&frozen,c->pin_length);}
                }
            }else if(!q->pin||q->pin!=c->pin||q->kind!=c->pin_kind||q->index!=c->pin_index)error=ESP_ERR_NOT_FOUND;
            if(error==ESP_OK){
                size_t offset=(size_t)q->part*CERBERUS_PROBE_SNIFF_CHUNK_BYTES;
                size_t count=(size_t)c->pin_length-offset;
                if(count>CERBERUS_PROBE_SNIFF_CHUNK_BYTES)count=CERBERUS_PROBE_SNIFF_CHUNK_BYTES;
                out->pin=c->pin;memcpy(out->data,c->pin_bytes+offset,count);
                if(!owned)c->retention_deadline_us=now+(uint64_t)CERBERUS_PROBE_SNIFF_RETENTION_MS*1000U;
            }
        }
        xSemaphoreGive(scanner_mutex);
    }
    if(error!=ESP_OK){uint32_t value=(uint32_t)error;for(unsigned i=0;i<4U;++i)out->data[i]=(uint8_t)(value>>(i*8U));}
    return error;
}

/* Idle worker polling exists only while this optional retained context exists;
 * it also permits new scanner jobs while a stopped bank is being downloaded. */
static bool probe_sniff_retained(void)
{
    xSemaphoreTake(scanner_mutex,portMAX_DELAY);bool retained=probe_sniff_context!=NULL;xSemaphoreGive(scanner_mutex);return retained;
}
