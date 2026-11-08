/* Production scanner with a full-token host boundary. Same owner and even the
 * same host epoch are intentionally reused by the replacement. */
static unsigned host_binding_checks;
#define HB(x) do { ++host_binding_checks; if(!(x)){fprintf(stderr,"host binding line %u: %s\n",__LINE__,#x);abort();} } while(0)
static void host_replace_scanner(void)
{
    current_host_lease=(cerberus_s3_ble_host_lease_t){++next_host_token,host_epoch,CERBERUS_S3_BLE_OWNER_SCANNER};
    owner=CERBERUS_S3_BLE_OWNER_SCANNER;
}
static void host_claim_reentry(void)
{
    cerberus_ble_scan_status_payload_t status;
    cerberus_s3_ble_passive_state_t passive;
    cerberus_s3_ble_mac_monitor_t m={0};cerberus_mac_monitor_ops_t ops;cerberus_mac_binding_t b;
    HB(scan_host_acquiring && !held);
    HB(cerberus_s3_ble_scanner_start(&status)==ESP_ERR_INVALID_STATE);
    HB(cerberus_s3_ble_scanner_passive_start(0,&passive)==ESP_ERR_INVALID_STATE);
    HB(cerberus_s3_ble_scanner_analyzer_begin()==ESP_ERR_INVALID_STATE);
    HB(cerberus_s3_ble_scanner_track_begin((uint8_t[6]){1},0)==ESP_ERR_INVALID_STATE);
    cerberus_s3_ble_scanner_mac_ops(&m,&ops);
    HB(ops.reserve(&m,CERBERUS_MAC_BLE,&b)==ESP_ERR_INVALID_STATE);
}
static void check_host_binding(void)
{
    cerberus_ble_scan_status_payload_t status;
    const uint8_t target[6]={1,2,3,4,5,6};
    struct ble_gap_event complete={.type=BLE_GAP_EVENT_DISC_COMPLETE};
    for(unsigned mode=0;mode<6;++mode)for(unsigned reset=0;reset<2;++reset)
    for(unsigned phase=0;phase<3;++phase){
        passive_test_idle();expected_active=mode==2;
        cerberus_s3_ble_passive_state_t passive;
        cerberus_s3_ble_discovery_meta_t discovery;
        cerberus_s3_ble_track_state_t track;
        cerberus_s3_ble_analyzer_state_t analyze;
        cerberus_s3_ble_mac_monitor_t m={0};cerberus_mac_monitor_ops_t ops;
        cerberus_mac_binding_t b={0};cerberus_mac_monitor_observation_t o;
        if(mode==0)HB(cerberus_s3_ble_scanner_start(&status)==ESP_OK);
        if(mode==1)HB(cerberus_s3_ble_scanner_passive_start(0,&passive)==ESP_OK);
        if(mode==2)HB(cerberus_s3_ble_scanner_start_discovery(CERBERUS_S3_BLE_FILTER_ALL,0,&discovery)==ESP_OK);
        if(mode==3){HB(cerberus_s3_ble_scanner_track_begin(target,1)==ESP_OK);HB(cerberus_s3_ble_scanner_track_start(&track)==ESP_OK);}
        if(mode==4){HB(cerberus_s3_ble_scanner_analyzer_begin()==ESP_OK);HB(cerberus_s3_ble_scanner_analyzer_start(&analyze)==ESP_OK);}
        if(mode==5){cerberus_s3_ble_scanner_mac_ops(&m,&ops);HB(!ops.reserve(&m,1,&b));
            cerberus_mac_start_request_t r=mac_request(b);HB(!ops.start(&m,1,&r,&o).error);b.epoch=o.status.epoch;}
        HB(scan_host_held && scan_host_lease.token && cerberus_s3_ble_host_lease_valid(&scan_host_lease));
        if(phase)pump();
        int(*callback)(struct ble_gap_event*,void*)=radio_callback;void *arg=radio_arg;
        if(phase==2){fail_host_release=true;analyzer_test_complete();fail_host_release=false;HB(scan_host_held);}
        if(reset)++host_epoch;
        host_replace_scanner();cerberus_s3_ble_host_lease_t replacement=current_host_lease;
        radio_active=true; /* Another client's SDK procedure, not our RF. */
        unsigned releases=host_releases,cancels=mac_radio_cancels,starts=mac_radio_starts;
        HB(cerberus_s3_ble_scanner_get_status(&status)==ESP_OK);
        HB(status.state==CERBERUS_BLE_SCAN_ERROR && status.error_code==ESP_ERR_INVALID_STATE);
        if(phase){callback(&complete,arg);mac_emit(0,false,false,-20,NULL,0,1);}
        while(head<tail)pump();
        if(mode==0)HB(cerberus_s3_ble_scanner_stop(&status)==ESP_ERR_INVALID_STATE);
        if(mode==1){HB(cerberus_s3_ble_scanner_passive_stop(passive.epoch,&passive)==ESP_OK && !passive.owned && passive.error);
            HB(cerberus_s3_ble_scanner_passive_release(passive.epoch)==ESP_OK);}
        if(mode==2){HB(cerberus_s3_ble_scanner_stop_discovery(discovery.scan_epoch,&discovery)==ESP_OK && !discovery.owned);
            HB(cerberus_s3_ble_scanner_release_discovery(discovery.scan_epoch)==ESP_OK);}
        if(mode==3){HB(cerberus_s3_ble_scanner_track_stop(&track)==ESP_OK && !track.owned && track.error);
            HB(cerberus_s3_ble_scanner_track_release()==ESP_OK);}
        if(mode==4){HB(cerberus_s3_ble_scanner_analyzer_stop(&analyze)==ESP_OK && !analyze.owned && analyze.error);
            HB(cerberus_s3_ble_scanner_analyzer_release()==ESP_OK);}
        if(mode==5){HB(!ops.stop(&m,1,&b,&o).error && !(o.status.flags&CERBERUS_MAC_OWNED));HB(!mac_monitor);}
        HB(cerberus_s3_ble_host_lease_valid(&replacement) && radio_active && host_releases==releases &&
            mac_radio_cancels==cancels && mac_radio_starts==starts);
        HB(!scan_host_held && !scan_host_acquiring);
        HB(cerberus_s3_ble_host_release(&replacement)==ESP_OK);radio_active=false;
    }
    /* A replacement at the final release itself must survive that exact CAS. */
    passive_test_idle();HB(cerberus_s3_ble_scanner_start(&status)==ESP_OK);pump();
    release_before=host_replace_scanner;analyzer_test_complete();release_before=NULL;
    cerberus_s3_ble_host_lease_t replacement=current_host_lease;
    HB(cerberus_s3_ble_scanner_get_status(&status)==ESP_OK && status.state==CERBERUS_BLE_SCAN_ERROR);
    HB(cerberus_s3_ble_host_lease_valid(&replacement));
    HB(cerberus_s3_ble_host_release(&replacement)==ESP_OK);
    /* Successful release is different: historical completed data stays valid. */
    passive_test_idle();HB(cerberus_s3_ble_scanner_start(&status)==ESP_OK);pump();analyzer_test_complete();
    host_replace_scanner();replacement=current_host_lease;unsigned releases=host_releases;
    HB(cerberus_s3_ble_scanner_get_status(&status)==ESP_OK && status.state==CERBERUS_BLE_SCAN_COMPLETE);
    HB(cerberus_s3_ble_scanner_stop(&status)==ESP_ERR_INVALID_STATE && host_releases==releases);
    HB(cerberus_s3_ble_host_release(&replacement)==ESP_OK);
    /* Yielding admission reserves the scanner before any host claim. */
    passive_test_idle();claim_before=host_claim_reentry;
    HB(cerberus_s3_ble_scanner_start(&status)==ESP_OK);claim_before=NULL;pump();analyzer_test_complete();
    /* A claim lost while initialization yielded returns no accepted scanner. */
    passive_test_idle();claim_after=host_replace_scanner;releases=host_releases;
    HB(cerberus_s3_ble_scanner_start(&status)==ESP_ERR_INVALID_STATE);
    claim_after=NULL;replacement=current_host_lease;
    HB(!scan_host_held && !scan_host_acquiring && host_releases==releases && cerberus_s3_ble_host_lease_valid(&replacement));
    HB(cerberus_s3_ble_host_release(&replacement)==ESP_OK);
    /* Address inference may yield to reset/replacement before SDK Start. */
    passive_test_idle();HB(cerberus_s3_ble_scanner_start(&status)==ESP_OK);
    unsigned starts=mac_radio_starts;
    infer_before=host_replace_scanner;pump();infer_before=NULL;
    replacement=current_host_lease;
    HB(cerberus_s3_ble_scanner_get_status(&status)==ESP_OK && status.state==CERBERUS_BLE_SCAN_ERROR);
    HB(mac_radio_starts==starts && cerberus_s3_ble_host_lease_valid(&replacement));
    HB(cerberus_s3_ble_host_release(&replacement)==ESP_OK);
    /* Defensive retained cleanup cannot be overwritten by another start. */
    passive_test_idle();claim_after=passive_claim_wifi;fail_host_release=true;
    HB(cerberus_s3_ble_scanner_start(&status)==ESP_ERR_INVALID_STATE);
    claim_after=NULL;simulated_wifi_owner=CERBERUS_S3_WIFI_NONE;
    HB(scan_host_cleanup && scan_host_held);
    uint64_t token=scan_host_lease.token;
    HB(cerberus_s3_ble_scanner_start(&status)==ESP_ERR_INVALID_STATE && scan_host_lease.token==token);
    fail_host_release=false;HB(cerberus_s3_ble_scanner_stop(&status)==ESP_OK && !scan_host_held);
    passive_test_idle();
    printf("Native scanner host tokens: %u checks; six clients, same-owner/epoch replacement, queued/final callbacks, admission and historical completion pass.\n",host_binding_checks);
}
#undef HB
