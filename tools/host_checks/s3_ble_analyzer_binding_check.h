/* Included after the production scanner and SDK event shim. These tests drive
 * real queued callbacks; synthetic response injection does not enable active RF. */
static unsigned analyzer_binding_checks;
#define AB(x) do { ++analyzer_binding_checks; assert(x); } while (0)
static void analyzer_test_complete(void)
{
    struct ble_gap_event done={.type=BLE_GAP_EVENT_DISC_COMPLETE};
    radio_active=false;radio_callback(&done,radio_arg);
}
static void analyzer_test_idle(void)
{
    AB(head==tail && !owner && !radio_active && !analyzer);
    head=tail=0;expected_active=false;tick=1000;clock_base_us=0;
}
static void check_analyzer_binding(void)
{
    cerberus_s3_ble_analyzer_state_t state;
    cerberus_ble_scan_status_payload_t generic;
    cerberus_ble_analyzer_view_t view;
    const uint8_t ad[]={2,1,6};
    analyzer_test_idle();
    unsigned allocated=allocations,started=mac_radio_starts;
    AB(cerberus_s3_ble_scanner_analyzer_start(NULL)==ESP_ERR_INVALID_ARG);
    AB(cerberus_s3_ble_scanner_analyzer_get_state(NULL)==ESP_ERR_INVALID_ARG);
    AB(cerberus_s3_ble_scanner_analyzer_stop(NULL)==ESP_ERR_INVALID_ARG);
    memset(&state,0xa5,sizeof(state));
    AB(cerberus_s3_ble_scanner_analyzer_get_state(&state)==ESP_OK && all_zero(&state,sizeof(state)));
    AB(cerberus_s3_ble_scanner_analyzer_start(&state)==ESP_ERR_INVALID_STATE && all_zero(&state,sizeof(state)));
    AB(cerberus_s3_ble_scanner_analyzer_stop(&state)==ESP_OK && all_zero(&state,sizeof(state)));
    AB(cerberus_s3_ble_scanner_analyzer_release()==ESP_OK && allocations==allocated && mac_radio_starts==started);
    fail_discovery_allocation=true;
    AB(cerberus_s3_ble_scanner_analyzer_begin()==ESP_ERR_NO_MEM && !analyzer && !owner);
    AB(cerberus_s3_ble_scanner_analyzer_begin()==ESP_OK);
    allocated=allocations;
    AB(cerberus_s3_ble_scanner_analyzer_begin()==ESP_ERR_INVALID_STATE && allocations==allocated);
    AB(cerberus_s3_ble_scanner_analyzer_get_state(&state)==ESP_OK && state.attached &&
       !state.owned && !state.receiving && !state.scan_epoch && !state.error);
    AB(cerberus_s3_ble_scanner_start(&generic)==ESP_ERR_INVALID_STATE);
    AB(cerberus_s3_ble_scanner_start_active(&generic)==ESP_ERR_INVALID_STATE);
    AB(cerberus_s3_ble_scanner_analyzer_start(&state)==ESP_OK && state.attached && state.owned &&
       !state.receiving && !state.stopping && state.scan_epoch && state.host_epoch==host_epoch);
    uint32_t first_epoch=state.scan_epoch;
    AB(cerberus_s3_ble_scanner_analyzer_start(&state)==ESP_ERR_INVALID_STATE);
    AB(cerberus_s3_ble_scanner_stop(&generic)==ESP_ERR_INVALID_STATE && tail==1);
    unsigned freed=frees;
    cerberus_s3_ble_scanner_analyzer_end();
    AB(analyzer && frees==freed && cerberus_s3_ble_scanner_analyzer_release()==ESP_ERR_INVALID_STATE);
    AB(cerberus_s3_ble_scanner_analyzer_stop(&state)==ESP_OK && state.stopping && state.owned && !state.receiving);
    unsigned queued=tail;
    AB(cerberus_s3_ble_scanner_analyzer_stop(&state)==ESP_OK && tail==queued);
    pump(); /* Cancel before SDK Start: no RF. */
    AB(mac_radio_starts==started && cerberus_s3_ble_scanner_analyzer_release()==ESP_ERR_INVALID_STATE);
    pump();
    AB(cerberus_s3_ble_scanner_analyzer_get_state(&state)==ESP_OK && !state.owned && !state.receiving);
    AB(cerberus_s3_ble_scanner_analyzer_release()==ESP_OK && frees==freed+1);
    AB(cerberus_s3_ble_scanner_analyzer_get_state(&state)==ESP_OK && all_zero(&state,sizeof(state)));

    analyzer_test_idle();
    AB(cerberus_s3_ble_scanner_analyzer_begin()==ESP_OK);
    mac_start_error=71;
    AB(cerberus_s3_ble_scanner_analyzer_start(&state)==ESP_OK);pump();
    AB(cerberus_s3_ble_scanner_analyzer_get_state(&state)==ESP_OK && state.error==71 &&
       state.attached && !state.receiving && !state.owned && state.scan_epoch>first_epoch);
    AB(cerberus_s3_ble_scanner_analyzer_start(&state)==ESP_ERR_INVALID_STATE);
    AB(cerberus_s3_ble_scanner_analyzer_release()==ESP_OK);mac_start_error=0;

    analyzer_test_idle();
    AB(cerberus_s3_ble_scanner_analyzer_begin()==ESP_OK);
    AB(cerberus_s3_ble_scanner_analyzer_start(&state)==ESP_OK);pump();
    first_epoch=state.scan_epoch;
    AB(cerberus_s3_ble_scanner_analyzer_get_state(&state)==ESP_OK && state.receiving && state.owned);
    allocated=allocations;
    emit_hc_report(ad,sizeof(ad),-42,false,1,3);
    emit_hc_report(ad,sizeof(ad),-39,true,1,3); /* Synthetic RSP, separate event. */
    AB(cerberus_s3_ble_scanner_analyzer_status(&view)==ESP_OK && view.total_events==2 && view.last_scan_response);
    AB(allocations==allocated && !active_scan);
    void *old_arg=radio_arg;
    analyzer_test_complete();
    AB(cerberus_s3_ble_scanner_analyzer_get_state(&state)==ESP_OK && !state.owned && !state.receiving);
    AB(cerberus_s3_ble_scanner_start(&generic)==ESP_ERR_INVALID_STATE);
    AB(cerberus_s3_ble_scanner_analyzer_start(&state)==ESP_OK && state.scan_epoch>first_epoch);pump();
    /* Old SDK completion cannot finish this accepted sweep. */
    struct ble_gap_event done={.type=BLE_GAP_EVENT_DISC_COMPLETE};
    gap_event(&done,old_arg);
    AB(cerberus_s3_ble_scanner_analyzer_get_state(&state)==ESP_OK && state.receiving && state.owned);
    AB(cerberus_s3_ble_scanner_analyzer_status(&view)==ESP_OK && view.total_events==2);
    mac_cancel_error=72;
    AB(cerberus_s3_ble_scanner_analyzer_stop(&state)==ESP_OK);pump();
    tick+=13000;
    AB(cerberus_s3_ble_scanner_analyzer_get_state(&state)==ESP_OK && state.owned && state.stopping && !state.receiving && state.error==72);
    AB(cerberus_s3_ble_scanner_analyzer_release()==ESP_ERR_INVALID_STATE);
    emit_hc_report(ad,sizeof(ad),-20,false,2,3);
    AB(cerberus_s3_ble_scanner_analyzer_status(&view)==ESP_OK && view.total_events==2);
    mac_cancel_error=0;
    AB(cerberus_s3_ble_scanner_analyzer_stop(&state)==ESP_OK);pump();
    AB(cerberus_s3_ble_scanner_analyzer_release()==ESP_OK);

    analyzer_test_idle();
    AB(cerberus_s3_ble_scanner_analyzer_begin()==ESP_OK);
    AB(cerberus_s3_ble_scanner_analyzer_start(&state)==ESP_OK);pump();
    fail_host_release=true;analyzer_test_complete();
    AB(cerberus_s3_ble_scanner_analyzer_get_state(&state)==ESP_OK && state.owned && !state.receiving);
    AB(cerberus_s3_ble_scanner_analyzer_release()==ESP_ERR_INVALID_STATE);
    AB(cerberus_s3_ble_scanner_analyzer_stop(&state)==ESP_ERR_TIMEOUT && state.owned);
    fail_host_release=false;
    AB(cerberus_s3_ble_scanner_analyzer_stop(&state)==ESP_OK && !state.owned);
    AB(cerberus_s3_ble_scanner_analyzer_release()==ESP_OK);

    /* Host reset before the queued Start: retired callback never starts RF. */
    analyzer_test_idle();
    AB(cerberus_s3_ble_scanner_analyzer_begin()==ESP_OK);
    AB(cerberus_s3_ble_scanner_analyzer_start(&state)==ESP_OK);
    unsigned old_host=state.host_epoch;started=mac_radio_starts;
    ++host_epoch;owner=CERBERUS_S3_BLE_OWNER_NONE;
    AB(cerberus_s3_ble_scanner_analyzer_get_state(&state)==ESP_OK && state.error==ESP_ERR_INVALID_STATE &&
       state.host_epoch==old_host && !state.owned && !state.receiving);
    memset(&view,0xa5,sizeof(view));
    AB(cerberus_s3_ble_scanner_analyzer_status(&view)==ESP_ERR_INVALID_STATE && all_zero(&view,sizeof(view)));
    AB(cerberus_s3_ble_scanner_analyzer_release()==ESP_OK);pump();
    AB(mac_radio_starts==started && !start_event_pending);

    /* Reset while SDK reception is active: even before get_state(), a late
     * report cannot enter the prior history. A different owner is untouched. */
    analyzer_test_idle();
    AB(cerberus_s3_ble_scanner_analyzer_begin()==ESP_OK);
    AB(cerberus_s3_ble_scanner_analyzer_start(&state)==ESP_OK);pump();
    emit_hc_report(ad,sizeof(ad),-50,false,3,3);
    const uint32_t before_reset_events=analyzer->total_events;
    ++host_epoch;owner=CERBERUS_S3_BLE_OWNER_ADVERTISER;radio_active=false;
    emit_hc_report(ad,sizeof(ad),-5,false,3,3);
    AB(analyzer->total_events==before_reset_events && !analyzer->active);
    unsigned reset_cancels=mac_radio_cancels,reset_releases=host_releases;
    AB(cerberus_s3_ble_scanner_analyzer_stop(&state)==ESP_OK && !state.owned &&
       state.error==ESP_ERR_INVALID_STATE && owner==CERBERUS_S3_BLE_OWNER_ADVERTISER);
    AB(mac_radio_cancels==reset_cancels && host_releases==reset_releases);
    AB(cerberus_s3_ble_scanner_start(&generic)==ESP_ERR_INVALID_STATE);
    AB(cerberus_s3_ble_scanner_analyzer_release()==ESP_OK && owner==CERBERUS_S3_BLE_OWNER_ADVERTISER);
    owner=CERBERUS_S3_BLE_OWNER_NONE;

    /* Fault-injected foreign scanner with identical generation8/owner label:
     * scan_epoch is different, so neither callbacks nor Stop may touch it. */
    analyzer_test_idle();
    AB(cerberus_s3_ble_scanner_analyzer_begin()==ESP_OK);
    AB(cerberus_s3_ble_scanner_analyzer_start(&state)==ESP_OK);pump();
    emit_hc_report(ad,sizeof(ad),-50,false,3,3);
    const uint32_t events=analyzer->total_events;
    const uint8_t generation=snapshot->status.generation;
    ++scan_epoch;radio_arg=(void *)(uintptr_t)scan_epoch;
    emit_hc_report(ad,sizeof(ad),-10,false,4,3); /* Before any status reconciliation. */
    AB(analyzer->total_events==events && snapshot->status.generation==generation);
    unsigned cancels=mac_radio_cancels,releases=host_releases;
    AB(cerberus_s3_ble_scanner_analyzer_get_state(&state)==ESP_OK && state.attached && !state.owned &&
       !state.receiving && state.error==ESP_ERR_INVALID_STATE);
    AB(cerberus_s3_ble_scanner_analyzer_stop(&state)==ESP_OK && mac_radio_cancels==cancels &&
       host_releases==releases && radio_active && owner==CERBERUS_S3_BLE_OWNER_SCANNER);
    AB(cerberus_s3_ble_scanner_analyzer_release()==ESP_OK && radio_active && host_releases==releases);
    AB(cerberus_s3_ble_scanner_stop(&generic)==ESP_OK);pump();

    analyzer_test_idle();
    AB(cerberus_s3_ble_scanner_analyzer_begin()==ESP_OK);
    uint32_t saved_epoch=scan_epoch;uint64_t saved_watch=watch_epoch;
    scan_epoch=UINT32_MAX;
    AB(cerberus_s3_ble_scanner_analyzer_start(&state)==ESP_ERR_INVALID_STATE && !state.owned && !owner && !tail);
    scan_epoch=saved_epoch;watch_epoch=UINT64_MAX;
    AB(cerberus_s3_ble_scanner_analyzer_start(&state)==ESP_ERR_INVALID_STATE && !state.owned && !owner && !tail);
    watch_epoch=saved_watch;
    AB(cerberus_s3_ble_scanner_analyzer_release()==ESP_OK);
    analyzer_test_idle();
    printf("Native Analyzer binding: %u checks; state %zu bytes; passive queued Start, exact epochs, held Stop, reset/replacement and no foreign history passed.\n",
        analyzer_binding_checks,sizeof(state));
}
#undef AB
