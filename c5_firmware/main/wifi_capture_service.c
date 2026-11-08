#include "wifi_capture_service.h"
#include "wifi_capture_filter.h"
#include "wifi_capture_ring.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* Allocated on first capture only. Retaining the small ring through later
 * sessions avoids freeing storage while a delayed Wi-Fi callback may hold it. */
static cerberus_wifi_capture_ring_t *capture_ring;
static cerberus_wifi_capture_start_request_t current_request;
static uint32_t last_generation,last_acked_sequence;
static atomic_uint deadline_ms;
static int32_t last_error;
static uint8_t state;
static atomic_bool receiving;
static atomic_uint callback_inflight;
static atomic_uint context_skipped;
static atomic_uchar receive_channel;
static atomic_uint receive_token;
static uint32_t last_admission_token;
/* Written only with admission closed and its callbacks drained. Publishing
 * receive_token makes this arm boundary visible to the RX producer. */
static int64_t receive_armed_us;
static cerberus_wifi_capture_plan_t staged_plan;
static uint32_t staged_generation,staged_present;

static uint32_t plan_target_mask(uint8_t count)
{return count==32U?UINT32_MAX:(UINT32_C(1)<<count)-1U;}

static void plan_status(cerberus_wifi_capture_plan_status_payload_t *out,esp_err_t error)
{
    memset(out,0,sizeof(*out));
    out->generation=staged_generation;out->channel_mask=staged_plan.channel_mask;
    out->target_count=staged_plan.target_count;out->target_present_mask=staged_present;
    out->state=!staged_generation?CERBERUS_WIFI_CAPTURE_PLAN_EMPTY:
        staged_present==plan_target_mask(staged_plan.target_count)?
            CERBERUS_WIFI_CAPTURE_PLAN_READY:CERBERUS_WIFI_CAPTURE_PLAN_STAGING;
    out->error_code=error;
}

static bool plan_mutable(bool radio_held)
{
    if(radio_held||cerberus_wifi_capture_service_active()||
       cerberus_wifi_capture_service_inflight())return false;
    if(capture_ring){
        cerberus_wifi_capture_ring_stats_t stats;
        cerberus_wifi_capture_ring_stats(capture_ring,&stats);
        if(stats.pending)return false;
    }
    return true;
}

esp_err_t cerberus_wifi_capture_service_plan_stage(
    const cerberus_wifi_capture_plan_request_t *request,bool radio_held,
    cerberus_wifi_capture_plan_status_payload_t *out)
{
    if(!out)return ESP_ERR_INVALID_ARG;
    esp_err_t error=ESP_OK;
    static const cerberus_wifi_capture_plan_request_t query;
    if(!request)error=ESP_ERR_INVALID_ARG;
    else if(!memcmp(request,&query,sizeof(query))){} /* Read-only staging query. */
    else if(!request->generation||request->reserved||
        !cerberus_wifi_capture_plan_dwell_valid(request->dwell_ms)||
        request->target_count>CERBERUS_WIFI_CAPTURE_PLAN_MAX_TARGETS||
        !cerberus_wifi_capture_plan_mask_valid(request->channel_mask))error=ESP_ERR_INVALID_ARG;
    else if(request->generation==staged_generation){
        if(request->channel_mask!=staged_plan.channel_mask||
           request->dwell_ms!=staged_plan.dwell_ms||
           request->target_count!=staged_plan.target_count)error=ESP_ERR_INVALID_STATE;
    }else if(request->generation<=staged_generation||request->generation<=last_generation||
        !plan_mutable(radio_held))error=ESP_ERR_INVALID_STATE;
    else{
        cerberus_wifi_capture_plan_clear(&staged_plan);
        staged_plan.channel_mask=request->channel_mask;
        staged_plan.dwell_ms=request->dwell_ms;
        staged_plan.target_count=request->target_count;
        staged_generation=request->generation;staged_present=0U;
    }
    plan_status(out,error);return error;
}

