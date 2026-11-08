/* Reuse the scanner/host fakes and actual frozen-selection lifecycle. */
#pragma push_macro("main")
#undef main
#define main existing_monitor_suite
#define cerberus_display_fill_rect fixture_display_fill_rect
#define cerberus_s3_ble_scanner_get_status fixture_base_get_status
#define cerberus_s3_ble_scanner_stop fixture_base_stop
#define draw_detail_page fixture_base_draw_detail_page
#define CERBERUS_FINDMY_MONITOR_DEFER_IMPLEMENTATION 1
#include "s3_findmy_monitor_ui_check.c"
#undef CERBERUS_FINDMY_MONITOR_DEFER_IMPLEMENTATION
#undef draw_detail_page
#undef cerberus_s3_ble_scanner_get_status
#undef cerberus_s3_ble_scanner_stop
#undef cerberus_display_fill_rect
#undef main
#pragma pop_macro("main")
static unsigned fixture_detail_draws;
static esp_err_t draw_detail_page(void)
{++fixture_detail_draws;return fixture_base_draw_detail_page();}
/* Injectable scanner boundaries for the actual Fox lifecycle. The inherited
 * Find My fixture retains its original counted boundary functions. */
static esp_err_t fox_status_error, fox_stop_error, fox_start_error;
static bool fox_hold_running;
static unsigned fox_host_epoch;
/* Exercise the production scan renderer's failed-write/cache recovery path.
 * The inherited monitor keeps its own framebuffer calls unchanged. */
static unsigned wifi_ui_fill_calls, wifi_ui_fail_fill_after, wifi_ui_full_repairs;
esp_err_t cerberus_display_fill_rect(uint16_t x,uint16_t y,uint16_t w,uint16_t h,uint16_t color)
{
    ++wifi_ui_fill_calls;
    if(wifi_ui_fail_fill_after && !--wifi_ui_fail_fill_after)return ESP_FAIL;
    if(x==0 && y==48 && w==320 && h==363)++wifi_ui_full_repairs;
    return fixture_display_fill_rect(x,y,w,h,color);
}
#include "scan_display_store.h"
#include "s3_ble_list.h"
#include "s3_ble_recent.h"
#include "s3_wifi_list.h"
#include "sentinel_scan_session.h"
#include "wifi_readout_diagnostics.h"
#include "sentinel_scan_view.h"
#include "ble_analyzer.h"
#include "s3_ble_tracker.h"
#include "ble_flock_oui.h"
#include "discovery_back_geometry.h"
#include "discovery_live_types.h"
#include "s3_ble_discovery_session.h"
#include "s3_ble_discovery_inventory.h"
#include "dual_ble_scan.h"
#include "dual_ble_list.h"
#include "ble_discovery_list.h"
#include "dual_ble_family.h"
#define UI_BLE_MODE_C5 99U

enum { UI_WIFI_MODE_LIST, UI_WIFI_MODE_MENU, UI_WIFI_MODE_BASELINE, UI_WIFI_MODE_STATS,
    UI_WIFI_MODE_DETAIL, UI_WIFI_MODE_TARGET, UI_WIFI_MODE_PASSWORD, UI_WIFI_MODE_CONNECTION,
    UI_WIFI_MODE_STATIONS, UI_WIFI_MODE_OBSERVED_CLIENTS, UI_WIFI_MODE_AP_JOB };
enum { UI_IEEE802154_MODE_ENERGY, UI_IEEE802154_MODE_BEACONS, UI_IEEE802154_MODE_ACTIVITY,
    UI_IEEE802154_MODE_MENU, UI_IEEE802154_MODE_ASSESSMENT };
#define UI_PAGE_FIELD_MENU 90
static struct { bool attack; } field_menu;
static unsigned wifi_mode,ieee802154_mode,wifi_target_count,wifi_selected_index,wifi_analysis_generation;
static bool wifi_attack_selecting,wifi_selection_valid,c5_online=true,live_radio_busy;
static bool live_owned_wifi_observation;
static esp_err_t wifi_analysis_error,wifi_trust_error,wifi_connection_error;
static cerberus_c5_wifi_result_t wifi_selected_result;
static unsigned relation_selection_calls;
static unsigned wifi_ui_connection_queries,wifi_ui_target_toggles,wifi_ui_target_index;
static bool relation_selection_ordinary;
static cerberus_c5_wifi_result_t relation_selected_ap;
static void wifi_relation_bind_selection(bool ordinary)
{
    assert(current_page==UI_PAGE_WIFI);
    ++relation_selection_calls;relation_selection_ordinary=ordinary;
    relation_selected_ap=wifi_selected_result;
}
static cerberus_scan_display_store_t store,*live_store=&store;
static cerberus_findmy_monitor_record_t observations[32];
static live_scan_record_t live_records[SENTINEL_SCAN_TOOL_COUNT];
static uint32_t live_capture_pending;
static uint8_t live_ble_focus_bits[32],live_ble_indexes[32];
static size_t live_ble_filtered_count;
static bool live_wifi_recent_only;
static cerberus_s3_wifi_sort_t live_wifi_sort;
static cerberus_s3_wifi_band_t live_wifi_band;
static uint64_t wifi_relation_publication;
static uint8_t wifi_relation_generation;
static live_wifi_paint_map_t live_wifi_built,live_wifi_painted;
static uint64_t live_wifi_view_token;
static uint8_t live_wifi_indexes[32];
static size_t live_wifi_filtered_count;
static cerberus_c5_status_t c5_status;
static uint16_t live_page[SENTINEL_SCAN_TOOL_COUNT];
static bool live_ble_order_dirty;
static bool live_ble_order_discovery;
static uint16_t live_ble_order_count;
static uint64_t live_ble_order_epoch,live_ble_order_revision;
static bool live_ble_recent_only;
static uint32_t live_ble_visible_mask=UINT32_MAX;
static int64_t live_ble_published_ms;
static cerberus_s3_ble_filter_t live_ble_order_filter;
static cerberus_s3_ble_sort_t live_ble_sort,live_ble_order_sort;
static char live_names[4][40],live_details[4][52],live_ble_filter_label[19],live_subtitle[52],live_hint[52];
static uint32_t live_frame,live_last_poll;
static int8_t live_energy[16];
static uint8_t live_activity[16][CERBERUS_IEEE802154_ACTIVITY_FRAME_TYPE_COUNT];
static sentinel_scan_view_t live_view;
static sentinel_scan_session_t session;
#define live_ble_session session
static sentinel_scan_session_t live_session;
static bool live_ble_session_initialized,ui_capability_blocked,live_scan_observation_valid,live_ble_observation_valid;
static cerberus_wifi_scan_status_payload_t wifi_scan_status;
static uint8_t wifi_snapshot_band;
static struct { uint8_t state; } wifi_defense_status;
static unsigned live_commands,live_observations;
static esp_err_t live_command_error;
static bool watch_ble_requested(void){return false;}
static bool live_observe(uint32_t now){(void)now;++live_observations;return false;}
/* This route/presentation fixture supplies frozen AP banks through seed helpers;
 * its live_observe boundary never queues an AP transfer. The dedicated scan
 * display suite owns actual incremental transfer, cancellation and SPI gates. */
static bool readout_busy;
static esp_err_t readout_error;
static unsigned readout_resets;
static bool live_wifi_capture_busy(void){return readout_busy;}
static esp_err_t live_wifi_capture_error(void){return readout_error;}
static bool live_wifi_capture_defers_c5(void){return false;}
static bool mesh_ui_busy(void){return false;}
/* Packet Count is inactive on these ordinary BLE and Recon routes. */
static bool wifi_packet_count_ui_busy(void){return false;}
static bool channel_analyzer_ui_busy(void){return false;}
static bool wifi_fox_hunt_ui_busy(void){return false;}
static bool saved_ble_lookup_ui_busy(void){return false;}
static bool live_wifi_capture_tick(uint32_t now,bool allowed,cerberus_wifi_readout_cancel_reason_t reason)
{(void)now;(void)allowed;(void)reason;return false;}
static void live_wifi_capture_reset_as(cerberus_wifi_readout_cancel_reason_t reason)
{assert(reason==CERBERUS_WIFI_READOUT_CANCEL_SUPERSEDED);++readout_resets;readout_error=ESP_OK;}
static esp_err_t live_execute(sentinel_scan_command_t command){(void)command;++live_commands;return live_command_error;}
static esp_err_t refresh_wifi_defense_survey(void){return ESP_OK;}
static sentinel_scan_tool_t live_page_tool(void){return current_page==UI_PAGE_BLE&&ble_mode==UI_BLE_MODE_LIST?SENTINEL_SCAN_BLE:
    current_page==UI_PAGE_WIFI&&wifi_mode==UI_WIFI_MODE_LIST?SENTINEL_SCAN_WIFI:SENTINEL_SCAN_NONE;}
static sentinel_scan_session_t *live_session_for(sentinel_scan_tool_t tool){return tool==SENTINEL_SCAN_BLE?&session:&live_session;}
typedef enum {CERBERUS_WIFI_TRUST_UNKNOWN,CERBERUS_WIFI_TRUST_EXACT,CERBERUS_WIFI_TRUST_MISMATCH} cerberus_wifi_trust_state_t;
static cerberus_wifi_trust_state_t cerberus_settings_wifi_trust_state(const char *s,const uint8_t *b,uint8_t security)
{(void)s;(void)b;(void)security;return CERBERUS_WIFI_TRUST_UNKNOWN;}
static const char *wifi_security_text(uint8_t security){(void)security;return "OPEN";}
static bool cerberus_settings_ble_is_favorite(const uint8_t *address,uint8_t type){(void)address;(void)type;return false;}
#define wifi_targets_find(...) (-1)
#define wifi_targets_clear() ((void)0)
#define wifi_target_open_attacks() ((void)0)
static bool fixture_wifi_target_toggle_selection(const cerberus_c5_wifi_result_t *row,uint8_t index)
{assert(row==&store.wifi[index]);++wifi_ui_target_toggles;wifi_ui_target_index=index;return true;}
#define wifi_target_toggle_selection fixture_wifi_target_toggle_selection
#define wifi_target_finish_selection() ((void)(wifi_mode=UI_WIFI_MODE_DETAIL))
#define c5_spi_link_get_wifi_connection_status(...) (++wifi_ui_connection_queries,ESP_OK)
static bool central_active;
static bool ble_connection_is_active(void){return central_active;}
static esp_err_t draw_current_page(void){return ESP_OK;}
static void return_to_launcher(void){current_page=UI_PAGE_OTHER;}
static bool ble_analyzer_exit_pending,ble_analyzer_retry_requested,ble_analyzer_start_pending;
static bool ble_analyzer_started,ble_analyzer_s3_details=true;
static esp_err_t ble_analyzer_error;
static uint32_t ble_analyzer_start_requested_ms,ble_analyzer_exit_started_ms,ble_analyzer_last_tick_ms,ble_analyzer_last_draw_ms;
static bool ble_analyzer_run_requested,ble_analyzer_s3_start_pending,ble_analyzer_s3_owned;
static bool ble_analyzer_s3_receiving_cached,ble_analyzer_cleanup_timed_out;
static esp_err_t ble_analyzer_barrier_error;
static uint32_t ble_analyzer_s3_start_requested_ms;
static bool ble_analyzer_requested(void);
static bool ble_analyzer_ready(void);
static bool ble_analyzer_s3_retryable(void);
static bool ble_analyzer_c5_retryable(void);
static bool ble_analyzer_s3_receiving(void);
static const char *ble_analyzer_s3_state(void);
static esp_err_t ble_analyzer_s3_error(void);
static void ble_analyzer_request_s3_retry(uint32_t);
static void ble_analyzer_request_c5_retry(uint32_t);
static void ble_analyzer_request_stop(uint32_t,bool);
static void ble_analyzer_open(uint32_t);
static bool ble_fox_hunt_exit_pending,ble_fox_hunt_retry_requested;
static bool ble_fox_hunt_from_c5,ble_fox_hunt_start_pending;
static bool ble_fox_hunt_s3_attached,ble_fox_hunt_s3_started;
static bool ble_fox_hunt_s3_start_pending,ble_fox_hunt_cleanup_timed_out;
static bool ble_fox_hunt_s3_owned,ble_fox_hunt_s3_receiving_cached;
static uint32_t ble_fox_hunt_s3_start_requested_ms;
static esp_err_t ble_fox_hunt_barrier_error;
static uint32_t ble_fox_hunt_start_requested_ms;
static cerberus_c5_ble_result_t ble_fox_hunt_target;
static bool companion_released=true;
static bool ble_findmy_monitor_requested(void);
static bool ble_findmy_monitor_available(void);
static void ble_findmy_monitor_open(void);
static void ble_findmy_monitor_return(void);
static void ble_findmy_request_stop(bool exit,bool select);
static void ble_findmy_monitor_tap(uint16_t x,uint16_t y);
static void ble_select_frozen_live(const cerberus_findmy_monitor_record_t *record);
static cerberus_dual_ble_scan_t companion_store;
static unsigned companion_syncs,companion_retries,companion_exchanges;
static int companion_requested_mode;
static uint8_t companion_requested_family;
static uint64_t companion_sync_clock;
static bool dual_ble_live_discovery_released(void);
static uint64_t live_ble_lease_epoch;
static bool live_ble_sleeping;
static bool live_ble_recon_released(void){return live_ble_lease_epoch==0U;}
static bool dual_ble_live_c5_retryable(void);
static bool ble_discovery_s3_retryable(void);
static bool dual_ble_ui_released(void);
static esp_err_t draw_button(uint16_t,uint16_t,uint16_t,uint16_t,const char *);
#include "discovery_companion_impl.h"
/* Small bound peer for actual C5 coordinator lifecycle, not a second UI
 * scheduler. Version 1 deliberately avoids inventory transfers in these
 * ownership tests; existing controller suites cover every snapshot version. */
static bool companion_actual_owner;
static struct {
    uint64_t boot,epoch;
    uint32_t job,delay_sample_job;
    unsigned starts,stops,releases,frames,samples;
    esp_err_t start_error,sample_error,stop_error;
    uint8_t mode,version,address[6],address_type;
    bool attached,owned,unsupported,hold_stop;
} composite_peer;
esp_err_t c5_spi_link_dual_ble_exchange(uint8_t command,const void *request,
    size_t request_size,void *response,size_t response_size)
{
    ++composite_peer.frames;
    if(command==CERBERUS_SPI_COMMAND_DUAL_BLE_INFO){
        assert(!request_size&&response_size==sizeof(cerberus_dual_ble_info_t));
        if(composite_peer.unsupported)return ESP_ERR_NOT_SUPPORTED;
        cerberus_dual_ble_info_t out={.boot_id=composite_peer.boot,.next_job_id=composite_peer.starts+1U,
            .version=composite_peer.version?composite_peer.version:1,.capacity=32,.raw_bytes=31,.flags=CERBERUS_DUAL_BLE_READY|
                (composite_peer.attached?CERBERUS_DUAL_BLE_ATTACHED:0U)};
        memcpy(response,&out,sizeof(out));return ESP_OK;
    }
    assert(request&&request_size>=sizeof(cerberus_dual_ble_binding_t)&&response_size==sizeof(cerberus_dual_ble_status_t));
    const cerberus_dual_ble_binding_t *binding=request;
    assert(binding->boot_id==composite_peer.boot);
    if(command==CERBERUS_SPI_COMMAND_DUAL_BLE_TRACK_STATUS){
        assert(binding->job_id==composite_peer.job&&composite_peer.attached&&composite_peer.mode==CERBERUS_DUAL_BLE_TRACK);
        ++composite_peer.samples;
        if(composite_peer.sample_error)return composite_peer.sample_error;
        cerberus_dual_ble_track_t out={.epoch=composite_peer.epoch,.job_id=composite_peer.job,
            .reports=composite_peer.samples,.age_ms=17U,.rssi=-61,
            .flags=CERBERUS_DUAL_BLE_SEEN|CERBERUS_DUAL_BLE_RSSI_VALID};
        memcpy(response,&out,sizeof(out));return ESP_OK;
    }
    if(command==CERBERUS_SPI_COMMAND_DUAL_BLE_ANALYZER_SAMPLE){
        assert(binding->job_id==composite_peer.job&&composite_peer.attached&&
            composite_peer.mode==CERBERUS_DUAL_BLE_ANALYZER&&response_size==sizeof(cerberus_dual_ble_analyzer_sample_t));
        ++composite_peer.samples;
        if(composite_peer.sample_error)return composite_peer.sample_error;
        cerberus_dual_ble_analyzer_sample_t out={.epoch=composite_peer.epoch,.job_id=composite_peer.job,
            .reports=composite_peer.samples*7U,.elapsed_ms=composite_peer.samples*500U,
            .flags=CERBERUS_DUAL_BLE_ANALYZER_VALID|(composite_peer.owned?0U:CERBERUS_DUAL_BLE_ANALYZER_FINAL),.rssi=127};
        if(composite_peer.delay_sample_job==composite_peer.job){out.reports=out.elapsed_ms=0U;out.flags=0U;}
        memcpy(response,&out,sizeof(out));return ESP_OK;
    }
    if(command==CERBERUS_SPI_COMMAND_DUAL_BLE_START){
        const cerberus_dual_ble_start_t *start=request;
        assert(request_size==sizeof(*start)&&cerberus_dual_ble_start_valid(start));
        assert((start->mode==CERBERUS_DUAL_BLE_DISCOVERY||start->mode==CERBERUS_DUAL_BLE_ANALYZER||
            start->mode==CERBERUS_DUAL_BLE_TRACK)&&!composite_peer.attached);
        composite_peer.mode=start->mode;composite_peer.samples=0U;
        memcpy(composite_peer.address,start->address,6U);composite_peer.address_type=start->address_type;
        composite_peer.job=start->job_id;++composite_peer.starts;++composite_peer.epoch;
        composite_peer.attached=true;composite_peer.owned=composite_peer.start_error==ESP_OK;
    }else{
        assert(binding->job_id==composite_peer.job&&composite_peer.attached);
        if(command==CERBERUS_SPI_COMMAND_DUAL_BLE_STOP){++composite_peer.stops;if(composite_peer.stop_error)return composite_peer.stop_error;if(!composite_peer.hold_stop)composite_peer.owned=false;}
        else if(command==CERBERUS_SPI_COMMAND_DUAL_BLE_RELEASE){++composite_peer.releases;assert(!composite_peer.owned);composite_peer.attached=false;}
        else assert(command==CERBERUS_SPI_COMMAND_DUAL_BLE_STATUS);
    }
    cerberus_dual_ble_status_t out={.epoch=composite_peer.epoch,.job_id=composite_peer.job,.version=composite_peer.version?composite_peer.version:1,
        .generation=(uint8_t)composite_peer.starts,.state=composite_peer.start_error&&composite_peer.attached?CERBERUS_BLE_SCAN_ERROR:composite_peer.owned?CERBERUS_BLE_SCAN_RUNNING:CERBERUS_BLE_SCAN_COMPLETE,
        .flags=(composite_peer.attached?CERBERUS_DUAL_BLE_ATTACHED:0U)|(composite_peer.owned?CERBERUS_DUAL_BLE_OWNED:0U)};
    if(composite_peer.attached)out.error_code=composite_peer.start_error;
    memcpy(response,&out,sizeof(out));return ESP_OK;
}
#include "discovery_companion_owner_impl.h"
static void dual_ble_ui_retry(void){++companion_retries;if(companion_actual_owner)fixture_actual_dual_ble_ui_retry();}
static bool dual_ble_ui_released(void){return companion_actual_owner?fixture_actual_dual_ble_ui_released():companion_released;}
static void dual_ble_ui_cancel(void){if(companion_actual_owner){fixture_actual_dual_ble_ui_cancel();return;}companion_requested_mode=dual_ble_ui_mode=-1;}
static void dual_ble_ui_sync(int mode,const uint8_t *address,uint8_t type,uint64_t now)
{if(companion_actual_owner){fixture_actual_dual_ble_ui_sync(mode,address,type,now);return;}assert(mode>=-1&&mode<=CERBERUS_DUAL_BLE_ANALYZER);companion_requested_mode=dual_ble_ui_mode=mode;companion_sync_clock=now;++companion_syncs;
 if(mode==CERBERUS_DUAL_BLE_TRACK){assert(address);memcpy(dual_ble_ui_target,address,6);dual_ble_ui_type=type;}
 (void)dual_ble_ui_attempted;(void)dual_ble_ui_family;}
