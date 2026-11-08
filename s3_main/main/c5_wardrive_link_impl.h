/* One wide transaction per observation, with the preceding ACK piggybacked.
 * A lost response retains the next head. The caller retries the same request. */
esp_err_t c5_spi_link_wardrive(uint8_t command,const cerberus_wd_request_t *query,void *reply)
{
    if(!query||!reply||!query->session||command<CERBERUS_WD_START||command>CERBERUS_WD_RESUME)
        return ESP_ERR_INVALID_ARG;
    memset(reply,0,54U);if(!link_mutex)return ESP_ERR_INVALID_STATE;
    int64_t began=esp_timer_get_time();if(began<0)return ESP_ERR_INVALID_STATE;
    uint32_t deadline=(uint32_t)(began/1000LL)+30U;
    if(take_link_mutex(pdMS_TO_TICKS(20))!=pdTRUE)return ESP_ERR_TIMEOUT;
    esp_err_t error=ESP_OK;
    if(command==CERBERUS_WD_START){
        cerberus_c5_status_t device={0};error=get_status_v2_passive_locked(++sequence,deadline,&device);
        peer_firmware_version=error==ESP_OK?device.firmware_version:0;
    }
    if(error==ESP_OK&&peer_firmware_version<CERBERUS_WD_MIN_FIRMWARE)error=ESP_ERR_NOT_SUPPORTED;
    if(error==ESP_OK&&command>=CERBERUS_WD_PAUSE&&peer_firmware_version<CERBERUS_WD_PAUSE_MIN_FIRMWARE)
        error=ESP_ERR_NOT_SUPPORTED;
    cerberus_wd_frame_t response={0};
    if(error==ESP_OK){
        cerberus_spi_frame_t request;uint8_t tx[64]={0},discarded[64]={0};uint8_t token=++sequence;
        if(!cerberus_protocol_prepare_frame(&request,(cerberus_spi_command_t)command,token,query,sizeof(*query)))error=ESP_ERR_INVALID_ARG;
        else{
            memcpy(tx,&request,sizeof(request));error=spi_request_response_passive(tx,discarded,&response,64U,deadline);
            if(error==ESP_OK&&(response.magic!=CERBERUS_SPI_MAGIC||response.version!=CERBERUS_PROTOCOL_VERSION||
                response.command!=command||response.sequence!=token||response.length!=54U||
                response.crc!=cerberus_protocol_crc16(&response,62U)))error=ESP_ERR_INVALID_RESPONSE;
            if(error==ESP_OK&&command!=CERBERUS_WD_READ){
                uint64_t returned;memcpy(&returned,response.payload,sizeof(returned));
                if(returned!=query->session)error=ESP_ERR_INVALID_RESPONSE;
            }
        }
    }
    give_link_mutex();if(error==ESP_OK)memcpy(reply,response.payload,54U);
    if(error==ESP_OK&&command==CERBERUS_WD_READ&&((cerberus_wd_record_t *)reply)->kind==255U)error=ESP_ERR_INVALID_STATE;
    return error;
}
