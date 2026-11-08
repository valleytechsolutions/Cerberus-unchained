/* Private scanner service. Commands never call SDK or wait for cleanup.
 * Existing Wi-Fi worker owns setup/retune/disposal; RX owns counter updates. */
#include "wifi_packet_count_protocol.h"
#include "wifi_capture_ring.h"

typedef struct {
    cerberus_packet_count_begin_t request;
    cerberus_wifi_packet_count_target_t targets[CERBERUS_PACKET_COUNT_TARGETS];
    cerberus_wifi_packet_count_table_t table;
    cerberus_wifi_capture_ring_t *raw;
    uint8_t pin_bytes[CERBERUS_PACKET_COUNT_SUMMARY_BYTES];
    uint8_t raw_pin_bytes[CERBERUS_PACKET_COUNT_RAW_META_BYTES];
    uint32_t raw_pin,raw_pin_counter;
    uint32_t pin,pin_counter,last_acked,acked,inflight;
    uint16_t configured,pin_length;
    uint64_t deadline,retention,clock_floor,next_hop,channel_armed_us;
    atomic_bool cancelled;
    bool initialized,release_requested,saved,disable_pending,accepted;
    uint8_t state,phase,channel,next_index,backoff;
    wifi_band_mode_t previous_mode;
    uint8_t previous_channel;
    wifi_second_chan_t previous_second;
    wifi_promiscuous_filter_t previous_filter;
    esp_err_t error;
} packet_count_context_t;
static packet_count_context_t *packet_count_context;
static uint64_t packet_count_epoch,packet_count_last_session;
static cerberus_packet_count_begin_t packet_count_last_request;
enum { COUNT_SETUP=0,COUNT_TUNE,COUNT_ACTIVE,COUNT_DISABLE,COUNT_DRAIN,COUNT_RESTORE };

static void packet_count_boot_init(void)
{
    packet_count_epoch=((uint64_t)esp_random()<<32U)|esp_random();
    if(!packet_count_epoch||packet_count_epoch==UINT64_MAX)packet_count_epoch=UINT64_MAX/2U;
}
static bool packet_count_retained(void)
{
    portENTER_CRITICAL(&packet_count_mux);bool retained=packet_count_context!=NULL;
    portEXIT_CRITICAL(&packet_count_mux);return retained;
}
static void packet_count_close(packet_count_context_t *c)
{
    portENTER_CRITICAL(&packet_count_mux);
    c->channel=0U;c->accepted=false;
    /* Exposure ends with callback admission, not a later successful SDK
     * disable. Failed cleanup must not add time in which we reject frames. */
    if(c->initialized&&c->table.window_open)
        (void)cerberus_wifi_packet_count_window_end(&c->table,esp_timer_get_time());
    portEXIT_CRITICAL(&packet_count_mux);
}
static void packet_count_fail(packet_count_context_t *c,esp_err_t error)
{
    if(c->error==ESP_OK)c->error=error;
    atomic_store_explicit(&c->cancelled,true,memory_order_release);
    c->state=CERBERUS_PACKET_COUNT_STOPPING;packet_count_close(c);
}
/* scanner_mutex serializes clock sampling; an older pre-lock sample must not
 * masquerade as a backwards source clock after another command renews. */