static void dual_ble_ui_sync_family(uint8_t family,uint64_t now)
{if(companion_actual_owner){fixture_actual_dual_ble_ui_sync_family(family,now);return;}assert(family<CERBERUS_S3_BLE_FILTER_COUNT);companion_requested_family=family;
 companion_requested_mode=dual_ble_ui_mode=family?CERBERUS_DUAL_BLE_FAMILY:CERBERUS_DUAL_BLE_DISCOVERY;
 dual_ble_ui_family=family;dual_ble_ui_response_mode=dual_ble_ui_response_requests;(void)now;++companion_syncs;}
static bool dual_ble_live_enabled(void);
static bool dual_ble_live_discovery_released(void);
typedef struct { bool busy; } cerberus_dual_wifi_status_t;
static bool analyzer_wifi_busy;
static void cerberus_dual_wifi_scan_get_status(cerberus_dual_wifi_status_t *s){s->busy=analyzer_wifi_busy;}
static bool dual_wifi_ui_busy(const cerberus_dual_wifi_status_t *s){return s->busy;}
static unsigned analyzer_diagnostic_publishes;
static void dual_ble_publish_diagnostics(uint64_t now,bool asleep){(void)now;(void)asleep;++analyzer_diagnostic_publishes;}
static void dual_ble_live_tap(uint16_t x,uint16_t y);
static unsigned archive_save_calls;
static bool archive_save_selected;
static cerberus_dual_ble_row_t archive_saved_c5;
static cerberus_dual_ble_provenance_t archive_saved_provenance;
static void ble_archive_save_c5(bool selected)
{++archive_save_calls;archive_save_selected=selected;archive_saved_c5=dual_ble_ui_selected;
 archive_saved_provenance=dual_ble_ui_selected_provenance;}
static esp_err_t draw_ble(void){return ESP_OK;}
static bool ble_fox_hunt_open_peer(const cerberus_c5_ble_result_t *,bool);
static bool saved_ble_lookup_fox_return(bool asleep){(void)asleep;return false;}
static bool dual_ble_live_prepare_fox(uint32_t,bool);
static void ble_fox_hunt_finish_exit(bool,bool);
static bool ble_fox_hunt_s3_retryable(void);
static bool ble_fox_hunt_c5_retryable(void);
static void ble_fox_hunt_request_stop(uint32_t,bool);
static void ble_fox_hunt_request_s3_retry(uint32_t);
static void ble_fox_hunt_request_c5_retry(void);
static void ble_fox_hunt_attach_s3(void);
static esp_err_t ble_fox_hunt_error;
static uint32_t ble_fox_hunt_exit_started_ms,ble_fox_hunt_last_tick_ms,ble_fox_hunt_last_draw_ms;
static ui_page_t ble_session_exit_page,tool_parent_page;
static bool learning_tool_active;
#define UI_CAP_BLE_GATT 1U
static unsigned ui_capability_map=UI_CAP_BLE_GATT;
static void ble_sound_open(bool all){(void)all;assert(!"Unexpected Sound action in observational test");}
static unsigned track_begins,track_ends,passive_starts;
static uint8_t track_address[6],track_type;
static esp_err_t track_error;
static cerberus_ble_analyzer_view_t analyzer_view;
typedef unsigned cerberus_s3_ble_owner_t;
static unsigned analyzer_begins,analyzer_ends;
#include "analyzer_native_state.h"
#include "ble_analyzer_ui_boundary.h"
static uint16_t analyzer_button_x,analyzer_button_y,analyzer_button_w,analyzer_button_h;
static unsigned analyzer_body_buttons,analyzer_stop_buttons,analyzer_stopping_buttons,analyzer_retry_buttons;
static esp_err_t draw_button(uint16_t x,uint16_t y,uint16_t w,uint16_t h,const char *s)
{
    if(current_page==UI_PAGE_BLE&&ble_mode==UI_BLE_MODE_ANALYZER){
        if(!strcmp(s,"STOP ANALYZER"))++analyzer_stop_buttons;
        if(!strcmp(s,"STOPPING"))++analyzer_stopping_buttons;
        if(!strcmp(s,"RETRY S3")||!strcmp(s,"RETRY C5")||!strcmp(s,"RETRY START"))++analyzer_retry_buttons;
        if(y<UI_BACK_Y){
            ++analyzer_body_buttons;
            analyzer_button_x=x;analyzer_button_y=y;analyzer_button_w=w;analyzer_button_h=h;
        }
    }
    return draw_small_button(x,y,w,h,s);
}
#include "track_native_state.h"
#include "ble_fox_ui_boundary.h"
esp_err_t cerberus_display_draw_mono_bitmap(uint16_t x,uint16_t y,uint16_t w,uint16_t h,const uint8_t *data,uint16_t fg,uint16_t bg)
{
    bounds(x,y,w,h);
    for(unsigned row=0;row<h;++row)for(unsigned col=0;col<w;++col)
        screen[(y+row)*320+x+col]=data[row*((w+7)/8)+col/8]&(0x80>>(col%8))?fg:bg;
    return ESP_OK;
}
#ifdef CERBERUS_DISCOVERY_NATIVE_TEST
#include "s3_ble_discovery_native_fakes.h"
static unsigned discovery_ui_allocations;
static unsigned unified_sort_calls,unified_classify_calls,unified_response_classify_calls;
static uint8_t measured_unified_order(const cerberus_s3_ble_discovery_inventory_t *s3,
    const cerberus_dual_ble_row_t *c5,uint8_t count,cerberus_ble_discovery_list_source_t source,
    cerberus_ble_discovery_list_sort_t sort,uint32_t s3_mask,uint32_t c5_mask,
    cerberus_ble_discovery_list_ref_t refs[CERBERUS_BLE_DISCOVERY_LIST_CAPACITY])
{++unified_sort_calls;return cerberus_ble_discovery_list_order_masked(s3,c5,count,source,sort,s3_mask,c5_mask,refs);}
static bool measured_family_classify(const cerberus_dual_ble_row_t *row,
    const cerberus_dual_ble_provenance_t *proof,cerberus_dual_ble_family_evidence_t *out)
{++unified_classify_calls;return cerberus_dual_ble_family_classify(row,proof,out);}
static bool measured_response_classify(const cerberus_dual_ble_row_t *row,
    const cerberus_dual_ble_provenance_t *proof,cerberus_dual_ble_family_evidence_t *out)
{++unified_response_classify_calls;return cerberus_dual_ble_response_classify(row,proof,out);}
static void *discovery_ui_calloc(size_t n,size_t size,unsigned capabilities)
{++discovery_ui_allocations;return heap_caps_calloc(n,size,capabilities);}
#define heap_caps_calloc discovery_ui_calloc
#define cerberus_ble_discovery_list_order_masked measured_unified_order
#define cerberus_dual_ble_family_classify measured_family_classify
#define cerberus_dual_ble_response_classify measured_response_classify
#include "s3_ble_discovery_live_ui.h"
#undef cerberus_ble_discovery_list_order_masked
#undef cerberus_dual_ble_family_classify
#undef cerberus_dual_ble_response_classify
#undef heap_caps_calloc
#else
/* This suite intentionally retains the legacy/frozen-bank presentation path
 * used by selection/Watch consumers. The separate native live suite includes
 * the actual new ordinary-list header and tests its scheduler and ownership. */
#include "discovery_native_types.h"
static ble_discovery_live_store_t *ble_discovery_live;
static esp_err_t ble_discovery_open_error;
static bool ble_discovery_page(void){return false;}
static bool ble_discovery_ordinary_page(void){return false;}
static bool ble_discovery_requested(void){return false;}
static bool ble_discovery_s3_retryable(void){return false;}
static esp_err_t ble_discovery_retry_s3(void){return ESP_ERR_INVALID_STATE;}
static esp_err_t ble_discovery_request_start(void){return ESP_ERR_INVALID_STATE;}
static void ble_discovery_request_stop(void){dual_ble_ui_cancel();}
static void ble_discovery_request_destination(uint8_t dest){(void)dest;}
static void ble_discovery_cancel_navigation(void){dual_ble_ui_cancel_selection();}
static bool ble_discovery_presenting(void){return false;}
static void ble_discovery_leave(void){assert(!ble_discovery_live);}
static void ble_discovery_tick(uint64_t now,bool asleep){(void)now;(void)asleep;}
static void ble_discovery_select_painted(unsigned row){(void)row;assert(!"native selection in legacy fixture");}
static void ble_discovery_toggle(void){assert(!"native toggle in legacy fixture");}
static uint32_t ble_discovery_visible_mask(uint64_t now){(void)now;return UINT32_MAX;}
static bool ble_discovery_unified(void){return false;}
static uint8_t ble_discovery_combined_order(void){assert(!"unified order in legacy fixture");return 0;}
static bool ble_discovery_build_unified_row(unsigned row,size_t position,uint64_t now)
{(void)row;(void)position;(void)now;assert(!"unified row in legacy fixture");return false;}
static esp_err_t ble_discovery_monitor_start(void){return ESP_ERR_NO_MEM;}
static bool ble_discovery_monitor_freeze_selection(unsigned row){(void)row;return false;}
static bool ble_discovery_monitor_apply_selection(void){return false;}
#endif
/* The inherited monitor fixture uses one generic error name. Preserve its
 * existing cases while exposing the actual TIMEOUT label for owned polling. */
static const char *view_error_name(esp_err_t error)
{return error==ESP_ERR_TIMEOUT?"ESP_ERR_TIMEOUT":error==ESP_ERR_NO_MEM?"ESP_ERR_NO_MEM":
 error==ESP_ERR_NOT_SUPPORTED?"ESP_ERR_NOT_SUPPORTED":esp_err_to_name(error);}
#define esp_err_to_name view_error_name
#include "ble_analyzer_dual_ui.h"
#include "discovery_routes_impl.h"
#include "findmy_monitor_ui_impl.h"
#undef esp_err_to_name

static void seed_list(void)
{
    live_ble_lease_epoch=0U;live_ble_sleeping=false;
    memset(&track_native,0,sizeof(track_native));
    (void)cerberus_s3_ble_scanner_track_end;(void)track_native_callback;
    (void)cerberus_s3_ble_scanner_analyzer_end;
    (void)actual_discovery_entry;(void)actual_discovery_menu_scan;(void)actual_discovery_missing_selection;
    (void)ble_discovery_cancel_navigation;
    (void)dual_ble_live_tick;
    companion_actual_owner=false;
    (void)dual_ble_ui_cancel;
    (void)draw_ble_findmy_monitor;
    ble_analyzer_s3_details=true;analyzer_wifi_busy=false;
    (void)draw_live_scan; /* Also compiled for the separate ordinary-live entry fixture. */
    (void)actual_c5_footer_tap;(void)actual_c5_footer_draw;
    (void)dual_ble_ui_select_frozen;
    memset(&companion_store,0,sizeof(companion_store));dual_ble_ui_store=&companion_store;
    companion_syncs=companion_retries=companion_exchanges=archive_save_calls=0;
    companion_released=true;dual_ble_ui_selection_pending=false;dual_ble_ui_detail=dual_ble_ui_receive_info=dual_ble_ui_from_unified=dual_ble_ui_from_monitor=false;
    dual_ble_ui_error=ESP_OK;dual_ble_ui_response_requests=dual_ble_ui_response_mode=dual_ble_ui_response_tab=false;
    reset();memset(observations,0,sizeof(observations));memset(&store,0,sizeof(store));
    ble_findmy_running=ble_findmy_stopping=ble_findmy_exit_pending=ble_findmy_select_pending=false;
    ble_findmy_available=ble_findmy_paint_valid=ble_findmy_legacy_pending=false;
    memset(live_records,0,sizeof(live_records));memset(live_ble_focus_bits,0,sizeof(live_ble_focus_bits));
    memset(live_page,0,sizeof(live_page));sentinel_scan_session_init(&session);
    session.completed=true;session.desired=SENTINEL_SCAN_BLE;ble_mode=UI_BLE_MODE_LIST;
    sentinel_scan_session_init(&live_session);live_session.completed=true;live_ble_session_initialized=true;
    live_ble_recent_only=false;live_ble_visible_mask=UINT32_MAX;live_ble_published_ms=1200;
    live_commands=live_observations=0;live_last_poll=0;
    live_capture_pending=0;live_command_error=ESP_OK;
    readout_busy=false;readout_error=ESP_OK;readout_resets=0;
    live_owned_wifi_observation=false;
    live_wifi_sort=CERBERUS_S3_WIFI_SORT_SIGNAL_DESC;live_wifi_band=CERBERUS_S3_WIFI_BAND_ALL;
    wifi_relation_publication=0;wifi_relation_generation=0;
    memset(&live_wifi_built,0,sizeof(live_wifi_built));memset(&live_wifi_painted,0,sizeof(live_wifi_painted));
    live_wifi_view_token=0;
    wifi_ui_fail_fill_after=0;
    relation_selection_calls=0;relation_selection_ordinary=false;
    wifi_ui_connection_queries=wifi_ui_target_toggles=wifi_ui_target_index=0;
    live_scan_observation_valid=live_ble_observation_valid=true;
    live_records[SENTINEL_SCAN_BLE]=(live_scan_record_t){.valid=true,.captured=true,.count=4,.saved_generation=7,.state=SENTINEL_SCAN_COMPLETE,.captured_ms=1200};
    const char *names[]={"Zeta","Ordinary","alpha","Beta"};
    const int8_t signal[]={-30,-10,-60,-80};
    for(unsigned i=0;i<4;++i){
        cerberus_findmy_monitor_record_t *r=&observations[i];
        r->peer.address[0]=(uint8_t)(0x30+i);r->peer.address[5]=0x77;r->peer.address_type=(uint8_t)(i%2);
        strcpy(r->peer.name,names[i]);r->peer.rssi=signal[i];r->peer.flags=CERBERUS_BLE_RESULT_CONNECTABLE;
        r->generation=7;r->index=(uint8_t)i;r->observed_ms=100;
        memcpy(r->advertisement.address,r->peer.address,6);r->advertisement.address_type=r->peer.address_type;
        r->advertisement.length=4;r->advertisement.data[0]=3;r->advertisement.data[1]=3;r->advertisement.data[2]=0x82;r->advertisement.data[3]=0x30;
        cerberus_s3_ble_discovery_evidence_t *e=&r->focus.discovery;
        e->generation=7;memcpy(e->address,r->peer.address,6);e->address_type=r->peer.address_type;
        e->advertisement_sequence=1;e->advertisement_observed_ms=100;e->scan_response_sequence=2;e->scan_response_observed_ms=1100;
        e->flags=CERBERUS_S3_BLE_EVIDENCE_ADV_COMPLETE|CERBERUS_S3_BLE_EVIDENCE_ADV_VALID|CERBERUS_S3_BLE_EVIDENCE_RAW_COMPLETE|CERBERUS_S3_BLE_EVIDENCE_RESPONSE_VALID;
        e->flipper_advertisement=(cerberus_ble_flipper_hints_t){CERBERUS_BLE_FLIPPER_HINT_WHITE,CERBERUS_BLE_FLIPPER_UUID16};
        e->flipper_scan_response=(cerberus_ble_flipper_hints_t){CERBERUS_BLE_FLIPPER_HINT_TRANSPARENT,CERBERUS_BLE_FLIPPER_UUID16|CERBERUS_BLE_FLIPPER_NAME};
        if(i!=1){r->focus.category_bits=CERBERUS_S3_BLE_FOCUS_FLIPPER;live_ble_focus_bits[i]=r->focus.category_bits;}
        store.ble[i]=r->peer;
    }
    observations[3].peer.name[0]='\0';store.ble[3]=observations[3].peer;
    live_ble_evidence=observations;live_ble_order_dirty=true;live_ble_sort=CERBERUS_S3_BLE_SORT_SIGNAL_DESC;
    ble_filter=CERBERUS_S3_BLE_FILTER_FIND_MY;clock_ms=8100;masked=true;live_make_view();
}

