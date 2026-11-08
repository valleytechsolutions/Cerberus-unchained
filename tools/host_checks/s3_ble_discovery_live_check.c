#define CERBERUS_DISCOVERY_NATIVE_TEST 1
#define main legacy_discovery_ui_suite
#include "s3_ble_discovery_ui_check.c"
#undef main

static unsigned live_checks;
#define CHECK(x) do { ++live_checks; if(!(x)){fprintf(stderr,"Live CHECK %u: %s\n",__LINE__,#x);exit(1);} } while(0)

static void clean_live(void)
{
    if(ble_discovery_live){
        CHECK(cerberus_ble_discovery_session_released(&ble_discovery_live->session));
        heap_caps_free(ble_discovery_live);
        ble_discovery_live=NULL;
    }
    if(ble_findmy_monitor){heap_caps_free(ble_findmy_monitor);ble_findmy_monitor=NULL;}
}

static void seed_native_stopped(cerberus_s3_ble_filter_t family)
{
    clean_live();
    drawn[0]=0;
    CHECK(cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND)==ESP_OK);
    seed_list();
    memset(&discovery_native,0,sizeof(discovery_native));
    memset(live_records,0,sizeof(live_records));
    ble_discovery_open_error=ESP_OK;ble_discovery_run_requested=false;ble_discovery_unallocated_destination=0U;ble_discovery_unallocated_last_draw_ms=0U;
    ble_filter=family;ble_attack_selecting=false;ui_capability_blocked=false;
    current_page=UI_PAGE_BLE;ble_mode=UI_BLE_MODE_LIST;
    clock_ms=1000;owner=CERBERUS_S3_BLE_OWNER_NONE;c5_online=false;
    sentinel_scan_session_init(&session);sentinel_scan_session_init(&live_session);
    session.completed=live_session.completed=true;
    live_commands=live_observations=starts=stops=0;
    live_last_poll=0;fail_alloc=false;
    discovery_ui_allocations=unified_sort_calls=unified_classify_calls=unified_response_classify_calls=0;
}

/* Most regression cases enter through the explicit Scan action. Separate
 * stopped seeds exercise drawing, allocation failure and retained returns. */
static void seed_native(cerberus_s3_ble_filter_t family)
{
    seed_native_stopped(family);
    CHECK(ble_discovery_request_start()==ESP_OK);
    companion_retries=0U; /* These older cases measure retries after explicit entry. */
}

static void native_tick(uint64_t now,bool asleep)
{
    clock_ms=(int64_t)now;
    const unsigned before=discovery_native.start+discovery_native.copy+discovery_native.poll+
        discovery_native.stop+discovery_native.release;
    live_scan_tick((uint32_t)now,asleep);
    CHECK(discovery_native.start+discovery_native.copy+discovery_native.poll+
        discovery_native.stop+discovery_native.release-before<=1U);
}

static void row(unsigned index,unsigned key,uint8_t type,int8_t rssi)
{
    cerberus_s3_ble_discovery_bank_t *bank=&discovery_native.bank;
    CHECK(index<32&&bank->meta.attached);
    cerberus_s3_ble_discovery_record_t *record=&bank->records[index];
    memset(record,0,sizeof(*record));
    record->index=(uint8_t)index;record->generation=bank->meta.status.generation;
    record->peer.address[0]=(uint8_t)key;record->peer.address[5]=0x61;
    record->peer.address_type=type;record->peer.rssi=rssi;
    record->peer.flags=CERBERUS_BLE_RESULT_CONNECTABLE;
    snprintf(record->peer.name,sizeof(record->peer.name),"Device %u",key);
    memcpy(record->advertisement.address,record->peer.address,6);
    record->advertisement.address_type=type;
    record->advertisement.length=4;record->advertisement.data[0]=3;
    record->advertisement.data[1]=3;record->advertisement.data[2]=0x82;record->advertisement.data[3]=0x30;
    record->advertisement.reports_seen=1;
    record->observed_ms=(uint32_t)clock_ms;
    cerberus_s3_ble_discovery_evidence_t *proof=&record->focus.discovery;
    memcpy(proof->address,record->peer.address,6);proof->address_type=type;proof->generation=record->generation;
    proof->advertisement_sequence=(uint32_t)++bank->meta.revision;
    proof->advertisement_observed_ms=(uint32_t)clock_ms;
    proof->flags=CERBERUS_S3_BLE_EVIDENCE_ADV_COMPLETE|CERBERUS_S3_BLE_EVIDENCE_ADV_VALID|CERBERUS_S3_BLE_EVIDENCE_RAW_COMPLETE;
    proof->flipper_advertisement=(cerberus_ble_flipper_hints_t){CERBERUS_BLE_FLIPPER_HINT_WHITE,CERBERUS_BLE_FLIPPER_UUID16};
    record->focus.category_bits=CERBERUS_S3_BLE_FOCUS_FLIPPER;
    bank->meta.source_now_ms=(uint64_t)clock_ms;
    if(bank->meta.status.stored_count<=index)bank->meta.status.stored_count=(uint8_t)(index+1U);
    ++bank->meta.status.total_count;
}

static void stop_and_release(uint64_t now)
{
    ble_discovery_request_stop();
    discovery_native.hold_stop=false;
    for(unsigned i=0;i<30&&!cerberus_ble_discovery_session_released(&ble_discovery_live->session);++i)
        native_tick(now+i*250U,false);
    CHECK(cerberus_ble_discovery_session_released(&ble_discovery_live->session));
    CHECK(!discovery_native.bank.meta.attached&&owner==CERBERUS_S3_BLE_OWNER_NONE);
}

static void finish_selected_handoff(void)
{
    CHECK(!ble_discovery_requested());
    for(unsigned i=0;i<20U&&ble_discovery_live->selection_pending;++i)
        native_tick((uint64_t)clock_ms+250U,false);
    CHECK(!ble_discovery_live->selection_pending);
}

static void first_results_and_frozen_tap(const char *prefix)
{
    seed_native(CERBERUS_S3_BLE_FILTER_ALL);
    CHECK(draw_live_scan()==ESP_OK);
    CHECK(ble_discovery_live&&discovery_native.start==0&&ble_discovery_keeps_awake());
    CHECK(discovery_ui_allocations==1);
    CHECK(ble_discovery_defers_c5_telemetry());
    native_tick(1000,false);
    CHECK(discovery_native.start==1&&discovery_native.bank.meta.owned);
    CHECK(live_commands==0&&live_observations==0); /* C5 offline does not delay S3. */
    clock_ms=1050;row(0,11,0,-45);
    native_tick(1050,false);
    CHECK(live_view.row_count==1&&ble_discovery_live->painted_count==1);
    CHECK(discovery_native.bank.meta.status.state==CERBERUS_BLE_SCAN_RUNNING);
    CHECK(!strcmp(live_names[0],"Device 11"));
    cerberus_s3_ble_discovery_entry_t selected=ble_discovery_live->painted[0];
    drawn[0]=0;CHECK(live_paint(true)==ESP_OK);save(prefix,"running-first-result");

    /* Newest bank has B, but the last physical screen still has A. Commit the
     * actual copy adapter without painting, then dispatch the actual row tap. */
    clock_ms=1100;row(0,11,1,-20); /* Same bytes, different BLE address type. */
    cerberus_ble_discovery_observation_t copied;
    CHECK(ble_discovery_copy(ble_discovery_live,ble_discovery_live->session.epoch,0,&copied).error==ESP_OK);
    CHECK(ble_discovery_live->inventory.entries[0].record.peer.address[0]==11);
    CHECK(ble_discovery_live->inventory.count==2); /* Missing A remains earlier. */
    CHECK(ble_discovery_live->inventory.entries[1].record.peer.address_type==1);
    CHECK(ble_discovery_live->painted[0].record.peer.address[0]==11);
    discovery_native.hold_stop=true;companion_released=false;
    handle_ble_tap(20,135);
    CHECK(ble_discovery_live->selection_pending&&!ble_discovery_live->session.enabled);
    CHECK(!memcmp(&selected,&ble_discovery_live->selected,sizeof(selected)));
    native_tick(1101,false);
    CHECK(discovery_native.stop==1&&ble_mode==UI_BLE_MODE_LIST);
    CHECK(owner==CERBERUS_S3_BLE_OWNER_SCANNER&&!ble_selected_from_monitor);
    native_tick(1401,false);
    CHECK(ble_mode==UI_BLE_MODE_LIST&&discovery_native.release==0);
    discovery_native_finish(true);
    native_tick(1601,false);native_tick(1801,false);
    CHECK(discovery_native.release==1&&owner==CERBERUS_S3_BLE_OWNER_NONE);
    CHECK(ble_mode==UI_BLE_MODE_LIST&&ble_discovery_live->selection_pending);
    companion_released=true;native_tick(1802,false);
    /* Existing frozen-detail bridge may use its RF-free queued monitor exit. */
    tick(1803);
    CHECK(ble_mode==UI_BLE_MODE_DETAIL);
    CHECK(ble_selected_result.address[0]==11&&ble_selected_result.address_type==0);
    CHECK(!memcmp(&ble_selected_result,&selected.record.peer,sizeof(ble_selected_result)));
    CHECK(ble_selected_original_time64_valid&&ble_selected_original_ms64==1050);
    CHECK(ble_selected_scan_epoch==selected.epoch&&ble_selected_scan_epoch!=0);
    CHECK(ble_selected_source_ceiling_ms64==selected.source_now_ms&&ble_selected_source_ceiling_ms64==1050);
    CHECK(discovery_native.start==1&&live_commands==0&&starts==0);
    CHECK(discovery_ui_allocations==1);
    actual_back(440);native_tick(2001,false);
    CHECK(ble_mode==UI_BLE_MODE_LIST&&discovery_native.start==1&&!ble_discovery_live->session.enabled);
    clean_live();
}

static void continuous_family_and_stop(const char *prefix)
{
    seed_native(CERBERUS_S3_BLE_FILTER_ALL);
    native_tick(1000,false);clock_ms=1050;row(0,31,1,-30);native_tick(1050,false);
    discovery_native_finish(false);
    native_tick(9000,false);
    CHECK(ble_discovery_live->session.final_copied&&discovery_native.start==1);
    native_tick(9050,false);
    CHECK(discovery_native.start==2&&discovery_native.release==0&&ble_discovery_live->inventory.count==1);
    CHECK(ble_discovery_live->session.active_family==CERBERUS_S3_BLE_FILTER_ALL);
    native_tick(9100,false);
    CHECK(ble_discovery_live->inventory.count==1&&!ble_discovery_live->inventory.current_mask);
    CHECK(strstr(live_details[0],"EARLIER"));

    handle_ble_tap(100,65); /* Actual family control requests iBeacon. */
    CHECK(ble_filter==CERBERUS_S3_BLE_FILTER_IBEACON);
    CHECK(ble_discovery_live->session.requested_family==1&&ble_discovery_live->session.active_family==0);
    dual_ble_live_tick(9110,false);
    CHECK(companion_requested_mode==CERBERUS_DUAL_BLE_FAMILY&&companion_requested_family==1);
    discovery_native.hold_stop=true;
    native_tick(9150,false);
    CHECK(discovery_native.stop==1&&discovery_native.start==2);
    handle_ble_tap(100,65);handle_ble_tap(100,65); /* Latest request wins: Flipper. */
    CHECK(ble_filter==CERBERUS_S3_BLE_FILTER_FLIPPER);
    dual_ble_live_tick(9300,false);
    CHECK(companion_requested_mode==CERBERUS_DUAL_BLE_FAMILY&&companion_requested_family==3);
    native_tick(9400,false);CHECK(discovery_native.start==2);
    discovery_native_finish(true);
    native_tick(9600,false);native_tick(9800,false);native_tick(10000,false);
    CHECK(discovery_native.start==3&&discovery_native.release==1);
    CHECK(ble_discovery_live->session.active_family==CERBERUS_S3_BLE_FILTER_FLIPPER);
    clock_ms=10050;row(0,33,2,-60);native_tick(10050,false);
    CHECK(ble_discovery_live->inventory.family==CERBERUS_S3_BLE_FILTER_FLIPPER);
    CHECK(ble_discovery_live->inventory.count==1&&live_view.row_count==1);
    CHECK(ble_discovery_live->painted[0].record.peer.address_type==2);
    drawn[0]=0;CHECK(live_paint(true)==ESP_OK);save(prefix,"focused-running");
    discovery_native.hold_stop=false;
    handle_live_scan_tap(100,380); /* Production pause control position. */
    CHECK(!ble_discovery_live->session.enabled);
    dual_ble_live_tick(10051,false);CHECK(companion_requested_mode==-1);
    stop_and_release(10100);
    unsigned starts_before=discovery_native.start;
    handle_ble_tap(100,65);native_tick(12000,false);
    CHECK(discovery_native.start==starts_before&&!ble_discovery_keeps_awake());
    CHECK(discovery_ui_allocations==1); /* Same UI block across sweeps and family changes. */
    clean_live();
}

static void failures_and_navigation(void)
{
    seed_native_stopped(CERBERUS_S3_BLE_FILTER_ALL);
    fail_alloc=true;
    CHECK(ble_discovery_request_start()==ESP_ERR_NO_MEM&&ble_discovery_requested());
    CHECK(draw_live_scan()==ESP_OK);
    native_tick(1000,false);
    CHECK(!ble_discovery_live&&ble_discovery_open_error==ESP_ERR_NO_MEM&&discovery_native.start==0);
    CHECK(discovery_ui_allocations==1);
    fail_alloc=false;
    native_tick(1500,false);
    CHECK(!ble_discovery_live&&discovery_native.start==0); /* Explicit retry only. */
    CHECK(discovery_ui_allocations==1);
    CHECK(ble_discovery_retry_s3()==ESP_OK);CHECK(ble_discovery_live&&ble_discovery_live->session.enabled);
    native_tick(1550,false);
    CHECK(discovery_native.start==1);
    CHECK(discovery_ui_allocations==2);
    discovery_native.stop_errors=2;discovery_native.release_errors=1;
    current_page=UI_PAGE_OTHER;
    for(uint64_t now=1650;now<5000;now+=250)native_tick(now,false);
    CHECK(!ble_discovery_live->session.epoch&&discovery_native.stop==3&&discovery_native.release==2);
    CHECK(discovery_native.start==1&&!ble_discovery_live->session.enabled);
    current_page=UI_PAGE_BLE;ble_mode=UI_BLE_MODE_LIST;native_tick(5100,false);
    CHECK(discovery_native.start==1);
    ble_discovery_toggle();native_tick(5150,false);CHECK(discovery_native.start==2);
    native_tick(5200,true);native_tick(5450,true);
    CHECK(!ble_discovery_live->session.epoch&&!ble_discovery_live->session.enabled);
    native_tick(5700,false);CHECK(discovery_native.start==2); /* Waking never restarts. */
    clean_live();

    seed_native(CERBERUS_S3_BLE_FILTER_ALL);
    discovery_native.ambiguous_start=true;
    native_tick(1000,false);
    CHECK(ble_discovery_live->session.epoch&&ble_discovery_live->session.faulted);
    native_tick(1050,false);native_tick(1300,false);
    CHECK(!ble_discovery_live->session.epoch&&discovery_native.release==1&&discovery_native.start==1);
    clean_live();

    seed_native_stopped(CERBERUS_S3_BLE_FILTER_ALL);
    ble_attack_selecting=true;native_tick(1000,false);
    CHECK(!ble_discovery_live&&discovery_native.start==0); /* Legacy scheduler path retained. */
    ble_attack_selecting=false;current_page=UI_PAGE_WATCH;native_tick(1500,false);
    CHECK(!ble_discovery_live&&discovery_native.start==0);
    current_page=UI_PAGE_OTHER;
}

