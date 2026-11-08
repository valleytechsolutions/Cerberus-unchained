/* Private to c5_spi_link.c. Staging never starts RF, but its generation and
 * target readiness must survive exact retries and reject conflicting writes. */
#include "wifi_capture_plan.h"

static bool capture_plan_status_valid(const cerberus_wifi_capture_plan_status_payload_t *r)
{
    if(r->state>CERBERUS_WIFI_CAPTURE_PLAN_READY||r->target_count>CERBERUS_WIFI_CAPTURE_PLAN_MAX_TARGETS)
        return false;
    if(!r->generation)return !r->state&&!r->channel_mask&&!r->target_count&&!r->target_present_mask;
    if(!r->state||!cerberus_wifi_capture_plan_mask_valid(r->channel_mask))return false;
    uint32_t mask=r->target_count==32U?UINT32_MAX:(UINT32_C(1)<<r->target_count)-1U;
    return !(r->target_present_mask&~mask)&&
        (r->state==CERBERUS_WIFI_CAPTURE_PLAN_READY)==(r->target_present_mask==mask);
}

static esp_err_t capture_plan_exchange(cerberus_spi_command_t command,
    const void *payload,size_t size,void *out,uint32_t hop_generation,uint8_t minimum_firmware)
{
    memset(out,0,sizeof(cerberus_wifi_capture_plan_status_payload_t));
    if(!link_mutex)return ESP_ERR_INVALID_STATE;
    int64_t begun=esp_timer_get_time();
    if(begun<0)return ESP_ERR_INVALID_STATE;
    uint32_t deadline=(uint32_t)((uint64_t)begun/1000U)+20U;
    if(take_link_mutex(pdMS_TO_TICKS(20))!=pdTRUE)return ESP_ERR_TIMEOUT;
    esp_err_t error=packet_monitor_session_gate_locked(deadline,true,minimum_firmware);
    if(error==ESP_OK){
        cerberus_spi_frame_t request,response={0};
        uint8_t discarded[CERBERUS_SPI_TRANSFER_SIZE]={0};
        uint8_t token=++sequence;
        if(!cerberus_protocol_prepare_frame(&request,command,token,payload,size))error=ESP_ERR_INVALID_ARG;
        else error=spi_request_response_passive(&request,discarded,&response,sizeof(request),deadline);
        if(error==ESP_OK&&(!cerberus_protocol_frame_is_valid(&response)||
           response.command!=command||response.sequence!=token))error=ESP_ERR_INVALID_RESPONSE;
        if(error==ESP_OK&&hop_generation&&response.payload_length==sizeof(cerberus_wifi_capture_hop_error_payload_t)){
            cerberus_wifi_capture_hop_error_payload_t failure;
            memcpy(&failure,response.payload,sizeof(failure));
            error=failure.generation==hop_generation&&failure.error_code!=ESP_OK?
                failure.error_code:ESP_ERR_INVALID_RESPONSE;
        }else if(error==ESP_OK){
            if(response.payload_length!=sizeof(cerberus_wifi_capture_plan_status_payload_t))error=ESP_ERR_INVALID_RESPONSE;
            else memcpy(out,response.payload,sizeof(cerberus_wifi_capture_plan_status_payload_t));
        }
    }
    give_link_mutex();
    int64_t ended=esp_timer_get_time();
    if(error==ESP_OK&&(ended<begun||(uint64_t)(ended-begun)>=20000U))error=ESP_ERR_TIMEOUT;
    if(error!=ESP_OK)memset(out,0,sizeof(cerberus_wifi_capture_plan_status_payload_t));
    return error;
}

