/* Scoped whole-bank Wardrive reads exercise real accepted passive sweeps. */
static unsigned wardrive_binding_checks;
#define WB(x) do { ++wardrive_binding_checks; assert(x); } while (0)

static void check_wardrive_binding(void)
{
    cerberus_c5_ble_result_t rows[33], expected_rows[32];
    cerberus_s3_ble_wardrive_evidence_t evidence[33],expected_evidence[32];
    cerberus_s3_ble_passive_state_t state, copied;
    cerberus_ble_scan_status_payload_t generic;
    const uint8_t fs[]={15,9,'F','S',' ','E','x','t',' ','B','a','t','t','e','r','y'};
    const uint8_t mfg[]={4,0xff,0xc8,9,1},ordinary[]={2,1,6},malformed[]={8,9,'x'};
    passive_test_idle();
    unsigned allocated=allocations,started=mac_radio_starts,cancelled=mac_radio_cancels;
    memset(rows,0xa5,sizeof(rows));memset(evidence,0xa5,sizeof(evidence));memset(&copied,0xa5,sizeof(copied));
    WB(cerberus_s3_ble_scanner_passive_copy_wardrive(0,0,rows,evidence,32,&copied)==ESP_ERR_INVALID_ARG);
    WB(all_zero(rows,32*sizeof(*rows)) && all_zero(evidence,32*sizeof(*evidence)) && all_zero(&copied,sizeof(copied)));
    WB(((uint8_t *)&rows[32])[0]==0xa5 && ((uint8_t *)&evidence[32])[0]==0xa5);
    WB(cerberus_s3_ble_scanner_passive_copy_wardrive(1,0,NULL,NULL,0,&copied)==ESP_ERR_NOT_FOUND);
    WB(allocations==allocated && mac_radio_starts==started && mac_radio_cancels==cancelled);
    WB(cerberus_s3_ble_scanner_start(&generic)==ESP_OK);pump();
    emit_hc_report(fs,sizeof(fs),-45,false,1,3);analyzer_test_complete();
    WB(cerberus_s3_ble_scanner_passive_copy_wardrive(watch_epoch,1,rows,evidence,32,&copied)==ESP_ERR_NOT_FOUND &&
        all_zero(rows,32*sizeof(*rows)) && all_zero(&copied,sizeof(copied)));

    /* One full typed bank; original and synthetic matched-response facts stay
     * separate across source32 wrap. The accepted SDK sweep remains passive. */
    passive_test_idle();tick=0;clock_base_us=((UINT64_C(1)<<32)-2U)*1000U;
    WB(cerberus_s3_ble_scanner_passive_start(0,&state)==ESP_OK);uint64_t epoch=state.epoch;
    WB(cerberus_s3_ble_scanner_passive_copy_wardrive(epoch,0,NULL,NULL,0,&copied)==ESP_ERR_INVALID_STATE);
    pump();
    cerberus_s3_ble_report_t report={.address={1,2,3,4,5,6},.address_type=0,.rssi=-50,
        .complete=true,.sid=3,.sid_valid=true,.data=fs,.length=sizeof(fs)};
    emit_wardrive_report(&report);tick=3;report.scan_response=true;report.data=mfg;report.length=sizeof(mfg);
    emit_wardrive_report(&report);
    report.scan_response=false;report.address_type=1;report.data=fs;report.length=sizeof(fs);++tick;
    emit_wardrive_report(&report); /* Same address bytes, distinct typed identity. */
    for(unsigned i=2;i<32;++i) {
        report.address[0]=(uint8_t)(i+1U);report.address_type=1;report.data=ordinary;report.length=sizeof(ordinary);
        ++tick;emit_wardrive_report(&report);
    }
    WB(snapshot->status.stored_count==32 && !active_scan);
    WB(cerberus_s3_ble_scanner_passive_copy_wardrive(epoch,32,rows,evidence,32,&copied)==ESP_ERR_INVALID_STATE);
    analyzer_test_complete();
    allocated=allocations;started=mac_radio_starts;unsigned locks=mutex_takes;
    WB(cerberus_s3_ble_scanner_passive_copy_wardrive(epoch,32,rows,evidence,32,&copied)==ESP_OK && mutex_takes==locks+1);
    WB(copied.epoch==epoch && copied.scan_epoch==state.scan_epoch && copied.host_epoch==state.host_epoch &&
        copied.attached && !copied.owned && !copied.receiving && !copied.stopping && !copied.error &&
        copied.scan.state==CERBERUS_BLE_SCAN_COMPLETE && copied.scan.stored_count==32 &&
        copied.started_ms==(UINT64_C(1)<<32)-2U && copied.source_now_ms==(UINT64_C(1)<<32)-2U+tick);
    WB(rows[0].address_type==0 && rows[1].address_type==1 && !memcmp(rows[0].address,rows[1].address,6));
    WB(evidence[0].advertisement_observed_ms==UINT32_MAX-1U && evidence[0].scan_response_observed_ms==1U &&
        evidence[0].advertisement.facts==CERBERUS_BLE_FLOCK_FS_BATTERY &&
        evidence[0].scan_response.facts==(CERBERUS_BLE_FLOCK_NO_LOCAL_NAME|CERBERUS_BLE_FLOCK_MFG_09C8|CERBERUS_BLE_FLOCK_MFG_PAYLOAD));
    WB(cerberus_s3_ble_wardrive_evidence_level(&evidence[0])==CERBERUS_BLE_FLOCK_LEVEL_SIGNATURE);
    WB(cerberus_s3_ble_scanner_copy_wardrive(state.scan.generation,32,expected_rows,expected_evidence,32)==ESP_OK &&
        !memcmp(rows,expected_rows,sizeof(expected_rows)) && !memcmp(evidence,expected_evidence,sizeof(expected_evidence)));
    WB(allocations==allocated && mac_radio_starts==started && head==tail);
    for(unsigned i=0;i<32;++i) WB(cerberus_s3_ble_wardrive_evidence_valid(&evidence[i]));

    /* A late corrupt row must leave even the early valid rows uncommitted. */
    cerberus_s3_ble_record_t saved=snapshot->records[31];
    for(unsigned corrupt=0;corrupt<7;++corrupt) {
        snapshot->records[31]=saved;
        if(corrupt==0)snapshot->records[31].name_length=33;
        if(corrupt==1)snapshot->records[31].uuid_length=3;
        if(corrupt==2)snapshot->records[31].advertisement_length=32;
        if(corrupt==3)++snapshot->records[31].focus.discovery.generation;
        if(corrupt==4)++snapshot->records[31].focus.discovery.address_type;
        if(corrupt==5)++snapshot->records[31].focus.discovery.address[0];
        if(corrupt==6)snapshot->records[31].last_advertisement_ms=(uint32_t)(clock_base_us/1000U+tick+1U);
        memset(rows,0xa5,sizeof(rows));memset(evidence,0xa5,sizeof(evidence));memset(&copied,0xa5,sizeof(copied));
        WB(cerberus_s3_ble_scanner_passive_copy_wardrive(epoch,32,rows,evidence,32,&copied)==ESP_ERR_INVALID_STATE &&
            all_zero(rows,32*sizeof(*rows)) && all_zero(evidence,32*sizeof(*evidence)) && all_zero(&copied,sizeof(copied)));
        WB(((uint8_t *)&rows[32])[0]==0xa5 && ((uint8_t *)&evidence[32])[0]==0xa5);
    }
    snapshot->records[31]=saved;
    for(unsigned invalid=0;invalid<6;++invalid) {
        memset(rows,0xa5,sizeof(rows));memset(evidence,0xa5,sizeof(evidence));memset(&copied,0xa5,sizeof(copied));
        WB(cerberus_s3_ble_scanner_passive_copy_wardrive(epoch,invalid==0?31:32,
            invalid==1?NULL:rows,invalid==2?NULL:evidence,invalid==3?33:invalid==4?31:32,
            invalid==5?NULL:&copied)==(invalid==0?ESP_ERR_INVALID_STATE:ESP_ERR_INVALID_ARG));
        size_t cleared=invalid==4?31U:32U;
        WB((invalid==1 || all_zero(rows,cleared*sizeof(*rows))) &&
            (invalid==2 || all_zero(evidence,cleared*sizeof(*evidence))) &&
            (invalid==5 || all_zero(&copied,sizeof(copied))));
        WB(((uint8_t *)&rows[32])[0]==0xa5 && ((uint8_t *)&evidence[32])[0]==0xa5);
    }
    fail_discovery_lock=true;
    WB(cerberus_s3_ble_scanner_passive_copy_wardrive(epoch,32,rows,evidence,32,&copied)==ESP_ERR_TIMEOUT &&
        all_zero(rows,32*sizeof(*rows)) && all_zero(evidence,32*sizeof(*evidence)) && all_zero(&copied,sizeof(copied)));
    WB(cerberus_s3_ble_scanner_passive_start(0,&copied)==ESP_ERR_INVALID_STATE && all_zero(&copied,sizeof(copied)));
    WB(cerberus_s3_ble_scanner_passive_release(epoch)==ESP_OK);
    WB(cerberus_s3_ble_scanner_passive_copy_wardrive(epoch,32,rows,evidence,32,&copied)==ESP_ERR_NOT_FOUND &&
        all_zero(rows,32*sizeof(*rows)) && all_zero(&copied,sizeof(copied)));

    /* Malformed current packets mean unavailable packet hints, not fabricated
     * pairing with earlier facts. Empty completed captures are also valid. */
    passive_test_idle();WB(cerberus_s3_ble_scanner_passive_start(0,&state)==ESP_OK);epoch=state.epoch;pump();
    emit_hc_report(fs,sizeof(fs),-50,false,1,3);emit_hc_report(malformed,sizeof(malformed),-45,false,1,3);
    analyzer_test_complete();
    WB(cerberus_s3_ble_scanner_passive_copy_wardrive(epoch,1,rows,evidence,32,&copied)==ESP_OK &&
        all_zero(&evidence[0],sizeof(evidence[0])) && all_zero(rows+1,31*sizeof(*rows)) &&
        all_zero(evidence+1,31*sizeof(*evidence)));
    WB(cerberus_s3_ble_scanner_passive_start(epoch,&state)==ESP_OK);epoch=state.epoch;pump();analyzer_test_complete();
    WB(cerberus_s3_ble_scanner_passive_copy_wardrive(epoch,0,NULL,NULL,0,&copied)==ESP_OK &&
        copied.attached && copied.epoch==epoch && !copied.scan.stored_count);
    WB(cerberus_s3_ble_scanner_passive_release(epoch)==ESP_OK);

    /* Public generation reuse cannot substitute for the old full capture. */
    passive_test_idle();WB(cerberus_s3_ble_scanner_passive_start(0,&state)==ESP_OK);epoch=state.epoch;
    pump();emit_hc_report(fs,sizeof(fs),-50,false,1,3);analyzer_test_complete();
    const uint8_t generation=state.scan.generation;
    for(unsigned i=0;i<256;++i) {
        WB(head==tail);head=tail=0;
        WB(cerberus_s3_ble_scanner_passive_start(state.epoch,&state)==ESP_OK);
        pump();analyzer_test_complete();
    }
    WB(state.scan.generation==generation && state.epoch!=epoch);
    WB(cerberus_s3_ble_scanner_passive_copy_wardrive(epoch,0,NULL,NULL,0,&copied)==ESP_ERR_NOT_FOUND && all_zero(&copied,sizeof(copied)));
    WB(cerberus_s3_ble_scanner_passive_copy_wardrive(state.epoch,0,NULL,NULL,0,&copied)==ESP_OK);
    WB(cerberus_s3_ble_scanner_passive_release(state.epoch)==ESP_OK);

    /* Queued cancellation and failed RF release cannot expose a COMPLETE bank. */
    passive_test_idle();WB(cerberus_s3_ble_scanner_passive_start(0,&state)==ESP_OK);epoch=state.epoch;
    WB(cerberus_s3_ble_scanner_passive_stop(epoch,&state)==ESP_OK);
    WB(cerberus_s3_ble_scanner_passive_copy_wardrive(epoch,0,NULL,NULL,0,&copied)==ESP_ERR_INVALID_STATE);
    pump();pump();
    WB(cerberus_s3_ble_scanner_passive_copy_wardrive(epoch,0,NULL,NULL,0,&copied)==ESP_ERR_INVALID_STATE);
    WB(cerberus_s3_ble_scanner_passive_release(epoch)==ESP_OK);
    passive_test_idle();WB(cerberus_s3_ble_scanner_passive_start(0,&state)==ESP_OK);epoch=state.epoch;pump();
    emit_hc_report(fs,sizeof(fs),-50,false,1,3);fail_host_release=true;analyzer_test_complete();
    WB(cerberus_s3_ble_scanner_passive_get_state(epoch,&state)==ESP_OK && state.owned &&
        state.scan.state==CERBERUS_BLE_SCAN_COMPLETE && state.error==ESP_ERR_TIMEOUT);
    WB(cerberus_s3_ble_scanner_passive_copy_wardrive(epoch,1,rows,evidence,32,&copied)==ESP_ERR_INVALID_STATE &&
        all_zero(rows,32*sizeof(*rows)) && all_zero(evidence,32*sizeof(*evidence)) && all_zero(&copied,sizeof(copied)));
    fail_host_release=false;
    WB(cerberus_s3_ble_scanner_passive_stop(epoch,&state)==ESP_OK && !state.owned);
    WB(cerberus_s3_ble_scanner_passive_release(epoch)==ESP_OK);

    for(unsigned reset=0;reset<2;++reset) {
        passive_test_idle();WB(cerberus_s3_ble_scanner_passive_start(0,&state)==ESP_OK);epoch=state.epoch;pump();
        emit_hc_report(fs,sizeof(fs),-50,false,1,3);analyzer_test_complete();
        if(reset)++host_epoch;else++scan_epoch;
        owner=CERBERUS_S3_BLE_OWNER_SCANNER;cancelled=mac_radio_cancels;
        WB(cerberus_s3_ble_scanner_passive_copy_wardrive(epoch,1,rows,evidence,32,&copied)==ESP_ERR_INVALID_STATE &&
            all_zero(rows,32*sizeof(*rows)) && all_zero(evidence,32*sizeof(*evidence)) && all_zero(&copied,sizeof(copied)));
        WB(cerberus_s3_ble_scanner_passive_stop(epoch,&state)==ESP_OK && !state.owned &&
            owner==CERBERUS_S3_BLE_OWNER_SCANNER && mac_radio_cancels==cancelled);
        WB(cerberus_s3_ble_scanner_passive_release(epoch)==ESP_OK);
        owner=CERBERUS_S3_BLE_OWNER_NONE;
    }
    passive_test_idle();
    printf("Scoped Wardrive bank: %u checks; one-lock complete/released token, typed facts, source wrap, atomic failure and generation-alias rejection passed.\n",wardrive_binding_checks);
}
#undef WB