esp_err_t cerberus_wifi_capture_service_plan_target(
    const cerberus_wifi_capture_target_request_t *request,bool radio_held,
    cerberus_wifi_capture_plan_status_payload_t *out)
{
    if(!out)return ESP_ERR_INVALID_ARG;
    esp_err_t error=ESP_OK;
    uint8_t any=0U;
    if(request)for(unsigned i=0;i<6U;++i)any|=request->bssid[i];
    if(!request||!request->generation||!any||(request->bssid[0]&1U)||
       request->index>=CERBERUS_WIFI_CAPTURE_PLAN_MAX_TARGETS)error=ESP_ERR_INVALID_ARG;
    else if(request->generation!=staged_generation||request->index>=staged_plan.target_count)
        error=ESP_ERR_INVALID_STATE;
    else if(staged_present&(UINT32_C(1)<<request->index)){
        if(memcmp(staged_plan.bssids[request->index],request->bssid,6U))error=ESP_ERR_INVALID_STATE;
    }else if(!plan_mutable(radio_held)||staged_generation<=last_generation)error=ESP_ERR_INVALID_STATE;
    else{
        for(unsigned i=0;i<staged_plan.target_count;++i)
            if((staged_present&(UINT32_C(1)<<i))&&!memcmp(staged_plan.bssids[i],request->bssid,6U))
                error=ESP_ERR_INVALID_ARG;
        if(error==ESP_OK){
            memcpy(staged_plan.bssids[request->index],request->bssid,6U);
            staged_present|=UINT32_C(1)<<request->index;
        }
    }
    plan_status(out,error);return error;
}

uint64_t cerberus_wifi_capture_service_plan_mask(void)
{return (current_request.flags&CERBERUS_WIFI_CAPTURE_FLAG_PLAN)&&
    current_request.generation==staged_generation?staged_plan.channel_mask:0U;}

uint16_t cerberus_wifi_capture_service_plan_dwell_ms(void)
{return (current_request.flags&CERBERUS_WIFI_CAPTURE_FLAG_PLAN)&&
    current_request.generation==staged_generation?staged_plan.dwell_ms:0U;}

static bool valid_channel(uint8_t channel)
{return (channel>=1U&&channel<=14U)||(channel>=32U&&channel<=177U);}

static bool valid_bssid(const uint8_t bssid[6])
{
    static const uint8_t zero[6]={0};
    return (bssid[0]&1U)==0U&&memcmp(bssid,zero,6U)!=0;
}

bool cerberus_wifi_capture_service_same_request(
    const cerberus_wifi_capture_start_request_t *request)
{
    return request&&request->generation==last_generation&&
        memcmp(request,&current_request,sizeof(*request))==0;
}

esp_err_t cerberus_wifi_capture_service_start(
    const cerberus_wifi_capture_start_request_t *request)
{
    bool hopping=request&&(request->flags&CERBERUS_WIFI_CAPTURE_FLAG_HOP)!=0U;
    bool planned=request&&(request->flags&CERBERUS_WIFI_CAPTURE_FLAG_PLAN)!=0U;
    bool plan_mode=request&&(cerberus_wifi_capture_plan_mode_valid(request->mode_mask,
        CERBERUS_WIFI_CAPTURE_PLAN_DWELL_MS)||cerberus_wifi_capture_plan_mode_valid(
        request->mode_mask,CERBERUS_WIFI_CAPTURE_PLAN_EAPOL_DWELL_MS));
    if(!request||!request->generation||
       (hopping?(request->channel!=0U||
           (planned?!plan_mode:request->mode_mask!=CERBERUS_WIFI_CAPTURE_MODE_BEACON)||
           request->flags!=(CERBERUS_WIFI_CAPTURE_FLAG_HOP|(planned?CERBERUS_WIFI_CAPTURE_FLAG_PLAN:0U))):
           (planned||!valid_channel(request->channel)))||
       !request->mode_mask||(request->mode_mask&~CERBERUS_WIFI_CAPTURE_MODE_ALL)||
       (request->flags&~(CERBERUS_WIFI_CAPTURE_FLAG_BSSID|CERBERUS_WIFI_CAPTURE_FLAG_HOP|CERBERUS_WIFI_CAPTURE_FLAG_PLAN))||
       ((request->flags&CERBERUS_WIFI_CAPTURE_FLAG_BSSID)&&
        !valid_bssid(request->bssid))||
       request->lease_ms<CERBERUS_WIFI_CAPTURE_LEASE_MIN_MS||
       request->lease_ms>CERBERUS_WIFI_CAPTURE_LEASE_MAX_MS)
        return ESP_ERR_INVALID_ARG;
    if(planned?(request->generation!=staged_generation||
        staged_present!=plan_target_mask(staged_plan.target_count)||
        !cerberus_wifi_capture_plan_valid(&staged_plan)):
        request->generation<=staged_generation)return ESP_ERR_INVALID_STATE;
    if(planned&&!cerberus_wifi_capture_plan_mode_valid(request->mode_mask,
        staged_plan.dwell_ms))return ESP_ERR_INVALID_ARG;
    if(atomic_load_explicit(&receiving,memory_order_acquire)||
       atomic_load_explicit(&callback_inflight,memory_order_seq_cst)||
       request->generation<=last_generation)return ESP_ERR_INVALID_STATE;
    if(!capture_ring){
        const size_t size=sizeof(*capture_ring);
        if(heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)<size+32768U)
            return ESP_ERR_NO_MEM;
        capture_ring=heap_caps_malloc(size,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
        if(!capture_ring)return ESP_ERR_NO_MEM;
        cerberus_wifi_capture_ring_init(capture_ring);
    }
    if(!cerberus_wifi_capture_ring_start(capture_ring,request->generation))
        return ESP_ERR_INVALID_STATE; /* previous generation must drain */
    current_request=*request;
    last_generation=request->generation;
    last_acked_sequence=0;
    atomic_store_explicit(&context_skipped,0U,memory_order_relaxed);
    atomic_store_explicit(&deadline_ms,0U,memory_order_relaxed);
    last_error=ESP_OK;
    state=CERBERUS_WIFI_CAPTURE_RUNNING;
    atomic_store_explicit(&receive_token,0U,memory_order_seq_cst);
    atomic_store_explicit(&receive_channel,0U,memory_order_release);
    atomic_store_explicit(&receiving,true,memory_order_release);
    return ESP_OK;
}

