/* Actual native discovery attachment, SDK event queue and snapshot collector. */
static void reacquire_report(uint8_t type, uint8_t identity, bool response)
{
    const uint8_t payload[] = {3,3,0x44,0xfd};
    cerberus_s3_ble_report_t report = {.address_type=type,.rssi=-47,
        .complete=true,.data=payload,.length=sizeof(payload),.sid=3U,
        .scan_response=response};
    memcpy(report.address,(uint8_t[]){identity,2,3,4,5,6},6U);
    ++tick;
    emit_wardrive_report(&report);
}

static void check_reacquire_native(void)
{
    const uint8_t target[6] = {90,2,3,4,5,6}, other[6] = {91,2,3,4,5,6};
    cerberus_s3_ble_discovery_meta_t meta, failed;
    cerberus_s3_ble_discovery_record_t rows[4];
    cerberus_s3_ble_discovery_bank_t bank;
    expected_active=false;clock_base_us=0;tick=200000U;head=tail=0;
    assert(!owner&&!radio_active&&!discovery_attached&&!tracker.enabled);
    assert(cerberus_s3_ble_scanner_start_reacquire(NULL,0,&meta)==ESP_ERR_INVALID_ARG);
    assert(cerberus_s3_ble_scanner_start_reacquire((uint8_t[6]){0},0,&meta)==ESP_ERR_INVALID_ARG);
    assert(cerberus_s3_ble_scanner_start_reacquire((uint8_t[6]){255,255,255,255,255,255},0,&meta)==ESP_ERR_INVALID_ARG);
    assert(all_zero(&meta,sizeof(meta)));
    unsigned allocs=allocations;
    assert(cerberus_s3_ble_scanner_start_reacquire(target,0,&meta)==ESP_OK);
    uint64_t epoch=meta.scan_epoch;
    assert(meta.reacquiring&&meta.family==CERBERUS_S3_BLE_FILTER_ALL&&meta.owned&&
        !memcmp(meta.target_address,target,6)&&!discovery_bindings&&allocations==allocs);
    assert(cerberus_s3_ble_scanner_start_discovery(CERBERUS_S3_BLE_FILTER_ALL,epoch,&failed)==ESP_ERR_INVALID_STATE);
    assert(all_zero(&failed,sizeof(failed)));
    assert(cerberus_s3_ble_scanner_track_begin(target,0)==ESP_ERR_INVALID_STATE&&!tracker.enabled);
    pump();assert(radio_active&&!active_scan);
    uint64_t revision=snapshot->revision;
    for(unsigned i=0;i<80U;++i)reacquire_report(1,(uint8_t)i,false);
    reacquire_report(0,90,true);
    reacquire_report(4,90,false);
    reacquire_report(255,90,false);
    assert(snapshot->revision==revision&&!snapshot->status.stored_count&&!snapshot->activity.status.identities);
    assert(cerberus_s3_ble_scanner_copy_reacquire(epoch,rows,&meta)==ESP_OK&&meta.status.stored_count==0);
    assert(all_zero(rows,sizeof(rows)));
    for(uint8_t type=0;type<4U;++type)reacquire_report(type,90,false);
    unsigned locks=mutex_takes;
    assert(cerberus_s3_ble_scanner_copy_reacquire(epoch,rows,&meta)==ESP_OK&&mutex_takes==locks+1U);
    assert(meta.status.stored_count==4U&&meta.status.total_count==4U&&!tracker.enabled&&allocations==allocs);
    for(unsigned i=0;i<4U;++i){
        assert(rows[i].peer.address_type==i&&!memcmp(rows[i].peer.address,target,6));
        assert(rows[i].focus.discovery.advertisement_sequence&&
            rows[i].focus.discovery.raw_advertisement_sequence==rows[i].focus.discovery.advertisement_sequence);
        assert((rows[i].focus.discovery.flags&(CERBERUS_S3_BLE_EVIDENCE_ADV_COMPLETE|
            CERBERUS_S3_BLE_EVIDENCE_ADV_VALID))==(CERBERUS_S3_BLE_EVIDENCE_ADV_COMPLETE|
            CERBERUS_S3_BLE_EVIDENCE_ADV_VALID));
        assert(!(rows[i].focus.discovery.flags&CERBERUS_S3_BLE_EVIDENCE_RESPONSE_VALID));
    }
    assert(cerberus_s3_ble_scanner_copy_discovery(epoch,&bank)==ESP_OK&&bank.meta.reacquiring&&
        !memcmp(bank.records,rows,sizeof(rows)));
    /* Mutex delay increases the copy ceiling, never the source timestamp. */
    uint32_t entered=tick+1U;
    next_lock_delay_us=250000U;
    reacquire_report(0,90,false);
    assert(cerberus_s3_ble_scanner_copy_reacquire(epoch,rows,&meta)==ESP_OK&&
        rows[0].observed_ms==entered&&meta.source_now_ms==(uint64_t)entered+250U);
    uint32_t reports=rows[0].advertisement.reports_seen;
    revision=meta.revision;
    /* A backwards callback clock does not update retained age or hints. */
    clock_base_us=0;tick=entered-2U;
    reacquire_report(0,90,false);
    assert(snapshot->revision==revision&&snapshot->records[0].reports_seen==reports);
    tick=entered+500U;
    /* Invalid callback envelope and an original longer than the callback's
     * supported PHY envelope cannot create a current observation. */
    uint8_t bytes[256]={0};
    struct ble_gap_event event={0};
#if FAKE_EXT_SCAN
    event.type=BLE_GAP_EVENT_EXT_DISC;
    memcpy(event.ext_disc.addr.val,target,6);event.ext_disc.addr.type=0;
    event.ext_disc.rssi=-35;event.ext_disc.sid=3;event.ext_disc.props=BLE_HCI_ADV_CONN_MASK;
    event.ext_disc.data=bytes;event.ext_disc.length_data=3;event.ext_disc.data_status=1;
    radio_callback(&event,radio_arg);
    event.ext_disc.data_status=BLE_GAP_EXT_ADV_DATA_STATUS_COMPLETE;event.ext_disc.sid=0xff;
    radio_callback(&event,radio_arg);
    event.ext_disc.sid=3;event.ext_disc.props=BLE_HCI_ADV_LEGACY_MASK;
    event.ext_disc.legacy_event_type=BLE_HCI_ADV_RPT_EVTYPE_ADV_IND;event.ext_disc.length_data=32;
    radio_callback(&event,radio_arg);
    event.ext_disc.length_data=3;event.ext_disc.legacy_event_type=9;
    radio_callback(&event,radio_arg);
    event.ext_disc.legacy_event_type=BLE_HCI_ADV_RPT_EVTYPE_ADV_IND;event.ext_disc.data=NULL;
    radio_callback(&event,radio_arg);
#else
    event.type=BLE_GAP_EVENT_DISC;
    memcpy(event.disc.addr.val,target,6);event.disc.addr.type=0;event.disc.rssi=-35;
    event.disc.event_type=BLE_HCI_ADV_RPT_EVTYPE_ADV_IND;event.disc.data=bytes;event.disc.length_data=32;
    radio_callback(&event,radio_arg);
    event.disc.length_data=3;event.disc.event_type=9;
    radio_callback(&event,radio_arg);
    event.disc.event_type=BLE_HCI_ADV_RPT_EVTYPE_ADV_IND;event.disc.data=NULL;
    radio_callback(&event,radio_arg);
#endif
    assert(snapshot->revision==revision&&snapshot->records[0].reports_seen==reports);
    /* A complete source with malformed AD bytes dequalifies this typed row;
     * it is a valid publication, not a transport or whole-bank error. */
    const uint8_t malformed[] = {6,9,'x'};
    cerberus_s3_ble_report_t broken = {.address_type=0,.rssi=-40,.complete=true,
        .data=malformed,.length=sizeof(malformed),.sid=3};
    memcpy(broken.address,target,6);++tick;emit_wardrive_report(&broken);
    assert(cerberus_s3_ble_scanner_copy_reacquire(epoch,rows,&meta)==ESP_OK&&meta.status.stored_count==4);
    assert((rows[0].advertisement.flags&CERBERUS_BLE_ADVERTISEMENT_MALFORMED)&&
        !(rows[0].focus.discovery.flags&CERBERUS_S3_BLE_EVIDENCE_ADV_VALID)&&
        rows[0].focus.discovery.raw_advertisement_sequence==rows[0].focus.discovery.advertisement_sequence);
    uint32_t raw_sequence=snapshot->records[0].focus.discovery.raw_advertisement_sequence;
    --snapshot->records[0].focus.discovery.raw_advertisement_sequence;
    assert(cerberus_s3_ble_scanner_copy_reacquire(epoch,rows,&meta)==ESP_ERR_INVALID_STATE&&
        all_zero(rows,sizeof(rows))&&all_zero(&meta,sizeof(meta)));
    snapshot->records[0].focus.discovery.raw_advertisement_sequence=raw_sequence;
    ++snapshot->records[0].focus.discovery.advertisement_observed_ms;
    assert(cerberus_s3_ble_scanner_copy_reacquire(epoch,rows,&meta)==ESP_ERR_INVALID_STATE);
    --snapshot->records[0].focus.discovery.advertisement_observed_ms;
    reacquire_report(0,90,false);
    assert(cerberus_s3_ble_scanner_copy_reacquire(epoch,rows,&meta)==ESP_OK&&
        (rows[0].focus.discovery.flags&CERBERUS_S3_BLE_EVIDENCE_ADV_VALID));
    /* Reject all output on wrong selector/type/provenance, not just one row. */
    snapshot->records[1].address[0]^=1U;
    assert(cerberus_s3_ble_scanner_copy_reacquire(epoch,rows,&meta)==ESP_ERR_INVALID_STATE&&
        all_zero(rows,sizeof(rows))&&all_zero(&meta,sizeof(meta)));
    snapshot->records[1].address[0]^=1U;
    fail_discovery_lock=true;
    assert(cerberus_s3_ble_scanner_copy_reacquire(epoch,rows,&meta)==ESP_ERR_TIMEOUT&&all_zero(rows,sizeof(rows)));
    assert(cerberus_s3_ble_scanner_copy_reacquire(epoch+1U,rows,&meta)==ESP_ERR_NOT_FOUND);
    void *old_arg=radio_arg;
    discovery_native_complete();
    assert(cerberus_s3_ble_scanner_copy_reacquire(epoch,rows,&meta)==ESP_OK&&!meta.owned);
    assert(cerberus_s3_ble_scanner_start_reacquire(other,epoch,&failed)==ESP_ERR_INVALID_STATE&&all_zero(&failed,sizeof(failed)));
    assert(cerberus_s3_ble_scanner_start_discovery(CERBERUS_S3_BLE_FILTER_ALL,epoch,&failed)==ESP_ERR_INVALID_STATE);
    assert(cerberus_s3_ble_scanner_start_reacquire(target,0,&failed)==ESP_ERR_INVALID_STATE);
    head=tail=0;
    /* The address may alias the caller's previous metadata. */
    assert(cerberus_s3_ble_scanner_start_reacquire(meta.target_address,epoch,&meta)==ESP_OK);
    epoch=meta.scan_epoch;pump();
    assert(meta.reacquiring&&meta.status.stored_count==0&&!memcmp(meta.target_address,target,6));
    revision=snapshot->revision;
#if FAKE_EXT_SCAN
    event.ext_disc.data=bytes;event.ext_disc.length_data=3;
#else
    event.disc.data=bytes;event.disc.length_data=3;
#endif
    radio_callback(&event,old_arg);
    assert(snapshot->revision==revision);
    mac_cancel_error=91;
    assert(cerberus_s3_ble_scanner_stop_discovery(epoch,&meta)==ESP_OK&&meta.stopping&&meta.owned);
    revision=snapshot->revision;
    reacquire_report(0,90,false);
    assert(snapshot->revision==revision&&snapshot->status.stored_count==0);
    pump();assert(cerberus_s3_ble_scanner_release_discovery(epoch)==ESP_ERR_NOT_FINISHED);
    assert(snapshot->status.stored_count==0);
    mac_cancel_error=0;
    assert(cerberus_s3_ble_scanner_stop_discovery(epoch,&meta)==ESP_OK);pump();
    assert(cerberus_s3_ble_scanner_release_discovery(epoch)==ESP_OK&&!discovery_reacquiring&&
        all_zero(discovery_target_address,sizeof(discovery_target_address))&&!owner&&!tracker.enabled);
    /* Ordinary active discovery is not a targeted copy source. */
    head=tail=0;expected_active=true;
    assert(cerberus_s3_ble_scanner_start_discovery(CERBERUS_S3_BLE_FILTER_ALL,0,&meta)==ESP_OK);
    epoch=meta.scan_epoch;assert(!meta.reacquiring&&all_zero(meta.target_address,6));pump();
    assert(cerberus_s3_ble_scanner_copy_reacquire(epoch,rows,&meta)==ESP_ERR_INVALID_STATE&&all_zero(rows,sizeof(rows)));
    discovery_native_complete();
    assert(cerberus_s3_ble_scanner_start_reacquire(target,epoch,&failed)==ESP_ERR_INVALID_STATE);
    assert(cerberus_s3_ble_scanner_release_discovery(epoch)==ESP_OK);
    /* Queued targeted Start followed by host reset must drain without using
     * the replacement central lease. */
    expected_active=false;head=tail=0;
    assert(cerberus_s3_ble_scanner_start_reacquire(target,0,&meta)==ESP_OK);epoch=meta.scan_epoch;
    ++host_epoch;owner=CERBERUS_S3_BLE_OWNER_CENTRAL;
    unsigned starts=mac_radio_starts;
    assert(cerberus_s3_ble_scanner_discovery_status(epoch,&meta)==ESP_OK&&meta.owned&&meta.stopping);
    assert(cerberus_s3_ble_scanner_release_discovery(epoch)==ESP_ERR_NOT_FINISHED);pump();
    assert(cerberus_s3_ble_scanner_stop_discovery(epoch,&meta)==ESP_OK&&!meta.owned);
    assert(cerberus_s3_ble_scanner_release_discovery(epoch)==ESP_OK&&owner==CERBERUS_S3_BLE_OWNER_CENTRAL&&mac_radio_starts==starts);
    owner=CERBERUS_S3_BLE_OWNER_NONE;head=tail=0;clock_base_us=0;tick=1000000U;
    puts("Native saved-address acquisition: passive originals, four observed types, early filtering, exact rearm/copy/Stop/reset passed.");
}
