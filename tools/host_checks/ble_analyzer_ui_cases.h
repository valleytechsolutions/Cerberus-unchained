/* Actual extracted menu/Back/taps/lifecycle and C5 owner/coordinator. Only the
 * scanner API and framed peer are simulated. No command routes here transmit. */
static void analyzer_reset_state(void)
{
    seed_list();reset();ble_discovery_live=NULL;
    ble_mode=UI_BLE_MODE_ANALYZER;ble_session_exit_page=UI_PAGE_BLE;learning_tool_active=false;
    ble_analyzer_start_pending=ble_analyzer_started=ble_analyzer_exit_pending=false;
    ble_analyzer_retry_requested=ble_analyzer_run_requested=ble_analyzer_s3_start_pending=false;
    ble_analyzer_s3_owned=ble_analyzer_s3_receiving_cached=ble_analyzer_cleanup_timed_out=false;
    ble_analyzer_error=ble_analyzer_barrier_error=ESP_OK;
    ble_analyzer_last_tick_ms=ble_analyzer_last_draw_ms=ble_analyzer_exit_started_ms=0U;
    ble_analyzer_start_requested_ms=ble_analyzer_s3_start_requested_ms=0U;
    memset(&analyzer_native,0,sizeof(analyzer_native));memset(&analyzer_view,0,sizeof(analyzer_view));
    analyzer_begins=analyzer_ends=passive_starts=stops=0U;
    fox_status_error=fox_stop_error=fox_start_error=ESP_OK;fox_hold_running=false;++fox_host_epoch;
    companion_actual_owner=true;memset(&composite_peer,0,sizeof(composite_peer));
    composite_peer.boot=UINT64_C(0x500000001);composite_peer.version=2U;
    const cerberus_dual_ble_ops_t ops={NULL,fixture_actual_dual_ble_ui_exchange,fixture_actual_dual_ble_ui_clock};
    cerberus_dual_ble_init(&companion_store,&ops);dual_ble_ui_store=&companion_store;
    dual_ble_ui_mode=-1;dual_ble_ui_family=0U;dual_ble_ui_attempted=false;dual_ble_ui_error=ESP_OK;
    dual_ble_ui_response_requests=false;ble_analyzer_s3_details=false;clock_ms=1000U;
}
static void analyzer_step(uint64_t now,bool asleep)
{
    clock_ms=(int64_t)now;
    unsigned frames=composite_peer.frames;
    dual_ble_live_tick(now,asleep);ble_analyzer_tick((uint32_t)now,asleep);
    ANALYZER_CHECK(composite_peer.frames-frames<=4U);
}
static void analyzer_steps(unsigned count)
{for(unsigned i=0;i<count;++i)analyzer_step((uint64_t)clock_ms+250U,false);}
static void analyzer_open_run(void)
{ble_mode=UI_BLE_MODE_MENU;actual_open_analyzer();analyzer_steps(6U);}
static void analyzer_drain(void)
{
    analyzer_native.get_error=analyzer_native.stop_error=ESP_OK;analyzer_native.hold_stop=false;
    composite_peer.hold_stop=false;composite_peer.stop_error=ESP_OK;
    ble_analyzer_request_stop((uint32_t)clock_ms,false);
    for(unsigned i=0;i<60U&&(ble_analyzer_exit_pending||!dual_ble_ui_released());++i)analyzer_steps(1U);
    ANALYZER_CHECK(!ble_analyzer_exit_pending&&!ble_analyzer_requested()&&dual_ble_ui_released());
    ANALYZER_CHECK(!analyzer_native.state.attached&&!analyzer_native.foreign_stops);
    if(dual_ble_ui_store&&dual_ble_ui_store!=&companion_store)heap_caps_free(dual_ble_ui_store);
    dual_ble_ui_store=&companion_store;companion_actual_owner=false;
}
static void analyzer_render(const char *prefix,const char *name)
{
    unsigned frames=composite_peer.frames,starts=analyzer_native.starts,stop=analyzer_native.stops;
    cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND);drawn[0]=0;
    ANALYZER_CHECK(draw_ble_analyzer()==ESP_OK&&actual_analyzer_footer()==ESP_OK);
    ANALYZER_CHECK(composite_peer.frames==frames&&analyzer_native.starts==starts&&analyzer_native.stops==stop);
    save(prefix,name);
}
static void check_analyzer_controls(const char *prefix)
{
    /* Each S3 failure is isolated from a healthy, full-bound C5 job. */
    for(unsigned failure=0;failure<4U;++failure){
        analyzer_reset_state();
        if(failure==0U)analyzer_native.begin_error=ESP_ERR_NO_MEM;
        if(failure==1U)analyzer_native.start_error=ESP_ERR_NO_MEM;
        analyzer_open_run();
        if(failure==2U){analyzer_native.state.error=ESP_FAIL;analyzer_steps(3U);}
        if(failure==3U){analyzer_native.get_error=ESP_FAIL;analyzer_steps(3U);}
        ANALYZER_CHECK(ble_analyzer_requested()&&ble_analyzer_error!=ESP_OK&&composite_peer.owned);
        ANALYZER_CHECK(ble_analyzer_s3_retryable());
        uint32_t job=composite_peer.job;uint64_t epoch=composite_peer.epoch;
        unsigned c5starts=composite_peer.starts,c5stops=composite_peer.stops,s3starts=analyzer_native.starts;
        analyzer_steps(4U);
        ANALYZER_CHECK(composite_peer.job==job&&composite_peer.epoch==epoch&&
            composite_peer.starts==c5starts&&composite_peer.stops==c5stops&&analyzer_native.starts==s3starts);
        analyzer_native.history_error=ESP_ERR_INVALID_STATE;
        if(failure==0U){analyzer_render(prefix,"analyzer-s3-allocation-c5-running");
            ANALYZER_CHECK(strstr(drawn,"S3 CALLBACKS UNKNOWN")&&strstr(drawn,"C5 LISTENING")&&strstr(drawn,"RETRY S3"));}
        if(failure<2U){
            handle_ble_tap(40U,400U);analyzer_steps(4U);
            ANALYZER_CHECK(ble_analyzer_s3_retryable()&&composite_peer.job==job&&
                composite_peer.epoch==epoch&&composite_peer.stops==c5stops);
        }
        analyzer_native.begin_error=analyzer_native.start_error=analyzer_native.history_error=analyzer_native.get_error=ESP_OK;
        handle_ble_tap(40U,400U);analyzer_steps(4U);
        ANALYZER_CHECK(ble_analyzer_error==ESP_OK&&analyzer_native.state.owned&&
            composite_peer.job==job&&composite_peer.epoch==epoch&&composite_peer.stops==c5stops);
        analyzer_drain();
    }
    /* C5 startup, telemetry and optional allocation failures leave the S3
     * exact scanner epoch and its history intact; retry does not reset it. */
    for(unsigned failure=0U;failure<3U;++failure){
        analyzer_reset_state();
        if(failure==0U)composite_peer.start_error=ESP_ERR_NO_MEM;
        analyzer_open_run();
        if(failure==1U){composite_peer.sample_error=ESP_FAIL;analyzer_steps(5U);}
        if(failure==2U){
            /* Stop only C5, then make its explicit allocation retry fail. */
            dual_ble_ui_cancel();composite_peer.hold_stop=false;
            while(!dual_ble_ui_released())fixture_actual_dual_ble_ui_sync(-1,NULL,0U,(uint64_t)(clock_ms+=250));
            dual_ble_ui_store=NULL;dual_ble_ui_attempted=false;fail_alloc=true;analyzer_steps(1U);fail_alloc=false;
        }
        if(!(analyzer_native.state.owned&&ble_analyzer_error==ESP_OK&&ble_analyzer_c5_retryable()))
            fprintf(stderr,"Analyzer C5 failure %u: S3own=%u S3err=%d C5mode=%d attempted=%u err=%d peerown=%u released=%u phase=%u\n",
                failure,analyzer_native.state.owned,ble_analyzer_error,dual_ble_ui_mode,dual_ble_ui_attempted,
                dual_ble_ui_error?dual_ble_ui_error:dual_ble_ui_store?dual_ble_ui_store->error:0,composite_peer.owned,dual_ble_ui_released(),dual_ble_ui_store?dual_ble_ui_store->phase:0U);
        ANALYZER_CHECK(analyzer_native.state.owned&&ble_analyzer_error==ESP_OK&&ble_analyzer_c5_retryable());
        uint32_t epoch=analyzer_native.state.scan_epoch;unsigned starts=analyzer_native.starts,stops_before=analyzer_native.stops;
        analyzer_view.total_events=73U;analyzer_view.current_events=7U;
        analyzer_view.history_count=1U;analyzer_view.history[0]=9U;
        cerberus_ble_analyzer_view_t history=analyzer_view;
        analyzer_steps(3U);ANALYZER_CHECK(analyzer_native.state.scan_epoch==epoch&&analyzer_native.starts==starts);
        if(failure==1U){analyzer_render(prefix,"analyzer-c5-error-s3-running");
            ANALYZER_CHECK(strstr(drawn,"S3 CALLBACKS 73")&&strstr(drawn,"RETRY C5"));}
        if(failure<2U){
            handle_ble_tap(200U,400U);analyzer_steps(6U);
            ANALYZER_CHECK(ble_analyzer_c5_retryable()&&analyzer_native.state.scan_epoch==epoch&&
                analyzer_native.starts==starts&&analyzer_native.stops==stops_before&&
                !memcmp(&history,&analyzer_view,sizeof(history)));
        }
        composite_peer.start_error=composite_peer.sample_error=ESP_OK;
        handle_ble_tap(200U,400U);analyzer_steps(5U);
        ANALYZER_CHECK(composite_peer.owned&&analyzer_native.state.scan_epoch==epoch&&
            analyzer_native.starts==starts&&analyzer_native.stops==stops_before&&
            !memcmp(&history,&analyzer_view,sizeof(history)));
        analyzer_drain();
    }
    /* An old peer is explicitly unavailable, but S3 stays usable. */
    analyzer_reset_state();composite_peer.unsupported=true;analyzer_open_run();
    ANALYZER_CHECK(analyzer_native.state.owned&&!ble_analyzer_c5_retryable());
    analyzer_render(prefix,"analyzer-dual-unsupported");
    ANALYZER_CHECK(strstr(drawn,"C5 UNAVAILABLE")&&strstr(drawn,"S3 LISTENING"));analyzer_drain();

    analyzer_reset_state();analyzer_native.pending_start=true;analyzer_open_run();
    ANALYZER_CHECK(analyzer_native.state.owned&&!analyzer_native.state.receiving&&composite_peer.owned);
    analyzer_render(prefix,"analyzer-s3-pending-c5-running");
    ANALYZER_CHECK(strstr(drawn,"S3 WAITING FOR RADIO")&&!strstr(drawn,"S3 LISTENING"));analyzer_drain();

    /* Automatic C5 job rollover retains the graph as history but invalidates
     * its old current count. First new sample is a new rate baseline. */
    analyzer_reset_state();analyzer_open_run();analyzer_steps(3U);
    uint32_t s3epoch=analyzer_native.state.scan_epoch;
    uint32_t oldjob=composite_peer.job;composite_peer.delay_sample_job=oldjob+1U;composite_peer.owned=false;
    for(unsigned i=0;i<12U&&composite_peer.job==oldjob;++i)analyzer_steps(1U);
    ANALYZER_CHECK(composite_peer.job==oldjob+1U&&analyzer_native.state.scan_epoch==s3epoch);
    cerberus_dual_ble_analyzer_view_t current;dual_ble_ui_get_analyzer_view(&current);
    ANALYZER_CHECK(!current.sample_valid&&current.history_count>0U);
    analyzer_render(prefix,"analyzer-c5-between-jobs");
    ANALYZER_CHECK(strstr(drawn,"C5 COUNT UNKNOWN")&&strstr(drawn,"RETAINED SOURCE INTERVALS"));
    composite_peer.delay_sample_job=0U;analyzer_steps(4U);
    dual_ble_ui_get_analyzer_view(&current);
    ANALYZER_CHECK(current.sample_valid&&current.epoch==composite_peer.epoch&&analyzer_native.state.scan_epoch==s3epoch);
    analyzer_drain();

    /* Stop wins before C5 INFO, including a clock beyond the uint32 UI wrap. */
    for(unsigned route=0U;route<3U;++route){
        analyzer_reset_state();clock_ms=(int64_t)UINT32_MAX+900;
        ble_mode=UI_BLE_MODE_MENU;actual_open_analyzer();
        analyzer_steps(1U);analyzer_steps(1U);
        ANALYZER_CHECK(dual_ble_ui_store->enabled&&!composite_peer.frames&&!composite_peer.starts);
        unsigned before=composite_peer.frames;
        if(route==0U)actual_back(UI_BACK_Y);
        else if(route==1U)ble_analyzer_request_stop((uint32_t)clock_ms,false);
        analyzer_step((uint64_t)clock_ms+250U,route==2U);
        analyzer_steps(4U);
        ANALYZER_CHECK(!ble_analyzer_requested()&&!composite_peer.starts&&composite_peer.frames==before&&
            !dual_ble_ui_store->enabled&&!analyzer_native.state.attached);
        analyzer_drain();
    }
    /* A deadline reports cleanup, never abandons the bound owners or rearms.
     * Both Stop errors are injected; eventual release remains recoverable. */
    analyzer_reset_state();analyzer_open_run();
    analyzer_native.hold_stop=composite_peer.hold_stop=true;
    analyzer_native.stop_error=composite_peer.stop_error=ESP_FAIL;
    actual_back(UI_BACK_Y);uint32_t since=ble_analyzer_exit_started_ms;
    analyzer_steps(1U);analyzer_step((uint64_t)clock_ms+13000U,false);
    ANALYZER_CHECK(ble_analyzer_exit_pending&&ble_analyzer_cleanup_timed_out&&
        analyzer_native.state.attached&&composite_peer.attached&&!ble_analyzer_requested());
    unsigned s3starts=analyzer_native.starts,c5starts=composite_peer.starts;
    handle_ble_tap(40U,400U);handle_ble_tap(200U,400U);actual_back(UI_BACK_Y);analyzer_steps(2U);
    ANALYZER_CHECK(ble_analyzer_exit_started_ms==since&&analyzer_native.starts==s3starts&&composite_peer.starts==c5starts);
    analyzer_render(prefix,"analyzer-dual-release-timeout");
    ANALYZER_CHECK(strstr(drawn,"TIMEOUT")&&strstr(drawn,"STOPPING"));analyzer_drain();

    /* Exact completed sweep restarts only itself. A foreign replacement or
     * host reset must retire the old S3 lease without generic Stop/Start. */
    analyzer_reset_state();analyzer_open_run();
    uint32_t first=analyzer_native.state.scan_epoch;uint32_t c5job=composite_peer.job;
    owner=CERBERUS_S3_BLE_OWNER_NONE;scan.state=CERBERUS_BLE_SCAN_COMPLETE;analyzer_steps(2U);
    ANALYZER_CHECK(analyzer_native.state.scan_epoch>first&&composite_peer.job==c5job);
    for(unsigned replacement=0U;replacement<2U;++replacement){
        if(replacement)++fox_host_epoch;else ++analyzer_native.current_epoch;
        scan.state=CERBERUS_BLE_SCAN_RUNNING;owner=CERBERUS_S3_BLE_OWNER_SCANNER;
        unsigned generic_stops=stops,oldstarts=analyzer_native.starts;
        analyzer_steps(4U);
        ANALYZER_CHECK(ble_analyzer_error!=ESP_OK&&composite_peer.owned&&stops==generic_stops&&
            analyzer_native.starts==oldstarts&&!analyzer_native.foreign_stops);
        if(!replacement){
            handle_ble_tap(40U,400U);analyzer_steps(2U);
            ANALYZER_CHECK(owner==CERBERUS_S3_BLE_OWNER_SCANNER&&stops==generic_stops);
            owner=CERBERUS_S3_BLE_OWNER_NONE;scan.state=CERBERUS_BLE_SCAN_IDLE;analyzer_steps(4U);
            if(ble_analyzer_s3_retryable()){handle_ble_tap(40U,400U);analyzer_steps(4U);}
            ANALYZER_CHECK(analyzer_native.state.owned);
        }
    }
    unsigned foreign_generation=scan.generation;analyzer_drain();
    ANALYZER_CHECK(owner==CERBERUS_S3_BLE_OWNER_SCANNER&&scan.generation==foreign_generation&&!stops);
    owner=CERBERUS_S3_BLE_OWNER_NONE;

    /* Initial prior-owner barriers are separate from source startup. A second
     * failed Retry Start also repaints its timeout; no foreign Stop is sent. */
    analyzer_reset_state();owner=CERBERUS_S3_BLE_OWNER_SCANNER;scan.state=CERBERUS_BLE_SCAN_RUNNING;
    ble_mode=UI_BLE_MODE_MENU;actual_open_analyzer();analyzer_steps(1U);
    analyzer_step((uint64_t)clock_ms+13000U,false);
    ANALYZER_CHECK(ble_analyzer_barrier_error==ESP_ERR_TIMEOUT&&!analyzer_begins&&!passive_starts&&!stops&&!composite_peer.frames);
    unsigned draws=fixture_detail_draws;
    handle_ble_tap(40U,400U);analyzer_step((uint64_t)clock_ms+13000U,false);
    ANALYZER_CHECK(ble_analyzer_barrier_error==ESP_ERR_TIMEOUT&&fixture_detail_draws>draws&&!stops&&!composite_peer.frames);
    analyzer_render(prefix,"analyzer-prior-owner-timeout");
    ANALYZER_CHECK(strstr(drawn,"START BLOCKED")&&strstr(drawn,"RETRY START"));
    owner=CERBERUS_S3_BLE_OWNER_NONE;scan.state=CERBERUS_BLE_SCAN_IDLE;
    analyzer_steps(2U);ANALYZER_CHECK(!analyzer_begins&&!composite_peer.starts);
    handle_ble_tap(40U,400U);analyzer_steps(6U);
    ANALYZER_CHECK(analyzer_native.state.owned&&composite_peer.owned);analyzer_drain();

    /* Reopening an Analyzer with its retained attachment does not orphan it.
     * The shared barrier cannot reset attachment identity to stop a stranger. */
    analyzer_reset_state();analyzer_open_run();analyzer_native.hold_stop=true;
    uint32_t previous_epoch=analyzer_native.state.scan_epoch;
    unsigned previous_ends=analyzer_ends;
    ble_mode=UI_BLE_MODE_MENU;actual_open_analyzer();analyzer_steps(2U);
    ANALYZER_CHECK(analyzer_native.state.owned&&analyzer_native.state.scan_epoch==previous_epoch&&
        analyzer_ends==previous_ends&&analyzer_native.stops>0U);
    actual_back(UI_BACK_Y);analyzer_native.hold_stop=false;analyzer_drain();
    ANALYZER_CHECK(!analyzer_native.foreign_stops);

    /* Hidden-page cleanup stays active without navigating over the new page;
     * actual sleep after acquisition also drains both sources. */
    for(unsigned hidden=0U;hidden<2U;++hidden){
        analyzer_reset_state();analyzer_open_run();
        analyzer_native.hold_stop=composite_peer.hold_stop=true;
        if(hidden)current_page=UI_PAGE_OTHER;
        analyzer_step((uint64_t)clock_ms+250U,!hidden);
        ANALYZER_CHECK(ble_analyzer_exit_pending&&!ble_analyzer_requested()&&analyzer_native.state.attached);
        analyzer_step((uint64_t)clock_ms+13000U,!hidden);
        ANALYZER_CHECK(ble_analyzer_cleanup_timed_out&&analyzer_native.state.attached);
        analyzer_native.hold_stop=composite_peer.hold_stop=false;
        analyzer_drain();
        ANALYZER_CHECK(current_page==(hidden?UI_PAGE_OTHER:UI_PAGE_BLE));
    }
    printf("Analyzer actual independent lifecycle/peer and exact scanner boundary: %u checks passed.\n",analyzer_ui_checks);
}

