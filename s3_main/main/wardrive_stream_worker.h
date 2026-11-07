/* Included in the integrated service. Native receive tasks own RF; this worker
 * is the sole SD writer. Legacy expansion records use the same files/session. */
#include "wardrive_radio.h"
#include "esp_random.h"
static esp_err_t stream_exchange(bool c5,uint8_t command,cerberus_wd_request_t *q,void *out)
{
    return c5?c5_spi_link_wardrive(command,q,out):cerberus_wardrive_radio_exchange(command,q,out);
}
static esp_err_t stream_append(const cerberus_wd_record_t *row,bool c5)
{
    if(row->kind!=1U&&row->kind!=2U)return ESP_ERR_INVALID_RESPONSE;
    if(row->label_length>32U)return ESP_ERR_INVALID_RESPONSE;
    cerberus_wardrive_observation_t observation={.type=(cerberus_wardrive_record_type_t)row->kind,
        .radio=c5?CERBERUS_WARDRIVE_RADIO_C5:CERBERUS_WARDRIVE_RADIO_S3,
        .address_type=row->address_type,.channel=row->channel,.rssi_dbm=row->rssi,
        .security=row->security,.manufacturer_id=row->manufacturer,.label_length=row->label_length};
    memcpy(observation.address,row->address,6);memcpy(observation.label,row->label,row->label_length);
    uint64_t received=now_ms64();stream_observed_ms=received>=row->age_ms?received-row->age_ms:0;
    wardrive_recent_attempt_t attempt={0};recent_preview(&observation,&attempt);
    cerberus_wardrive_recent_outcome_t outcome;
    esp_err_t error=append_observation_impl(&observation,&outcome);recent_outcome(attempt.ticket,outcome);
    return error;
}
static void stream_publish(bool c5,const cerberus_wd_status_t *s)
{
    portENTER_CRITICAL(&wardrive_lock);
    if(c5){wardrive_status.c5_stream=*s;wardrive_status.c5_wifi_sweeps=s->wifi_sweeps;
        wardrive_status.c5_wifi_scan_dropped=s->dropped;}
    else {wardrive_status.s3_stream=*s;wardrive_status.s3_ble_scan_dropped=s->dropped;
        wardrive_status.s3_ble_sweeps=s->ble_ms/CERBERUS_WD_BLE_WINDOW_MS;}
    wardrive_status.elapsed_ms=(uint32_t)(now_ms64()-started_ms);
    portEXIT_CRITICAL(&wardrive_lock);
}
static void wardrive_stream_worker(void *unused)
{
    (void)unused;recent_begin_session();stream_recording=true;
    portENTER_CRITICAL(&wardrive_lock);
    if(wardrive_recent)wardrive_recent->all_observations=true;
    portEXIT_CRITICAL(&wardrive_lock);
    bool gps_enabled=wardrive_status.gps_requested;
    esp_err_t gps_error=start_optional_gps(gps_enabled);
    uint64_t session=((uint64_t)esp_random()<<32U)|esp_random();if(!session)session=1;
    cerberus_wd_request_t queries[2]={{.session=session,.mode=wardrive_status.mode},
        {.session=session,.mode=wardrive_status.mode==CERBERUS_WD_BLE?CERBERUS_WD_BLE:CERBERUS_WD_WIFI}};
    cerberus_wd_status_t radios[2]={{0}};
    bool begun[2]={false,false},stopped[2]={false,false};
    uint32_t last_ok[2]={now_ms(),now_ms()},last_sync=now_ms(),last_expansion=now_ms();
    esp_err_t failure=gps_error==ESP_OK?cerberus_wardrive_start_check(gps_enabled):gps_error;
    bool storage_failed=false;
    portENTER_CRITICAL(&wardrive_lock);wardrive_status.gps_error=gps_error;portEXIT_CRITICAL(&wardrive_lock);
    if(failure!=ESP_OK)publish_error(failure,'G');
    while(!wants_stop()&&failure==ESP_OK){
        for(unsigned chip=0;chip<2&&failure==ESP_OK&&!wants_stop();++chip){
            bool c5=chip==1;cerberus_wd_request_t *q=&queries[chip];
            if(!begun[chip]){
                failure=cerberus_wardrive_start_check(gps_enabled);
                if(failure!=ESP_OK){publish_error(failure,'G');break;}
            }
            esp_err_t e=stream_exchange(c5,begun[chip]?CERBERUS_WD_STATUS:CERBERUS_WD_START,q,&radios[chip]);
            if(e==ESP_OK){
                begun[chip]=true;last_ok[chip]=now_ms();stream_publish(c5,&radios[chip]);
                e=(esp_err_t)radios[chip].error;
                if(e==ESP_OK&&radios[chip].state==CERBERUS_WD_STOPPED)e=ESP_FAIL;
            }
            if(e==ESP_ERR_TIMEOUT||e==ESP_ERR_INVALID_RESPONSE||e==ESP_ERR_NOT_FINISHED){
                if((uint32_t)(now_ms()-last_ok[chip])<5000U)continue;
            }
            if(e!=ESP_OK){failure=e;publish_error(e,c5?'C':'B');break;}
            /* Bounded fairness: service both queues even in a dense band. RF
             * workers continue through all CSV writes and SD sync operations. */
            for(unsigned batch=0;batch<16U&&!wants_stop();++batch){
                cerberus_wd_record_t row={0};e=stream_exchange(c5,CERBERUS_WD_READ,q,&row);
                if(e!=ESP_OK)break;
                if(!row.sequence)break;
                if(row.sequence<=q->ack){e=ESP_ERR_INVALID_RESPONSE;break;}
                e=stream_append(&row,c5);
                if(e!=ESP_OK){storage_failed=true;failure=e;publish_error(e,'S');break;}
                q->ack=row.sequence;
            }
            if(e!=ESP_OK&&e!=ESP_ERR_TIMEOUT&&e!=ESP_ERR_INVALID_RESPONSE&&failure==ESP_OK){
                failure=e;publish_error(e,c5?'C':'B');
            }
        }
        uint32_t now=now_ms();
        if(failure==ESP_OK&&wardrive_status.expansions_requested&&(uint32_t)(now-last_expansion)>=WARDRIVE_EXPANSION_POLL_MS){
            failure=capture_expansions(false);last_expansion=now;
            if(failure!=ESP_OK&&!(failure==ESP_ERR_NOT_FINISHED&&wants_stop()))publish_error(failure,'E');
        }
        if(failure==ESP_OK&&(uint32_t)(now-last_sync)>=WARDRIVE_SYNC_MS){
            failure=wardrive_file_open?cerberus_storage_wardrive_sync():ESP_OK;
            if(failure==ESP_OK&&wigle_file_open)failure=cerberus_storage_wardrive_wigle_sync();
            last_sync=now;if(failure!=ESP_OK){storage_failed=true;publish_error(failure,'S');}
        }
        portENTER_CRITICAL(&wardrive_lock);
        if(radios[0].state==CERBERUS_WD_RUNNING&&radios[1].state==CERBERUS_WD_RUNNING)wardrive_status.state=CERBERUS_WARDRIVE_RUNNING;
        portEXIT_CRITICAL(&wardrive_lock);
        vTaskDelay(pdMS_TO_TICKS(5U) ? pdMS_TO_TICKS(5U) : 1U);
    }
    portENTER_CRITICAL(&wardrive_lock);wardrive_status.state=CERBERUS_WARDRIVE_STOPPING;portEXIT_CRITICAL(&wardrive_lock);
    /* START responses can be lost. Always reconcile/stop the exact candidate
     * session, including candidates whose START was never acknowledged. */
    while(!stopped[0]||!stopped[1]){
        for(unsigned chip=0;chip<2;++chip){
            if(stopped[chip])continue;
            bool c5=chip==1;cerberus_wd_request_t *q=&queries[chip];
            esp_err_t e=stream_exchange(c5,CERBERUS_WD_STOP,q,&radios[chip]);
            if(e==ESP_ERR_INVALID_STATE||(e==ESP_OK&&radios[chip].state==CERBERUS_WD_OFF&&radios[chip].error==ESP_ERR_INVALID_STATE)){
                stopped[chip]=true;continue; /* Authoritative absence. */
            }
            if(e!=ESP_OK){publish_error(e,c5?'C':'B');continue;}
            stream_publish(c5,&radios[chip]);
            if(radios[chip].held||radios[chip].state!=CERBERUS_WD_STOPPED)continue;
            if(radios[chip].error!=ESP_OK)publish_error((esp_err_t)radios[chip].error,c5?'C':'B');
            if(!storage_failed){
                for(unsigned batch=0;batch<32U;++batch){
                    cerberus_wd_record_t row={0};e=stream_exchange(c5,CERBERUS_WD_READ,q,&row);
                    if(e!=ESP_OK||!row.sequence)break;
                    if(row.sequence<=q->ack){e=ESP_ERR_INVALID_RESPONSE;break;}
                    e=stream_append(&row,c5);if(e!=ESP_OK){storage_failed=true;publish_error(e,'S');break;}
                    q->ack=row.sequence;
                }
                if(e!=ESP_OK&&!storage_failed)continue;
            }
            q->discard=storage_failed?1:0;
            e=stream_exchange(c5,CERBERUS_WD_RELEASE,q,&radios[chip]);
            if(e==ESP_OK&&radios[chip].error==ESP_OK){stream_publish(c5,&radios[chip]);stopped[chip]=true;}
            /* RELEASE reports the retained session's historical RF error even
             * after success. A following exact STOP proves it is absent. */
        }
        if(!stopped[0]||!stopped[1])vTaskDelay(pdMS_TO_TICKS(20));
    }
    if(wardrive_status.expansions_requested)stop_expansions_impl(!storage_failed);
    if(wardrive_file_open)(void)save_open_file(open_file_rows!=0);
    if(wigle_file_open)(void)save_log_stream(true,wigle_file_rows!=0);
    finish_storage_session();
    do{gps_error=stop_optional_gps(gps_enabled);if(gps_error!=ESP_OK)vTaskDelay(pdMS_TO_TICKS(50));}while(gps_error!=ESP_OK);
    esp_err_t idle=field_survey_set_idle_probe_enabled(true);if(idle!=ESP_OK)publish_error(idle,'E');
    stream_recording=false;release_workspace();
    portENTER_CRITICAL(&wardrive_lock);wardrive_status.elapsed_ms=(uint32_t)(now_ms64()-started_ms);
    wardrive_status.state=wardrive_status.error_code==ESP_OK?CERBERUS_WARDRIVE_STOPPED:CERBERUS_WARDRIVE_ERROR;
    recent_worker_session=false;worker_active=false;portEXIT_CRITICAL(&wardrive_lock);vTaskDelete(NULL);
}