static void deferred_navigation_and_copy_failure(void)
{
    seed_native(CERBERUS_S3_BLE_FILTER_ALL);
    native_tick(1000,false);clock_ms=1050;row(0,50,0,-40);native_tick(1050,false);
    cerberus_s3_ble_discovery_inventory_t frozen=ble_discovery_live->inventory;
    clock_ms=1100;row(0,51,1,-30);discovery_native.copy_errors=1;
    native_tick(1250,false);
    CHECK(!memcmp(&frozen,&ble_discovery_live->inventory,sizeof(frozen)));
    CHECK(ble_discovery_live->session.enabled&&ble_discovery_live->session.error==ESP_ERR_TIMEOUT);
    native_tick(1500,false);native_tick(1700,false);
    CHECK(ble_discovery_live->inventory.count==2&&ble_discovery_live->session.error==0);
    discovery_native.hold_stop=true;
    handle_ble_tap(20,135);
    CHECK(ble_discovery_live->selection_pending);
    actual_back(440);
    CHECK(ble_mode==UI_BLE_MODE_MENU);
    CHECK(!ble_discovery_live->selection_pending&&!ble_discovery_live->session.enabled);
    CHECK(!ble_discovery_defers_c5_telemetry());
    native_tick(1750,false);
    CHECK(!ble_discovery_live->selection_pending&&!ble_discovery_live->session.enabled);
    CHECK(discovery_native.stop==1&&ble_mode==UI_BLE_MODE_MENU);
    discovery_native_finish(true);
    native_tick(2000,false);native_tick(2250,false);
    CHECK(!ble_discovery_live->session.epoch&&ble_mode==UI_BLE_MODE_MENU);
    CHECK(!ble_selected_from_monitor&&discovery_native.start==1);
    ble_mode=UI_BLE_MODE_LIST;native_tick(2500,false);
    CHECK(discovery_native.start==1&&!ble_discovery_live->session.enabled);
    clean_live();

    seed_native(CERBERUS_S3_BLE_FILTER_ALL);
    native_tick(1000,false);clock_ms=1050;row(0,55,0,-50);native_tick(1050,false);
    discovery_native.hold_stop=true;
    handle_live_scan_tap(270,380);
    CHECK(ble_discovery_live->destination==1&&!ble_discovery_live->session.enabled);
    native_tick(1100,false);CHECK(ble_mode==UI_BLE_MODE_LIST);
    discovery_native_finish(true);native_tick(1350,false);native_tick(1600,false);
    CHECK(ble_mode==UI_BLE_MODE_MENU&&discovery_native.release==1&&starts==0);
    clean_live();
}

static void source_switch_and_maximum(const char *prefix)
{
    seed_native(CERBERUS_S3_BLE_FILTER_ALL);
    native_tick(1000,false);clock_ms=1050;
    for(unsigned i=0;i<32;++i)row(i,i+1,(uint8_t)(i%4U),(int8_t)(-30-i));
    native_tick(1050,false);
    CHECK(live_ble_filtered_count==32&&live_view.total_count==32);
    CHECK(ble_discovery_unified()&&ble_discovery_live->built_count==4);
    for(unsigned i=0;i<32;++i)CHECK(ble_discovery_live->order[i].source==CERBERUS_BLE_DISCOVERY_LIST_S3);
    ble_discovery_live->inventory.replacements=UINT32_MAX;
    ble_discovery_live->binding_overflows=UINT16_MAX;
    live_make_view();drawn[0]=0;CHECK(live_paint(true)==ESP_OK);save(prefix,"maximum");
    /* Same filter/sort/mask but a different backing bank must invalidate its
     * index cache. Legacy attack selection cannot reuse discovery indexes. */
    memset(&live_records[SENTINEL_SCAN_BLE],0,sizeof(live_records[SENTINEL_SCAN_BLE]));
    live_records[SENTINEL_SCAN_BLE].valid=true;
    live_records[SENTINEL_SCAN_BLE].captured=true;
    ble_attack_selecting=true;live_make_view();
    CHECK(!ble_discovery_defers_c5_telemetry());
    CHECK(live_ble_filtered_count==0&&live_view.row_count==0&&live_view.total_count==0);
    ble_attack_selecting=false;live_make_view();
    CHECK(ble_discovery_defers_c5_telemetry());
    CHECK(live_ble_filtered_count==32&&live_view.row_count==4);
    stop_and_release(1100);clean_live();
}

static void retained_badge_age(const char *prefix)
{
    seed_native(CERBERUS_S3_BLE_FILTER_IBEACON);
    native_tick(1000,false);clock_ms=1050;row(0,61,1,-55);
    cerberus_s3_ble_discovery_record_t *record=&discovery_native.bank.records[0];
    memset(record->advertisement.data,0,sizeof(record->advertisement.data));
    record->advertisement.length=27;
    record->advertisement.data[0]=26;record->advertisement.data[1]=0xff;
    record->advertisement.data[2]=0x4c;record->advertisement.data[4]=2;
    record->advertisement.data[5]=0x15;record->advertisement.data[26]=0xc5;
    record->focus.category_bits=CERBERUS_S3_BLE_FOCUS_IBEACON;
    record->focus.ibeacon_valid=true;
    record->focus.ibeacon_sequence=record->focus.discovery.advertisement_sequence;
    record->focus.ibeacon_observed_ms=record->observed_ms;
    CHECK(cerberus_ibeacon_parse_ad(record->advertisement.data,record->advertisement.length,
        &record->focus.ibeacon)==CERBERUS_IBEACON_FOUND);
    native_tick(1050,false);
    CHECK(strstr(live_badges[0]," / 0s"));
    stop_and_release(1100);
    cerberus_s3_ble_discovery_record_t frozen=ble_discovery_live->inventory.entries[0].record;
    clock_ms=INT64_C(4294967296)+1050;
    live_make_view();
    CHECK(strstr(live_badges[0]," / >49d")&&!strstr(live_badges[0]," / 0s"));
    drawn[0]=0;CHECK(live_paint(true)==ESP_OK);save(prefix,"retained-ibeacon-age");
    CHECK(!memcmp(&frozen,&ble_discovery_live->inventory.entries[0].record,sizeof(frozen)));
    ble_discovery_live->inventory.entries[0].source_now_ms=(uint64_t)clock_ms+1U;
    live_make_view();CHECK(strstr(live_badges[0]," / AGE ?"));
    CHECK(!memcmp(&frozen,&ble_discovery_live->inventory.entries[0].record,sizeof(frozen)));
    CHECK(discovery_native.start==1);
    clean_live();
}

static void companion_pane_and_fox_barrier(void)
{
    seed_native(CERBERUS_S3_BLE_FILTER_ALL);
    companion_released=true;native_tick(1000,false);
    clock_ms=1050;row(0,77,1,-45);native_tick(1050,false);
    CHECK(ble_discovery_live->painted_count==1);
    uint32_t frame=live_frame;
    cerberus_s3_ble_discovery_entry_t painted=ble_discovery_live->painted[0];
    handle_live_scan_tap(140,113);
    CHECK(ble_mode==UI_BLE_MODE_C5&&ble_discovery_page()&&!ble_discovery_presenting());
    clock_ms=1450;row(1,88,2,-60);native_tick(1450,false);
    CHECK(ble_discovery_live->session.enabled&&ble_discovery_live->inventory.count==2);
    CHECK(live_frame==frame&&!memcmp(&painted,&ble_discovery_live->painted[0],sizeof(painted)));
    ble_discovery_select_painted(0);CHECK(!ble_discovery_live->selection_pending);
    cerberus_c5_ble_result_t original=ble_selected_result;
    cerberus_c5_ble_result_t target={.address={9,8,7,6,5,4},.address_type=3,.name="C5 only"};
    companion_released=false;track_begins=0;
    ble_fox_hunt_open_peer(&target,true);
    CHECK(ble_mode==UI_BLE_MODE_FOX_HUNT&&ble_fox_hunt_start_pending&&track_begins==0);
    CHECK(!memcmp(&original,&ble_selected_result,sizeof(original)));
    CHECK(!dual_ble_live_prepare_fox(1500,false));
    native_tick(1550,false);native_tick(1800,false);native_tick(2050,false);
    CHECK(dual_ble_live_discovery_released()&&!dual_ble_live_prepare_fox(2100,false));
    CHECK(track_begins==0);companion_released=true;
    CHECK(!dual_ble_live_prepare_fox(2150,true)&&track_begins==0); /* Sleeping cannot arm a pending target. */
    CHECK(dual_ble_live_prepare_fox(2200,false)&&track_begins==1&&track_type==3);
    CHECK(!memcmp(track_address,target.address,6)&&!memcmp(&original,&ble_selected_result,sizeof(original)));
    ble_fox_hunt_request_stop((uint32_t)clock_ms,false);ble_fox_hunt_tick((uint32_t)clock_ms,false);CHECK(ble_mode==UI_BLE_MODE_C5);
    ble_mode=UI_BLE_MODE_LIST;native_tick(2300,false);
    CHECK(!ble_discovery_live->session.enabled&&discovery_native.start==1);
    clean_live();
}

static esp_err_t presentation_exchange(void *context,uint8_t command,const void *request,
    size_t request_size,void *response,size_t response_size)
{
    (void)context;(void)command;(void)request;(void)request_size;(void)response;(void)response_size;
    ++companion_exchanges;return ESP_ERR_INVALID_STATE;
}

static void companion_row(unsigned index,unsigned key,uint8_t type,int8_t rssi)
{
    CHECK(index<CERBERUS_DUAL_BLE_CAPACITY);
    companion_store.ops.exchange=presentation_exchange;
    companion_store.published_boot_id=UINT64_C(0x12340000C500);
    companion_store.published_epoch=UINT64_C(0x987600000022);
    companion_store.published_generation=0; /* Full identity remains valid across gen8 wrap. */
    ++companion_store.snapshots;
    cerberus_dual_ble_row_t *peer=&companion_store.rows[index];
    memset(peer,0,sizeof(*peer));
    peer->address[0]=(uint8_t)key;peer->address[5]=0x61;peer->address_type=type;peer->rssi=rssi;
    snprintf(peer->name,sizeof(peer->name),"C5 Device %u",key);
    peer->raw_length=3;peer->raw[0]=2;peer->raw[1]=1;peer->raw[2]=6;
    peer->event_type=3;peer->reports=17;peer->manufacturer_id=0x004c;
    peer->timing_flags=CERBERUS_DUAL_BLE_SEEN|CERBERUS_DUAL_BLE_RSSI_VALID;
    peer->callback_age_ms=5;peer->request_started_ms=1000;
    peer->receive=(cerberus_dual_ble_receive_metadata_t){
        .source_kind=CERBERUS_DUAL_BLE_RECEIVE_LEGACY,.callback_length=3,
        .sid=0xff,.data_status=0xff,.ad_validation=CERBERUS_DUAL_BLE_AD_VALID};
    if(companion_store.count<=index)companion_store.count=(uint8_t)(index+1U);
}

static unsigned all_boundary_calls(void)
{
    return discovery_native.start+discovery_native.copy+discovery_native.poll+
        discovery_native.stop+discovery_native.release+companion_syncs+companion_retries+
        companion_exchanges+live_commands+live_observations+starts+stops+track_begins;
}

static void repaint_unified(void)
{drawn[0]=0;live_make_view();CHECK(live_paint(true)==ESP_OK);}

static void c5_payload(unsigned index,const uint8_t *data,size_t length)
{
    CHECK(index<32U&&length<=31U);
    cerberus_dual_ble_row_t *peer=&companion_store.rows[index];
    memset(peer->raw,0,sizeof(peer->raw));memcpy(peer->raw,data,length);
    peer->raw_length=(uint8_t)length;peer->receive.callback_length=(uint8_t)length;
    ++companion_store.snapshots;
}

/* Use the real S3 collector to seed original/response evidence, then pass its
 * result through the real inventory copy and UI. This is not a second family
 * classifier invented by the presentation fixture. */
static void s3_payload(unsigned index,unsigned key,uint8_t type,const uint8_t *data,
    size_t length,bool response_only,bool later_ordinary)
{
    static cerberus_s3_ble_snapshot_t snapshot;
    static const uint8_t ordinary[]={2,1,6};
    row(index,key,type,-70);
    cerberus_s3_ble_discovery_bank_t *bank=&discovery_native.bank;
    cerberus_s3_ble_discovery_record_t *record=&bank->records[index];
    cerberus_s3_ble_snapshot_begin(&snapshot,record->generation,(uint32_t)clock_ms,false);
    cerberus_s3_ble_report_t report={.address_type=type,.rssi=-70,.complete=true,.event_type=3,
        .data=response_only?ordinary:data,.length=response_only?sizeof(ordinary):length};
    memcpy(report.address,record->peer.address,6);
    cerberus_s3_ble_snapshot_observe(&snapshot,&report,(uint32_t)clock_ms);
    if(response_only||later_ordinary){
        report.scan_response=response_only;report.event_type=response_only?4U:3U;
        report.data=response_only?data:ordinary;report.length=response_only?length:sizeof(ordinary);
        cerberus_s3_ble_snapshot_observe(&snapshot,&report,(uint32_t)clock_ms+100U);
    }
    CHECK(snapshot.status.stored_count==1U);
    const cerberus_s3_ble_record_t *from=&snapshot.records[0];
    record->focus=from->focus;record->observed_ms=from->last_advertisement_ms;
    memcpy(record->advertisement.data,from->advertisement,sizeof(record->advertisement.data));
    record->advertisement.length=from->advertisement_length;
    record->advertisement.event_type=from->advertisement_event_type;
    record->advertisement.flags=from->advertisement_flags;
    record->advertisement.reports_seen=from->reports_seen;
    bank->meta.source_now_ms=(uint64_t)clock_ms+(response_only||later_ordinary?100U:0U);
}