static void check_meta_ui(const char *prefix)
{
    seed_list();track_begins=track_ends=passive_starts=0;
    for(unsigned i=0;i<4;++i){
        cerberus_s3_ble_focus_t *f=&observations[i].focus;
        f->category_bits=0;live_ble_focus_bits[i]=0;
        memset(&f->discovery.flipper_advertisement,0,sizeof(f->discovery.flipper_advertisement));
        memset(&f->discovery.flipper_scan_response,0,sizeof(f->discovery.flipper_scan_response));
    }
    cerberus_s3_ble_focus_t *f=&observations[0].focus;
    assert(cerberus_ble_meta_evidence_add(&f->discovery.meta_advertisement,0xFD5F,CERBERUS_BLE_META_MFG16));
    assert(cerberus_ble_meta_evidence_add(&f->discovery.meta_advertisement,0xFEB7,CERBERUS_BLE_META_LIST128));
    f->category_bits=CERBERUS_S3_BLE_FOCUS_META_HINT;live_ble_focus_bits[0]=f->category_bits;
    f=&observations[1].focus;f->meta_suppressed=true;
    assert(cerberus_ble_meta_evidence_add(&f->discovery.meta_advertisement,0xFD5F,CERBERUS_BLE_META_MFG16));
    /* Earlier blocked evidence suppresses this row even though its current
     * source has an allowed identifier: UI must trust published categories. */
    f=&observations[2].focus;
    assert(cerberus_ble_meta_evidence_add(&f->discovery.meta_scan_response,0xFD5F,CERBERUS_BLE_META_LIST128));
    assert(cerberus_ble_meta_evidence_add(&f->discovery.meta_scan_response,0xFEB7,CERBERUS_BLE_META_DATA16));
    f->category_bits=CERBERUS_S3_BLE_FOCUS_META_HINT;live_ble_focus_bits[2]=f->category_bits;
    observations[2].advertisement.length=3;observations[2].advertisement.data[0]=2;
    observations[2].advertisement.data[1]=1;observations[2].advertisement.data[2]=6;
    ble_filter=CERBERUS_S3_BLE_FILTER_FLOCK;live_ble_order_dirty=true;live_make_view();
    handle_ble_tap(100,65);assert(ble_filter==CERBERUS_S3_BLE_FILTER_META_HINT);
    assert(live_ble_filtered_count==2&&live_ble_indexes[0]==0&&live_ble_indexes[1]==2);
    assert(!strcmp(live_badges[0],"ID PAIRS / ADV 2 / RSP 0"));
    handle_ble_tap(250,65);assert(live_ble_sort==CERBERUS_S3_BLE_SORT_NAME_ASC&&live_ble_indexes[0]==2);
    drawn[0]='\0';assert(sentinel_scan_view_draw(&live_view,0)==ESP_OK);save(prefix,"meta-list");
    cerberus_findmy_monitor_record_t expected=observations[2];
    handle_ble_tap(20,135);tick(8200);
    assert(ble_mode==UI_BLE_MODE_DETAIL&&ble_selected_scan_index==2&&ble_selected_sound_profile==CERBERUS_FINDMY_PROFILE_NONE);
    assert(!memcmp(&ble_findmy_selected,&expected,sizeof(expected)));
    memset(observations,0,sizeof(observations));memset(&store,0,sizeof(store));++scan.generation;
    drawn[0]='\0';cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND);assert(actual_hint_detail()==ESP_OK);
    assert(strstr(drawn,"ADV NO META IDENTIFIER PAIRS"));
    assert(strstr(drawn,"RSP FD5F LIST128, FEB7 DATA16 / 7s"));
    assert(!strstr(drawn,"FEB7 LIST128")&&!strstr(drawn,"FD5F DATA16"));save(prefix,"meta-frozen-detail");
    handle_ble_tap(30,300);assert(dual_ble_live_prepare_fox((uint32_t)clock_ms,false));assert(ble_mode==UI_BLE_MODE_FOX_HUNT&&track_begins==1);
    assert(!memcmp(track_address,expected.peer.address,6)&&track_type==expected.peer.address_type);
    ble_fox_hunt_tick(8500,false);clock_ms=8600;actual_back(440);hold_stop=true;
    ble_fox_hunt_tick(8800,false);assert(ble_mode==UI_BLE_MODE_FOX_HUNT&&ble_fox_hunt_exit_pending);
    owner=CERBERUS_S3_BLE_OWNER_NONE;scan.state=CERBERUS_BLE_SCAN_COMPLETE;ble_fox_hunt_tick(9100,false);
    assert(ble_mode==UI_BLE_MODE_DETAIL&&track_ends==1&&!memcmp(&ble_findmy_selected,&expected,sizeof(expected)));
    actual_back(440);assert(ble_mode==UI_BLE_MODE_LIST&&ble_filter==CERBERUS_S3_BLE_FILTER_META_HINT);

    /* Preserve all 77 exact pairs while the small display names two and
     * reports +75 MORE. No context is associated with a different ID. */
    cerberus_ble_meta_evidence_t all={0};
    for(size_t id=0;id<CERBERUS_BLE_META_ID_COUNT;++id)
        for(unsigned context=0;context<CERBERUS_BLE_META_CONTEXT_COUNT;++context)
            assert(cerberus_ble_meta_evidence_add(&all,cerberus_ble_meta_id_value(id),(cerberus_ble_meta_context_t)context));
    assert(cerberus_ble_meta_evidence_count(&all)==77);
    ble_selected_focus=(cerberus_s3_ble_focus_t){0};ble_selected_focus.meta_suppressed=true;
    ble_selected_focus.discovery.meta_advertisement=all;ble_selected_focus.discovery.meta_scan_response=all;
    clock_ms=UINT32_MAX;drawn[0]='\0';cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND);assert(actual_hint_detail()==ESP_OK);
    assert(strstr(drawn,"META FILTER SUPPRESSED FOR THIS SWEEP"));
    assert(strstr(drawn,"ADV FD5F MFG16, FD5F LIST16 +75 MORE / 4294967s"));save(prefix,"meta-77-pairs");
    assert(cerberus_ble_meta_evidence_count(&ble_selected_focus.discovery.meta_scan_response)==77);
    memset(&ble_selected_focus.discovery,0,sizeof(ble_selected_focus.discovery));
    drawn[0]='\0';assert(actual_hint_detail()==ESP_OK);assert(strstr(drawn,"SUPPRESSED FOR THIS SWEEP")&&strstr(drawn,"NO META IDENTIFIER PAIRS"));

    /* Actual Analyzer renderer retains stronger Watch text and separately
     * presents the exact latest event source/matrix/time without clipping. */
    analyzer_view=(cerberus_ble_analyzer_view_t){.active=true,.has_last=true,.last_report_valid=true,.last_scan_response=true,
        .last_meta=all,.last_seen_age_ms=UINT32_MAX,.last_rssi_dbm=-127,.last_name_length=32,
        .total_events=UINT32_MAX,.current_events=UINT16_MAX,.malformed_reports=UINT32_MAX,.unclassified_reports=UINT32_MAX,
        .evidence_kind=CERBERUS_BLE_ANALYZER_EVIDENCE_WATCH,.evidence_confidence=SENTINEL_WATCH_CONFIDENCE_HIGH,.evidence_label="FIND MY OFFLINE FINDING"};
    memset(analyzer_view.last_name,'W',sizeof(analyzer_view.last_name));ble_analyzer_started=true;ble_analyzer_start_pending=false;ble_analyzer_error=ESP_OK;
    drawn[0]='\0';cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND);assert(draw_ble_analyzer()==ESP_OK);
    assert(strstr(drawn,"HINT: FIND MY OFFLINE FINDING"));
    assert(strstr(drawn,"RSP FD5F MFG16, FD5F LIST16 +75 MORE / 4294967s"));
    assert(strstr(drawn,"META FILTER BLOCKED IN THIS REPORT"));save(prefix,"meta-analyzer-max");
    analyzer_view.last_meta=expected.focus.discovery.meta_scan_response;analyzer_view.last_scan_response=false;
    analyzer_view.evidence_kind=CERBERUS_BLE_ANALYZER_EVIDENCE_META_ID_HINT;analyzer_view.evidence_label="META IDENTIFIER";
    analyzer_view.evidence_confidence=SENTINEL_WATCH_CONFIDENCE_LOW;
    analyzer_view.last_seen_age_ms=2100;analyzer_view.last_name_length=0;analyzer_view.total_events=12;
    analyzer_view.malformed_reports=analyzer_view.unclassified_reports=0;analyzer_view.current_events=2;
    drawn[0]='\0';cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND);assert(draw_ble_analyzer()==ESP_OK);
    assert(strstr(drawn,"ADV FD5F LIST128, FEB7 DATA16 / 2s"));assert(!strstr(drawn,"BLOCKED IN THIS REPORT"));save(prefix,"meta-analyzer");
    analyzer_view.last_report_valid=false;drawn[0]='\0';assert(draw_ble_analyzer()==ESP_OK);assert(!strstr(drawn,"FD5F LIST128"));
}

static void check_ibeacon_ui(const char *prefix)
{
    seed_list();track_begins=track_ends=passive_starts=0;
    for(unsigned i=0;i<4;++i){
        cerberus_s3_ble_focus_t *f=&observations[i].focus;
        f->category_bits=0;live_ble_focus_bits[i]=0;
        memset(&f->discovery.flipper_advertisement,0,sizeof(f->discovery.flipper_advertisement));
        memset(&f->discovery.flipper_scan_response,0,sizeof(f->discovery.flipper_scan_response));
        if(i==1)continue;
        f->ibeacon_valid=true;f->ibeacon_sequence=i==2?2:1;
        f->ibeacon_scan_response=i==2;f->ibeacon_observed_ms=i==2?1100:i==3?50:100;
        for(unsigned b=0;b<16;++b)f->ibeacon.uuid[b]=(uint8_t)((i==2?0xF0:0)+b);
        f->ibeacon.major=65535;f->ibeacon.minor=i==2?65535:0;
        f->ibeacon.measured_power_dbm=i==2?127:-128;
        f->category_bits=CERBERUS_S3_BLE_FOCUS_IBEACON;live_ble_focus_bits[i]=f->category_bits;
    }
    observations[3].focus.discovery.advertisement_sequence=3;
    observations[3].focus.discovery.scan_response_sequence=0;
    observations[3].focus.discovery.scan_response_observed_ms=0;
    observations[3].focus.discovery.flags&=(uint8_t)~CERBERUS_S3_BLE_EVIDENCE_RESPONSE_VALID;
    observations[2].advertisement.length=3;observations[2].advertisement.data[0]=2;
    observations[2].advertisement.data[1]=1;observations[2].advertisement.data[2]=6;
    cerberus_s3_ble_focus_t earlier=observations[3].focus;
    assert(cerberus_s3_ble_ibeacon_is_current(&observations[0].focus));
    assert(cerberus_s3_ble_ibeacon_is_current(&observations[2].focus));
    assert(!cerberus_s3_ble_ibeacon_is_current(&earlier));
    ble_filter=CERBERUS_S3_BLE_FILTER_ALL;live_ble_order_dirty=true;live_make_view();
    handle_ble_tap(100,65);assert(ble_filter==CERBERUS_S3_BLE_FILTER_IBEACON);
    assert(live_ble_filtered_count==3&&live_ble_indexes[0]==0&&live_ble_indexes[1]==2);
    assert(!strcmp(live_badges[0],"IBEACON ADV CURRENT / 8s"));
    assert(!strcmp(live_badges[1],"IBEACON RSP CURRENT / 7s"));
    assert(!strcmp(live_badges[2],"IBEACON ADV EARLIER / 8s"));
    handle_ble_tap(250,65);assert(live_ble_sort==CERBERUS_S3_BLE_SORT_NAME_ASC);
    assert(live_ble_indexes[0]==3&&live_ble_indexes[1]==2);
    drawn[0]='\0';assert(sentinel_scan_view_draw(&live_view,0)==ESP_OK);save(prefix,"ibeacon-list");
    cerberus_findmy_monitor_record_t expected=observations[2];
    handle_ble_tap(20,200);tick(8200);
    assert(ble_mode==UI_BLE_MODE_DETAIL&&ble_selected_scan_index==2&&ble_selected_sound_profile==CERBERUS_FINDMY_PROFILE_NONE);
    assert(ble_findmy_selected.observed_ms==100&&ble_selected_focus.ibeacon_observed_ms==1100);
    memset(observations,0,sizeof(observations));memset(&store,0,sizeof(store));++scan.generation;
    drawn[0]='\0';cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND);assert(actual_hint_detail()==ESP_OK);
    assert(strstr(drawn,"IBEACON FORMAT / RSP / CURRENT / 7s"));
    assert(strstr(drawn,"IBEACON MAJOR 65535 MINOR 65535 CAL 127 dBm"));
    assert(strstr(drawn,"F0F1F2F3-F4F5-F6F7-F8F9-FAFBFCFDFEFF"));save(prefix,"ibeacon-frozen-detail");
    handle_ble_tap(30,300);assert(dual_ble_live_prepare_fox((uint32_t)clock_ms,false));assert(ble_mode==UI_BLE_MODE_FOX_HUNT&&track_begins==1);
    assert(!memcmp(track_address,expected.peer.address,6)&&track_type==expected.peer.address_type);
    ble_fox_hunt_tick(8500,false);clock_ms=8600;actual_back(440);hold_stop=true;
    ble_fox_hunt_tick(8800,false);assert(ble_mode==UI_BLE_MODE_FOX_HUNT&&ble_fox_hunt_exit_pending);
    owner=CERBERUS_S3_BLE_OWNER_NONE;scan.state=CERBERUS_BLE_SCAN_COMPLETE;ble_fox_hunt_tick(9100,false);
    assert(ble_mode==UI_BLE_MODE_DETAIL&&track_ends==1&&!memcmp(&ble_findmy_selected,&expected,sizeof(expected)));
    clock_ms=12100;drawn[0]='\0';assert(actual_hint_detail()==ESP_OK);assert(strstr(drawn,"RSP / CURRENT / 11s"));
    actual_back(440);assert(ble_mode==UI_BLE_MODE_LIST&&ble_filter==CERBERUS_S3_BLE_FILTER_IBEACON);

    /* Historical matching evidence keeps its own payload/time even when the
     * current original advertisement was replaced with unrelated bytes. */
    ble_selected_focus=earlier;clock_ms=8100;drawn[0]='\0';cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND);assert(actual_hint_detail()==ESP_OK);
    assert(strstr(drawn,"ADV / EARLIER / 8s"));assert(strstr(drawn,"CAL -128 dBm"));
    assert(strstr(drawn,"00010203-0405-0607-0809-0A0B0C0D0E0F"));save(prefix,"ibeacon-earlier-detail");
    char badge[FINDMY_BADGE_CAPACITY];cerberus_ibeacon_source_line(true,false,UINT32_MAX,true,badge,sizeof(badge));
    assert(!strcmp(badge,"IBEACON RSP EARLIER / 4294967s"));
    clock_ms=UINT32_MAX;ble_selected_focus.ibeacon_observed_ms=0;drawn[0]='\0';assert(actual_hint_detail()==ESP_OK);assert(strstr(drawn,"EARLIER / 4294967s"));

    /* Latest-event Analyzer payload uses the same event source/age as line
     * 290, retains stronger Watch text, and takes display priority over the
     * Meta004C field. The complete Meta matrix itself remains unchanged. */
    analyzer_view=(cerberus_ble_analyzer_view_t){.active=true,.has_last=true,.last_report_valid=true,.last_scan_response=true,
        .last_ibeacon=expected.focus.ibeacon,.last_ibeacon_valid=true,.last_seen_age_ms=7100,.last_rssi_dbm=-48,
        .total_events=123,.current_events=4,.evidence_kind=CERBERUS_BLE_ANALYZER_EVIDENCE_WATCH,
        .evidence_label="STRONGER WATCH HINT",.evidence_confidence=SENTINEL_WATCH_CONFIDENCE_HIGH};
    assert(cerberus_ble_meta_evidence_add(&analyzer_view.last_meta,0x004C,CERBERUS_BLE_META_MFG16));
    cerberus_ble_meta_evidence_t matrix=analyzer_view.last_meta;
    drawn[0]='\0';cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND);assert(draw_ble_analyzer()==ESP_OK);
    assert(strstr(drawn,"RSP -48 dBm / 7.1s"));assert(strstr(drawn,"HINT: STRONGER WATCH HINT / High"));
    assert(strstr(drawn,"IBEACON MAJOR 65535 MINOR 65535 CAL 127 dBm"));
    assert(strstr(drawn,"F0F1F2F3-F4F5-F6F7-F8F9-FAFBFCFDFEFF"));assert(!strstr(drawn,"004C MFG16"));
    assert(!memcmp(&matrix,&analyzer_view.last_meta,sizeof(matrix)));save(prefix,"ibeacon-analyzer");
    analyzer_view.last_ibeacon.measured_power_dbm=-128;analyzer_view.last_seen_age_ms=UINT32_MAX;
    analyzer_view.total_events=analyzer_view.malformed_reports=analyzer_view.unclassified_reports=UINT32_MAX;
    analyzer_view.last_name_length=32;memset(analyzer_view.last_name,'W',32);drawn[0]='\0';cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND);assert(draw_ble_analyzer()==ESP_OK);
    assert(strstr(drawn,"CAL -128 dBm"));save(prefix,"ibeacon-analyzer-max");
    analyzer_view.last_ibeacon_valid=false;drawn[0]='\0';assert(draw_ble_analyzer()==ESP_OK);
    assert(!strstr(drawn,"F0F1F2F3")&&strstr(drawn,"004C MFG16"));
    analyzer_view.last_ibeacon_valid=true;analyzer_view.last_report_valid=false;drawn[0]='\0';assert(draw_ble_analyzer()==ESP_OK);
    assert(!strstr(drawn,"F0F1F2F3")&&!strstr(drawn,"004C MFG16"));
}