void cerberus_wifi_capture_service_stop(esp_err_t reason)
{
    atomic_store_explicit(&receive_token,0U,memory_order_seq_cst);
    atomic_store_explicit(&receiving,false,memory_order_release);
    atomic_store_explicit(&receive_channel,0U,memory_order_release);
    if(capture_ring)cerberus_wifi_capture_ring_stop(capture_ring);
    last_error=reason;
    state=reason==ESP_OK||reason==ESP_ERR_TIMEOUT?
        CERBERUS_WIFI_CAPTURE_STOPPED:CERBERUS_WIFI_CAPTURE_ERROR;
}

bool cerberus_wifi_capture_service_active(void)
{return atomic_load_explicit(&receiving,memory_order_acquire);}

void cerberus_wifi_capture_service_cleanup_error(esp_err_t reason)
{
    cerberus_wifi_capture_service_stop(reason);
    state=CERBERUS_WIFI_CAPTURE_ERROR;
}

void cerberus_wifi_capture_service_confirm_stopped(void)
{
    if(!atomic_load_explicit(&receiving,memory_order_acquire)&&last_generation)
        state=CERBERUS_WIFI_CAPTURE_STOPPED;
}

uint32_t cerberus_wifi_capture_service_generation(void)
{return last_generation;}

uint32_t cerberus_wifi_capture_service_deadline_ms(void)
{return atomic_load_explicit(&deadline_ms,memory_order_acquire);}

void cerberus_wifi_capture_service_set_deadline(uint32_t value)
{
    /* Zero means setup has not armed a lease. A real deadline landing at
     * the 32-bit clock rollover must expire one millisecond later, not turn
     * into an unlimited session. */
    atomic_store_explicit(&deadline_ms,value?value:1U,memory_order_release);
}

static void renew_lease(void)
{
    if(atomic_load_explicit(&receiving,memory_order_acquire))
        cerberus_wifi_capture_service_set_deadline(
            (uint32_t)(esp_timer_get_time()/1000LL)+current_request.lease_ms);
}

uint32_t cerberus_wifi_capture_service_inflight(void)
{return atomic_load_explicit(&callback_inflight,memory_order_seq_cst);}

uint32_t cerberus_wifi_capture_service_admission_token(void)
{return atomic_load_explicit(&receive_token,memory_order_seq_cst);}

