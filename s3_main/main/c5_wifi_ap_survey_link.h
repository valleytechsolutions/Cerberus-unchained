/* Private bounded passive transport. The immutable copy has its own identity;
 * live revisions intentionally are not required to stop during transfer. */
static bool ap_survey_peer_equal(const cerberus_c5_status_t *a,const cerberus_c5_status_t *b)
{
    return a->protocol_version==b->protocol_version&&a->firmware_version==b->firmware_version&&
        a->capabilities==b->capabilities&&a->reset_reason==b->reset_reason&&
        a->boot_count==b->boot_count&&b->uptime_ms>=a->uptime_ms;
}
static esp_err_t ap_survey_link_peer(cerberus_c5_status_t *out)
{
    memset(out,0,sizeof(*out));
    if(!link_mutex)return ESP_ERR_INVALID_STATE;
    int64_t now=esp_timer_get_time();if(now<0)return ESP_ERR_INVALID_STATE;
    uint32_t deadline=(uint32_t)((uint64_t)now/1000U)+20U;
    if(take_link_mutex(pdMS_TO_TICKS(20))!=pdTRUE)return ESP_ERR_TIMEOUT;
    esp_err_t error=get_status_v2_passive_locked(++sequence,deadline,out);
    give_link_mutex();
    int64_t ended=esp_timer_get_time();
    if(error==ESP_OK&&(ended<now||ended-now>=20000))error=ESP_ERR_TIMEOUT;
    if(error==ESP_OK&&(out->protocol_version!=CERBERUS_PROTOCOL_VERSION||
        out->firmware_version<CERBERUS_AP_SURVEY_MIN_FIRMWARE))error=ESP_ERR_NOT_SUPPORTED;
    peer_firmware_version=error==ESP_OK?out->firmware_version:0U;
    if(error!=ESP_OK)memset(out,0,sizeof(*out));
    return error;
}
esp_err_t c5_spi_link_wifi_ap_survey_exchange(uint8_t command,const void *request,
    size_t request_size,void *reply,size_t reply_size)
{
    size_t expected=cerberus_ap_survey_request_bytes(command);
    if(!reply||expected==SIZE_MAX||request_size!=expected||reply_size!=cerberus_ap_survey_reply_bytes(command)||
        (expected?!request:request!=NULL))return ESP_ERR_INVALID_ARG;
    uint8_t query[20]={0};if(expected)memcpy(query,request,expected);
    memset(reply,0,reply_size);
    uint64_t session=0U;if(expected)memcpy(&session,query,sizeof(session));
    if(expected&&!session)return ESP_ERR_INVALID_ARG;
    if(!link_mutex)return ESP_ERR_INVALID_STATE;
    int64_t now=esp_timer_get_time();if(now<0)return ESP_ERR_INVALID_STATE;
    uint32_t deadline=(uint32_t)((uint64_t)now/1000U)+20U;
    if(take_link_mutex(pdMS_TO_TICKS(20))!=pdTRUE)return ESP_ERR_TIMEOUT;
    esp_err_t error=ESP_OK;
    if(command==CERBERUS_AP_SURVEY_COMMAND_INFO||command==CERBERUS_AP_SURVEY_COMMAND_START) {
        cerberus_c5_status_t peer={0};error=get_status_v2_passive_locked(++sequence,deadline,&peer);
        peer_firmware_version=error==ESP_OK&&peer.protocol_version==CERBERUS_PROTOCOL_VERSION?peer.firmware_version:0U;
    }
    if(error==ESP_OK&&peer_firmware_version<CERBERUS_AP_SURVEY_MIN_FIRMWARE)error=ESP_ERR_NOT_SUPPORTED;
    if(error==ESP_OK&&command==CERBERUS_AP_SURVEY_COMMAND_SNAPSHOT_READ) {
        cerberus_spi_frame_t frame;
        uint8_t token=++sequence,tx[CERBERUS_AP_SURVEY_TRANSFER_BYTES]={0},discarded[sizeof(tx)]={0};
        cerberus_ap_survey_frame_t response={0};
        if(!cerberus_protocol_prepare_frame(&frame,(cerberus_spi_command_t)command,token,query,expected))error=ESP_ERR_INVALID_ARG;
        else {
            memcpy(tx,&frame,sizeof(frame));error=spi_request_response_passive(tx,discarded,&response,sizeof(tx),deadline);
            if(error==ESP_OK&&(response.magic!=CERBERUS_SPI_MAGIC||response.protocol_version!=CERBERUS_PROTOCOL_VERSION||
                response.command!=command||response.sequence!=token||response.payload_length!=sizeof(response.payload)||
                response.crc16!=cerberus_protocol_crc16(&response,offsetof(cerberus_ap_survey_frame_t,crc16))))error=ESP_ERR_INVALID_RESPONSE;
            if(error==ESP_OK)memcpy(reply,&response.payload,reply_size);
        }
    } else if(error==ESP_OK)error=survey_session_frame_locked((cerberus_spi_command_t)command,
        expected?query:NULL,expected,reply,reply_size,deadline);
    give_link_mutex();
    int64_t ended=esp_timer_get_time();
    if(error==ESP_OK&&(ended<now||ended-now>=20000))error=ESP_ERR_TIMEOUT;
    uint64_t returned=0U;memcpy(&returned,reply,sizeof(returned));
    if(error==ESP_OK&&expected&&returned!=session)error=ESP_ERR_INVALID_RESPONSE;
    if(error==ESP_OK&&command==CERBERUS_AP_SURVEY_COMMAND_SNAPSHOT_READ) {
        cerberus_ap_survey_read_t q;memcpy(&q,query,sizeof(q));
        cerberus_ap_survey_chunk_t *out=reply;
        if(out->offset!=q.offset||(out->projection&&out->projection!=q.projection))error=ESP_ERR_INVALID_RESPONSE;
        if(!out->projection) {
            int32_t remote;memcpy(&remote,out->data,4U);
            if(!remote)error=ESP_ERR_INVALID_RESPONSE;
            for(unsigned i=4U;i<sizeof(out->data);++i)if(out->data[i])error=ESP_ERR_INVALID_RESPONSE;
        }
    } else if(error==ESP_OK&&command==CERBERUS_AP_SURVEY_COMMAND_SNAPSHOT_BEGIN) {
        if(!cerberus_ap_survey_projection_valid(reply,session))error=ESP_ERR_INVALID_RESPONSE;
    } else if(error==ESP_OK) {
        if(!cerberus_ap_survey_control_valid(reply,command==CERBERUS_AP_SURVEY_COMMAND_INFO,session))
            error=ESP_ERR_INVALID_RESPONSE;
    }
    if(error!=ESP_OK)memset(reply,0,reply_size);
    return error;
}
enum { AP_SURVEY_READER_PEER=1,AP_SURVEY_READER_BEGIN,AP_SURVEY_READER_READ,
    AP_SURVEY_READER_FINAL,AP_SURVEY_READER_DONE,AP_SURVEY_READER_FAILED };