static void check_flock_ui(const char *prefix)
{
    seed_list();track_begins=track_ends=passive_starts=0;
    for(unsigned i=0;i<4;++i){
        cerberus_s3_ble_focus_t *f=&observations[i].focus;f->category_bits=0;live_ble_focus_bits[i]=0;
        memset(&f->discovery.flipper_advertisement,0,sizeof(f->discovery.flipper_advertisement));
        memset(&f->discovery.flipper_scan_response,0,sizeof(f->discovery.flipper_scan_response));
        f->discovery.flock_advertisement.facts=CERBERUS_BLE_FLOCK_NO_LOCAL_NAME;
    }
    cerberus_s3_ble_focus_t *f=&observations[0].focus;
    f->discovery.flock_advertisement.facts=CERBERUS_BLE_FLOCK_FS_BATTERY;
    f->discovery.flock_scan_response.facts=CERBERUS_BLE_FLOCK_MFG_09C8|CERBERUS_BLE_FLOCK_MFG_PAYLOAD|CERBERUS_BLE_FLOCK_NO_LOCAL_NAME;
    f=&observations[1].focus;observations[1].peer.address_type=0;
    observations[1].peer.address[5]=0x58;observations[1].peer.address[4]=0x8E;observations[1].peer.address[3]=0x81;
    memcpy(observations[1].advertisement.address,observations[1].peer.address,6);observations[1].advertisement.address_type=0;
    memcpy(f->discovery.address,observations[1].peer.address,6);f->discovery.address_type=0;
    f->flock_oui_clue=cerberus_ble_flock_oui_hint(observations[1].peer.address,0);assert(f->flock_oui_clue);
    store.ble[1]=observations[1].peer;
    f=&observations[2].focus;
    f->discovery.flock_scan_response.facts=CERBERUS_BLE_FLOCK_PENGUIN|CERBERUS_BLE_FLOCK_MFG_09C8|CERBERUS_BLE_FLOCK_MFG_PAYLOAD;
    observations[2].advertisement.length=3;observations[2].advertisement.data[0]=2;observations[2].advertisement.data[1]=1;observations[2].advertisement.data[2]=6;
    for(unsigned i=0;i<3;++i){observations[i].focus.category_bits=CERBERUS_S3_BLE_FOCUS_FLOCK;live_ble_focus_bits[i]=CERBERUS_S3_BLE_FOCUS_FLOCK;}
    cerberus_s3_ble_focus_t separate=observations[0].focus,oui_only=observations[1].focus;
    uint8_t oui_address[6];memcpy(oui_address,observations[1].peer.address,6);
    ble_filter=CERBERUS_S3_BLE_FILTER_FLIPPER;live_ble_order_dirty=true;live_make_view();
    handle_ble_tap(100,65);assert(ble_filter==CERBERUS_S3_BLE_FILTER_FLOCK);
    assert(live_ble_filtered_count==3&&live_ble_indexes[0]==1&&live_ble_indexes[1]==0&&live_ble_indexes[2]==2);
    assert(!strcmp(live_badges[0],"ADV NONE / RSP NONE / OUI"));
    assert(!strcmp(live_badges[1],"ADV SIG / RSP CLUE"));
    assert(!strcmp(live_badges[2],"ADV NONE / RSP CORROB"));
    handle_ble_tap(250,65);assert(live_ble_sort==CERBERUS_S3_BLE_SORT_NAME_ASC&&live_ble_indexes[0]==2);
    drawn[0]='\0';assert(sentinel_scan_view_draw(&live_view,0)==ESP_OK);save(prefix,"flock-list");
    cerberus_findmy_monitor_record_t expected=observations[2];
    handle_ble_tap(20,135);tick(8200);assert(ble_mode==UI_BLE_MODE_DETAIL&&ble_selected_scan_index==2);
    assert(ble_selected_sound_profile==CERBERUS_FINDMY_PROFILE_NONE&&!memcmp(&ble_findmy_selected,&expected,sizeof(expected)));
    memset(observations,0,sizeof(observations));memset(&store,0,sizeof(store));++scan.generation;
    drawn[0]='\0';cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND);assert(actual_hint_detail()==ESP_OK);
    assert(strstr(drawn,"ADV NO QUALIFYING FLOCK FORMAT"));assert(strstr(drawn,"RSP CORROB PENGUIN+09C8DATA / 7s"));save(prefix,"flock-frozen-detail");
    handle_ble_tap(30,300);assert(dual_ble_live_prepare_fox((uint32_t)clock_ms,false));assert(ble_mode==UI_BLE_MODE_FOX_HUNT&&track_begins==1);
    assert(!memcmp(track_address,expected.peer.address,6)&&track_type==expected.peer.address_type);
    ble_fox_hunt_tick(8500,false);clock_ms=8600;actual_back(440);hold_stop=true;ble_fox_hunt_tick(8800,false);
    assert(ble_mode==UI_BLE_MODE_FOX_HUNT&&ble_fox_hunt_exit_pending);owner=CERBERUS_S3_BLE_OWNER_NONE;scan.state=CERBERUS_BLE_SCAN_COMPLETE;ble_fox_hunt_tick(9100,false);
    assert(ble_mode==UI_BLE_MODE_DETAIL&&track_ends==1&&!memcmp(&ble_findmy_selected,&expected,sizeof(expected)));
    actual_back(440);assert(ble_mode==UI_BLE_MODE_LIST&&ble_filter==CERBERUS_S3_BLE_FILTER_FLOCK);

    /* A name in ADV and company payload in RSP never become CORROB. */
    ble_selected_focus=separate;clock_ms=8100;drawn[0]='\0';cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND);assert(actual_hint_detail()==ESP_OK);
    assert(strstr(drawn,"ADV SIG FS BATTERY NAME / 8s"));assert(strstr(drawn,"RSP CLUE 09C8+NO NAME +1 FACTS / 7s"));
    assert(!strstr(drawn,"ADV CORROB")&&!strstr(drawn,"RSP CORROB"));save(prefix,"flock-separate-sources");
    ble_selected_focus=oui_only;memcpy(ble_selected_result.address,oui_address,6);masked=true;
    drawn[0]='\0';cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND);assert(actual_hint_detail()==ESP_OK);
    assert(strstr(drawn,"ADDRESS OUI CLUE / PREFIX MASKED")&&!strstr(drawn,"58:8E:81"));save(prefix,"flock-oui-masked");
    masked=false;drawn[0]='\0';assert(actual_hint_detail()==ESP_OK);assert(strstr(drawn,"ADDRESS OUI 58:8E:81 / CLUE ONLY"));
    /* General detail gives real payloads priority over an address-only clue. */
    ble_filter=CERBERUS_S3_BLE_FILTER_ALL;ble_selected_focus.discovery.findmy_advertisement=(cerberus_ble_findmy_hints_t){CERBERUS_BLE_FINDMY_HINT_FMNA,CERBERUS_BLE_FINDMY_UUID16};
    drawn[0]='\0';assert(actual_hint_detail()==ESP_OK);assert(strstr(drawn,"ADV HINT FMNA16")&&!strstr(drawn,"ADDRESS OUI"));
    ble_selected_focus=oui_only;ble_selected_focus.discovery.flipper_advertisement=(cerberus_ble_flipper_hints_t){CERBERUS_BLE_FLIPPER_HINT_WHITE,CERBERUS_BLE_FLIPPER_UUID16};
    drawn[0]='\0';assert(actual_hint_detail()==ESP_OK);assert(strstr(drawn,"3082 WHITE CODE")&&!strstr(drawn,"ADDRESS OUI"));
    ble_selected_focus=oui_only;ble_selected_focus.ibeacon_valid=true;
    drawn[0]='\0';assert(actual_hint_detail()==ESP_OK);assert(strstr(drawn,"IBEACON MAJOR")&&!strstr(drawn,"ADDRESS OUI"));
    ble_selected_focus=oui_only;assert(cerberus_ble_meta_evidence_add(&ble_selected_focus.discovery.meta_advertisement,0xFD5F,CERBERUS_BLE_META_LIST128));
    drawn[0]='\0';assert(actual_hint_detail()==ESP_OK);assert(strstr(drawn,"FD5F LIST128")&&!strstr(drawn,"ADDRESS OUI"));
    ble_filter=CERBERUS_S3_BLE_FILTER_FLOCK;drawn[0]='\0';assert(actual_hint_detail()==ESP_OK);assert(strstr(drawn,"ADDRESS OUI"));

    /* Service-pair contexts stay paired with their own service and use an
     * explicit remainder for extra facts. Bare decimal names stay clues. */
    ble_selected_focus=(cerberus_s3_ble_focus_t){0};
    ble_selected_focus.discovery.flock_advertisement=(cerberus_ble_flock_hints_t){.facts=CERBERUS_BLE_FLOCK_NO_LOCAL_NAME,.raven_3100_contexts=CERBERUS_BLE_FLOCK_LIST128,.raven_3200_contexts=CERBERUS_BLE_FLOCK_DATA128};
    ble_selected_focus.discovery.flock_scan_response.facts=CERBERUS_BLE_FLOCK_MFG_09C8|CERBERUS_BLE_FLOCK_SERIAL10;
    clock_ms=UINT32_MAX;drawn[0]='\0';cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND);assert(actual_hint_detail()==ESP_OK);
    assert(strstr(drawn,"ADV SIG 3100 L128+3200 D128 +1 FACTS / 4294967s"));
    assert(strstr(drawn,"RSP CLUE 09C8+10DIGIT NAME / 4294967s"));assert(!strstr(drawn,"SERIAL"));save(prefix,"flock-context-max");
    ble_selected_focus.discovery.flock_advertisement=(cerberus_ble_flock_hints_t){.accessory_contexts=CERBERUS_BLE_FLOCK_LIST128|CERBERUS_BLE_FLOCK_DATA128};
    drawn[0]='\0';assert(actual_hint_detail()==ESP_OK);assert(strstr(drawn,"ACCESSORY UUID L128 +1 FACTS"));
    /* The selected detail signal describes the scan peak and still fits at
     * both metadata widths; it does not borrow a per-signature RSSI. */
    ble_selected_result.rssi=-128;ble_selected_result.address_type=1;ble_selected_result.flags=CERBERUS_BLE_RESULT_CONNECTABLE;
    drawn[0]='\0';cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND);assert(actual_ble_peak_metadata()==ESP_OK);
    assert(strstr(drawn,"PEAK RSSI UNAVAILABLE / RANDOM / CONNECTABLE"));save(prefix,"ble-peak-metadata");
    ble_selected_result.flags=0;drawn[0]='\0';assert(actual_ble_peak_metadata()==ESP_OK);assert(strstr(drawn,"ADV ONLY"));

    analyzer_view=(cerberus_ble_analyzer_view_t){.active=true,.has_last=true,.last_report_valid=true,.last_scan_response=true,
        .last_flock=expected.focus.discovery.flock_scan_response,.last_seen_age_ms=7100,.last_rssi_dbm=-48,
        .evidence_kind=CERBERUS_BLE_ANALYZER_EVIDENCE_WATCH,.evidence_label="STRONGER WATCH HINT",.evidence_confidence=SENTINEL_WATCH_CONFIDENCE_HIGH};
    assert(cerberus_ble_meta_evidence_add(&analyzer_view.last_meta,0xFD5F,CERBERUS_BLE_META_LIST128));
    drawn[0]='\0';cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND);assert(draw_ble_analyzer()==ESP_OK);
    assert(strstr(drawn,"HINT: STRONGER WATCH HINT"));assert(strstr(drawn,"RSP CORROB PENGUIN+09C8DATA / 7s"));
    assert(!strstr(drawn,"FD5F LIST128"));save(prefix,"flock-analyzer");
    analyzer_view.last_ibeacon_valid=true;drawn[0]='\0';assert(draw_ble_analyzer()==ESP_OK);
    assert(strstr(drawn,"IBEACON MAJOR")&&!strstr(drawn,"RSP CORROB"));
    analyzer_view.last_ibeacon_valid=false;analyzer_view.last_flock=(cerberus_ble_flock_hints_t){.facts=CERBERUS_BLE_FLOCK_NO_LOCAL_NAME};
    analyzer_view.last_flock_oui_clue=true;memcpy(analyzer_view.last_address,oui_address,6);
    drawn[0]='\0';assert(draw_ble_analyzer()==ESP_OK);assert(strstr(drawn,"FD5F LIST128")&&!strstr(drawn,"ADDRESS OUI"));
    memset(&analyzer_view.last_meta,0,sizeof(analyzer_view.last_meta));masked=true;
    drawn[0]='\0';cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND);assert(draw_ble_analyzer()==ESP_OK);
    assert(strstr(drawn,"RSP NO QUALIFYING FLOCK FORMAT"));assert(strstr(drawn,"ADDRESS OUI CLUE / PREFIX MASKED"));save(prefix,"flock-analyzer-address-clue");
    analyzer_view.last_report_valid=false;drawn[0]='\0';assert(draw_ble_analyzer()==ESP_OK);assert(!strstr(drawn,"ADDRESS OUI"));
}