static void unified_all_families(const char *prefix)
{
    static const uint8_t ibeacon[27]={26,0xff,0x4c,0,2,0x15,1,2,3,4};
    static const uint8_t findmy[]={3,3,0x44,0xfd},flipper[]={3,3,0x82,0x30};
    static const uint8_t flock[]={4,0xff,0xc8,9,1},meta[]={3,0xff,0x5f,0xfd};
    static const uint8_t hc[]={6,9,'H','C','-','0','5'},ordinary[]={2,1,6};
    const struct {const uint8_t *data;size_t size;const char *preview;} cases[]={
        {ibeacon,sizeof(ibeacon),"family-ibeacon"},{findmy,sizeof(findmy),"family-findmy"},
        {flipper,sizeof(flipper),"family-flipper"},{flock,sizeof(flock),"family-flock"},
        {meta,sizeof(meta),"family-meta"},{hc,sizeof(hc),"family-hc"}};
    for(unsigned family=1U;family<CERBERUS_S3_BLE_FILTER_COUNT;++family){
        const unsigned c=family-1U;seed_native((cerberus_s3_ble_filter_t)family);
        native_tick(1000,false);clock_ms=1050;
        dual_ble_live_tick(1000,false);
        CHECK(companion_requested_mode==CERBERUS_DUAL_BLE_FAMILY&&companion_requested_family==family);
        s3_payload(0,70,1,cases[c].data,cases[c].size,false,false);
        companion_row(0,70,1,-20);c5_payload(0,cases[c].data,cases[c].size);
        companion_row(1,70,2,-40);c5_payload(1,cases[c].data,cases[c].size);
        companion_row(2,71,0,-10);c5_payload(2,cases[c].data,cases[c].size);
        memset(&companion_store.rows[2].receive,0,sizeof(companion_store.rows[2].receive));
        companion_row(3,72,0,-5);c5_payload(3,ordinary,sizeof(ordinary));
        strcpy(companion_store.rows[3].name,"Flipper HC-05"); /* Aggregate name is not raw proof. */
        companion_store.rows[3].uuid[0]=0x82;companion_store.rows[3].uuid[1]=0x30;
        companion_store.rows[3].uuid_length=2;companion_store.rows[3].manufacturer_id=0xfd5f;
        native_tick(1050,false);
        CHECK(ble_discovery_unified()&&live_view.total_count==3U&&live_view.row_count==3U);
        CHECK(ble_discovery_live->painted_source[0]==CERBERUS_BLE_DISCOVERY_LIST_C5&&
            ble_discovery_live->painted_source[1]==CERBERUS_BLE_DISCOVERY_LIST_C5&&
            ble_discovery_live->painted_source[2]==CERBERUS_BLE_DISCOVERY_LIST_S3);
        CHECK(ble_discovery_live->painted_c5[0].address_type==1U&&ble_discovery_live->painted_c5[1].address_type==2U);
        CHECK(strstr(live_badges[0],"C5 ADV HINT")&&strstr(live_details[2],"S3 PEAK"));
        CHECK(strstr(live_hint,"C5 2 RETAINED / UNCLASSIFIED 1"));
        CHECK(unified_classify_calls==4U);
        unsigned parsed=unified_classify_calls,boundaries=all_boundary_calls(),sorts=unified_sort_calls;
        for(unsigned i=0;i<4U;++i){clock_ms+=200;repaint_unified();}
        CHECK(unified_classify_calls==parsed&&unified_sort_calls==sorts&&all_boundary_calls()==boundaries);
        if(family==CERBERUS_S3_BLE_FILTER_FIND_MY)CHECK(!strcmp(live_view.tools_label,"MONITOR"));
        drawn[0]=0;CHECK(live_paint(true)==ESP_OK);save(prefix,cases[c].preview);
        /* Source-only controls do not parse, copy or request radio work. */
        handle_live_scan_tap(260,85);CHECK(live_view.total_count==1U&&!strcmp(live_view.age_label,"SOURCE S3 >"));
        handle_live_scan_tap(260,85);CHECK(live_view.total_count==2U&&!strcmp(live_view.age_label,"SOURCE C5 >"));
        CHECK(unified_classify_calls==parsed&&all_boundary_calls()==boundaries);
        if(family==CERBERUS_S3_BLE_FILTER_FLIPPER){
            cerberus_dual_ble_row_t painted=ble_discovery_live->painted_c5[0];
            cerberus_dual_ble_provenance_t proof=ble_discovery_live->painted_c5_provenance[0];
            companion_row(0,99,3,-2);++companion_store.published_boot_id;++companion_store.published_epoch;
            handle_live_scan_tap(20,135);finish_selected_handoff();CHECK(ble_mode==UI_BLE_MODE_C5&&dual_ble_ui_from_unified);
            CHECK(!memcmp(&painted,&dual_ble_ui_selected,sizeof(painted))&&
                !memcmp(&proof,&dual_ble_ui_selected_provenance,sizeof(proof)));
            actual_c5_footer_tap(250,440);CHECK(archive_save_calls==1U&&archive_save_selected);
            CHECK(!memcmp(&painted,&archive_saved_c5,sizeof(painted)));
            actual_c5_footer_tap(40,440);CHECK(ble_mode==UI_BLE_MODE_LIST);
            repaint_unified();wifi_ui_fail_fill_after=1;CHECK(live_paint(true)==ESP_FAIL);
            CHECK(!ble_discovery_live->painted_count);ble_discovery_select_painted(0);
            CHECK(ble_mode==UI_BLE_MODE_LIST&&!ble_discovery_live->selection_pending);
        }
        stop_and_release((uint64_t)clock_ms+250U);clean_live();
    }
}

static void unified_family_recency(const char *prefix)
{
    static const uint8_t findmy[]={3,3,0x44,0xfd};
    static const uint8_t ibeacon[27]={26,0xff,0x4c,0,2,0x15};
    seed_native(CERBERUS_S3_BLE_FILTER_FIND_MY);native_tick(1000,false);
    s3_payload(0,81,1,findmy,sizeof(findmy),true,false);
    companion_row(0,81,1,-20);c5_payload(0,findmy,sizeof(findmy));
    companion_store.rows[0].callback_age_ms=0;clock_ms=1100;native_tick(1100,false);
    CHECK(live_view.total_count==2U&&strstr(live_badges[1],"RSP"));
    stop_and_release(1200);clock_ms=31000;repaint_unified();
    unsigned boundaries=all_boundary_calls(),parsed=unified_classify_calls;
    const char *labels[]={"SOURCE S3 >","SOURCE C5 >","RECENT ALL >","RECENT S3 >","RECENT C5 >","SOURCE ALL >"};
    const unsigned counts[]={1,1,2,1,1,2};
    for(unsigned i=0;i<6U;++i){handle_live_scan_tap(260,85);CHECK(!strcmp(live_view.age_label,labels[i])&&live_view.total_count==counts[i]);}
    handle_live_scan_tap(260,85);handle_live_scan_tap(260,85);handle_live_scan_tap(260,85);
    CHECK(live_ble_recent_only&&live_view.total_count==2U);
    clock_ms=31001;repaint_unified();
    CHECK(live_view.total_count==1U&&ble_discovery_live->painted_source[0]==CERBERUS_BLE_DISCOVERY_LIST_S3);
    CHECK(strstr(live_badges[0],"RSP")); /* response age29901, original age30001 */
    drawn[0]=0;CHECK(live_paint(true)==ESP_OK);save(prefix,"family-recent-response-only");
    clock_ms=31101;repaint_unified();CHECK(!live_view.total_count);
    CHECK(unified_classify_calls==parsed&&all_boundary_calls()==boundaries);
    /* Unknown/future C5 source time cannot pass the recent mask. */
    companion_store.rows[0].request_started_ms=40000;clock_ms=31100;repaint_unified();
    CHECK(live_view.total_count==1U&&ble_discovery_live->painted_source[0]==CERBERUS_BLE_DISCOVERY_LIST_S3);
    CHECK(unified_classify_calls==parsed);
    companion_store.rows[0].request_started_ms=1000;companion_store.rows[0].timing_flags=0;repaint_unified();
    CHECK(live_view.total_count==1U); /* S3 exact boundary survives; C5 unknown is excluded. */
    handle_live_scan_tap(260,85); /* RECENT S3 */
    handle_live_scan_tap(100,65); /* Flipper clears unsupported recency, preserves S3 source. */
    CHECK(ble_filter==CERBERUS_S3_BLE_FILTER_FLIPPER&&!live_ble_recent_only&&
        ble_discovery_live->source_filter==CERBERUS_BLE_DISCOVERY_LIST_S3&&!live_view.total_count);
    CHECK(all_boundary_calls()==boundaries);clean_live();

    seed_native(CERBERUS_S3_BLE_FILTER_IBEACON);native_tick(1000,false);
    s3_payload(0,82,1,ibeacon,sizeof(ibeacon),false,true);
    clock_ms=1100;native_tick(1100,false);stop_and_release(1200);
    live_ble_recent_only=true;clock_ms=31000;repaint_unified();CHECK(live_view.total_count==1U);
    clock_ms=31001;repaint_unified();CHECK(!live_view.total_count);
    CHECK(ble_discovery_live->inventory.entries[0].original_source_ms==1100U&&
        ble_discovery_live->inventory.entries[0].record.focus.ibeacon_observed_ms==1000U);
    clean_live();
}

static void unified_family_cache_and_unavailable(const char *prefix)
{
    static const uint8_t flipper[]={3,3,0x82,0x30},ordinary[]={2,1,6};
    seed_native(CERBERUS_S3_BLE_FILTER_FLIPPER);native_tick(1000,false);clock_ms=1050;
    companion_row(0,90,1,-20);c5_payload(0,flipper,sizeof(flipper));native_tick(1050,false);
    CHECK(live_view.total_count==1U&&unified_classify_calls==1U);
    unsigned parsed=unified_classify_calls;
    /* Same generation cannot conceal a new full epoch/boot publication. */
    c5_payload(0,ordinary,sizeof(ordinary));++companion_store.published_epoch;
    repaint_unified();CHECK(!live_view.total_count&&unified_classify_calls==++parsed);
    c5_payload(0,flipper,sizeof(flipper));++companion_store.published_boot_id;
    repaint_unified();CHECK(live_view.total_count==1U&&unified_classify_calls==++parsed);
    companion_store.rows[0].raw_length=31U;companion_store.rows[0].raw_flags=CERBERUS_BLE_ADVERTISEMENT_TRUNCATED;
    companion_store.rows[0].receive.source_kind=CERBERUS_DUAL_BLE_RECEIVE_EXTENDED;
    companion_store.rows[0].receive.callback_length=32U;companion_store.rows[0].receive.data_status=0U;
    companion_store.rows[0].receive.primary_phy=1U;++companion_store.snapshots;
    repaint_unified();CHECK(!live_view.total_count&&strstr(live_hint,"C5 0 RETAINED / UNCLASSIFIED 1"));++parsed;
    drawn[0]=0;CHECK(live_paint(true)==ESP_OK);
    CHECK(strstr(drawn,"WAITING FOR HINTS")&&strstr(drawn,"LIVE RESULTS UPDATE AS AVAILABLE"));
    save(prefix,"family-unclassifiable-prefix");
    /* A source publication while ALL is visible invalidates cached family
     * bits even though ALL does not parse/classify its generic results. */
    ble_filter=CERBERUS_S3_BLE_FILTER_ALL;repaint_unified();
    companion_row(0,90,1,-20);c5_payload(0,flipper,sizeof(flipper));repaint_unified();
    CHECK(unified_classify_calls==parsed);
    ble_filter=CERBERUS_S3_BLE_FILTER_FLIPPER;repaint_unified();CHECK(live_view.total_count==1U&&unified_classify_calls==++parsed);
    /* OUI-only admission is an address clue, never advertised payload evidence. */
    ble_filter=CERBERUS_S3_BLE_FILTER_FLOCK;c5_payload(0,ordinary,sizeof(ordinary));
    companion_store.rows[0].address_type=0;companion_store.rows[0].address[5]=0x58;
    companion_store.rows[0].address[4]=0x8e;companion_store.rows[0].address[3]=0x81;
    repaint_unified();CHECK(live_view.total_count==1U&&strstr(live_badges[0],"C5 ADDR CLUE")&&!strstr(live_badges[0],"ADV HINT"));
    drawn[0]=0;CHECK(live_paint(true)==ESP_OK);save(prefix,"family-flock-address-clue");
    companion_store.rows[0].address_type=1;++companion_store.snapshots;repaint_unified();CHECK(!live_view.total_count);
    stop_and_release(1200);clean_live();
}