static esp_err_t ap_survey_reader_fail(cerberus_c5_ap_survey_reader_t *r,esp_err_t error)
{
    if(r->staging)memset(r->staging,0,sizeof(*r->staging));
    r->phase=AP_SURVEY_READER_FAILED;r->error=error;return error;
}
void c5_spi_link_wifi_ap_survey_reader_cancel(cerberus_c5_ap_survey_reader_t *r)
{ if(r)(void)ap_survey_reader_fail(r,ESP_ERR_NOT_FINISHED); }
esp_err_t c5_spi_link_wifi_ap_survey_reader_begin(cerberus_c5_ap_survey_reader_t *r,
    uint64_t session,cerberus_ap_survey_snapshot_t *staging,uint32_t deadline)
{
    if(!r||!staging||!session)return ESP_ERR_INVALID_ARG;
    memset(r,0,sizeof(*r));memset(staging,0,sizeof(*staging));
    r->staging=staging;r->session_id=session;r->deadline_ms=deadline;r->phase=AP_SURVEY_READER_PEER;
    return ESP_OK;
}
static esp_err_t ap_survey_reader_clock(cerberus_c5_ap_survey_reader_t *r,uint64_t *out)
{
    int64_t now=esp_timer_get_time();
    if(now<0||(uint64_t)now<r->last_us)return ESP_ERR_INVALID_STATE;
    uint32_t left=r->deadline_ms-(uint32_t)((uint64_t)now/1000U);
    if(!left||left>INT32_MAX)return ESP_ERR_TIMEOUT;
    r->last_us=(uint64_t)now;*out=(uint64_t)now;return ESP_OK;
}
esp_err_t c5_spi_link_wifi_ap_survey_reader_step(cerberus_c5_ap_survey_reader_t *r,bool *complete)
{
    if(complete)*complete=false;
    if(!r||!r->staging||!complete)return ESP_ERR_INVALID_ARG;
    if(r->phase==AP_SURVEY_READER_FAILED)return r->error;
    if(r->phase==AP_SURVEY_READER_DONE){*complete=true;return ESP_OK;}
    uint64_t now=0U;esp_err_t error=ap_survey_reader_clock(r,&now);
    if(error!=ESP_OK)return ap_survey_reader_fail(r,error);
    switch(r->phase) {
    case AP_SURVEY_READER_PEER:
        error=ap_survey_link_peer(&r->peer);
        if(error==ESP_OK)r->phase=AP_SURVEY_READER_BEGIN;
        break;
    case AP_SURVEY_READER_BEGIN: {
        cerberus_ap_survey_request_t q={r->session_id};cerberus_ap_survey_projection_t p;
        r->request_started_us=now;
        error=c5_spi_link_wifi_ap_survey_exchange(CERBERUS_AP_SURVEY_COMMAND_SNAPSHOT_BEGIN,&q,sizeof(q),&p,sizeof(p));
        if(error==ESP_OK&&p.error_code)error=p.error_code;
        if(error==ESP_OK&&(!p.projection||p.length!=sizeof(*r->staging)||p.reserved||
            (p.flags&~7U)||p.ap_count>CERBERUS_AP_SURVEY_AP_CAPACITY||
            p.relation_count>CERBERUS_AP_SURVEY_RELATION_CAPACITY))error=ESP_ERR_INVALID_RESPONSE;
        if(error==ESP_OK){r->projection=p.projection;r->ap_count=p.ap_count;
            r->relation_count=p.relation_count;r->snapshot_flags=p.flags;r->phase=AP_SURVEY_READER_READ;}
        break;
    }
    case AP_SURVEY_READER_READ: {
        cerberus_ap_survey_read_t q={r->session_id,r->projection,r->offset};cerberus_ap_survey_chunk_t p;
        error=c5_spi_link_wifi_ap_survey_exchange(CERBERUS_AP_SURVEY_COMMAND_SNAPSHOT_READ,&q,sizeof(q),&p,sizeof(p));
        if(error==ESP_OK&&!p.projection)memcpy(&error,p.data,sizeof(error));
        size_t count=sizeof(*r->staging)-r->offset;if(count>sizeof(p.data))count=sizeof(p.data);
        if(error==ESP_OK)for(size_t i=count;i<sizeof(p.data);++i)if(p.data[i])error=ESP_ERR_INVALID_RESPONSE;
        if(error==ESP_OK) {
            memcpy((uint8_t *)r->staging+r->offset,p.data,count);r->offset+=(uint16_t)count;
            if(r->offset==sizeof(*r->staging))r->phase=AP_SURVEY_READER_FINAL;
        }
        break;
    }
    case AP_SURVEY_READER_FINAL: {
        cerberus_c5_status_t peer;error=ap_survey_link_peer(&peer);
        if(error==ESP_OK&&(!ap_survey_peer_equal(&r->peer,&peer)||
            r->staging->session_id!=r->session_id||r->staging->ap_count!=r->ap_count||
            r->staging->relation_count!=r->relation_count||r->staging->flags!=r->snapshot_flags||
            !cerberus_ap_survey_snapshot_valid(r->staging)))error=ESP_ERR_INVALID_RESPONSE;
        if(error==ESP_OK){r->phase=AP_SURVEY_READER_DONE;*complete=true;}
        break;
    }
    default:error=ESP_ERR_INVALID_STATE;break;
    }
    if(error==ESP_OK)error=ap_survey_reader_clock(r,&r->receipt_us);
    if(error!=ESP_OK){*complete=false;return ap_survey_reader_fail(r,error);}
    return ESP_OK;
}