static void check_hc_ui(const char *prefix)
{
    seed_list();track_begins=track_ends=passive_starts=0;
    for(unsigned i=0;i<4;++i){
        cerberus_s3_ble_focus_t *f=&observations[i].focus;
        memset(&f->discovery.flipper_advertisement,0,sizeof(f->discovery.flipper_advertisement));
        memset(&f->discovery.flipper_scan_response,0,sizeof(f->discovery.flipper_scan_response));
        f->category_bits=0;live_ble_focus_bits[i]=0;
        observations[i].advertisement.length=3;
        observations[i].advertisement.data[0]=2;observations[i].advertisement.data[1]=1;observations[i].advertisement.data[2]=6;
    }
    observations[0].focus.discovery.hc_advertisement=CERBERUS_BLE_HC_03;
    observations[0].focus.discovery.hc_scan_response=CERBERUS_BLE_HC_05;
    observations[2].focus.discovery.hc_scan_response=CERBERUS_BLE_HC_06;
    /* Same address bytes, different address types must remain distinct. */
    memcpy(observations[2].peer.address,observations[0].peer.address,6);observations[2].peer.address_type=1;
    memcpy(observations[2].advertisement.address,observations[2].peer.address,6);observations[2].advertisement.address_type=1;
    memcpy(observations[2].focus.discovery.address,observations[2].peer.address,6);observations[2].focus.discovery.address_type=1;
    observations[3].focus.discovery.hc_advertisement=CERBERUS_BLE_HC_05;
    observations[3].peer.rssi=CERBERUS_BLE_RSSI_UNAVAILABLE;strcpy(observations[3].peer.name,"Zulu");
    for(unsigned i=0;i<4;++i){
        if(i!=1){observations[i].focus.category_bits=CERBERUS_S3_BLE_FOCUS_HC_NAME_HINT;live_ble_focus_bits[i]=CERBERUS_S3_BLE_FOCUS_HC_NAME_HINT;}
        store.ble[i]=observations[i].peer;
    }
    cerberus_scan_display_store_t unchanged=store;
    cerberus_s3_ble_focus_t separate=observations[0].focus;
    ble_filter=CERBERUS_S3_BLE_FILTER_META_HINT;live_ble_order_dirty=true;live_make_view();
    handle_ble_tap(100,65);assert(ble_filter==CERBERUS_S3_BLE_FILTER_HC_NAME_HINT);
    assert(live_ble_filtered_count==3&&live_ble_indexes[0]==0&&live_ble_indexes[1]==2&&live_ble_indexes[2]==3);
    assert(!strcmp(live_badges[0],"ADV HC-03 / RSP HC-05 / HINTS"));
    assert(!strcmp(live_badges[1],"ADV NONE / RSP HC-06 / HINTS"));
    assert(live_view.rows[0].signal_valid&&live_view.rows[1].signal_valid&&!live_view.rows[2].signal_valid);
    drawn[0]='\0';cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND);assert(sentinel_scan_view_draw(&live_view,0)==ESP_OK);
    assert(!strstr(drawn,"127 dBm"));save(prefix,"hc-list");
    handle_ble_tap(250,65);assert(live_ble_sort==CERBERUS_S3_BLE_SORT_NAME_ASC&&live_ble_indexes[0]==2);
    assert(!memcmp(&store,&unchanged,sizeof(store)));
    cerberus_findmy_monitor_record_t expected=observations[2];
    handle_ble_tap(20,135);tick(8200);
    assert(ble_mode==UI_BLE_MODE_DETAIL&&ble_selected_scan_index==2&&ble_selected_raw_frozen);
    assert(ble_selected_sound_profile==CERBERUS_FINDMY_PROFILE_NONE&&!memcmp(&ble_findmy_selected,&expected,sizeof(expected)));
    memset(observations,0,sizeof(observations));memset(&store,0,sizeof(store));++scan.generation;
    drawn[0]='\0';cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND);assert(actual_hint_detail()==ESP_OK);
    assert(strstr(drawn,"ADV NO EXACT HC NAME")&&strstr(drawn,"RSP NAME HC-06 / 7s"));
    assert(strstr(drawn,"NOT DEVICE IDENTITY")&&!strstr(drawn,"SKIMMER"));save(prefix,"hc-frozen-detail");
    handle_ble_tap(30,300);assert(dual_ble_live_prepare_fox((uint32_t)clock_ms,false));assert(ble_mode==UI_BLE_MODE_FOX_HUNT&&track_begins==1);
    assert(!memcmp(track_address,expected.peer.address,6)&&track_type==1);
    ble_fox_hunt_tick(8500,false);clock_ms=8600;actual_back(440);hold_stop=true;ble_fox_hunt_tick(8800,false);
    assert(ble_mode==UI_BLE_MODE_FOX_HUNT&&ble_fox_hunt_exit_pending);
    owner=CERBERUS_S3_BLE_OWNER_NONE;scan.state=CERBERUS_BLE_SCAN_COMPLETE;ble_fox_hunt_tick(9100,false);
    assert(ble_mode==UI_BLE_MODE_DETAIL&&track_ends==1&&!memcmp(&ble_findmy_selected,&expected,sizeof(expected)));
    clock_ms=10100;drawn[0]='\0';assert(actual_hint_detail()==ESP_OK);assert(strstr(drawn,"RSP NAME HC-06 / 9s"));
    actual_back(440);assert(ble_mode==UI_BLE_MODE_LIST&&ble_filter==CERBERUS_S3_BLE_FILTER_HC_NAME_HINT);

    ble_selected_focus=separate;clock_ms=8100;drawn[0]='\0';cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND);assert(actual_hint_detail()==ESP_OK);
    assert(strstr(drawn,"ADV NAME HC-03 / 8s")&&strstr(drawn,"RSP NAME HC-05 / 7s"));save(prefix,"hc-separate-sources");
    /* A replacement is current evidence only, never the old name at a new age. */
    ble_selected_focus.discovery.hc_advertisement=CERBERUS_BLE_HC_06;
    ble_selected_focus.discovery.advertisement_observed_ms=8000;
    ble_selected_focus.discovery.hc_scan_response=CERBERUS_BLE_HC_NONE;
    drawn[0]='\0';assert(actual_hint_detail()==ESP_OK);
    assert(strstr(drawn,"ADV NAME HC-06 / 0s")&&strstr(drawn,"RSP NO EXACT HC NAME"));assert(!strstr(drawn,"HC-03")&&!strstr(drawn,"HC-05"));
    ble_selected_focus.discovery.hc_advertisement=0;ble_selected_focus.category_bits=0;
    drawn[0]='\0';assert(actual_hint_detail()==ESP_OK);assert(!strstr(drawn,"HC-06"));
    char text[64];cerberus_hc_hint_line(CERBERUS_BLE_HC_03,false,UINT32_MAX,text,sizeof(text));assert(strstr(text,"HC-03 / 4294967s"));
    cerberus_hc_hint_line(255,true,1000,text,sizeof(text));assert(!strcmp(text,"RSP NO EXACT HC NAME"));
    ble_selected_focus=separate;ble_selected_focus.discovery.advertisement_observed_ms=UINT32_MAX-100;
    ble_selected_focus.discovery.scan_response_observed_ms=100;clock_ms=1100;drawn[0]='\0';assert(actual_hint_detail()==ESP_OK);
    assert(strstr(drawn,"ADV NAME HC-03 / 1s")&&strstr(drawn,"RSP NAME HC-05 / 1s"));

    /* Focused HC remains explicit; ALL preserves stronger payload priorities. */
    ble_filter=CERBERUS_S3_BLE_FILTER_ALL;ble_selected_focus=separate;ble_selected_focus.flock_oui_clue=true;
    drawn[0]='\0';assert(actual_hint_detail()==ESP_OK);assert(strstr(drawn,"ADV NAME HC-03")&&!strstr(drawn,"ADDRESS OUI"));
    ble_selected_focus.meta_suppressed=true;drawn[0]='\0';assert(actual_hint_detail()==ESP_OK);
    assert(strstr(drawn,"ADV NAME HC-03")&&!strstr(drawn,"META FILTER SUPPRESSED"));
    assert(cerberus_ble_meta_evidence_add(&ble_selected_focus.discovery.meta_advertisement,0xFD5F,CERBERUS_BLE_META_LIST16));
    drawn[0]='\0';assert(actual_hint_detail()==ESP_OK);assert(strstr(drawn,"FD5F LIST16")&&!strstr(drawn,"ADV NAME HC-03"));
    ble_filter=CERBERUS_S3_BLE_FILTER_HC_NAME_HINT;drawn[0]='\0';assert(actual_hint_detail()==ESP_OK);assert(strstr(drawn,"ADV NAME HC-03")&&!strstr(drawn,"FD5F LIST16"));
    ble_filter=CERBERUS_S3_BLE_FILTER_ALL;ble_selected_focus=separate;ble_selected_focus.discovery.flock_advertisement.facts=CERBERUS_BLE_FLOCK_FS_BATTERY;
    drawn[0]='\0';assert(actual_hint_detail()==ESP_OK);assert(strstr(drawn,"FS BATTERY NAME")&&!strstr(drawn,"ADV NAME HC-03"));
    ble_selected_focus=separate;ble_selected_focus.discovery.findmy_advertisement=(cerberus_ble_findmy_hints_t){CERBERUS_BLE_FINDMY_HINT_FMNA,CERBERUS_BLE_FINDMY_UUID16};
    drawn[0]='\0';assert(actual_hint_detail()==ESP_OK);assert(strstr(drawn,"ADV HINT FMNA16")&&!strstr(drawn,"ADV NAME HC-03"));
    ble_selected_focus=separate;ble_selected_focus.discovery.flipper_advertisement=(cerberus_ble_flipper_hints_t){CERBERUS_BLE_FLIPPER_HINT_WHITE,CERBERUS_BLE_FLIPPER_UUID16};
    drawn[0]='\0';assert(actual_hint_detail()==ESP_OK);assert(strstr(drawn,"3082 WHITE CODE")&&!strstr(drawn,"ADV NAME HC-03"));
    ble_selected_focus=separate;ble_selected_focus.ibeacon_valid=true;drawn[0]='\0';assert(actual_hint_detail()==ESP_OK);
    assert(strstr(drawn,"IBEACON MAJOR")&&!strstr(drawn,"ADV NAME HC-03"));

    /* Invalid scan peaks have no numeric dBm rendering. Both valid endpoints fit. */
    const int invalid[]={127,-128,21,126};
    ble_selected_result.address_type=1;ble_selected_result.flags=CERBERUS_BLE_RESULT_CONNECTABLE;
    for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i){
        ble_selected_result.rssi=(int8_t)invalid[i];drawn[0]='\0';assert(actual_ble_peak_metadata()==ESP_OK);
        assert(strstr(drawn,"PEAK RSSI UNAVAILABLE / RANDOM / CONNECTABLE")&&!strstr(drawn,"dBm"));
    }
    cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND);assert(actual_ble_peak_metadata()==ESP_OK);save(prefix,"hc-unavailable-peak");
    ble_selected_result.rssi=-127;drawn[0]='\0';cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND);assert(actual_ble_peak_metadata()==ESP_OK);
    assert(strstr(drawn,"PEAK RSSI -127 dBm / RANDOM / CONNECTABLE"));save(prefix,"hc-valid-peak");
    ble_selected_result.rssi=20;drawn[0]='\0';assert(actual_ble_peak_metadata()==ESP_OK);assert(strstr(drawn,"PEAK RSSI 20 dBm"));

    analyzer_view=(cerberus_ble_analyzer_view_t){.active=true,.has_last=true,.last_report_valid=true,.last_scan_response=true,
        .last_hc_name=CERBERUS_BLE_HC_06,.last_seen_age_ms=UINT32_MAX,.last_rssi_dbm=127,.last_name_length=32,
        .evidence_kind=CERBERUS_BLE_ANALYZER_EVIDENCE_WATCH,.evidence_label="STRONGER WATCH HINT",.evidence_confidence=SENTINEL_WATCH_CONFIDENCE_HIGH};
    memset(analyzer_view.last_name,'X',32);analyzer_view.last_flock_oui_clue=true;
    drawn[0]='\0';cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND);assert(draw_ble_analyzer()==ESP_OK);
    assert(strstr(drawn,"HINT: STRONGER WATCH HINT")&&strstr(drawn,"RSP NAME HC-06 / 4294967s"));
    assert(strstr(drawn,"RSP RSSI N/A")&&!strstr(drawn,"127 dBm")&&!strstr(drawn,"ADDRESS OUI"));save(prefix,"hc-analyzer-max");
    analyzer_view.last_ibeacon_valid=true;drawn[0]='\0';assert(draw_ble_analyzer()==ESP_OK);assert(strstr(drawn,"IBEACON MAJOR")&&!strstr(drawn,"RSP NAME HC-06"));
    analyzer_view.last_ibeacon_valid=false;analyzer_view.last_flock.facts=CERBERUS_BLE_FLOCK_FS_BATTERY;
    drawn[0]='\0';assert(draw_ble_analyzer()==ESP_OK);assert(strstr(drawn,"FS BATTERY NAME")&&!strstr(drawn,"RSP NAME HC-06"));
    analyzer_view.last_flock.facts=0;assert(cerberus_ble_meta_evidence_add(&analyzer_view.last_meta,0xFD5F,CERBERUS_BLE_META_LIST16));
    drawn[0]='\0';assert(draw_ble_analyzer()==ESP_OK);assert(strstr(drawn,"FD5F LIST16")&&!strstr(drawn,"RSP NAME HC-06"));
    memset(&analyzer_view.last_meta,0,sizeof(analyzer_view.last_meta));analyzer_view.last_scan_response=false;
    analyzer_view.last_hc_name=CERBERUS_BLE_HC_05;analyzer_view.last_seen_age_ms=10;analyzer_view.last_rssi_dbm=20;
    drawn[0]='\0';assert(draw_ble_analyzer()==ESP_OK);assert(strstr(drawn,"ADV NAME HC-05 / 0s")&&!strstr(drawn,"RSP NAME HC-06"));
    analyzer_view.last_hc_name=0;drawn[0]='\0';assert(draw_ble_analyzer()==ESP_OK);assert(!strstr(drawn,"NAME HC-05"));
    analyzer_view.last_hc_name=CERBERUS_BLE_HC_03;analyzer_view.last_report_valid=false;
    drawn[0]='\0';assert(draw_ble_analyzer()==ESP_OK);assert(!strstr(drawn,"NAME HC-03"));
}

static void seed_recent_findmy(void)
{
    seed_list();passive_starts=0;live_records[SENTINEL_SCAN_BLE].count=10;clock_ms=40000;live_ble_published_ms=40000;
    const uint32_t times[]={10000,9999,39000,9000,0,90000,35000,10000,38000,37000};
    for(unsigned i=0;i<10;++i){
        cerberus_findmy_monitor_record_t *r=&observations[i];memset(r,0,sizeof(*r));
        r->peer.address[0]=(uint8_t)(40+i);r->peer.address[5]=0x77;r->peer.address_type=(uint8_t)(i%2);
        if(i==0)r->peer.address_type=1;
        if(i==2)r->peer.address[0]=40; /* Equal bytes remain distinct by type. */
        snprintf(r->peer.name,sizeof(r->peer.name),"Node%02u",i);r->peer.rssi=(int8_t)(-40+(int)i);
        r->generation=7;r->index=(uint8_t)i;r->observed_ms=39000;
        memcpy(r->advertisement.address,r->peer.address,6);r->advertisement.address_type=r->peer.address_type;
        r->advertisement.length=3;r->advertisement.data[0]=2;r->advertisement.data[1]=1;r->advertisement.data[2]=6;
        cerberus_s3_ble_discovery_evidence_t *e=&r->focus.discovery;
        memcpy(e->address,r->peer.address,6);e->address_type=r->peer.address_type;e->generation=7;
        e->advertisement_sequence=1;e->advertisement_observed_ms=times[i];
        e->flags=CERBERUS_S3_BLE_EVIDENCE_ADV_VALID|CERBERUS_S3_BLE_EVIDENCE_ADV_COMPLETE|CERBERUS_S3_BLE_EVIDENCE_RAW_COMPLETE;
        e->findmy_advertisement=(cerberus_ble_findmy_hints_t){CERBERUS_BLE_FINDMY_HINT_FMNA,CERBERUS_BLE_FINDMY_UUID16};
        if(i==2||i==7){
            e->findmy_advertisement=(cerberus_ble_findmy_hints_t){0};
            e->flags|=CERBERUS_S3_BLE_EVIDENCE_RESPONSE_VALID;e->scan_response_sequence=2;
            e->scan_response_observed_ms=times[i]+500;e->findmy_scan_response=(cerberus_ble_findmy_hints_t){CERBERUS_BLE_FINDMY_HINT_DULT,0};
        }
        r->focus.category_bits=CERBERUS_S3_BLE_FOCUS_FIND_MY;live_ble_focus_bits[i]=r->focus.category_bits;store.ble[i]=r->peer;
    }
    ble_filter=CERBERUS_S3_BLE_FILTER_FIND_MY;live_ble_order_dirty=true;live_make_view();
}

static void recent_tick(int64_t now)
{
    clock_ms=now;live_scan_tick((uint32_t)now,false);
}

static void check_recent_ui(const char *prefix)
{
    seed_recent_findmy();assert(!live_ble_recent_only&&live_ble_filtered_count==10);
    assert(!strcmp(live_view.age_label,"ALL AGES >"));
    cerberus_findmy_monitor_record_t before[10];memcpy(before,observations,sizeof(before));
    handle_ble_tap(250,86);assert(live_ble_recent_only&&!strcmp(live_view.age_label,"WITHIN 30S >"));
    assert(live_ble_filtered_count==6&&live_ble_visible_mask==UINT32_C(0x3c5));
    const uint8_t signal_order[]={9,8,7,6,2,0},first_order[]={0,2,6,7,8,9};
    assert(!memcmp(live_ble_indexes,signal_order,sizeof(signal_order)));
    assert(live_commands==0&&starts==0&&passive_starts==0);
    handle_ble_tap(250,60);assert(live_ble_sort==CERBERUS_S3_BLE_SORT_NAME_ASC);
    assert(!memcmp(live_ble_indexes,first_order,sizeof(first_order)));
    handle_ble_tap(250,60);assert(live_ble_sort==CERBERUS_S3_BLE_SORT_DISCOVERY);
    assert(!memcmp(live_ble_indexes,first_order,sizeof(first_order)));
    drawn[0]='\0';cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND);assert(sentinel_scan_view_draw(&live_view,0)==ESP_OK);save(prefix,"recent-findmy");
    handle_ble_tap(190,380);assert(live_page[SENTINEL_SCAN_BLE]==1&&live_view.row_count==2);
    /* The production polling tick repaints a completed list without RF start. */
    recent_tick(40001);assert(live_ble_filtered_count==5&&live_page[SENTINEL_SCAN_BLE]==1);
    assert(live_commands==0&&starts==0&&passive_starts==0&&owner==CERBERUS_S3_BLE_OWNER_NONE);
    recent_tick(70001);assert(live_ble_filtered_count==0&&live_page[SENTINEL_SCAN_BLE]==0&&live_view.row_count==0);
    assert(!strcmp(live_view.message,"NO HINTS WITHIN 30S")&&live_view.state==SENTINEL_VIEW_COMPLETE);
    assert(live_commands==0&&starts==0&&passive_starts==0&&!memcmp(before,observations,sizeof(before)));
    drawn[0]='\0';cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND);assert(sentinel_scan_view_draw(&live_view,0)==ESP_OK);save(prefix,"recent-empty");
    handle_ble_tap(250,86);assert(!live_ble_recent_only&&live_ble_filtered_count==10);
    /* Paused lists also expire by the existing tick, without an implicit rearm. */
    seed_recent_findmy();handle_ble_tap(250,86);session.completed=false;session.paused=true;
    recent_tick(40001);assert(live_ble_filtered_count==5&&live_view.state==SENTINEL_VIEW_PAUSED);
    recent_tick(70001);assert(live_ble_filtered_count==0&&session.paused&&live_commands==0);

    /* Tap the painted boundary row after it expires but before a repaint. The
     * handler must freeze that row, not silently recompute a different target. */
    seed_recent_findmy();handle_ble_tap(250,86);handle_ble_tap(250,60);
    cerberus_findmy_monitor_record_t expected=observations[0];
    clock_ms=40001;handle_ble_tap(20,135);tick(40002);
    assert(ble_mode==UI_BLE_MODE_DETAIL&&ble_selected_scan_index==0&&!memcmp(&ble_findmy_selected,&expected,sizeof(expected)));
    assert(ble_selected_sound_profile==CERBERUS_FINDMY_PROFILE_NONE);
    memset(observations,0,sizeof(observations));memset(&store,0,sizeof(store));++scan.generation;
    track_begins=track_ends=passive_starts=0;handle_ble_tap(30,300);assert(dual_ble_live_prepare_fox((uint32_t)clock_ms,false));
    assert(ble_mode==UI_BLE_MODE_FOX_HUNT&&track_begins==1&&track_type==1&&track_type==expected.peer.address_type&&!memcmp(track_address,expected.peer.address,6));
    ble_fox_hunt_tick(40500,false);clock_ms=40600;actual_back(440);hold_stop=true;ble_fox_hunt_tick(40800,false);
    assert(ble_fox_hunt_exit_pending&&ble_mode==UI_BLE_MODE_FOX_HUNT);
    owner=CERBERUS_S3_BLE_OWNER_NONE;scan.state=CERBERUS_BLE_SCAN_COMPLETE;ble_fox_hunt_tick(41100,false);
    assert(ble_mode==UI_BLE_MODE_DETAIL&&track_ends==1&&!memcmp(&ble_findmy_selected,&expected,sizeof(expected)));
    actual_back(440);assert(ble_mode==UI_BLE_MODE_LIST&&ble_filter==CERBERUS_S3_BLE_FILTER_FIND_MY);

    /* Reset unsupported families; Watch never consumes the normal-list age mask. */
    seed_recent_findmy();handle_ble_tap(250,86);handle_ble_tap(100,65);
    assert(ble_filter==CERBERUS_S3_BLE_FILTER_FLIPPER&&!live_ble_recent_only&&!live_view.age_label);
    ble_filter=CERBERUS_S3_BLE_FILTER_FIND_MY;live_make_view();assert(!strcmp(live_view.age_label,"ALL AGES >"));
    live_ble_recent_only=true;current_page=UI_PAGE_WATCH;clock_ms=INT64_C(4294967296)+40000;
    assert(live_ble_recent_mask(clock_ms)==UINT32_MAX&&live_ble_recent_only);
    current_page=UI_PAGE_BLE;

    /* A full uint32 clock revolution cannot resurrect a never-before-filtered
     * snapshot. A clock rewind before its publication also excludes it. */
    seed_recent_findmy();clock_ms=INT64_C(4294967296)+40000;handle_ble_tap(250,86);
    assert(live_ble_filtered_count==0&&live_commands==0);
    handle_ble_tap(250,86);assert(live_ble_filtered_count==10);
    clock_ms=39999;handle_ble_tap(250,86);assert(live_ble_filtered_count==0);
    /* A genuinely new publication restores visibility using the new own times. */
    clock_ms=INT64_C(4294967296)+40000;live_ble_published_ms=clock_ms;
    for(unsigned i=0;i<10;++i){observations[i].generation=8;observations[i].focus.discovery.generation=8;}
    live_records[SENTINEL_SCAN_BLE].generation=live_records[SENTINEL_SCAN_BLE].saved_generation=8;
    live_ble_order_dirty=true;live_make_view();assert(live_ble_filtered_count==6);

    seed_recent_findmy();live_records[SENTINEL_SCAN_BLE].count=1;
    observations[0].focus.discovery.advertisement_observed_ms=0;clock_ms=0;live_ble_published_ms=0;
    live_ble_order_dirty=true;live_make_view();handle_ble_tap(250,86);assert(live_ble_filtered_count==1);
    clock_ms=30000;live_make_view();assert(live_ble_filtered_count==1);
    clock_ms=30001;live_make_view();assert(live_ble_filtered_count==0);
    clock_ms=INT64_C(4294967296)+1000;live_ble_published_ms=clock_ms-100;
    observations[0].focus.discovery.advertisement_observed_ms=UINT32_MAX-500;live_ble_order_dirty=true;
    live_make_view();assert(live_ble_filtered_count==1);
    observations[0].focus.discovery.advertisement_observed_ms=1001;live_make_view();assert(live_ble_filtered_count==0);

    /* Retained earlier iBeacon matches use their own timestamp; a recent raw
     * original cannot refresh an expired tuple, nor must it make one CURRENT. */
    seed_list();clock_ms=40000;live_ble_published_ms=40000;
    for(unsigned i=0;i<4;++i){
        cerberus_s3_ble_focus_t *f=&observations[i].focus;memset(&f->discovery.findmy_advertisement,0,sizeof(f->discovery.findmy_advertisement));
        f->ibeacon_valid=true;f->ibeacon_sequence=1;f->ibeacon_observed_ms=i==0?10000:i==1?9999:i==2?39000:90000;
        f->discovery.advertisement_sequence=3;f->discovery.advertisement_observed_ms=39500;
        f->category_bits=CERBERUS_S3_BLE_FOCUS_IBEACON;live_ble_focus_bits[i]=f->category_bits;
    }
    ble_filter=CERBERUS_S3_BLE_FILTER_ALL;live_ble_order_dirty=true;live_make_view();handle_ble_tap(100,65);
    assert(ble_filter==CERBERUS_S3_BLE_FILTER_IBEACON&&!strcmp(live_view.age_label,"ALL AGES >"));
    handle_ble_tap(250,86);assert(live_ble_filtered_count==2&&live_ble_indexes[0]==0&&live_ble_indexes[1]==2);
    assert(!strcmp(live_badges[0],"IBEACON ADV EARLIER / 30s"));
    drawn[0]='\0';cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND);assert(sentinel_scan_view_draw(&live_view,0)==ESP_OK);save(prefix,"recent-ibeacon");
    recent_tick(40001);assert(live_ble_filtered_count==1&&live_ble_indexes[0]==2&&live_commands==0);
}

