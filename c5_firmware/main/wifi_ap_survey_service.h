/* Private to wifi_scanner.c. One optional context, existing scanner worker.
 * Commands serialize through scanner_mutex; RX uses only ap_survey_mux. SDK
 * calls and heap operations never execute in the receive critical section. */
#include "wifi_ap_survey_protocol.h"
#include "wifi_packet_count.h"

typedef struct {
    cerberus_ap_survey_table_t table;
    cerberus_ap_survey_snapshot_t snapshot;
    cerberus_ap_survey_start_t request;
    uint64_t lease, retention, clock_floor, next_hop, pass_seen, pass_applied;
    uint32_t projection, projection_counter, inflight;
    uint8_t phase, next_index, state, backoff;
    bool stop, release, saved, disable_pending, clock_invalid;
    wifi_band_mode_t previous_mode;
    uint8_t previous_channel;
    wifi_second_chan_t previous_second;
    wifi_promiscuous_filter_t previous_filter;
    esp_err_t error, tune_error;
} ap_survey_context_t;
static ap_survey_context_t *ap_survey_context;
static uint64_t ap_survey_epoch, ap_survey_last_session;
static cerberus_ap_survey_start_t ap_survey_last_request;
static uint32_t ap_survey_arm_counter;
enum { AP_SURVEY_SETUP, AP_SURVEY_TUNE, AP_SURVEY_ACTIVE,
    AP_SURVEY_DISABLE, AP_SURVEY_DRAIN, AP_SURVEY_RESTORE };