static void unified_filters_capacity_and_family(const char *prefix)
{
    seed_native(CERBERUS_S3_BLE_FILTER_ALL);native_tick(1000,false);clock_ms=1050;
    for(unsigned i=0;i<32;++i){row(i,i+1,(uint8_t)(i%4),-50);companion_row(i,i+1,(uint8_t)(i%4),-50);}
    native_tick(1050,false);
    CHECK(ble_discovery_unified()&&live_view.total_count==64&&live_view.page_count==16&&live_view.row_count==4);
    CHECK(!strcmp(live_view.age_label,"SOURCE ALL >"));
    unsigned before=all_boundary_calls(),allocated=discovery_ui_allocations;
    handle_live_scan_tap(260,60);handle_live_scan_tap(260,60); /* Source order, not cross-radio first-seen. */
    CHECK(strstr(live_view.sort_label,"SOURCE ORDER"));
    for(unsigned i=0;i<64;++i){
        CHECK(ble_discovery_live->order[i].source==(i<32?CERBERUS_BLE_DISCOVERY_LIST_S3:CERBERUS_BLE_DISCOVERY_LIST_C5));
        CHECK(ble_discovery_live->order[i].index==(uint8_t)(i%32));
    }
    for(unsigned i=0;i<15;++i)handle_live_scan_tap(180,380);
    CHECK(live_view.page==15&&live_view.row_count==4&&!live_view.has_next&&live_view.has_previous);
    for(unsigned i=0;i<4;++i){
        CHECK(ble_discovery_live->painted_source[i]==CERBERUS_BLE_DISCOVERY_LIST_C5);
        CHECK(ble_discovery_live->painted_c5_provenance[i].index==28+i);
    }
    drawn[0]=0;CHECK(live_paint(true)==ESP_OK);save(prefix,"unified-last-page");
    handle_live_scan_tap(260,85);CHECK(live_view.total_count==32&&live_view.page==0&&!strcmp(live_view.age_label,"SOURCE S3 >"));
    CHECK(ble_discovery_live->painted_source[0]==CERBERUS_BLE_DISCOVERY_LIST_S3);
    handle_live_scan_tap(260,85);CHECK(live_view.total_count==32&&!strcmp(live_view.age_label,"SOURCE C5 >"));
    CHECK(ble_discovery_live->painted_source[0]==CERBERUS_BLE_DISCOVERY_LIST_C5);
    CHECK(all_boundary_calls()==before&&discovery_ui_allocations==allocated);
    drawn[0]=0;CHECK(live_paint(true)==ESP_OK);save(prefix,"unified-c5-filter");

    /* Source preference persists; ordinary C5 AD has no iBeacon hint, and
     * the old ALL S3 inventory cannot masquerade as the new family bank. */
    handle_live_scan_tap(100,65);
    CHECK(ble_filter==CERBERUS_S3_BLE_FILTER_IBEACON&&ble_discovery_unified());
    CHECK(ble_discovery_live->source_filter==CERBERUS_BLE_DISCOVERY_LIST_C5);
    CHECK(!strcmp(live_view.age_label,"SOURCE C5 >")&&live_view.total_count==0);
    while(ble_filter!=CERBERUS_S3_BLE_FILTER_ALL)handle_live_scan_tap(100,65);
    CHECK(ble_discovery_unified()&&!strcmp(live_view.age_label,"SOURCE C5 >")&&live_view.total_count==32);
    handle_live_scan_tap(260,85);CHECK(live_view.total_count==64&&!strcmp(live_view.age_label,"SOURCE ALL >"));
    CHECK(all_boundary_calls()==before);
    /* The unified count must never masquerade as a reusable family cache.
     * Same family/epoch/revision before and after an intervening ALL view. */
    ble_filter=CERBERUS_S3_BLE_FILTER_FLIPPER;repaint_unified();CHECK(live_view.total_count==0);
    ble_filter=CERBERUS_S3_BLE_FILTER_ALL;live_page[SENTINEL_SCAN_BLE]=15;repaint_unified();CHECK(live_view.total_count==64);
    ble_filter=CERBERUS_S3_BLE_FILTER_FLIPPER;repaint_unified();
    CHECK(live_view.total_count==0&&live_view.page==0&&live_view.row_count==0);
    ble_filter=CERBERUS_S3_BLE_FILTER_ALL;repaint_unified();CHECK(live_view.total_count==64);
    ble_discovery_live->inventory.family=CERBERUS_S3_BLE_FILTER_FLIPPER;
    repaint_unified();CHECK(live_view.total_count==32&&ble_discovery_live->painted_source[0]==CERBERUS_BLE_DISCOVERY_LIST_C5);
    ble_discovery_live->inventory.family=CERBERUS_S3_BLE_FILTER_ALL;
    companion_store.error=ESP_ERR_TIMEOUT;companion_store.count=0;
    repaint_unified();CHECK(live_view.total_count==32);
    cerberus_dual_ble_view_t failed;dual_ble_ui_get_view(&failed);CHECK(failed.error==ESP_ERR_TIMEOUT);
    handle_live_scan_tap(260,85);handle_live_scan_tap(260,85);
    CHECK(live_view.total_count==0&&live_view.row_count==0&&ble_discovery_live->painted_count==0&&live_view.page==0);
    CHECK(all_boundary_calls()==before);
    stop_and_release(1200);
    for(unsigned i=0;i<32;++i)companion_row(i,i+1,(uint8_t)(i%4),-50);
    companion_store.error=ESP_OK;live_page[SENTINEL_SCAN_BLE]=3;repaint_unified();
    CHECK(ble_discovery_live->source_filter==CERBERUS_BLE_DISCOVERY_LIST_C5&&live_view.page==3);
    handle_live_scan_tap(20,135);finish_selected_handoff();CHECK(ble_mode==UI_BLE_MODE_C5&&dual_ble_ui_from_unified);
    actual_c5_footer_tap(40,440);CHECK(ble_mode==UI_BLE_MODE_LIST&&!ble_discovery_live->session.enabled);
    repaint_unified();CHECK(live_view.page==3&&ble_discovery_live->source_filter==CERBERUS_BLE_DISCOVERY_LIST_C5);
    CHECK(discovery_native.start==1);clean_live();
}

static void unified_frozen_c5_selection_save_fox(const char *prefix)
{
    seed_native(CERBERUS_S3_BLE_FILTER_ALL);native_tick(1000,false);clock_ms=1050;
    row(0,70,1,-70);row(1,71,0,-90);companion_row(0,70,1,-20);companion_row(1,70,2,-40);
    native_tick(1050,false);
    CHECK(live_view.total_count==4&&ble_discovery_live->painted_source[0]==CERBERUS_BLE_DISCOVERY_LIST_C5);
    CHECK(ble_discovery_live->painted_source[1]==CERBERUS_BLE_DISCOVERY_LIST_C5);
    CHECK(ble_discovery_live->painted_source[2]==CERBERUS_BLE_DISCOVERY_LIST_S3);
    CHECK(ble_discovery_live->painted_source[3]==CERBERUS_BLE_DISCOVERY_LIST_S3);
    drawn[0]=0;CHECK(live_paint(true)==ESP_OK);save(prefix,"unified-mixed-page");
    cerberus_dual_ble_row_t selected=ble_discovery_live->painted_c5[0];
    cerberus_dual_ble_provenance_t proof=ble_discovery_live->painted_c5_provenance[0];
    cerberus_c5_ble_result_t old_s3=ble_selected_result;
    companion_row(0,99,3,-90);++companion_store.published_epoch;++companion_store.published_boot_id;
    companion_store.start.job_id=123;companion_store.remote.epoch=456;
    companion_store.published_generation=8;companion_store.rows[0].receive.source_kind=0;
    unsigned before=all_boundary_calls();
    /* y=127 is inside the first card, not the status-line receiver switch. */
    handle_live_scan_tap(20,127);
    CHECK(ble_mode==UI_BLE_MODE_LIST&&dual_ble_ui_detail&&dual_ble_ui_from_unified);
    CHECK(!memcmp(&selected,&dual_ble_ui_selected,sizeof(selected))&&!memcmp(&proof,&dual_ble_ui_selected_provenance,sizeof(proof)));
    CHECK(!memcmp(&old_s3,&ble_selected_result,sizeof(old_s3))&&ble_discovery_live->selection_pending);
    CHECK(!ble_discovery_live->session.enabled&&!ble_discovery_requested()&&all_boundary_calls()==before);
    companion_released=false;discovery_native.hold_stop=true;
    native_tick(1100,false);native_tick(14500,false);
    CHECK(ble_mode==UI_BLE_MODE_LIST&&ble_discovery_live->selection_pending);
    repaint_unified();CHECK(ble_discovery_live->painted_count>=2U);
    handle_live_scan_tap(20,190); /* A later painted row cannot replace the pending proof. */
    CHECK(!memcmp(&selected,&dual_ble_ui_selected,sizeof(selected))&&
        !memcmp(&proof,&dual_ble_ui_selected_provenance,sizeof(proof)));
    CHECK(ble_discovery_live->selection_pending&&ble_discovery_live->selected_source==CERBERUS_BLE_DISCOVERY_LIST_C5);
    discovery_native.hold_stop=false;discovery_native_finish(true);
    stop_and_release(14750);CHECK(ble_mode==UI_BLE_MODE_LIST);
    companion_released=true;native_tick(17000,false);
    CHECK(ble_mode==UI_BLE_MODE_C5&&!ble_discovery_live->selection_pending);
    CHECK(!memcmp(&selected,&dual_ble_ui_selected,sizeof(selected))&&!memcmp(&proof,&dual_ble_ui_selected_provenance,sizeof(proof)));
    before=all_boundary_calls();
    drawn[0]=0;CHECK(actual_c5_footer_draw()==ESP_OK);CHECK(strstr(drawn,"BLE LIST")&&strstr(drawn,"SAVE SD"));
    actual_c5_footer_tap(250,440);
    CHECK(archive_save_calls==1&&archive_save_selected);
    CHECK(!memcmp(&selected,&archive_saved_c5,sizeof(selected))&&!memcmp(&proof,&archive_saved_provenance,sizeof(proof)));
    CHECK(all_boundary_calls()==before);
    companion_released=false;track_begins=0;
    dual_ble_live_tap(220,335);
    CHECK(ble_mode==UI_BLE_MODE_FOX_HUNT&&ble_fox_hunt_from_c5&&ble_fox_hunt_start_pending);
    CHECK(!memcmp(ble_fox_hunt_target.address,selected.address,6)&&ble_fox_hunt_target.address_type==1);
    CHECK(!memcmp(&old_s3,&ble_selected_result,sizeof(old_s3)));
    CHECK(!dual_ble_live_prepare_fox(17100,false)&&track_begins==0);
    stop_and_release(17200);CHECK(!dual_ble_live_prepare_fox(17500,false)&&track_begins==0);
    companion_released=true;CHECK(dual_ble_live_prepare_fox(17750,false)&&track_begins==1&&track_type==1);
    CHECK(!memcmp(track_address,selected.address,6));
    ble_fox_hunt_request_stop((uint32_t)clock_ms,false);ble_fox_hunt_tick((uint32_t)clock_ms,false);CHECK(ble_mode==UI_BLE_MODE_C5);
    dual_ble_live_tap(60,335);CHECK(ble_mode==UI_BLE_MODE_LIST&&!ble_discovery_live->session.enabled);
    repaint_unified();drawn[0]=0;CHECK(live_paint(true)==ESP_OK);save(prefix,"unified-after-c5-detail");
    CHECK(ble_discovery_live->painted_source[0]!=0);
    clean_live();
}

static void unified_age_failure_and_back(const char *prefix)
{
    seed_native(CERBERUS_S3_BLE_FILTER_ALL);native_tick(1000,false);clock_ms=1050;
    row(0,80,0,-40);companion_row(0,80,0,-20);
    companion_store.rows[0].callback_age_ms=31001;native_tick(1050,false);
    CHECK(strstr(live_badges[0],"C5 LAST")&&strstr(live_badges[0],"<=32s"));
    CHECK(strstr(live_badges[0],"RECENCY?")&&!strstr(live_badges[0],"STALE"));
    CHECK(strstr(live_badges[1],"S3 PEAK")&&!strstr(live_badges[1],"<="));
    memset(discovery_native.bank.records[0].peer.name,0,sizeof(discovery_native.bank.records[0].peer.name));
    ble_discovery_live->inventory.entries[0].record.peer.name[0]=0;
    companion_store.rows[0].name[0]=0;masked=true;repaint_unified();
    CHECK(!strcmp(live_names[0],"Unnamed device")&&!strcmp(live_names[1],"Unnamed device"));
    drawn[0]=0;CHECK(live_paint(true)==ESP_OK);save(prefix,"unified-age-semantics");
    unsigned before=all_boundary_calls();
    /* Failed full or incremental rendering must revoke every card tap even
     * when a prior screen authorized the same row position. */
    wifi_ui_fail_fill_after=1;CHECK(live_paint(true)==ESP_FAIL);
    CHECK(ble_discovery_live->painted_count==0);
    ble_discovery_select_painted(0);CHECK(ble_mode==UI_BLE_MODE_LIST&&!ble_discovery_live->selection_pending);
    CHECK(all_boundary_calls()==before);repaint_unified();
    companion_store.rows[0].timing_flags=0;live_make_view();
    wifi_ui_fail_fill_after=1;CHECK(live_paint(false)==ESP_FAIL&&ble_discovery_live->painted_count==0);
    ble_discovery_select_painted(0);CHECK(ble_mode==UI_BLE_MODE_LIST&&!ble_discovery_live->selection_pending);
    repaint_unified();CHECK(strstr(live_badges[0],"AGE UNKNOWN"));
    handle_live_scan_tap(260,85);CHECK(ble_discovery_live->source_filter==CERBERUS_BLE_DISCOVERY_LIST_S3);
    CHECK(ble_discovery_live->painted_source[0]==CERBERUS_BLE_DISCOVERY_LIST_S3);
    cerberus_s3_ble_discovery_entry_t frozen=ble_discovery_live->painted[0];
    companion_store.rows[0].address_type=3;
    handle_live_scan_tap(20,135);CHECK(ble_discovery_live->selection_pending);
    CHECK(!memcmp(&frozen,&ble_discovery_live->selected,sizeof(frozen))&&ble_mode==UI_BLE_MODE_LIST);
    actual_back(440);CHECK(ble_mode==UI_BLE_MODE_MENU&&!ble_discovery_live->selection_pending);
    for(unsigned i=0;i<8;++i)native_tick(1200+i*250,false);
    CHECK(cerberus_ble_discovery_session_released(&ble_discovery_live->session));
    clean_live();
}

static void unified_committed_order_cache(void)
{
    seed_native(CERBERUS_S3_BLE_FILTER_ALL);native_tick(1000,false);clock_ms=1050;
    row(0,41,0,-60);companion_row(0,42,1,-40);companion_row(1,43,2,-80);native_tick(1050,false);
    CHECK(ble_discovery_live->order[0].source==CERBERUS_BLE_DISCOVERY_LIST_C5&&ble_discovery_live->order[0].index==0);
    unsigned sorts=unified_sort_calls,boundaries=all_boundary_calls();
    for(unsigned i=0;i<6;++i){clock_ms+=200;repaint_unified();}
    CHECK(unified_sort_calls==sorts&&all_boundary_calls()==boundaries);
    companion_store.rows[1].rssi=-10;++companion_store.snapshots;repaint_unified();
    CHECK(unified_sort_calls==++sorts&&ble_discovery_live->order[0].index==1);
    ++companion_store.published_epoch;repaint_unified();CHECK(unified_sort_calls==++sorts);
    ++companion_store.published_boot_id;repaint_unified();CHECK(unified_sort_calls==++sorts);
    /* Exercise the actual S3 inventory application before validating the
     * revision key, rather than assigning a fake second ordering algorithm. */
    row(0,41,0,-5);cerberus_ble_discovery_observation_t copied;
    CHECK(ble_discovery_copy(ble_discovery_live,ble_discovery_live->session.epoch,0,&copied).error==ESP_OK);
    repaint_unified();CHECK(unified_sort_calls==++sorts&&ble_discovery_live->order[0].source==CERBERUS_BLE_DISCOVERY_LIST_S3);
    companion_store.snapshots=UINT32_MAX;
    repaint_unified();CHECK(unified_sort_calls==++sorts);repaint_unified();CHECK(unified_sort_calls==++sorts);
    companion_store.snapshots=0;ble_discovery_live->inventory.revision=UINT64_MAX;
    repaint_unified();CHECK(unified_sort_calls==++sorts);repaint_unified();CHECK(unified_sort_calls==++sorts);
    stop_and_release(3000);clean_live();
}

static void empty_receive_explanation(void)
{
    sentinel_scan_view_t view={.kind=SENTINEL_VIEW_BLE,.state=SENTINEL_VIEW_LIVE};
    drawn[0]=0;CHECK(sentinel_scan_view_draw(&view,0)==ESP_OK);
    CHECK(strstr(drawn,"LIVE RESULTS UPDATE AS AVAILABLE"));
    CHECK(!strstr(drawn,"RESULTS APPEAR WHEN THE SWEEP FINISHES"));
    view.kind=SENTINEL_VIEW_WIFI;
    drawn[0]=0;CHECK(sentinel_scan_view_draw(&view,0)==ESP_OK);
    CHECK(strstr(drawn,"RESULTS APPEAR WHEN THE SWEEP FINISHES"));
    CHECK(!strstr(drawn,"LIVE RESULTS UPDATE AS AVAILABLE"));
}