static void check_analyzer_dual_ui(const char *prefix)
{
    analyzer_reset_state();analyzer_open_run();
    analyzer_view.total_events=73U;analyzer_view.current_events=7U;analyzer_view.history_count=3U;
    analyzer_view.history[0]=2U;analyzer_view.history[1]=0U;analyzer_view.history[2]=10U;
    cerberus_dual_ble_analyzer_history_t *h=&dual_ble_ui_store->analyzer;
    memset(h,0,sizeof(*h));
    ANALYZER_CHECK(cerberus_dual_ble_analyzer_accept(h,composite_peer.epoch,composite_peer.job,40U,500U,false,false));
    ANALYZER_CHECK(cerberus_dual_ble_analyzer_accept(h,composite_peer.epoch,composite_peer.job,52U,1000U,false,false));
    cerberus_dual_ble_analyzer_gap(h);
    ANALYZER_CHECK(cerberus_dual_ble_analyzer_accept(h,composite_peer.epoch,composite_peer.job,60U,1500U,false,false));
    ANALYZER_CHECK(cerberus_dual_ble_analyzer_accept(h,composite_peer.epoch,composite_peer.job,60U,2250U,false,false));
    ANALYZER_CHECK(cerberus_dual_ble_analyzer_accept(h,composite_peer.epoch,composite_peer.job,72U,2750U,false,false));
    cerberus_dual_ble_scan_t frozen=*dual_ble_ui_store;
    analyzer_render(prefix,"analyzer-dual-independent");
    ANALYZER_CHECK(strstr(drawn,"S3 CALLBACKS 73 / OPEN BUCKET 7")&&strstr(drawn,"3 COMPLETE 200ms BUCKETS / ADV+RSP")&&
        strstr(drawn,"C5 COUNT 72 / SWEEP 2750 ms")&&strstr(drawn,"C5 24 CALLBACKS/s OVER 500 ms")&&
        strstr(drawn,"SEPARATE CLOCKS / NOT UNIQUE DEVICES")&&strstr(drawn,"C5 ORIGINALS + PARTIAL / NO RESPONSES"));
    ANALYZER_CHECK(screen[331U*320U+(32U+29U*8U)]==UI_MUTED&&screen[331U*320U+(32U+30U*8U)]==UI_PANEL&&
        screen[300U*320U+(32U+31U*8U)]==UI_GOOD&&!memcmp(&frozen,dual_ble_ui_store,sizeof(frozen)));
    const uint16_t xs[]={7U,8U,160U,311U,312U},ys[]={53U,54U,79U,80U};
    unsigned starts=analyzer_native.starts,stop=analyzer_native.stops,frames=composite_peer.frames;
    for(unsigned x=0;x<5U;++x)for(unsigned y=0;y<4U;++y){
        ble_analyzer_s3_details=false;handle_ble_tap(xs[x],ys[y]);
        ANALYZER_CHECK(ble_analyzer_s3_details==(xs[x]>=8U&&xs[x]<312U&&ys[y]>=54U&&ys[y]<80U));
        ANALYZER_CHECK(analyzer_native.starts==starts&&analyzer_native.stops==stop&&composite_peer.frames==frames);
    }
    ble_analyzer_s3_details=false;analyzer_view.total_events=UINT32_MAX;analyzer_view.current_events=UINT16_MAX;
    ANALYZER_CHECK(cerberus_dual_ble_analyzer_accept(h,composite_peer.epoch,composite_peer.job,UINT32_MAX,UINT32_MAX,true,true));
    analyzer_render(prefix,"analyzer-dual-saturated");
    ANALYZER_CHECK(strstr(drawn,"S3 CALLBACKS >=4294967295 / OPEN BUCKET >=65535")&&
        strstr(drawn,"C5 COUNT >=4294967295 / SWEEP 4294967295 ms")&&strstr(drawn,"C5 RATE UNKNOWN"));
    analyzer_native.history_error=ESP_FAIL;analyzer_render(prefix,"analyzer-s3-unknown-sample");
    ANALYZER_CHECK(strstr(drawn,"S3 CALLBACKS UNKNOWN")&&!strstr(drawn,"S3 CALLBACKS 0"));
    analyzer_native.history_error=ESP_OK;analyzer_drain();ble_analyzer_s3_details=true;
    printf("Analyzer receiver graphs, source denominators and unavailable samples: %u total Analyzer UI checks passed.\n",analyzer_ui_checks);
}
