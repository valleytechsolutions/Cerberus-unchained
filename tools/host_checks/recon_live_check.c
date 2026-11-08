/* Actual production acquisition, scheduler, Watch ingestion and navigation.
 * Native radio SDK functions and C5 Wi-Fi command transport are boundaries. */
#define CERBERUS_RECON_LIVE_TEST 1
#define main existing_recon_recorder_suite
#include "recon_recorder_ui_check.c"
#undef main
#include "recon_live_tick_impl.h"
static const char *preview_directory;

static void lifecycle_seed(void)
{
    if(live_ble_evidence&&live_ble_evidence!=evidence)heap_caps_free(live_ble_evidence);
    live_ble_evidence=evidence;recon_lifecycle_active=false;seed();recon_lifecycle_active=true;
    live_ble_evidence=NULL;memset(&recon_native,0,sizeof(recon_native));
    recon_native.current_host_epoch=1U;recon_native.next_epoch=UINT64_C(0x100000100);
    c5_starts=c5_stops=c5_status_reads=c5_captures=ui_redraws=0U;
    c5_start_error=c5_query_error=ESP_OK;c5_held=false;memset(&c5_scan,0,sizeof(c5_scan));
    c5_status.capabilities=CERBERUS_C5_CAP_WIFI_2_4_GHZ;
    live_status_changed=live_owned_wifi_observation=live_scan_observation_valid=live_ble_observation_valid=false;
    live_capture_pending=live_frame=live_last_poll=0U;clock_us=1000000;
    ble_discovery_live=NULL;wifi_selection_valid=false;
}
static void lifecycle_tick(uint32_t now,bool asleep)
{
    clock_us=(int64_t)now*1000;live_scan_tick(now,asleep);watch_ui_tick(now);
    CHECK(!recon_native.foreign_stops); /* Generic getters independently fail fast. */
}
static void lifecycle_cleanup(void)
{
    current_page=UI_PAGE_MENU;watch_paused=true;recon_native.held=false;
    recon_native.stop_error=recon_native.query_error=ESP_OK;c5_held=false;
    for(unsigned i=0;i<24U&&(live_ble_lease_epoch||live_ble_session.active||live_session.active);++i)
        lifecycle_tick((uint32_t)(clock_us/1000)+1000U,false);
    CHECK(!live_ble_lease_epoch&&!live_ble_session.active&&!live_session.active);
    if(live_ble_evidence){heap_caps_free(live_ble_evidence);live_ble_evidence=NULL;}
    watch_inventory_release();
}
static void complete_ble(unsigned count)
{
    CHECK(count<=CERBERUS_BLE_SCAN_MAX_RESULTS&&recon_native.state.attached);
    recon_native.state.owned=recon_native.state.receiving=recon_native.state.stopping=false;
    recon_native.state.scan.state=CERBERUS_BLE_SCAN_COMPLETE;
    recon_native.state.scan.stored_count=(uint8_t)count;
    for(unsigned i=0;i<count;++i){
        cerberus_s3_ble_discovery_record_t *r=&recon_native.rows[i];memset(r,0,sizeof(*r));
        r->generation=recon_native.state.scan.generation;r->index=(uint8_t)i;r->observed_ms=0U;
        r->peer.address[0]=42;r->peer.address_type=(uint8_t)(i%4U);r->peer.rssi=-45-(int)i;
        strcpy(r->peer.name,i?"ordinary BLE":"Hint BLE");
        memcpy(r->advertisement.address,r->peer.address,6U);r->advertisement.address_type=r->peer.address_type;
        r->advertisement.event_type=0U;r->advertisement.length=3U;
        r->advertisement.data[0]=2U;r->advertisement.data[1]=1U;r->advertisement.data[2]=6U;
        cerberus_s3_ble_discovery_evidence_t *p=&r->focus.discovery;
        memcpy(p->address,r->peer.address,6U);p->address_type=r->peer.address_type;p->generation=r->generation;
        p->flags=CERBERUS_S3_BLE_EVIDENCE_ADV_COMPLETE|CERBERUS_S3_BLE_EVIDENCE_ADV_VALID;
        p->advertisement_sequence=p->raw_advertisement_sequence=i+1U;
    }
}
static void complete_wifi(void)
{c5_scan.state=CERBERUS_WIFI_SCAN_COMPLETE;c5_scan.stored_count=c5_scan.total_count=1U;}
static void local_rejection_does_not_adopt(void)
{
    const esp_err_t errors[]={ESP_ERR_INVALID_STATE,ESP_ERR_NO_MEM,ESP_FAIL};
    for(unsigned f=0;f<3U;++f){
        lifecycle_seed();recon_native.start_error=errors[f];recon_native.foreign=f==0U;
        lifecycle_tick(1000,false);
        CHECK(c5_starts==1U&&recon_native.starts==1U&&!live_ble_lease_epoch&&
            live_ble_session.active==SENTINEL_SCAN_NONE&&!live_ble_session.command_failed);
        /* A foreign completed/RUNNING bank with exactly the same public
         * generation is not queried, copied, classified or stopped. */
        recon_native.state=(cerberus_s3_ble_passive_state_t){.epoch=99U,.scan_epoch=1U,.host_epoch=1U,
            .scan={.generation=0U,.state=CERBERUS_BLE_SCAN_RUNNING,.stored_count=1U},.attached=true,.owned=true};
        recon_native.current_scan_epoch=1U;
        for(unsigned n=0;n<3U;++n){complete_wifi();lifecycle_tick(2000+n*1500,false);lifecycle_tick(2500+n*1500,false);}
        CHECK(c5_starts>=3U&&!c5_stops&&recon_native.queries==0U&&recon_native.copies==0U&&
            recon_native.stops==0U&&recon_native.releases==0U&&offer_count>=3U);
        for(size_t i=0;i<offer_count;++i)CHECK(offered[i].radio==CERBERUS_RECON_RADIO_WIFI);
        unsigned starts=recon_native.starts;
        current_page=UI_PAGE_MENU;lifecycle_tick(8000,false);lifecycle_tick(9000,false);
        CHECK(!live_ble_session.active&&!live_ble_lease_epoch&&recon_native.starts==starts&&!recon_native.stops);
        lifecycle_cleanup();
    }
}
static void accepted_capture_and_history(void)
{
    lifecycle_seed();lifecycle_tick(1000,false);uint64_t epoch=live_ble_lease_epoch;
    CHECK(!live_ble_recon_released());complete_ble(2U);complete_wifi();
    cerberus_s3_ble_discovery_record_t expected=recon_native.rows[1];
    lifecycle_tick(2000,false);
    CHECK(live_ble_capture_epoch==epoch&&live_ble_capture_state.epoch==epoch&&
        live_records[SENTINEL_SCAN_BLE].captured&&live_records[SENTINEL_SCAN_BLE].count==2U&&offer_count==3U);
    CHECK(watch_seen_ble_epoch==epoch&&!live_ble_session.completed&&recon_native.copies==2U);
    CHECK(!memcmp(&live_ble_evidence[1].advertisement,&expected.advertisement,sizeof(expected.advertisement))&&
        !memcmp(&live_ble_evidence[1].focus,&expected.focus,sizeof(expected.focus))&&
        live_ble_evidence[1].observed_ms==0U&&live_ble_evidence[1].peer.address_type==1U);
    CHECK(offered[1].source_kind==CERBERUS_RECON_SOURCE_S3_ADV_MS32&&offered[1].source_time==0U&&
        offered[1].address_type==0U&&offered[2].address_type==1U);
    const sentinel_watch_entry_t *entry=NULL;
    for(unsigned i=0;i<watch_state.history_count;++i){const sentinel_watch_entry_t *r=sentinel_watch_history(&watch_state,i);
        if(r->radio==SENTINEL_WATCH_RADIO_BLE){entry=r;break;}}
    CHECK(entry);watch_select_detail(entry);sentinel_watch_entry_t detail=watch_detail;
    unsigned c5before=c5_starts;lifecycle_tick(2500,false);
    CHECK(live_ble_lease_epoch>epoch&&c5_starts==c5before+1U&&!memcmp(&detail,&watch_detail,sizeof(detail)));
    memset(recon_native.rows,0,sizeof(recon_native.rows));
    CHECK(!memcmp(&live_ble_evidence[1].advertisement,&expected.advertisement,sizeof(expected.advertisement)));
    lifecycle_cleanup();
}
static void capture_faults_do_not_publish(void)
{
    for(unsigned fault=0;fault<5U;++fault){
        lifecycle_seed();lifecycle_tick(1000,false);complete_ble(2U);
        if(fault==0U)recon_native.copy_error=ESP_ERR_NO_MEM;
        if(fault==1U)recon_native.replace_after_copy=1U;
        if(fault==2U)recon_native.replace_after_copy=2U;
        if(fault==3U)++recon_native.rows[1].generation;
        if(fault==4U)allocation_fail=true;
        complete_wifi();lifecycle_tick(2000,false);
        CHECK(!live_records[SENTINEL_SCAN_BLE].captured&&!live_ble_capture_epoch&&
            !live_ble_evidence&&offer_count==1U&&offered[0].radio==CERBERUS_RECON_RADIO_WIFI);
        CHECK(watch_capture_failed[1]||live_records[SENTINEL_SCAN_BLE].error!=ESP_OK);
        allocation_fail=false;recon_native.copy_error=ESP_OK;lifecycle_cleanup();
    }
    /* Replacement after publication but before Watch must reject the saved
     * epoch even when generation/count and address bytes still agree. */
    for(unsigned fault=0;fault<3U;++fault){
        lifecycle_seed();lifecycle_tick(1000,false);complete_ble(1U);
        clock_us=2000000;live_scan_tick(2000,false);CHECK(live_ble_capture_epoch);
        if(fault==0U)++recon_native.current_scan_epoch;
        if(fault==1U)++recon_native.current_host_epoch;
        if(fault==2U)++recon_native.state.epoch;
        CHECK(!watch_consume_radio(SENTINEL_SCAN_BLE,2000)&&!offer_count&&watch_capture_failed[1]);
        lifecycle_tick(2500,false);CHECK(!recon_native.stops||!recon_native.foreign_stops);
        lifecycle_cleanup();
    }
}
static void cleanup_and_reentry(void)
{
    for(unsigned route=0;route<4U;++route){
        lifecycle_seed();lifecycle_tick(1000,false);recon_native.held=true;recon_native.stop_error=ESP_FAIL;
        uint64_t epoch=live_ble_lease_epoch;
        if(route==0U){watch_handle_tap(110,440);CHECK(watch_paused);}
        if(route==1U){watch_handle_tap(20,440);CHECK(current_page==UI_PAGE_MENU);}
        if(route==2U)current_page=UI_PAGE_BLE,ble_mode=UI_BLE_MODE_LIST;
        lifecycle_tick(2000,route==3U);
        unsigned starts=recon_native.starts;
        for(unsigned i=0;i<18U;++i)lifecycle_tick(3000+i*1000U,route==3U);
        CHECK(live_ble_lease_epoch==epoch&&!live_ble_recon_released()&&recon_native.stops>3U&&recon_native.starts==starts);
        if(route==0U){unsigned queries=recon_native.queries;
            painted_paused=painted_pausing=painted_resume=0U;
            CHECK(watch_draw(21000,true)==ESP_OK&&strstr(watch_view.phase,"PAUSING / WAITING FOR RADIO RELEASE"));
            CHECK(watch_view.paused&&watch_view.releasing&&painted_pausing==1U&&
                !painted_paused&&painted_resume==1U);
            /* A releasing-only change must invalidate the cached header,
             * while paused art/controls remain unchanged. */
            sentinel_watch_view_t changed=watch_view;changed.releasing=false;
            painted_paused=painted_pausing=painted_resume=0U;calls=0U;
            CHECK(sentinel_watch_view_update(&changed,21000U)==ESP_OK);
            CHECK(painted_paused==1U&&!painted_pausing&&painted_resume==1U&&calls>0U);
            painted_paused=painted_pausing=painted_resume=0U;
            CHECK(sentinel_watch_view_update(&watch_view,21000U)==ESP_OK);
            CHECK(painted_pausing==1U&&!painted_paused&&painted_resume==1U);
            CHECK(recon_native.queries==queries);save_preview(preview_directory,"recon-pausing-held");}
        recon_native.stop_error=ESP_OK;recon_native.held=false;
        /* Keep this exited/paused during drain; waking an active Recon page
         * is a separate user intent, not cleanup confirmation. */
        if(route!=0U){current_page=UI_PAGE_BLE;ble_mode=UI_BLE_MODE_LIST;}
        lifecycle_tick(22000,false);lifecycle_tick(23000,false);
        CHECK(live_ble_recon_released()&&!live_ble_session.active&&recon_native.starts==starts);
        if(route==0U){unsigned queries=recon_native.queries;
            painted_paused=painted_pausing=painted_resume=0U;
            CHECK(watch_draw(23000,true)==ESP_OK&&watch_view.paused&&!watch_view.releasing);
            CHECK(painted_paused==1U&&!painted_pausing&&painted_resume==1U&&
                strstr(watch_view.phase,"PAUSED / TAP RESUME"));
            CHECK(recon_native.queries==queries);save_preview(preview_directory,"recon-paused-released");}
        cerberus_s3_ble_passive_state_t next;
        CHECK(cerberus_s3_ble_scanner_passive_start(0U,&next)==ESP_OK&&next.epoch>epoch);
        CHECK(cerberus_s3_ble_scanner_passive_stop(next.epoch,&next)==ESP_OK);
        CHECK(cerberus_s3_ble_scanner_passive_release(next.epoch)==ESP_OK);
        lifecycle_cleanup();
    }
    /* A non-OK Start can still return an exact cleanup lease. It must not use
     * the local-rejection shortcut or lose ownership on retry exhaustion. */
    lifecycle_seed();recon_native.start_error=ESP_FAIL;recon_native.error_with_lease=true;
    recon_native.held=true;recon_native.stop_error=ESP_FAIL;lifecycle_tick(1000,false);
    CHECK(live_ble_lease_epoch&&live_ble_session.active==SENTINEL_SCAN_BLE&&live_ble_stop_pending);
    unsigned starts=recon_native.starts;uint64_t epoch=live_ble_lease_epoch;
    for(unsigned i=0;i<20U;++i){if(i%3U==0U)complete_wifi();lifecycle_tick(2000+i*1000U,false);}
    CHECK(recon_native.starts==starts&&live_ble_lease_epoch==epoch&&c5_starts>3U&&offer_count>0U);
    lifecycle_cleanup();
}
static void local_rearm_rejection_and_cached_render(void)
{
    for(unsigned empty=0;empty<2U;++empty){
    lifecycle_seed();lifecycle_tick(1000,false);complete_ble(1U);lifecycle_tick(2000,false);
    uint64_t prior=live_ble_lease_epoch;unsigned c5=c5_starts;
    CHECK(prior&&offer_count==1U&&!live_ble_session.completed);
    /* Native returns its existing attachment. Also retain coverage for an
     * uncertain empty reply: neither may discard the caller's old token. */
    recon_native.empty_rearm_error=empty!=0U;
    recon_native.start_error=ESP_ERR_INVALID_STATE;lifecycle_tick(2500,false);
    CHECK(live_ble_lease_epoch==prior&&live_ble_stop_pending&&live_ble_session.active==SENTINEL_SCAN_BLE);
    lifecycle_tick(3000,false);
    CHECK(!live_ble_lease_epoch&&!recon_native.state.attached&&c5_starts==c5&&offer_count==1U);
    recon_native.start_error=ESP_OK;lifecycle_tick(4000,false);
    CHECK(live_ble_lease_epoch>prior&&recon_native.state.owned&&offer_count==1U);
    lifecycle_cleanup();
    }

    lifecycle_seed();c5_online=false;recon_native.queued=true;lifecycle_tick(1000,false);
    CHECK(!c5_starts&&recon_native.state.owned&&!recon_native.state.receiving);
    unsigned queries=recon_native.queries,copies=recon_native.copies,starts=recon_native.starts;
    drawn_size=0;drawn[0]=0;CHECK(watch_draw(1000,true)==ESP_OK);
    CHECK(strstr(watch_view.phase,"S3 BLE WAITING")&&!strstr(watch_view.phase,"SCANNING"));
    save_preview(preview_directory,"recon-queued");
    CHECK(recon_native.queries==queries&&recon_native.copies==copies&&recon_native.starts==starts);
    recon_native.state.receiving=true;lifecycle_tick(1500,false);
    drawn_size=0;drawn[0]=0;queries=recon_native.queries;CHECK(watch_draw(1500,true)==ESP_OK);
    CHECK(strstr(watch_view.phase,"S3 BLE SCANNING")&&recon_native.queries==queries);
    complete_ble(1U);lifecycle_tick(2000,false);
    CHECK(!c5_starts&&offer_count==1U&&offered[0].radio==CERBERUS_RECON_RADIO_BLE);
    lifecycle_cleanup();

    lifecycle_seed();recon_native.start_error=ESP_ERR_NO_MEM;lifecycle_tick(1000,false);
    queries=recon_native.queries;starts=recon_native.starts;drawn_size=0;drawn[0]=0;
    CHECK(watch_draw(1000,true)==ESP_OK&&strstr(watch_view.phase,"S3 BLE ERROR"));
    save_preview(preview_directory,"recon-local-rejection");
    CHECK(recon_native.queries==queries&&recon_native.starts==starts&&!live_ble_lease_epoch);
    lifecycle_cleanup();
}
int main(int argc,char **argv)
{
    CHECK(argc==2);preview_directory=argv[1];
    (void)existing_recon_recorder_suite;(void)inventory_allocation;
    local_rejection_does_not_adopt();accepted_capture_and_history();capture_faults_do_not_publish();cleanup_and_reentry();
    local_rearm_rejection_and_cached_render();
    printf("PASS: %u actual Recon lifecycle/capture/Watch checks.\n",checks);
    return 0;
}
