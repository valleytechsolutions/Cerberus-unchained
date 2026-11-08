/* Reuse the real view and its typed renderer/coordinator test adapters. */
#define main existing_view_checks
#include "dual_wifi_ui_check.c"
#undef main
#include "scan_display_store.h"
#include "sentinel_scan_session.h"
#include "wifi_ap_job.h"
#include "wifi_targets.h"
#include "field_menu.h"
#include "wifi_packet_count_scan.h"
#define ESP_ERROR_CHECK(call) assert((call) == ESP_OK)

enum { UI_PAGE_WIFI, UI_PAGE_OTHER, UI_WIFI_MODE_DUAL, UI_WIFI_MODE_LIST,
    UI_WIFI_MODE_TARGET };
static int current_page, wifi_mode;
static bool wifi_attack_selecting, ui_capability_blocked, wifi_selection_valid;
static bool dual_wifi_return_to_view, c5_online;
static cerberus_c5_status_t c5_status;
static cerberus_field_menu_t field_menu;
static sentinel_scan_session_t live_session, live_ble_session;
typedef struct { bool released; } mock_discovery_session_t;
static struct { mock_discovery_session_t session; } discovery_store;
static __typeof__(discovery_store) *ble_discovery_live;
typedef struct {
    bool valid, captured;
    uint8_t state, generation, saved_generation, completed;
    uint16_t count;
    uint32_t captured_ms;
    esp_err_t error;
} live_scan_record_t;
static live_scan_record_t live_records[SENTINEL_SCAN_TOOL_COUNT];
static cerberus_scan_display_store_t *live_store;
static uint64_t wifi_relation_publication;
static bool wifi_relation_exhausted;
static int64_t live_wifi_published_us;
static uint32_t live_capture_pending, live_last_poll;
static uint8_t live_wifi_built, live_wifi_painted, live_page[SENTINEL_SCAN_TOOL_COUNT];
static cerberus_wifi_scan_status_payload_t wifi_scan_status;
static cerberus_c5_wifi_result_t wifi_selected_result;
static uint8_t wifi_selected_index, wifi_analysis_generation;
static esp_err_t wifi_analysis_error, wifi_trust_error, wifi_connection_error;
static cerberus_wifi_connection_status_payload_t wifi_connection_status;
static bool readout_busy, companion_released, store_fail, proof_valid;
static uint64_t recon_lease_epoch;
static unsigned rebind_calls, copy_calls, publish_calls, bind_calls, launcher_calls;
static esp_err_t rebind_error, copy_error;
static unsigned corrupt_export;
static cerberus_dual_wifi_row_t requested;
static cerberus_dual_wifi_handoff_t export_handoff, imported_handoff;
static int64_t mock_time_us;
static unsigned archive_all_calls, archive_selected_calls;
static unsigned packet_count_entries;
static wifi_packet_count_selection_t packet_count_selection;
static esp_err_t wifi_packet_count_open_selection(const wifi_packet_count_selection_t *selection,unsigned origin)
{ assert(origin==UI_WIFI_MODE_DUAL);packet_count_selection=*selection;++packet_count_entries;return ESP_OK; }
static void wifi_archive_save_live(bool selected)
{ if (selected) ++archive_selected_calls; else ++archive_all_calls; }

static bool live_wifi_capture_busy(void) { return readout_busy; }
static bool live_ble_recon_released(void) { return recon_lease_epoch == 0U; }
static bool cerberus_ble_discovery_session_released(const mock_discovery_session_t *s)
{ return s->released; }
static bool dual_ble_ui_released(void) { return companion_released; }
static void ble_discovery_leave(void) { /* Release remains asynchronous. */ }
static void wifi_relation_cancel(void) { }
static bool live_store_acquire(void)
{
    if (store_fail) return false;
    if (!live_store) live_store = calloc(1U, sizeof(*live_store));
    assert(live_store); return true;
}
static void live_wifi_capture_reset(void) { assert(!readout_busy); }
static int64_t esp_timer_get_time(void) { return mock_time_us; }
static void wifi_commit_cached_results_at(const cerberus_c5_wifi_result_t *rows,
    uint16_t count, uint8_t generation, int64_t received_us, uint8_t band,
    const cerberus_c5_wifi_relation_bank_t *proof, uint64_t session_id)
{
    assert(rows == live_store->wifi && count == 3 && generation == 91);
    assert(proof->status.generation == generation);
    imported_handoff = (cerberus_dual_wifi_handoff_t){
        .received_us = received_us, .session_id = session_id, .band = band};
}
bool cerberus_s3_wifi_relation_meta_valid(const cerberus_s3_wifi_relation_meta_t *proof)
{ return proof_valid && proof->clock_valid; }
static void wifi_relation_publish(uint8_t generation,
    const cerberus_c5_wifi_relation_bank_t *proof, esp_err_t error)
{
    assert(generation == 91 && proof->status.generation == 91 && !error);
    ++publish_calls; ++wifi_relation_publication;
}
static void wifi_relation_bind_selection(bool ordinary)
{
    assert(ordinary && publish_calls && wifi_selection_valid && wifi_selected_index == 2);
    assert(!memcmp(&wifi_selected_result, &live_store->wifi[2], sizeof(wifi_selected_result)));
    ++bind_calls;
}
static void wifi_target_finish_selection(void) { wifi_mode = UI_WIFI_MODE_TARGET; }
static esp_err_t draw_detail_page(void)
{ return wifi_mode == UI_WIFI_MODE_DUAL ? dual_wifi_ui_draw() : ESP_OK; }
static void return_to_launcher(void)
{ ++launcher_calls; current_page = UI_PAGE_OTHER; dual_wifi_ui_leave(); }
#include "target_sync_impl.h"

