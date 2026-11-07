/* These cases call the actual service worker, not a reconstructed scheduler. */
static void run_pending_worker(void)
{
    CHECK(pending_task!=NULL);void (*worker)(void *)=pending_task;
    pending_task=NULL;worker(NULL);
    CHECK(!worker_active && !workspace && !storage_owned);
}
static void assert_no_ble_rows(void)
{
    cerberus_wardrive_recent_snapshot_t view=snapshot();
    for (unsigned i=0;i<view.count;++i)
        CHECK(view.rows[i].observation.type!=CERBERUS_WARDRIVE_BLE);
    CHECK(!wardrive_status.s3_ble_observations && !wardrive_status.s3_ble_logged);
}
static unsigned held_checks,held_appends,held_starts;
static void check_held_cleanup(void)
{
    if ((uint64_t)(clock_us/1000)<20000U ||
        (uint64_t)(clock_us/1000)>=passive_hold_until) return;
    CHECK(worker_active && workspace!=NULL && storage_owned);
    CHECK(wardrive_status.state==CERBERUS_WARDRIVE_STOPPING);
    CHECK(passive_source.attached && passive_source.owned);
    CHECK(cerberus_wardrive_start(false,true)==ESP_ERR_INVALID_STATE);
    if (held_checks) CHECK(sd_appends==held_appends && rf_starts==held_starts);
    held_appends=sd_appends;held_starts=rf_starts;++held_checks;
}
static void run_passive_worker_cases(void)
{
    unsigned before=checks;
    /* GPS cleanup is a held resource after radios/files drain, never an early
     * terminal state or workspace free. The same worker retries with backoff. */
    reset();CHECK(cerberus_wardrive_start(false,true)==ESP_OK);
    auto_complete=true;stop_after_append=4;gps_stop_failures=4;inspect_gps_cleanup=true;
    run_pending_worker();
    CHECK(gps_stop_calls==5 && gps_cleanup_checks==5 && !gps_stop_failures);
    CHECK(wardrive_status.state==CERBERUS_WARDRIVE_STOPPED && wardrive_status.gps_error==ESP_OK);
    CHECK(sd_appends==4 && worker_delays>=4 && frees==1 && !workspace);
    CHECK(!passive_source.attached && !storage_owned && !worker_active);
    /* The next session can start normally with the preview fix retained. */
    reset();CHECK(cerberus_wardrive_start(false,true)==ESP_OK);
    cerberus_wardrive_request_stop();run_pending_worker();
    CHECK(gps_calls && gps_stop_calls==1 && wardrive_status.state==CERBERUS_WARDRIVE_STOPPED);
    /* Existing first-use host warming may yield. A replacement/reset must not
     * cause the wrapper to release another caller's shared owner label. */
    for(unsigned fault=0;fault<5;++fault){
        reset();host_initialized_value=false;
        reset_during_claim=fault==1;replace_during_claim=fault==2;
        reset_before_release=fault==3;replace_before_release=fault==4;
        esp_err_t result=cerberus_wardrive_start(false,true);
        CHECK(host_claims==1);
        if(fault){
            CHECK(result==ESP_ERR_INVALID_STATE && host_releases==(fault>=3?1U:0U) && !workspace &&
                !worker_active && !task_creates && !storage_owned && !allocations);
            CHECK(host_owner_value==(fault==2?CERBERUS_S3_BLE_OWNER_CENTRAL:
                fault==4?CERBERUS_S3_BLE_OWNER_SCANNER:CERBERUS_S3_BLE_OWNER_NONE));
            if(fault==4)CHECK(host_live_lease.token==2 && host_live_lease.epoch==8);
        }else{
            CHECK(result==ESP_OK && host_releases==1 && host_owner_value==CERBERUS_S3_BLE_OWNER_NONE);
            cerberus_wardrive_request_stop();run_pending_worker();
            CHECK(!rf_starts && !passive_starts && !passive_stops && !passive_releases);
        }
    }
    /* A race after admission cannot turn a local rejection into ownership of
     * a foreign scanner with the same public generation/history. */
    const esp_err_t rejected[]={ESP_ERR_INVALID_STATE,ESP_ERR_NO_MEM,ESP_FAIL};
    for(unsigned i=0;i<sizeof(rejected)/sizeof(rejected[0]);++i){
        reset();CHECK(cerberus_wardrive_start(false,true)==ESP_OK);
        passive_start_error=rejected[i];auto_complete=true;
        run_pending_worker();
        CHECK(wardrive_status.state==CERBERUS_WARDRIVE_ERROR &&
            wardrive_status.s3_ble_error==rejected[i]);
        CHECK(passive_starts==1 && passive_queries==0 && passive_stops==0 &&
            passive_releases==0 && ble_copies==0);
        assert_no_ble_rows();CHECK(gps_calls>0);
    }
    reset();CHECK(cerberus_wardrive_start(false,true)==ESP_OK);
    passive_foreign=true;ble_status.generation=77;ble_status.state=CERBERUS_BLE_SCAN_RUNNING;
    auto_complete=true;run_pending_worker();
    CHECK(passive_starts==1 && !passive_stops && !passive_releases && !ble_copies);
    CHECK(ble_status.generation==77 && ble_status.state==CERBERUS_BLE_SCAN_RUNNING);
    assert_no_ble_rows();

    reset();CHECK(cerberus_wardrive_start(false,true)==ESP_OK);
    auto_complete=true;passive_fault_on_query=1;passive_replace_token=true;
    run_pending_worker();
    CHECK(passive_foreign && passive_source.attached && !passive_releases && !passive_stops);
    CHECK(!ble_copies && wardrive_status.s3_ble_error!=ESP_OK);
    assert_no_ble_rows();

    /* First-bank contradictory proof cannot publish rows, even when a native
     * boundary writes the destination before returning its failed binding. */
    const unsigned proof_faults[]={1,4,5};
    for(unsigned i=0;i<sizeof(proof_faults)/sizeof(proof_faults[0]);++i){
        reset();CHECK(cerberus_wardrive_start(false,true)==ESP_OK);
        auto_complete=true;passive_copy_fault=proof_faults[i];run_pending_worker();
        CHECK(ble_copies==1 && passive_releases==1 && wardrive_status.s3_ble_error!=ESP_OK);
        assert_no_ble_rows();
    }

    /* A matching faulted token is an old attachment, not authority over the
     * replacement. The old gen8 remains unchanged to defeat byte-only gates. */
    reset();CHECK(cerberus_wardrive_start(false,true)==ESP_OK);
    auto_complete=true;passive_fault_on_query=1;run_pending_worker();
    CHECK(passive_foreign && passive_releases==1 && !ble_copies);
    CHECK(wardrive_status.s3_ble_error==ESP_ERR_INVALID_STATE);
    assert_no_ble_rows();

    /* Accepted error with a token must outlive the old nine-second warning.
     * The existing terminal-source policy stops the session; cleanup keeps
     * its workspace/storage owner until the exact S3 lease releases. */
    reset();CHECK(cerberus_wardrive_start(false,true)==ESP_OK);
    passive_start_error=ESP_ERR_NO_MEM;passive_accept_error=true;
    passive_hold_until=28000;worker_delay_hook=check_held_cleanup;
    held_checks=held_appends=held_starts=0;run_pending_worker();
    CHECK(held_checks>10 && passive_starts==1 && passive_stops>1 && passive_releases==1);
    CHECK(!passive_source.attached && !ble_copies && wardrive_status.s3_ble_error!=ESP_OK);
    assert_no_ble_rows();

    /* Native cancellation discards an incomplete queued/receiving sweep and
     * publishes canonical IDLE, not an invented completed result bank. */
    for(unsigned queued=0;queued<2;++queued){
        reset();CHECK(cerberus_wardrive_start(false,true)==ESP_OK);
        passive_queued=queued!=0;worker_stop_after_delay=1;run_pending_worker();
        CHECK(wardrive_status.state==CERBERUS_WARDRIVE_STOPPED && passive_stops==1 &&
            passive_releases==1 && !ble_copies && !wardrive_status.s3_ble_sweeps);
        CHECK(!sd_appends && !snapshot().count && !passive_source.attached);
    }

    /* A naturally completed bank can precede release of its native resources.
     * Hold that exact release beyond the warning, drain once, and restart. */
    reset();CHECK(cerberus_wardrive_start(false,true)==ESP_OK);
    passive_completed_held=true;auto_complete=true;worker_stop_after_delay=1;passive_hold_until=28000;
    wifi_source[0].channel=wifi_source[1].channel=0;
    worker_delay_hook=check_held_cleanup;held_checks=held_appends=held_starts=0;
    run_pending_worker();CHECK(held_checks>10 && passive_releases==1 && ble_copies==1);
    CHECK(wardrive_status.s3_ble_observations==2 && sd_appends==2 && snapshot().count==2);
    uint64_t first_session=snapshot().session_id;
    worker_delay_hook=NULL;worker_stop_after_delay=0;passive_hold_until=0;
    passive_completed_held=false;auto_complete=true;stop_after_append=sd_appends+4;
    wifi_source[0].channel=wifi_source[1].channel=6;
    CHECK(cerberus_wardrive_start(false,true)==ESP_OK);run_pending_worker();
    CHECK(snapshot().session_id==first_session+1 && snapshot().count==4);
    CHECK(wardrive_status.s3_ble_observations==2 && sd_appends==6 && passive_releases==2);

    /* A failed exact rearm preserves its prior completed token. Also keep the
     * conservative empty-reply case: absence of new proof cannot erase old. */
    for(unsigned empty=0;empty<2;++empty){
        reset();CHECK(cerberus_wardrive_start(false,true)==ESP_OK);
        auto_complete=true;passive_start_error=ESP_ERR_NO_MEM;passive_start_fail_at=2;passive_dropped=3;
        passive_empty_rearm_error=empty!=0;run_pending_worker();
        CHECK(passive_starts==2 && passive_releases==1 && !passive_source.attached);
        CHECK(ble_copies==1 && wardrive_status.s3_ble_observations==2);
        CHECK(wardrive_status.s3_ble_sweeps==1 && wardrive_status.s3_ble_scan_dropped==3);
        CHECK(wardrive_status.s3_ble_error==ESP_ERR_NO_MEM);
    }
    reset();CHECK(cerberus_wardrive_start(false,true)==ESP_OK);
    wifi_source[0].channel=wifi_source[1].channel=0;auto_complete=true;
    passive_stop_on_copy=true;passive_release_failures=3;passive_dropped=5;
    run_pending_worker();
    CHECK(passive_releases==4 && !passive_source.attached && ble_copies==1);
    CHECK(wardrive_status.s3_ble_sweeps==1 && wardrive_status.s3_ble_scan_dropped==5);
    CHECK(wardrive_status.s3_ble_observations==2 && sd_appends==2 && snapshot().count==2);
    CHECK(wardrive_status.s3_ble_error==ESP_OK);

    /* A lost C5 START reply may still mean accepted RF; a lock timeout means
     * no command. Both existing reconciliation paths preserve S3 progress. */
    const esp_err_t c5_start_faults[]={ESP_ERR_TIMEOUT,ESP_ERR_INVALID_RESPONSE};
    for(unsigned i=0;i<sizeof(c5_start_faults)/sizeof(c5_start_faults[0]);++i){
        reset();CHECK(cerberus_wardrive_start(false,true)==ESP_OK);
        wifi_first_start_error=c5_start_faults[i];auto_complete=true;stop_after_append=4;
        run_pending_worker();CHECK(wardrive_status.state==CERBERUS_WARDRIVE_STOPPED);
        CHECK(sd_appends==4 && wardrive_status.s3_ble_observations>=2 &&
            wifi_start_calls>=2 && passive_releases>=1 && gps_calls>0);
    }

    /* SD failure is terminal storage evidence, not a fabricated BLE source
     * fault. One successful prefix and the possibly partial failed write are
     * never replayed while the exact native attachment is being released. */
    for(unsigned failed_row=1;failed_row<=2;++failed_row){
        reset();CHECK(cerberus_wardrive_start(false,true)==ESP_OK);
        wifi_source[0].channel=wifi_source[1].channel=0;auto_complete=true;
        append_error=ESP_FAIL;fail_append_at=failed_row;run_pending_worker();
        CHECK(wardrive_status.state==CERBERUS_WARDRIVE_ERROR &&
            wardrive_status.sd_error==ESP_FAIL && wardrive_status.s3_ble_error==ESP_OK);
        CHECK(passive_releases==1 && !passive_source.attached && ble_copies==1);
        CHECK(sd_appends==failed_row && partial_bytes>0 &&
            wardrive_status.s3_ble_observations==failed_row-1);
        CHECK(snapshot().count==failed_row &&
            snapshot().rows[0].outcome==CERBERUS_WARDRIVE_RECENT_ERROR);
    }
    printf("Wardriving exact passive worker: %u boundary/lifecycle checks pass\n",checks-before);
}