static void unified_full_original_and_focus(const char *prefix)
{
    seed_native(CERBERUS_S3_BLE_FILTER_FLIPPER);native_tick(1000,false);
    dual_ble_live_tick(1000,false);
    CHECK(companion_requested_mode==CERBERUS_DUAL_BLE_FAMILY&&companion_requested_family==3U);
    companion_row(0,87,2,-35);
    cerberus_dual_ble_row_t *row=&companion_store.rows[0];
    uint8_t full[255]={250,0xff};full[251]=3;full[252]=3;full[253]=0x82;full[254]=0x30;
    memcpy(row->raw,full,31U);row->raw_length=31U;row->raw_flags=3U;
    row->receive=(cerberus_dual_ble_receive_metadata_t){
        .source_kind=CERBERUS_DUAL_BLE_RECEIVE_EXTENDED,.callback_length=255U,
        .sid=255U,.primary_phy=1U,.ad_validation=CERBERUS_DUAL_BLE_AD_VALID};
    cerberus_dual_ble_family_evidence_t evidence;
    CHECK(cerberus_dual_ble_family_parse_ad(full,sizeof(full),row->address,row->address_type,&evidence));
    CHECK(cerberus_dual_ble_family_wire_encode(&evidence,3U,&row->family));
    ++companion_store.snapshots;clock_ms=1100;native_tick(1100,false);
    CHECK(live_view.total_count==1U&&strstr(live_badges[0],"C5 FULL HINT"));
    save(prefix,"family-full255-list");
    cerberus_dual_ble_row_t frozen=ble_discovery_live->painted_c5[0];
    cerberus_dual_ble_provenance_t proof=ble_discovery_live->painted_c5_provenance[0];
    memset(&row->family,0,sizeof(row->family));++companion_store.snapshots;
    ++companion_store.published_boot_id;++companion_store.published_epoch;
    handle_live_scan_tap(20,135);finish_selected_handoff();
    CHECK(ble_mode==UI_BLE_MODE_C5&&dual_ble_ui_from_unified);
    CHECK(!memcmp(&frozen,&dual_ble_ui_selected,sizeof(frozen))&&
        !memcmp(&proof,&dual_ble_ui_selected_provenance,sizeof(proof)));
    actual_c5_footer_tap(250,440);CHECK(archive_save_calls==1U&&archive_save_selected);
    CHECK(!memcmp(&archive_saved_c5,&frozen,sizeof(frozen)));
    actual_c5_footer_tap(40,440);CHECK(ble_mode==UI_BLE_MODE_LIST);
    repaint_unified();CHECK(!live_view.total_count); /* Old truncated row has no full evidence. */
    companion_store.rows[0]=frozen;++companion_store.snapshots;
    companion_store.start.mode=CERBERUS_DUAL_BLE_FAMILY;companion_store.start.reserved=3U;
    companion_store.error=ESP_ERR_NOT_SUPPORTED;repaint_unified();
    CHECK(live_view.total_count==1U&&strstr(live_subtitle,"C5 ESP_ERR_NOT_SUPPOR"));
    CHECK(strstr(live_hint,"C5 1 RETAINED"));save(prefix,"family-focus-unavailable-retained");
    unsigned parsed=unified_classify_calls,boundaries=all_boundary_calls();
    clock_ms=1200;repaint_unified();
    CHECK(unified_classify_calls==parsed&&all_boundary_calls()==boundaries);
    /* Sleep, another passive owner and leaving the page all request normal
     * companion cleanup, rather than creating a focused job in the background. */
    dual_ble_live_tick(1300,true);CHECK(companion_requested_mode==-1);
    analyzer_wifi_busy=true;dual_ble_live_tick(1400,false);CHECK(companion_requested_mode==-1);
    analyzer_wifi_busy=false;ble_discovery_leave();dual_ble_live_tick(1500,false);
    CHECK(companion_requested_mode==-1);
    stop_and_release(1600);clean_live();
}

#include "dual_ble_response_ui_fixture.h"
static void unified_response_families(const char *prefix)
{
    const uint8_t beacon[27]={26,0xff,0x4c,0,2,0x15,1,2,3,4};
    const uint8_t findmy[]={3,3,0x44,0xfd},flipper[]={3,3,0x82,0x30};
    const uint8_t flock[]={4,0xff,0xc8,9,1},meta[]={3,0xff,0x5f,0xfd},hc[]={6,9,'H','C','-','0','5'};
    const struct {const uint8_t *data;size_t length;} cases[]={
        {beacon,sizeof(beacon)},{findmy,sizeof(findmy)},{flipper,sizeof(flipper)},
        {flock,sizeof(flock)},{meta,sizeof(meta)},{hc,sizeof(hc)}};
    for(unsigned family=1;family<=6;++family){
        seed_native((cerberus_s3_ble_filter_t)family);native_tick(1000,false);
        companion_row(0,90,1,-20);companion_row(1,90,2,-40);
        for(unsigned i=0;i<2;++i){
            fixture_response_row(&companion_store.rows[i],cases[family-1].data,cases[family-1].length,
                (uint8_t)family,1000);companion_store.rows[i].address_type=(uint8_t)(i+1);
            companion_store.rows[i].rssi=(int8_t)(-20-(int)i*20);
        }
        ++companion_store.snapshots;clock_ms=1100;native_tick(1100,false);
        CHECK(live_view.total_count==2&&ble_discovery_live->painted_count==2);
        CHECK(strstr(live_badges[0],"C5 RSP HINT")&&!strstr(live_badges[0],"FULL HINT"));
        CHECK(ble_discovery_live->painted_c5[0].address_type==1&&ble_discovery_live->painted_c5[1].address_type==2);
        CHECK(ble_discovery_live->c5_family_bits[0]==0&&ble_discovery_live->c5_response_bits[0]&(1U<<(family-1)));
        unsigned parsed=unified_classify_calls,rsp_parsed=unified_response_classify_calls,boundaries=all_boundary_calls();
        clock_ms=1200;repaint_unified();CHECK(unified_classify_calls==parsed&&unified_response_classify_calls==rsp_parsed);
        CHECK(all_boundary_calls()==boundaries);
        if(cerberus_s3_ble_recent_supported((cerberus_s3_ble_filter_t)family)){
            companion_store.rows[0].callback_age_ms=UINT32_MAX;companion_store.rows[0].timing_flags=0;
            companion_store.rows[1].response.age_ms=UINT32_MAX;
            companion_store.rows[1].response.flags&=(uint8_t)~CERBERUS_DUAL_BLE_RESPONSE_AGE_VALID;
            ++companion_store.snapshots;live_ble_recent_only=true;clock_ms=30900;repaint_unified();
            CHECK(live_view.total_count==1); /* Response bound exactly30,000ms; unknown excluded. */
            clock_ms=30901;repaint_unified();CHECK(live_view.total_count==0);
            live_ble_recent_only=false;repaint_unified();CHECK(live_view.total_count==2);
        }
        if(family==6){
            memcpy(companion_store.rows[0].response.name,"HC-05",5);companion_store.rows[0].response.name_length=5;
            companion_store.rows[0].response.flags|=CERBERUS_DUAL_BLE_RESPONSE_NAME_COMPLETE;
            companion_store.rows[0].response.age_ms=100;companion_store.rows[0].response.flags|=CERBERUS_DUAL_BLE_RESPONSE_AGE_VALID;
            companion_store.rows[0].timing_flags=3;companion_store.rows[0].callback_age_ms=900;
            ++companion_store.snapshots;clock_ms=1100;repaint_unified();
            CHECK(!strcmp(live_names[0],"HC-05")&&strstr(live_details[0],"RSP NAME"));
            save(prefix,"response-family-name-list");
            cerberus_dual_ble_row_t frozen=ble_discovery_live->painted_c5[0];
            cerberus_dual_ble_provenance_t proof=ble_discovery_live->painted_c5_provenance[0];
            memset(&companion_store.rows[0].response,0,sizeof(companion_store.rows[0].response));
            ++companion_store.published_epoch;++companion_store.snapshots;
            handle_live_scan_tap(20,135);finish_selected_handoff();CHECK(ble_mode==UI_BLE_MODE_C5);
            CHECK(!memcmp(&frozen,&dual_ble_ui_selected,sizeof(frozen))&&!memcmp(&proof,&dual_ble_ui_selected_provenance,sizeof(proof)));
            actual_c5_footer_tap(250,440);CHECK(archive_save_calls==1&&!memcmp(&frozen,&archive_saved_c5,sizeof(frozen)));
            actual_c5_footer_tap(40,440);CHECK(ble_mode==UI_BLE_MODE_LIST);
            repaint_unified();CHECK(live_view.total_count==1);
        }
        stop_and_release((uint64_t)clock_ms+1000);clean_live();
    }
    seed_native(CERBERUS_S3_BLE_FILTER_ALL);native_tick(1000,false);
    companion_row(0,80,1,-20);fixture_response_row(&companion_store.rows[0],hc,sizeof(hc),0,1000);
    memcpy(companion_store.rows[0].response.name,"HC-05",5);companion_store.rows[0].response.name_length=5;
    ++companion_store.snapshots;clock_ms=1100;native_tick(1100,false);
    CHECK(!strcmp(live_names[0],"HC-05")&&strstr(live_details[0],"RSP NAME")&&strstr(live_badges[0],"C5 LAST"));
    dual_ble_ui_response_requests=true;companion_store.start.mode=CERBERUS_DUAL_BLE_DISCOVERY;
    repaint_unified();CHECK(strstr(live_subtitle,"MODE CHANGING")&&strstr(live_subtitle,"RETAINED PASSIVE"));
    companion_store.start.mode=CERBERUS_DUAL_BLE_RESPONSE_DISCOVERY;companion_store.error=ESP_ERR_NOT_SUPPORTED;
    repaint_unified();CHECK(strstr(live_subtitle,"C5 ESP_ERR_NOT_SUPPOR")&&live_view.total_count==1);
    save(prefix,"response-unavailable-retained-list");
    companion_store.error=ESP_ERR_NO_MEM;repaint_unified();CHECK(strstr(live_subtitle,"C5 ESP_ERR_NO_MEM"));
    unsigned before=all_boundary_calls();
    handle_live_scan_tap(20,113);CHECK(ble_mode==UI_BLE_MODE_C5);
    dual_ble_live_tap(200,65);CHECK(!dual_ble_ui_response_requests&&all_boundary_calls()==before);
    dual_ble_live_tap(30,65);CHECK(ble_mode==UI_BLE_MODE_LIST);
    stop_and_release(2100);clean_live();
}

static cerberus_c5_ble_result_t fox_target(void)
{
    cerberus_c5_ble_result_t target={.address={1,2,3,4,5,6},.address_type=2};
    return target;
}

static void fox_seed(void)
{
    seed_native(CERBERUS_S3_BLE_FILTER_ALL);
    ble_mode=UI_BLE_MODE_DETAIL;
    ble_fox_hunt_exit_pending=ble_fox_hunt_start_pending=ble_fox_hunt_retry_requested=false;
    ble_fox_hunt_s3_started=ble_fox_hunt_s3_attached=false;
    ble_fox_hunt_s3_owned=ble_fox_hunt_s3_receiving_cached=false;
    ble_fox_hunt_s3_start_requested_ms=0U;
    ble_fox_hunt_s3_start_pending=ble_fox_hunt_cleanup_timed_out=false;
    ble_fox_hunt_error=ble_fox_hunt_barrier_error=ESP_OK;
    fox_status_error=fox_stop_error=fox_start_error=track_error=ESP_OK;
    fox_hold_running=hold_stop=false;
    track_begins=track_ends=passive_starts=0;
    fixture_detail_draws=0;
    companion_released=true;companion_retries=companion_syncs=0;
    companion_requested_mode=-1;dual_ble_ui_mode=-1;dual_ble_ui_error=ESP_OK;
    memset(&companion_store,0,sizeof(companion_store));dual_ble_ui_store=&companion_store;
    memset(&scan,0,sizeof(scan));++fox_host_epoch;
    learning_tool_active=false;ble_attack_selecting=false;analyzer_wifi_busy=false;
}

static void fox_step(uint64_t now,bool asleep)
{
    clock_ms=(int64_t)now;
    /* The real UI loop services C5 before the Fox/S3 lifecycle. */
    dual_ble_live_tick(now,asleep);
    ble_fox_hunt_tick((uint32_t)now,asleep);
}

static void fox_finish(uint64_t now)
{
    fox_status_error=fox_stop_error=ESP_OK;fox_hold_running=hold_stop=false;
    owner=CERBERUS_S3_BLE_OWNER_NONE;scan.state=CERBERUS_BLE_SCAN_COMPLETE;
    companion_released=true;
    clock_ms=(int64_t)now;ble_fox_hunt_request_stop((uint32_t)now,false);
    fox_step(now+250U,false);
    CHECK(!ble_fox_hunt_exit_pending&&!ble_fox_hunt_s3_started&&!ble_fox_hunt_s3_attached);
    clean_live();
}

static void fox_hold_companion(void)
{
    companion_released=false;
    companion_store.start.mode=CERBERUS_DUAL_BLE_TRACK;
    companion_store.start.address_type=ble_fox_hunt_target.address_type;
    memcpy(companion_store.start.address,ble_fox_hunt_target.address,6U);
}

static void fox_independent_failure_and_retry(void)
{
    fox_seed();cerberus_c5_ble_result_t target=fox_target();
    track_error=ESP_ERR_NO_MEM;ble_fox_hunt_open_peer(&target,true);
    fox_step(1100,false);fox_step(1400,false);
    CHECK(!ble_fox_hunt_start_pending&&!ble_fox_hunt_s3_attached&&track_begins==1);
    CHECK(ble_fox_hunt_error==ESP_ERR_NO_MEM&&passive_starts==0);
    CHECK(companion_requested_mode==CERBERUS_DUAL_BLE_TRACK&&dual_ble_ui_type==target.address_type);
    CHECK(!memcmp(dual_ble_ui_target,target.address,6)&&ble_fox_hunt_s3_retryable());
    track_error=ESP_OK;unsigned c5_retries=companion_retries;
    handle_ble_tap(40,365);CHECK(ble_fox_hunt_retry_requested);
    fox_step(1700,false);
    CHECK(track_begins==2&&passive_starts==1&&ble_fox_hunt_s3_attached&&ble_fox_hunt_s3_started);
    CHECK(companion_retries==c5_retries&&companion_requested_mode==CERBERUS_DUAL_BLE_TRACK);
    unsigned begun=track_begins,started=passive_starts,stopped=stops;
    dual_ble_ui_error=ESP_ERR_TIMEOUT;
    CHECK(ble_fox_hunt_c5_retryable());handle_ble_tap(200,365);
    CHECK(companion_retries==c5_retries+1&&track_begins==begun&&passive_starts==started&&stops==stopped);
    CHECK(ble_fox_hunt_s3_started&&!ble_fox_hunt_retry_requested);
    dual_ble_ui_type^=1;CHECK(!ble_fox_hunt_c5_retryable());dual_ble_ui_type^=1;
    dual_ble_ui_error=ESP_OK;fox_finish(2000);

    fox_seed();fox_start_error=ESP_ERR_NO_MEM;ble_fox_hunt_open_peer(&target,false);
    fox_step(1100,false);fox_step(1400,false);
    CHECK(!ble_fox_hunt_s3_attached&&!ble_fox_hunt_s3_started&&passive_starts==1&&track_ends==1);
    CHECK(companion_requested_mode==CERBERUS_DUAL_BLE_TRACK&&ble_fox_hunt_error==ESP_ERR_NO_MEM);
    fox_start_error=ESP_OK;handle_ble_tap(40,365);fox_step(1700,false);
    CHECK(track_begins==2&&track_ends==1&&passive_starts==2&&ble_fox_hunt_s3_started);
    /* Completed own sweep restarts; an asynchronous scan error requires retry. */
    owner=CERBERUS_S3_BLE_OWNER_NONE;scan.state=CERBERUS_BLE_SCAN_COMPLETE;
    fox_step(2000,false);CHECK(passive_starts==3&&ble_fox_hunt_s3_started);
    owner=CERBERUS_S3_BLE_OWNER_NONE;scan.state=CERBERUS_BLE_SCAN_ERROR;scan.error_code=ESP_ERR_TIMEOUT;
    fox_step(2300,false);CHECK(passive_starts==3&&ble_fox_hunt_error==ESP_ERR_TIMEOUT);
    handle_ble_tap(40,365);fox_step(2600,false);
    CHECK(passive_starts==4&&ble_fox_hunt_error==ESP_OK&&companion_retries==0);
    fox_finish(2900);
}