static void seed_wifi_recent(void);

static void check_start_publication_lifecycle(void)
{
    const esp_err_t replies[]={ESP_OK,ESP_FAIL,ESP_ERR_INVALID_STATE};
    const sentinel_scan_tool_t tools[]={SENTINEL_SCAN_BLE,SENTINEL_SCAN_WIFI};
    for(unsigned t=0;t<2;++t)for(unsigned i=0;i<3;++i){
        sentinel_scan_tool_t tool=tools[t];
        if(tool==SENTINEL_SCAN_BLE){seed_list();session.completed=false;}
        else {seed_wifi_recent();live_session.completed=false;}
        readout_error=ESP_ERR_TIMEOUT;
        live_command_error=replies[i];
        cerberus_scan_display_store_t frozen=store;int64_t published=live_ble_published_ms;
        recent_tick(41000);assert(live_commands==1);
        assert(!!(live_capture_pending&(UINT32_C(1)<<tool))==(replies[i]==ESP_OK));
        assert(live_records[tool].captured&&live_records[tool].saved_generation==7);
        assert(!memcmp(&frozen,&store,sizeof(store))&&live_ble_published_ms==published);
        /* The real scheduler invokes the readout-reset adapter only after an
         * accepted Wi-Fi START. The scan-display suite owns its actual latch. */
        bool reset_expected=tool==SENTINEL_SCAN_WIFI&&replies[i]==ESP_OK;
        assert(readout_resets==(unsigned)reset_expected);
        assert(readout_error==(reset_expected?ESP_OK:ESP_ERR_TIMEOUT));
    }
}

static void seed_wifi_recent(void)
{
    wifi_snapshot_band=CERBERUS_WIFI_SCAN_BAND_ALL;
    seed_list();current_page=UI_PAGE_WIFI;wifi_mode=UI_WIFI_MODE_LIST;
    c5_status.firmware_version=CERBERUS_WIFI_OBSERVATION_MIN_FIRMWARE;
    c5_online=true;live_radio_busy=false;wifi_attack_selecting=false;
    wifi_selection_valid=false;live_wifi_recent_only=false;
    wifi_relation_publication=UINT64_C(0x1234567800000042);wifi_relation_generation=7;
    live_session.desired=SENTINEL_SCAN_WIFI;live_session.completed=true;
    session.desired=SENTINEL_SCAN_NONE;
    clock_ms=40000;live_commands=starts=passive_starts=0;
    live_records[SENTINEL_SCAN_WIFI]=(live_scan_record_t){.valid=true,.captured=true,
        .count=10,.generation=7,.saved_generation=7,.state=SENTINEL_SCAN_COMPLETE,.captured_ms=40000};
    const uint32_t ages[]={30000,30001,500,0,UINT32_MAX,0,25000,500,0,15000};
    for(unsigned i=0;i<10;++i){
        cerberus_c5_wifi_result_t *row=&store.wifi[i];
        snprintf(row->ssid,sizeof(row->ssid),"Network %u",i);
        row->bssid[0]=2;row->bssid[5]=(uint8_t)(i+1);row->channel=i%2?36:6;row->rssi=(int8_t)(-30-i);
        store.wifi_observations[i]=(cerberus_s3_wifi_observation_t){
            .request_started_us=40000000,.source_age_ms=ages[i],.valid=i!=3&&i!=5};
    }
    live_make_view();drawn[0]='\0';assert(live_paint(true)==ESP_OK);
}

static void check_wifi_restricted_coverage(void)
{
    seed_wifi_recent();
    for(unsigned i=0;i<10;++i)store.wifi[i].channel=36;
    wifi_snapshot_band=CERBERUS_WIFI_SCAN_BAND_5G;
    cerberus_scan_display_store_t original=store;
    uint32_t captured=live_records[SENTINEL_SCAN_WIFI].captured_ms;
    live_make_view();
    assert(strstr(live_view.hint,"5 GHZ ONLY")&&!strcmp(live_view.count_label,"ALL STORED >"));
    assert(live_view.total_count==10);
    live_wifi_band=CERBERUS_S3_WIFI_BAND_24_GHZ;live_make_view();
    assert(!live_view.total_count&&!strcmp(live_view.message,"BAND NOT SCANNED"));
    live_wifi_band=CERBERUS_S3_WIFI_BAND_5_GHZ;live_make_view();
    assert(live_view.total_count==10&&strstr(live_view.hint,"5 GHZ ONLY"));
    assert(!memcmp(&original,&store,sizeof(store))&&live_records[SENTINEL_SCAN_WIFI].captured_ms==captured);
    assert(!live_commands&&!starts&&!passive_starts);
    seed_wifi_recent();
}

static void check_wifi_recent_ui(const char *prefix)
{
    seed_wifi_recent();assert(live_view.total_count==10&&!strcmp(live_view.age_label,"ALL AGES >"));
    cerberus_scan_display_store_t frozen=store;
    handle_live_scan_tap(250,86);
    const uint8_t expected[]={0,2,6,7,8,9};
    assert(live_wifi_recent_only&&live_wifi_filtered_count==6&&!memcmp(live_wifi_indexes,expected,sizeof(expected)));
    assert(!strcmp(live_names[1],"Network 2")&&live_view.page_count==2);
    assert(live_commands==0&&starts==0&&passive_starts==0);
    drawn[0]='\0';cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND);
    assert(sentinel_scan_view_draw(&live_view,0)==ESP_OK);save(prefix,"recent-wifi");
    handle_live_scan_tap(190,380);assert(live_page[SENTINEL_SCAN_WIFI]==1&&live_view.row_count==2);
    recent_tick(40001);assert(live_wifi_filtered_count==5&&live_page[SENTINEL_SCAN_WIFI]==1);
    recent_tick(70001);assert(live_wifi_filtered_count==0&&live_page[SENTINEL_SCAN_WIFI]==0);
    assert(live_view.state==SENTINEL_VIEW_COMPLETE&&!strcmp(live_view.message,"NO NETWORKS WITHIN 30S"));
    assert(live_commands==0&&starts==0&&passive_starts==0&&!memcmp(&store,&frozen,sizeof(store)));
    drawn[0]='\0';cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND);
    assert(sentinel_scan_view_draw(&live_view,0)==ESP_OK);save(prefix,"recent-wifi-empty");
    handle_live_scan_tap(250,86);assert(live_wifi_filtered_count==10&&!live_wifi_recent_only);

    /* Expiration between paint and tap must never select a different AP. */
    seed_wifi_recent();handle_live_scan_tap(250,86);
    cerberus_c5_wifi_result_t chosen=store.wifi[0];
    clock_ms=40001;handle_live_scan_tap(20,135);
    assert(wifi_selection_valid&&wifi_selected_index==0&&wifi_mode==UI_WIFI_MODE_DETAIL);
    assert(!memcmp(&wifi_selected_result,&chosen,sizeof(chosen)));
    assert(relation_selection_calls==1&&relation_selection_ordinary&&
        !memcmp(&relation_selected_ap,&chosen,sizeof(chosen)));
    memset(store.wifi,0,sizeof(store.wifi));
    assert(!memcmp(&wifi_selected_result,&chosen,sizeof(chosen)));
    /* All ages retains the original driver order, including unproven rows. */
    seed_wifi_recent();handle_live_scan_tap(250,86);handle_live_scan_tap(250,86);
    handle_live_scan_tap(20,200);assert(wifi_selected_index==1&&!strcmp(wifi_selected_result.ssid,"Network 1"));

    seed_wifi_recent();handle_live_scan_tap(250,86);live_session.completed=false;live_session.paused=true;
    recent_tick(70001);assert(live_view.state==SENTINEL_VIEW_PAUSED&&live_view.total_count==0&&live_commands==0);
    seed_wifi_recent();handle_live_scan_tap(250,86);
    c5_status.firmware_version=CERBERUS_WIFI_OBSERVATION_MIN_FIRMWARE-1;
    live_make_view();assert(!live_wifi_recent_only&&!live_view.age_label&&live_view.total_count==10);
    handle_live_scan_tap(250,86);assert(!live_wifi_recent_only&&live_view.total_count==10);
    seed_wifi_recent();c5_online=false;live_make_view();handle_live_scan_tap(250,86);
    assert(live_view.state==SENTINEL_VIEW_OFFLINE&&!live_wifi_recent_only);
    c5_online=true;
}

