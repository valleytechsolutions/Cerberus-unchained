/* Real scanner + SDK queue tests. The synthetic response/partial callbacks
 * preserve Fox's existing report semantics; the accepted scan stays passive. */
static unsigned track_binding_checks;
#define TB(x) do { ++track_binding_checks; assert(x); } while (0)
static void track_test_idle(void)
{
    TB(head==tail && !owner && !radio_active && !tracker.enabled && !analyzer);
    head=tail=0;expected_active=false;tick=2000;clock_base_us=0;
}
static void check_track_binding(void)
{
    const uint8_t target[]={1,2,3,4,5,6}, other[]={8,2,3,4,5,6}, ad[]={2,1,6};
    cerberus_s3_ble_track_state_t state;
    cerberus_s3_ble_track_status_t sample;
    cerberus_ble_scan_status_payload_t generic;
    track_test_idle();
    unsigned allocated=allocations,started=mac_radio_starts;
    TB(cerberus_s3_ble_scanner_track_start(NULL)==ESP_ERR_INVALID_ARG);
    TB(cerberus_s3_ble_scanner_track_get_state(NULL)==ESP_ERR_INVALID_ARG);
    TB(cerberus_s3_ble_scanner_track_stop(NULL)==ESP_ERR_INVALID_ARG);
    TB(cerberus_s3_ble_scanner_track_begin(NULL,1)==ESP_ERR_INVALID_ARG);
    TB(cerberus_s3_ble_scanner_track_begin(target,4)==ESP_ERR_INVALID_ARG);
    memset(&state,0xa5,sizeof(state));
    TB(cerberus_s3_ble_scanner_track_get_state(&state)==ESP_OK && all_zero(&state,sizeof(state)));
    TB(cerberus_s3_ble_scanner_track_start(&state)==ESP_ERR_INVALID_STATE && all_zero(&state,sizeof(state)));
    TB(cerberus_s3_ble_scanner_track_stop(&state)==ESP_OK && all_zero(&state,sizeof(state)));
    TB(cerberus_s3_ble_scanner_track_release()==ESP_OK && allocations==allocated);
    TB(cerberus_s3_ble_scanner_track_begin(target,1)==ESP_OK && allocations==allocated);
    TB(cerberus_s3_ble_scanner_track_begin(other,0)==ESP_ERR_INVALID_STATE);
    TB(cerberus_s3_ble_scanner_track_status(&sample)==ESP_OK && sample.enabled && !sample.seen &&
        sample.address_type==1 && !memcmp(sample.address,target,6));
    TB(cerberus_s3_ble_scanner_track_get_state(&state)==ESP_OK && state.attached &&
        !state.owned && !state.receiving && !state.scan_epoch && !state.error);
    TB(cerberus_s3_ble_scanner_start(&generic)==ESP_ERR_INVALID_STATE);
    TB(cerberus_s3_ble_scanner_start_active(&generic)==ESP_ERR_INVALID_STATE);
    TB(cerberus_s3_ble_scanner_analyzer_begin()==ESP_ERR_INVALID_STATE);
    TB(cerberus_s3_ble_scanner_track_start(&state)==ESP_OK && state.attached && state.owned &&
        !state.receiving && !state.stopping && state.scan_epoch && state.host_epoch==host_epoch);
    TB(cerberus_s3_ble_scanner_track_start(&state)==ESP_ERR_INVALID_STATE);
    TB(cerberus_s3_ble_scanner_stop(&generic)==ESP_ERR_INVALID_STATE && tail==1);
    cerberus_s3_ble_scanner_track_end();
    TB(tracker.enabled && cerberus_s3_ble_scanner_track_release()==ESP_ERR_INVALID_STATE);
    TB(cerberus_s3_ble_scanner_track_stop(&state)==ESP_OK && state.stopping && state.owned && !state.receiving);
    unsigned queued=tail;
    TB(cerberus_s3_ble_scanner_track_stop(&state)==ESP_OK && tail==queued);
    pump();TB(mac_radio_starts==started && cerberus_s3_ble_scanner_track_release()==ESP_ERR_INVALID_STATE);
    pump();
    TB(cerberus_s3_ble_scanner_track_release()==ESP_OK);
    TB(cerberus_s3_ble_scanner_track_get_state(&state)==ESP_OK && all_zero(&state,sizeof(state)));

    track_test_idle();
    TB(cerberus_s3_ble_scanner_track_begin(target,1)==ESP_OK);
    mac_start_error=71;
    TB(cerberus_s3_ble_scanner_track_start(&state)==ESP_OK);pump();
    TB(cerberus_s3_ble_scanner_track_get_state(&state)==ESP_OK && state.error==71 &&
        state.attached && !state.receiving && !state.owned);
    TB(cerberus_s3_ble_scanner_track_start(&state)==ESP_ERR_INVALID_STATE);
    memset(&sample,0xa5,sizeof(sample));
    TB(cerberus_s3_ble_scanner_track_status(&sample)==ESP_ERR_INVALID_STATE && all_zero(&sample,sizeof(sample)));
    TB(cerberus_s3_ble_scanner_track_release()==ESP_OK);mac_start_error=0;

    track_test_idle();
    TB(cerberus_s3_ble_scanner_track_begin(target,1)==ESP_OK);
    TB(cerberus_s3_ble_scanner_track_start(&state)==ESP_OK);pump();
    uint32_t first_epoch=state.scan_epoch;
    TB(cerberus_s3_ble_scanner_track_get_state(&state)==ESP_OK && state.owned && state.receiving);
    emit_hc_report(ad,sizeof(ad),-50,false,1,3);
    emit_hc_report(ad,sizeof(ad),-40,true,1,3);
    emit_hc_report(ad,sizeof(ad),-10,false,8,3); /* Different address. */
    cerberus_s3_ble_report_t report={.address={1,2,3,4,5,6},.address_type=0,
        .rssi=-5,.complete=true,.data=ad,.length=sizeof(ad),.sid=3};
    emit_wardrive_report(&report); /* Same bytes but a different address type. */
    report.address_type=1;report.rssi=127;emit_wardrive_report(&report);
    TB(cerberus_s3_ble_scanner_track_status(&sample)==ESP_OK && sample.reports==2 && sample.rssi_dbm==-40);
    report.rssi=-60;report.complete=false;emit_wardrive_report(&report);
    TB(cerberus_s3_ble_scanner_track_status(&sample)==ESP_OK && sample.reports==3 && sample.rssi_dbm==-60);
    TB(allocations==allocated && !active_scan);
    void *old_arg=radio_arg;
    analyzer_test_complete();
    TB(cerberus_s3_ble_scanner_track_get_state(&state)==ESP_OK && !state.owned && !state.receiving);
    TB(cerberus_s3_ble_scanner_start(&generic)==ESP_ERR_INVALID_STATE);
    TB(cerberus_s3_ble_scanner_track_begin(other,1)==ESP_ERR_INVALID_STATE);
    TB(cerberus_s3_ble_scanner_track_start(&state)==ESP_OK && state.scan_epoch>first_epoch);pump();
    struct ble_gap_event done={.type=BLE_GAP_EVENT_DISC_COMPLETE};
    gap_event(&done,old_arg);
    TB(cerberus_s3_ble_scanner_track_get_state(&state)==ESP_OK && state.receiving && state.owned);
    TB(cerberus_s3_ble_scanner_track_status(&sample)==ESP_OK && sample.reports==3);
    emit_hc_report(ad,sizeof(ad),-30,false,1,3);
    TB(cerberus_s3_ble_scanner_track_status(&sample)==ESP_OK && sample.reports==4 && sample.rssi_dbm==-30);
    tracker.reports=UINT32_MAX;emit_hc_report(ad,sizeof(ad),-31,false,1,3);
    TB(cerberus_s3_ble_scanner_track_status(&sample)==ESP_OK && sample.reports==UINT32_MAX && sample.rssi_dbm==-31);
    mac_cancel_error=72;
    TB(cerberus_s3_ble_scanner_track_stop(&state)==ESP_OK);pump();tick+=13000;
    TB(cerberus_s3_ble_scanner_track_get_state(&state)==ESP_OK && state.owned && state.stopping && !state.receiving && state.error==72);
    TB(cerberus_s3_ble_scanner_track_release()==ESP_ERR_INVALID_STATE);
    emit_hc_report(ad,sizeof(ad),-2,false,1,3);
    TB(tracker.rssi_dbm==-31 && tracker.reports==UINT32_MAX);
    mac_cancel_error=0;
    TB(cerberus_s3_ble_scanner_track_stop(&state)==ESP_OK);pump();
    TB(cerberus_s3_ble_scanner_track_release()==ESP_OK);

    track_test_idle();
    TB(cerberus_s3_ble_scanner_track_begin(target,1)==ESP_OK);
    TB(cerberus_s3_ble_scanner_track_start(&state)==ESP_OK);pump();
    fail_host_release=true;analyzer_test_complete();
    TB(cerberus_s3_ble_scanner_track_get_state(&state)==ESP_OK && state.owned && !state.receiving);
    TB(cerberus_s3_ble_scanner_track_release()==ESP_ERR_INVALID_STATE);
    TB(cerberus_s3_ble_scanner_track_stop(&state)==ESP_ERR_TIMEOUT && state.owned);
    fail_host_release=false;
    TB(cerberus_s3_ble_scanner_track_stop(&state)==ESP_OK && !state.owned);
    TB(cerberus_s3_ble_scanner_track_release()==ESP_OK);

    /* Host reset while queued, then while listening. Neither late SDK report
     * nor polling permits automatic rearm or adopts the replacement owner. */
    for(unsigned queued_reset=0;queued_reset<2;++queued_reset) {
        track_test_idle();
        TB(cerberus_s3_ble_scanner_track_begin(target,1)==ESP_OK);
        TB(cerberus_s3_ble_scanner_track_start(&state)==ESP_OK);
        unsigned old_host=state.host_epoch;
        if(!queued_reset) {pump();emit_hc_report(ad,sizeof(ad),-50,false,1,3);}
        started=mac_radio_starts;
        ++host_epoch;owner=CERBERUS_S3_BLE_OWNER_ADVERTISER;radio_active=false;
        if(!queued_reset)emit_hc_report(ad,sizeof(ad),-1,false,1,3);
        unsigned cancels=mac_radio_cancels,releases=host_releases;
        TB(cerberus_s3_ble_scanner_track_get_state(&state)==ESP_OK && state.error==ESP_ERR_INVALID_STATE &&
            state.host_epoch==old_host && !state.owned && !state.receiving);
        memset(&sample,0xa5,sizeof(sample));
        TB(cerberus_s3_ble_scanner_track_status(&sample)==ESP_ERR_INVALID_STATE && all_zero(&sample,sizeof(sample)));
        TB(cerberus_s3_ble_scanner_track_start(&state)==ESP_ERR_INVALID_STATE);
        TB(cerberus_s3_ble_scanner_track_stop(&state)==ESP_OK && owner==CERBERUS_S3_BLE_OWNER_ADVERTISER);
        TB(mac_radio_cancels==cancels && host_releases==releases);
        TB(cerberus_s3_ble_scanner_track_release()==ESP_OK);
        if(queued_reset)pump();
        TB(mac_radio_starts==started && !start_event_pending);
        owner=CERBERUS_S3_BLE_OWNER_NONE;
    }

    /* A fault-injected replacement scan shares generation8 and host epoch.
     * The never-reused callback epoch still excludes its reports and Stop. */
    track_test_idle();
    TB(cerberus_s3_ble_scanner_track_begin(target,1)==ESP_OK);
    TB(cerberus_s3_ble_scanner_track_start(&state)==ESP_OK);pump();
    emit_hc_report(ad,sizeof(ad),-50,false,1,3);
    const uint8_t generation=snapshot->status.generation;
    ++scan_epoch;radio_arg=(void *)(uintptr_t)scan_epoch;
    emit_hc_report(ad,sizeof(ad),-1,false,1,3);
    TB(tracker.reports==1 && tracker.rssi_dbm==-50 && snapshot->status.generation==generation);
    unsigned cancels=mac_radio_cancels,releases=host_releases;
    TB(cerberus_s3_ble_scanner_track_get_state(&state)==ESP_OK && state.attached && !state.owned &&
        !state.receiving && state.error==ESP_ERR_INVALID_STATE);
    TB(cerberus_s3_ble_scanner_track_stop(&state)==ESP_OK && mac_radio_cancels==cancels &&
        host_releases==releases && radio_active && owner==CERBERUS_S3_BLE_OWNER_SCANNER);
    TB(cerberus_s3_ble_scanner_track_release()==ESP_OK && radio_active && host_releases==releases);
    TB(cerberus_s3_ble_scanner_stop(&generic)==ESP_OK);pump();

    track_test_idle();
    TB(cerberus_s3_ble_scanner_track_begin(target,1)==ESP_OK);
    uint32_t saved_epoch=scan_epoch;uint64_t saved_watch=watch_epoch;
    scan_epoch=UINT32_MAX;
    TB(cerberus_s3_ble_scanner_track_start(&state)==ESP_ERR_INVALID_STATE && !state.owned && !owner && !tail);
    scan_epoch=saved_epoch;watch_epoch=UINT64_MAX;
    TB(cerberus_s3_ble_scanner_track_start(&state)==ESP_ERR_INVALID_STATE && !state.owned && !owner && !tail);
    watch_epoch=saved_watch;
    TB(cerberus_s3_ble_scanner_track_release()==ESP_OK);
    TB(cerberus_s3_ble_scanner_track_begin(other,0)==ESP_OK);
    TB(cerberus_s3_ble_scanner_track_status(&sample)==ESP_OK && sample.address_type==0 &&
        !memcmp(sample.address,other,6) && !sample.seen && !sample.reports);
    TB(cerberus_s3_ble_scanner_track_release()==ESP_OK);
    track_test_idle();
    printf("Native TRACK binding: %u checks; state %zu bytes; typed report semantics, exact epochs, held cleanup, reset/replacement and source isolation passed.\n",
        track_binding_checks,sizeof(state));
}
#undef TB