esp_err_t cerberus_dual_wifi_scan_begin_rebind(const cerberus_dual_wifi_row_t *target)
{
    ++rebind_calls; requested = *target;
    if (!rebind_error) {
        mock_status.rebind_ready = false;
        mock_status.rebinding = true;
        mock_status.state = CERBERUS_DUAL_WIFI_RUNNING;
    }
    return rebind_error;
}
esp_err_t cerberus_dual_wifi_scan_copy_legacy_bank(cerberus_c5_wifi_result_t *rows,
    cerberus_s3_wifi_observation_t *observations, size_t capacity,
    cerberus_wifi_scan_status_payload_t *status, cerberus_c5_wifi_relation_bank_t *proof,
    uint8_t *selected, cerberus_dual_wifi_handoff_t *handoff)
{
    ++copy_calls; assert(capacity == 32U);
    assert(mock_status.rebind_ready && !mock_status.rebinding && !mock_status.cleanup_pending);
    if (copy_error) return copy_error;
    memset(rows, 0, capacity * sizeof(*rows));
    memset(observations, 0, capacity * sizeof(*observations));
    rows[0].channel = rows[1].channel = requested.ap.channel;
    rows[2] = requested.ap;
    observations[2] = (cerberus_s3_wifi_observation_t){.valid = true,
        .request_started_us = 111111U, .source_age_ms = 70U};
    *status = (cerberus_wifi_scan_status_payload_t){.state = CERBERUS_WIFI_SCAN_COMPLETE,
        .generation = 91, .stored_count = 3, .total_count = 3};
    memset(proof, 0, sizeof(*proof)); proof->clock_valid = true; proof->status.generation = 91;
    *selected = 2;
    *handoff = export_handoff;
    switch (corrupt_export) {
    case 1: ++rows[2].channel; break;
    case 2: ++rows[2].security; break;
    case 3: ++rows[2].bssid[5]; break;
    case 4: rows[2].ssid[0] = '!'; break;
    case 5: memset(rows[2].ssid, 'x', sizeof(rows[2].ssid)); break;
    case 6: status->stored_count = 33; break;
    case 7: *selected = 3; break;
    case 8: proof->status.generation = 90; break;
    case 9: status->state = CERBERUS_WIFI_SCAN_RUNNING; break;
    case 10: status->error_code = ESP_FAIL; break;
    case 11: status->stored_count = 0; break;
    case 16: handoff->received_us = -1; break;
    case 17: handoff->received_us = mock_time_us + 1; break;
    case 18: handoff->session_id = 0; break;
    case 19: handoff->band = 3; break;
    case 20: handoff->reused = true; break; /* S3 source cannot reuse a C5 row. */
    case 21: handoff->band = CERBERUS_WIFI_SCAN_BAND_5G; break; /* Fresh rebind is all-band. */
    default: break;
    }
    return ESP_OK;
}

#include "../../s3_main/main/dual_wifi_live_ui.h"

