#include "recon_framebuffer.h"
#include "sentinel_scan_session.h"
#include "sentinel_watch_activity.h"
#include "s3_ble_scanner.h"
#include "s3_findmy_monitor.h"
#include "s3_wifi_list.h"
#include "scan_display_store.h"
#include "recon_store.h"
#include "recon_recorder.h"
#include "recon_live_types.h"

#define UI_BACKGROUND 0x0042
#define UI_PANEL 0x08A4
#define UI_MUTED 0x7CB5
#define UI_TEXT 0xE77F
#define UI_GOOD 0x471B
#define TAG "recon-ui-check"
#define CERBERUS_NOTIFICATION_TEXT_MAX 64U
#define ESP_ERROR_CHECK(call) do { assert((call) == ESP_OK); } while (0)
static unsigned checks;
#define CHECK(test) do { ++checks; if (!(test)) { fprintf(stderr, "FAIL %s:%u: %s\n", __FILE__, __LINE__, #test); exit(1); } } while (0)
#define MALLOC_CAP_SPIRAM 1U
#define MALLOC_CAP_8BIT 2U
static void *inventory_allocation;
static unsigned allocation_calls, free_calls;
static bool allocation_fail;
static void *heap_caps_calloc(size_t count, size_t size, unsigned capabilities)
{
#ifdef CERBERUS_RECON_LIVE_TEST
    ++allocation_calls;CHECK(capabilities==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    return allocation_fail?NULL:calloc(count,size);
#else
    ++allocation_calls; CHECK(!inventory_allocation);
    CHECK(capabilities == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (allocation_fail) return NULL;
    inventory_allocation = calloc(count, size); CHECK(inventory_allocation); return inventory_allocation;
#endif
}
static void heap_caps_free(void *pointer)
{
#ifdef CERBERUS_RECON_LIVE_TEST
    if(pointer){++free_calls;free(pointer);}
#else
    CHECK(pointer && pointer == inventory_allocation); ++free_calls;
    free(pointer); inventory_allocation = NULL;
#endif
}
enum { UI_PAGE_MENU, UI_PAGE_WATCH, UI_PAGE_WIFI, UI_PAGE_BLE, UI_PAGE_IEEE802154, UI_PAGE_FIELD_MENU };
static int current_page;
static bool ui_capability_blocked, c5_online, live_ble_session_initialized, live_radio_busy;
static const char *ui_capability_blocked_reason;
static uint32_t live_last_poll;
static cerberus_c5_status_t c5_status;
static cerberus_wifi_scan_status_payload_t wifi_scan_status;
static cerberus_ble_scan_status_payload_t ble_scan_status;
static sentinel_scan_session_t live_session, live_ble_session;
static live_scan_record_t live_records[SENTINEL_SCAN_TOOL_COUNT];
static cerberus_scan_display_store_t store;
static cerberus_scan_display_store_t *live_store = &store;
static cerberus_findmy_monitor_record_t evidence[CERBERUS_BLE_SCAN_MAX_RESULTS];
static cerberus_findmy_monitor_record_t *live_ble_evidence = evidence;
static int64_t clock_us;
static int64_t clock_script[8];
static size_t clock_script_count, clock_script_index;
static unsigned raw_calls, status_calls, c5_calls, extra_calls, notifications;
static esp_err_t raw_error, extra_error;
static unsigned raw_fail_index;
static bool raw_identity_bad, raw_final_bad, extra_final_bad, extra_drone;
static bool recon_lifecycle_active;
static char drawn[16384];
static size_t drawn_size;
static cerberus_recon_record_t offered[256];
static size_t offer_count;
static unsigned start_calls, stop_calls;
static bool reject_offer;
static esp_err_t start_error;
static cerberus_recon_recorder_status_t recorder_status;

static const char *esp_err_to_name(esp_err_t error)
{ return error == ESP_ERR_NO_MEM ? "ESP_ERR_NO_MEM" : error == ESP_ERR_INVALID_STATE ? "ESP_ERR_INVALID_STATE" : error == ESP_OK ? "ESP_OK" : "ESP_FAIL"; }
esp_err_t cerberus_recon_recorder_start(void)
{
    ++start_calls;
    if (start_error != ESP_OK) return start_error;
    recorder_status.state = CERBERUS_RECON_RECORDER_STARTING;
    recorder_status.worker_active = true;
    return ESP_OK;
}
void cerberus_recon_recorder_stop(void)
{
    ++stop_calls;
    if (recorder_status.worker_active) recorder_status.state = CERBERUS_RECON_RECORDER_STOPPING;
}
void cerberus_recon_recorder_get_status(cerberus_recon_recorder_status_t *out)
{ CHECK(out); *out = recorder_status; }

static int64_t esp_timer_get_time(void)
{ return clock_script_index < clock_script_count ? clock_script[clock_script_index++] : clock_us; }
static sentinel_scan_session_t *live_session_for(sentinel_scan_tool_t tool)
{ return tool == SENTINEL_SCAN_BLE ? &live_ble_session : &live_session; }
static void open_main_menu(void) { current_page = UI_PAGE_MENU; }
static void cerberus_ui_notification_add(uint32_t now, const char *text)
{ (void)now; CHECK(text && *text); ++notifications; }
static bool privacy_addresses_masked(void) { return true; }
static void cerberus_privacy_format_ble_identifier(const uint8_t *address, bool mask, char *out, size_t size)
{ (void)address; (void)mask; snprintf(out, size, "PRIVATE BLE ADDRESS"); }
static void cerberus_privacy_format_wifi_identifier(const uint8_t *address, bool mask, char *out, size_t size)
{ (void)address; (void)mask; snprintf(out, size, "PRIVATE WIFI ADDRESS"); }
recon_db_result_t recon_store_ble_vendor(const uint8_t *address, uint8_t type, char *out)
{ (void)address; (void)type; out[0] = 0; return RECON_DB_UNAVAILABLE; }
recon_db_result_t recon_store_vendor(const uint8_t *address, char *out)
{ (void)address; out[0] = 0; return RECON_DB_UNAVAILABLE; }

static void remember_text(const char *text)
{
    size_t length = strlen(text);
    CHECK(drawn_size + length + 2U < sizeof(drawn));
    memcpy(drawn + drawn_size, text, length); drawn_size += length;
    drawn[drawn_size++] = '\n'; drawn[drawn_size] = 0;
}
static esp_err_t draw_centered(uint16_t y, const char *text, uint16_t color, uint8_t scale)
{
    remember_text(text);
    CHECK(strlen(text) * 6U * scale <= 320U);
    return cerberus_display_draw_text((320U - strlen(text) * 6U * scale) / 2U,
        y, text, color, UI_BACKGROUND, scale);
}
static esp_err_t draw_header(const char *text) { return draw_centered(20, text, UI_TEXT, 2); }
static esp_err_t draw_button(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const char *text)
{
    remember_text(text); CHECK(strlen(text) * 6U <= w);
    cerberus_display_fill_rect(x, y, w, h, UI_PANEL);
    return cerberus_display_draw_text(x + (w - strlen(text) * 6U) / 2U,
        y + (h - 7U) / 2U, text, UI_TEXT, UI_PANEL, 1);
}

/* Classifier decisions are controlled to exercise UI routing; classifier
 * signature correctness is covered by its independent production suites. */
bool sentinel_watch_classify_wifi(const sentinel_watch_wifi_observation_t *row, sentinel_watch_match_t *match)
{
    memset(match, 0, sizeof(*match));
    if (!row->ssid_length || row->ssid[0] != 'H') return false;
    *match = (sentinel_watch_match_t){SENTINEL_WATCH_CAMERA, SENTINEL_WATCH_CONFIDENCE_HIGH,
        "fixture.wifi", "Camera hint", "Synthetic test hint"}; return true;
}
bool sentinel_watch_classify_ble(const sentinel_watch_ble_observation_t *row, sentinel_watch_match_t *match)
{
    memset(match, 0, sizeof(*match));
    if (!row->name_length || (row->name[0] != 'H' && row->name[0] != 'D')) return false;
    *match = (sentinel_watch_match_t){row->name[0] == 'D' ? SENTINEL_WATCH_DRONE : SENTINEL_WATCH_CAMERA,
        SENTINEL_WATCH_CONFIDENCE_HIGH, "fixture.ble", "Bluetooth hint", "Synthetic test hint"}; return true;
}
#include "recon_category_labels.h"

esp_err_t cerberus_s3_ble_scanner_get_advertisement(uint8_t generation, uint8_t index,
    cerberus_c5_ble_advertisement_t *out)
{
    CHECK(!recon_lifecycle_active);
    ++raw_calls; CHECK(generation == live_records[SENTINEL_SCAN_BLE].saved_generation);
    if (raw_error != ESP_OK && index == raw_fail_index) return raw_error;
    *out = evidence[index].advertisement;
    memcpy(out->address, store.ble[index].address, 6); out->address_type = store.ble[index].address_type;
    if (raw_identity_bad) out->address_type ^= 1U;
    return ESP_OK;
}
esp_err_t cerberus_s3_ble_scanner_get_status(cerberus_ble_scan_status_payload_t *out)
{
    CHECK(!recon_lifecycle_active);
    ++status_calls; *out = ble_scan_status;
    if (raw_final_bad) ++out->generation;
    return ESP_OK;
}
static esp_err_t extra_status(uint8_t radio, uint8_t generation, cerberus_watch_scan_status_payload_t *out)
{
    ++extra_calls;
    if (extra_error != ESP_OK) return extra_error;
    *out = (cerberus_watch_scan_status_payload_t){.radio = radio, .generation = generation,
        .state = CERBERUS_WATCH_COMPLETE, .stored_count = extra_drone && radio == CERBERUS_WATCH_RADIO_BLE ? 1U : 0U};
    if (extra_final_bad && extra_calls % 2U == 0U) ++out->generation;
    return ESP_OK;
}
esp_err_t c5_spi_link_get_watch_status(uint8_t radio, uint8_t generation, cerberus_watch_scan_status_payload_t *out)
{ ++c5_calls; return extra_status(radio, generation, out); }
esp_err_t cerberus_s3_ble_scanner_get_watch_status(uint8_t generation, cerberus_watch_scan_status_payload_t *out)
{ CHECK(!recon_lifecycle_active);return extra_status(CERBERUS_WATCH_RADIO_BLE, generation, out); }
esp_err_t c5_spi_link_get_watch_result(uint8_t radio, uint8_t generation, uint8_t index, cerberus_watch_observation_t *out)
{ (void)radio; (void)generation; (void)index; (void)out; ++c5_calls; CHECK(false); return ESP_FAIL; }
esp_err_t cerberus_s3_ble_scanner_get_watch_result(uint8_t generation, uint8_t index, cerberus_watch_observation_t *out)
{
    CHECK(!recon_lifecycle_active);
    CHECK(extra_drone && index == 0 && generation == ble_scan_status.generation);
    *out = (cerberus_watch_observation_t){.kind = SENTINEL_WATCH_PROTOCOL_DRONE,
        .transport = SENTINEL_WATCH_TRANSPORT_BLE, .address_type = store.ble[0].address_type, .rssi = -60};
    memcpy(out->address, store.ble[0].address, 6); strcpy(out->id, "Drone hint"); return ESP_OK;
}
esp_err_t cerberus_s3_ble_scanner_get_activity(uint8_t generation, cerberus_ble_activity_payload_t *out)
{
    CHECK(!recon_lifecycle_active);
    *out = (cerberus_ble_activity_payload_t){.generation = generation, .state = CERBERUS_WATCH_COMPLETE,
        .duration_ms = 8000}; return ESP_OK;
}

/* Recorder adapter definitions follow its production declaration. A value
 * copy verifies the UI never leaves pointers into a mutable scan bank. */
bool cerberus_recon_recorder_offer(const cerberus_recon_record_t *record)
{
    CHECK(record && offer_count < sizeof(offered) / sizeof(offered[0]));
    cerberus_recon_record_t accepted = *record;
    accepted.session_id = 1; accepted.sequence = offer_count + 1U;
    CHECK(cerberus_recon_record_valid(&accepted));
    offered[offer_count++] = *record;
    return !reject_offer;
}

#include "recon_passive_boundary.h"
#ifdef CERBERUS_RECON_LIVE_TEST
#include "recon_live_setup.h"
#else
#include "recon_capture_owner_impl.h"
#endif
#include "sentinel_watch_ui.h"

static bool screen_asleep;
static uint8_t awake_brightness;
static int64_t now_ms, last_user_activity_ms;
static unsigned brightness_reads, brightness_writes;
static esp_err_t brightness_error;
uint8_t cerberus_display_get_brightness(void) { ++brightness_reads; return 64; }
esp_err_t cerberus_display_set_brightness(uint8_t value)
{ CHECK(value == 0); ++brightness_writes; return brightness_error; }
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#include "recon_sleep_impl.h"

static void seed(void)
{
    CHECK(!recon_lifecycle_active);
    memset(&recon_native,0,sizeof(recon_native));
    live_ble_lease_epoch=live_ble_capture_epoch=0U;
    CHECK(live_ble_recon_released());
    memset(&live_ble_lease_state,0,sizeof(live_ble_lease_state));
    memset(&live_ble_capture_state,0,sizeof(live_ble_capture_state));
    live_ble_stop_pending=false;live_ble_lease_error=ESP_OK;watch_seen_ble_epoch=0U;
    watch_inventory_release(); allocation_calls = free_calls = 0; allocation_fail = false;
    memset(&store, 0, sizeof(store)); memset(evidence, 0, sizeof(evidence));
    memset(live_records, 0, sizeof(live_records));
    memset(watch_seen_capture, 0, sizeof(watch_seen_capture));
    memset(watch_last_read, 0, sizeof(watch_last_read));
    memset(watch_capture_waiting, 0, sizeof(watch_capture_waiting));
    memset(watch_capture_failed, 0, sizeof(watch_capture_failed));
    memset(watch_last_count, 0, sizeof(watch_last_count));
    memset(watch_limited_capture, 0, sizeof(watch_limited_capture));
    memset(&watch_extra_status, 0, sizeof(watch_extra_status));
    sentinel_watch_init(&watch_state); sentinel_watch_activity_init(&watch_activity_state);
    sentinel_scan_session_init(&live_session); sentinel_scan_session_init(&live_ble_session);
    live_ble_session_initialized = watch_initialized = true;
    live_store = &store; live_ble_evidence = evidence;
    watch_page = WATCH_LIVE; current_page = UI_PAGE_WATCH; watch_paused = false; watch_radar = false;
    c5_online = true; live_radio_busy = false;
    c5_status = (cerberus_c5_status_t){.firmware_version = CERBERUS_WATCH_MIN_FIRMWARE,
        .capabilities = CERBERUS_C5_CAP_WATCH_OBSERVATIONS | CERBERUS_C5_CAP_WIFI_2_4_GHZ};
    wifi_scan_status = (cerberus_wifi_scan_status_payload_t){0};
    ble_scan_status = (cerberus_ble_scan_status_payload_t){0};
    clock_us = 5000000; clock_script_count = clock_script_index = 0;
    raw_calls = status_calls = c5_calls = extra_calls = notifications = 0;
    raw_error = extra_error = ESP_OK; raw_fail_index = 0;
    raw_identity_bad = raw_final_bad = extra_final_bad = extra_drone = false;
    offer_count = start_calls = stop_calls = 0; start_error = ESP_OK; reject_offer = false;
    drawn_size = 0; drawn[0] = 0; recon_confirm = 0;
    painted_text_size=0;painted_text[0]=0;fail_paint_at=0;
    memset(&recorder_status, 0, sizeof(recorder_status));
    watch_recording_context = false;
    sentinel_watch_view_invalidate();
}

static void bank(sentinel_scan_tool_t tool, uint16_t count)
{
    live_records[tool] = (live_scan_record_t){.valid = true, .captured = true,
        .state = SENTINEL_SCAN_COMPLETE, .generation = 7, .saved_generation = 7,
        .count = count, .captured_ms = 3000};
    sentinel_scan_session_t *session = live_session_for(tool);
    session->desired = tool; session->completed = true;
    if (tool == SENTINEL_SCAN_WIFI) {
        wifi_scan_status = (cerberus_wifi_scan_status_payload_t){.stored_count = count, .total_count = count};
        for (unsigned i = 0; i < count; ++i) {
            strcpy(store.wifi[i].ssid, i ? "ordinary AP" : "Hint AP");
            store.wifi[i].bssid[5] = i + 1U; store.wifi[i].channel = 36; store.wifi[i].rssi = -60;
            store.wifi_observations[i] = (cerberus_s3_wifi_observation_t){.valid = true,
                .request_started_us = 2500000, .source_age_ms = 123};
        }
    } else {
        ble_scan_status = (cerberus_ble_scan_status_payload_t){.state = CERBERUS_BLE_SCAN_COMPLETE,
            .generation = 7, .stored_count = count};
        live_ble_capture_epoch=live_ble_capture_epoch?live_ble_capture_epoch+1U:UINT64_C(0x100000007);
        live_ble_lease_epoch=live_ble_capture_epoch;
        live_ble_lease_state=live_ble_capture_state=(cerberus_s3_ble_passive_state_t){
            .epoch=live_ble_capture_epoch,.scan=ble_scan_status,.scan_epoch=17,.host_epoch=3,.attached=true};
        for (unsigned i = 0; i < count; ++i) {
            strcpy(store.ble[i].name, i ? "ordinary BLE" : "Hint BLE");
            store.ble[i].address[5] = i + 1U; store.ble[i].address_type = i % 4U; store.ble[i].rssi = -55;
            evidence[i].peer = store.ble[i]; evidence[i].generation = 7;
            evidence[i].index=(uint8_t)i;
            memcpy(evidence[i].advertisement.address,store.ble[i].address,6U);
            evidence[i].advertisement.address_type=store.ble[i].address_type;
            cerberus_s3_ble_discovery_evidence_t *source = &evidence[i].focus.discovery;
            memcpy(source->address, store.ble[i].address, 6); source->address_type = store.ble[i].address_type;
            source->generation = 7; source->flags = CERBERUS_S3_BLE_EVIDENCE_ADV_COMPLETE | CERBERUS_S3_BLE_EVIDENCE_ADV_VALID;
            source->advertisement_sequence = i + 1U; source->advertisement_observed_ms = 0; /* Zero is valid source time. */
        }
    }
}

static void test_accepted_banks(void)
{
    for (unsigned radio = 0; radio < 2; ++radio) {
        sentinel_scan_tool_t tool = radio ? SENTINEL_SCAN_BLE : SENTINEL_SCAN_WIFI;
        seed(); bank(tool, 3); cerberus_scan_display_store_t before = store;
        CHECK(watch_consume_radio(tool, 5000)); CHECK(offer_count == 3);
        CHECK(!memcmp(&before, &store, sizeof(store)));
        CHECK(!live_session_for(tool)->completed && !live_session_for(tool)->paused);
        CHECK(offered[0].classified && !offered[1].classified && !offered[2].classified);
        CHECK(offered[0].alert_state == CERBERUS_RECON_ALERT_QUEUED);
        CHECK(offered[1].alert_state == CERBERUS_RECON_ALERT_NOT_QUEUED);
        CHECK(offered[0].session_id == 0 && offered[0].sequence == 0 && offered[0].consumed_us == (uint64_t)clock_us);
        CHECK(!watch_consume_radio(tool, 6000) && offer_count == 3);
        live_session_for(tool)->completed = true; live_records[tool].captured_ms = 6000;
        if(radio){
            CHECK(!watch_consume_radio(tool,6500)); /* Receipt time cannot invent a new S3 bank. */
            live_ble_capture_state.epoch=live_ble_lease_state.epoch=++live_ble_capture_epoch;
            live_ble_lease_epoch=live_ble_capture_epoch;
        }
        CHECK(watch_consume_radio(tool, 7000) && offer_count == 6);
        CHECK(offered[3].alert_state == CERBERUS_RECON_ALERT_NOT_QUEUED);
        CHECK(offered[3].source_time == offered[0].source_time);
        if (radio) {
            CHECK(c5_calls == 0 && raw_calls == 0 && status_calls == 12);
            CHECK(offered[0].source_kind == CERBERUS_RECON_SOURCE_S3_ADV_MS32);
            CHECK(offered[0].source_time == 0 && offered[0].source_sequence == 1);
            CHECK(offered[2].address_type == 2 && !offered[2].channel_valid);
        } else {
            CHECK(c5_calls == 4 && raw_calls == 0 && status_calls == 0);
            CHECK(offered[0].source_kind == CERBERUS_RECON_SOURCE_C5_AGE_MS);
            CHECK(offered[0].source_time == 123 && offered[0].source_anchor_us == 2500000);
            CHECK(offered[0].channel_valid && offered[0].channel == 36);
        }
    }
    seed(); bank(SENTINEL_SCAN_BLE, 1); extra_drone = true; strcpy(store.ble[0].name, "Drone");
    CHECK(watch_consume_radio(SENTINEL_SCAN_BLE, 5000));
    CHECK(offer_count == 1 && offered[0].classified && offered[0].category == SENTINEL_WATCH_DRONE);
    CHECK(offered[0].alert_state == CERBERUS_RECON_ALERT_NOT_EVALUATED && watch_state.history_count == 1);
    seed(); bank(SENTINEL_SCAN_WIFI, 2); reject_offer = true;
    CHECK(watch_consume_radio(SENTINEL_SCAN_WIFI, 5000) && offer_count == 2);
    CHECK(!live_session.completed && watch_seen_capture[0] == 3000);
}

static void test_rejected_banks(void)
{
    for (unsigned fault = 0; fault < 11; ++fault) {
        seed(); bank(SENTINEL_SCAN_BLE, 3);
        switch (fault) {
        case 0: watch_paused = true; break;
        case 1: watch_paused = true; break;
        case 2: live_ble_session.completed = false; break;
        case 3: live_ble_session.active = SENTINEL_SCAN_BLE; break;
        case 4: live_records[SENTINEL_SCAN_BLE].saved_generation++; break;
        case 5: live_records[SENTINEL_SCAN_BLE].valid = false; break;
        case 6: live_ble_capture_state.host_epoch++; break;
        case 7: evidence[1].advertisement.address_type ^= 1U; break;
        case 8: raw_final_bad = true; break;
        case 9: extra_error = ESP_ERR_INVALID_RESPONSE; break;
        case 10: extra_final_bad = true; break;
        }
        CHECK(!watch_consume_radio(SENTINEL_SCAN_BLE, 5000)); CHECK(offer_count == 0);
        CHECK(watch_state.observations == 0);
    }
    seed(); bank(SENTINEL_SCAN_WIFI, 2); c5_online = false;
    CHECK(!watch_consume_radio(SENTINEL_SCAN_WIFI, 5000) && offer_count == 0 && c5_calls == 0);
    seed(); bank(SENTINEL_SCAN_BLE, 0);
    CHECK(watch_consume_radio(SENTINEL_SCAN_BLE, 5000) && offer_count == 0 && !live_ble_session.completed);
}

static void test_unfiltered_feed(void)
{
    for (unsigned policy = 0; policy < 5; ++policy) {
        seed(); bank(SENTINEL_SCAN_WIFI, 2);
        sentinel_watch_entry_t selected = {.radio = SENTINEL_WATCH_RADIO_WIFI};
        memcpy(selected.address, store.wifi[0].bssid, 6);
        if (policy == 0) watch_state.muted = true;
        if (policy == 1) sentinel_watch_set_mode(&watch_state, SENTINEL_WATCH_CAMERA, SENTINEL_WATCH_MODE_OFF);
        if (policy == 2) sentinel_watch_set_mode(&watch_state, SENTINEL_WATCH_CAMERA, SENTINEL_WATCH_MODE_HISTORY);
        if (policy == 3) CHECK(sentinel_watch_set_ignored(&watch_state, &selected, true, 5000));
        if (policy == 4) CHECK(sentinel_watch_snooze(&watch_state, &selected, 60000, 5000));
        CHECK(watch_consume_radio(SENTINEL_SCAN_WIFI, 5000) && offer_count == 2);
        CHECK(offered[0].classified && offered[0].alert_state == CERBERUS_RECON_ALERT_NOT_QUEUED);
        CHECK(!offered[1].classified && offered[1].name_length == strlen("ordinary AP"));
        CHECK(offered[0].mode == (policy == 1 ? SENTINEL_WATCH_MODE_OFF :
            policy == 2 ? SENTINEL_WATCH_MODE_HISTORY : SENTINEL_WATCH_MODE_ALERT));
    }
    seed(); bank(SENTINEL_SCAN_WIFI, CERBERUS_WIFI_SCAN_MAX_RESULTS);
    bank(SENTINEL_SCAN_BLE, CERBERUS_BLE_SCAN_MAX_RESULTS);
    CHECK(watch_consume(5000));
    CHECK(offer_count == CERBERUS_WIFI_SCAN_MAX_RESULTS + CERBERUS_BLE_SCAN_MAX_RESULTS);
    CHECK(!live_session.completed && !live_ble_session.completed);
    CHECK(offered[CERBERUS_WIFI_SCAN_MAX_RESULTS].radio == CERBERUS_RECON_RADIO_BLE);
    seed(); bank(SENTINEL_SCAN_WIFI, 1); bank(SENTINEL_SCAN_BLE, 1);
    live_records[SENTINEL_SCAN_WIFI].valid = false;
    CHECK(watch_consume(5000) && offer_count == 1 && offered[0].radio == CERBERUS_RECON_RADIO_BLE);
    CHECK(watch_capture_failed[0] && !live_ble_session.completed);
}

static void test_source_fences(void)
{
    for (unsigned fault = 0; fault < 9; ++fault) {
        seed(); bank(SENTINEL_SCAN_BLE, 1);
        cerberus_s3_ble_discovery_evidence_t *source = &evidence[0].focus.discovery;
        switch (fault) {
        case 0: live_ble_evidence = NULL; break;
        case 1: evidence[0].generation++; break;
        case 2: evidence[0].peer.address_type++; break;
        case 3: evidence[0].peer.address[0]++; break;
        case 4: source->generation++; break;
        case 5: source->address_type++; break;
        case 6: source->address[0]++; break;
        case 7: source->flags |= CERBERUS_S3_BLE_EVIDENCE_SID_INVALID; break;
        case 8: source->advertisement_sequence = 0; break;
        }
        watch_record_observation(SENTINEL_SCAN_BLE, 0, NULL, CERBERUS_RECON_ALERT_NOT_QUEUED);
        CHECK(offer_count == 1 && offered[0].source_kind == CERBERUS_RECON_SOURCE_NONE);
        CHECK(offered[0].source_flags == 0 && offered[0].source_time == 0 && offered[0].source_sequence == 0);
        CHECK(offered[0].source_limits & CERBERUS_RECON_LIMIT_SOURCE_UNAVAILABLE);
        CHECK(c5_calls == 0 && raw_calls == 0 && status_calls == 0 && extra_calls == 0);
    }
    seed(); bank(SENTINEL_SCAN_BLE, 1);
    evidence[0].focus.ibeacon_valid = true; evidence[0].focus.ibeacon_sequence = 9;
    evidence[0].focus.ibeacon_observed_ms = 999; /* Retained earlier iBeacon cannot refresh current ADV. */
    watch_record_observation(SENTINEL_SCAN_BLE, 0, NULL, CERBERUS_RECON_ALERT_NOT_QUEUED);
    CHECK(!offered[0].ibeacon && offered[0].source_time == 0 && offered[0].source_sequence == 1);
    evidence[0].focus.ibeacon_sequence = 1; evidence[0].focus.ibeacon_observed_ms = 0;
    watch_record_observation(SENTINEL_SCAN_BLE, 0, NULL, CERBERUS_RECON_ALERT_NOT_QUEUED);
    CHECK(offered[1].ibeacon);
    seed(); bank(SENTINEL_SCAN_WIFI, 1); store.wifi_observations[0].valid = false;
    store.wifi[0].channel = 0; store.wifi[0].rssi = -128;
    watch_record_observation(SENTINEL_SCAN_WIFI, 0, NULL, CERBERUS_RECON_ALERT_NOT_QUEUED);
    CHECK(!offered[0].channel_valid && !offered[0].rssi_valid && offered[0].rssi == 0);
    CHECK(offered[0].source_kind == CERBERUS_RECON_SOURCE_NONE);
    seed(); bank(SENTINEL_SCAN_WIFI, 1);
    memset(store.wifi[0].ssid, 'x', 32); store.wifi[0].ssid[0] = '='; store.wifi[0].ssid[1] = '\n';
    store.wifi[0].ssid[2] = '"'; store.wifi[0].ssid[3] = (char)0xFE; store.wifi[0].ssid[32] = 0;
    wifi_scan_status.total_count++;
    watch_record_observation(SENTINEL_SCAN_WIFI, 0, NULL, CERBERUS_RECON_ALERT_NOT_QUEUED);
    CHECK(offered[0].name_length == 32 && !memcmp(offered[0].name, store.wifi[0].ssid, 32));
    CHECK(offered[0].source_limits & CERBERUS_RECON_LIMIT_RETAINED_CAPACITY);
    CHECK(!(offered[0].source_limits & CERBERUS_RECON_LIMIT_NAME_TRUNCATED));
    sentinel_watch_match_t long_rule = {SENTINEL_WATCH_CAMERA, SENTINEL_WATCH_CONFIDENCE_HIGH,
        "abcdefghijklmnopqrstuvwxyz0123456789", "hint", "hint"};
    watch_record_observation(SENTINEL_SCAN_WIFI, 0, &long_rule, CERBERUS_RECON_ALERT_QUEUED);
    CHECK(!offered[1].classified && offered[1].rule[0] == 0 && offered[1].alert_state == CERBERUS_RECON_ALERT_NOT_EVALUATED);
    seed(); bank(SENTINEL_SCAN_BLE, 1); store.ble[0].rssi = 20;
    watch_record_observation(SENTINEL_SCAN_BLE, 0, NULL, CERBERUS_RECON_ALERT_NOT_QUEUED);
    CHECK(offered[0].rssi_valid && offered[0].rssi == 20);
    clock_us = -1;
    watch_record_observation(SENTINEL_SCAN_BLE, 0, NULL, CERBERUS_RECON_ALERT_NOT_QUEUED);
    CHECK(offer_count == 1);
}

static void save_preview(const char *directory, const char *name)
{
    char path[1024]; snprintf(path, sizeof(path), "%s/%s.ppm", directory, name);
    FILE *file = fopen(path, "wb"); CHECK(file); fprintf(file, "P6\n320 480\n255\n");
    for (size_t i = 0; i < sizeof(screen) / sizeof(screen[0]); ++i) {
        uint16_t pixel = screen[i];
        uint8_t rgb[] = {(uint8_t)((pixel >> 11U) * 255U / 31U),
            (uint8_t)(((pixel >> 5U) & 63U) * 255U / 63U), (uint8_t)((pixel & 31U) * 255U / 31U)};
        assert(fwrite(rgb, 1, sizeof(rgb), file) == sizeof(rgb));
    }
    CHECK(fclose(file) == 0);
}

static void test_controls(const char *directory)
{
    seed(); watch_open(5000);
    CHECK(start_calls == 0 && stop_calls == 1 && watch_recording_context);
    CHECK(watch_requested_tool() == SENTINEL_SCAN_WIFI && watch_ble_requested() && watch_keeps_awake());
    watch_page = WATCH_OPTIONS;
    CHECK(recon_draw_options() == ESP_OK);
    CHECK(strstr(drawn, "SD RECORD: OFF") && strstr(drawn, "NO SD SESSION FOLDER"));
    CHECK(strstr(drawn, "CLEAR LIVE HISTORY") && strstr(drawn, "SAVED SD: KEPT"));
    save_preview(directory, "options-off");
    const uint16_t gaps[][2] = {{7,120},{312,120},{20,119},{20,164},{20,169},{20,214},
        {156,190},{163,190},{20,223},{20,268},{20,277},{20,322},{63,440},{256,440},{100,470}};
    unsigned original_stops = stop_calls;
    for (size_t i = 0; i < sizeof(gaps) / sizeof(gaps[0]); ++i)
        CHECK(!recon_handle_control_tap(gaps[i][0], gaps[i][1], 5000));
    CHECK(!start_calls && stop_calls == original_stops);
    CHECK(recon_handle_control_tap(8,120,5000) && start_calls == 1);
    CHECK(recorder_status.state == CERBERUS_RECON_RECORDER_STARTING);
    CHECK(recon_handle_control_tap(311,163,5000) && stop_calls == original_stops + 1);
    CHECK(recorder_status.state == CERBERUS_RECON_RECORDER_STOPPING);
    CHECK(recon_handle_control_tap(20,130,5000) && stop_calls == original_stops + 1 && start_calls == 1);
    recorder_status = (cerberus_recon_recorder_status_t){.state = CERBERUS_RECON_RECORDER_COMPLETE};
    strcpy(recorder_status.session_name,"R01234"); recorder_status.counters.written = 12345;
    drawn_size = 0; drawn[0] = 0;
    CHECK(recon_draw_options() == ESP_OK);
    CHECK(strstr(drawn, "SD RECORD: SAVED") && strstr(drawn, "/CERBERUS/RECON/R01234"));
    CHECK(strstr(drawn, "WRITTEN 12345")); save_preview(directory, "options-saved");
    recorder_status.counters.dropped = 2; recorder_status.counters.invalid = 3; recorder_status.counters.abandoned = 4;
    drawn_size = 0; drawn[0] = 0; CHECK(recon_draw_options() == ESP_OK);
    CHECK(strstr(drawn, "MISSED 9"));
    recorder_status.counters.written = recorder_status.counters.dropped = UINT32_MAX;
    drawn_size = 0; drawn[0] = 0; CHECK(recon_draw_options() == ESP_OK);
    CHECK(strstr(drawn, "WRITTEN 4294967295+ / MISSED 4294967295+"));
    recorder_status.counters = (cerberus_recon_counters_t){.written = 12345};
    for (unsigned state = CERBERUS_RECON_RECORDER_IDLE; state <= CERBERUS_RECON_RECORDER_CANCELED; ++state) {
        recorder_status.state = (cerberus_recon_recorder_state_t)state;
        recorder_status.worker_active = state >= CERBERUS_RECON_RECORDER_STARTING && state <= CERBERUS_RECON_RECORDER_STOPPING;
        recorder_status.error = state == CERBERUS_RECON_RECORDER_ERROR ? ESP_ERR_NO_MEM : ESP_OK;
        drawn_size = 0; drawn[0] = 0;
        CHECK(recon_draw_options() == ESP_OK);
        if (state == CERBERUS_RECON_RECORDER_CANCELED) CHECK(strstr(drawn, "STOPPED / NO FILE") && !strstr(drawn, "SD RECORD: SAVED"));
        if (state == CERBERUS_RECON_RECORDER_ERROR) {
            CHECK(strstr(drawn, "ESP_ERR_NO_MEM") && strstr(drawn,"R01234"));
            save_preview(directory, "options-error");
        }
    }
    recorder_status.worker_active = false; start_error = ESP_ERR_INVALID_STATE;
    CHECK(recon_handle_control_tap(20,130,5000)); CHECK(!strcmp(recon_message,"ESP_ERR_INVALID_STATE"));
    unsigned old_starts = start_calls; watch_paused = true;
    CHECK(recon_handle_control_tap(20,130,5000) && start_calls == old_starts);
    watch_paused = false;
    CHECK(recon_handle_control_tap(8,170,5000) && watch_state.minimum_confidence == SENTINEL_WATCH_CONFIDENCE_HIGH);
    CHECK(recon_handle_control_tap(311,213,5000) && watch_state.muted);
    CHECK(recon_handle_control_tap(8,224,5000) && watch_page == WATCH_FILTERS);
    watch_page = WATCH_OPTIONS;
    sentinel_watch_match_t match = {SENTINEL_WATCH_CAMERA,SENTINEL_WATCH_CONFIDENCE_HIGH,"fixture", "Camera", "Hint"};
    uint8_t address[6] = {1};
    CHECK(!sentinel_watch_observe(&watch_state,SENTINEL_WATCH_RADIO_WIFI,address,0,"Name",4,-60,&match,5000));
    CHECK(watch_state.history_count == 1);
    recorder_status.counters.written = 12345;
    cerberus_recon_recorder_status_t saved_status = recorder_status;
    CHECK(recon_handle_control_tap(8,278,5000) && recon_confirm == 5 && watch_state.history_count == 1);
    CHECK(recon_handle_control_tap(155,321,5000) && recon_confirm == 0 && watch_state.history_count == 0);
    CHECK(!memcmp(&saved_status, &recorder_status, sizeof(saved_status)));
    CHECK(start_calls == old_starts && !raw_calls && !status_calls && !c5_calls && !extra_calls);
    /* Refresh asynchronous save status without a new scan or filesystem call. */
    watch_last_draw = 5000; drawn_size = 0; drawn[0] = 0;
    watch_ui_tick(5499); CHECK(!drawn_size);
    watch_ui_tick(5500); CHECK(drawn_size > 0 && watch_last_draw == 5500);
}

static void active_sessions(void)
{
    live_session.desired = live_session.active = SENTINEL_SCAN_WIFI;
    live_ble_session.desired = live_ble_session.active = SENTINEL_SCAN_BLE;
    recorder_status.state = CERBERUS_RECON_RECORDER_RECORDING; recorder_status.worker_active = true;
}
static void test_lifecycle(void)
{
    seed(); active_sessions(); CHECK(watch_draw(5000,true) == ESP_OK);
    watch_handle_tap(100,440); /* Real live-view Pause hitbox. */
    CHECK(watch_paused && stop_calls == 1 && !watch_requested_tool() && !watch_ble_requested());
    CHECK(live_session.desired == SENTINEL_SCAN_NONE && live_ble_session.desired == SENTINEL_SCAN_NONE);
    CHECK(live_session.active == SENTINEL_SCAN_WIFI && live_ble_session.active == SENTINEL_SCAN_BLE);
    CHECK(!watch_keeps_awake());
    watch_handle_tap(100,440);
    CHECK(!watch_paused && start_calls == 0 && stop_calls == 1);
    CHECK(live_session.desired == SENTINEL_SCAN_WIFI && live_ble_session.desired == SENTINEL_SCAN_BLE);
    seed(); active_sessions(); CHECK(watch_draw(5000,true) == ESP_OK);
    watch_recording_context = true; watch_handle_tap(20,440);
    CHECK(current_page == UI_PAGE_MENU && stop_calls == 1 && !watch_recording_context);
    CHECK(live_session.desired == SENTINEL_SCAN_NONE && live_ble_session.desired == SENTINEL_SCAN_NONE);
    seed(); active_sessions(); watch_page = WATCH_OPTIONS;
    CHECK(recon_handle_control_tap(164,278,5000));
    CHECK(watch_page == WATCH_OPTIONS && watch_radar && stop_calls == 0 && watch_requested_tool() && watch_ble_requested());
    CHECK(live_session.desired == SENTINEL_SCAN_WIFI && live_ble_session.desired == SENTINEL_SCAN_BLE);
    CHECK(watch_draw(5000,true) == ESP_OK); watch_handle_tap(100,440);
    CHECK(watch_page == WATCH_LIVE && start_calls == 0 && stop_calls == 0);
    seed(); active_sessions(); watch_recording_context = true; current_page = UI_PAGE_MENU;
    watch_ui_tick(5000); watch_ui_tick(6000);
    CHECK(stop_calls == 1 && !watch_recording_context && !offer_count);
    CHECK(!raw_calls && !status_calls && !c5_calls && !extra_calls);
    for (unsigned failure = 0; failure < 2; ++failure) {
        seed(); active_sessions();
        screen_asleep = false; awake_brightness = 0;
        now_ms = 9000; last_user_activity_ms = 1234;
        brightness_reads = brightness_writes = 0;
        brightness_error = failure ? ESP_FAIL : ESP_OK;
        recon_test_sleep_attempt();
        CHECK(brightness_reads == 1 && brightness_writes == 1 && awake_brightness == 64);
        CHECK(screen_asleep == !failure && stop_calls == !failure);
        CHECK(last_user_activity_ms == (failure ? now_ms : 1234));
        CHECK(!raw_calls && !status_calls && !c5_calls && !extra_calls && !start_calls && !offer_count);
    }
}

static void test_inventory_integration(void)
{
    seed(); watch_inventory_open(); CHECK(recon_inventory_ui && allocation_calls == 1);
    CHECK(recon_inventory_ui->filter == CERBERUS_RECON_INVENTORY_ALL);
    bank(SENTINEL_SCAN_WIFI, 2); bank(SENTINEL_SCAN_BLE, 2);
    /* The same bytes on different radios and BLE address types are distinct. */
    memcpy(store.ble[0].address, store.wifi[0].bssid, 6);
    memcpy(store.ble[1].address, store.ble[0].address, 6);
    for (unsigned i = 0; i < 2; ++i) {
        evidence[i].peer = store.ble[i];
        memcpy(evidence[i].focus.discovery.address, store.ble[i].address, 6);
        memcpy(evidence[i].advertisement.address,store.ble[i].address,6);
    }
    CHECK(watch_consume(5000));
    CHECK(recon_inventory_ui->model.count[0] == 2 && recon_inventory_ui->model.count[1] == 2);
    CHECK(recon_inventory_ui->model.counters.added == 4 && recon_inventory_error == ESP_OK);
    CHECK(!recon_inventory_ui->model.entries[1].record.classified);
    CHECK(!recon_inventory_ui->model.entries[33].record.classified);
    CHECK(recon_inventory_ui->model.entries[32].record.address_type != recon_inventory_ui->model.entries[33].record.address_type);
    CHECK(recorder_status.state == CERBERUS_RECON_RECORDER_IDLE && start_calls == 0 && offer_count == 4);
    cerberus_recon_inventory_t before = recon_inventory_ui->model;
    CHECK(!watch_consume(6000)); CHECK(!memcmp(&before, &recon_inventory_ui->model, sizeof(before)));

    live_ble_session.completed = true; live_records[SENTINEL_SCAN_BLE].captured_ms = 4000;
    live_ble_capture_state.epoch=live_ble_lease_state.epoch=++live_ble_capture_epoch;
    live_ble_lease_epoch=live_ble_capture_epoch;
    clock_us = 6000000; recon_native.query_error=ESP_ERR_TIMEOUT;
    CHECK(!watch_consume_radio(SENTINEL_SCAN_BLE, 6000));
    CHECK(!memcmp(&before, &recon_inventory_ui->model, sizeof(before)) && offer_count == 4);
    recon_native.query_error=ESP_OK; clock_us = 7000000;
    store.ble[1].address_type = store.ble[0].address_type; /* Duplicate after validated staging poisons whole batch. */
    evidence[1].peer = store.ble[1]; evidence[1].focus.discovery.address_type = store.ble[1].address_type;
    evidence[1].advertisement.address_type=store.ble[1].address_type;
    CHECK(watch_consume_radio(SENTINEL_SCAN_BLE, 7000));
    CHECK(recon_inventory_error == ESP_ERR_INVALID_RESPONSE && !recon_inventory_ui->model.batch_active);
    CHECK(!memcmp(before.entries, recon_inventory_ui->model.entries, sizeof(before.entries)));
    CHECK(recon_inventory_ui->model.counters.added == 4 && recon_inventory_ui->model.count[1] == 2);
    CHECK(offer_count == 6); /* Optional inventory failure does not swallow existing journal offers. */

    watch_page = WATCH_INVENTORY; drawn_size = 0; drawn[0] = 0;
    CHECK(watch_draw(7000, true) == ESP_OK && strstr(drawn,"BLE BATCH:"));
    watch_handle_tap(8,150); CHECK(watch_page == WATCH_INVENTORY_DETAIL);
    CHECK(recon_inventory_ui->selected_valid); watch_handle_tap(100,440);
    CHECK(watch_page == WATCH_INVENTORY); watch_handle_tap(100,440); CHECK(watch_page == WATCH_OPTIONS);
    watch_handle_tap(100,440); CHECK(watch_page == WATCH_LIVE);
    CHECK(start_calls == 0); watch_handle_tap(20,440);
    CHECK(current_page == UI_PAGE_MENU && !recon_inventory_ui && free_calls == 1);
    watch_ui_tick(8000); CHECK(free_calls == 1);

    seed(); watch_inventory_open(); bank(SENTINEL_SCAN_BLE, 3);
    CHECK(watch_consume_radio(SENTINEL_SCAN_BLE, 5000)); before = recon_inventory_ui->model;
    live_ble_session.completed = true; live_records[SENTINEL_SCAN_BLE].captured_ms = 6000;
    live_ble_capture_state.epoch=live_ble_lease_state.epoch=++live_ble_capture_epoch;
    live_ble_lease_epoch=live_ble_capture_epoch;
    clock_us = UINT64_C(130000000); /* A good later end cannot rescue the bad middle timestamp. */
    clock_script[0] = clock_script[1] = clock_script[3] = clock_us;
    clock_script[2] = -1; clock_script_count = 4;
    CHECK(watch_consume_radio(SENTINEL_SCAN_BLE, 130000));
    CHECK(clock_script_index == 4 && !watch_inventory_accepting && !recon_inventory_ui->model.batch_active);
    CHECK(recon_inventory_error == ESP_ERR_INVALID_STATE);
    CHECK(!memcmp(before.entries, recon_inventory_ui->model.entries, sizeof(before.entries)));
    CHECK(!memcmp(before.history, recon_inventory_ui->model.history, sizeof(before.history)));
    CHECK(before.counters.expired == recon_inventory_ui->model.counters.expired);
    CHECK(before.counters.added == recon_inventory_ui->model.counters.added);
    CHECK(offer_count == 5); /* Negative-clock row stays excluded from the independent recorder. */
    clock_script_count = 0; ++clock_us; bank(SENTINEL_SCAN_WIFI, 1);
    store.wifi_observations[0].request_started_us = (uint64_t)clock_us - 1000U;
    CHECK(watch_consume_radio(SENTINEL_SCAN_WIFI, 130001));
    CHECK(recon_inventory_ui->model.count[0] == 1 && recon_inventory_ui->radio_errors[0] == ESP_OK);
    CHECK(recon_inventory_ui->radio_errors[1] == ESP_ERR_INVALID_STATE && recon_inventory_error == ESP_ERR_INVALID_STATE);
    watch_page = WATCH_INVENTORY; drawn_size = 0; drawn[0] = 0;
    CHECK(watch_draw(130001, true) == ESP_OK && strstr(drawn,"BLE BATCH: ESP_ERR_INVALID_STATE"));

    seed(); allocation_fail = true; watch_open(5000);
    CHECK(!recon_inventory_ui && recon_inventory_error == ESP_ERR_NO_MEM && allocation_calls == 1);
    CHECK(watch_requested_tool() == SENTINEL_SCAN_WIFI && watch_ble_requested());
    bank(SENTINEL_SCAN_WIFI, 2); CHECK(watch_consume_radio(SENTINEL_SCAN_WIFI,6000));
    CHECK(offer_count == 2 && !start_calls); watch_page = WATCH_OPTIONS;
    CHECK(recon_handle_control_tap(20,130,6000) && start_calls == 1);
    CHECK(recon_handle_control_tap(8,332,6000) && watch_page == WATCH_INVENTORY);
    drawn_size = 0; drawn[0] = 0; CHECK(watch_draw(6000,true) == ESP_OK && strstr(drawn,"ESP_ERR_NO_MEM"));
    CHECK(recon_inventory_handle_tap(100,440,6000000) && watch_page == WATCH_OPTIONS);

    seed(); watch_open(5000); bank(SENTINEL_SCAN_WIFI,1); CHECK(watch_consume_radio(SENTINEL_SCAN_WIFI,6000));
    recon_inventory_ui->page = 3; recon_inventory_ui->filter = 1; recon_inventory_ui->selected_valid = true;
    clock_us = 7000000; watch_open(7000);
    CHECK(allocation_calls == 1 && free_calls == 0);
    CHECK(recon_inventory_ui->model.count[0] == 0 && recon_inventory_ui->filter == CERBERUS_RECON_INVENTORY_ALL);
    CHECK(!recon_inventory_ui->selected_valid && recon_inventory_ui->paint_valid && !recon_inventory_ui->painted_count && recon_inventory_ui->page == 0);
    current_page = UI_PAGE_MENU; watch_ui_tick(8000); watch_ui_tick(9000);
    CHECK(!recon_inventory_ui && free_calls == 1);
}

static void clear_dashboard_text(void)
{drawn_size=painted_text_size=0;drawn[0]=painted_text[0]=0;calls=0;}
static void test_dashboard(const char *directory)
{
    seed();watch_inventory_open();bank(SENTINEL_SCAN_WIFI,3);bank(SENTINEL_SCAN_BLE,3);
    /* Same bytes on S3 BLE public/random and C5 Wi-Fi stay distinct. */
    memcpy(store.ble[0].address,store.wifi[0].bssid,6);memcpy(store.ble[1].address,store.ble[0].address,6);
    for(unsigned i=0;i<2;++i){evidence[i].peer=store.ble[i];
        memcpy(evidence[i].focus.discovery.address,store.ble[i].address,6);
        memcpy(evidence[i].advertisement.address,store.ble[i].address,6);}
    CHECK(watch_consume(5000));
    CHECK(recon_inventory_ui->model.count[0]==3&&recon_inventory_ui->model.count[1]==3);
    uint32_t added=recon_inventory_ui->model.counters.added;
    live_session.completed=live_ble_session.completed=true;
    live_records[SENTINEL_SCAN_WIFI].captured_ms=6000;
    live_ble_capture_state.epoch=live_ble_lease_state.epoch=++live_ble_capture_epoch;live_ble_lease_epoch=live_ble_capture_epoch;
    clock_us=6000000;CHECK(watch_consume(6000));
    CHECK(recon_inventory_ui->model.count[0]==3&&recon_inventory_ui->model.count[1]==3&&recon_inventory_ui->model.counters.added==added);
    /* The dashboard receives the last32 chronological buckets, not the
     * oldest32 prefix returned by a capacity-limited public copy. */
    for(unsigned i=0;i<64;++i)recon_inventory_ui->model.history[i]=(cerberus_recon_inventory_bucket_t){.second=i,.added=i,.expired=64-i};
    recon_inventory_ui->model.history_count=64;recon_inventory_ui->model.history_head=63;
    recorder_status.state=CERBERUS_RECON_RECORDER_RECORDING;recorder_status.worker_active=true;recorder_status.counters.written=128;
    unsigned getters=status_calls,starts=start_calls,stops=stop_calls,offers=(unsigned)offer_count;
    clear_dashboard_text();CHECK(watch_draw(6000,true)==ESP_OK);
    CHECK(watch_view.dashboard&&watch_view.dashboard->total==6&&watch_view.dashboard->pages==2&&watch_view.dashboard->count==4);
    CHECK(watch_view.dashboard->added[0]==32&&watch_view.dashboard->added[31]==63&&watch_view.dashboard->expired[31]==1);
    CHECK(strstr(painted_text,"DASHBOARD")&&strstr(painted_text,"RADAR")&&strstr(painted_text,"ALL LIST CHANGES / LAST 32 SECONDS"));
    CHECK(strstr(painted_text,"SD: RECORDING / 128 ROWS")&&strstr(painted_text,"C5 / WI-FI")&&strstr(painted_text,"S3 / BLUETOOTH LE"));
    CHECK(!strstr(painted_text,"SCENE GALLERY")&&!strstr(painted_text,"DEMO ART"));
    CHECK(recon_inventory_ui->paint_valid&&status_calls==getters&&start_calls==starts&&stop_calls==stops&&offer_count==offers);
    save_preview(directory,"dashboard-mixed");
    for(unsigned i=0;i<recon_inventory_ui->painted_count;++i)for(unsigned j=i+1;j<recon_inventory_ui->painted_count;++j){
        const cerberus_recon_record_t *a=&recon_inventory_ui->painted[i].record,*b=&recon_inventory_ui->painted[j].record;
        CHECK(a->radio!=b->radio||a->address_type!=b->address_type||memcmp(a->address,b->address,6));
    }
    watch_handle_tap(270,388);CHECK(recon_inventory_ui->dashboard_page==1&&recon_inventory_ui->painted_count==2);
    CHECK(strstr(recon_inventory_ui->dashboard.rows[0].identity,"T1")||strstr(recon_inventory_ui->dashboard.rows[1].identity,"T1"));
    cerberus_recon_inventory_entry_t frozen=recon_inventory_ui->painted[0];
    memset(recon_inventory_ui->model.entries,0,sizeof(recon_inventory_ui->model.entries));
    watch_handle_tap(24,184);CHECK(watch_page==WATCH_INVENTORY_DETAIL&&recon_inventory_ui->detail_from_dashboard);
    CHECK(!memcmp(&frozen,&recon_inventory_ui->selected,sizeof(frozen)));
    watch_handle_tap(100,440);CHECK(watch_page==WATCH_LIVE&&recon_inventory_ui->dashboard_page==0);
    /* Restore a real source publication for failed-paint and Radar tests. */
    seed();watch_inventory_open();bank(SENTINEL_SCAN_WIFI,2);bank(SENTINEL_SCAN_BLE,2);CHECK(watch_consume(5000));
    clear_dashboard_text();CHECK(watch_draw(5000,true)==ESP_OK);unsigned painted_calls=calls;
    for(unsigned fault=1;fault<=painted_calls;fault+=7){
        clear_dashboard_text();fail_paint_at=fault;CHECK(watch_draw(5000,true)==ESP_FAIL);
        CHECK(!recon_inventory_ui->paint_valid&&!watch_dashboard_tap(SENTINEL_WATCH_ACTION_ROW_0));
        fail_paint_at=0;
    }
    clear_dashboard_text();CHECK(watch_draw(5000,true)==ESP_OK&&recon_inventory_ui->paint_valid);
    watch_page=WATCH_OPTIONS;starts=start_calls;stops=stop_calls;
    CHECK(recon_handle_control_tap(164,278,5000)&&watch_radar&&watch_page==WATCH_OPTIONS);
    CHECK(start_calls==starts&&stop_calls==stops);watch_handle_tap(100,440);
    CHECK(watch_page==WATCH_LIVE&&watch_view.radar);
    CHECK(strstr(painted_text,"POSITIONS ARE LIST SLOTS")&&strstr(painted_text,"NO BEARING OR DISTANCE"));
    save_preview(directory,"radar-mixed");
    frozen=recon_inventory_ui->painted[2];watch_handle_tap(24,342);
    CHECK(watch_page==WATCH_INVENTORY_DETAIL&&!memcmp(&frozen,&recon_inventory_ui->selected,sizeof(frozen)));
    watch_handle_tap(100,440);CHECK(watch_page==WATCH_LIVE&&watch_radar);
    watch_last_draw=5000;clear_dashboard_text();watch_ui_tick(5499);CHECK(!calls);
    watch_ui_tick(5500);CHECK(calls&&watch_last_draw==5500);
    seed();watch_inventory_open();bank(SENTINEL_SCAN_WIFI,32);bank(SENTINEL_SCAN_BLE,32);CHECK(watch_consume(5000));
    CHECK(recon_inventory_ui->model.count[0]==32&&recon_inventory_ui->model.count[1]==32);
    recon_inventory_ui->model.entries[0].record.channel_valid=false;
    recon_inventory_ui->model.entries[0].record.channel=0;
    recon_inventory_ui->model.entries[32].age_source_kind=CERBERUS_RECON_SOURCE_S3_CALLBACK_US;
    clear_dashboard_text();CHECK(watch_draw(5000,true)==ESP_OK);
    CHECK(strstr(painted_text,"CH?")&&strstr(painted_text,"CAMERA / HIGH")&&!strstr(painted_text,"GOPRO"));
    static cerberus_recon_record_t displayed[64];unsigned used=0;bool saw_callback_age=false;
    for(unsigned page=0;page<16;++page){
        CHECK(recon_inventory_ui->dashboard_page==page&&watch_view.dashboard->total==64&&watch_view.dashboard->pages==16);
        CHECK(recon_inventory_ui->paint_valid&&recon_inventory_ui->painted_count==4);
        for(unsigned row=0;row<4;++row){
            const cerberus_recon_record_t *r=&recon_inventory_ui->painted[row].record;
            for(unsigned previous=0;previous<used;++previous)
                CHECK(r->radio!=displayed[previous].radio||r->address_type!=displayed[previous].address_type||memcmp(r->address,displayed[previous].address,6));
            displayed[used++]=*r;
        }
        for(unsigned row=0;row<recon_inventory_ui->painted_count;++row)
            if(strstr(recon_inventory_ui->dashboard.rows[row].age,"RX "))saw_callback_age=true;
        clear_dashboard_text();watch_handle_tap(270,388);
    }
    CHECK(used==64&&recon_inventory_ui->dashboard_page==15&&saw_callback_age);
    save_preview(directory,"dashboard-full64-final-page");
    watch_page=WATCH_OPTIONS;CHECK(recon_handle_control_tap(164,278,5000)&&watch_radar);watch_handle_tap(100,440);
    clear_dashboard_text();CHECK(watch_draw(5000,true)==ESP_OK);painted_calls=calls;
    const unsigned radar_faults[]={1,40,painted_calls};
    for(unsigned i=0;i<sizeof(radar_faults)/sizeof(radar_faults[0]);++i){
        clear_dashboard_text();fail_paint_at=radar_faults[i];CHECK(watch_draw(5000,true)==ESP_FAIL);
        CHECK(!recon_inventory_ui->paint_valid&&!watch_dashboard_tap(SENTINEL_WATCH_ACTION_ROW_0));fail_paint_at=0;
    }
    /* No inventory allocation still has useful source/SD status; changing a
     * written count without changing the phase must repaint at the next tick. */
    seed();allocation_fail=true;watch_open(5000);CHECK(!recon_inventory_ui);
    recorder_status.state=CERBERUS_RECON_RECORDER_RECORDING;recorder_status.worker_active=true;
    recorder_status.counters.written=1;clear_dashboard_text();CHECK(watch_draw(5000,true)==ESP_OK);
    CHECK(strstr(painted_text,"INVENTORY UNAVAILABLE")&&strstr(painted_text,"SD: RECORDING / 1 ROWS"));
    recorder_status.counters.written=2;clear_dashboard_text();CHECK(watch_draw(5500,false)==ESP_OK);
    CHECK(strstr(painted_text,"SD: RECORDING / 2 ROWS"));save_preview(directory,"dashboard-no-memory");
    watch_capture_failed[0]=true;live_ble_lease_error=ESP_ERR_TIMEOUT;
    watch_limited_capture[0]=true;watch_extra_unavailable[1]=true;
    clear_dashboard_text();CHECK(watch_draw(6000,false)==ESP_OK);
    CHECK(!strcmp(watch_view.wifi_status,"ERROR / CAPPED")&&!strcmp(watch_view.ble_status,"ERROR / LIMITED"));
    CHECK(strstr(painted_text,"ERROR / CAPPED")&&strstr(painted_text,"ERROR / LIMITED"));
    save_preview(directory,"dashboard-limited-no-memory");
    watch_handle_tap(250,440);CHECK(watch_page==WATCH_OPTIONS);
    CHECK(recon_handle_control_tap(20,130,5500)&&stop_calls==2);
    watch_handle_tap(100,440);watch_handle_tap(100,440);CHECK(watch_paused);
    clear_dashboard_text();CHECK(watch_draw(5500,true)==ESP_OK);save_preview(directory,"dashboard-paused-saving");
    watch_handle_tap(20,440);CHECK(current_page==UI_PAGE_MENU);
}

static void test_dashboard_priority(const char *directory)
{
    seed();watch_inventory_open();bank(SENTINEL_SCAN_WIFI,32);bank(SENTINEL_SCAN_BLE,32);CHECK(watch_consume(5000));
    /* Existing committed classifier results are the only input to presentation.
     * This fixture supplies controlled categories, not additional signatures. */
    static const uint8_t slots[]={42,5,47,0,32,31};
    static const uint8_t categories[]={SENTINEL_WATCH_FINDMY,SENTINEL_WATCH_FLOCK,SENTINEL_WATCH_AXON,
        SENTINEL_WATCH_CAMERA,SENTINEL_WATCH_CAMERA,SENTINEL_WATCH_TRACKER};
    for(unsigned i=0;i<6;++i){
        cerberus_recon_inventory_entry_t *e=&recon_inventory_ui->model.entries[slots[i]];
        e->record.classified=true;e->record.category=categories[i];
        e->record.confidence=i<4?SENTINEL_WATCH_CONFIDENCE_HIGH:i==4?SENTINEL_WATCH_CONFIDENCE_MEDIUM:SENTINEL_WATCH_CONFIDENCE_LOW;
        snprintf(e->record.rule,sizeof(e->record.rule),"fixture.category.%u",i);
        e->first_consumed_us=UINT64_C(4800000)-i*100000U;
    }
    /* Ordinary rows are much more recently refreshed, but do not displace a
     * recognized hint. Graph/counters retain every ordinary observation. */
    for(unsigned i=0;i<64;++i)recon_inventory_ui->model.entries[i].last_consumed_us=UINT64_C(5000000)+i;
    static cerberus_recon_inventory_t unchanged;
    unchanged=recon_inventory_ui->model;
    unsigned before_starts=start_calls,before_stops=stop_calls,before_getters=status_calls,before_offers=(unsigned)offer_count;
    clear_dashboard_text();CHECK(watch_draw(6000,true)==ESP_OK);
    CHECK(!memcmp(&unchanged,&recon_inventory_ui->model,sizeof(unchanged)));
    CHECK(watch_view.dashboard->total==64&&watch_view.dashboard->pages==16);
    for(unsigned i=0;i<4;++i)CHECK(!memcmp(&recon_inventory_ui->painted[i],&unchanged.entries[slots[i]],sizeof(recon_inventory_ui->painted[i])));
    CHECK(strstr(painted_text,"FIND MY / HIGH")&&strstr(painted_text,"FLOCK / HIGH")&&strstr(painted_text,"AXON / HIGH")&&strstr(painted_text,"CAMERA / HIGH"));
    CHECK(strstr(painted_text,"HINTS FIRST / TAP A ROW FOR DETAILS"));save_preview(directory,"dashboard-priority");
    cerberus_recon_record_t order[4];for(unsigned i=0;i<4;++i)order[i]=recon_inventory_ui->painted[i].record;
    for(unsigned i=0;i<64;++i)recon_inventory_ui->model.entries[i].last_consumed_us=UINT64_C(5999000)+(63U-i);
    clear_dashboard_text();CHECK(watch_draw(6000,true)==ESP_OK);
    for(unsigned i=0;i<4;++i)CHECK(!memcmp(&order[i],&recon_inventory_ui->painted[i].record,sizeof(order[i])));
    CHECK(start_calls==before_starts&&stop_calls==before_stops&&status_calls==before_getters&&offer_count==before_offers);

    /* View tabs are visible, idempotent, bounded and do not alter radio/SD
     * ownership, page number, model, or the frozen selection destination. */
    CHECK(sentinel_watch_view_hit_test(&watch_view,100,4)==SENTINEL_WATCH_ACTION_DASHBOARD);
    CHECK(sentinel_watch_view_hit_test(&watch_view,223,41)==SENTINEL_WATCH_ACTION_DASHBOARD);
    CHECK(sentinel_watch_view_hit_test(&watch_view,232,4)==SENTINEL_WATCH_ACTION_RADAR);
    CHECK(sentinel_watch_view_hit_test(&watch_view,311,41)==SENTINEL_WATCH_ACTION_RADAR);
    CHECK(sentinel_watch_view_hit_test(&watch_view,224,20)==SENTINEL_WATCH_ACTION_NONE);
    CHECK(sentinel_watch_view_hit_test(&watch_view,231,20)==SENTINEL_WATCH_ACTION_NONE);
    CHECK(sentinel_watch_view_hit_test(&watch_view,250,42)==SENTINEL_WATCH_ACTION_NONE);
    unchanged=recon_inventory_ui->model;
    watch_handle_tap(270,20);CHECK(watch_radar&&watch_page==WATCH_LIVE&&recon_inventory_ui->dashboard_page==0);
    CHECK(!memcmp(&unchanged,&recon_inventory_ui->model,sizeof(unchanged)));
    CHECK(strstr(painted_text,"RADAR")&&strstr(painted_text,"DASHBOARD")&&strstr(painted_text,"NO BEARING OR DISTANCE"));
    save_preview(directory,"radar-priority");watch_handle_tap(270,20);CHECK(watch_radar);
    watch_handle_tap(140,20);CHECK(!watch_radar);watch_handle_tap(140,20);CHECK(!watch_radar);
    /* The compact title fallback keeps a long category, large age and full
     * signed signal readable; no truncation is used to make the row fit. */
    strcpy(recon_inventory_ui->dashboard.rows[0].hint,"BLE ACTIVITY / HIGH");
    strcpy(recon_inventory_ui->dashboard.rows[0].age,"COPY 999999s");
    clear_dashboard_text();CHECK(sentinel_watch_view_draw(&watch_view,6000)==ESP_OK);
    CHECK(strstr(painted_text,"BLE ACTIVITY / HIGH")&&strstr(painted_text,"COPY 999999s"));
    CHECK(watch_draw(6000,true)==ESP_OK);
    watch_handle_tap(270,388);CHECK(recon_inventory_ui->dashboard_page==1);
    CHECK(recon_inventory_ui->painted[0].record.category==SENTINEL_WATCH_CAMERA&&recon_inventory_ui->painted[0].record.confidence==SENTINEL_WATCH_CONFIDENCE_MEDIUM);
    CHECK(recon_inventory_ui->painted[1].record.category==SENTINEL_WATCH_TRACKER&&!recon_inventory_ui->painted[2].record.classified);
    watch_handle_tap(270,388);CHECK(recon_inventory_ui->dashboard_page==2);
    for(unsigned i=0;i<4;++i)CHECK(!recon_inventory_ui->painted[i].record.classified);
    CHECK(strstr(painted_text,"OTHER OBSERVATION"));save_preview(directory,"dashboard-ordinary-page");
    watch_handle_tap(270,20);CHECK(watch_radar&&recon_inventory_ui->dashboard_page==2);
    watch_handle_tap(140,20);CHECK(!watch_radar&&recon_inventory_ui->dashboard_page==2);
    CHECK(start_calls==before_starts&&stop_calls==before_stops&&status_calls==before_getters&&offer_count==before_offers);
    cerberus_recon_inventory_entry_t frozen=recon_inventory_ui->painted[0];
    memset(&recon_inventory_ui->model.entries[1],0,sizeof(recon_inventory_ui->model.entries[1]));
    watch_handle_tap(24,184);CHECK(watch_page==WATCH_INVENTORY_DETAIL&&!memcmp(&frozen,&recon_inventory_ui->selected,sizeof(frozen)));
    watch_handle_tap(100,440);CHECK(watch_page==WATCH_LIVE&&recon_inventory_ui->dashboard_page==2);

    /* Category preference names change only presentation; OFF/HISTORY/ALERT
     * values and their existing cycle retain the same behavior. */
    watch_page=WATCH_FILTERS;watch_filters_page=0;clear_dashboard_text();CHECK(watch_draw(6000,true)==ESP_OK);
    CHECK(strstr(painted_text,"ALERT")&&!strstr(painted_text,"ANIMATED"));save_preview(directory,"categories-alert");
    sentinel_watch_category_t category=watch_categories[0];
    sentinel_watch_mode_t mode=sentinel_watch_get_mode(&watch_state,category);
    watch_handle_tap(24,120);CHECK(sentinel_watch_get_mode(&watch_state,category)==(sentinel_watch_mode_t)((mode+1U)%3U));
    watch_handle_tap(24,120);watch_handle_tap(24,120);CHECK(sentinel_watch_get_mode(&watch_state,category)==mode);
    CHECK(start_calls==before_starts&&stop_calls==before_stops&&status_calls==before_getters&&offer_count==before_offers);
}

static void test_meta_projection_ui(const char *directory)
{
    /* Real pure projection over the exact frozen native boundary. Actual
     * producer/parser combinations have their own snapshot/helper fixture. */
    for(unsigned variant=0;variant<11;++variant){
        seed();watch_inventory_open();bank(SENTINEL_SCAN_BLE,2);
        strcpy(store.ble[0].name,"ordinary BLE");evidence[0].peer=store.ble[0];
        cerberus_findmy_monitor_record_t *f=&evidence[0];
        cerberus_s3_ble_discovery_evidence_t *source=&f->focus.discovery;
        f->focus.category_bits=CERBERUS_S3_BLE_FOCUS_META_HINT;
        source->flags|=CERBERUS_S3_BLE_EVIDENCE_RAW_COMPLETE;
        source->raw_advertisement_sequence=source->advertisement_sequence;
        CHECK(cerberus_ble_meta_evidence_add(&source->meta_advertisement,0xfd5f,CERBERUS_BLE_META_LIST16));
        f->advertisement.length=4;memcpy(f->advertisement.data,(uint8_t[]){3,3,0x5f,0xfd},4);
        if(variant==1)++source->raw_advertisement_sequence;
        if(variant==2){source->meta_scan_response=source->meta_advertisement;memset(&source->meta_advertisement,0,sizeof(source->meta_advertisement));}
        if(variant==3)f->focus.meta_suppressed=true;
        if(variant==4)CHECK(cerberus_ble_meta_evidence_add(&source->meta_advertisement,0x004c,CERBERUS_BLE_META_MFG16));
        if(variant==5)++f->index;
        if(variant==6)live_ble_lease_epoch+=UINT64_C(0x100000000);
        if(variant==7)strcpy(store.ble[0].name,"Hint BLE");
        if(variant==8)++f->observed_ms;
        if(variant==9)source->address_type^=1;
        if(variant==10)extra_final_bad=true;
        unsigned starts=start_calls,stops=stop_calls;
        bool consumed=watch_consume_radio(SENTINEL_SCAN_BLE,5000);
        bool fence_failure=variant==5||variant==6||variant==10;
        CHECK(consumed!=fence_failure);
        if(fence_failure){CHECK(offer_count==0&&recon_inventory_ui->model.count[1]==0);continue;}
        CHECK(offer_count==2&&start_calls==starts&&stop_calls==stops);
        if(variant==0){
            CHECK(offered[0].classified&&offered[0].category==SENTINEL_WATCH_SMART_GLASSES);
            CHECK(offered[0].confidence==SENTINEL_WATCH_CONFIDENCE_LOW&&!strcmp(offered[0].rule,"meta.original-id-hint"));
            CHECK(offered[0].alert_state==CERBERUS_RECON_ALERT_NOT_QUEUED&&notifications==0);
            clear_dashboard_text();CHECK(watch_draw(5000,true)==ESP_OK);
            CHECK(strstr(painted_text,"META HINT / LOW")&&!strstr(painted_text,"DEMO"));save_preview(directory,"dashboard-meta-hint");
            CHECK(recon_inventory_ui->painted[0].record.category==SENTINEL_WATCH_SMART_GLASSES);
            memset(&evidence[0],0,sizeof(evidence[0]));watch_handle_tap(24,184);
            CHECK(watch_page==WATCH_INVENTORY_DETAIL&&recon_inventory_ui->selected.record.category==SENTINEL_WATCH_SMART_GLASSES);
            CHECK(!strcmp(recon_inventory_ui->selected.record.rule,"meta.original-id-hint"));
            const sentinel_watch_entry_t *entry=sentinel_watch_history(&watch_state,0);CHECK(entry&&entry->match.category==SENTINEL_WATCH_SMART_GLASSES);
            watch_select_detail(entry);watch_page=WATCH_WHY;clear_dashboard_text();CHECK(watch_draw(5000,true)==ESP_OK);
            CHECK(strstr(painted_text,"Glasses, model and recording unknown.")&&!strstr(painted_text,"art preview"));save_preview(directory,"meta-hint-why");
            watch_page=WATCH_FILTERS;watch_filters_page=1;clear_dashboard_text();CHECK(watch_draw(5000,true)==ESP_OK);
            CHECK(strstr(painted_text,"Meta identifier hint")&&!strstr(painted_text,"demo"));save_preview(directory,"categories-meta-alert");
            CHECK(watch_categories[6]==SENTINEL_WATCH_SMART_GLASSES);
            sentinel_watch_mode_t mode=sentinel_watch_get_mode(&watch_state,SENTINEL_WATCH_SMART_GLASSES);
            watch_handle_tap(24,245);CHECK(sentinel_watch_get_mode(&watch_state,SENTINEL_WATCH_SMART_GLASSES)==(sentinel_watch_mode_t)((mode+1U)%3U));
        }else if(variant==7){CHECK(offered[0].category==SENTINEL_WATCH_CAMERA&&offered[0].confidence==SENTINEL_WATCH_CONFIDENCE_HIGH);}
        else CHECK(!offered[0].classified);
    }
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    test_accepted_banks(); test_rejected_banks(); test_unfiltered_feed(); test_source_fences();
    test_controls(argv[1]); test_lifecycle(); test_inventory_integration(); test_dashboard(argv[1]);test_dashboard_priority(argv[1]);test_meta_projection_ui(argv[1]);watch_inventory_release();
    printf("PASS: %u actual Recon UI/journal adapter assertions.\n", checks);
    return 0;
}
