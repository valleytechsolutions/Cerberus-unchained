/* Private to c5_spi_link.c. New peers use one bounded passive exchange per
 * step. A copied image includes the legacy row and motion so neither can be
 * spliced onto a different full-epoch profile by an eight-bit generation wrap. */
enum { WATCH_PROFILE_PEER=1, WATCH_PROFILE_BEGIN, WATCH_PROFILE_READ,
    WATCH_PROFILE_FINAL_PEER, WATCH_PROFILE_LEGACY, WATCH_PROFILE_DONE, WATCH_PROFILE_FAILED };

static esp_err_t watch_profile_fail(cerberus_c5_watch_profile_reader_t *r,esp_err_t error)
{
    memset(&r->image,0,sizeof(r->image));memset(&r->pending,0,sizeof(r->pending));
    r->phase=WATCH_PROFILE_FAILED;r->error=error;return error;
}

void c5_spi_link_watch_profile_cancel(cerberus_c5_watch_profile_reader_t *r)
{ if(r)(void)watch_profile_fail(r,ESP_ERR_NOT_FINISHED); }

esp_err_t c5_spi_link_watch_profile_begin(cerberus_c5_watch_profile_reader_t *r,
    const cerberus_c5_status_t *expected,uint8_t radio,uint8_t generation,
    uint8_t index,uint64_t epoch,uint32_t deadline_ms)
{
    if(!r)return ESP_ERR_INVALID_ARG;
    cerberus_c5_status_t copy={0};if(expected)copy=*expected;
    memset(r,0,sizeof(*r));
    if(radio!=CERBERUS_WATCH_RADIO_WIFI||index>=CERBERUS_WATCH_MAX_OBSERVATIONS)
        return watch_profile_fail(r,ESP_ERR_INVALID_ARG);
    r->expected_peer=copy;r->expect_peer=expected!=NULL;r->radio=radio;
    r->generation=generation;r->index=index;r->expected_epoch=epoch;
    r->deadline_ms=deadline_ms;r->phase=WATCH_PROFILE_PEER;
    return ESP_OK;
}

static esp_err_t watch_profile_clock(cerberus_c5_watch_profile_reader_t *r,uint64_t *now)
{
    int64_t sampled=esp_timer_get_time();
    if(sampled<0||(r->anchored&&(uint64_t)sampled<r->last_us))return ESP_ERR_INVALID_STATE;
    uint32_t left=r->deadline_ms-(uint32_t)((uint64_t)sampled/1000U);
    if(!left||left>INT32_MAX)return ESP_ERR_TIMEOUT;
    r->last_us=(uint64_t)sampled;r->anchored=true;if(now)*now=(uint64_t)sampled;
    return ESP_OK;
}

static esp_err_t watch_profile_exchange(cerberus_c5_watch_profile_reader_t *r,bool peer,
    cerberus_spi_command_t command,const void *request,size_t request_size,
    void *out,size_t out_size)
{
    memset(out,0,out_size);
    uint64_t started;
    esp_err_t error=watch_profile_clock(r,&started);if(error!=ESP_OK)return error;
    if(!link_mutex)return ESP_ERR_INVALID_STATE;
    uint32_t left=r->deadline_ms-(uint32_t)(started/1000U);
    if(left>20U)left=20U;
    uint32_t step_deadline=(uint32_t)(started/1000U)+left;
    if(take_link_mutex(pdMS_TO_TICKS(left))!=pdTRUE)return ESP_ERR_TIMEOUT;
    error=watch_profile_clock(r,NULL);
    if(error==ESP_OK){
        if(peer)error=get_status_v2_passive_locked(++sequence,step_deadline,out);
        else {
            cerberus_spi_frame_t q,response={0};
            uint8_t discarded[CERBERUS_SPI_TRANSFER_SIZE]={0},token=++sequence;
            if(!cerberus_protocol_prepare_frame(&q,command,token,request,request_size))error=ESP_ERR_INVALID_ARG;
            else error=spi_request_response_passive(&q,discarded,&response,sizeof(q),step_deadline);
            if(error==ESP_OK&&(!cerberus_protocol_frame_is_valid(&response)||
               response.command!=command||response.sequence!=token))error=ESP_ERR_INVALID_RESPONSE;
            if(error==ESP_OK&&command==CERBERUS_SPI_COMMAND_WATCH_PROFILE_READ&&
               response.payload_length==sizeof(cerberus_watch_profile_read_error_t)){
                cerberus_watch_profile_read_error_t failure;
                memcpy(&failure,response.payload,sizeof(failure));
                error=failure.capture_epoch==r->capture_epoch&&failure.projection==r->projection&&failure.error_code?
                    failure.error_code:ESP_ERR_INVALID_RESPONSE;
                /* Only the fresh version check can authorize old-peer fallback. */
                if(error==ESP_ERR_NOT_SUPPORTED)error=ESP_ERR_INVALID_RESPONSE;
            }else if(error==ESP_OK){
                if(response.payload_length!=out_size)error=ESP_ERR_INVALID_RESPONSE;
                else memcpy(out,response.payload,out_size);
            }
        }
    }
    give_link_mutex();
    if(error==ESP_ERR_NOT_SUPPORTED)error=ESP_ERR_INVALID_RESPONSE;
    uint64_t ended=0U;
    if(error==ESP_OK)error=watch_profile_clock(r,&ended);
    if(error==ESP_OK&&ended-started>=20000U)error=ESP_ERR_TIMEOUT;
    if(error!=ESP_OK)memset(out,0,out_size);
    return error;
}