static bool packet_count_expire(packet_count_context_t *c,int64_t now)
{
    bool held=atomic_load_explicit(&packet_count_radio_reserved,memory_order_acquire);
    bool bad=now<0||now>INT64_MAX-(int64_t)CERBERUS_PACKET_COUNT_RETENTION_MS*1000LL||(uint64_t)now<c->clock_floor;
    if(bad||(uint64_t)now>=(held?c->deadline:c->retention)){
        if(bad&&c->initialized){portENTER_CRITICAL(&packet_count_mux);
            c->table.summary.flags|=CERBERUS_WIFI_PACKET_COUNT_CLOCK_INVALID;portEXIT_CRITICAL(&packet_count_mux);}
        if(held&&!atomic_load_explicit(&c->cancelled,memory_order_acquire))
            packet_count_fail(c,bad?ESP_ERR_INVALID_STATE:ESP_ERR_TIMEOUT);
        else if(!held)c->release_requested=true;
        return true;
    }
    c->clock_floor=(uint64_t)now;return false;
}
static void packet_count_reply(uint64_t session,bool info,esp_err_t error,cerberus_packet_count_control_t *out)
{
    memset(out,0,sizeof(*out));out->session_id=info?packet_count_epoch:session;out->error_code=error;
    packet_count_context_t *c=packet_count_context;
    if(!c||(!info&&session!=c->request.session_id))return;
    out->session_id=c->request.session_id;out->error_code=error==ESP_OK?c->error:error;
    out->configured=c->configured;out->state=c->state;out->count=c->request.target_count;
    out->flags=CERBERUS_PACKET_COUNT_BOUND;
    if(atomic_load_explicit(&packet_count_radio_reserved,memory_order_acquire))out->flags|=CERBERUS_PACKET_COUNT_HELD;
    if(c->request.record)out->flags|=CERBERUS_PACKET_COUNT_RECORDING;
    portENTER_CRITICAL(&packet_count_mux);
    out->channel=c->channel;out->sequence=c->table.summary.revision;
    if(c->accepted&&c->channel)out->flags|=CERBERUS_PACKET_COUNT_RX_ACTIVE;
    if(c->initialized&&!c->release_requested)out->flags|=CERBERUS_PACKET_COUNT_DATA_READY;
    if(c->table.summary.flags&CERBERUS_WIFI_PACKET_COUNT_CLOCK_INVALID)out->flags|=CERBERUS_PACKET_COUNT_CLOCK_INVALID;
    if(c->table.summary.flags&CERBERUS_WIFI_PACKET_COUNT_SATURATED)out->flags|=CERBERUS_PACKET_COUNT_SATURATED;
    if(c->pin_counter==UINT32_MAX||c->raw_pin_counter==UINT32_MAX-1U||c->table.summary.revision==UINT32_MAX||
       (c->raw&&atomic_load_explicit(&c->raw->next_sequence,memory_order_acquire)>=CERBERUS_PACKET_COUNT_SEQUENCE_MAX))
        out->flags|=CERBERUS_PACKET_COUNT_EXHAUSTED;
    portEXIT_CRITICAL(&packet_count_mux);
}
static void packet_count_raw_stats(packet_count_context_t *c,cerberus_packet_count_record_stats_t *out)
{
    memset(out,0,sizeof(*out));if(!c->raw)return;
    cerberus_wifi_capture_ring_stats_t s;cerberus_wifi_capture_ring_stats(c->raw,&s);
    out->accepted=s.accepted;out->acked=c->acked;out->pending=s.pending;out->dropped_full=s.dropped_full;
    uint64_t other=(uint64_t)s.dropped_stopped+s.dropped_stale+s.dropped_oversize+s.dropped_truncated+s.dropped_invalid;
    out->dropped_other=other>UINT32_MAX?UINT32_MAX:(uint32_t)other;
    out->last_sequence=atomic_load_explicit(&c->raw->next_sequence,memory_order_acquire);
}
static void packet_count_receive(const wifi_promiscuous_pkt_t *p,wifi_promiscuous_pkt_type_t type,int64_t now)
{
    if(!p||(type!=WIFI_PKT_MGMT&&type!=WIFI_PKT_DATA))return;
    unsigned kind=type==WIFI_PKT_MGMT?CERBERUS_WIFI_PACKET_COUNT_MGMT:
        CERBERUS_WIFI_PACKET_COUNT_DATA;
    /* ESP-IDF v6.0.1: esp_wifi_types_native.h defines payload extent by
     * sig_len; examples/network/simple_sniffer/main/cmd_sniffer.c excludes
     * MISC/error callbacks and strips four FCS bytes. dump_len equality is
     * not part of that callback contract. Never read a MISC/CTRL payload. */
    size_t length=p->rx_ctrl.sig_len>=4U?p->rx_ctrl.sig_len-4U:0U;
    bool complete=p->rx_ctrl.rx_state==0U&&p->rx_ctrl.sig_len>=4U;
    portENTER_CRITICAL(&packet_count_mux);
    packet_count_context_t *c=packet_count_context;
    if(!c||!c->channel||(now>=0&&(uint64_t)now<c->channel_armed_us)){
        portEXIT_CRITICAL(&packet_count_mux);return;}
    ++c->inflight;uint64_t session=c->request.session_id;
    /* Admission and the counter update are one bounded critical section.
     * Stop cannot advance the model clock between these operations and make
     * an already admitted callback appear to have a backwards timestamp. */
    if(!c->table.window_open&&c->channel==p->rx_ctrl.channel&&complete)
        (void)cerberus_wifi_packet_count_window_begin(&c->table,c->channel,now);
    uint16_t matched=cerberus_wifi_packet_count_observe(&c->table,p->payload,
        complete?length:0U,kind,complete,p->rx_ctrl.channel,p->rx_ctrl.rssi,now);
    portEXIT_CRITICAL(&packet_count_mux);
    if(matched&&c->raw){
        unsigned seq=atomic_load_explicit(&c->raw->next_sequence,memory_order_acquire);
        if(seq<CERBERUS_PACKET_COUNT_SEQUENCE_MAX){
            cerberus_wifi_pcap_metadata_t meta={.channel=p->rx_ctrl.channel,.rssi_dbm=p->rx_ctrl.rssi,
                .rssi_known=p->rx_ctrl.rssi>=-127&&p->rx_ctrl.rssi<=0,.time_kind=CERBERUS_WIFI_PCAP_TIME_MONOTONIC,.capture_time_us=(uint64_t)now};
            (void)cerberus_wifi_capture_ring_push(c->raw,1U,&meta,p->payload,length,length);
        }else {
            unsigned dropped=atomic_load_explicit(&c->raw->dropped_invalid,memory_order_relaxed);
            if(dropped!=UINT32_MAX)atomic_store_explicit(&c->raw->dropped_invalid,dropped+1U,memory_order_relaxed);
            atomic_store_explicit(&c->cancelled,true,memory_order_release);
        }
    }
    portENTER_CRITICAL(&packet_count_mux);
    if(packet_count_context==c&&c->request.session_id==session)--c->inflight;
    portEXIT_CRITICAL(&packet_count_mux);
}
static bool packet_count_drained(packet_count_context_t *c)
{
    portENTER_CRITICAL(&packet_count_mux);bool done=c->inflight==0U;portEXIT_CRITICAL(&packet_count_mux);return done;
}
static bool packet_count_begin_valid(const cerberus_packet_count_begin_t *q)
{
    return q&&q->session_id&&!q->reserved&&q->target_count&&q->target_count<=CERBERUS_PACKET_COUNT_TARGETS&&q->record<=1U&&
        cerberus_wifi_packet_count_mask_valid(q->channel_mask)&&q->dwell_ms>=CERBERUS_WIFI_PACKET_COUNT_DWELL_MIN_MS&&
        q->dwell_ms<=CERBERUS_WIFI_PACKET_COUNT_DWELL_MAX_MS;
}
static esp_err_t packet_count_begin(const cerberus_packet_count_begin_t *q,cerberus_packet_count_control_t *out)
{
    xSemaphoreTake(scanner_mutex,portMAX_DELAY);
    if(!packet_count_begin_valid(q)){packet_count_reply(q->session_id,false,ESP_ERR_INVALID_ARG,out);
        xSemaphoreGive(scanner_mutex);return ESP_ERR_INVALID_ARG;}
    if(packet_count_context)(void)packet_count_expire(packet_count_context,esp_timer_get_time());
    if(q->session_id==packet_count_last_session){
        esp_err_t e=memcmp(q,&packet_count_last_request,sizeof(*q))?ESP_ERR_INVALID_ARG:
            packet_count_context?ESP_OK:ESP_ERR_NOT_FOUND;
        packet_count_reply(q->session_id,false,e,out);xSemaphoreGive(scanner_mutex);return e;
    }
    bool allowed=!packet_count_context&&packet_count_epoch!=UINT64_MAX&&q->session_id==packet_count_epoch+1U;
    xSemaphoreGive(scanner_mutex);
    if(!allowed){memset(out,0,sizeof(*out));out->session_id=q->session_id;out->error_code=ESP_ERR_INVALID_STATE;return ESP_ERR_INVALID_STATE;}
    packet_count_context_t *c=heap_caps_calloc(1U,sizeof(*c),MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if(c&&q->record)c->raw=heap_caps_calloc(1U,sizeof(*c->raw),MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    esp_err_t error=!c||(q->record&&!c->raw)?ESP_ERR_NO_MEM:ESP_OK;
    xSemaphoreTake(scanner_mutex,portMAX_DELAY);
    int64_t now=esp_timer_get_time();
    if(error==ESP_OK&&(!scanner_ready||packet_count_context||q->session_id!=packet_count_epoch+1U||
       station_reserved||scan_status.state==CERBERUS_WIFI_SCAN_RUNNING||defense_status.state==CERBERUS_WIFI_DEFENSE_RUNNING||
       channel_survey_status.state==CERBERUS_WIFI_CHANNEL_SURVEY_RUNNING||atomic_load_explicit(&channel_survey_radio_reserved,memory_order_acquire)||
       wifi_connection_is_active_locked()||cerberus_c5_test_running()||another_radio_operation_is_active()))error=ESP_ERR_INVALID_STATE;
    if(error==ESP_OK&&(now<0||now>INT64_MAX-(int64_t)CERBERUS_PACKET_COUNT_RETENTION_MS*1000LL))error=ESP_ERR_INVALID_STATE;
    if(error==ESP_OK){
        c->request=*q;c->state=CERBERUS_PACKET_COUNT_STAGING;c->pin_counter=CERBERUS_PACKET_COUNT_PIN_BASE-1U;c->raw_pin_counter=CERBERUS_PACKET_COUNT_PIN_BASE;
        c->clock_floor=(uint64_t)now;c->deadline=(uint64_t)now+(uint64_t)CERBERUS_PACKET_COUNT_LEASE_MS*1000U;
        atomic_init(&c->cancelled,false);if(c->raw)cerberus_wifi_capture_ring_init(c->raw);
        packet_count_epoch=packet_count_last_session=q->session_id;packet_count_last_request=*q;
        portENTER_CRITICAL(&packet_count_mux);packet_count_context=c;portEXIT_CRITICAL(&packet_count_mux);
        atomic_store_explicit(&packet_count_radio_reserved,true,memory_order_release);
    }
    packet_count_reply(q->session_id,false,error,out);xSemaphoreGive(scanner_mutex);
    if(error!=ESP_OK&&c){heap_caps_free(c->raw);heap_caps_free(c);}
    if(error==ESP_OK)xTaskNotifyGive(scanner_task_handle);
    return error;
}
static esp_err_t packet_count_pin(packet_count_context_t *c,const cerberus_packet_count_peek_t *q,int64_t now)
{
    if(!c->initialized||c->release_requested)return ESP_ERR_INVALID_STATE;
    bool raw_kind=q->kind==CERBERUS_PACKET_COUNT_READ_RAW_META;
    if((raw_kind?c->raw_pin_counter:c->pin_counter)>UINT32_MAX-2U){packet_count_fail(c,ESP_ERR_INVALID_STATE);return ESP_ERR_INVALID_STATE;}
    bool ok=false;uint16_t length=0;
    if(q->kind==CERBERUS_PACKET_COUNT_READ_RAW_META){
        if(q->index||!c->raw)return ESP_ERR_INVALID_ARG;
        const cerberus_wifi_capture_record_t *r=cerberus_wifi_capture_ring_peek(c->raw);
        if(!r)return ESP_ERR_NOT_FOUND;
        memset(c->raw_pin_bytes,0,sizeof(c->raw_pin_bytes));
        cerberus_packet_count_put(c->raw_pin_bytes,r->sequence,4U);cerberus_packet_count_put(c->raw_pin_bytes+4,r->captured_length,2U);
        cerberus_packet_count_put(c->raw_pin_bytes+6,r->original_length,2U);cerberus_packet_count_put(c->raw_pin_bytes+8,r->metadata.capture_time_us,8U);
        cerberus_packet_count_put(c->raw_pin_bytes+16,cerberus_protocol_crc32(r->frame,r->captured_length),4U);
        c->raw_pin_bytes[20]=r->metadata.channel;c->raw_pin_bytes[21]=r->metadata.rssi_known?(uint8_t)r->metadata.rssi_dbm:0U;
        c->raw_pin_bytes[22]=(uint8_t)(1U|(r->metadata.rssi_known?2U:0U));length=CERBERUS_PACKET_COUNT_RAW_META_BYTES;ok=true;
    }else{
        cerberus_packet_count_record_stats_t raw={0};
        portENTER_CRITICAL(&packet_count_mux);
        /* RX may advance the table after the command sampled its clock. Sample
         * again under the model lock. A raw producer publishes several atomics;
         * do not present a mixed queue/counter summary mid-publication. */
        now=esp_timer_get_time();
        if(q->kind==CERBERUS_PACKET_COUNT_READ_SUMMARY&&!q->index){
            if(c->raw&&c->inflight){portEXIT_CRITICAL(&packet_count_mux);return ESP_ERR_NOT_FINISHED;}
            packet_count_raw_stats(c,&raw);
            length=CERBERUS_PACKET_COUNT_SUMMARY_BYTES;ok=cerberus_packet_count_encode_summary(&c->table,now,c->state,c->channel,
                c->request.record,c->error,c->configured,&raw,c->pin_bytes);
        }else if(q->kind==CERBERUS_PACKET_COUNT_READ_ROW){length=CERBERUS_PACKET_COUNT_ROW_BYTES;
            ok=cerberus_packet_count_encode_row(&c->table,q->index,now,c->pin_bytes);
        }else if(q->kind==CERBERUS_PACKET_COUNT_READ_CHANNEL){length=CERBERUS_PACKET_COUNT_CHANNEL_BYTES;
            ok=cerberus_packet_count_encode_channel(&c->table,q->index,now,c->pin_bytes);}
        portEXIT_CRITICAL(&packet_count_mux);
    }
    if(!ok)return ESP_ERR_INVALID_ARG;
    if(raw_kind){c->raw_pin_counter+=2U;c->raw_pin=c->raw_pin_counter;}
    else {c->pin_counter+=2U;c->pin=c->pin_counter;c->pin_length=length;}
    return ESP_OK;
}
/* Public single-dispatch boundary shared with the independent S3 producer.
 * All request sizes checked before reading; all replies canonical on error. */
esp_err_t cerberus_wifi_packet_count_exchange(uint8_t command,const void *request,size_t request_size,void *reply,size_t reply_size)
{
    size_t expected=sizeof(cerberus_packet_count_request_t),output=sizeof(cerberus_packet_count_control_t);
    switch(command){
    case CERBERUS_PACKET_COUNT_COMMAND_INFO:expected=0;break;
    case CERBERUS_PACKET_COUNT_COMMAND_BEGIN:expected=sizeof(cerberus_packet_count_begin_t);break;
    case CERBERUS_PACKET_COUNT_COMMAND_TARGET:expected=sizeof(cerberus_packet_count_target_t);break;
    case CERBERUS_PACKET_COUNT_COMMAND_PEEK:expected=sizeof(cerberus_packet_count_peek_t);break;
    case CERBERUS_PACKET_COUNT_COMMAND_READ:expected=sizeof(cerberus_packet_count_read_t);output=sizeof(cerberus_packet_count_chunk_t);break;
    case CERBERUS_PACKET_COUNT_COMMAND_RAW_READ:expected=sizeof(cerberus_packet_count_read_t);output=sizeof(cerberus_packet_count_raw_chunk_t);break;
    case CERBERUS_PACKET_COUNT_COMMAND_RAW_ACK:expected=sizeof(cerberus_packet_count_ack_t);break;
    case CERBERUS_PACKET_COUNT_COMMAND_START:case CERBERUS_PACKET_COUNT_COMMAND_STATUS:
    case CERBERUS_PACKET_COUNT_COMMAND_STOP:case CERBERUS_PACKET_COUNT_COMMAND_RELEASE:break;
    default:return ESP_ERR_NOT_SUPPORTED;
    }
    if(!reply||reply_size!=output||request_size!=expected||(expected&&!request))return ESP_ERR_INVALID_ARG;
    if(expected){uintptr_t a=(uintptr_t)request,b=(uintptr_t)reply;
        if(a>UINTPTR_MAX-expected||b>UINTPTR_MAX-output||(a<b+output&&b<a+expected))return ESP_ERR_INVALID_ARG;}
    memset(reply,0,output);uint64_t session=0;if(expected)memcpy(&session,request,8U);
    if(!scanner_mutex||!scanner_task_handle){
        memcpy(reply,&session,8U);
        if(command==CERBERUS_PACKET_COUNT_COMMAND_READ||command==CERBERUS_PACKET_COUNT_COMMAND_RAW_READ){
            memcpy((uint8_t *)reply+12U,(const uint8_t *)request+12U,2U);
            cerberus_packet_count_put((uint8_t *)reply+14U,(uint32_t)ESP_ERR_INVALID_STATE,4U);
        }else ((cerberus_packet_count_control_t *)reply)->error_code=ESP_ERR_INVALID_STATE;
        return ESP_ERR_INVALID_STATE;
    }
    if(command==CERBERUS_PACKET_COUNT_COMMAND_BEGIN){cerberus_packet_count_begin_t q;memcpy(&q,request,sizeof(q));return packet_count_begin(&q,reply);}
    xSemaphoreTake(scanner_mutex,portMAX_DELAY);
    packet_count_context_t *c=packet_count_context;
    int64_t now=esp_timer_get_time();esp_err_t error=ESP_OK;
    bool info=command==CERBERUS_PACKET_COUNT_COMMAND_INFO;
    bool bound=c&&session&&session==c->request.session_id;
    if(!info&&!bound)error=session&&command==CERBERUS_PACKET_COUNT_COMMAND_RELEASE&&session==packet_count_last_session?ESP_OK:ESP_ERR_NOT_FOUND;
    if(bound&&!info)(void)packet_count_expire(c,now);
    uint32_t ack=0;
    if(bound){
        if(command==CERBERUS_PACKET_COUNT_COMMAND_TARGET){
            cerberus_packet_count_target_t q;memcpy(&q,request,sizeof(q));
            cerberus_wifi_packet_count_target_t value={.role=q.role,.channel_hint=q.channel_hint};memcpy(value.mac,q.mac,6U);
            if(q.reserved||q.index>=c->request.target_count||!cerberus_wifi_packet_count_target_valid(&value))error=ESP_ERR_INVALID_ARG;
            else if(c->configured&(1U<<q.index))error=memcmp(&value,&c->targets[q.index],sizeof(value))?ESP_ERR_INVALID_ARG:ESP_OK;
            else if(c->state!=CERBERUS_PACKET_COUNT_STAGING||atomic_load_explicit(&c->cancelled,memory_order_acquire))error=ESP_ERR_INVALID_STATE;
            else {for(unsigned i=0;i<c->request.target_count;++i)if((c->configured&(1U<<i))&&c->targets[i].role==value.role&&!memcmp(c->targets[i].mac,value.mac,6U))error=ESP_ERR_INVALID_ARG;
                if(error==ESP_OK){c->targets[q.index]=value;c->configured|=(uint16_t)(1U<<q.index);}}
        }else if(command==CERBERUS_PACKET_COUNT_COMMAND_START){
            if(c->initialized){/* exact START replay cannot rearm */}
            else if(c->state!=CERBERUS_PACKET_COUNT_STAGING||atomic_load_explicit(&c->cancelled,memory_order_acquire)||
                c->configured!=(uint16_t)((UINT32_C(1)<<c->request.target_count)-1U))error=ESP_ERR_INVALID_STATE;
            else if(!cerberus_wifi_packet_count_init(&c->table,c->targets,c->request.target_count,c->request.channel_mask,c->request.dwell_ms))error=ESP_ERR_INVALID_ARG;
            else {c->initialized=true;c->state=CERBERUS_PACKET_COUNT_QUEUED;c->phase=COUNT_SETUP;
                if(c->raw&&!cerberus_wifi_capture_ring_start(c->raw,1U)){packet_count_fail(c,ESP_ERR_INVALID_STATE);error=ESP_ERR_INVALID_STATE;}}
        }else if(command==CERBERUS_PACKET_COUNT_COMMAND_STOP||command==CERBERUS_PACKET_COUNT_COMMAND_RELEASE){
            if(command==CERBERUS_PACKET_COUNT_COMMAND_RELEASE){c->release_requested=true;c->pin=c->raw_pin=0;}
            if(atomic_load_explicit(&packet_count_radio_reserved,memory_order_acquire))packet_count_fail(c,ESP_OK);
        }else if(command==CERBERUS_PACKET_COUNT_COMMAND_STATUS){
            if(!c->release_requested&&now>=0&&!atomic_load_explicit(&c->cancelled,memory_order_acquire)&&
               (uint64_t)now<c->deadline)c->deadline=(uint64_t)now+(uint64_t)CERBERUS_PACKET_COUNT_LEASE_MS*1000U;
        }else if(command==CERBERUS_PACKET_COUNT_COMMAND_PEEK){cerberus_packet_count_peek_t q;memcpy(&q,request,sizeof(q));error=packet_count_pin(c,&q,now);
        }else if(command==CERBERUS_PACKET_COUNT_COMMAND_RAW_ACK){
            cerberus_packet_count_ack_t q;memcpy(&q,request,sizeof(q));
            if(!c->raw||!q.sequence||c->release_requested)error=ESP_ERR_INVALID_STATE;
            else if(q.sequence==c->last_acked)ack=q.sequence;
            else if(cerberus_wifi_capture_ring_ack(c->raw,1U,q.sequence)){c->last_acked=ack=q.sequence;if(c->acked!=UINT32_MAX)++c->acked;}
            else error=ESP_ERR_INVALID_STATE;
        }
    }
    if(command==CERBERUS_PACKET_COUNT_COMMAND_READ||command==CERBERUS_PACKET_COUNT_COMMAND_RAW_READ){
        cerberus_packet_count_read_t q;memcpy(&q,request,sizeof(q));
        uint8_t *bytes=reply;memcpy(bytes,&q.session_id,8U);memcpy(bytes+12U,&q.part,2U);
        const uint8_t *data=NULL;size_t length=0,chunk=command==CERBERUS_PACKET_COUNT_COMMAND_READ?8U:40U;
        if(bound&&error==ESP_OK&&!c->release_requested){
            if(command==CERBERUS_PACKET_COUNT_COMMAND_READ&&q.pin&&q.pin==c->pin){data=c->pin_bytes;length=c->pin_length;}
            else if(command==CERBERUS_PACKET_COUNT_COMMAND_READ&&q.pin&&q.pin==c->raw_pin){data=c->raw_pin_bytes;length=sizeof(c->raw_pin_bytes);}
            else if(command==CERBERUS_PACKET_COUNT_COMMAND_RAW_READ&&c->raw){const cerberus_wifi_capture_record_t *r=cerberus_wifi_capture_ring_peek(c->raw);
                if(r&&r->sequence==q.pin){data=r->frame;length=r->captured_length;}}
        }
        size_t offset=(size_t)q.part*chunk;
        if(!data||offset>=length)error=ESP_ERR_INVALID_STATE;
        if(error==ESP_OK){memcpy(bytes+8U,&q.pin,4U);size_t n=length-offset;if(n>chunk)n=chunk;memcpy(bytes+14U,data+offset,n);}
        else cerberus_packet_count_put(bytes+14U,(uint32_t)error,4U);
    }else{
        packet_count_reply(session,info,error,reply);
        cerberus_packet_count_control_t *out=reply;
        if(command==CERBERUS_PACKET_COUNT_COMMAND_PEEK){cerberus_packet_count_peek_t q;memcpy(&q,request,sizeof(q));
            out->sequence=error==ESP_OK&&bound?(q.kind==CERBERUS_PACKET_COUNT_READ_RAW_META?c->raw_pin:c->pin):0U;}
        if(command==CERBERUS_PACKET_COUNT_COMMAND_RAW_ACK)out->sequence=error==ESP_OK?ack:0U;
        if(info&&!c&&(another_radio_operation_is_active()||station_reserved||
            scan_status.state==CERBERUS_WIFI_SCAN_RUNNING||defense_status.state==CERBERUS_WIFI_DEFENSE_RUNNING||
            channel_survey_status.state==CERBERUS_WIFI_CHANNEL_SURVEY_RUNNING||
            atomic_load_explicit(&channel_survey_radio_reserved,memory_order_acquire)||wifi_connection_is_active_locked())){
            out->flags=CERBERUS_PACKET_COUNT_HELD;out->state=CERBERUS_PACKET_COUNT_QUEUED;}
    }
    if(bound&&error==ESP_OK&&!c->release_requested&&!atomic_load_explicit(&packet_count_radio_reserved,memory_order_acquire)&&
        now>=0&&(uint64_t)now<c->retention&&(command==CERBERUS_PACKET_COUNT_COMMAND_STATUS||command==CERBERUS_PACKET_COUNT_COMMAND_PEEK||
        command==CERBERUS_PACKET_COUNT_COMMAND_READ||command==CERBERUS_PACKET_COUNT_COMMAND_RAW_READ))
        c->retention=(uint64_t)now+(uint64_t)CERBERUS_PACKET_COUNT_RETENTION_MS*1000U;
    xSemaphoreGive(scanner_mutex);
    if(bound&&(command==CERBERUS_PACKET_COUNT_COMMAND_START||command==CERBERUS_PACKET_COUNT_COMMAND_STOP||command==CERBERUS_PACKET_COUNT_COMMAND_RELEASE))xTaskNotifyGive(scanner_task_handle);
    return error;
}

static void packet_count_worker_tick(void)
{
    xSemaphoreTake(scanner_mutex,portMAX_DELAY);
    packet_count_context_t *c=packet_count_context;
    if(!c){xSemaphoreGive(scanner_mutex);return;}
    int64_t now=esp_timer_get_time();(void)packet_count_expire(c,now);
    bool held=atomic_load_explicit(&packet_count_radio_reserved,memory_order_acquire);
    if(!held&&c->release_requested&&packet_count_drained(c)){
        portENTER_CRITICAL(&packet_count_mux);packet_count_context=NULL;portEXIT_CRITICAL(&packet_count_mux);
        xSemaphoreGive(scanner_mutex);heap_caps_free(c->raw);heap_caps_free(c);return;
    }
    if(!held){xSemaphoreGive(scanner_mutex);return;}
    if(c->backoff){--c->backoff;xSemaphoreGive(scanner_mutex);return;}
    portENTER_CRITICAL(&packet_count_mux);
    bool clock_failed=(c->table.summary.flags&CERBERUS_WIFI_PACKET_COUNT_CLOCK_INVALID)!=0U;
    portEXIT_CRITICAL(&packet_count_mux);
    if(clock_failed)packet_count_fail(c,ESP_ERR_INVALID_STATE);
    if(c->raw&&atomic_load_explicit(&c->raw->next_sequence,memory_order_acquire)>=CERBERUS_PACKET_COUNT_SEQUENCE_MAX)
        packet_count_fail(c,ESP_ERR_INVALID_STATE);
    bool stop=atomic_load_explicit(&c->cancelled,memory_order_acquire);
    if(!c->initialized){
        if(stop){c->state=c->error==ESP_OK?CERBERUS_PACKET_COUNT_COMPLETE:CERBERUS_PACKET_COUNT_ERROR;
            atomic_store_explicit(&packet_count_radio_reserved,false,memory_order_release);
            c->retention=now>=0?(uint64_t)now+(uint64_t)CERBERUS_PACKET_COUNT_RETENTION_MS*1000U:0U;}
        xSemaphoreGive(scanner_mutex);return;
    }
    if(stop&&c->phase<COUNT_DISABLE)c->phase=COUNT_DISABLE;
    uint8_t phase=c->phase;xSemaphoreGive(scanner_mutex);
    esp_err_t error=ESP_OK;
    if(phase==COUNT_SETUP){
        error=esp_wifi_get_band_mode(&c->previous_mode);
        if(error==ESP_OK)error=esp_wifi_get_channel(&c->previous_channel,&c->previous_second);
        if(error==ESP_OK)error=esp_wifi_get_promiscuous_filter(&c->previous_filter);
        if(error==ESP_OK)c->saved=true;
        if(error==ESP_OK)error=esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
        wifi_promiscuous_filter_t filter={.filter_mask=WIFI_PROMIS_FILTER_MASK_MGMT|WIFI_PROMIS_FILTER_MASK_DATA};
        if(error==ESP_OK)error=esp_wifi_set_promiscuous_filter(&filter);
        xSemaphoreTake(scanner_mutex,portMAX_DELAY);if(error==ESP_OK)c->phase=COUNT_TUNE;else packet_count_fail(c,error);
        xSemaphoreGive(scanner_mutex);return;
    }
    if(phase==COUNT_TUNE){
        uint8_t channel=0;
        for(unsigned i=0;i<CERBERUS_WIFI_PACKET_COUNT_CHANNELS;++i){unsigned index=c->next_index;
            c->next_index=(uint8_t)((index+1U)%CERBERUS_WIFI_PACKET_COUNT_CHANNELS);
            if(c->request.channel_mask&(UINT64_C(1)<<index)){channel=cerberus_wifi_packet_count_channel(index);break;}}
        error=esp_wifi_set_band(channel<=14U?WIFI_BAND_2G:WIFI_BAND_5G);
        if(error==ESP_OK)error=esp_wifi_set_channel(channel,WIFI_SECOND_CHAN_NONE);
        xSemaphoreTake(scanner_mutex,portMAX_DELAY);
        now=esp_timer_get_time();(void)packet_count_expire(c,now);
        portENTER_CRITICAL(&packet_count_mux);cerberus_wifi_packet_count_attempt(&c->table,channel,error==ESP_OK);
        if(error==ESP_OK&&!atomic_load_explicit(&c->cancelled,memory_order_acquire)){
            c->channel=channel;c->channel_armed_us=(uint64_t)now;}
        portEXIT_CRITICAL(&packet_count_mux);
        bool cancelled=atomic_load_explicit(&c->cancelled,memory_order_acquire);
        if(error!=ESP_OK)packet_count_fail(c,error);
        xSemaphoreGive(scanner_mutex);if(error!=ESP_OK||cancelled)return;
        c->disable_pending=true;error=esp_wifi_set_promiscuous(true);
        xSemaphoreTake(scanner_mutex,portMAX_DELAY);
        now=esp_timer_get_time();(void)packet_count_expire(c,now);
        if(error!=ESP_OK)packet_count_fail(c,error);
        else if(!atomic_load_explicit(&c->cancelled,memory_order_acquire)){
            portENTER_CRITICAL(&packet_count_mux);
            bool window=c->table.window_open||cerberus_wifi_packet_count_window_begin(&c->table,channel,now);
            c->accepted=window;portEXIT_CRITICAL(&packet_count_mux);
            if(!window)packet_count_fail(c,ESP_ERR_INVALID_STATE);
            else {c->state=CERBERUS_PACKET_COUNT_RUNNING;c->phase=COUNT_ACTIVE;
                c->next_hop=(uint64_t)now+(uint64_t)c->request.dwell_ms*1000U;}}
        xSemaphoreGive(scanner_mutex);return;
    }
    if(phase==COUNT_ACTIVE){
        xSemaphoreTake(scanner_mutex,portMAX_DELAY);
        portENTER_CRITICAL(&packet_count_mux);bool exhausted=c->table.summary.revision==UINT32_MAX;portEXIT_CRITICAL(&packet_count_mux);
        if(exhausted)packet_count_fail(c,ESP_ERR_INVALID_STATE);
        if(atomic_load_explicit(&c->cancelled,memory_order_acquire)||((c->request.channel_mask&(c->request.channel_mask-1U))&&(uint64_t)now>=c->next_hop)){
            packet_count_close(c);c->phase=COUNT_DISABLE;}
        xSemaphoreGive(scanner_mutex);return;
    }
    if(phase==COUNT_DISABLE){
        if(c->disable_pending){error=esp_wifi_set_promiscuous(false);if(error==ESP_OK)c->disable_pending=false;}
        xSemaphoreTake(scanner_mutex,portMAX_DELAY);
        if(error!=ESP_OK){packet_count_fail(c,error);c->backoff=13U;}else c->phase=COUNT_DRAIN;
        xSemaphoreGive(scanner_mutex);return;
    }
    if(phase==COUNT_DRAIN){
        if(!packet_count_drained(c))return;
        xSemaphoreTake(scanner_mutex,portMAX_DELAY);now=esp_timer_get_time();
        portENTER_CRITICAL(&packet_count_mux);
        if(c->table.window_open)(void)cerberus_wifi_packet_count_window_end(&c->table,now);
        portEXIT_CRITICAL(&packet_count_mux);
        if(!atomic_load_explicit(&c->cancelled,memory_order_acquire))c->phase=COUNT_TUNE;
        else {if(c->raw)cerberus_wifi_capture_ring_stop(c->raw);c->phase=COUNT_RESTORE;}
        xSemaphoreGive(scanner_mutex);return;
    }
    if(phase==COUNT_RESTORE){
        if(c->saved){error=esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
            if(error==ESP_OK&&c->previous_channel){error=esp_wifi_set_band(c->previous_channel<=14U?WIFI_BAND_2G:WIFI_BAND_5G);
                if(error==ESP_OK)error=esp_wifi_set_channel(c->previous_channel,c->previous_second);}
            if(error==ESP_OK)error=esp_wifi_set_promiscuous_filter(&c->previous_filter);
            if(error==ESP_OK)error=esp_wifi_set_band_mode(c->previous_mode);}
        xSemaphoreTake(scanner_mutex,portMAX_DELAY);
        if(error!=ESP_OK){packet_count_fail(c,error);c->backoff=13U;xSemaphoreGive(scanner_mutex);return;}
        now=esp_timer_get_time();portENTER_CRITICAL(&packet_count_mux);cerberus_wifi_packet_count_finish(&c->table,now);portEXIT_CRITICAL(&packet_count_mux);
        c->state=c->error==ESP_OK?CERBERUS_PACKET_COUNT_COMPLETE:CERBERUS_PACKET_COUNT_ERROR;
        c->retention=now>=0?(uint64_t)now+(uint64_t)CERBERUS_PACKET_COUNT_RETENTION_MS*1000U:0U;
        atomic_store_explicit(&packet_count_radio_reserved,false,memory_order_release);
        xSemaphoreGive(scanner_mutex);
    }
}