static unsigned wifi_list_ui_checks;
#define WIFI_UI_CHECK(value) do { ++wifi_list_ui_checks; if(!(value)){ \
    fprintf(stderr,"Wi-Fi list UI check at %u: %s\n",__LINE__,#value);abort();} } while(0)

static void wifi_ui_tap(uint16_t x,uint16_t y)
{ drawn[0]='\0';handle_live_scan_tap(x,y); }

static void seed_wifi_order(void)
{
    seed_wifi_recent();
    const char *names[]={"Zulu","alpha","Beta","delta","","Gamma","epsilon","Theta","Eta","Iota"};
    const uint8_t channels[]={6,36,1,149,11,44,0,165,14,233};
    const int8_t signals[]={-40,-30,-80,-20,-70,-50,-60,-90,-10,-100};
    for(unsigned i=0;i<10;++i){
        strcpy(store.wifi[i].ssid,names[i]);store.wifi[i].channel=channels[i];store.wifi[i].rssi=signals[i];
    }
    live_make_view();drawn[0]='\0';WIFI_UI_CHECK(live_paint(true)==ESP_OK);
}

static void wifi_ui_expect_map(const uint8_t *expected,size_t count)
{
    WIFI_UI_CHECK(live_wifi_filtered_count==count && live_view.total_count==count);
    WIFI_UI_CHECK(!memcmp(live_wifi_indexes,expected,count));
    WIFI_UI_CHECK(live_wifi_painted.valid && live_wifi_painted.publication==wifi_relation_publication);
    WIFI_UI_CHECK(live_wifi_painted.generation==7 && live_wifi_painted.bank_count==10);
    WIFI_UI_CHECK(live_wifi_painted.token==live_wifi_built.token);
    WIFI_UI_CHECK(!live_commands && !starts && !passive_starts && !wifi_ui_connection_queries);
}

static void check_wifi_order_controls(const char *prefix)
{
    /* Fixed expected source orders, independent of the model implementation. */
    static const uint8_t order[3][10]={
        {8,3,1,0,5,6,4,2,7,9},
        {4,1,2,3,6,8,5,9,7,0},
        {2,0,4,8,1,5,3,7,6,9},
    };
    const uint16_t bands[]={1023,277,170},recent=965;
    for(unsigned sort=0;sort<3;++sort)for(unsigned band=0;band<3;++band)for(unsigned age=0;age<2;++age){
        seed_wifi_order();cerberus_scan_display_store_t frozen=store;
        for(unsigned i=0;i<sort;++i)wifi_ui_tap(250,60);
        for(unsigned i=0;i<band;++i)wifi_ui_tap(100,65);
        if(age)wifi_ui_tap(250,86);
        WIFI_UI_CHECK(live_wifi_sort==(cerberus_s3_wifi_sort_t)sort && live_wifi_band==(cerberus_s3_wifi_band_t)band);
        WIFI_UI_CHECK(live_wifi_recent_only==!!age && live_page[SENTINEL_SCAN_WIFI]==0);
        uint8_t expected[10];size_t count=0;
        for(unsigned i=0;i<10;++i){
            unsigned bit=1U<<order[sort][i];
            if((bands[band]&bit) && (!age || (recent&bit)))expected[count++]=order[sort][i];
        }
        wifi_ui_expect_map(expected,count);
        WIFI_UI_CHECK(!memcmp(&frozen,&store,sizeof(store)));
        /* Every source position remains selectable through the real painted
         * page map, including the partially filled final page. */
        size_t selected=count-1;
        while(live_view.has_next)wifi_ui_tap(190,380);
        unsigned source=expected[selected];cerberus_c5_wifi_result_t chosen=store.wifi[source];
        wifi_ui_tap(20,(uint16_t)(135+56*(selected%4)));
        WIFI_UI_CHECK(wifi_mode==UI_WIFI_MODE_DETAIL && wifi_selection_valid && wifi_selected_index==source);
        WIFI_UI_CHECK(!memcmp(&chosen,&wifi_selected_result,sizeof(chosen)));
        WIFI_UI_CHECK(relation_selection_calls==1 && relation_selection_ordinary);
        WIFI_UI_CHECK(!memcmp(&chosen,&relation_selected_ap,sizeof(chosen)));
        WIFI_UI_CHECK(!memcmp(&frozen,&store,sizeof(store)) && !live_commands && !starts && !passive_starts);
        WIFI_UI_CHECK(wifi_ui_connection_queries==1);
    }
    seed_wifi_order();wifi_ui_tap(190,380);WIFI_UI_CHECK(live_page[SENTINEL_SCAN_WIFI]==1);
    wifi_ui_tap(250,60);WIFI_UI_CHECK(live_wifi_sort==CERBERUS_S3_WIFI_SORT_NAME_ASC && !live_page[SENTINEL_SCAN_WIFI]);
    wifi_ui_tap(190,380);wifi_ui_tap(100,65);WIFI_UI_CHECK(!live_page[SENTINEL_SCAN_WIFI]);
    wifi_ui_tap(100,65);wifi_ui_tap(100,65);WIFI_UI_CHECK(live_wifi_band==CERBERUS_S3_WIFI_BAND_ALL);
    wifi_ui_tap(190,380);wifi_ui_tap(250,86);WIFI_UI_CHECK(!live_page[SENTINEL_SCAN_WIFI]);
    wifi_ui_tap(250,60);wifi_ui_tap(250,60);WIFI_UI_CHECK(live_wifi_sort==CERBERUS_S3_WIFI_SORT_SIGNAL_DESC);

    seed_wifi_order();save(prefix,"wifi-sort-signal");
    wifi_ui_tap(250,60);save(prefix,"wifi-sort-name");
    wifi_ui_tap(250,60);save(prefix,"wifi-sort-channel");
    wifi_ui_tap(100,65);save(prefix,"wifi-band-24");
    wifi_ui_tap(100,65);wifi_ui_tap(250,86);save(prefix,"wifi-band-5-recent");
    recent_tick(80001);WIFI_UI_CHECK(!live_view.total_count && !live_view.row_count && !live_page[SENTINEL_SCAN_WIFI]);
    save(prefix,"wifi-band-5-empty");

    /* Local sort/band work on retained rows while C5 is offline. Existing age
     * and Scan controls stay disabled; no new radio operation is dispatched. */
    seed_wifi_order();c5_online=false;live_make_view();drawn[0]='\0';WIFI_UI_CHECK(live_paint(false)==ESP_OK);
    wifi_ui_tap(100,65);wifi_ui_tap(250,60);
    WIFI_UI_CHECK(live_wifi_band==CERBERUS_S3_WIFI_BAND_24_GHZ && live_wifi_sort==CERBERUS_S3_WIFI_SORT_NAME_ASC);
    wifi_ui_tap(250,86);WIFI_UI_CHECK(!live_wifi_recent_only && !live_commands);
    save(prefix,"wifi-band-offline");
    c5_online=true;
}

static void check_wifi_painted_binding(void)
{
    /* Same-generation replacement must not turn the old painted row into a
     * new AP. Rejection must happen before copying any selected identity. */
    seed_wifi_order();cerberus_c5_wifi_result_t prior=wifi_selected_result;
    ++wifi_relation_publication;strcpy(store.wifi[8].ssid,"Replacement");
    wifi_ui_tap(20,135);
    WIFI_UI_CHECK(!wifi_selection_valid && !relation_selection_calls && wifi_mode==UI_WIFI_MODE_LIST);
    WIFI_UI_CHECK(!memcmp(&prior,&wifi_selected_result,sizeof(prior)));
    live_make_view();drawn[0]='\0';WIFI_UI_CHECK(live_paint(false)==ESP_OK);
    wifi_ui_tap(20,135);WIFI_UI_CHECK(wifi_selection_valid && !strcmp(wifi_selected_result.ssid,"Replacement"));

    /* Any build without a successful paint disables binding, even when the
     * publication itself is unchanged and the index array has been rebuilt. */
    seed_wifi_order();live_wifi_sort=CERBERUS_S3_WIFI_SORT_NAME_ASC;live_make_view();
    wifi_ui_tap(20,135);WIFI_UI_CHECK(!wifi_selection_valid && !relation_selection_calls);
    drawn[0]='\0';WIFI_UI_CHECK(live_paint(false)==ESP_OK);
    wifi_ui_tap(20,135);WIFI_UI_CHECK(wifi_selection_valid && wifi_selected_index==4);

    /* Fault every rectangle write in a real reordered update, including writes
     * after a row has already changed, not just the first summary operation. */
    seed_wifi_order();live_wifi_sort=CERBERUS_S3_WIFI_SORT_NAME_ASC;live_make_view();
    unsigned fill_before=wifi_ui_fill_calls;drawn[0]='\0';WIFI_UI_CHECK(live_paint(false)==ESP_OK);
    unsigned update_fills=wifi_ui_fill_calls-fill_before;WIFI_UI_CHECK(update_fills>8);
    for(unsigned failure=1;failure<=update_fills;++failure){
        seed_wifi_order();live_wifi_paint_map_t old=live_wifi_painted;
        live_wifi_sort=CERBERUS_S3_WIFI_SORT_NAME_ASC;live_make_view();
        drawn[0]='\0';wifi_ui_fail_fill_after=failure;
        WIFI_UI_CHECK(live_paint(false)==ESP_FAIL && !wifi_ui_fail_fill_after);
        WIFI_UI_CHECK(!live_wifi_painted.valid);
        WIFI_UI_CHECK(live_wifi_painted.publication==old.publication && live_wifi_painted.token==old.token);
        WIFI_UI_CHECK(!memcmp(live_wifi_painted.indexes,old.indexes,sizeof(old.indexes)));
        unsigned repairs=wifi_ui_full_repairs;
        wifi_ui_tap(20,135);WIFI_UI_CHECK(!wifi_selection_valid && !relation_selection_calls);
        WIFI_UI_CHECK(live_wifi_painted.valid && wifi_ui_full_repairs==repairs+1);
        drawn[0]='\0';WIFI_UI_CHECK(live_paint(false)==ESP_OK && wifi_ui_full_repairs==repairs+1);
        wifi_ui_tap(20,135);WIFI_UI_CHECK(wifi_selection_valid && wifi_selected_index==4);
    }
    /* A bank changing after view construction cannot be bound by a later
     * successful draw. Full-width publication and saved generation/count all
     * participate in the guard; a token overflow never becomes eligible. */
    for(unsigned change=0;change<5;++change){
        seed_wifi_order();live_make_view();
        if(change==0)wifi_relation_publication+=UINT64_C(1)<<32;
        if(change==1)++wifi_relation_generation;
        if(change==2)++live_records[SENTINEL_SCAN_WIFI].saved_generation;
        if(change==3)--live_records[SENTINEL_SCAN_WIFI].count;
        if(change==4)live_records[SENTINEL_SCAN_WIFI].captured=false;
        drawn[0]='\0';WIFI_UI_CHECK(live_paint(false)==ESP_OK && !live_wifi_painted.valid);
        wifi_ui_tap(20,135);WIFI_UI_CHECK(!wifi_selection_valid && !relation_selection_calls);
    }
    seed_wifi_order();live_wifi_view_token=UINT64_MAX;live_make_view();
    drawn[0]='\0';WIFI_UI_CHECK(live_paint(false)==ESP_OK && !live_wifi_painted.valid);
    wifi_ui_tap(20,135);WIFI_UI_CHECK(!wifi_selection_valid && !relation_selection_calls);
    seed_wifi_recent();
}

static void check_wifi_list_geometry_and_cache(void)
{
    seed_wifi_order();
    for(unsigned state=0;state<=SENTINEL_VIEW_OFFLINE;++state){
        sentinel_scan_view_t view=live_view;view.state=(sentinel_view_state_t)state;
        for(unsigned edge=0;edge<2;++edge){
            uint16_t left=edge?215:8,right=edge?311:224;
            WIFI_UI_CHECK(sentinel_scan_view_hit_test(&view,left,48)==SENTINEL_VIEW_BAND);
            WIFI_UI_CHECK(sentinel_scan_view_hit_test(&view,left,97)==SENTINEL_VIEW_BAND);
            WIFI_UI_CHECK(sentinel_scan_view_hit_test(&view,left,47)==SENTINEL_VIEW_NONE);
            WIFI_UI_CHECK(sentinel_scan_view_hit_test(&view,left,98)==SENTINEL_VIEW_NONE);
            WIFI_UI_CHECK(sentinel_scan_view_hit_test(&view,right,48)==SENTINEL_VIEW_SORT);
            WIFI_UI_CHECK(sentinel_scan_view_hit_test(&view,right,73)==SENTINEL_VIEW_SORT);
            WIFI_UI_CHECK(sentinel_scan_view_hit_test(&view,right,74)==
                (state==SENTINEL_VIEW_OFFLINE?SENTINEL_VIEW_NONE:SENTINEL_VIEW_AGE));
            WIFI_UI_CHECK(sentinel_scan_view_hit_test(&view,right,97)==
                (state==SENTINEL_VIEW_OFFLINE?SENTINEL_VIEW_NONE:SENTINEL_VIEW_AGE));
            WIFI_UI_CHECK(sentinel_scan_view_hit_test(&view,right,47)==SENTINEL_VIEW_NONE);
            WIFI_UI_CHECK(sentinel_scan_view_hit_test(&view,right,98)==SENTINEL_VIEW_NONE);
        }
        for(uint16_t x=216;x<224;++x)WIFI_UI_CHECK(sentinel_scan_view_hit_test(&view,x,65)==SENTINEL_VIEW_NONE);
        WIFI_UI_CHECK(sentinel_scan_view_hit_test(&view,7,65)==SENTINEL_VIEW_NONE);
        WIFI_UI_CHECK(sentinel_scan_view_hit_test(&view,312,65)==SENTINEL_VIEW_NONE);
        WIFI_UI_CHECK(sentinel_scan_view_hit_test(&view,20,126)==SENTINEL_VIEW_ROW0);
        WIFI_UI_CHECK(sentinel_scan_view_hit_test(&view,250,113)==SENTINEL_VIEW_NONE);
    }
    /* Old C5 peers omit the unsupported age control but still have local sort
     * and band controls; no hidden lower area silently enables age. */
    c5_status.firmware_version=CERBERUS_WIFI_OBSERVATION_MIN_FIRMWARE-1;
    live_make_view();WIFI_UI_CHECK(!live_view.age_label);
    WIFI_UI_CHECK(sentinel_scan_view_hit_test(&live_view,250,97)==SENTINEL_VIEW_SORT);

    seed_wifi_order();sentinel_scan_view_t view=live_view;
    char sort[13]="SIGNAL >",band[19]="ALL BANDS >";
    view.sort_label=sort;view.count_label=band;
    drawn[0]='\0';WIFI_UI_CHECK(sentinel_scan_view_draw(&view,0)==ESP_OK);
    unsigned before=calls;WIFI_UI_CHECK(sentinel_scan_view_update(&view,1)==ESP_OK && calls==before);
    static uint16_t previous_screen[320*480];
    for(unsigned update=0;update<3;++update){
        memcpy(previous_screen,screen,sizeof(screen));before=calls;drawn[0]='\0';
        if(update==0)strcpy(sort,"NAME A-Z >");
        if(update==1)strcpy(sort,"CHANNEL >");
        if(update==2)strcpy(band,"5 GHZ >");
        WIFI_UI_CHECK(sentinel_scan_view_update(&view,2+update)==ESP_OK && calls>before);
        WIFI_UI_CHECK(strstr(drawn,sort) && strstr(drawn,band));
        for(unsigned y=0;y<480;++y)if(y<48 || y>=98)
            WIFI_UI_CHECK(!memcmp(screen+y*320,previous_screen+y*320,320*sizeof(screen[0])));
        before=calls;WIFI_UI_CHECK(sentinel_scan_view_update(&view,6+update)==ESP_OK && calls==before);
    }
    before=calls;view.band_control=false;drawn[0]='\0';
    WIFI_UI_CHECK(sentinel_scan_view_update(&view,10)==ESP_OK && calls>before);
    WIFI_UI_CHECK(sentinel_scan_view_hit_test(&view,100,65)==SENTINEL_VIEW_NONE);
    view.kind=SENTINEL_VIEW_BLE;view.band_control=true;
    WIFI_UI_CHECK(sentinel_scan_view_hit_test(&view,100,65)==SENTINEL_VIEW_NONE);

    /* Existing explicit target selection keeps source order and its Clear
     * control. These new ordinary-list settings cannot filter its targets. */
    seed_wifi_order();live_wifi_sort=CERBERUS_S3_WIFI_SORT_NAME_ASC;
    live_wifi_band=CERBERUS_S3_WIFI_BAND_5_GHZ;live_wifi_recent_only=true;
    wifi_attack_selecting=true;wifi_target_count=1;live_make_view();drawn[0]='\0';
    WIFI_UI_CHECK(live_paint(true)==ESP_OK);
    WIFI_UI_CHECK(live_view.selection_mode && !live_view.band_control && !live_view.sort_label && !live_view.age_label);
    WIFI_UI_CHECK(live_view.total_count==10 && !strcmp(live_names[0],"Zulu") && !strcmp(live_names[1],"alpha"));
    WIFI_UI_CHECK(sentinel_scan_view_hit_test(&live_view,100,65)==SENTINEL_VIEW_NONE);
    WIFI_UI_CHECK(sentinel_scan_view_hit_test(&live_view,250,65)==SENTINEL_VIEW_CLEAR);
    WIFI_UI_CHECK(sentinel_scan_view_hit_test(&live_view,250,86)==SENTINEL_VIEW_NONE);
    cerberus_scan_display_store_t frozen=store;
    wifi_ui_tap(100,65);wifi_ui_tap(250,86);wifi_ui_tap(20,200);
    WIFI_UI_CHECK(wifi_ui_target_toggles==1 && wifi_ui_target_index==1 && !wifi_selection_valid);
    WIFI_UI_CHECK(live_wifi_sort==CERBERUS_S3_WIFI_SORT_NAME_ASC && live_wifi_band==CERBERUS_S3_WIFI_BAND_5_GHZ && live_wifi_recent_only);
    WIFI_UI_CHECK(!memcmp(&frozen,&store,sizeof(store)) && !wifi_ui_connection_queries && !live_commands && !starts && !passive_starts);
    wifi_attack_selecting=false;wifi_target_count=0;seed_wifi_recent();
}

static void check_wifi_readout_error_ui(const char *prefix)
{
    const struct {esp_err_t error;const char *label;} failures[]={
        {ESP_ERR_TIMEOUT,"C5 READOUT TIMEOUT"},
        {ESP_ERR_INVALID_RESPONSE,"C5 REPLY INVALID"},
        {ESP_ERR_NO_MEM,"S3 MEMORY LOW"},
        {ESP_ERR_INVALID_STATE,"READOUT ERROR / ESP_ERR_INVALID_STATE"}};
    unsigned checks=0;
#define READOUT_CHECK(value) do {assert(value);++checks;} while(0)
    /* Full production view-builder precedence: fault/busy, selection and age
     * must not hide a matching latched readout failure, online or offline. */
    for(unsigned f=0;f<4;++f)for(unsigned flags=0;flags<64;++flags){
        seed_wifi_recent();
        cerberus_scan_display_store_t frozen=store;
        live_session.faulted=!!(flags&1);live_radio_busy=!!(flags&2);
        wifi_attack_selecting=!!(flags&4);live_wifi_recent_only=!!(flags&8);
        c5_online=!(flags&16);live_records[SENTINEL_SCAN_WIFI].captured=!(flags&32);
        if(!live_records[SENTINEL_SCAN_WIFI].captured)live_records[SENTINEL_SCAN_WIFI].count=0;
        readout_error=failures[f].error;live_records[SENTINEL_SCAN_WIFI].error=readout_error;
        live_make_view();
        READOUT_CHECK(!strcmp(live_view.subtitle,failures[f].label));
        READOUT_CHECK(strstr(live_view.hint,flags&32?"NO NEW RESULTS":"PREVIOUS RESULTS KEPT"));
        READOUT_CHECK(strstr(live_view.hint,c5_online?"TAP SCAN FOR NEW SWEEP":"WAIT FOR C5"));
        READOUT_CHECK(live_view.state==(c5_online?SENTINEL_VIEW_ERROR:SENTINEL_VIEW_OFFLINE));
        READOUT_CHECK(strlen(live_view.subtitle)<=50&&strlen(live_view.hint)<=50);
        READOUT_CHECK(!memcmp(&store,&frozen,sizeof(store))&&live_commands==0&&readout_resets==0);
        READOUT_CHECK(sentinel_scan_view_hit_test(&live_view,100,380)==
            (c5_online?SENTINEL_VIEW_PAUSE:SENTINEL_VIEW_NONE));
    }
    /* A stale readout latch must not replace a later unrelated record error. */
    seed_wifi_recent();readout_error=ESP_ERR_TIMEOUT;
    live_records[SENTINEL_SCAN_WIFI].error=ESP_ERR_INVALID_STATE;live_make_view();
    READOUT_CHECK(!strcmp(live_view.subtitle,"RADIO ERROR / ESP_ERR_INVALID_STATE"));
    READOUT_CHECK(!strstr(live_view.hint,"PREVIOUS RESULTS KEPT"));
    live_session.faulted=true;live_make_view();
    READOUT_CHECK(!strcmp(live_view.subtitle,"SCAN NEEDS ATTENTION / TAP SCAN"));
    seed_wifi_recent();live_records[SENTINEL_SCAN_WIFI].error=ESP_ERR_INVALID_STATE;
    live_make_view();READOUT_CHECK(!strstr(live_view.subtitle,"READOUT"));
    seed_list();readout_error=ESP_ERR_TIMEOUT;live_records[SENTINEL_SCAN_BLE].error=readout_error;
    live_make_view();READOUT_CHECK(!strstr(live_view.subtitle,"C5 READOUT"));

    /* Busy transfer progress keeps its existing Stop control. */
    seed_wifi_recent();readout_error=ESP_ERR_TIMEOUT;live_records[SENTINEL_SCAN_WIFI].error=readout_error;
    readout_busy=true;store.wifi_transfer.count=10;store.wifi_transfer.next=4;live_make_view();
    READOUT_CHECK(!strcmp(live_view.subtitle,"READING RESULTS 4 / 10"));
    READOUT_CHECK(live_view.state==SENTINEL_VIEW_LIVE);
    READOUT_CHECK(!strcmp(live_view.hint,"PREVIOUS SWEEP SHOWN / STOP CANCELS READOUT"));
    handle_live_scan_tap(100,380);READOUT_CHECK(live_session.paused&&live_commands==0);

    seed_wifi_recent();readout_error=ESP_ERR_TIMEOUT;live_records[SENTINEL_SCAN_WIFI].error=readout_error;
    live_make_view();drawn[0]='\0';cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND);
    READOUT_CHECK(sentinel_scan_view_draw(&live_view,0)==ESP_OK);
    READOUT_CHECK(strstr(drawn,"C5 READOUT TIMEOUT")&&strstr(drawn,"PREVIOUS RESULTS KEPT"));
    save(prefix,"readout-timeout-retained");
    uint32_t arm=live_session.arm_generation;
    handle_live_scan_tap(100,380);
    READOUT_CHECK(live_session.arm_generation!=arm&&!live_session.completed&&!live_session.paused);
    READOUT_CHECK(live_commands==0&&readout_resets==0&&readout_error==ESP_ERR_TIMEOUT);
    recent_tick(41000);
    READOUT_CHECK(live_commands==1&&readout_resets==1&&readout_error==ESP_OK);
    READOUT_CHECK(live_records[SENTINEL_SCAN_WIFI].state==SENTINEL_SCAN_RUNNING);

    seed_wifi_recent();c5_online=false;readout_error=ESP_ERR_INVALID_RESPONSE;
    live_records[SENTINEL_SCAN_WIFI].error=readout_error;
    live_records[SENTINEL_SCAN_WIFI].captured=false;live_records[SENTINEL_SCAN_WIFI].count=0;
    live_make_view();drawn[0]='\0';cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND);
    READOUT_CHECK(sentinel_scan_view_draw(&live_view,0)==ESP_OK);
    READOUT_CHECK(strstr(drawn,"C5 REPLY INVALID")&&strstr(drawn,"NO NEW RESULTS / WAIT FOR C5"));
    save(prefix,"readout-offline-empty");
    arm=live_session.arm_generation;handle_live_scan_tap(100,380);
    READOUT_CHECK(live_session.arm_generation==arm&&live_commands==0);
    actual_live_list_back(UI_BACK_Y-1);READOUT_CHECK(current_page==UI_PAGE_WIFI);
    actual_live_list_back(UI_BACK_Y);READOUT_CHECK(current_page==UI_PAGE_OTHER&&live_commands==0);
    seed_wifi_recent();
    printf("Wi-Fi readout error presentation and recovery controls: %u checks passed.\n",checks);
#undef READOUT_CHECK
}

static void check_owned_wifi_observation_view(void)
{
    seed_wifi_recent();
    live_owned_wifi_observation=true;live_radio_busy=true;
    live_session.active=SENTINEL_SCAN_WIFI;live_session.completed=false;
    live_records[SENTINEL_SCAN_WIFI].state=SENTINEL_SCAN_RUNNING;
    live_make_view();
    assert(live_view.state==SENTINEL_VIEW_LIVE);
    assert(!strcmp(live_view.subtitle,"2.4 + 5 GHZ / LISTENING"));
    assert(live_radio_busy&&sentinel_scan_view_hit_test(&live_view,100,380)==SENTINEL_VIEW_PAUSE);
    assert(live_commands==0);
    /* This is a failed status observation, not a latched result-readout error. */
    live_records[SENTINEL_SCAN_WIFI].valid=false;
    live_records[SENTINEL_SCAN_WIFI].state=SENTINEL_SCAN_IDLE;
    live_records[SENTINEL_SCAN_WIFI].error=ESP_ERR_TIMEOUT;
    live_make_view();
    assert(readout_error==ESP_OK&&live_view.state==SENTINEL_VIEW_ERROR);
    assert(!strcmp(live_view.subtitle,"RADIO ERROR / ESP_ERR_TIMEOUT"));
    assert(live_radio_busy&&live_session.active==SENTINEL_SCAN_WIFI&&live_commands==0);
    /* The scheduler has released the completed owner, but admission is still
     * conservative until the next full observation. Presentation is complete. */
    live_records[SENTINEL_SCAN_WIFI].valid=true;
    live_records[SENTINEL_SCAN_WIFI].state=SENTINEL_SCAN_COMPLETE;
    live_records[SENTINEL_SCAN_WIFI].error=ESP_OK;
    live_session.active=SENTINEL_SCAN_NONE;live_session.completed=true;
    live_make_view();
    assert(live_view.state==SENTINEL_VIEW_COMPLETE);
    assert(!strcmp(live_view.subtitle,"SWEEP COMPLETE / TAP AGAIN TO RESCAN"));
    assert(live_radio_busy&&live_commands==0);
    /* A full observation identifying other radio work keeps its old message. */
    live_owned_wifi_observation=false;live_make_view();
    assert(!strcmp(live_view.subtitle,"WAITING FOR ACTIVE RADIO SESSION"));
    live_session.completed=false;live_records[SENTINEL_SCAN_WIFI].state=SENTINEL_SCAN_RUNNING;
    live_make_view();assert(!strcmp(live_view.subtitle,"WAITING FOR ACTIVE RADIO SESSION"));
    seed_wifi_recent();
    puts("Owned Wi-Fi observation view: listening, status timeout, completed release and unrelated busy messages passed");
}