esp_err_t cerberus_wifi_capture_service_set_channel(uint8_t channel)
{
    atomic_store_explicit(&receive_token,0U,memory_order_seq_cst);
    atomic_store_explicit(&receive_channel,0U,memory_order_release);
    if(!channel)return ESP_OK;
    if(!valid_channel(channel))return ESP_ERR_INVALID_ARG;
    if(!cerberus_wifi_capture_service_active()||cerberus_wifi_capture_service_inflight())
        return ESP_ERR_INVALID_STATE;
    /* Retunes keep the capture generation, but each armed interval needs a
     * distinct token even if a delayed callback samples its clock afterward. */
    if(last_admission_token==UINT32_MAX)return ESP_ERR_INVALID_STATE;
    int64_t armed_us=esp_timer_get_time();
    if(armed_us<0||(uint64_t)armed_us>UINT64_C(0xffffffffffff))return ESP_ERR_INVALID_STATE;
    receive_armed_us=armed_us;
    atomic_store_explicit(&receive_channel,channel,memory_order_release);
    atomic_store_explicit(&receive_token,++last_admission_token,memory_order_seq_cst);
    return ESP_OK;
}

void cerberus_wifi_capture_service_receive(const uint8_t *frame,size_t length,
    uint8_t channel,int8_t rssi_dbm,int64_t callback_us,uint32_t admission_token)
{
    if(!admission_token)return;
    /* Sequential admission/counter operations prevent Stop/retune and a late
     * callback from both missing each other's gate/count update. The radio
     * owner cannot replace request/arm fields while an admitted reader lives. */
    atomic_fetch_add_explicit(&callback_inflight,1U,memory_order_seq_cst);
    if(admission_token!=atomic_load_explicit(&receive_token,memory_order_seq_cst)||
       !atomic_load_explicit(&receiving,memory_order_acquire))goto done;
    if(callback_us<receive_armed_us||callback_us<0||
       (uint64_t)callback_us>UINT64_C(0xffffffffffff))goto done;
    if(!frame||!length||length>UINT16_MAX||!capture_ring)goto done;
    const cerberus_wifi_capture_start_request_t *request=&current_request;
    if(channel==0U||channel!=atomic_load_explicit(&receive_channel,memory_order_acquire)||
       !cerberus_wifi_capture_filter_match(request->mode_mask,
           (request->flags&CERBERUS_WIFI_CAPTURE_FLAG_BSSID)!=0U,
           request->bssid,frame,length))goto done;
    if(request->flags&CERBERUS_WIFI_CAPTURE_FLAG_PLAN){
        /* Infrastructure EAPOL identifies its BSSID by DS direction, not
         * addr3. Do not turn WDS or direct-link peers into AP observations. */
        if((request->mode_mask&CERBERUS_WIFI_CAPTURE_MODE_EAPOL)&&
           ((frame[0]>>2U)&3U)==2U&&((frame[1]&3U)==0U||(frame[1]&3U)==3U))goto done;
        if(!cerberus_wifi_capture_plan_matches(&staged_plan,
            cerberus_wifi_capture_filter_bssid(frame,length),channel))goto done;
    }
    if(request->mode_mask==(CERBERUS_WIFI_CAPTURE_MODE_EAPOL|CERBERUS_WIFI_CAPTURE_MODE_BEACON)&&
       frame[0]==0x80U){
        /* Context may occupy only an empty ring, leaving the second slot for
         * EAPOL in fixed and planned sessions. RX is the sole producer;
         * concurrent ACKs can only reduce occupancy. Never evict or mutate a
         * pinned record. Standalone beacon and RAW capture stay unchanged. */
        cerberus_wifi_capture_ring_stats_t stats;
        cerberus_wifi_capture_ring_stats(capture_ring,&stats);
        if(stats.pending){
            unsigned skipped=atomic_load_explicit(&context_skipped,memory_order_relaxed);
            if(skipped<UINT16_MAX)atomic_store_explicit(&context_skipped,skipped+1U,memory_order_relaxed);
            goto done;
        }
    }
    cerberus_wifi_pcap_metadata_t metadata={
        .channel=channel,.rssi_dbm=rssi_dbm,.rssi_known=true,
        .frame_has_fcs=false,.time_kind=CERBERUS_WIFI_PCAP_TIME_MONOTONIC,
        .capture_time_us=(uint64_t)callback_us,
    };
    (void)cerberus_wifi_capture_ring_push(capture_ring,request->generation,
        &metadata,frame,length,length);
done:
    atomic_fetch_sub_explicit(&callback_inflight,1U,memory_order_seq_cst);
}