static bool watch_profile_decode(cerberus_c5_watch_profile_reader_t *r)
{
    const cerberus_watch_profile_projection_t *p=&r->image;
    sentinel_watch_motion_t motion;memcpy(&motion,&p->motion,sizeof(motion));
    if(!watch_record_valid(r->radio,&p->observation)||!sentinel_watch_motion_valid(&motion)||
       !sentinel_watch_profile_valid(&p->profile)||(p->timing_flags&~3U)||
       (p->group_valid_mask&~p->profile.present)||
       (p->observation.kind!=SENTINEL_WATCH_PROTOCOL_DRONE&&(motion.flags||p->profile.present))||
       ((p->timing_flags&CERBERUS_WATCH_PROFILE_MOTION_TIMED)&&
        !(motion.flags&SENTINEL_WATCH_MOTION_LOCATION_PRESENT))||
       (!(p->timing_flags&CERBERUS_WATCH_PROFILE_SOURCE_TIMED)&&p->source_age_ms)||
       (!(p->timing_flags&CERBERUS_WATCH_PROFILE_MOTION_TIMED)&&p->motion_age_ms)||
       p->source_age_ms>CERBERUS_WATCH_MAX_SOURCE_AGE_MS||p->motion_age_ms>CERBERUS_WATCH_MAX_SOURCE_AGE_MS)
        return false;
    for(unsigned i=0;i<SENTINEL_WATCH_PROFILE_GROUPS;++i)
        if(((p->group_valid_mask&(1U<<i))!=0U)!=(p->group_age_ms[i]!=UINT32_MAX))return false;
    r->pending.observation=p->observation;r->pending.motion=motion;
    r->pending.profile.values=p->profile;r->pending.profile.capture_epoch=r->capture_epoch;
    r->pending.profile.available=true;r->pending.peer=r->peer;
    if(p->timing_flags&CERBERUS_WATCH_PROFILE_SOURCE_TIMED)
        r->pending.timing=(cerberus_watch_receive_timing_t){r->started_us,p->source_age_ms,true};
    if(p->timing_flags&CERBERUS_WATCH_PROFILE_MOTION_TIMED)
        r->pending.motion_timing=(cerberus_watch_receive_timing_t){r->started_us,p->motion_age_ms,true};
    for(unsigned i=0;i<SENTINEL_WATCH_PROFILE_GROUPS;++i)
        if(p->group_valid_mask&(1U<<i))r->pending.profile.timing[i]=
            (cerberus_watch_receive_timing_t){r->started_us,p->group_age_ms[i],true};
    return true;
}