static void check_analyzer_window_ui(const char *prefix)
{
    cerberus_ble_analyzer_t analyzer;
    const uint8_t ordinary[]={2,1,6};
    cerberus_ble_analyzer_report_t event={.address_type=1,.rssi_dbm=-48,.complete=true,
        .advertisement=ordinary,.advertisement_length=sizeof(ordinary)};
    cerberus_ble_analyzer_begin(&analyzer,0);
    ble_analyzer_started=true;ble_analyzer_start_pending=ble_analyzer_exit_pending=false;ble_analyzer_error=ESP_OK;
    cerberus_ble_analyzer_snapshot(&analyzer,0,&analyzer_view);
    drawn[0]='\0';cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND);assert(draw_ble_analyzer()==ESP_OK);
    assert(strstr(drawn,"PEAK -- / MEAN -- / N0"));
    assert(screen[155*320+40]==UI_PANEL);save(prefix,"analyzer-window-empty");
    for(unsigned i=0;i<6;++i)assert(cerberus_ble_analyzer_observe(&analyzer,&event,1));
    for(unsigned i=0;i<3;++i)assert(cerberus_ble_analyzer_observe(&analyzer,&event,201));
    cerberus_ble_analyzer_snapshot(&analyzer,600,&analyzer_view);
    /* A much larger partial bucket is visible as a separate number but must
     * not flatten completed graph bars, change mean, or claim a completed peak. */
    analyzer_view.current_events=UINT16_MAX;
    analyzer_view.total_events=UINT16_MAX+9U;
    drawn[0]='\0';cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND);assert(draw_ble_analyzer()==ESP_OK);
    assert(strstr(drawn,"PEAK 6 / MEAN 3.0 / N3")&&strstr(drawn,"PARTIAL >=65535"));
    assert(strstr(drawn,"N:200MS COMPLETE"));
    assert(screen[155*320+40]==UI_GOOD&&screen[203*320+40]==UI_MUTED);
    assert(screen[155*320+(32+61*4)]==UI_GOOD); /* actual six-count oldest bar */
    save(prefix,"analyzer-window-partial");
    cerberus_ble_analyzer_begin(&analyzer,0);
    for(unsigned b=0;b<64;++b){
        for(unsigned i=0;i<(b*3U)%13U;++i)assert(cerberus_ble_analyzer_observe(&analyzer,&event,b*200U+1));
        cerberus_ble_analyzer_advance(&analyzer,(b+1U)*200U);
    }
    cerberus_ble_analyzer_snapshot(&analyzer,12800,&analyzer_view);
    drawn[0]='\0';cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND);assert(draw_ble_analyzer()==ESP_OK);
    assert(strstr(drawn,"PEAK 12 / MEAN 5.9 / N64"));save(prefix,"analyzer-window-full");
    analyzer_view.history_count=64;for(unsigned i=0;i<64;++i)analyzer_view.history[i]=UINT16_MAX;
    analyzer_view.total_events=UINT32_MAX;analyzer_view.current_events=UINT16_MAX;
    analyzer_view.malformed_reports=analyzer_view.unclassified_reports=UINT32_MAX;
    drawn[0]='\0';cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND);assert(draw_ble_analyzer()==ESP_OK);
    assert(strstr(drawn,"PEAK >=65535 / MEAN >=65535.0 / N64"));
    assert(strstr(drawn,"BLE CALLBACKS >=4294967295 / PARTIAL >=65535"));save(prefix,"analyzer-window-capped");
    analyzer_view.history_count=255;drawn[0]='\0';assert(draw_ble_analyzer()==ESP_OK);
    assert(strstr(drawn,"PEAK -- / MEAN -- / N0"));
    memset(&analyzer_view,0,sizeof(analyzer_view));
}

static unsigned analyzer_ui_checks;
#define ANALYZER_CHECK(value) do { ++analyzer_ui_checks; assert(value); } while(0)

static void paint_analyzer(void)
{
    cerberus_ble_analyzer_view_t before=analyzer_view;
    analyzer_body_buttons=analyzer_stop_buttons=analyzer_stopping_buttons=analyzer_retry_buttons=0U;
    drawn[0]='\0';cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND);
    ANALYZER_CHECK(draw_ble_analyzer()==ESP_OK);
    unsigned retry=(unsigned)ble_analyzer_s3_retryable()+(unsigned)ble_analyzer_c5_retryable();
    ANALYZER_CHECK(analyzer_body_buttons==retry);
    if(retry){
        /* Error-only Retry must leave every footer row clear. */
        ANALYZER_CHECK(analyzer_button_y>=370U&&analyzer_button_y+analyzer_button_h<UI_BACK_Y);
    }else{
        bool body_clear=true;
        for(unsigned y=378U;y<UI_BACK_Y;++y)for(unsigned x=40U;x<280U;++x)
            body_clear&=screen[y*320U+x]==UI_BACKGROUND;
        ANALYZER_CHECK(body_clear); /* no hidden healthy/stopping body control */
    }
    ANALYZER_CHECK(actual_analyzer_footer()==ESP_OK);
    ANALYZER_CHECK(analyzer_stop_buttons==(ble_analyzer_exit_pending?0U:1U)&&
        analyzer_stopping_buttons==(ble_analyzer_exit_pending?1U:0U)&&analyzer_retry_buttons==(retry?1U:0U));
    ANALYZER_CHECK(memcmp(&before,&analyzer_view,sizeof(before))==0);
    ANALYZER_CHECK(!strstr(drawn,"MATCHED"));
}

static void check_analyzer_family_ui(const char *prefix)
{
    reset();current_page=UI_PAGE_BLE;ble_mode=UI_BLE_MODE_ANALYZER;
    ble_analyzer_started=true;ble_analyzer_error=ESP_OK;
    ble_analyzer_start_pending=ble_analyzer_exit_pending=false;
    const uint8_t mixed[]={3,3,0x44,0xfd,17,7,0x85,0x2a,0x9f,0x57,0xc5,0x2a,0xed,0x88,
        0x26,0xc2,0xf4,0x12,1,0,0x19,0x15,3,3,0x81,0x30};
    const uint8_t ordinary[]={2,1,6};
    cerberus_ble_analyzer_t analyzer;
    cerberus_ble_analyzer_report_t event={.address_type=1,.rssi_dbm=-48,.complete=true,
        .advertisement=mixed,.advertisement_length=sizeof(mixed)};
    cerberus_ble_analyzer_begin(&analyzer,0U);
    ANALYZER_CHECK(cerberus_ble_analyzer_observe(&analyzer,&event,100U));
    cerberus_ble_analyzer_snapshot(&analyzer,1400U,&analyzer_view);paint_analyzer();
    ANALYZER_CHECK(strstr(drawn,"FIND MY 1 / FLIPPER 1 EVENTS")&&
        strstr(drawn,"ADV HINT FMNA16 DULT / 1s")&&strstr(drawn,"ADV HINT 3081 BLACK CODE / 1s"));
    ANALYZER_CHECK(strstr(drawn,"HINT: Find My format hint"));save(prefix,"analyzer-family-adv");

    event.scan_response=true;
    ANALYZER_CHECK(cerberus_ble_analyzer_observe(&analyzer,&event,1500U));
    cerberus_ble_analyzer_snapshot(&analyzer,4900U,&analyzer_view);paint_analyzer();
    ANALYZER_CHECK(!analyzer_view.last_fresh&&strstr(drawn,"RSP -48 dBm / 3.4s")&&
        strstr(drawn,"RSP HINT FMNA16 DULT / 3s")&&strstr(drawn,"RSP HINT 3081 BLACK CODE / 3s"));
    ANALYZER_CHECK(strstr(drawn,"FIND MY 2 / FLIPPER 2 EVENTS"));save(prefix,"analyzer-family-rsp");
    paint_analyzer();ANALYZER_CHECK(analyzer_view.last_seen_age_ms==3400U);

    event.advertisement=ordinary;event.advertisement_length=sizeof(ordinary);
    ANALYZER_CHECK(cerberus_ble_analyzer_observe(&analyzer,&event,5000U));
    cerberus_ble_analyzer_snapshot(&analyzer,5100U,&analyzer_view);paint_analyzer();
    ANALYZER_CHECK(strstr(drawn,"RSP NO FIND MY HINT")&&strstr(drawn,"RSP NO FLIPPER UUID HINT")&&
        !strstr(drawn,"FMNA16")&&!strstr(drawn,"3081 BLACK CODE"));
    ANALYZER_CHECK(strstr(drawn,"FIND MY 2 / FLIPPER 2 EVENTS")&&strstr(drawn,"NO CLASSIFIED HINT IN LAST REPORT"));
    save(prefix,"analyzer-family-ordinary");
    event.advertisement=mixed;event.advertisement_length=sizeof(mixed);event.complete=false;
    ANALYZER_CHECK(cerberus_ble_analyzer_observe(&analyzer,&event,5200U));
    cerberus_ble_analyzer_snapshot(&analyzer,5300U,&analyzer_view);paint_analyzer();
    ANALYZER_CHECK(strstr(drawn,"FIND MY: NO COMPLETE REPORT")&&strstr(drawn,"FLIPPER: NO COMPLETE REPORT")&&
        !strstr(drawn,"FMNA16")&&!strstr(drawn,"3081 BLACK CODE"));save(prefix,"analyzer-family-invalid");
    /* A malformed/stale view cannot bypass the renderer's complete-report gate. */
    analyzer_view.last_findmy.bits=7U;analyzer_view.last_flipper.bits=15U;paint_analyzer();
    ANALYZER_CHECK(!strstr(drawn,"HINT APPLE")&&!strstr(drawn,"CODES"));

    const uint8_t beacon[27]={26,0xff,0x4c,0,2,0x15,0x12,0x34,0x56,0x78};
    const uint8_t meta[]={3,0xff,0x5f,0xfd},flock[]={3,0xff,0xc8,9};
    const struct {const uint8_t *bytes;size_t size;const char *detail,*preview;} cases[]={
        {beacon,sizeof(beacon),"IBEACON MAJOR","analyzer-family-ibeacon"},
        {meta,sizeof(meta),"FD5F MFG16","analyzer-family-meta"},
        {flock,sizeof(flock),"09C8+NO NAME","analyzer-family-flock"},
    };
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i){
        uint8_t packet[64]={0};memcpy(packet,cases[i].bytes,cases[i].size);
        memcpy(packet+cases[i].size,mixed,sizeof(mixed));
        event.advertisement=packet;event.advertisement_length=cases[i].size+sizeof(mixed);
        event.complete=true;event.scan_response=i!=0U;
        cerberus_ble_analyzer_begin(&analyzer,6000U);
        ANALYZER_CHECK(cerberus_ble_analyzer_observe(&analyzer,&event,6100U));
        cerberus_ble_analyzer_snapshot(&analyzer,6200U,&analyzer_view);paint_analyzer();
        ANALYZER_CHECK(strstr(drawn,cases[i].detail)&&strstr(drawn,"FMNA16 DULT")&&strstr(drawn,"3081 BLACK CODE"));
        ANALYZER_CHECK(strstr(drawn,"FIND MY 1 / FLIPPER 1 EVENTS"));save(prefix,cases[i].preview);
    }
    /* The combined iBeacon fixture is >31 bytes: rendering/parser evidence,
     * not proof of extended reception in the currently legacy native build. */
    analyzer_view.findmy_hint_events=analyzer_view.flipper_hint_events=UINT32_MAX;
    analyzer_view.total_events=UINT32_MAX;analyzer_view.current_events=UINT16_MAX;
    analyzer_view.last_findmy=(cerberus_ble_findmy_hints_t){7U,7U};
    analyzer_view.last_flipper=(cerberus_ble_flipper_hints_t){15U,31U};
    analyzer_view.last_seen_age_ms=UINT32_MAX;analyzer_view.last_fresh=false;
    analyzer_view.last_rssi_dbm=127;paint_analyzer();
    ANALYZER_CHECK(strstr(drawn,"FIND MY >=4294967295 / FLIPPER >=4294967295 EVENTS")&&
        strstr(drawn,"APPLE FMNA16+32+128 DULT / AGE UNKNOWN")&&
        strstr(drawn,"3080/81/82/83 CODES+NAME+DATA / AGE ?")&&strstr(drawn,"RSSI N/A"));
    save(prefix,"analyzer-family-capped");
    cerberus_ble_analyzer_begin(&analyzer,7000U);
    cerberus_ble_analyzer_snapshot(&analyzer,7000U,&analyzer_view);paint_analyzer();
    ANALYZER_CHECK(strstr(drawn,"FIND MY 0 / FLIPPER 0 EVENTS")&&strstr(drawn,"WAITING FOR ADVERTISEMENTS"));
    save(prefix,"analyzer-family-reset");
}

#include "ble_analyzer_ui_cases.h"
#include "ble_fox_ui_cases.h"

int main(int argc,char **argv)
{
    assert(existing_monitor_suite(argc,argv)==0);seed_list();
    /* Actual header hit advances FIND MY to FLIPPER and applies its category. */
    handle_ble_tap(100,65);assert(ble_filter==CERBERUS_S3_BLE_FILTER_FLIPPER);
    assert(live_ble_filtered_count==3&&live_ble_indexes[0]==0&&live_ble_indexes[1]==2&&live_ble_indexes[2]==3);
    assert(!strcmp(live_badges[0],"ADV 3082 / RSP 3083 / HINTS"));
    assert(!strcmp(live_names[2],"Unnamed device"));assert(!strstr(live_details[2],"77:00:00:00:00:33"));
    /* Real sort hit, full view builder and renderer preserve original index. */
    handle_ble_tap(250,65);assert(live_ble_sort==CERBERUS_S3_BLE_SORT_NAME_ASC);
    assert(live_ble_indexes[0]==3&&live_ble_indexes[1]==2&&live_ble_indexes[2]==0);
    drawn[0]='\0';assert(sentinel_scan_view_draw(&live_view,0)==ESP_OK);save(argv[1],"focused-list");
    assert(strstr(drawn,"ADV 3082 / RSP 3083 / HINTS"));
    cerberus_findmy_monitor_record_t expected=observations[2];
    ++observations[2].generation;handle_ble_tap(20,200);assert(ble_mode==UI_BLE_MODE_LIST);observations[2]=expected;
    handle_ble_tap(20,200);assert(ble_mode==UI_BLE_MODE_DETAIL&&!ble_findmy_stopping);
    tick(8200);assert(ble_mode==UI_BLE_MODE_DETAIL&&ble_selected_raw_frozen&&ble_selected_scan_index==2);
    assert(!memcmp(&ble_findmy_selected,&expected,sizeof(expected)));
    assert(ble_selected_sound_profile==CERBERUS_FINDMY_PROFILE_NONE);
    memset(observations,0,sizeof(observations));memset(&store,0,sizeof(store));++scan.generation;
    drawn[0]='\0';cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND);assert(actual_hint_detail()==ESP_OK);
    assert(strstr(drawn,"ADV HINT 3082 WHITE CODE / 8s"));assert(strstr(drawn,"RSP HINT 3083 TRANSPARENT CODE+NAME / 7s"));save(argv[1],"frozen-detail");

    /* The real detail TRACK branch sends the frozen typed address to the
     * unchanged Fox Hunt API even after the source list has been replaced. */
    handle_ble_tap(30,300);assert(dual_ble_live_prepare_fox((uint32_t)clock_ms,false));assert(ble_mode==UI_BLE_MODE_FOX_HUNT&&track_begins==1);
    assert(!memcmp(track_address,expected.peer.address,6)&&track_type==expected.peer.address_type);
    ble_fox_hunt_tick(8500,false);assert(passive_starts==1&&owner==CERBERUS_S3_BLE_OWNER_SCANNER);
    assert(!memcmp(&ble_findmy_selected,&expected,sizeof(expected)));
    clock_ms=8600;actual_back(440);hold_stop=true;ble_fox_hunt_tick(8800,false);
    assert(ble_fox_hunt_exit_pending&&ble_mode==UI_BLE_MODE_FOX_HUNT&&track_ends==0);
    owner=CERBERUS_S3_BLE_OWNER_NONE;scan.state=CERBERUS_BLE_SCAN_COMPLETE;ble_fox_hunt_tick(9100,false);
    assert(ble_mode==UI_BLE_MODE_DETAIL&&track_ends==1);
    assert(!memcmp(&ble_findmy_selected,&expected,sizeof(expected)));
    clock_ms=10100;drawn[0]='\0';assert(actual_hint_detail()==ESP_OK);assert(strstr(drawn,"ADV HINT 3082 WHITE CODE / 10s"));
    actual_back(440);assert(ble_mode==UI_BLE_MODE_LIST&&ble_filter==CERBERUS_S3_BLE_FILTER_FLIPPER);

    /* Render worst-case code mixtures, name/service-data provenance and long
     * ages. Do not silently truncate a response or imply physical colors. */
    ble_selected_focus=expected.focus;ble_selected_focus.discovery.flipper_advertisement=(cerberus_ble_flipper_hints_t){15,31};
    ble_selected_focus.discovery.flipper_scan_response=(cerberus_ble_flipper_hints_t){8,31};
    clock_ms=INT32_MAX;drawn[0]='\0';cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND);assert(actual_hint_detail()==ESP_OK);
    assert(strstr(drawn,"3080/81/82/83 CODES+NAME+DATA"));assert(strstr(drawn,"3083 TRANSPARENT CODE+NAME+DATA"));save(argv[1],"mixed-detail");
    char text[64];cerberus_ble_flipper_hints_t name_only={0,CERBERUS_BLE_FLIPPER_NAME};
    cerberus_flipper_hint_line(&name_only,false,0,100,text,sizeof(text));assert(!strcmp(text,"ADV NAME ONLY / NO UUID HINT"));
    cerberus_flipper_hint_line(&expected.focus.discovery.flipper_advertisement,false,UINT32_MAX-100,1100,text,sizeof(text));assert(strstr(text," / 1s"));
    check_meta_ui(argv[1]);
    check_ibeacon_ui(argv[1]);
    check_flock_ui(argv[1]);
    check_hc_ui(argv[1]);
    check_recent_ui(argv[1]);
    check_start_publication_lifecycle();
    check_wifi_recent_ui(argv[1]);
    check_wifi_order_controls(argv[1]);
    check_wifi_painted_binding();
    check_wifi_list_geometry_and_cache();
    printf("Wi-Fi list UI: %u checks; actual sort/band/age, immutable maps, successful-paint binding, failure recovery, hit regions and cache-only controls passed\n",wifi_list_ui_checks);
    check_wifi_readout_error_ui(argv[1]);
    check_wifi_restricted_coverage();
    check_owned_wifi_observation_view();
    check_analyzer_window_ui(argv[1]);
    check_analyzer_family_ui(argv[1]);
    check_analyzer_controls(argv[1]);
    check_analyzer_dual_ui(argv[1]);
    check_fox_composite_ui(argv[1]);
    current_page=UI_PAGE_BLE;ble_mode=UI_BLE_MODE_FOX_HUNT;ble_fox_hunt_exit_pending=false;
    drawn[0]=0;assert(actual_fox_footer()==ESP_OK&&strstr(drawn,"STOP AND BACK"));
    ble_fox_hunt_exit_pending=true;drawn[0]=0;assert(actual_fox_footer()==ESP_OK&&strstr(drawn,"STOPPING"));
    puts("BLE discovery UI: production filter/sort/age/cards/hit-test, timed completed/paused expiry without RF start, publication ceiling/wrap, frozen typed selection through Fox Hunt/Back, source ages, priorities, privacy, RSSI and unchanged Sound gate passed");
    return 0;
}
