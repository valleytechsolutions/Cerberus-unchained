/* Production scanner, callback queue and immutable completed evidence. */
static unsigned passive_binding_checks;
#define PB(x) do { ++passive_binding_checks; assert(x); } while (0)

static void passive_test_idle(void)
{
    PB(head==tail && !owner && !radio_active && !passive_state.attached &&
        !tracker.enabled && !analyzer && !discovery_attached && !mac_monitor);
    head=tail=0; expected_active=false; tick=2000; clock_base_us=0;
    available_internal=128U*1024U; simulated_wifi_owner=CERBERUS_S3_WIFI_NONE;
    claim_before=claim_after=NULL;
}

static void passive_claim_foreign(void)
{ owner=CERBERUS_S3_BLE_OWNER_CENTRAL; }
static void passive_claim_wifi(void)
{ simulated_wifi_owner=CERBERUS_S3_WIFI_RECEIVER; }
static void passive_claim_cancel(void)
{
    cerberus_s3_ble_passive_state_t state;
    cerberus_ble_scan_status_payload_t generic;
    PB(!held && passive_acquiring && passive_state.attached);
    PB(cerberus_s3_ble_scanner_start(&generic)==ESP_ERR_INVALID_STATE);
    PB(cerberus_s3_ble_scanner_passive_stop(passive_state.epoch,&state)==ESP_OK && state.owned && state.stopping);
    PB(cerberus_s3_ble_scanner_passive_release(state.epoch)==ESP_ERR_INVALID_STATE);
}

static void passive_assert_copy_failure(uint64_t epoch,esp_err_t expected)
{
    cerberus_s3_ble_passive_state_t state;
    cerberus_s3_ble_discovery_record_t row;
    cerberus_watch_scan_status_payload_t watch;
    cerberus_watch_observation_t result;
    cerberus_watch_receive_timing_t timing,motion_timing;
    sentinel_watch_motion_t motion;
    cerberus_ble_activity_payload_t activity;
    memset(&row,0xa5,sizeof(row));memset(&state,0xa5,sizeof(state));
    PB(cerberus_s3_ble_scanner_passive_copy_row(epoch,0,&row,&state)==expected &&
        all_zero(&row,sizeof(row)) && all_zero(&state,sizeof(state)));
    memset(&watch,0xa5,sizeof(watch));
    PB(cerberus_s3_ble_scanner_passive_get_watch_status(epoch,&watch)==expected && all_zero(&watch,sizeof(watch)));
    memset(&result,0xa5,sizeof(result));
    PB(cerberus_s3_ble_scanner_passive_get_watch_result(epoch,0,&result)==expected && all_zero(&result,sizeof(result)));
    memset(&result,0xa5,sizeof(result));memset(&timing,0xa5,sizeof(timing));
    memset(&motion,0xa5,sizeof(motion));memset(&motion_timing,0xa5,sizeof(motion_timing));
    PB(cerberus_s3_ble_scanner_passive_get_watch_detailed_result(epoch,0,&result,&timing,&motion,&motion_timing)==expected &&
        all_zero(&result,sizeof(result)) && all_zero(&timing,sizeof(timing)) &&
        all_zero(&motion,sizeof(motion)) && all_zero(&motion_timing,sizeof(motion_timing)));
    memset(&activity,0xa5,sizeof(activity));
    PB(cerberus_s3_ble_scanner_passive_get_activity(epoch,&activity)==expected && all_zero(&activity,sizeof(activity)));
}

