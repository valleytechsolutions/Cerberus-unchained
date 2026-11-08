/* Composite Fox regression: production lifecycle, both controls, draw, actual
 * C5 coordinator and production C5 owner facade. SDK driver boundary only is
 * modeled; the native scanner fixture proves its actual callback fencing. */
static unsigned fox_ui_checks;
#define FOX_CHECK(x) do{++fox_ui_checks;if(!(x)){fprintf(stderr,"Fox CHECK %u: %s\n",__LINE__,#x);exit(1);}}while(0)
static cerberus_c5_ble_result_t fox_composite_target(void)
{return (cerberus_c5_ble_result_t){.address={1,2,3,4,5,6},.address_type=2,.name="OWN TEST TAG"};}
static void fox_composite_seed(void)
{
    analyzer_reset_state();ble_mode=UI_BLE_MODE_DETAIL;
    ble_fox_hunt_start_pending=ble_fox_hunt_exit_pending=ble_fox_hunt_retry_requested=false;
    ble_fox_hunt_s3_started=ble_fox_hunt_s3_attached=ble_fox_hunt_s3_owned=false;
    ble_fox_hunt_s3_receiving_cached=ble_fox_hunt_s3_start_pending=ble_fox_hunt_cleanup_timed_out=false;
    ble_fox_hunt_error=ble_fox_hunt_barrier_error=ESP_OK;
    ble_fox_hunt_last_tick_ms=ble_fox_hunt_last_draw_ms=ble_fox_hunt_exit_started_ms=0U;
    ble_fox_hunt_s3_start_requested_ms=0U;
    track_begins=track_ends=passive_starts=0U;track_error=ESP_OK;
}
static void fox_composite_step(uint64_t now,bool asleep)
{
    clock_ms=(int64_t)now;unsigned frames=composite_peer.frames;
    dual_ble_live_tick(now,asleep);ble_fox_hunt_tick((uint32_t)now,asleep);
    FOX_CHECK(composite_peer.frames-frames<=4U);
}
static void fox_composite_steps(unsigned count)
{for(unsigned i=0;i<count;++i)fox_composite_step((uint64_t)clock_ms+250U,false);}
static void fox_composite_open(bool from_c5)
{
    cerberus_c5_ble_result_t target=fox_composite_target();
    ble_fox_hunt_open_peer(&target,from_c5);fox_composite_steps(6U);
}
static void fox_composite_render(const char *prefix,const char *name)
{
    unsigned frames=composite_peer.frames,starts=track_native.starts,stop=track_native.stops;
    cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND);drawn[0]=0;
    FOX_CHECK(draw_ble_fox_hunt()==ESP_OK&&actual_fox_footer()==ESP_OK);
    FOX_CHECK(composite_peer.frames==frames&&track_native.starts==starts&&track_native.stops==stop);
    save(prefix,name);
}
static void fox_composite_drain(void)
{
    fox_status_error=fox_stop_error=ESP_OK;fox_hold_running=hold_stop=false;
    composite_peer.hold_stop=false;composite_peer.stop_error=ESP_OK;
    ble_fox_hunt_request_stop((uint32_t)clock_ms,false);
    for(unsigned i=0;i<60U&&(ble_fox_hunt_exit_pending||!dual_ble_ui_released());++i)fox_composite_steps(1U);
    FOX_CHECK(!ble_fox_hunt_exit_pending&&!track_native.state.attached&&dual_ble_ui_released());
    FOX_CHECK(!track_native.foreign_stops);
    if(dual_ble_ui_store&&dual_ble_ui_store!=&companion_store)heap_caps_free(dual_ble_ui_store);
    dual_ble_ui_store=&companion_store;companion_actual_owner=false;
}
static void check_fox_composite_ui(const char *prefix)
{
    cerberus_c5_ble_result_t target=fox_composite_target();
    for(unsigned failure=0;failure<4U;++failure){
        fox_composite_seed();if(failure==0U)track_error=ESP_ERR_NO_MEM;
        if(failure==1U)fox_start_error=ESP_ERR_NO_MEM;
        fox_composite_open(true);
        if(failure==2U){track_native.state.error=ESP_FAIL;fox_composite_steps(3U);}
        if(failure==3U){fox_status_error=ESP_FAIL;fox_composite_steps(3U);}
        FOX_CHECK(ble_fox_hunt_error!=ESP_OK&&composite_peer.owned&&ble_fox_hunt_s3_retryable());
        FOX_CHECK(composite_peer.address_type==target.address_type&&!memcmp(composite_peer.address,target.address,6U));
        uint32_t job=composite_peer.job;uint64_t epoch=composite_peer.epoch;
        unsigned c5starts=composite_peer.starts,c5stops=composite_peer.stops;
        fox_composite_steps(4U);FOX_CHECK(composite_peer.job==job&&composite_peer.epoch==epoch&&
            composite_peer.starts==c5starts&&composite_peer.stops==c5stops);
        if(failure<2U){handle_ble_tap(40,365);fox_composite_steps(4U);
            FOX_CHECK(ble_fox_hunt_error!=ESP_OK&&composite_peer.job==job&&composite_peer.stops==c5stops);}
        if(failure==0U){fox_composite_render(prefix,"fox-s3-allocation-c5-running");
            FOX_CHECK(strstr(drawn,"S3 / ERROR")&&strstr(drawn,"C5 / LISTENING")&&
                strstr(drawn,"S3 REPORTS / NOT AVAILABLE"));}
        if(failure==1U){fox_composite_render(prefix,"fox-s3-start-error-c5-running");
            FOX_CHECK(strstr(drawn,"S3 REPORTS / NOT AVAILABLE")&&!strstr(drawn,"S3 REPORTS 0"));}
        track_error=fox_start_error=fox_status_error=ESP_OK;
        handle_ble_tap(40,365);fox_composite_steps(4U);
        FOX_CHECK(track_native.state.owned&&ble_fox_hunt_error==ESP_OK&&composite_peer.job==job&&
            composite_peer.epoch==epoch&&composite_peer.stops==c5stops);
        fox_composite_drain();
    }
    for(unsigned failure=0;failure<3U;++failure){
        fox_composite_seed();if(failure==0U)composite_peer.start_error=ESP_ERR_NO_MEM;
        fox_composite_open(false);
        if(failure==1U){composite_peer.sample_error=ESP_FAIL;fox_composite_steps(5U);}
        if(failure==2U){
            dual_ble_ui_cancel();while(!dual_ble_ui_released())
                fixture_actual_dual_ble_ui_sync(-1,NULL,0U,(uint64_t)(clock_ms+=250));
            dual_ble_ui_store=NULL;dual_ble_ui_attempted=false;fail_alloc=true;fox_composite_steps(1U);fail_alloc=false;
        }
        FOX_CHECK(track_native.state.owned&&ble_fox_hunt_error==ESP_OK&&ble_fox_hunt_c5_retryable());
        FOX_CHECK(track_native_callback(track_native.state.scan_epoch,fox_host_epoch,target.address,target.address_type,-47));
        uint32_t epoch=track_native.state.scan_epoch;unsigned starts=track_native.starts,stop=track_native.stops;
        cerberus_s3_ble_tracker_t tracker=track_native.tracker;
        fox_composite_steps(3U);FOX_CHECK(track_native.state.scan_epoch==epoch&&track_native.starts==starts);
        if(failure==1U){fox_composite_render(prefix,"fox-c5-error-s3-running");
            FOX_CHECK(strstr(drawn,"S3 LAST -47")&&strstr(drawn,"C5 / ERROR"));}
        composite_peer.sample_error=composite_peer.start_error=ESP_OK;
        handle_ble_tap(200,365);fox_composite_steps(5U);
        FOX_CHECK(composite_peer.owned&&track_native.state.scan_epoch==epoch&&track_native.starts==starts&&
            track_native.stops==stop&&!memcmp(&tracker,&track_native.tracker,sizeof(tracker)));
        fox_composite_drain();
    }
    /* SDK queued is owned, but not yet listening and cannot accept reports. */
    fox_composite_seed();track_native.queued=true;fox_composite_open(false);
    FOX_CHECK(track_native.state.owned&&!track_native.state.receiving&&composite_peer.owned);
    FOX_CHECK(!track_native_callback(track_native.state.scan_epoch,fox_host_epoch,target.address,target.address_type,-47));
    fox_composite_render(prefix,"fox-s3-queued-c5-running");
    FOX_CHECK(!strstr(drawn,"S3 / LISTENING")&&!strstr(drawn,"S3 LAST -47"));
    track_native.state.receiving=true;fox_composite_steps(1U);
    FOX_CHECK(!track_native_callback(track_native.state.scan_epoch,fox_host_epoch,target.address,1U,-33));
    FOX_CHECK(track_native_callback(track_native.state.scan_epoch,fox_host_epoch,target.address,target.address_type,-47));
    fox_composite_render(prefix,"fox-both-bound");
    FOX_CHECK(strstr(drawn,"S3 / LISTENING")&&strstr(drawn,"S3 LAST -47")&&strstr(drawn,"C5 LAST -61"));
    fox_composite_drain();

    /* A replacement with the SAME public generation must fail closed even
     * before the next UI lifecycle tick, including the actual rendered RSSI. */
    for(unsigned replacement=0U;replacement<2U;++replacement){
        fox_composite_seed();fox_composite_open(false);
        uint32_t old_epoch=track_native.state.scan_epoch;unsigned old_host=fox_host_epoch;
        FOX_CHECK(track_native_callback(old_epoch,old_host,target.address,target.address_type,-47));
        uint8_t public_generation=scan.generation;
        if(replacement)++fox_host_epoch;else ++track_native.current_epoch;
        FOX_CHECK(!track_native_callback(old_epoch,old_host,target.address,target.address_type,-88));
        FOX_CHECK(!track_native_callback(track_native.current_epoch,fox_host_epoch,target.address,target.address_type,-22));
        FOX_CHECK(scan.generation==public_generation&&ble_fox_hunt_s3_started);
        unsigned calls=track_native.starts,stopped=stops;
        fox_composite_render(prefix,replacement?"fox-host-reset-before-poll":"fox-replaced-epoch-before-poll");
        FOX_CHECK(!strstr(drawn,"S3 LAST -47")&&!strstr(drawn,"S3 LAST -88")&&!strstr(drawn,"S3 LAST -22")&&
            !strstr(drawn,"S3 / LISTENING")&&strstr(drawn,"C5 / LISTENING"));
        actual_back(UI_BACK_Y);fox_composite_steps(4U);
        FOX_CHECK(stops==stopped&&track_native.starts==calls&&owner==CERBERUS_S3_BLE_OWNER_SCANNER&&
            scan.generation==public_generation&&!track_native.state.attached);
        fox_composite_drain();owner=CERBERUS_S3_BLE_OWNER_NONE;
    }
    /* Exact natural completion can restart the same target without changing
     * the independent C5 job or resetting the S3 tracker history. */
    fox_composite_seed();fox_composite_open(false);
    uint32_t old_epoch=track_native.state.scan_epoch,job=composite_peer.job;
    FOX_CHECK(track_native_callback(old_epoch,fox_host_epoch,target.address,target.address_type,-47));
    owner=CERBERUS_S3_BLE_OWNER_NONE;scan.state=CERBERUS_BLE_SCAN_COMPLETE;
    fox_composite_steps(2U);
    FOX_CHECK(track_native.state.scan_epoch>old_epoch&&track_native.tracker.reports==1U&&composite_peer.job==job);
    FOX_CHECK(!track_native_callback(old_epoch,fox_host_epoch,target.address,target.address_type,-88));
    fox_composite_drain();

    /* An old discovery lease and a different S3 radio owner form the shared
     * barrier. Delayed release is not permission to arm after its timeout. */
    fox_composite_seed();
    for(unsigned i=0;i<5U;++i)fixture_actual_dual_ble_ui_sync(
        CERBERUS_DUAL_BLE_DISCOVERY,NULL,0U,(uint64_t)(clock_ms+=250));
    FOX_CHECK(composite_peer.owned&&composite_peer.mode==CERBERUS_DUAL_BLE_DISCOVERY);
    composite_peer.hold_stop=true;owner=CERBERUS_S3_BLE_OWNER_CENTRAL;
    unsigned prior_starts=composite_peer.starts,prior_stops=stops;
    ble_fox_hunt_open_peer(&target,true);fox_composite_steps(2U);
    FOX_CHECK(ble_fox_hunt_start_pending&&!track_begins&&composite_peer.starts==prior_starts);
    fox_composite_step((uint64_t)clock_ms+13000U,false);
    FOX_CHECK(ble_fox_hunt_barrier_error==ESP_ERR_TIMEOUT&&!track_begins&&stops==prior_stops);
    composite_peer.hold_stop=false;owner=CERBERUS_S3_BLE_OWNER_NONE;fox_composite_steps(5U);
    FOX_CHECK(dual_ble_ui_released()&&!track_begins&&composite_peer.starts==prior_starts);
    handle_ble_tap(40,365);fox_composite_steps(6U);
    FOX_CHECK(track_native.state.owned&&composite_peer.mode==CERBERUS_DUAL_BLE_TRACK&&
        composite_peer.starts==prior_starts+1U);
    fox_composite_drain();

    /* A rejected begin must not adopt, query, release or stop an existing
     * tracker merely because the native global getter could observe it. */
    fox_composite_seed();uint8_t foreign_address[6]={7,7,7,7,7,7};
    FOX_CHECK(cerberus_s3_ble_scanner_track_begin(foreign_address,1U)==ESP_OK);
    fox_composite_open(false);FOX_CHECK(!ble_fox_hunt_s3_attached&&composite_peer.owned);
    cerberus_s3_ble_track_state_t foreign_state;
    FOX_CHECK(cerberus_s3_ble_scanner_track_start(&foreign_state)==ESP_OK);
    unsigned queries=track_native.queries,release=track_native.releases,stop_requests=track_native.stops;
    cerberus_s3_ble_tracker_t foreign_tracker=track_native.tracker;
    fox_composite_steps(3U);actual_back(UI_BACK_Y);fox_composite_steps(5U);
    FOX_CHECK(!ble_fox_hunt_exit_pending&&track_native.state.owned&&dual_ble_ui_released()&&
        track_native.queries==queries&&track_native.releases==release&&track_native.stops==stop_requests&&
        !memcmp(&foreign_tracker,&track_native.tracker,sizeof(foreign_tracker)));
    FOX_CHECK(cerberus_s3_ble_scanner_track_stop(&foreign_state)==ESP_OK);
    FOX_CHECK(cerberus_s3_ble_scanner_track_release()==ESP_OK);fox_composite_drain();

    /* A source retry which cannot drain for 12 seconds loses retry intent,
     * but retains cleanup. Later release requires another explicit Retry. */
    fox_composite_seed();fox_composite_open(false);
    uint32_t c5_job=composite_peer.job;unsigned c5_starts=composite_peer.starts,c5_stop=composite_peer.stops;
    ble_fox_hunt_error=ESP_FAIL;fox_stop_error=ESP_FAIL;fox_hold_running=true;
    handle_ble_tap(40,365);FOX_CHECK(ble_fox_hunt_retry_requested);
    unsigned old_begins=track_begins,old_starts=track_native.starts;
    fox_composite_step((uint64_t)clock_ms+13000U,false);
    FOX_CHECK(!ble_fox_hunt_retry_requested&&ble_fox_hunt_error!=ESP_OK&&track_native.state.owned);
    fox_stop_error=ESP_OK;fox_hold_running=false;fox_composite_steps(4U);
    FOX_CHECK(!track_native.state.attached&&track_begins==old_begins&&track_native.starts==old_starts&&
        composite_peer.job==c5_job&&composite_peer.starts==c5_starts&&composite_peer.stops==c5_stop);
    handle_ble_tap(40,365);fox_composite_steps(3U);
    FOX_CHECK(track_native.state.owned&&track_begins==old_begins+1U&&track_native.starts==old_starts+1U&&
        composite_peer.job==c5_job);fox_composite_drain();

    /* Hidden old Fox cleanup cannot cancel a newer companion request. Test
     * the production old-page tick alone; the current page owns global sync. */
    fox_composite_seed();fox_composite_open(false);
    dual_ble_ui_cancel();while(!dual_ble_ui_released())
        fixture_actual_dual_ble_ui_sync(-1,NULL,0U,(uint64_t)(clock_ms+=250));
    for(unsigned i=0;i<5U;++i)fixture_actual_dual_ble_ui_sync(
        CERBERUS_DUAL_BLE_TRACK,foreign_address,1U,(uint64_t)(clock_ms+=250));
    FOX_CHECK(composite_peer.owned&&!memcmp(composite_peer.address,foreign_address,6U));
    c5_job=composite_peer.job;c5_starts=composite_peer.starts;c5_stop=composite_peer.stops;
    current_page=UI_PAGE_OTHER;ble_fox_hunt_tick((uint32_t)(clock_ms+=250),false);
    FOX_CHECK(!ble_fox_hunt_exit_pending&&!track_native.state.attached&&dual_ble_ui_store->enabled&&
        composite_peer.job==c5_job&&composite_peer.starts==c5_starts&&composite_peer.stops==c5_stop);
    fixture_actual_dual_ble_ui_sync(CERBERUS_DUAL_BLE_TRACK,foreign_address,1U,(uint64_t)(clock_ms+=250));
    FOX_CHECK(composite_peer.job==c5_job&&composite_peer.stops==c5_stop);
    dual_ble_ui_cancel();while(!dual_ble_ui_released())
        fixture_actual_dual_ble_ui_sync(-1,NULL,0U,(uint64_t)(clock_ms+=250));
    fox_composite_drain();

    for(unsigned route=0;route<3U;++route){
        fox_composite_seed();clock_ms=(int64_t)UINT32_MAX+1000;
        ble_fox_hunt_open_peer(&target,true);fox_composite_steps(2U);
        FOX_CHECK(dual_ble_ui_store->enabled&&!composite_peer.frames&&!composite_peer.starts);
        unsigned frames=composite_peer.frames;
        if(route==0U)actual_back(UI_BACK_Y);
        if(route==1U)current_page=UI_PAGE_OTHER;
        fox_composite_step((uint64_t)clock_ms+250U,route==2U);fox_composite_steps(3U);
        FOX_CHECK(!composite_peer.frames&&!composite_peer.starts&&!track_native.state.attached&&!ble_fox_hunt_exit_pending);
        FOX_CHECK(composite_peer.frames==frames&&current_page==(route==1U?UI_PAGE_OTHER:UI_PAGE_BLE));
        fox_composite_drain();
    }
    /* A held exact Stop is retried past deadline; repeated UI controls never
     * turn cleanup into start, even while hidden. */
    fox_composite_seed();fox_composite_open(true);fox_hold_running=composite_peer.hold_stop=true;
    fox_stop_error=composite_peer.stop_error=ESP_FAIL;actual_back(UI_BACK_Y);
    uint32_t since=ble_fox_hunt_exit_started_ms;
    fox_composite_steps(1U);fox_composite_step((uint64_t)clock_ms+13000U,false);
    FOX_CHECK(ble_fox_hunt_exit_pending&&ble_fox_hunt_cleanup_timed_out&&track_native.state.owned&&composite_peer.attached);
    unsigned s3starts=track_native.starts,c5starts=composite_peer.starts;
    handle_ble_tap(40,365);handle_ble_tap(200,365);actual_back(UI_BACK_Y);fox_composite_steps(1U);
    FOX_CHECK(track_native.starts==s3starts&&composite_peer.starts==c5starts&&ble_fox_hunt_exit_started_ms==since);
    fox_composite_render(prefix,"fox-bound-release-timeout");
    current_page=UI_PAGE_OTHER;fox_composite_steps(2U);fox_composite_drain();
    FOX_CHECK(current_page==UI_PAGE_OTHER&&track_native.starts==s3starts&&composite_peer.starts==c5starts);
    printf("Fox actual typed owner/UI/C5 composite: %u checks passed.\n",fox_ui_checks);
}