static void ap_survey_boot_init(void)
{
    ap_survey_epoch=((uint64_t)esp_random()<<32U)|esp_random();
    if(!ap_survey_epoch||ap_survey_epoch==UINT64_MAX)ap_survey_epoch=UINT64_MAX/2U;
}
static bool ap_survey_retained(void)
{
    portENTER_CRITICAL(&ap_survey_mux);
    bool retained=ap_survey_context!=NULL;
    portEXIT_CRITICAL(&ap_survey_mux);
    return retained;
}
static void ap_survey_close(ap_survey_context_t *c,int64_t now)
{
    portENTER_CRITICAL(&ap_survey_mux);
    atomic_store_explicit(&ap_survey_admission,0U,memory_order_release);
    if(c->table.accepting) {
        uint64_t end=now>=0&&(uint64_t)now>=c->table.armed_us ? (uint64_t)now:c->table.armed_us;
        (void)cerberus_ap_survey_disarm(&c->table,c->request.session_id,c->table.arm_token,end);
    }
    portEXIT_CRITICAL(&ap_survey_mux);
}
static void ap_survey_fail(ap_survey_context_t *c,esp_err_t error,int64_t now)
{
    if(c->error==ESP_OK)c->error=error;
    c->stop=true;c->state=CERBERUS_AP_SURVEY_STOPPING;
    ap_survey_close(c,now);
    if(c->phase<AP_SURVEY_DISABLE)c->phase=AP_SURVEY_DISABLE;
}
/* scanner_mutex held. A failed or expired clock never revives a deadline. */
static bool ap_survey_expire(ap_survey_context_t *c,int64_t now)
{
    bool held=atomic_load_explicit(&ap_survey_radio_reserved,memory_order_acquire);
    bool valid=now>=0&&(uint64_t)now>=c->clock_floor&&
        (uint64_t)now<=INT64_MAX-(uint64_t)CERBERUS_AP_SURVEY_RETENTION_MS*1000U;
    if(!valid||(uint64_t)now>=(held?c->lease:c->retention)) {
        if(!valid)c->clock_invalid=true;
        if(held&&!c->stop)ap_survey_fail(c,valid?ESP_ERR_TIMEOUT:ESP_ERR_INVALID_STATE,now);
        else if(!held)c->release=true;
        return true;
    }
    c->clock_floor=(uint64_t)now;
    return c->stop||c->release;
}
static bool ap_survey_drained(ap_survey_context_t *c)
{
    portENTER_CRITICAL(&ap_survey_mux);
    bool drained=c->inflight==0U;
    portEXIT_CRITICAL(&ap_survey_mux);
    return drained;
}
static void ap_survey_reply(uint64_t session,bool info,esp_err_t error,
    cerberus_ap_survey_control_t *out)
{
    memset(out,0,sizeof(*out));out->session_id=info?ap_survey_epoch:session;out->error_code=error;
    ap_survey_context_t *c=ap_survey_context;
    if(!c||(!info&&session!=c->request.session_id))return;
    out->session_id=c->request.session_id;out->state=c->state;
    out->flags=CERBERUS_AP_SURVEY_BOUND;
    if(atomic_load_explicit(&ap_survey_radio_reserved,memory_order_acquire))out->flags|=CERBERUS_AP_SURVEY_HELD;
    portENTER_CRITICAL(&ap_survey_mux);
    if(!c->release&&!c->clock_invalid) {
        out->revision=c->table.live.revision;out->ap_count=c->table.live.ap_count;
        out->relation_count=c->table.live.relation_count;
        if(c->table.accepting){out->flags|=CERBERUS_AP_SURVEY_RECEIVING;out->channel=c->table.live.channel;}
        if(c->table.live.revision)out->flags|=CERBERUS_AP_SURVEY_DATA_READY;
    }
    portEXIT_CRITICAL(&ap_survey_mux);
    if(error==ESP_OK)out->error_code=c->error!=ESP_OK?c->error:c->tune_error;
}
static bool ap_survey_start_valid(const cerberus_ap_survey_start_t *q)
{
    return q&&q->session_id&&!q->reserved&&q->dwell_ms==CERBERUS_AP_SURVEY_DWELL_MS&&
        cerberus_wifi_packet_count_mask_valid(q->channel_mask);
}
static esp_err_t ap_survey_start(const cerberus_ap_survey_start_t *q,cerberus_ap_survey_control_t *out)
{
    xSemaphoreTake(scanner_mutex,portMAX_DELAY);
    if(q->session_id==ap_survey_last_session&&ap_survey_last_session) {
        esp_err_t error=memcmp(q,&ap_survey_last_request,sizeof(*q))?ESP_ERR_INVALID_ARG:
            ap_survey_context?ESP_OK:ESP_ERR_NOT_FOUND;
        ap_survey_reply(q->session_id,false,error,out);xSemaphoreGive(scanner_mutex);return error;
    }
    bool allowed=ap_survey_start_valid(q)&&!ap_survey_context&&ap_survey_epoch!=UINT64_MAX&&q->session_id==ap_survey_epoch+1U;
    xSemaphoreGive(scanner_mutex);
    esp_err_t error=allowed?ESP_OK:ESP_ERR_INVALID_STATE;
    ap_survey_context_t *candidate=NULL;
    if(error==ESP_OK) {
        candidate=heap_caps_calloc(1U,sizeof(*candidate),MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
        if(!candidate)error=ESP_ERR_NO_MEM;
    }
    if(error==ESP_OK&&(cerberus_c5_test_running()||another_radio_operation_is_active()))error=ESP_ERR_INVALID_STATE;
    xSemaphoreTake(scanner_mutex,portMAX_DELAY);
    int64_t now=esp_timer_get_time();
    if(error==ESP_OK&&(!scanner_ready||!scanner_task_handle||ap_survey_context||
        ap_survey_epoch==UINT64_MAX||q->session_id!=ap_survey_epoch+1U||now<0||
        (uint64_t)now>INT64_MAX-(uint64_t)CERBERUS_AP_SURVEY_RETENTION_MS*1000U||
        station_reserved||wifi_connection_is_active_locked()||scan_status.state==CERBERUS_WIFI_SCAN_RUNNING||
        defense_status.state==CERBERUS_WIFI_DEFENSE_RUNNING||
        atomic_load_explicit(&channel_survey_radio_reserved,memory_order_acquire)||
        atomic_load_explicit(&probe_sniff_radio_reserved,memory_order_acquire)||
        atomic_load_explicit(&packet_count_radio_reserved,memory_order_acquire)||
        atomic_load_explicit(&mac_radio_reserved,memory_order_acquire)||
        atomic_load_explicit(&csi_radio_reserved,memory_order_acquire)||
        atomic_load_explicit(&remote_id_radio_reserved,memory_order_acquire)||
        atomic_load_explicit(&capture_radio_reserved,memory_order_acquire)||
        atomic_load_explicit(&packet_monitor_radio_reserved,memory_order_acquire)||cerberus_wardrive_radio_held()))
        error=ESP_ERR_INVALID_STATE;
    if(error==ESP_OK&&!cerberus_ap_survey_begin(&candidate->table,q->session_id,q->channel_mask,q->channel_mask))
        error=ESP_ERR_INVALID_ARG;
    if(error==ESP_OK) {
        candidate->request=*q;candidate->lease=(uint64_t)now+(uint64_t)CERBERUS_AP_SURVEY_LEASE_MS*1000U;
        candidate->clock_floor=(uint64_t)now;candidate->state=CERBERUS_AP_SURVEY_QUEUED;
        ap_survey_epoch=ap_survey_last_session=q->session_id;ap_survey_last_request=*q;
        portENTER_CRITICAL(&ap_survey_mux);ap_survey_context=candidate;portEXIT_CRITICAL(&ap_survey_mux);
        candidate=NULL;atomic_store_explicit(&ap_survey_radio_reserved,true,memory_order_release);
    }
    ap_survey_reply(q->session_id,false,error,out);xSemaphoreGive(scanner_mutex);
    if(candidate)heap_caps_free(candidate);
    if(error==ESP_OK)xTaskNotifyGive(scanner_task_handle);
    return error;
}

esp_err_t cerberus_wifi_ap_survey_exchange(uint8_t command,const void *request,size_t request_size,
    void *reply,size_t reply_size)
{
    size_t expected=cerberus_ap_survey_request_bytes(command);
    if(!reply||expected==SIZE_MAX||request_size!=expected||reply_size!=cerberus_ap_survey_reply_bytes(command)||
        (expected?!request:request!=NULL))return ESP_ERR_INVALID_ARG;
    uint8_t query[sizeof(cerberus_ap_survey_start_t)]={0};
    if(expected)memcpy(query,request,expected);
    memset(reply,0,reply_size);
    uint64_t session=0U;
    if(expected)memcpy(&session,query,sizeof(session));
    if(!scanner_mutex)return ESP_ERR_INVALID_STATE;
    if(command==CERBERUS_AP_SURVEY_COMMAND_START) {
        cerberus_ap_survey_start_t q;memcpy(&q,query,sizeof(q));return ap_survey_start(&q,reply);
    }
    bool occupied=command==CERBERUS_AP_SURVEY_COMMAND_INFO&&another_radio_operation_is_active();
    xSemaphoreTake(scanner_mutex,portMAX_DELAY);
    ap_survey_context_t *c=ap_survey_context;
    int64_t now=esp_timer_get_time();
    if(c)(void)ap_survey_expire(c,now);
    if(command==CERBERUS_AP_SURVEY_COMMAND_INFO) {
        cerberus_ap_survey_control_t *out=reply;ap_survey_reply(0U,true,ESP_OK,out);
        if(!c&&(occupied||station_reserved||wifi_connection_is_active_locked()||
            scan_status.state==CERBERUS_WIFI_SCAN_RUNNING||defense_status.state==CERBERUS_WIFI_DEFENSE_RUNNING||
            atomic_load_explicit(&channel_survey_radio_reserved,memory_order_acquire))) {
            out->flags=CERBERUS_AP_SURVEY_HELD;out->state=CERBERUS_AP_SURVEY_QUEUED;
        }
        xSemaphoreGive(scanner_mutex);return ESP_OK;
    }
    esp_err_t error=session&&c&&session==c->request.session_id?ESP_OK:ESP_ERR_NOT_FOUND;
    if(command==CERBERUS_AP_SURVEY_COMMAND_RELEASE&&session&&session==ap_survey_last_session&&!c)error=ESP_OK;
    bool held=c&&atomic_load_explicit(&ap_survey_radio_reserved,memory_order_acquire);
    if(error==ESP_OK&&c&&c->release&&command!=CERBERUS_AP_SURVEY_COMMAND_RELEASE&&
        command!=CERBERUS_AP_SURVEY_COMMAND_STOP&&command!=CERBERUS_AP_SURVEY_COMMAND_STATUS)
        error=ESP_ERR_NOT_FOUND;
    if(error==ESP_OK&&c) {
        if(command==CERBERUS_AP_SURVEY_COMMAND_STOP||command==CERBERUS_AP_SURVEY_COMMAND_RELEASE) {
            c->stop=true;ap_survey_close(c,now);
            if(held)c->state=CERBERUS_AP_SURVEY_STOPPING;
            if(command==CERBERUS_AP_SURVEY_COMMAND_RELEASE)c->release=true;
        } else if(command==CERBERUS_AP_SURVEY_COMMAND_RENEW&&held&&!c->stop) {
            c->lease=(uint64_t)now+(uint64_t)CERBERUS_AP_SURVEY_LEASE_MS*1000U;
        }
        if(!held&&!c->release&&now>=0&&(uint64_t)now>=c->clock_floor)
            c->retention=(uint64_t)now+(uint64_t)CERBERUS_AP_SURVEY_RETENTION_MS*1000U;
    }
    if(command==CERBERUS_AP_SURVEY_COMMAND_SNAPSHOT_BEGIN) {
        cerberus_ap_survey_projection_t *out=reply;out->session_id=session;
        if(error==ESP_OK&&c->clock_invalid)error=ESP_ERR_INVALID_STATE;
        if(error==ESP_OK&&c->projection_counter==UINT32_MAX) {
            error=ESP_ERR_INVALID_STATE;c->projection=0U;ap_survey_fail(c,error,now);
        }
        if(error==ESP_OK) {
            portENTER_CRITICAL(&ap_survey_mux);
            now=esp_timer_get_time();
            bool copied=c->projection_counter!=UINT32_MAX&&now>=0&&
                cerberus_ap_survey_snapshot(&c->table,(uint64_t)now,&c->snapshot);
            if(copied) {
                c->projection=++c->projection_counter;out->projection=c->projection;
                out->length=sizeof(c->snapshot);out->ap_count=c->snapshot.ap_count;
                out->relation_count=c->snapshot.relation_count;out->flags=c->snapshot.flags;
            } else error=ESP_ERR_INVALID_STATE;
            portEXIT_CRITICAL(&ap_survey_mux);
            if(error==ESP_OK&&!cerberus_ap_survey_snapshot_valid(&c->snapshot)) {
                c->projection=0U;memset(out,0,sizeof(*out));out->session_id=session;error=ESP_ERR_INVALID_STATE;
            }
        }
        out->error_code=error;
    } else if(command==CERBERUS_AP_SURVEY_COMMAND_SNAPSHOT_READ) {
        cerberus_ap_survey_read_t q;memcpy(&q,query,sizeof(q));
        cerberus_ap_survey_chunk_t *out=reply;
        if(error==ESP_OK&&(!q.projection||q.projection!=c->projection))error=ESP_ERR_NOT_FOUND;
        if(error==ESP_OK&&(q.offset>=sizeof(c->snapshot)||q.offset%CERBERUS_AP_SURVEY_CHUNK_BYTES))error=ESP_ERR_INVALID_ARG;
        if(error==ESP_OK) {
            out->session_id=session;out->projection=q.projection;out->offset=q.offset;
            size_t count=sizeof(c->snapshot)-q.offset;
            if(count>sizeof(out->data))count=sizeof(out->data);
            memcpy(out->data,(const uint8_t *)&c->snapshot+q.offset,count);
        } else cerberus_ap_survey_read_error(out,session,q.offset,error);
    } else ap_survey_reply(session,false,error,reply);
    xSemaphoreGive(scanner_mutex);
    if(c&&(command==CERBERUS_AP_SURVEY_COMMAND_STOP||command==CERBERUS_AP_SURVEY_COMMAND_RELEASE))xTaskNotifyGive(scanner_task_handle);
    return error;
}

static void ap_survey_receive(const wifi_promiscuous_pkt_t *packet,wifi_promiscuous_pkt_type_t type,
    int64_t entered,uint32_t token)
{
    if(!packet||!token||entered<0||(type!=WIFI_PKT_MGMT&&type!=WIFI_PKT_DATA)||
        packet->rx_ctrl.rx_state||packet->rx_ctrl.sig_len<4U)return;
    uint64_t session=0U;
    portENTER_CRITICAL(&ap_survey_mux);
    ap_survey_context_t *c=ap_survey_context;
    if(c&&c->table.accepting&&token==atomic_load_explicit(&ap_survey_admission,memory_order_acquire)&&
        c->table.arm_token==token&&c->table.live.channel==packet->rx_ctrl.channel&&
        (uint64_t)entered>=c->table.armed_us) {
        session=c->request.session_id;++c->inflight;
    }
    portEXIT_CRITICAL(&ap_survey_mux);
    if(!session)return;
    cerberus_ap_survey_decoded_t decoded;
    (void)cerberus_ap_survey_decode(packet->payload,(size_t)packet->rx_ctrl.sig_len-4U,
        packet->rx_ctrl.channel,packet->rx_ctrl.rssi,(uint64_t)entered,&decoded);
    portENTER_CRITICAL(&ap_survey_mux);
    if(c==ap_survey_context) {
        (void)cerberus_ap_survey_commit(&c->table,session,token,packet->rx_ctrl.channel,(uint64_t)entered,&decoded);
        --c->inflight;
    }
    portEXIT_CRITICAL(&ap_survey_mux);
}
static void ap_survey_finish_pass(ap_survey_context_t *c)
{
    if(c->pass_seen!=c->table.live.effective_channels)return;
    if(c->pass_applied) {
        portENTER_CRITICAL(&ap_survey_mux);cerberus_ap_survey_note_pass(&c->table);portEXIT_CRITICAL(&ap_survey_mux);
        c->pass_seen=c->pass_applied=0U;
    } else ap_survey_fail(c,c->tune_error?c->tune_error:ESP_ERR_NOT_SUPPORTED,esp_timer_get_time());
}
static void ap_survey_worker_tick(void)
{
    xSemaphoreTake(scanner_mutex,portMAX_DELAY);
    ap_survey_context_t *c=ap_survey_context;
    if(!c){xSemaphoreGive(scanner_mutex);return;}
    int64_t now=esp_timer_get_time();(void)ap_survey_expire(c,now);
    bool held=atomic_load_explicit(&ap_survey_radio_reserved,memory_order_acquire);
    if(!held&&c->release&&ap_survey_drained(c)) {
        portENTER_CRITICAL(&ap_survey_mux);ap_survey_context=NULL;portEXIT_CRITICAL(&ap_survey_mux);
        xSemaphoreGive(scanner_mutex);heap_caps_free(c);return;
    }
    if(!held){xSemaphoreGive(scanner_mutex);return;}
    if(c->backoff){--c->backoff;xSemaphoreGive(scanner_mutex);return;}
    portENTER_CRITICAL(&ap_survey_mux);
    bool exhausted=(c->table.live.flags&CERBERUS_AP_SURVEY_SNAPSHOT_EXHAUSTED)!=0U;
    portEXIT_CRITICAL(&ap_survey_mux);
    if(exhausted)ap_survey_fail(c,ESP_ERR_INVALID_STATE,now);
    if(c->stop&&c->phase<AP_SURVEY_DISABLE)c->phase=AP_SURVEY_DISABLE;
    uint8_t phase=c->phase;xSemaphoreGive(scanner_mutex);
    esp_err_t error=ESP_OK;
    if(phase==AP_SURVEY_SETUP) {
        error=esp_wifi_get_band_mode(&c->previous_mode);
        if(error==ESP_OK)error=esp_wifi_get_channel(&c->previous_channel,&c->previous_second);
        if(error==ESP_OK)error=esp_wifi_get_promiscuous_filter(&c->previous_filter);
        if(error==ESP_OK)c->saved=true;
        uint64_t effective=c->request.channel_mask;
        if(error==ESP_OK&&(effective&UINT64_C(0x3fff))) {
            wifi_country_t country={0};error=esp_wifi_get_country(&country);
            unsigned end=(unsigned)country.schan+country.nchan;
            if(error==ESP_OK&&(!country.schan||!country.nchan||country.schan>14U||end>15U))error=ESP_ERR_INVALID_STATE;
            if(error==ESP_OK) {
                uint64_t low=0U;
                for(unsigned channel=country.schan;channel<end;++channel)low|=UINT64_C(1)<<(channel-1U);
                effective&=~UINT64_C(0x3fff)|low;
                if(!effective)error=ESP_ERR_NOT_SUPPORTED;
            }
        }
        if(error==ESP_OK)error=esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
        wifi_promiscuous_filter_t filter={.filter_mask=WIFI_PROMIS_FILTER_MASK_MGMT|WIFI_PROMIS_FILTER_MASK_DATA};
        if(error==ESP_OK)error=esp_wifi_set_promiscuous_filter(&filter);
        xSemaphoreTake(scanner_mutex,portMAX_DELAY);
        if(error==ESP_OK) {
            portENTER_CRITICAL(&ap_survey_mux);
            bool initialized=cerberus_ap_survey_begin(&c->table,c->request.session_id,c->request.channel_mask,effective);
            portEXIT_CRITICAL(&ap_survey_mux);
            if(!initialized)error=ESP_ERR_INVALID_STATE;
        }
        if(error==ESP_OK)c->phase=AP_SURVEY_TUNE;else ap_survey_fail(c,error,esp_timer_get_time());
        xSemaphoreGive(scanner_mutex);return;
    }
    if(phase==AP_SURVEY_TUNE) {
        if(!ap_survey_drained(c))return;
        unsigned index=0U;
        for(unsigned i=0;i<CERBERUS_WIFI_PACKET_COUNT_CHANNELS;++i) {
            index=c->next_index;c->next_index=(uint8_t)((index+1U)%CERBERUS_WIFI_PACKET_COUNT_CHANNELS);
            if(c->table.live.effective_channels&(UINT64_C(1)<<index))break;
        }
        uint8_t channel=cerberus_wifi_packet_count_channel(index);
        error=esp_wifi_set_band(channel<=14U?WIFI_BAND_2G:WIFI_BAND_5G);
        if(error==ESP_OK)error=esp_wifi_set_channel(channel,WIFI_SECOND_CHAN_NONE);
        xSemaphoreTake(scanner_mutex,portMAX_DELAY);now=esp_timer_get_time();(void)ap_survey_expire(c,now);
        c->pass_seen|=UINT64_C(1)<<index;
        portENTER_CRITICAL(&ap_survey_mux);(void)cerberus_ap_survey_note_channel(&c->table,channel,error==ESP_OK);portEXIT_CRITICAL(&ap_survey_mux);
        if(error!=ESP_OK) {
            if(c->tune_error==ESP_OK)c->tune_error=error;
            ap_survey_finish_pass(c);xSemaphoreGive(scanner_mutex);return;
        }
        bool stop=c->stop;xSemaphoreGive(scanner_mutex);if(stop)return;
        if(!c->disable_pending){c->disable_pending=true;error=esp_wifi_set_promiscuous(true);}
        xSemaphoreTake(scanner_mutex,portMAX_DELAY);now=esp_timer_get_time();(void)ap_survey_expire(c,now);
        if(error!=ESP_OK)ap_survey_fail(c,error,now);
        else if(!c->stop) {
            portENTER_CRITICAL(&ap_survey_mux);
            bool armed=ap_survey_arm_counter!=UINT32_MAX&&cerberus_ap_survey_arm(&c->table,c->request.session_id,
                (uint64_t)ap_survey_arm_counter+1U,channel,(uint64_t)now);
            if(armed)atomic_store_explicit(&ap_survey_admission,++ap_survey_arm_counter,memory_order_release);
            portEXIT_CRITICAL(&ap_survey_mux);
            if(!armed)ap_survey_fail(c,ESP_ERR_INVALID_STATE,now);
            else {c->state=CERBERUS_AP_SURVEY_RUNNING;c->phase=AP_SURVEY_ACTIVE;
                c->pass_applied|=UINT64_C(1)<<index;c->next_hop=(uint64_t)now+(uint64_t)c->request.dwell_ms*1000U;}
        }
        xSemaphoreGive(scanner_mutex);return;
    }
    if(phase==AP_SURVEY_ACTIVE) {
        xSemaphoreTake(scanner_mutex,portMAX_DELAY);
        if(!c->stop&&(uint64_t)now>=c->next_hop) {
            if((c->table.live.effective_channels&(c->table.live.effective_channels-1U))==0U) {
                portENTER_CRITICAL(&ap_survey_mux);cerberus_ap_survey_note_pass(&c->table);portEXIT_CRITICAL(&ap_survey_mux);
                c->next_hop=(uint64_t)now+(uint64_t)c->request.dwell_ms*1000U;
            } else {
                ap_survey_close(c,now);ap_survey_finish_pass(c);
                if(!c->stop)c->phase=AP_SURVEY_TUNE;
            }
        }
        xSemaphoreGive(scanner_mutex);return;
    }
    if(phase==AP_SURVEY_DISABLE) {
        if(c->disable_pending){error=esp_wifi_set_promiscuous(false);if(error==ESP_OK)c->disable_pending=false;}
        xSemaphoreTake(scanner_mutex,portMAX_DELAY);
        if(error!=ESP_OK){ap_survey_fail(c,error,now);c->backoff=13U;}else c->phase=AP_SURVEY_DRAIN;
        xSemaphoreGive(scanner_mutex);return;
    }
    if(phase==AP_SURVEY_DRAIN) {
        if(!ap_survey_drained(c))return;
        xSemaphoreTake(scanner_mutex,portMAX_DELAY);c->phase=AP_SURVEY_RESTORE;xSemaphoreGive(scanner_mutex);return;
    }
    if(phase==AP_SURVEY_RESTORE) {
        if(c->saved) {
            error=esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
            if(error==ESP_OK&&c->previous_channel) {
                error=esp_wifi_set_band(c->previous_channel<=14U?WIFI_BAND_2G:WIFI_BAND_5G);
                if(error==ESP_OK)error=esp_wifi_set_channel(c->previous_channel,c->previous_second);
            }
            if(error==ESP_OK)error=esp_wifi_set_promiscuous_filter(&c->previous_filter);
            if(error==ESP_OK)error=esp_wifi_set_band_mode(c->previous_mode);
        }
        xSemaphoreTake(scanner_mutex,portMAX_DELAY);
        if(error!=ESP_OK){ap_survey_fail(c,error,esp_timer_get_time());c->backoff=13U;xSemaphoreGive(scanner_mutex);return;}
        c->state=c->error==ESP_OK?CERBERUS_AP_SURVEY_COMPLETE:CERBERUS_AP_SURVEY_ERROR;
        now=esp_timer_get_time();
        if(now>=0&&(uint64_t)now>=c->clock_floor&&
            (uint64_t)now<=INT64_MAX-(uint64_t)CERBERUS_AP_SURVEY_RETENTION_MS*1000U)
            c->retention=(uint64_t)now+(uint64_t)CERBERUS_AP_SURVEY_RETENTION_MS*1000U;
        else c->release=true;
        atomic_store_explicit(&ap_survey_radio_reserved,false,memory_order_release);
        xSemaphoreGive(scanner_mutex);
    }
}