static void check_passive_binding(void)
{
    const uint8_t ordinary[]={2,1,6}, findmy[]={3,3,0x44,0xfd};
    const uint8_t target[]={1,2,3,4,5,6};
    cerberus_s3_ble_passive_state_t state,copy_state;
    cerberus_ble_scan_status_payload_t generic;
    cerberus_s3_ble_discovery_record_t row,frozen;
    cerberus_s3_ble_discovery_meta_t discovery;
    passive_test_idle();
    unsigned allocated=allocations,started=mac_radio_starts,cancelled=mac_radio_cancels,released=host_releases;
    PB(cerberus_s3_ble_scanner_passive_start(0,NULL)==ESP_ERR_INVALID_ARG);
    PB(cerberus_s3_ble_scanner_passive_get_state(1,NULL)==ESP_ERR_INVALID_ARG);
    PB(cerberus_s3_ble_scanner_passive_stop(1,NULL)==ESP_ERR_INVALID_ARG);
    PB(cerberus_s3_ble_scanner_passive_get_state(0,&state)==ESP_ERR_INVALID_ARG && all_zero(&state,sizeof(state)));
    PB(cerberus_s3_ble_scanner_passive_release(0)==ESP_ERR_INVALID_ARG);
    PB(cerberus_s3_ble_scanner_passive_get_state(1,&state)==ESP_ERR_NOT_FOUND && all_zero(&state,sizeof(state)));
    passive_assert_copy_failure(1,ESP_ERR_NOT_FOUND);
    PB(cerberus_s3_ble_scanner_passive_start(99,&state)==ESP_ERR_NOT_FOUND && all_zero(&state,sizeof(state)));
    available_internal=32767;
    PB(cerberus_s3_ble_scanner_passive_start(0,&state)==ESP_ERR_NO_MEM && all_zero(&state,sizeof(state)) && !owner);
    available_internal=128U*1024U;
    cerberus_s3_ble_snapshot_t *saved_snapshot=snapshot;snapshot=NULL;fail_discovery_allocation=true;
    PB(cerberus_s3_ble_scanner_passive_start(0,&state)==ESP_ERR_NO_MEM && all_zero(&state,sizeof(state)) && !owner);
    snapshot=saved_snapshot;allocated=allocations;
    PB(host_releases==released && mac_radio_starts==started && mac_radio_cancels==cancelled);

    /* A rejected Start never adopts any foreign generic scan or host owner. */
    PB(cerberus_s3_ble_scanner_start(&generic)==ESP_OK);pump();
    PB(cerberus_s3_ble_scanner_passive_start(0,&state)==ESP_ERR_INVALID_STATE && all_zero(&state,sizeof(state)));
    PB(cerberus_s3_ble_scanner_passive_stop(1,&state)==ESP_ERR_NOT_FOUND && radio_active && mac_radio_cancels==cancelled);
    analyzer_test_complete();
    owner=CERBERUS_S3_BLE_OWNER_CENTRAL;
    PB(cerberus_s3_ble_scanner_passive_start(0,&state)==ESP_ERR_INVALID_STATE && all_zero(&state,sizeof(state)) &&
        owner==CERBERUS_S3_BLE_OWNER_CENTRAL);
    owner=CERBERUS_S3_BLE_OWNER_NONE;
    claim_before=passive_claim_foreign;
    PB(cerberus_s3_ble_scanner_passive_start(0,&state)==ESP_ERR_INVALID_STATE && all_zero(&state,sizeof(state)) &&
        !passive_state.attached && owner==CERBERUS_S3_BLE_OWNER_CENTRAL);
    claim_before=NULL;owner=CERBERUS_S3_BLE_OWNER_NONE;

    passive_test_idle();
    PB(cerberus_s3_ble_scanner_passive_start(0,&state)==ESP_OK && state.attached && state.owned &&
        !state.receiving && !state.stopping && state.epoch && state.scan_epoch && !active_scan);
    uint64_t epoch=state.epoch;
    PB(cerberus_s3_ble_scanner_start(&generic)==ESP_ERR_INVALID_STATE);
    PB(cerberus_s3_ble_scanner_start_active(&generic)==ESP_ERR_INVALID_STATE);
    PB(cerberus_s3_ble_scanner_stop(&generic)==ESP_ERR_INVALID_STATE);
    PB(cerberus_s3_ble_scanner_track_begin(target,1)==ESP_ERR_INVALID_STATE);
    PB(cerberus_s3_ble_scanner_analyzer_begin()==ESP_ERR_INVALID_STATE);
    PB(cerberus_s3_ble_scanner_start_discovery(CERBERUS_S3_BLE_FILTER_ALL,0,&discovery)==ESP_ERR_INVALID_STATE);
    PB(cerberus_s3_ble_scanner_passive_start(0,&copy_state)==ESP_ERR_INVALID_STATE && all_zero(&copy_state,sizeof(copy_state)));
    PB(cerberus_s3_ble_scanner_passive_start(epoch,&copy_state)==ESP_ERR_INVALID_STATE && copy_state.epoch==epoch && copy_state.owned);
    PB(cerberus_s3_ble_scanner_passive_release(epoch)==ESP_ERR_INVALID_STATE);
    passive_assert_copy_failure(epoch,ESP_ERR_INVALID_STATE);
    PB(cerberus_s3_ble_scanner_passive_stop(epoch+1,&copy_state)==ESP_ERR_NOT_FOUND && !cancel_requested);
    started=mac_radio_starts;
    PB(cerberus_s3_ble_scanner_passive_stop(epoch,&state)==ESP_OK && state.stopping && state.owned);
    unsigned queued=tail;
    PB(cerberus_s3_ble_scanner_passive_stop(epoch,&state)==ESP_OK && tail==queued);
    pump();PB(mac_radio_starts==started && cerberus_s3_ble_scanner_passive_release(epoch)==ESP_ERR_INVALID_STATE);
    pump();PB(cerberus_s3_ble_scanner_passive_get_state(epoch,&state)==ESP_OK && !state.owned && !state.receiving);
    PB(cerberus_s3_ble_scanner_passive_release(epoch)==ESP_OK);

    /* Original and response evidence share one exact completed lease. This
     * synthetic response tests parser separation, not active RF acquisition. */
    passive_test_idle();tick=0;clock_base_us=(UINT64_C(1)<<32)*1000U;
    PB(cerberus_s3_ble_scanner_passive_start(0,&state)==ESP_OK);epoch=state.epoch;pump();
    PB(cerberus_s3_ble_scanner_passive_get_state(epoch,&state)==ESP_OK && state.receiving &&
        state.started_ms==(UINT64_C(1)<<32) && state.source_now_ms==state.started_ms);
    emit_hc_report(ordinary,sizeof(ordinary),-70,false,1,3);
    const uint32_t original_time=tick;
    emit_hc_report(findmy,sizeof(findmy),-20,true,1,3);
    uint8_t basic[31]={30,0x16,0xfa,0xff,0x0d,0,0x02,0x10,'R','E','C','O','N'};
    uint8_t location[31]={30,0x16,0xfa,0xff,0x0d,0,0x12,7,180,1};
    location[11]=1;location[15]=2;location[21]=0xd1;location[22]=7;
    emit_hc_report(basic,sizeof(basic),-40,false,2,3);
    emit_hc_report(location,sizeof(location),-41,false,2,3);
    analyzer_test_complete();
    unsigned locks=mutex_takes;allocated=allocations;started=mac_radio_starts;
    PB(cerberus_s3_ble_scanner_passive_copy_row(epoch,0,&row,&state)==ESP_OK && mutex_takes==locks+1);
    PB(state.epoch==epoch && state.scan.state==CERBERUS_BLE_SCAN_COMPLETE && !state.owned &&
        row.observed_ms==original_time && row.peer.rssi==-70 && row.advertisement.reports_seen==1 &&
        row.focus.discovery.findmy_scan_response.bits && !row.focus.discovery.findmy_advertisement.bits &&
        row.focus.discovery.raw_advertisement_sequence && row.generation==state.scan.generation &&
        row.peer.address_type==1 && row.peer.address[0]==1);
    frozen=row;
    cerberus_watch_scan_status_payload_t watch;
    cerberus_watch_observation_t watch_row;
    cerberus_watch_receive_timing_t timing,motion_timing;
    sentinel_watch_motion_t motion;
    cerberus_ble_activity_payload_t activity;
    PB(cerberus_s3_ble_scanner_passive_get_watch_status(epoch,&watch)==ESP_OK && watch.stored_count);
    locks=mutex_takes;
    PB(cerberus_s3_ble_scanner_passive_get_watch_detailed_result(epoch,0,&watch_row,&timing,&motion,&motion_timing)==ESP_OK &&
        mutex_takes==locks+1 && timing.valid && snapshot->watch.capture_epoch==epoch &&
        timing.request_started_us==clock_base_us+(uint64_t)tick*1000U);
    PB(cerberus_s3_ble_scanner_passive_get_watch_result(epoch,0,&watch_row)==ESP_OK);
    PB(cerberus_s3_ble_scanner_passive_get_activity(epoch,&activity)==ESP_OK);
    PB(allocations==allocated && mac_radio_starts==started);
    PB(cerberus_s3_ble_scanner_start(&generic)==ESP_ERR_INVALID_STATE);
    PB(cerberus_s3_ble_scanner_track_begin(target,1)==ESP_ERR_INVALID_STATE);
    PB(cerberus_s3_ble_scanner_analyzer_begin()==ESP_ERR_INVALID_STATE);
    PB(cerberus_s3_ble_scanner_start_discovery(CERBERUS_S3_BLE_FILTER_ALL,0,&discovery)==ESP_ERR_INVALID_STATE);
    cerberus_s3_ble_mac_monitor_t mac={0};cerberus_mac_monitor_ops_t ops;cerberus_mac_binding_t binding;
    cerberus_s3_ble_scanner_mac_ops(&mac,&ops);
    PB(ops.reserve(ops.context,CERBERUS_MAC_BLE,&binding)==ESP_ERR_INVALID_STATE && !mac_monitor);

    /* Memory pressure, wrong-token rearm and failed host CAS preserve the
     * completed bank and its old token. Invalid raw/time never partly copies. */
    available_internal=32767;
    PB(cerberus_s3_ble_scanner_passive_start(epoch,&state)==ESP_ERR_NO_MEM && state.epoch==epoch && !state.owned);
    available_internal=128U*1024U;
    PB(cerberus_s3_ble_scanner_passive_start(epoch+1,&state)==ESP_ERR_NOT_FOUND && all_zero(&state,sizeof(state)));
    claim_before=passive_claim_foreign;
    PB(cerberus_s3_ble_scanner_passive_start(epoch,&state)==ESP_ERR_INVALID_STATE && state.epoch==epoch && !state.owned);
    claim_before=NULL;owner=CERBERUS_S3_BLE_OWNER_NONE;
    PB(cerberus_s3_ble_scanner_passive_copy_row(epoch,0,&row,&state)==ESP_OK && !memcmp(&row,&frozen,sizeof(row)));
    fail_discovery_lock=true;memset(&row,0xa5,sizeof(row));memset(&state,0xa5,sizeof(state));
    PB(cerberus_s3_ble_scanner_passive_copy_row(epoch,0,&row,&state)==ESP_ERR_TIMEOUT &&
        all_zero(&row,sizeof(row)) && all_zero(&state,sizeof(state)));
    snapshot->records[0].name_length=33;
    PB(cerberus_s3_ble_scanner_passive_copy_row(epoch,0,&row,&state)==ESP_ERR_INVALID_STATE && all_zero(&row,sizeof(row)));
    snapshot->records[0].name_length=0;
    uint64_t prior_clock=clock_base_us;
    clock_base_us=0;passive_assert_copy_failure(epoch,ESP_ERR_INVALID_STATE);
    clock_base_us=prior_clock;
    PB(cerberus_s3_ble_scanner_passive_release(epoch)==ESP_OK);

    /* Successful rearm changes full token even when generation8 aliases. */
    passive_test_idle();PB(cerberus_s3_ble_scanner_passive_start(0,&state)==ESP_OK);epoch=state.epoch;
    pump();emit_hc_report(ordinary,sizeof(ordinary),-50,false,1,3);analyzer_test_complete();
    const uint8_t generation=state.scan.generation;void *old_arg=radio_arg;
    for(unsigned n=0;n<256;++n) {
        PB(head==tail);head=tail=0;
        uint64_t previous=state.epoch;
        PB(cerberus_s3_ble_scanner_passive_start(previous,&state)==ESP_OK && state.epoch>previous);
        pump();
        if(n==255) {
            struct ble_gap_event stale={.type=BLE_GAP_EVENT_DISC_COMPLETE};gap_event(&stale,old_arg);
            PB(cerberus_s3_ble_scanner_passive_get_state(state.epoch,&copy_state)==ESP_OK && copy_state.receiving);
            passive_assert_copy_failure(epoch,ESP_ERR_NOT_FOUND);
        }
        analyzer_test_complete();
    }
    PB(state.scan.generation==generation && cerberus_s3_ble_scanner_passive_release(state.epoch)==ESP_OK);

    /* Failed SDK start/cancel and failed release preserve exact cleanup. */
    passive_test_idle();mac_start_error=71;
    PB(cerberus_s3_ble_scanner_passive_start(0,&state)==ESP_OK);epoch=state.epoch;pump();
    PB(cerberus_s3_ble_scanner_passive_get_state(epoch,&state)==ESP_OK && state.error==71 && !state.owned);
    passive_assert_copy_failure(epoch,ESP_ERR_INVALID_STATE);
    PB(cerberus_s3_ble_scanner_passive_release(epoch)==ESP_OK);mac_start_error=0;
    passive_test_idle();PB(cerberus_s3_ble_scanner_passive_start(0,&state)==ESP_OK);epoch=state.epoch;pump();
    mac_cancel_error=72;
    PB(cerberus_s3_ble_scanner_passive_stop(epoch,&state)==ESP_OK);pump();
    PB(cerberus_s3_ble_scanner_passive_get_state(epoch,&state)==ESP_OK && state.owned && state.error==72 && state.stopping);
    PB(cerberus_s3_ble_scanner_passive_release(epoch)==ESP_ERR_INVALID_STATE);
    mac_cancel_error=0;fail_host_release=true;
    PB(cerberus_s3_ble_scanner_passive_stop(epoch,&state)==ESP_OK);pump();
    PB(cerberus_s3_ble_scanner_passive_get_state(epoch,&state)==ESP_OK && state.owned && !state.receiving && state.error==ESP_ERR_TIMEOUT);
    PB(cerberus_s3_ble_scanner_passive_release(epoch)==ESP_ERR_INVALID_STATE);
    fail_host_release=false;
    PB(cerberus_s3_ble_scanner_passive_stop(epoch,&state)==ESP_OK && !state.owned);
    PB(cerberus_s3_ble_scanner_passive_release(epoch)==ESP_OK);

    /* Post-claim failure with unreleased host must not be reported unowned. */
    passive_test_idle();claim_after=passive_claim_wifi;fail_host_release=true;started=mac_radio_starts;
    PB(cerberus_s3_ble_scanner_passive_start(0,&state)==ESP_ERR_INVALID_STATE && state.attached && state.owned &&
        state.epoch && state.error==ESP_ERR_TIMEOUT && !state.scan_epoch && !state.receiving && head==tail);
    epoch=state.epoch;claim_after=NULL;simulated_wifi_owner=CERBERUS_S3_WIFI_NONE;
    PB(cerberus_s3_ble_scanner_passive_release(epoch)==ESP_ERR_INVALID_STATE);
    passive_assert_copy_failure(epoch,ESP_ERR_INVALID_STATE);
    fail_host_release=false;
    PB(cerberus_s3_ble_scanner_passive_stop(epoch,&state)==ESP_OK && !state.owned);
    PB(cerberus_s3_ble_scanner_passive_release(epoch)==ESP_OK && mac_radio_starts==started);
    passive_test_idle();claim_after=passive_claim_cancel;
    PB(cerberus_s3_ble_scanner_passive_start(0,&state)==ESP_ERR_INVALID_STATE && all_zero(&state,sizeof(state)) && !owner && head==tail);
    claim_after=NULL;

    /* The same post-claim failure on rearm must retain the OLD exact token,
     * not report an accepted new sweep or expose old rows under cleanup. */
    passive_test_idle();PB(cerberus_s3_ble_scanner_passive_start(0,&state)==ESP_OK);epoch=state.epoch;
    pump();emit_hc_report(ordinary,sizeof(ordinary),-51,false,1,3);analyzer_test_complete();
    PB(cerberus_s3_ble_scanner_passive_copy_row(epoch,0,&frozen,&copy_state)==ESP_OK);
    claim_after=passive_claim_wifi;
    PB(cerberus_s3_ble_scanner_passive_start(epoch,&state)==ESP_ERR_INVALID_STATE && state.attached &&
        state.epoch==epoch && !state.owned && !state.error && state.scan_epoch==copy_state.scan_epoch);
    claim_after=NULL;simulated_wifi_owner=CERBERUS_S3_WIFI_NONE;
    PB(cerberus_s3_ble_scanner_passive_copy_row(epoch,0,&row,&state)==ESP_OK && !memcmp(&row,&frozen,sizeof(row)));
    claim_after=passive_claim_wifi;fail_host_release=true;
    PB(cerberus_s3_ble_scanner_passive_start(epoch,&state)==ESP_ERR_INVALID_STATE && state.attached &&
        state.epoch==epoch && state.owned && state.error==ESP_ERR_TIMEOUT && !state.receiving);
    claim_after=NULL;simulated_wifi_owner=CERBERUS_S3_WIFI_NONE;
    passive_assert_copy_failure(epoch,ESP_ERR_INVALID_STATE);
    fail_host_release=false;
    PB(cerberus_s3_ble_scanner_passive_stop(epoch,&state)==ESP_OK && !state.owned);
    PB(cerberus_s3_ble_scanner_passive_release(epoch)==ESP_OK);

    passive_test_idle();
    uint64_t saved_watch_epoch=watch_epoch;uint32_t saved_scan_epoch=scan_epoch;
    for(unsigned exhaustion=0;exhaustion<2;++exhaustion) {
        watch_epoch=exhaustion?saved_watch_epoch:UINT64_MAX;
        scan_epoch=exhaustion?UINT32_MAX:saved_scan_epoch;
        PB(cerberus_s3_ble_scanner_passive_start(0,&state)==ESP_ERR_INVALID_STATE &&
            all_zero(&state,sizeof(state)) && !owner && head==tail);
    }
    watch_epoch=saved_watch_epoch;scan_epoch=saved_scan_epoch;
    PB(cerberus_s3_ble_scanner_passive_start(0,&state)==ESP_OK);epoch=state.epoch;pump();
    emit_hc_report(ordinary,sizeof(ordinary),-55,false,1,3);analyzer_test_complete();
    clock_base_us=(UINT64_C(1)<<32)*1000U;
    passive_assert_copy_failure(epoch,ESP_ERR_INVALID_STATE); /* One full source-clock revolution is ambiguous. */
    PB(cerberus_s3_ble_scanner_passive_release(epoch)==ESP_OK);

    /* Reset/replacement retires only our lease. Identical public generation
     * cannot authorize reads or Stop against a foreign scanner. */
    for(unsigned reset=0;reset<2;++reset) {
        passive_test_idle();PB(cerberus_s3_ble_scanner_passive_start(0,&state)==ESP_OK);epoch=state.epoch;pump();
        emit_hc_report(ordinary,sizeof(ordinary),-50,false,1,3);
        cancelled=mac_radio_cancels;released=host_releases;
        if(reset) ++host_epoch; else ++scan_epoch;
        PB(cerberus_s3_ble_scanner_passive_get_state(epoch,&state)==ESP_OK && state.attached &&
            !state.owned && !state.receiving && state.error==ESP_ERR_INVALID_STATE);
        passive_assert_copy_failure(epoch,ESP_ERR_INVALID_STATE);
        PB(cerberus_s3_ble_scanner_passive_stop(epoch,&state)==ESP_OK && !state.owned &&
            mac_radio_cancels==cancelled && host_releases==released && owner==CERBERUS_S3_BLE_OWNER_SCANNER);
        PB(cerberus_s3_ble_scanner_passive_release(epoch)==ESP_OK);
        owner=CERBERUS_S3_BLE_OWNER_NONE;radio_active=false;
        snapshot->status.state=CERBERUS_BLE_SCAN_COMPLETE;
    }
    passive_test_idle();
    printf("Native passive binding: %u checks; state %zu bytes; rejected ownership, exact completed row/Watch clocks, rearm, reset and retained cleanup passed.\n",
        passive_binding_checks,sizeof(state));
}
#undef PB
