/* Exact native lease boundary for the complete production Wardrive worker.
 * Scanner callback ownership is independently exercised by the native-event
 * fixture. This model deliberately keeps public generation equal on replacement. */
static cerberus_s3_ble_passive_state_t passive_source;
static uint64_t passive_next_epoch, passive_hold_until;
static unsigned passive_starts, passive_queries, passive_stops, passive_releases;
static unsigned passive_start_fail_at, passive_fault_on_query, passive_copy_fault;
static unsigned worker_delays, worker_stop_after_delay;
static unsigned passive_release_failures;
static uint16_t passive_dropped;
static esp_err_t passive_start_error, passive_query_error, passive_stop_error;
static bool passive_accept_error, passive_foreign, passive_queued;
static bool passive_empty_rearm_error, passive_stop_on_copy;
static bool passive_replace_token;
static bool passive_completed_held;
static void (*worker_delay_hook)(void);

static void passive_complete(void)
{
    passive_source.scan = ble_status;
    passive_source.scan.state = CERBERUS_BLE_SCAN_COMPLETE;
    passive_source.scan.stored_count = 2U;
    passive_source.owned = passive_source.receiving = passive_source.stopping = false;
    ble_status = passive_source.scan;
}
static void passive_cancel(void)
{
    uint8_t generation=passive_source.scan.generation;
    passive_source.scan=(cerberus_ble_scan_status_payload_t){.generation=generation};
    passive_source.owned=passive_source.receiving=passive_source.stopping=false;
    ble_status=passive_source.scan;
}
static void passive_seed_completed(uint64_t epoch, uint8_t generation, uint8_t count)
{
    passive_source = (cerberus_s3_ble_passive_state_t){
        .epoch=epoch,.started_ms=1,.source_now_ms=(uint64_t)(clock_us/1000),
        .scan={.state=CERBERUS_BLE_SCAN_COMPLETE,.generation=generation,.stored_count=count},
        .scan_epoch=71,.host_epoch=8,.attached=true};
    ble_status=passive_source.scan;
}
static void passive_boundary_reset(void)
{
    passive_source=(cerberus_s3_ble_passive_state_t){0};
    passive_next_epoch=UINT64_C(0x100000001); passive_hold_until=0;
    passive_starts=passive_queries=passive_stops=passive_releases=0;
    passive_start_fail_at=passive_fault_on_query=passive_copy_fault=0;
    worker_delays=worker_stop_after_delay=0;worker_delay_hook=NULL;
    passive_release_failures=0;passive_dropped=0;
    passive_start_error=passive_query_error=passive_stop_error=ESP_OK;
    passive_accept_error=passive_foreign=passive_queued=false;
    passive_empty_rearm_error=passive_stop_on_copy=passive_replace_token=passive_completed_held=false;
}
esp_err_t cerberus_s3_ble_scanner_passive_start(uint64_t previous,
    cerberus_s3_ble_passive_state_t *out)
{
    CHECK(out!=NULL);memset(out,0,sizeof(*out));++passive_starts;
    if (previous && (!passive_source.attached || previous!=passive_source.epoch))
        return ESP_ERR_NOT_FOUND;
    if (passive_foreign || (passive_source.attached &&
        (!previous || passive_source.owned))) return ESP_ERR_INVALID_STATE;
    bool fail=passive_start_error && (!passive_start_fail_at ||
        passive_starts==passive_start_fail_at);
    if (fail && !passive_accept_error) {
        if (previous && !passive_empty_rearm_error) *out=passive_source;
        return passive_start_error;
    }
    ++rf_starts;
    uint8_t generation=(uint8_t)(ble_status.generation+1U);
    passive_source=(cerberus_s3_ble_passive_state_t){
        .epoch=passive_next_epoch++,.started_ms=(uint64_t)(clock_us/1000),
        .source_now_ms=(uint64_t)(clock_us/1000),.scan_epoch=passive_starts+70U,
        .host_epoch=8,.attached=true,.owned=true,.receiving=!passive_queued,
        .scan={.state=CERBERUS_BLE_SCAN_RUNNING,.generation=generation,
            .dropped_reports=passive_dropped}};
    if (fail) {passive_source.error=passive_start_error;passive_source.receiving=false;}
    ble_status=passive_source.scan;*out=passive_source;
    return fail?passive_start_error:ESP_OK;
}
esp_err_t cerberus_s3_ble_scanner_passive_get_state(uint64_t epoch,
    cerberus_s3_ble_passive_state_t *out)
{
    CHECK(out!=NULL);memset(out,0,sizeof(*out));++passive_queries;++status_polls;
    if (!epoch || !passive_source.attached || epoch!=passive_source.epoch)
        return ESP_ERR_NOT_FOUND;
    if (passive_query_error) return passive_query_error;
    if (passive_fault_on_query && passive_queries==passive_fault_on_query) {
        if(passive_replace_token){
            passive_foreign=true;passive_source.epoch+=UINT64_C(0x100000000);
            return ESP_ERR_NOT_FOUND;
        }
        passive_foreign=true;passive_source.error=ESP_ERR_INVALID_STATE;
        passive_source.owned=passive_source.receiving=passive_source.stopping=false;
        /* The foreign scanner may reuse exactly this public generation. */
        ble_status.state=CERBERUS_BLE_SCAN_RUNNING;
    }
    if (passive_source.owned && passive_source.stopping &&
        (uint64_t)(clock_us/1000)>=passive_hold_until) {
        if(passive_source.scan.state==CERBERUS_BLE_SCAN_COMPLETE)passive_complete();
        else passive_cancel();
    } else if (!passive_source.error && passive_source.owned && auto_complete &&
        !passive_source.stopping) {
        passive_complete();
        if(passive_completed_held && (uint64_t)(clock_us/1000)<passive_hold_until)
            passive_source.owned=true;
    }
    passive_source.source_now_ms=(uint64_t)(clock_us/1000);
    *out=passive_source;return ESP_OK;
}
esp_err_t cerberus_s3_ble_scanner_passive_stop(uint64_t epoch,
    cerberus_s3_ble_passive_state_t *out)
{
    CHECK(out!=NULL);memset(out,0,sizeof(*out));++passive_stops;
    CHECK(epoch!=0);
    if (!passive_source.attached || epoch!=passive_source.epoch) return ESP_ERR_NOT_FOUND;
    if (passive_stop_error) {*out=passive_source;return passive_stop_error;}
    if (passive_source.owned) {
        CHECK(!passive_foreign);
        passive_source.receiving=false;passive_source.stopping=true;
        if ((uint64_t)(clock_us/1000)>=passive_hold_until) {
            if(passive_source.scan.state==CERBERUS_BLE_SCAN_COMPLETE)passive_complete();
            else passive_cancel();
        }
    }
    passive_source.source_now_ms=(uint64_t)(clock_us/1000);
    *out=passive_source;return ESP_OK;
}
esp_err_t cerberus_s3_ble_scanner_passive_release(uint64_t epoch)
{
    ++passive_releases;
    if (!epoch || !passive_source.attached || epoch!=passive_source.epoch)
        return ESP_ERR_NOT_FOUND;
    if (passive_source.owned) return ESP_ERR_INVALID_STATE;
    if(passive_release_failures){--passive_release_failures;return ESP_ERR_TIMEOUT;}
    passive_source=(cerberus_s3_ble_passive_state_t){0};return ESP_OK;
}
esp_err_t cerberus_s3_ble_scanner_passive_copy_wardrive(uint64_t epoch,uint8_t count,
    cerberus_c5_ble_result_t *out,cerberus_s3_ble_wardrive_evidence_t *hints,
    size_t cap,cerberus_s3_ble_passive_state_t *state)
{
    CHECK(cap==32 && state!=NULL && out!=NULL && hints!=NULL);
    ++ble_copies;memset(out,0,cap*sizeof(*out));memset(hints,0,cap*sizeof(*hints));
    memset(state,0,sizeof(*state));
    if (!passive_source.attached || epoch!=passive_source.epoch) return ESP_ERR_NOT_FOUND;
    if (copy_error) return copy_error;
    if (passive_source.error || passive_source.owned || passive_source.stopping ||
        passive_source.scan.state!=CERBERUS_BLE_SCAN_COMPLETE ||
        count!=passive_source.scan.stored_count || count>cap) return ESP_ERR_INVALID_STATE;
    memcpy(out,ble_source,count*sizeof(*out));memcpy(hints,ble_evidence,count*sizeof(*hints));
    passive_source.source_now_ms=(uint64_t)(clock_us/1000);*state=passive_source;
    if (passive_copy_fault==1U) ++state->epoch;
    if (passive_copy_fault==4U) state->owned=true;
    if (passive_copy_fault==5U) state->error=ESP_ERR_INVALID_STATE;
    if (passive_stop_on_copy) cerberus_wardrive_request_stop();
    return ESP_OK;
}
static void passive_delay(unsigned ms)
{
    clock_us+=(int64_t)ms*1000;++worker_delays;
    CHECK(worker_delays<1000U); /* A lost cleanup handle fails deterministically. */
    if (worker_delay_hook) worker_delay_hook();
    if (worker_stop_after_delay && worker_delays==worker_stop_after_delay)
        cerberus_wardrive_request_stop();
}