static void bridge_reset(void)
{
    reset();
    wifi_targets_clear();
    free(live_store); live_store = NULL;
    sentinel_scan_session_init(&live_session); sentinel_scan_session_init(&live_ble_session);
    memset(live_records, 0, sizeof(live_records));
    memset(&wifi_selected_result, 0x5a, sizeof(wifi_selected_result));
    memset(&wifi_scan_status, 0, sizeof(wifi_scan_status));
    ble_discovery_live = NULL;
    companion_released = proof_valid = true;
    recon_lease_epoch = 0U;
    readout_busy = store_fail = wifi_relation_exhausted = false;
    rebind_error = copy_error = ESP_OK; corrupt_export = 0U;
    rebind_calls = copy_calls = publish_calls = bind_calls = launcher_calls = 0U;
    packet_count_entries=0;memset(&packet_count_selection,0,sizeof(packet_count_selection));
    dual_wifi_live_handoff_pending = dual_wifi_return_to_view = false;
    ui_capability_blocked = false;
    current_page = UI_PAGE_WIFI; wifi_mode = UI_WIFI_MODE_DUAL;
    wifi_relation_publication = 2U;
    live_capture_pending = UINT32_MAX;
    mock_time_us = 1234567LL;
    export_handoff = (cerberus_dual_wifi_handoff_t){.received_us=900000,
        .session_id=UINT64_C(0xabcddcba00000042), .band=CERBERUS_WIFI_SCAN_BAND_ALL};
    memset(&imported_handoff, 0, sizeof(imported_handoff));
}
static void select_and_request(void)
{
    publish(4, 42); dual_wifi_live_open(false); draw_ok();
    dual_wifi_live_tap(20, 160); /* Select S3 identity, not a legacy C5 index. */
    dual_wifi_live_tap(30, 335);
    assert(!rebind_calls);
    dual_wifi_live_tick(300, false);
    assert(rebind_calls == 1 && requested.source == CERBERUS_DUAL_WIFI_SOURCE_S3);
    assert(!publish_calls && dual_wifi_live_handoff_pending);
}
static void finish_rebind(void)
{
    mock_status.state = CERBERUS_DUAL_WIFI_COMPLETE;
    mock_status.rebinding = mock_status.cleanup_pending = false;
    mock_status.rebind_ready = true;
}
static void released_admission(void)
{
    bridge_reset(); dual_wifi_live_open(true);
    live_session.active = SENTINEL_SCAN_WIFI;
    dual_wifi_live_tick(1, false); assert(!start_calls);
    live_session.active = SENTINEL_SCAN_NONE;
    live_ble_session.pending.kind = SENTINEL_SCAN_COMMAND_STOP;
    dual_wifi_live_tick(2, false); assert(!start_calls);
    live_ble_session.pending.kind = SENTINEL_SCAN_COMMAND_NONE;
    readout_busy = true; dual_wifi_live_tick(3, false); assert(!start_calls);
    readout_busy = false; companion_released = false;
    dual_wifi_live_tick(4, false); assert(!start_calls);
    companion_released = true; ble_discovery_live = &discovery_store;
    discovery_store.session.released = false;
    dual_wifi_live_tick(5, false); assert(!start_calls);
    discovery_store.session.released = true;
    live_session.stop_requested = true; dual_wifi_live_tick(6, false); assert(!start_calls);
    live_session.stop_requested = false; live_session.waiting = true;
    /* A completed native Recon attachment outlives scheduler.active. Opening
     * split Wi-Fi must wait for that exact attachment, without an early Start
     * failure or any retry that consumes the pending user request. */
    recon_lease_epoch = UINT64_C(0x100000100);
    assert(live_ble_session.active == SENTINEL_SCAN_NONE);
    for (unsigned now = 7U; now < 20U; ++now) {
        dual_wifi_live_tick(now, false);
        assert(!start_calls && !rebind_calls && !dual_wifi_live_released());
    }
    recon_lease_epoch = 0U;
    dual_wifi_live_tick(20, false); assert(start_calls == 1); /* Backoff cannot deadlock idle. */
    dual_wifi_live_tick(21, false); assert(start_calls == 1);
}
static void publication(void)
{
    bridge_reset();
    cerberus_c5_wifi_result_t prior = {.bssid = {2, 2, 2, 2, 2, 2}, .channel = 11};
    strcpy(prior.ssid, "Prior selection A");
    assert(wifi_targets_add(&prior, 19));
    select_and_request(); finish_rebind();
    dual_wifi_live_tick(600, false);
    assert(publish_calls == 1 && bind_calls == 1 && copy_calls == 1);
    assert(!dual_wifi_live_handoff_pending && dual_wifi_return_to_view && !dual_wifi_ui_opened);
    assert(wifi_mode == UI_WIFI_MODE_TARGET && wifi_selected_index == 2);
    assert(wifi_target_count == 0); /* Old multi-select cannot override new B. */
    wifi_target_sync_menu();
    assert(wifi_target_count == 1 && wifi_selected_index == 2 &&
        !memcmp(wifi_selected_result.bssid, requested.ap.bssid, 6));
    assert(live_records[SENTINEL_SCAN_WIFI].captured && live_records[SENTINEL_SCAN_WIFI].count == 3);
    assert(live_records[SENTINEL_SCAN_WIFI].saved_generation == 91 && wifi_scan_status.generation == 91);
    assert(live_records[SENTINEL_SCAN_WIFI].captured_ms == 900 && live_wifi_published_us == 900000);
    assert(imported_handoff.received_us == export_handoff.received_us &&
        imported_handoff.session_id == export_handoff.session_id && imported_handoff.band == CERBERUS_WIFI_SCAN_BAND_ALL);
    assert(live_store->wifi_observations[2].valid && live_store->wifi_observations[2].source_age_ms == 70);
    assert(!(live_capture_pending & (1U << SENTINEL_SCAN_WIFI)));
    dual_wifi_live_tick(900, false); assert(publish_calls == 1);
    dual_wifi_live_open(false); dual_wifi_live_tick(1200, false);
    assert(!start_calls && !dual_wifi_return_to_view && dual_wifi_ui_store->count == 4);
}
static void rejected_publications(void)
{
    for (unsigned fault = 1; fault <= 21; ++fault) {
        bridge_reset(); select_and_request(); finish_rebind();
        assert(live_store_acquire());
        memset(live_store->wifi, 0x35, sizeof(live_store->wifi));
        if (fault <= 11 || fault >= 16) corrupt_export = fault;
        if (fault == 12) proof_valid = false;
        if (fault == 13) wifi_relation_publication = UINT64_MAX;
        if (fault == 14) copy_error = ESP_ERR_INVALID_STATE;
        if (fault == 15) store_fail = true;
        dual_wifi_live_tick(600, false);
        assert(!publish_calls && !bind_calls && !dual_wifi_return_to_view);
        assert(!dual_wifi_live_handoff_pending && wifi_mode == UI_WIFI_MODE_DUAL);
        assert((unsigned char)live_store->wifi[0].ssid[0] == 0x35);
        assert((unsigned char)wifi_selected_result.ssid[0] == 0x5a);
        assert(dual_wifi_ui_error != ESP_OK);
    }
}
static void cached_c5_handoff(void)
{
    bridge_reset();
    publish(4, 42); dual_wifi_live_open(false); draw_ok();
    dual_wifi_live_tap(20, 220); /* Second row is C5/5GHz. */
    dual_wifi_live_tap(30, 335); dual_wifi_live_tick(300, false);
    assert(rebind_calls == 1 && requested.source == CERBERUS_DUAL_WIFI_SOURCE_C5);
    export_handoff.reused = true;
    export_handoff.band = CERBERUS_WIFI_SCAN_BAND_5G;
    export_handoff.received_us = 500000;
    finish_rebind(); mock_status.rebind_cached = true;
    dual_wifi_live_tick(600, false);
    assert(publish_calls == 1 && live_records[SENTINEL_SCAN_WIFI].captured_ms == 500);
    assert(live_wifi_published_us == 500000 && imported_handoff.received_us == 500000);
    assert(imported_handoff.band == CERBERUS_WIFI_SCAN_BAND_5G &&
        imported_handoff.session_id == export_handoff.session_id);
    const cerberus_s3_wifi_observation_t original = live_store->wifi_observations[2];
    const cerberus_c5_wifi_result_t ap = wifi_selected_result;

    /* Back/reopen later retains the original capture and callback anchors. */
    mock_time_us += 900000000;
    dual_wifi_live_open(false); draw_ok();
    dual_wifi_live_tap(20, 220);
    dual_wifi_live_tap(30, 335); dual_wifi_live_tick(900100, false);
    assert(rebind_calls == 2 && requested.source == CERBERUS_DUAL_WIFI_SOURCE_C5);
    finish_rebind(); mock_status.rebind_cached = true;
    dual_wifi_live_tick(900400, false);
    assert(publish_calls == 2 && !start_calls && live_wifi_published_us == 500000);
    assert(live_records[SENTINEL_SCAN_WIFI].captured_ms == 500);
    assert(!memcmp(&original, &live_store->wifi_observations[2], sizeof(original)));
    assert(!memcmp(&ap, &wifi_selected_result, sizeof(ap)));

    /* A ready cached bank must still obey user cancellation before import. */
    dual_wifi_live_open(false); draw_ok();
    dual_wifi_live_tap(20, 220);
    dual_wifi_live_tap(30, 335); dual_wifi_live_tick(900500, false);
    finish_rebind(); mock_status.rebind_cached = true;
    dual_wifi_live_tap(20, 440);
    dual_wifi_live_tick(900600, false);
    assert(publish_calls == 2 && !dual_wifi_live_handoff_pending);
}
static void cancellation_and_fallback(void)
{
    for (unsigned mode = 0; mode < 5; ++mode) {
        bridge_reset(); select_and_request();
        if (mode == 0) dual_wifi_live_tap(20, 440); /* Detail Back. */
        else if (mode == 1) dual_wifi_live_tap(180, 440); /* Stop during rebind. */
        else if (mode == 2) dual_wifi_live_tick(400, true); /* Sleep. */
        else if (mode == 3) { current_page = UI_PAGE_OTHER; dual_wifi_live_tick(400, false); }
        else { dual_wifi_ui_store->selected.source_generation++; dual_wifi_live_tick(400, false); }
        assert(!dual_wifi_live_handoff_pending);
        finish_rebind(); dual_wifi_live_tick(600, false);
        assert(!publish_calls && !dual_wifi_return_to_view);
    }
    bridge_reset(); dual_wifi_live_open(true); start_error = ESP_ERR_NOT_SUPPORTED;
    dual_wifi_live_tick(200, false);
    assert(wifi_mode == UI_WIFI_MODE_LIST && !dual_wifi_ui_opened);
    assert(live_session.desired == SENTINEL_SCAN_WIFI && !live_session.completed && !live_session.paused);
    bridge_reset(); dual_wifi_live_open(true); current_page = UI_PAGE_OTHER;
    dual_wifi_live_tick(200, false); assert(!start_calls && !dual_wifi_ui_opened && step_calls);
    bridge_reset(); dual_wifi_live_open(false); dual_wifi_live_tap(20, 440);
    assert(launcher_calls == 1 && current_page == UI_PAGE_OTHER);
    bridge_reset(); publish(4, 42); dual_wifi_live_open(false); draw_ok();
    dual_wifi_live_tap(20, 160); dual_wifi_live_tap(20, 335);
    rebind_error = ESP_FAIL; dual_wifi_live_tick(300, false);
    assert(!dual_wifi_live_handoff_pending && dual_wifi_ui_error == ESP_FAIL && !publish_calls);
}
static void packet_count_navigation(void)
{
    for(unsigned source=0;source<2;++source){
        bridge_reset();publish(4,42);dual_wifi_live_open(false);draw_ok();
        dual_wifi_live_tap(20,source?220:160);
        cerberus_dual_wifi_row_t frozen;assert(dual_wifi_ui_copy_selected(&frozen)==ESP_OK);
        unsigned starts=start_calls,rebinds=rebind_calls;
        /* Replace the producer bank after painting: this route copies the
         * selected value, never a new source index or attack target set. */
        mock_rows[source].ap.bssid[5]^=0xff;
        dual_wifi_live_tap(180,335);
        assert(packet_count_entries==1&&packet_count_selection.target.role==CERBERUS_WIFI_PACKET_COUNT_AP);
        assert(packet_count_selection.target.channel_hint==frozen.ap.channel&&
            !memcmp(packet_count_selection.target.mac,frozen.ap.bssid,6)&&
            !memcmp(packet_count_selection.bssid,frozen.ap.bssid,6));
        assert(packet_count_selection.discovery_source==frozen.source&&
            packet_count_selection.discovery_publication==frozen.source_generation);
        assert(!memcmp(packet_count_selection.label,frozen.ap.ssid,32));
        assert(start_calls==starts&&rebind_calls==rebinds&&!dual_wifi_live_handoff_pending);
    }
}
int main(void)
{
    existing_view_checks();
    dual_wifi_live_action(DUAL_WIFI_UI_SAVE_ALL);
    dual_wifi_live_action(DUAL_WIFI_UI_SAVE_SELECTED);
    assert(archive_all_calls == 1U && archive_selected_calls == 1U);
    released_admission(); publication(); rejected_publications(); cancellation_and_fallback();
    cached_c5_handoff();
    packet_count_navigation();
    bridge_reset(); dual_wifi_ui_leave(); free(live_store);
    puts("Dual Wi-Fi bridge: owner admission, C5 proof/index publication, identity faults, legacy fallback and Back/Stop/sleep cancellation passed.");
    return 0;
}