esp_err_t c5_spi_link_wifi_capture_plan_stage(const cerberus_wifi_capture_plan_request_t *request,
    cerberus_wifi_capture_plan_status_payload_t *out)
{
    if(!out)return ESP_ERR_INVALID_ARG;
    memset(out,0,sizeof(*out));
    static const cerberus_wifi_capture_plan_request_t query;
    if(!request)return ESP_ERR_INVALID_ARG;
    bool read_only=!memcmp(request,&query,sizeof(query));
    if(!read_only&&(!request->generation||request->reserved||
       !cerberus_wifi_capture_plan_dwell_valid(request->dwell_ms)||
       request->target_count>CERBERUS_WIFI_CAPTURE_PLAN_MAX_TARGETS||
       !cerberus_wifi_capture_plan_mask_valid(request->channel_mask)))return ESP_ERR_INVALID_ARG;
    esp_err_t error=capture_plan_exchange(CERBERUS_SPI_COMMAND_WIFI_CAPTURE_PLAN,
        request,sizeof(*request),out,0U,request->dwell_ms==CERBERUS_WIFI_CAPTURE_PLAN_EAPOL_DWELL_MS?
            CERBERUS_WIFI_EAPOL_PLAN_MIN_FIRMWARE:CERBERUS_WIFI_CAPTURE_PLAN_MIN_FIRMWARE);
    if(error==ESP_OK&&!capture_plan_status_valid(out))error=ESP_ERR_INVALID_RESPONSE;
    if(error==ESP_OK&&out->error_code)error=out->error_code;
    if(error==ESP_OK&&!read_only&&(out->generation!=request->generation||
       out->channel_mask!=request->channel_mask||out->target_count!=request->target_count))error=ESP_ERR_INVALID_RESPONSE;
    if(error!=ESP_OK)memset(out,0,sizeof(*out));
    return error;
}

esp_err_t c5_spi_link_wifi_capture_plan_status(cerberus_wifi_capture_plan_status_payload_t *out)
{
    const cerberus_wifi_capture_plan_request_t query={0};
    return c5_spi_link_wifi_capture_plan_stage(&query,out);
}

esp_err_t c5_spi_link_wifi_capture_plan_target(const cerberus_wifi_capture_target_request_t *request,
    cerberus_wifi_capture_plan_status_payload_t *out)
{
    if(!out)return ESP_ERR_INVALID_ARG;
    memset(out,0,sizeof(*out));
    uint8_t any=0U;if(request)for(unsigned i=0;i<6U;++i)any|=request->bssid[i];
    if(!request||!request->generation||request->index>=CERBERUS_WIFI_CAPTURE_PLAN_MAX_TARGETS||
       !any||(request->bssid[0]&1U))return ESP_ERR_INVALID_ARG;
    esp_err_t error=capture_plan_exchange(CERBERUS_SPI_COMMAND_WIFI_CAPTURE_TARGET,
        request,sizeof(*request),out,0U,CERBERUS_WIFI_CAPTURE_PLAN_MIN_FIRMWARE);
    if(error==ESP_OK&&!capture_plan_status_valid(out))error=ESP_ERR_INVALID_RESPONSE;
    if(error==ESP_OK&&out->error_code)error=out->error_code;
    if(error==ESP_OK&&(out->generation!=request->generation||request->index>=out->target_count||
       !(out->target_present_mask&(UINT32_C(1)<<request->index))))error=ESP_ERR_INVALID_RESPONSE;
    if(error!=ESP_OK)memset(out,0,sizeof(*out));
    return error;
}

esp_err_t c5_spi_link_wifi_capture_hop_status(uint32_t generation,
    cerberus_wifi_capture_hop_status_payload_t *out)
{
    if(!out)return ESP_ERR_INVALID_ARG;
    memset(out,0,sizeof(*out));
    if(!generation)return ESP_ERR_INVALID_ARG;
    cerberus_wifi_capture_stop_request_t request={generation};
    esp_err_t error=capture_plan_exchange(CERBERUS_SPI_COMMAND_WIFI_CAPTURE_HOP_STATUS,
        &request,sizeof(request),out,generation,CERBERUS_WIFI_CAPTURE_PLAN_MIN_FIRMWARE);
    const uint64_t all=cerberus_wifi_capture_plan_band_mask(CERBERUS_WIFI_CAPTURE_PLAN_BOTH);
    if(error==ESP_OK&&(out->generation!=generation||(out->attempted_mask&~all)||
       (out->applied_mask&~out->attempted_mask)))error=ESP_ERR_INVALID_RESPONSE;
    if(error!=ESP_OK)memset(out,0,sizeof(*out));
    return error;
}