void cerberus_wifi_capture_service_status(cerberus_wifi_capture_status_payload_t *out)
{
    if(!out)return;
    memset(out,0,sizeof(*out));
    out->generation=last_generation;
    out->state=state;
    out->channel=(current_request.flags&CERBERUS_WIFI_CAPTURE_FLAG_HOP)?
        atomic_load_explicit(&receive_channel,memory_order_acquire):current_request.channel;
    out->mode_mask=current_request.mode_mask;
    out->error_code=last_error;
    if(!capture_ring)return;
    cerberus_wifi_capture_ring_stats_t stats;
    cerberus_wifi_capture_ring_stats(capture_ring,&stats);
    if(stats.generation!=last_generation)return;
    out->pending=stats.pending;
    out->accepted=stats.accepted;
    out->dropped_full=stats.dropped_full;
    uint64_t other=(uint64_t)stats.dropped_stopped+stats.dropped_stale+
        stats.dropped_oversize+stats.dropped_truncated+stats.dropped_invalid+
        atomic_load_explicit(&context_skipped,memory_order_relaxed);
    out->dropped_other=other>UINT16_MAX?UINT16_MAX:(uint16_t)other;
}

esp_err_t cerberus_wifi_capture_service_peek(uint32_t generation,
    cerberus_wifi_capture_peek_payload_t *out)
{
    if(!out||!generation)return ESP_ERR_INVALID_ARG;
    memset(out,0,sizeof(*out));
    if(!capture_ring||generation!=last_generation)return ESP_ERR_INVALID_STATE;
    out->generation=generation;
    const cerberus_wifi_capture_record_t *record=cerberus_wifi_capture_ring_peek(capture_ring);
    if(!record){renew_lease();return ESP_OK;}
    if(record->generation!=generation||
       record->metadata.capture_time_us>UINT64_C(0xffffffffffff))return ESP_ERR_INVALID_STATE;
    out->sequence=record->sequence;
    out->length=record->captured_length;
    uint64_t time=record->metadata.capture_time_us;
    for(unsigned i=0;i<6U;++i)out->monotonic_us_le48[i]=(uint8_t)(time>>(8U*i));
    out->channel=record->metadata.channel;
    out->rssi_dbm=record->metadata.rssi_dbm;
    out->crc32=cerberus_protocol_crc32(record->frame,record->captured_length);
    renew_lease();
    return ESP_OK;
}

esp_err_t cerberus_wifi_capture_service_read(
    const cerberus_wifi_capture_read_request_t *request,
    cerberus_wifi_capture_read_payload_t *out)
{
    if(!request||!out||!capture_ring||!request->generation||!request->sequence||
       request->offset%CERBERUS_WIFI_CAPTURE_CHUNK_SIZE)return ESP_ERR_INVALID_ARG;
    const cerberus_wifi_capture_record_t *record=cerberus_wifi_capture_ring_peek(capture_ring);
    if(!record||record->generation!=request->generation||
       record->sequence!=request->sequence||
       request->offset>=record->captured_length)return ESP_ERR_INVALID_STATE;
    memset(out,0,sizeof(*out));
    out->offset=request->offset;
    size_t available=record->captured_length-request->offset;
    size_t copy=available<CERBERUS_WIFI_CAPTURE_CHUNK_SIZE?
        available:CERBERUS_WIFI_CAPTURE_CHUNK_SIZE;
    memcpy(out->data,record->frame+request->offset,copy);
    renew_lease();
    return ESP_OK;
}

esp_err_t cerberus_wifi_capture_service_ack(
    const cerberus_wifi_capture_ack_request_t *request)
{
    if(!request||!request->generation||!request->sequence||
       request->generation!=last_generation||!capture_ring)return ESP_ERR_INVALID_ARG;
    if(request->sequence==last_acked_sequence){renew_lease();return ESP_OK;} /* lost reply retry */
    if(!cerberus_wifi_capture_ring_ack(capture_ring,
        request->generation,request->sequence))return ESP_ERR_INVALID_STATE;
    last_acked_sequence=request->sequence;
    renew_lease();
    return ESP_OK;
}