static void fox_barrier_reentry_and_foreign_owner(void)
{
    fox_seed();cerberus_c5_ble_result_t target=fox_target(),other=target;other.address_type=3;
    companion_released=false;ble_fox_hunt_open_peer(&target,true);
    ble_fox_hunt_open_peer(&other,false);
    CHECK(ble_fox_hunt_target.address_type==2&&ble_fox_hunt_from_c5);
    fox_step(13500,false);
    CHECK(ble_fox_hunt_start_pending&&ble_fox_hunt_barrier_error==ESP_ERR_TIMEOUT&&ble_fox_hunt_error==ESP_OK);
    CHECK(!track_begins&&!passive_starts&&companion_requested_mode==-1);
    CHECK(fixture_detail_draws==1);
    /* Actual retry clears the visible timeout. A second held deadline must
     * repaint TIMEOUT again, without starting either receiver. */
    handle_ble_tap(40,365);CHECK(fixture_detail_draws==2&&ble_fox_hunt_barrier_error==ESP_OK);
    fox_step(25800,false);
    CHECK(fixture_detail_draws==3&&ble_fox_hunt_barrier_error==ESP_ERR_TIMEOUT);
    CHECK(!track_begins&&!passive_starts&&companion_requested_mode==-1);
    fox_step(26100,false);CHECK(fixture_detail_draws==3);
    companion_released=true;fox_step(26400,false);CHECK(!track_begins);
    handle_ble_tap(40,365);fox_step(26700,false);fox_step(27000,false);
    CHECK(track_begins==1&&ble_fox_hunt_s3_started&&companion_requested_mode==CERBERUS_DUAL_BLE_TRACK);
    unsigned starts_before=passive_starts;
    ble_fox_hunt_open_peer(&other,false);
    CHECK(ble_fox_hunt_target.address_type==2&&ble_fox_hunt_s3_started&&passive_starts==starts_before);
    fox_finish(27300);

    fox_seed();ble_fox_hunt_open_peer(&target,false);
    CHECK(dual_ble_live_prepare_fox(1100,false)&&ble_fox_hunt_s3_attached);
    owner=CERBERUS_S3_BLE_OWNER_CENTRAL;fox_step(1400,false);
    CHECK(ble_fox_hunt_error==ESP_OK&&passive_starts==1&&!stops&&
        ble_fox_hunt_s3_start_pending&&!ble_fox_hunt_s3_owned&&!ble_fox_hunt_s3_started);
    CHECK(companion_requested_mode==CERBERUS_DUAL_BLE_TRACK);
    handle_ble_tap(40,365);CHECK(!ble_fox_hunt_retry_requested);fox_step(13700,false);
    CHECK(ble_fox_hunt_error==ESP_ERR_TIMEOUT&&!ble_fox_hunt_retry_requested&&!stops);
    handle_ble_tap(160,405);fox_step(14000,false);
    CHECK(ble_mode==UI_BLE_MODE_DETAIL&&owner==CERBERUS_S3_BLE_OWNER_CENTRAL&&!stops);
    clean_live();

    /* A replacement SCANNER generation or reset epoch is not our sweep. */
    for(unsigned changed=0;changed<2;++changed){
        fox_seed();ble_fox_hunt_open_peer(&target,false);fox_step(1100,false);
        CHECK(ble_fox_hunt_s3_started);
        if(changed)++fox_host_epoch;else ++scan.generation;
        handle_ble_tap(160,405);fox_step(1400,false);
        CHECK(!stops&&owner==CERBERUS_S3_BLE_OWNER_SCANNER&&ble_mode==UI_BLE_MODE_DETAIL);
        clean_live();
    }
}

static void fox_stop_retains_cleanup_and_clock(void)
{
    fox_seed();cerberus_c5_ble_result_t target=fox_target();
    ble_fox_hunt_open_peer(&target,true);fox_step(1100,false);fox_step(1400,false);
    fox_hold_companion();fox_stop_error=ESP_FAIL;
    const uint64_t wrap=(UINT64_C(1)<<32)+2000U;
    unsigned syncs_before=companion_syncs;
    clock_ms=(int64_t)wrap;handle_ble_tap(160,405);
    CHECK(ble_fox_hunt_exit_pending&&!ble_fox_hunt_retry_requested&&companion_requested_mode==-1);
    CHECK(companion_syncs==syncs_before); /* Stop changes intent without a second SPI tick. */
    fox_step(wrap+250U,false);unsigned tries=stops;
    CHECK(companion_sync_clock==wrap+250U); /* The normal coordinator keeps its full clock. */
    CHECK(tries&&ble_fox_hunt_s3_started);
    fox_step(wrap+12250U,false);
    CHECK(ble_fox_hunt_cleanup_timed_out&&ble_fox_hunt_exit_pending&&ble_mode==UI_BLE_MODE_FOX_HUNT);
    CHECK(stops>tries&&track_ends==0);
    unsigned begun=track_begins,started=passive_starts,c5retry=companion_retries;
    handle_ble_tap(40,365);handle_ble_tap(200,365);
    CHECK(track_begins==begun&&passive_starts==started&&companion_retries==c5retry);
    fox_status_error=ESP_ERR_TIMEOUT;tries=stops;fox_step(wrap+12500U,false);
    CHECK(stops>tries&&ble_fox_hunt_exit_pending); /* Exact Stop is safe even if its state query failed. */
    fox_status_error=fox_stop_error=ESP_OK;fox_hold_running=true;fox_step(wrap+12750U,false);
    CHECK(stops>tries&&ble_fox_hunt_exit_pending);
    fox_hold_running=false;owner=CERBERUS_S3_BLE_OWNER_NONE;scan.state=CERBERUS_BLE_SCAN_COMPLETE;
    fox_step(wrap+13000U,false);CHECK(ble_fox_hunt_exit_pending&&track_ends==1&&!ble_fox_hunt_s3_attached);
    companion_released=true;fox_step(wrap+13250U,false);
    CHECK(!ble_fox_hunt_exit_pending&&ble_mode==UI_BLE_MODE_C5&&track_ends==1&&passive_starts==started);
    clean_live();

    /* Retry cleanup also stays owned after timeout, without a late rearm. */
    fox_seed();ble_fox_hunt_open_peer(&target,false);fox_step(1100,false);
    ble_fox_hunt_error=ESP_FAIL;fox_stop_error=ESP_FAIL;handle_ble_tap(40,365);
    fox_step(13500,false);CHECK(!ble_fox_hunt_retry_requested&&ble_fox_hunt_error==ESP_FAIL);
    tries=stops;fox_step(13800,false);CHECK(stops>tries);
    fox_stop_error=ESP_OK;owner=CERBERUS_S3_BLE_OWNER_NONE;scan.state=CERBERUS_BLE_SCAN_COMPLETE;
    fox_step(14100,false);CHECK(!ble_fox_hunt_retry_requested&&passive_starts==1&&ble_fox_hunt_s3_retryable());
    handle_ble_tap(40,365);fox_step(14400,false);CHECK(passive_starts==2&&track_begins==2);
    fox_finish(14700);
}

static void fox_back_sleep_and_pending_stop(void)
{
    cerberus_c5_ble_result_t target=fox_target(),other=target;other.address[0]=99;
    for(unsigned route=0;route<3;++route){
        fox_seed();ble_fox_hunt_open_peer(&target,false);fox_step(1100,false);
        fox_hold_running=true;fox_hold_companion();
        if(route==0)ble_attack_selecting=true;
        if(route==1){learning_tool_active=true;tool_parent_page=UI_PAGE_OTHER;}
        actual_back(UI_BACK_Y-1U);CHECK(!ble_fox_hunt_exit_pending);
        clock_ms=1400;actual_back(UI_BACK_Y);
        CHECK(ble_fox_hunt_exit_pending&&companion_requested_mode==-1&&!ble_fox_hunt_retry_requested);
        ui_page_t destination=ble_session_exit_page;clock_ms=1500;actual_back(UI_BACK_Y+UI_BACK_HEIGHT-1U);
        CHECK(ble_fox_hunt_exit_started_ms==1400&&ble_session_exit_page==destination);
        fox_step(13700,false);CHECK(ble_fox_hunt_exit_pending&&ble_fox_hunt_cleanup_timed_out);
        fox_hold_running=false;owner=CERBERUS_S3_BLE_OWNER_NONE;scan.state=CERBERUS_BLE_SCAN_COMPLETE;
        companion_released=true;fox_step(14000,false);
        CHECK(!ble_fox_hunt_exit_pending&&track_ends==1&&passive_starts==1);
        CHECK(current_page==(route==0?UI_PAGE_BLE_ATTACK:route==1?UI_PAGE_OTHER:UI_PAGE_BLE));
        clean_live();
    }
    /* Cancel the desired C5 request even when INFO has no acquired job. A
     * rejected reentry cannot replace target A with B while cleanup is owed. */
    for(unsigned asleep=0;asleep<2;++asleep){
        fox_seed();ble_fox_hunt_open_peer(&target,true);
        if(!asleep){CHECK(dual_ble_live_prepare_fox(1100,false));dual_ble_live_tick(1100,false);}
        ble_fox_hunt_open_peer(&other,false);CHECK(ble_fox_hunt_target.address[0]==target.address[0]);
        unsigned attached=track_begins;clock_ms=1400;
        if(!asleep)handle_ble_tap(160,405);
        fox_step(1400,asleep!=0);
        CHECK(companion_requested_mode==-1&&!ble_fox_hunt_exit_pending&&!passive_starts);
        CHECK(track_begins==attached&&ble_mode==UI_BLE_MODE_C5);
        fox_step(1700,false);CHECK(!passive_starts&&track_begins==attached&&companion_requested_mode==-1);
        clean_live();
    }
}