esp_err_t c5_spi_link_watch_profile_step(cerberus_c5_watch_profile_reader_t *r,
    cerberus_c5_watch_profile_result_t *out,bool *complete)
{
    if(out)memset(out,0,sizeof(*out));
    if(complete)*complete=false;
    if(!r||!out||!complete)return ESP_ERR_INVALID_ARG;
    if(r->phase==WATCH_PROFILE_FAILED)return r->error;
    if(r->phase==WATCH_PROFILE_DONE){*out=r->pending;*complete=true;return ESP_OK;}
    esp_err_t error=ESP_ERR_INVALID_STATE;
    switch(r->phase){
    case WATCH_PROFILE_PEER:
        error=watch_profile_exchange(r,true,CERBERUS_SPI_COMMAND_STATUS,NULL,0,&r->peer,sizeof(r->peer));
        if(error==ESP_OK&&(r->peer.protocol_version!=CERBERUS_PROTOCOL_VERSION||
           !(r->peer.capabilities&CERBERUS_C5_CAP_WATCH_OBSERVATIONS)||
           (r->expect_peer&&!watch_peer_equal(&r->expected_peer,&r->peer))))error=ESP_ERR_INVALID_RESPONSE;
        if(error==ESP_OK){
            r->phase=r->peer.firmware_version<CERBERUS_REMOTE_ID_PROFILE_MIN_FIRMWARE?
                WATCH_PROFILE_LEGACY:WATCH_PROFILE_BEGIN;
            if(r->phase==WATCH_PROFILE_LEGACY&&r->expected_epoch)error=ESP_ERR_INVALID_RESPONSE;
        }
        break;
    case WATCH_PROFILE_BEGIN:{
        cerberus_watch_profile_begin_request_t query={r->expected_epoch,r->radio,r->generation,r->index,0};
        cerberus_watch_profile_begin_payload_t reply;
        error=watch_profile_clock(r,&r->started_us);
        if(error==ESP_OK)error=watch_profile_exchange(r,false,CERBERUS_SPI_COMMAND_WATCH_PROFILE_BEGIN,
            &query,sizeof(query),&reply,sizeof(reply));
        if(error==ESP_OK&&(reply.radio!=r->radio||reply.generation!=r->generation||reply.index!=r->index||reply.reserved))
            error=ESP_ERR_INVALID_RESPONSE;
        if(error==ESP_OK&&reply.error_code){
            if(reply.capture_epoch!=r->expected_epoch||reply.projection||reply.object_length)
                error=ESP_ERR_INVALID_RESPONSE;
            else error=reply.error_code==ESP_ERR_NOT_SUPPORTED?ESP_ERR_INVALID_RESPONSE:reply.error_code;
        }else if(error==ESP_OK){
            if(!reply.capture_epoch||(r->expected_epoch&&reply.capture_epoch!=r->expected_epoch)||
               !reply.projection||reply.object_length!=sizeof(r->image))error=ESP_ERR_INVALID_RESPONSE;
            else {r->capture_epoch=reply.capture_epoch;r->projection=reply.projection;r->phase=WATCH_PROFILE_READ;}
        }
        break;
    }
    case WATCH_PROFILE_READ:{
        cerberus_watch_profile_read_request_t query={r->capture_epoch,r->projection,r->radio,r->generation,r->index,r->chunk};
        cerberus_watch_profile_read_payload_t reply;
        error=watch_profile_exchange(r,false,CERBERUS_SPI_COMMAND_WATCH_PROFILE_READ,&query,sizeof(query),&reply,sizeof(reply));
        size_t offset=(size_t)r->chunk*CERBERUS_WATCH_PROFILE_CHUNK_SIZE,count=sizeof(r->image)-offset;
        if(count>CERBERUS_WATCH_PROFILE_CHUNK_SIZE)count=CERBERUS_WATCH_PROFILE_CHUNK_SIZE;
        if(error==ESP_OK&&(reply.projection!=r->projection||reply.chunk!=r->chunk||reply.length!=count))
            error=ESP_ERR_INVALID_RESPONSE;
        if(error==ESP_OK){
            for(size_t i=count;i<sizeof(reply.data);++i)if(reply.data[i])error=ESP_ERR_INVALID_RESPONSE;
        }
        if(error==ESP_OK){
            memcpy((uint8_t *)&r->image+offset,reply.data,count);
            if(offset+count==sizeof(r->image))r->phase=WATCH_PROFILE_FINAL_PEER;
            else ++r->chunk;
        }
        break;
    }
    case WATCH_PROFILE_LEGACY:
        error=watch_profile_clock(r,NULL);
        if(error==ESP_OK)error=c5_spi_link_get_watch_detailed_result(r->radio,r->generation,r->index,
            &r->pending.observation,&r->pending.timing,&r->pending.motion,&r->pending.motion_timing);
        if(error==ESP_OK)error=watch_profile_clock(r,NULL);
        if(error==ESP_OK)r->phase=WATCH_PROFILE_FINAL_PEER;
        break;
    case WATCH_PROFILE_FINAL_PEER:{
        cerberus_c5_status_t peer;
        error=watch_profile_exchange(r,true,CERBERUS_SPI_COMMAND_STATUS,NULL,0,&peer,sizeof(peer));
        if(error==ESP_OK&&!watch_peer_equal(&r->peer,&peer))error=ESP_ERR_INVALID_RESPONSE;
        if(error==ESP_OK&&r->peer.firmware_version>=CERBERUS_REMOTE_ID_PROFILE_MIN_FIRMWARE&&!watch_profile_decode(r))
            error=ESP_ERR_INVALID_RESPONSE;
        if(error==ESP_OK){r->pending.peer=peer;r->phase=WATCH_PROFILE_DONE;*out=r->pending;*complete=true;}
        break;
    }
    default:break;
    }
    return error==ESP_OK?ESP_OK:watch_profile_fail(r,error);
}
