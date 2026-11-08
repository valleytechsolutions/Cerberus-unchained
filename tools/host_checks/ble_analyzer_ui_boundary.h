/* Counted scanner API boundary for actual Analyzer UI lifecycle tests. The
 * production scanner owner fixture independently tests its mutex/SDK fencing.
 * This model exposes exact host/scan replacement, pending receive and errors;
 * it never permits a generic Stop to stand in for an Analyzer-bound Stop. */
static struct {
    cerberus_s3_ble_analyzer_state_t state;
    esp_err_t begin_error,start_error,get_error,stop_error,history_error;
    uint32_t next_epoch,current_epoch;
    unsigned starts,stops,releases,queries,history_queries,foreign_stops;
    bool hold_stop,pending_start;
} analyzer_native;

static void analyzer_native_reconcile(void)
{
    if(analyzer_native.state.attached&&analyzer_native.state.owned&&
        (analyzer_native.state.host_epoch!=fox_host_epoch||
         analyzer_native.state.scan_epoch!=analyzer_native.current_epoch||
         analyzer_native.state.scan.generation!=scan.generation)){
        analyzer_native.state.error=ESP_ERR_INVALID_STATE;
        analyzer_native.state.owned=analyzer_native.state.receiving=analyzer_native.state.stopping=false;
    }
    if(analyzer_native.state.attached&&analyzer_native.state.owned&&
        owner==CERBERUS_S3_BLE_OWNER_NONE&&scan.state!=CERBERUS_BLE_SCAN_RUNNING){
        analyzer_native.state.scan=scan;
        analyzer_native.state.owned=analyzer_native.state.receiving=analyzer_native.state.stopping=false;
    }
}
static esp_err_t cerberus_s3_ble_scanner_analyzer_begin(void)
{
    ++analyzer_begins;
    if(analyzer_native.begin_error)return analyzer_native.begin_error;
    if(analyzer_native.state.attached||owner!=CERBERUS_S3_BLE_OWNER_NONE)return ESP_ERR_INVALID_STATE;
    analyzer_native.state=(cerberus_s3_ble_analyzer_state_t){.attached=true,.host_epoch=fox_host_epoch};
    memset(&analyzer_view,0,sizeof(analyzer_view));analyzer_view.active=true;return ESP_OK;
}
static esp_err_t cerberus_s3_ble_scanner_analyzer_get_state(cerberus_s3_ble_analyzer_state_t *out)
{
    ++analyzer_native.queries;
    memset(out,0,sizeof(*out));
    if(analyzer_native.get_error)return analyzer_native.get_error;
    analyzer_native_reconcile();*out=analyzer_native.state;return ESP_OK;
}
static esp_err_t cerberus_s3_ble_scanner_analyzer_start(cerberus_s3_ble_analyzer_state_t *out)
{
    ++analyzer_native.starts;analyzer_native_reconcile();
    if(!analyzer_native.state.attached||analyzer_native.state.owned||
        owner!=CERBERUS_S3_BLE_OWNER_NONE){*out=analyzer_native.state;return ESP_ERR_INVALID_STATE;}
    if(analyzer_native.start_error){*out=analyzer_native.state;return analyzer_native.start_error;}
    ++passive_starts;++scan.generation;scan.state=CERBERUS_BLE_SCAN_RUNNING;
    owner=CERBERUS_S3_BLE_OWNER_SCANNER;
    analyzer_native.state.scan=scan;analyzer_native.current_epoch=++analyzer_native.next_epoch;
    analyzer_native.state.scan_epoch=analyzer_native.current_epoch;
    analyzer_native.state.host_epoch=fox_host_epoch;analyzer_native.state.error=ESP_OK;
    analyzer_native.state.owned=true;analyzer_native.state.receiving=!analyzer_native.pending_start;
    *out=analyzer_native.state;return ESP_OK;
}
static esp_err_t cerberus_s3_ble_scanner_analyzer_stop(cerberus_s3_ble_analyzer_state_t *out)
{
    ++analyzer_native.stops;analyzer_native_reconcile();
    if(!analyzer_native.state.attached){*out=analyzer_native.state;return ESP_ERR_INVALID_STATE;}
    if(!analyzer_native.state.owned){*out=analyzer_native.state;return ESP_OK;}
    if(owner!=CERBERUS_S3_BLE_OWNER_SCANNER){++analyzer_native.foreign_stops;assert(!"Foreign Analyzer stop");}
    analyzer_native.state.stopping=true;analyzer_native.state.receiving=false;
    if(analyzer_native.stop_error){*out=analyzer_native.state;return analyzer_native.stop_error;}
    if(!analyzer_native.hold_stop){
        owner=CERBERUS_S3_BLE_OWNER_NONE;scan.state=CERBERUS_BLE_SCAN_COMPLETE;
        analyzer_native.state.scan=scan;analyzer_native.state.owned=analyzer_native.state.stopping=false;
    }
    *out=analyzer_native.state;return ESP_OK;
}
static esp_err_t cerberus_s3_ble_scanner_analyzer_release(void)
{
    ++analyzer_native.releases;analyzer_native_reconcile();
    if(analyzer_native.state.owned)return ESP_ERR_INVALID_STATE;
    if(analyzer_native.state.attached){++analyzer_ends;memset(&analyzer_view,0,sizeof(analyzer_view));}
    memset(&analyzer_native.state,0,sizeof(analyzer_native.state));return ESP_OK;
}
static void cerberus_s3_ble_scanner_analyzer_end(void)
{(void)cerberus_s3_ble_scanner_analyzer_release();}
static esp_err_t cerberus_s3_ble_scanner_analyzer_status(cerberus_ble_analyzer_view_t *out)
{
    ++analyzer_native.history_queries;
    memset(out,0,sizeof(*out));
    if(analyzer_native.history_error)return analyzer_native.history_error;
    if(!analyzer_view.active)return ESP_ERR_INVALID_STATE;
    *out=analyzer_view;return ESP_OK;
}