static void shared_monitor_acquisition_and_frozen_selection(void)
{
    static const uint8_t findmy[]={3,3,0x44,0xfd};
    const uint64_t base=UINT64_C(0x100000000)+1000U;
    seed_native_stopped(CERBERUS_S3_BLE_FILTER_FLIPPER);
    ble_mode=UI_BLE_MODE_FINDMY_MONITOR;ble_findmy_running=ble_findmy_available=true;
    CHECK(ble_discovery_page()&&!ble_discovery_presenting()&&!ble_discovery_unified());
    CHECK(ble_discovery_acquire()&&!ble_discovery_live->session.enabled);
    CHECK(ble_discovery_family()==CERBERUS_S3_BLE_FILTER_FIND_MY);
    CHECK(ble_discovery_monitor_start()==ESP_OK);
    clock_ms=(int64_t)base;unsigned frame=live_frame;
    ble_discovery_tick(base,false);
    CHECK(discovery_native.start==1&&discovery_native.bank.meta.family==CERBERUS_S3_BLE_FILTER_FIND_MY);
    CHECK(live_frame==frame&&ble_discovery_live->painted_count==0U);
    dual_ble_live_tick(base,false);
    CHECK(companion_requested_mode==CERBERUS_DUAL_BLE_FAMILY&&companion_requested_family==2U);
    clock_ms=(int64_t)base+50;s3_payload(0,41,1,findmy,sizeof(findmy),true,false);
    clock_ms=(int64_t)base+150;ble_discovery_tick(base+150U,false);
    companion_row(0,41,1,-30);
    fixture_response_row(&companion_store.rows[0],findmy,sizeof(findmy),2U,base);
    ++companion_store.snapshots;
    CHECK(ble_discovery_combined_order()==2U); /* Typed sources remain distinct. */
    CHECK(ble_discovery_live->c5_response_bits[0]&CERBERUS_DUAL_BLE_FAMILY_FIND_MY);
    live_ble_recent_only=true;clock_ms=(int64_t)base+40000;
    CHECK(ble_discovery_combined_order()==2U); /* List-only recent toggle is not monitor policy. */
    ble_discovery_live->source_filter=CERBERUS_BLE_DISCOVERY_LIST_S3;
    CHECK(ble_discovery_combined_order()==1U);
    CHECK(ble_discovery_build_unified_row(0,0,(uint64_t)clock_ms));
    CHECK(!ble_discovery_build_unified_row(1,1,(uint64_t)clock_ms));
    const unsigned classifies=unified_classify_calls;
    cerberus_s3_ble_discovery_entry_t different_hint=ble_discovery_live->inventory.entries[0];
    different_hint.record.focus.discovery.findmy_advertisement=(cerberus_ble_findmy_hints_t){CERBERUS_BLE_FINDMY_HINT_DULT,0};
    different_hint.record.focus.discovery.findmy_scan_response.bits=0;
    different_hint.record.focus.discovery.advertisement_observed_ms=(uint32_t)(base+150U);
    CHECK(ble_discovery_monitor_hint_recent(&different_hint,base+120149U));
    CHECK(!ble_discovery_monitor_hint_recent(&different_hint,base+120150U));
    CHECK(!ble_discovery_monitor_hint_recent(&different_hint,base+UINT64_C(0x100000000)+149U));
    clock_ms=(int64_t)base+120149;CHECK(ble_discovery_combined_order()==1U);
    clock_ms=(int64_t)base+120150;CHECK(ble_discovery_combined_order()==0U);
    clock_ms=(int64_t)base+UINT64_C(0x100000000)+150U;CHECK(ble_discovery_combined_order()==0U);
    CHECK(unified_classify_calls==classifies); /* No C5 raw reparse on age repaint. */
    clock_ms=(int64_t)base+40000;CHECK(ble_discovery_combined_order()==1U);
    uint8_t hint=ble_discovery_live->inventory.entries[0].record.focus.discovery.findmy_scan_response.bits;
    ble_discovery_live->inventory.entries[0].record.focus.discovery.findmy_scan_response.bits=0U;
    ++ble_discovery_live->inventory.revision;
    CHECK(ble_discovery_combined_order()==0U); /* Category alone is not Find My evidence. */
    ble_discovery_live->inventory.entries[0].record.focus.discovery.findmy_scan_response.bits=hint;
    ++ble_discovery_live->inventory.revision;CHECK(ble_discovery_combined_order()==1U);
    ++ble_discovery_live->inventory.entries[0].record.focus.discovery.advertisement_observed_ms;
    ++ble_discovery_live->inventory.revision;CHECK(ble_discovery_combined_order()==0U);
    --ble_discovery_live->inventory.entries[0].record.focus.discovery.advertisement_observed_ms;
    ++ble_discovery_live->inventory.revision;CHECK(ble_discovery_combined_order()==1U);
    CHECK(ble_discovery_build_unified_row(0,0,(uint64_t)clock_ms));
    CHECK(!ble_discovery_monitor_freeze_selection(0)); /* Built is not successfully painted. */
    drawn[0]=0;wifi_ui_fail_fill_after=1;
    CHECK(draw_ble_findmy_monitor()==ESP_FAIL&&ble_discovery_live->painted_count==0U);
    CHECK(!ble_discovery_monitor_freeze_selection(0));
    drawn[0]=0;CHECK(draw_ble_findmy_monitor()==ESP_OK&&ble_discovery_live->painted_count==1U);
    cerberus_s3_ble_discovery_entry_t frozen=ble_discovery_live->painted[0];
    CHECK(frozen.original_source_ms==base+50U&&frozen.epoch!=0U);
    CHECK(ble_discovery_monitor_freeze_selection(0));
    CHECK(!ble_discovery_monitor_freeze_selection(0)&&!ble_discovery_live->session.enabled);
    memset(&ble_discovery_live->inventory,0,sizeof(ble_discovery_live->inventory));
    CHECK(!ble_discovery_monitor_apply_selection());
    ble_findmy_running=false;discovery_native.hold_stop=true;companion_released=false;
    ble_discovery_tick(base+40100U,false);dual_ble_live_tick(base+40100U,false);
    CHECK(companion_requested_mode==-1&&discovery_native.stop==1U);
    ble_discovery_tick(base+55000U,false);
    CHECK(discovery_native.bank.meta.attached&&!ble_discovery_monitor_apply_selection());
    discovery_native.hold_stop=false;discovery_native_finish(true);
    for(unsigned i=0;i<10U&&!cerberus_ble_discovery_session_released(&ble_discovery_live->session);++i)
        ble_discovery_tick(base+55200U+i*250U,false);
    CHECK(!ble_discovery_monitor_apply_selection()); /* C5 still owes release. */
    /* Foreign scanner ownership is never adopted/stopped for cached details. */
    unsigned before=all_boundary_calls();owner=CERBERUS_S3_BLE_OWNER_SCANNER;
    companion_released=true;CHECK(ble_discovery_monitor_apply_selection());
    CHECK(ble_mode==UI_BLE_MODE_DETAIL&&ble_selected_from_monitor);
    CHECK(owner==CERBERUS_S3_BLE_OWNER_SCANNER&&all_boundary_calls()==before);
    owner=CERBERUS_S3_BLE_OWNER_NONE;
    CHECK(ble_selected_scan_epoch==frozen.epoch&&ble_selected_original_time64_valid);
    CHECK(ble_selected_original_ms64==frozen.original_source_ms&&
        ble_selected_source_ceiling_ms64==frozen.source_now_ms);
    CHECK(!memcmp(&ble_selected_result,&frozen.record.peer,sizeof(ble_selected_result)));
    clean_live();

    /* C5 frozen selection survives every current-bank identity being replaced. */
    seed_native_stopped(CERBERUS_S3_BLE_FILTER_FIND_MY);ble_mode=UI_BLE_MODE_FINDMY_MONITOR;
    CHECK(ble_discovery_acquire()&&!ble_discovery_live->session.enabled);
    companion_row(0,41,2,-25);fixture_response_row(&companion_store.rows[0],findmy,sizeof(findmy),2U,1000U);
    companion_store.rows[0].address_type=2U;++companion_store.snapshots;
    CHECK(ble_discovery_combined_order()==1U&&ble_discovery_build_unified_row(0,0,1100U));
    clock_ms=1100;drawn[0]=0;
    CHECK(draw_ble_findmy_monitor()==ESP_OK&&ble_discovery_live->painted_count==1U);
    cerberus_dual_ble_row_t c5=ble_discovery_live->painted_c5[0];
    cerberus_dual_ble_provenance_t proof=ble_discovery_live->painted_c5_provenance[0];
    ble_findmy_monitor_tap(20,140);
    CHECK(ble_findmy_select_pending&&ble_findmy_stopping);
    memset(&companion_store,0,sizeof(companion_store));
    ble_findmy_monitor_tick(1200,false);
    CHECK(ble_mode==UI_BLE_MODE_C5&&dual_ble_ui_from_monitor&&!ble_findmy_stopping);
    CHECK(!memcmp(&c5,&dual_ble_ui_selected,sizeof(c5))&&
        !memcmp(&proof,&dual_ble_ui_selected_provenance,sizeof(proof)));
    /* An old accidental S3 intent cannot rearm a stopped monitor detail. */
    ble_discovery_live->session.enabled=true;dual_ble_live_tick(1200,false);
    CHECK(companion_requested_mode==-1);
    ble_discovery_live->session.enabled=false;clean_live();
}

static void shared_monitor_independent_intent_and_sleep(void)
{
    seed_native_stopped(CERBERUS_S3_BLE_FILTER_FIND_MY);ble_mode=UI_BLE_MODE_FINDMY_MONITOR;
    ble_findmy_running=true;CHECK(ble_discovery_monitor_start()==ESP_OK);
    discovery_native.reject_start=true;ble_discovery_tick(1000U,false);
    CHECK(!ble_discovery_live->session.enabled&&ble_discovery_live->session.error!=ESP_OK);
    dual_ble_live_tick(1000U,false);
    CHECK(companion_requested_mode==CERBERUS_DUAL_BLE_FAMILY&&companion_requested_family==2U);
    unsigned attempts=discovery_native.start;
    ble_discovery_tick(2000U,false);CHECK(discovery_native.start==attempts);
    ble_findmy_running=false;dual_ble_live_tick(2000U,false);CHECK(companion_requested_mode==-1);
    discovery_native.reject_start=false;ble_findmy_running=true;
    CHECK(ble_discovery_monitor_start()==ESP_OK);clock_ms=2100;ble_discovery_tick(2100U,false);
    CHECK(discovery_native.start==attempts+1U);
    ble_findmy_running=false;
    for(unsigned i=0;i<10U&&!cerberus_ble_discovery_session_released(&ble_discovery_live->session);++i){
        clock_ms=2400+(int64_t)i*250;ble_discovery_tick((uint64_t)clock_ms,true);dual_ble_live_tick((uint64_t)clock_ms,true);
    }
    CHECK(cerberus_ble_discovery_session_released(&ble_discovery_live->session)&&companion_requested_mode==-1);
    attempts=discovery_native.start;ble_discovery_tick(6000U,false);dual_ble_live_tick(6000U,false);
    CHECK(discovery_native.start==attempts&&!ble_discovery_live->session.enabled&&companion_requested_mode==-1);
    clean_live();
}

/* Composite tests compile both actual UI owner adapters, both controllers and
 * the global tick. Only the RF-driver and framed C5 peer are simulated. */
static void composite_seed(void)
{
    seed_native_stopped(CERBERUS_S3_BLE_FILTER_ALL);
    companion_actual_owner=true;
    memset(&composite_peer,0,sizeof(composite_peer));composite_peer.boot=UINT64_C(0x500000001);
    const cerberus_dual_ble_ops_t ops={NULL,fixture_actual_dual_ble_ui_exchange,fixture_actual_dual_ble_ui_clock};
    cerberus_dual_ble_init(&companion_store,&ops);
    dual_ble_ui_mode=-1;dual_ble_ui_family=0U;dual_ble_ui_attempted=false;
    dual_ble_ui_error=ESP_OK;dual_ble_ui_selection_pending=false;
}

static void composite_step(uint64_t now,bool asleep)
{
    unsigned frames=composite_peer.frames;
    native_tick(now,asleep);
    dual_ble_live_tick(now,asleep);
    CHECK(composite_peer.frames-frames<=4U);
}

static void composite_drain(void)
{
    ble_discovery_leave();discovery_native.hold_stop=false;composite_peer.hold_stop=false;
    for(unsigned i=0;i<60U&&(!dual_ble_live_discovery_released()||!dual_ble_ui_released());++i)
        composite_step((uint64_t)clock_ms+250U,false);
    CHECK(!ble_discovery_requested()&&dual_ble_live_discovery_released()&&dual_ble_ui_released());
    if(dual_ble_ui_store&&dual_ble_ui_store!=&companion_store)heap_caps_free(dual_ble_ui_store);
    dual_ble_ui_store=&companion_store;companion_actual_owner=false;
    clean_live();
}

static void ordinary_explicit_intent_and_receiver_failures(const char *prefix)
{
    composite_seed();
    CHECK(draw_live_scan()==ESP_OK&&!ble_discovery_live&&!ble_discovery_requested());
    composite_step(1000,false);composite_step(1250,false);
    CHECK(!discovery_native.start&&!composite_peer.starts&&!ble_discovery_requested());
    /* Rendering/visible ticks may allocate presentation, never a scan. */
    CHECK(ble_discovery_live&&!ble_discovery_live->session.enabled);
    actual_discovery_entry();CHECK(ble_discovery_requested());
    composite_step(1500,false);composite_step(1750,false);
    CHECK(discovery_native.start==1U&&composite_peer.starts==1U&&ble_discovery_requested());
    CHECK(!ble_discovery_s3_retryable()&&!dual_ble_live_c5_retryable());
    discovery_native.copy_errors=1U;composite_step(2000,false);
    CHECK(ble_discovery_live->session.enabled&&!ble_discovery_s3_retryable());
    CHECK(ble_discovery_retry_s3()==ESP_ERR_INVALID_STATE&&composite_peer.starts==1U);
    /* An asynchronous terminal S3 error drains only S3. */
    discovery_native.bank.meta.status.state=CERBERUS_BLE_SCAN_ERROR;
    discovery_native.bank.meta.status.error_code=ESP_FAIL;
    discovery_native.bank.meta.owned=false;owner=CERBERUS_S3_BLE_OWNER_NONE;
    for(unsigned i=0;i<12U&&!ble_discovery_live->session.faulted;++i)composite_step((uint64_t)clock_ms+250U,false);
    CHECK(ble_discovery_s3_retryable()&&ble_discovery_requested()&&composite_peer.owned);
    unsigned c5starts=composite_peer.starts,c5stops=composite_peer.stops,s3starts=discovery_native.start;
    for(unsigned i=0;i<8U;++i)composite_step((uint64_t)clock_ms+250U,false);
    CHECK(discovery_native.start==s3starts&&composite_peer.starts==c5starts&&composite_peer.stops==c5stops);
    drawn[0]=0;CHECK(draw_live_scan()==ESP_OK&&live_view.stop_action&&live_view.state==SENTINEL_VIEW_LIVE);
    save(prefix,"independent-s3-error");
    handle_live_scan_tap(20,113);CHECK(ble_mode==UI_BLE_MODE_C5);
    dual_ble_live_tap(40,125);composite_step((uint64_t)clock_ms+250U,false);
    CHECK(discovery_native.start==s3starts+1U&&composite_peer.starts==c5starts&&composite_peer.stops==c5stops);
    CHECK(ble_discovery_requested()&&!ble_discovery_s3_retryable());
    composite_drain();

    /* C5 unsupported is terminal for that receiver only; S3 keeps its epoch. */
    composite_seed();composite_peer.unsupported=true;CHECK(ble_discovery_request_start()==ESP_OK);
    composite_step(1000,false);composite_step(1250,false);
    CHECK(ble_discovery_requested()&&ble_discovery_live->session.enabled&&discovery_native.start==1U);
    CHECK(dual_ble_live_c5_retryable()&&!ble_discovery_s3_retryable());
    uint64_t epoch=ble_discovery_live->session.epoch;
    unsigned s3stops=discovery_native.stop;
    composite_peer.unsupported=false;handle_live_scan_tap(20,113);dual_ble_live_tap(200,125);
    composite_step(1500,false);composite_step(1750,false);
    CHECK(composite_peer.starts==1U&&ble_discovery_live->session.epoch==epoch&&
        discovery_native.stop==s3stops&&discovery_native.start==1U);
    composite_drain();
}

static void ordinary_allocation_failure_and_no_store_navigation(const char *prefix)
{
    composite_seed();dual_ble_ui_store=NULL;fail_alloc=true;
    CHECK(ble_discovery_request_start()==ESP_ERR_NO_MEM&&ble_discovery_requested());
    fail_alloc=false;composite_step(1000,false);composite_step(1250,false);
    CHECK(!ble_discovery_live&&ble_discovery_s3_retryable()&&composite_peer.starts==1U&&composite_peer.owned);
    CHECK(draw_live_scan()==ESP_OK&&!live_view.row_count&&!live_view.total_count&&live_view.stop_action);
    CHECK(live_view.state==SENTINEL_VIEW_LIVE&&strstr(live_subtitle,"S3"));save(prefix,"independent-s3-store-unavailable");
    unsigned starts_c5=composite_peer.starts,stop_c5=composite_peer.stops;
    handle_live_scan_tap(20,113);CHECK(ble_mode==UI_BLE_MODE_C5);
    dual_ble_live_tap(40,125);composite_step(1500,false);
    CHECK(ble_discovery_live&&discovery_native.start==1U&&composite_peer.starts==starts_c5&&composite_peer.stops==stop_c5);
    composite_drain();

    composite_seed();CHECK(ble_discovery_request_start()==ESP_OK);dual_ble_ui_store=NULL;fail_alloc=true;
    composite_step(1000,false);fail_alloc=false;
    CHECK(discovery_native.start==1U&&ble_discovery_requested()&&dual_ble_live_c5_retryable());
    CHECK(!dual_ble_ui_store&&ble_discovery_live->session.enabled);
    composite_step(1250,false);CHECK(!dual_ble_ui_store); /* No implicit allocation retry. */
    handle_live_scan_tap(20,113);dual_ble_live_tap(200,125);composite_step(1500,false);composite_step(1750,false);
    CHECK(composite_peer.starts==1U&&discovery_native.start==1U&&!discovery_native.stop);
    composite_drain();

    /* Tools and monitor transitions work with no S3 store and held C5 cleanup. */
    for(unsigned destination=1U;destination<=2U;++destination){
        composite_seed();fail_alloc=true;CHECK(ble_discovery_request_start()==ESP_ERR_NO_MEM);
        fail_alloc=false;composite_step(1000,false);composite_step(1250,false);
        composite_peer.hold_stop=true;ble_discovery_request_destination((uint8_t)destination);
        unsigned allocs=discovery_ui_allocations;
        composite_step(1500,false);composite_step(14500,false);
        CHECK(!ble_discovery_requested()&&ble_discovery_unallocated_destination==destination&&!ble_discovery_live);
        CHECK(ble_mode==UI_BLE_MODE_LIST&&discovery_ui_allocations==allocs);
        /* Stop cancels pending destination without any new RF or allocation. */
        ble_discovery_toggle();CHECK(!ble_discovery_unallocated_destination&&!ble_discovery_requested());
        composite_peer.hold_stop=false;
        for(unsigned i=0;i<10U&&!dual_ble_ui_released();++i)composite_step((uint64_t)clock_ms+250U,false);
        CHECK(ble_mode==UI_BLE_MODE_LIST&&!ble_discovery_live);
        if(destination==1U){ble_discovery_request_destination(1U);composite_step((uint64_t)clock_ms+250U,false);
            CHECK(ble_mode==UI_BLE_MODE_MENU&&!ble_discovery_live);}
        composite_drain();
    }
}

