/* Exact scanner lease boundary for extracted Fox UI. Actual scanner callbacks
 * and mutex fencing are verified separately by the native scanner fixture. */
static struct {
    cerberus_s3_ble_track_state_t state;
    cerberus_s3_ble_tracker_t tracker;
    uint32_t next_epoch,current_epoch;
    unsigned queries,starts,stops,releases,status_reads,callback_attempts,foreign_stops;
    bool queued;
} track_native;

static void track_native_reconcile(void)
{
    if(!track_native.state.attached)return;
    if(track_native.state.host_epoch!=fox_host_epoch||
        (track_native.state.scan_epoch&&track_native.state.scan_epoch!=track_native.current_epoch)||
        (track_native.state.owned&&track_native.state.scan.generation!=scan.generation)){
        track_native.state.error=ESP_ERR_INVALID_STATE;
        track_native.state.owned=track_native.state.receiving=track_native.state.stopping=false;
        cerberus_s3_ble_tracker_host_reset(&track_native.tracker);
    }else if(track_native.state.owned&&owner==CERBERUS_S3_BLE_OWNER_NONE&&scan.state!=CERBERUS_BLE_SCAN_RUNNING){
        track_native.state.scan=scan;
        track_native.state.owned=track_native.state.receiving=track_native.state.stopping=false;
        if(scan.state==CERBERUS_BLE_SCAN_ERROR)track_native.state.error=scan.error_code?scan.error_code:ESP_FAIL;
    }
}
static esp_err_t cerberus_s3_ble_scanner_track_begin(const uint8_t *address,uint8_t type)
{
    ++track_begins;if(track_error)return track_error;
    if(!address||type>3U)return ESP_ERR_INVALID_ARG;
    if(track_native.state.attached)return ESP_ERR_INVALID_STATE;
    memcpy(track_address,address,6);track_type=type;
    cerberus_s3_ble_tracker_begin(&track_native.tracker,address,type);
    track_native.state=(cerberus_s3_ble_track_state_t){.attached=true,.host_epoch=fox_host_epoch};
    return ESP_OK;
}
static esp_err_t cerberus_s3_ble_scanner_track_get_state(cerberus_s3_ble_track_state_t *out)
{
    ++track_native.queries;memset(out,0,sizeof(*out));
    if(fox_status_error)return fox_status_error;
    track_native_reconcile();*out=track_native.state;return ESP_OK;
}
static esp_err_t cerberus_s3_ble_scanner_track_start(cerberus_s3_ble_track_state_t *out)
{
    ++track_native.starts;++passive_starts;track_native_reconcile();
    if(!track_native.state.attached||track_native.state.owned||track_native.state.error||
        owner!=CERBERUS_S3_BLE_OWNER_NONE){*out=track_native.state;return ESP_ERR_INVALID_STATE;}
    if(fox_start_error){*out=track_native.state;return fox_start_error;}
    ++scan.generation;scan.state=CERBERUS_BLE_SCAN_RUNNING;scan.error_code=ESP_OK;
    owner=CERBERUS_S3_BLE_OWNER_SCANNER;
    track_native.state.scan=scan;track_native.current_epoch=++track_native.next_epoch;
    track_native.state.scan_epoch=track_native.current_epoch;track_native.state.host_epoch=fox_host_epoch;
    track_native.state.owned=true;track_native.state.receiving=!track_native.queued;
    *out=track_native.state;return ESP_OK;
}
static esp_err_t cerberus_s3_ble_scanner_track_stop(cerberus_s3_ble_track_state_t *out)
{
    ++track_native.stops;track_native_reconcile();
    if(!track_native.state.attached){*out=track_native.state;return ESP_ERR_INVALID_STATE;}
    if(!track_native.state.owned){*out=track_native.state;return ESP_OK;}
    if(owner!=CERBERUS_S3_BLE_OWNER_SCANNER){++track_native.foreign_stops;assert(!"Foreign track Stop");}
    ++stops;track_native.state.stopping=true;track_native.state.receiving=false;
    if(fox_stop_error){*out=track_native.state;return fox_stop_error;}
    if(!fox_hold_running&&!hold_stop){
        owner=CERBERUS_S3_BLE_OWNER_NONE;scan.state=CERBERUS_BLE_SCAN_COMPLETE;
        track_native.state.scan=scan;track_native.state.owned=track_native.state.stopping=false;
    }
    *out=track_native.state;return ESP_OK;
}
static esp_err_t cerberus_s3_ble_scanner_track_release(void)
{
    ++track_native.releases;track_native_reconcile();
    if(track_native.state.owned)return ESP_ERR_INVALID_STATE;
    if(track_native.state.attached)++track_ends;
    memset(&track_native.state,0,sizeof(track_native.state));cerberus_s3_ble_tracker_end(&track_native.tracker);
    return ESP_OK;
}
static void cerberus_s3_ble_scanner_track_end(void)
{(void)cerberus_s3_ble_scanner_track_release();}
static esp_err_t cerberus_s3_ble_scanner_track_status(cerberus_s3_ble_track_status_t *out)
{
    ++track_native.status_reads;memset(out,0,sizeof(*out));track_native_reconcile();
    if(!track_native.state.attached||track_native.state.error)return ESP_ERR_INVALID_STATE;
    cerberus_s3_ble_tracker_snapshot(&track_native.tracker,(uint32_t)clock_ms,out);return ESP_OK;
}
static bool track_native_callback(uint32_t epoch,unsigned host,const uint8_t *address,uint8_t type,int8_t rssi)
{
    ++track_native.callback_attempts;track_native_reconcile();
    if(!track_native.state.attached||!track_native.state.owned||!track_native.state.receiving||
        track_native.state.stopping||track_native.state.error||epoch!=track_native.state.scan_epoch||
        host!=track_native.state.host_epoch)return false;
    return cerberus_s3_ble_tracker_observe(&track_native.tracker,address,type,rssi,(uint32_t)clock_ms);
}
