/* Exact native passive-lease boundary for actual Recon UI tests. Native
 * scanner callbacks/mutex ownership are exercised by the scanner event suite. */
static struct {
    cerberus_s3_ble_passive_state_t state;
    cerberus_s3_ble_discovery_record_t rows[CERBERUS_BLE_SCAN_MAX_RESULTS];
    uint64_t next_epoch;
    uint32_t current_scan_epoch;
    unsigned current_host_epoch;
    unsigned starts,stops,releases,queries,copies,foreign_stops;
    unsigned replace_after_copy;
    esp_err_t start_error,query_error,stop_error,copy_error;
    bool error_with_lease,empty_rearm_error,foreign,held,queued;
} recon_native;

static void recon_native_reconcile(void)
{
    if(!recon_native.state.attached)return;
    if(recon_native.state.scan_epoch!=recon_native.current_scan_epoch||
        recon_native.state.host_epoch!=recon_native.current_host_epoch){
        recon_native.state.error=ESP_ERR_INVALID_STATE;
        recon_native.state.owned=recon_native.state.receiving=recon_native.state.stopping=false;
    }
}
esp_err_t cerberus_s3_ble_scanner_passive_get_state(uint64_t epoch,cerberus_s3_ble_passive_state_t *out)
{
    ++recon_native.queries;memset(out,0,sizeof(*out));
    if(recon_native.query_error)return recon_native.query_error;
    if(!recon_lifecycle_active){
        ++status_calls;
        if(!epoch)return ESP_ERR_NOT_FOUND;
        *out=(cerberus_s3_ble_passive_state_t){.epoch=epoch,.started_ms=0,
            .source_now_ms=(uint64_t)clock_us/1000U,.scan=ble_scan_status,
            .scan_epoch=17,.host_epoch=3,.attached=true};
        if(raw_final_bad)++out->scan.generation;
        return ESP_OK;
    }
    recon_native_reconcile();
    if(!epoch||!recon_native.state.attached||recon_native.state.epoch!=epoch)return ESP_ERR_NOT_FOUND;
    *out=recon_native.state;
    if(clock_us>=0)out->source_now_ms=(uint64_t)clock_us/1000U;
    return ESP_OK;
}
esp_err_t cerberus_s3_ble_scanner_passive_start(uint64_t previous,cerberus_s3_ble_passive_state_t *out)
{
    CHECK(recon_lifecycle_active);++recon_native.starts;memset(out,0,sizeof(*out));
    recon_native_reconcile();
    if(recon_native.foreign||(recon_native.state.attached&&
        (!previous||previous!=recon_native.state.epoch||recon_native.state.owned)))return ESP_ERR_INVALID_STATE;
    if(recon_native.start_error&&!recon_native.error_with_lease){
        if(previous&&!recon_native.empty_rearm_error)*out=recon_native.state;
        return recon_native.start_error;
    }
    uint8_t generation=recon_native.state.scan.generation+1U;
    recon_native.state=(cerberus_s3_ble_passive_state_t){.epoch=++recon_native.next_epoch,
        .started_ms=(uint64_t)clock_us/1000U,.source_now_ms=(uint64_t)clock_us/1000U,
        .scan={.generation=generation,.state=CERBERUS_BLE_SCAN_RUNNING},
        .scan_epoch=++recon_native.current_scan_epoch,.host_epoch=recon_native.current_host_epoch,
        .attached=true,.owned=true,.receiving=!recon_native.queued,.error=recon_native.start_error};
    *out=recon_native.state;return recon_native.start_error;
}
esp_err_t cerberus_s3_ble_scanner_passive_stop(uint64_t epoch,cerberus_s3_ble_passive_state_t *out)
{
    CHECK(recon_lifecycle_active);++recon_native.stops;memset(out,0,sizeof(*out));recon_native_reconcile();
    if(!epoch||!recon_native.state.attached||epoch!=recon_native.state.epoch)return ESP_ERR_NOT_FOUND;
    if(recon_native.state.owned){
        if(recon_native.foreign){++recon_native.foreign_stops;CHECK(false);}
        recon_native.state.stopping=true;recon_native.state.receiving=false;
        if(!recon_native.held&&!recon_native.stop_error){
            recon_native.state.owned=recon_native.state.stopping=false;
            recon_native.state.scan.state=CERBERUS_BLE_SCAN_COMPLETE;
        }
    }
    *out=recon_native.state;return recon_native.stop_error;
}
esp_err_t cerberus_s3_ble_scanner_passive_release(uint64_t epoch)
{
    CHECK(recon_lifecycle_active);++recon_native.releases;recon_native_reconcile();
    if(!epoch||!recon_native.state.attached||epoch!=recon_native.state.epoch)return ESP_ERR_NOT_FOUND;
    if(recon_native.state.owned)return ESP_ERR_INVALID_STATE;
    memset(&recon_native.state,0,sizeof(recon_native.state));return ESP_OK;
}
esp_err_t cerberus_s3_ble_scanner_passive_copy_row(uint64_t epoch,uint8_t index,
    cerberus_s3_ble_discovery_record_t *out,cerberus_s3_ble_passive_state_t *state)
{
    ++recon_native.copies;memset(out,0,sizeof(*out));memset(state,0,sizeof(*state));
    cerberus_s3_ble_passive_state_t current;
    esp_err_t err=cerberus_s3_ble_scanner_passive_get_state(epoch,&current);
    if(err!=ESP_OK)return err;
    if(current.error||current.owned||current.scan.state!=CERBERUS_BLE_SCAN_COMPLETE||
        index>=current.scan.stored_count)return ESP_ERR_INVALID_STATE;
    if(recon_native.copy_error)return recon_native.copy_error;
    if(recon_lifecycle_active)*out=recon_native.rows[index];
    else *out=(cerberus_s3_ble_discovery_record_t){.peer=evidence[index].peer,
        .advertisement=evidence[index].advertisement,.focus=evidence[index].focus,
        .observed_ms=evidence[index].observed_ms,.generation=evidence[index].generation,.index=index};
    *state=current;
    if(recon_native.replace_after_copy==recon_native.copies)++recon_native.current_scan_epoch;
    return ESP_OK;
}
static esp_err_t recon_native_complete(uint64_t epoch,cerberus_s3_ble_passive_state_t *out)
{
    esp_err_t err=cerberus_s3_ble_scanner_passive_get_state(epoch,out);
    if(err==ESP_OK&&(out->error||out->owned||out->scan.state!=CERBERUS_BLE_SCAN_COMPLETE))err=ESP_ERR_INVALID_STATE;
    return err;
}
esp_err_t cerberus_s3_ble_scanner_passive_get_watch_status(uint64_t epoch,cerberus_watch_scan_status_payload_t *out)
{
    memset(out,0,sizeof(*out));cerberus_s3_ble_passive_state_t state;
    esp_err_t err=recon_native_complete(epoch,&state);if(err!=ESP_OK)return err;
    return extra_status(CERBERUS_WATCH_RADIO_BLE,state.scan.generation,out);
}
esp_err_t cerberus_s3_ble_scanner_passive_get_watch_result(uint64_t epoch,uint8_t index,cerberus_watch_observation_t *out)
{
    memset(out,0,sizeof(*out));cerberus_s3_ble_passive_state_t state;
    esp_err_t err=recon_native_complete(epoch,&state);if(err!=ESP_OK)return err;
    if(!recon_lifecycle_active)return cerberus_s3_ble_scanner_get_watch_result(state.scan.generation,index,out);
    CHECK(extra_drone&&index==0U);*out=(cerberus_watch_observation_t){.kind=SENTINEL_WATCH_PROTOCOL_DRONE,
        .transport=SENTINEL_WATCH_TRANSPORT_BLE,.address_type=recon_native.rows[0].peer.address_type,.rssi=-60};
    memcpy(out->address,recon_native.rows[0].peer.address,6U);strcpy(out->id,"Drone hint");return ESP_OK;
}
esp_err_t cerberus_s3_ble_scanner_passive_get_activity(uint64_t epoch,cerberus_ble_activity_payload_t *out)
{
    memset(out,0,sizeof(*out));cerberus_s3_ble_passive_state_t state;
    esp_err_t err=recon_native_complete(epoch,&state);if(err!=ESP_OK)return err;
    if(!recon_lifecycle_active)return cerberus_s3_ble_scanner_get_activity(state.scan.generation,out);
    *out=(cerberus_ble_activity_payload_t){.generation=state.scan.generation,.state=CERBERUS_WATCH_COMPLETE,
        .duration_ms=8000};return ESP_OK;
}