static void ordinary_no_store_navigation_superseded(void)
{
    for(unsigned route=0;route<2U;++route){
        composite_seed();fail_alloc=true;CHECK(ble_discovery_request_start()==ESP_ERR_NO_MEM);
        fail_alloc=false;composite_step(1000,false);composite_step(1250,false);
        composite_peer.hold_stop=true;ble_discovery_request_destination(1U);
        composite_step(1500,false);CHECK(ble_discovery_unallocated_destination==1U);
        if(route==0U)handle_live_scan_tap(20,113);
        else handle_live_scan_tap(40,65);
        CHECK(!ble_discovery_unallocated_destination&&!ble_discovery_requested());
        composite_peer.hold_stop=false;
        for(unsigned i=0;i<12U&&!dual_ble_ui_released();++i)composite_step((uint64_t)clock_ms+250U,false);
        CHECK(ble_mode==(route==0U?UI_BLE_MODE_C5:UI_BLE_MODE_LIST)&&!ble_discovery_live);
        composite_drain();
    }
}

static void ordinary_unallocated_status_refresh(const char *prefix)
{
    composite_seed();fail_alloc=true;CHECK(ble_discovery_request_start()==ESP_ERR_NO_MEM);
    fail_alloc=false;
    unsigned allocations=discovery_ui_allocations;
    composite_step(1000,false); /* Paint STOPPED, then queue C5 INFO. */
    CHECK(!ble_discovery_live&&strstr(live_subtitle,"C5 STOPPED"));
    unsigned frame=live_frame,frames=composite_peer.frames;
    native_tick(1100,false);CHECK(live_frame==frame&&composite_peer.frames==frames);
    composite_step(1250,false); /* Paint STARTING before actual C5 start. */
    CHECK(live_frame==frame+1U&&strstr(live_subtitle,"C5 STARTING"));
    CHECK(composite_peer.starts==1U&&composite_peer.owned);
    frame=live_frame;frames=composite_peer.frames;
    native_tick(1450,false); /* Only cached UI/S3 tick; no C5 exchange. */
    CHECK(live_frame==frame+1U&&strstr(live_subtitle,"C5 LIVE"));
    CHECK(composite_peer.frames==frames&&discovery_ui_allocations==allocations&&!discovery_native.start);
    /* A committed C5 bank count remains visible without borrowing legacy S3
     * rows or allocating another shared index. */
    companion_store.count=7U;ble_discovery_request_stop();
    native_tick(1650,false);CHECK(strstr(live_subtitle,"STOPPING"));
    composite_step(1900,false);composite_step(2150,false);native_tick(2400,false);
    CHECK(dual_ble_ui_released()&&strstr(live_hint,"C5 HAS 7 RETAINED"));
    CHECK(!live_view.row_count&&!live_view.total_count&&discovery_ui_allocations==allocations);
    /* A different cached error is repainted on cadence with the exact code. */
    dual_ble_ui_error=ESP_ERR_TIMEOUT;frames=composite_peer.frames;frame=live_frame;
    native_tick(2600,false);CHECK(live_frame==frame+1U&&strstr(live_subtitle,"C5 ERROR"));
    CHECK(composite_peer.frames==frames&&discovery_ui_allocations==allocations);
    drawn[0]=0;CHECK(live_paint(true)==ESP_OK);save(prefix,"no-store-cached-c5-error");
    frame=live_frame;native_tick(2700,false);CHECK(live_frame==frame);
    native_tick(2900,true);CHECK(live_frame==frame); /* Hidden/sleep never paint. */
    CHECK(!ble_discovery_requested()&&discovery_ui_allocations==allocations&&!ble_discovery_live);
    composite_drain();
}

static void ordinary_stop_pending_info_and_source_cleanup(void)
{
    for(unsigned route=0;route<4U;++route){
        composite_seed();CHECK(ble_discovery_request_start()==ESP_OK);
        composite_step(1000,false); /* C5 begin queued INFO, no owner/transport yet. */
        CHECK(dual_ble_ui_store->enabled&&!dual_ble_ui_store->start.job_id&&!composite_peer.frames);
        if(route==0)ble_discovery_toggle();
        else if(route==1)actual_back(UI_BACK_Y);
        else if(route==2)ble_discovery_leave(); /* Same ordinary Save cancellation hook. */
        composite_step(1250,route==3U);
        CHECK(!ble_discovery_requested()&&!dual_ble_ui_store->enabled&&!composite_peer.frames&&!composite_peer.starts);
        for(unsigned i=0;i<4U;++i)composite_step((uint64_t)clock_ms+250U,false);
        CHECK(!composite_peer.starts&&discovery_native.start==1U);
        composite_drain();
    }
    composite_seed();CHECK(ble_discovery_request_start()==ESP_OK);
    composite_step(1000,false);composite_step(1250,false);
    discovery_native.hold_stop=composite_peer.hold_stop=true;
    ble_discovery_toggle();composite_step(1500,false);
    CHECK(!ble_discovery_requested()&&!dual_ble_live_discovery_released()&&!dual_ble_ui_released());
    ble_discovery_toggle();composite_step(14500,false);CHECK(!ble_discovery_requested());
    ble_mode=UI_BLE_MODE_C5;dual_ble_ui_detail=false;
    dual_ble_live_tap(150,390);composite_step(14750,false);
    CHECK(!ble_discovery_requested()&&!ble_discovery_live->session.enabled&&composite_peer.starts==1U&&discovery_native.start==1U);
    composite_drain();
}

static void ordinary_entries_and_failed_family_changes(const char *prefix)
{
    for(unsigned entry=0;entry<3U;++entry){
        composite_seed();
        if(entry==0U)actual_discovery_entry();
        else if(entry==1U)actual_discovery_menu_scan();
        else actual_discovery_missing_selection();
        CHECK(ble_discovery_requested()&&ble_discovery_live&&ble_discovery_live->session.enabled);
        composite_step(1000,false);composite_step(1250,false);
        CHECK(discovery_native.start==1U&&composite_peer.starts==1U);
        composite_drain();
    }
    composite_seed();ui_capability_blocked=true;actual_discovery_entry();
    CHECK(!ble_discovery_requested()&&!ble_discovery_live);ui_capability_blocked=false;
    ble_attack_selecting=true;actual_discovery_entry();CHECK(!ble_discovery_requested());
    ble_attack_selecting=false;composite_drain();

    composite_seed();discovery_native.reject_start=true;composite_peer.unsupported=true;
    CHECK(ble_discovery_request_start()==ESP_OK);composite_step(1000,false);composite_step(1250,false);
    CHECK(ble_discovery_s3_retryable()&&dual_ble_live_c5_retryable());
    unsigned s3starts=discovery_native.start,c5frames=composite_peer.frames;
    drawn[0]=0;CHECK(draw_live_scan()==ESP_OK&&live_view.state==SENTINEL_VIEW_ERROR&&live_view.stop_action);
    CHECK(!strcmp(live_view.message,"RETRY RECEIVERS"));
    CHECK(!strstr(drawn,"WAITING FOR REPORTS")&&!strstr(drawn,"WAITING FOR HINTS"));
    save(prefix,"both-receivers-retry");
    handle_live_scan_tap(40,65);composite_step(1500,false);composite_step(1750,false);
    CHECK(ble_filter==CERBERUS_S3_BLE_FILTER_IBEACON&&ble_discovery_requested());
    CHECK(discovery_native.start==s3starts&&composite_peer.frames==c5frames&&
        ble_discovery_s3_retryable()&&dual_ble_live_c5_retryable());
    ble_discovery_toggle();composite_step(2000,false);
    CHECK(!ble_discovery_requested()&&!ble_discovery_s3_retryable()&&!dual_ble_live_c5_retryable());
    handle_live_scan_tap(40,65);composite_step(2250,false);
    CHECK(discovery_native.start==s3starts&&composite_peer.frames==c5frames);
    composite_drain();

    /* A deliberate new Start resets both failed attempts without needing an
     * intervening cancellation tick. The source-only retry stays separate. */
    composite_seed();composite_peer.unsupported=true;discovery_native.reject_start=true;
    CHECK(ble_discovery_request_start()==ESP_OK);composite_step(1000,false);composite_step(1250,false);
    CHECK(dual_ble_live_c5_retryable()&&ble_discovery_s3_retryable());
    composite_peer.unsupported=false;discovery_native.reject_start=false;
    CHECK(ble_discovery_request_start()==ESP_OK);composite_step(1500,false);composite_step(1750,false);
    CHECK(composite_peer.starts==1U&&discovery_native.start==2U&&ble_discovery_requested());
    composite_drain();
}

static void ordinary_direct_c5_frozen_handoff(void)
{
    for(unsigned cancel=0;cancel<4U;++cancel){
        composite_seed();fail_alloc=true;CHECK(ble_discovery_request_start()==ESP_ERR_NO_MEM);
        fail_alloc=false;composite_step(1000,false);composite_step(1250,false);
        handle_live_scan_tap(20,113);CHECK(ble_mode==UI_BLE_MODE_C5&&!ble_discovery_live);
        companion_row(0,57,2,-32);
        dual_ble_ui_painted[0]=companion_store.rows[0];
        dual_ble_ui_painted_provenance[0]=(cerberus_dual_ble_provenance_t){.boot_id=11,.epoch=22,.generation=3,.index=0};
        dual_ble_ui_painted_count=1U;
        cerberus_dual_ble_row_t frozen=dual_ble_ui_painted[0];
        cerberus_dual_ble_provenance_t proof=dual_ble_ui_painted_provenance[0];
        /* companion_row's presentation exchange is never used by this owner. */
        companion_store.ops.exchange=fixture_actual_dual_ble_ui_exchange;
        composite_peer.hold_stop=true;dual_ble_live_tap(20,160);
        CHECK(dual_ble_ui_selection_pending&&!dual_ble_ui_detail&&!ble_discovery_requested());
        memset(&companion_store.rows[0],0,sizeof(companion_store.rows[0]));++companion_store.published_epoch;
        composite_step(1500,false);composite_step(14500,false);
        CHECK(dual_ble_ui_selection_pending&&!dual_ble_ui_detail);
        if(cancel==1U)dual_ble_live_tap(150,390);
        if(cancel==2U)actual_c5_footer_tap(40,440);
        if(cancel==3U)composite_step(14750,true);
        if(cancel)CHECK(!dual_ble_ui_selection_pending);
        composite_peer.hold_stop=false;
        for(unsigned i=0;i<12U&&(!dual_ble_ui_released()||dual_ble_ui_selection_pending);++i)
            composite_step((uint64_t)clock_ms+250U,false);
        CHECK(dual_ble_ui_detail==(cancel==0U));
        CHECK(!memcmp(&frozen,&dual_ble_ui_selected,sizeof(frozen))&&!memcmp(&proof,&dual_ble_ui_selected_provenance,sizeof(proof)));
        unsigned starts=composite_peer.starts;
        ble_mode=UI_BLE_MODE_LIST;handle_live_scan_tap(50,65);composite_step((uint64_t)clock_ms+250U,false);
        CHECK(!ble_discovery_requested()&&composite_peer.starts==starts&&!discovery_native.start);
        composite_drain();
    }
}

static void recon_lease_blocks_new_discovery(void)
{
    seed_native(CERBERUS_S3_BLE_FILTER_ALL);
    /* Recon's scheduler can already be NONE while its exact lease drains.
     * Both subsequent observers must honor the separate release predicate. */
    live_ble_session.active=SENTINEL_SCAN_NONE;
    live_ble_lease_epoch=UINT64_C(0x100000007);
    native_tick(1000,false);dual_ble_live_tick(1000,false);
    native_tick(2000,false);dual_ble_live_tick(2000,false);
    CHECK(discovery_native.start==0U&&companion_requested_mode==-1&&ble_discovery_requested());
    live_ble_lease_epoch=0U;native_tick(2500,false);dual_ble_live_tick(2500,false);
    CHECK(discovery_native.start==1U&&companion_requested_mode==CERBERUS_DUAL_BLE_DISCOVERY);
    stop_and_release(3000);clean_live();

    fox_seed();cerberus_c5_ble_result_t target=fox_target();
    live_ble_lease_epoch=UINT64_C(0x100000007);ble_fox_hunt_open_peer(&target,false);
    fox_step(1100,false);fox_step(1400,false);
    CHECK(ble_fox_hunt_start_pending&&!track_begins&&companion_requested_mode==-1);
    live_ble_lease_epoch=0U;fox_step(1700,false);fox_step(2000,false);
    CHECK(ble_fox_hunt_s3_started&&companion_requested_mode==CERBERUS_DUAL_BLE_TRACK);
    fox_finish(2300);
}

int main(int argc,char **argv)
{
    CHECK(argc==2);
    ordinary_explicit_intent_and_receiver_failures(argv[1]);
    ordinary_allocation_failure_and_no_store_navigation(argv[1]);
    ordinary_no_store_navigation_superseded();
    ordinary_unallocated_status_refresh(argv[1]);
    ordinary_stop_pending_info_and_source_cleanup();
    ordinary_entries_and_failed_family_changes(argv[1]);
    ordinary_direct_c5_frozen_handoff();
    empty_receive_explanation();
    first_results_and_frozen_tap(argv[1]);
    continuous_family_and_stop(argv[1]);
    failures_and_navigation();
    deferred_navigation_and_copy_failure();
    source_switch_and_maximum(argv[1]);
    retained_badge_age(argv[1]);
    companion_pane_and_fox_barrier();
    unified_filters_capacity_and_family(argv[1]);
    unified_frozen_c5_selection_save_fox(argv[1]);
    unified_age_failure_and_back(argv[1]);
    unified_committed_order_cache();
    unified_all_families(argv[1]);
    unified_family_recency(argv[1]);
    unified_family_cache_and_unavailable(argv[1]);
    unified_full_original_and_focus(argv[1]);
    unified_response_families(argv[1]);
    fox_independent_failure_and_retry();
    fox_barrier_reentry_and_foreign_owner();
    fox_stop_retains_cleanup_and_clock();
    fox_back_sleep_and_pending_stop();
    shared_monitor_acquisition_and_frozen_selection();
    shared_monitor_independent_intent_and_sleep();
    recon_lease_blocks_new_discovery();
    printf("Ordinary BLE live UI: %u checks passed; active updates, frozen painted selection, scoped cleanup and C5 independence\n",live_checks);
    return 0;
}
