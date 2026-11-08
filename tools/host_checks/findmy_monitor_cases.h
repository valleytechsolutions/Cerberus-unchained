static void monitor_reset(void)
{
    reset();memset(&monitor_store,0,sizeof(monitor_store));ble_discovery_live=NULL;
    memset(monitor_bank,0,sizeof(monitor_bank));memset(monitor_proof,0,sizeof(monitor_proof));
    memset(&monitor_c5_view,0,sizeof(monitor_c5_view));monitor_bank_count=0;
    monitor_c5_held=false;dual_ble_ui_response_requests=false;monitor_fail_text_after=0;
    monitor_start_calls=monitor_cancel_calls=monitor_retry_calls=monitor_apply_calls=0;
    ble_findmy_available=ble_findmy_stopping=ble_findmy_running=ble_findmy_legacy_pending=false;
    ble_findmy_exit_pending=ble_findmy_select_pending=ble_findmy_paint_valid=false;
    ble_findmy_last_draw=0;ble_findmy_error=0;
}
static void monitor_seed(void)
{
    monitor_reset();ble_findmy_monitor_open();assert(ble_findmy_monitor_available());
    assert(ble_findmy_monitor_requested()&&monitor_start_calls==1&&starts==0);
    fixture.observed_ms=100;bind_fixture(7);
    fixture.focus.discovery.raw_advertisement_sequence=1;
    cerberus_s3_ble_discovery_entry_t *entry=&monitor_store.inventory.entries[0];
    *entry=(cerberus_s3_ble_discovery_entry_t){.record={.peer=fixture.peer,.advertisement=fixture.advertisement,
        .focus=fixture.focus,.generation=7,.index=0,.observed_ms=100},.epoch=123,.source_now_ms=1000,.original_source_ms=100};
    monitor_store.inventory.count=1;monitor_store.session.epoch=123;monitor_store.session.completed_sweeps=2;
    const uint8_t response[]={3,3,0x44,0xFD};
    fixture_response_row(&monitor_bank[0],response,sizeof(response),2,1000);
    /* Same address bytes on different receivers remain distinct rows. */
    memcpy(entry->record.peer.address,monitor_bank[0].address,6);
    memcpy(entry->record.advertisement.address,monitor_bank[0].address,6);
    memcpy(entry->record.focus.discovery.address,monitor_bank[0].address,6);
    entry->record.peer.address_type=entry->record.advertisement.address_type=entry->record.focus.discovery.address_type=1;
    monitor_proof[0]=(cerberus_dual_ble_provenance_t){.boot_id=0x1234,.epoch=0x5678,.generation=9,.index=0};
    monitor_bank_count=1;monitor_c5_held=true;monitor_c5_view=(cerberus_dual_ble_view_t){.enabled=true,.receiving=true,.snapshots=8,.requested_family=2};
    clock_ms=2000;drawn[0]=0;cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND);
}
static void monitor_paint(void)
{drawn[0]=0;assert(cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND)==ESP_OK);assert(draw_ble_findmy_monitor()==ESP_OK);}
static void monitor_release(void)
{monitor_store.session.epoch=0;monitor_c5_held=false;monitor_c5_view.receiving=false;}
static void check_monitor(const char *prefix)
{
    monitor_seed();monitor_paint();assert(ble_findmy_visible_count==2&&monitor_store.painted_count==2);
    assert(strstr(drawn,"S3 ACTIVE / RUNNING")&&strstr(drawn,"C5 PASSIVE / RECEIVING"));
    assert(strstr(drawn,"ADV APPLE / 1s")&&strstr(drawn,"RSP FIND MY HINT / <=2s"));
    assert(strstr(drawn,"S3 RETAINED HISTORY")&&strstr(drawn,"C5 COMMITTED BANK"));
    save(prefix,"dual-monitor");
    unsigned start_count=monitor_start_calls,retry_count=monitor_retry_calls,cancel_count=monitor_cancel_calls;
    for(unsigned i=0;i<3;++i){ble_findmy_monitor_tap(30,110);monitor_paint();}
    assert(monitor_store.source_filter==CERBERUS_BLE_DISCOVERY_LIST_ALL);
    assert(start_count==monitor_start_calls&&retry_count==monitor_retry_calls&&cancel_count==monitor_cancel_calls);
    cerberus_dual_ble_row_t painted=monitor_store.painted_c5[1];
    cerberus_dual_ble_provenance_t proof=monitor_store.painted_c5_provenance[1];
    memset(monitor_bank,0,sizeof(monitor_bank));memset(monitor_proof,0,sizeof(monitor_proof));
    ble_findmy_monitor_tap(30,FINDMY_ROWS_Y+FINDMY_ROW_STEP+2);
    assert(ble_findmy_stopping&&ble_findmy_select_pending&&!ble_findmy_monitor_requested());
    assert(!memcmp(&monitor_selected_c5,&painted,sizeof(painted))&&!memcmp(&monitor_selected_proof,&proof,sizeof(proof)));
    clock_ms=15000;ble_findmy_monitor_tick(15000,false);assert(ble_findmy_stopping&&ble_findmy_error==ESP_ERR_TIMEOUT&&monitor_apply_calls==0);
    monitor_release();ble_findmy_monitor_tick(15001,false);assert(ble_mode==UI_BLE_MODE_C5&&monitor_apply_calls==1);
    unsigned redraws=monitor_draw_calls;ble_findmy_monitor_return();
    assert(monitor_draw_calls==redraws&&!ble_findmy_monitor_requested());ble_findmy_monitor_tick(15002,false);
    assert(ble_mode==UI_BLE_MODE_FINDMY_MONITOR&&!ble_findmy_stopping);

    /* Every text failure invalidates all row taps, including final controls. */
    monitor_seed();monitor_paint();unsigned text_count=1;
    for(const char *s=drawn;*s;++s)if(*s=='\n')++text_count;
    for(unsigned fail=1;fail<text_count;++fail){
        monitor_fail_text_after=fail;drawn[0]=0;assert(draw_ble_findmy_monitor()==ESP_FAIL);
        assert(!ble_findmy_paint_valid&&!monitor_store.painted_count);
        ble_findmy_monitor_tap(30,FINDMY_ROWS_Y+2);assert(!ble_findmy_select_pending);
    }
    monitor_fail_text_after=0;monitor_paint();
    /* Frozen S3 selection retains exact full source clock despite bank mutation. */
    cerberus_s3_ble_discovery_entry_t selected=monitor_store.painted[0];
    memset(&monitor_store.inventory,0,sizeof(monitor_store.inventory));
    ble_findmy_monitor_tap(30,FINDMY_ROWS_Y+2);assert(ble_findmy_select_pending);
    monitor_release();owner=CERBERUS_S3_BLE_OWNER_SCANNER; /* foreign scanner is not ours */
    ble_findmy_monitor_tick(3000,false);
    assert(ble_mode==UI_BLE_MODE_DETAIL&&ble_selected_from_monitor&&ble_selected_scan_epoch==selected.epoch);
    assert(ble_selected_original_ms64==selected.original_source_ms&&ble_selected_source_ceiling_ms64==selected.source_now_ms);
    assert(!memcmp(&ble_findmy_selected.advertisement,&selected.record.advertisement,sizeof(selected.record.advertisement))&&stops==0);

    /* Independent retry never restarts the healthy receiver. */
    monitor_seed();monitor_store.session.error=ESP_FAIL;monitor_store.session.faulted=true;monitor_store.session.enabled=false;
    assert(ble_findmy_monitor_requested());retry_count=monitor_retry_calls;
    ble_findmy_monitor_tap(30,380);assert(monitor_start_calls==2&&monitor_retry_calls==retry_count);
    monitor_c5_view.error=ESP_ERR_NOT_SUPPORTED;start_count=monitor_start_calls;
    ble_findmy_monitor_tap(200,380);assert(monitor_retry_calls==retry_count+1&&monitor_start_calls==start_count);
    monitor_c5_view.error=ESP_ERR_NOT_SUPPORTED;monitor_paint();save(prefix,"unsupported-monitor");

    /* Explicit Stop/save cancellation wins over a pending selected row. */
    monitor_seed();monitor_paint();ble_findmy_monitor_tap(30,FINDMY_ROWS_Y+2);
    ble_findmy_request_stop(false,false);assert(!ble_findmy_select_pending&&monitor_store.selected_source==0);
    current_page=UI_PAGE_OTHER;cancel_count=monitor_cancel_calls;clock_ms=16000;
    ble_findmy_monitor_tick(16000,false);ble_findmy_monitor_tick(17000,false);
    assert(monitor_cancel_calls==cancel_count&&ble_findmy_stopping&&monitor_apply_calls==0&&current_page==UI_PAGE_OTHER);
    monitor_release();ble_findmy_monitor_tick(17001,false);assert(current_page==UI_PAGE_OTHER&&!ble_findmy_stopping);
    current_page=UI_PAGE_BLE;ble_mode=UI_BLE_MODE_FINDMY_MONITOR;ble_findmy_monitor_tick(17002,false);
    assert(!ble_findmy_select_pending&&ble_mode==UI_BLE_MODE_FINDMY_MONITOR);

    monitor_seed();cancel_count=monitor_cancel_calls;ble_findmy_monitor_tick(3000,true);
    assert(!ble_findmy_monitor_requested()&&ble_findmy_stopping&&monitor_cancel_calls==cancel_count+1);
    ble_findmy_monitor_tick(20000,true);assert(monitor_cancel_calls==cancel_count+1&&ble_findmy_stopping);
    monitor_release();ble_findmy_monitor_tick(20001,true);assert(!ble_findmy_stopping);
    ble_findmy_monitor_tick(20002,false);assert(!ble_findmy_monitor_requested());
    monitor_seed();clock_ms=(INT64_C(1)<<32)+120;
    ble_findmy_request_stop(true,false);assert(monitor_cancel_clock==(UINT64_C(1)<<32)+120);
    monitor_release();ble_findmy_monitor_tick(121,false);assert(ble_mode==UI_BLE_MODE_LIST&&!ble_findmy_monitor_available());

    /* Bounded legacy wait does not stop a foreign owner and Back cancels it. */
    monitor_reset();owner=CERBERUS_S3_BLE_OWNER_SCANNER;ble_select_frozen_live(&fixture);
    assert(ble_findmy_legacy_pending&&ble_findmy_stopping);clock_ms=12300;ble_findmy_monitor_tick(12300,false);
    assert(ble_findmy_error==ESP_ERR_TIMEOUT&&stops==0);
    ble_findmy_request_stop(true,false);ble_findmy_monitor_tick(12301,false);
    assert(!ble_findmy_legacy_pending&&ble_mode==UI_BLE_MODE_LIST&&owner==CERBERUS_S3_BLE_OWNER_SCANNER);
    monitor_reset();fail_alloc=true;ble_findmy_monitor_open();assert(ble_findmy_error==ESP_ERR_NO_MEM&&ble_findmy_monitor_requested());
    ble_findmy_request_stop(true,false);ble_findmy_monitor_tick(500,false);assert(ble_mode==UI_BLE_MODE_LIST);

    /* Source clocks remain independent; unknown upper bounds never say stale. */
    monitor_seed();clock_ms=40000;monitor_paint();assert(strstr(drawn,"RECENCY?")&&!strstr(drawn,"STALE"));
    monitor_bank[0].request_started_ms=50000;monitor_paint();assert(strstr(drawn,"RSP FIND MY HINT / AGE ?"));
    monitor_store.inventory.count=0;monitor_store.source_filter=CERBERUS_BLE_DISCOVERY_LIST_S3;
    monitor_paint();assert(strstr(drawn,"NO RETAINED FIND MY")&&strstr(drawn,"PAGE 1 OF 1"));save(prefix,"empty-monitor");
    monitor_seed();masked=true;monitor_paint();assert(!strstr(drawn,"46:45:44:43:42:41"));save(prefix,"private-monitor");masked=false;
    /* All four rows, typed collisions and a long conservative upper age fit. */
    monitor_seed();
    monitor_store.inventory.entries[1]=monitor_store.inventory.entries[0];
    monitor_store.inventory.entries[1].record.peer.address_type=0;
    monitor_store.inventory.entries[1].record.advertisement.address_type=0;
    monitor_store.inventory.entries[1].record.focus.discovery.address_type=0;
    monitor_store.inventory.count=2;
    monitor_bank[1]=monitor_bank[0];monitor_proof[1]=monitor_proof[0];
    monitor_bank[1].address_type=0;monitor_proof[1].index=1;
    monitor_bank[1].response.age_ms=UINT32_MAX-3000U;
    monitor_bank_count=2;monitor_paint();
    assert(ble_findmy_visible_count==4&&monitor_store.painted_count==4);
    assert(strstr(drawn,"T0")&&strstr(drawn,"T1")&&strstr(drawn,"RECENCY?"));
    save(prefix,"four-row-monitor");
    assert(starts==0&&stops==0); /* Shared controls only, no legacy scanner work. */
    (void)dual_ble_ui_sync;
}
