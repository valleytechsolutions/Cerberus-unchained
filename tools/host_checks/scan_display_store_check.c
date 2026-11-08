#ifdef NDEBUG
#undef NDEBUG
#endif
#include "scan_display_store.h"
#include "s3_findmy_monitor.h"
#include "s3_ble_scanner.h"
#include "sentinel_scan_session.h"
#include "wifi_readout_diagnostics.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scan_display_record_type.h"

enum { UI_PAGE_WIFI, UI_PAGE_BLE, UI_PAGE_IEEE802154, UI_PAGE_WATCH, UI_PAGE_FIELD_MENU };
enum { UI_WIFI_MODE_LIST, UI_WIFI_MODE_BASELINE, UI_WIFI_MODE_STATS,
    UI_WIFI_MODE_DETAIL, UI_WIFI_MODE_TARGET, UI_WIFI_MODE_PASSWORD,
    UI_WIFI_MODE_CONNECTION, UI_WIFI_MODE_STATIONS, UI_WIFI_MODE_OBSERVED_CLIENTS,
    UI_WIFI_MODE_AP_JOB, UI_WIFI_MODE_DEFENSE };
enum { UI_IEEE802154_MODE_ASSESSMENT, UI_IEEE802154_MODE_ENERGY,
    UI_IEEE802154_MODE_BEACONS, UI_IEEE802154_MODE_ACTIVITY };
enum { UI_BLE_MODE_MENU, UI_BLE_MODE_LIST };
enum { MALLOC_CAP_SPIRAM = 1U, MALLOC_CAP_8BIT = 2U };
static unsigned current_page, wifi_mode, ble_mode;
static live_scan_record_t live_records[SENTINEL_SCAN_TOOL_COUNT];
static uint32_t live_capture_pending;
static sentinel_scan_session_t live_session;
static sentinel_scan_session_t live_ble_session;
static void live_wifi_capture_reset(void);
static void live_wifi_capture_reset_as(cerberus_wifi_readout_cancel_reason_t reason);
static esp_err_t live_wifi_capture_error(void);
static bool live_status_changed;
static bool mesh_test_busy;
static bool mesh_ui_busy(void){return mesh_test_busy;}
static bool wifi_packet_count_ui_busy(void){return false;}
static bool channel_analyzer_ui_busy(void){return false;}
static bool wifi_fox_hunt_ui_busy(void){return false;}
static bool saved_ble_lookup_ui_busy(void){return false;}
static bool live_supported(sentinel_scan_tool_t tool){return tool!=SENTINEL_SCAN_NONE;}
static uint16_t live_page[SENTINEL_SCAN_TOOL_COUNT];
static cerberus_scan_display_store_t *live_store;
static cerberus_findmy_monitor_record_t *live_ble_evidence;
/* Legacy snapshot capture has no attached live-discovery owner in this suite. */
static struct { struct { uint64_t epoch; } session; } *ble_discovery_live;
static bool live_ble_order_dirty;
static int64_t live_ble_published_ms, live_wifi_published_us, clock_ms;
static int64_t clock_extra_us, clock_advance_us, clock_io_us, clock_allocation_us, clock_commit_us;
static int64_t clock_last_us;
static int64_t clock_script[16];
static unsigned clock_script_count, clock_script_next;
static bool inspect_allocation_readout;
static int64_t clock_peek(void) { return clock_ms * 1000LL + clock_extra_us; }
static int64_t esp_timer_get_time(void)
{
    if (clock_script_next < clock_script_count) {
        clock_last_us = clock_script[clock_script_next++];
        return clock_last_us;
    }
    clock_last_us = clock_peek(); clock_extra_us += clock_advance_us;
    return clock_last_us;
}
static cerberus_wifi_readout_diagnostics_t readout(void)
{
    cerberus_wifi_readout_diagnostics_t snapshot;
    cerberus_wifi_readout_diagnostics_get(&snapshot);
    return snapshot;
}
static void assert_readout_unchanged(cerberus_wifi_readout_diagnostics_t before)
{
    cerberus_wifi_readout_diagnostics_t after = readout();
    assert(after.sampled_us == before.sampled_us && after.elapsed_us == before.elapsed_us &&
        after.max_step_us == before.max_step_us && after.max_tick_gap_us == before.max_tick_gap_us &&
        after.attempts == before.attempts && after.completed == before.completed &&
        after.failed == before.failed && after.cancelled == before.cancelled &&
        after.step_calls == before.step_calls && after.error == before.error &&
        after.rows_total == before.rows_total && after.rows_copied == before.rows_copied &&
        after.state == before.state && after.phase == before.phase &&
        after.cancel_reason == before.cancel_reason && after.timing_valid == before.timing_valid &&
        after.counters_saturated == before.counters_saturated);
}
static uint8_t live_ble_focus_bits[CERBERUS_BLE_SCAN_MAX_RESULTS];
static uint8_t live_activity[16][CERBERUS_IEEE802154_ACTIVITY_FRAME_TYPE_COUNT];
static uint8_t ieee802154_activity_counts[16][CERBERUS_IEEE802154_ACTIVITY_FRAME_TYPE_COUNT];
static int8_t live_energy[16];
static uint16_t wifi_defense_loaded_count;
static cerberus_ieee802154_energy_scan_result_payload_t ieee802154_scan_result;
static cerberus_wifi_scan_status_payload_t wifi_scan_status, wifi_end;
static cerberus_ble_scan_status_payload_t ble_scan_status, ble_end;
static cerberus_s3_ble_passive_state_t passive_source;
static uint64_t passive_next_epoch;
static unsigned passive_fence_fault;
static bool passive_fault_at_row;
static bool ble_scan_checked;
static esp_err_t ble_scan_error;
static cerberus_ieee802154_beacon_scan_status_payload_t ieee802154_beacon_status, beacon_end;

static struct { void *pointer; size_t bytes; bool live; } allocations[128];
static size_t allocation_calls, live_allocations, freed_allocations, fail_allocation_call;
static unsigned result_calls, end_status_calls, refresh_calls, physical_attempts;
static bool row_stalled, fence_stalled, row_copy_only;
enum io_cancel_kind { IO_CANCEL_NONE, IO_CANCEL_STOP, IO_CANCEL_ARM,
    IO_CANCEL_INVALID, IO_CANCEL_STATE, IO_CANCEL_GENERATION };
static enum io_cancel_kind io_cancel;
static unsigned io_cancel_at;
static void physical_attempt(void)
{
    ++physical_attempts;
    clock_extra_us += clock_io_us;
    if (physical_attempts != io_cancel_at) return;
    if (io_cancel == IO_CANCEL_STOP) live_session.paused = true;
    if (io_cancel == IO_CANCEL_ARM) ++live_session.arm_generation;
    if (io_cancel == IO_CANCEL_INVALID) live_records[SENTINEL_SCAN_WIFI].valid = false;
    if (io_cancel == IO_CANCEL_STATE) live_records[SENTINEL_SCAN_WIFI].state = SENTINEL_SCAN_RUNNING;
    if (io_cancel == IO_CANCEL_GENERATION) ++live_records[SENTINEL_SCAN_WIFI].generation;
}
static unsigned wifi_result_calls, wifi_end_calls;
static unsigned relation_before_calls, relation_after_calls, relation_publications;
static unsigned relation_result_start, relation_status_start;
static int64_t publish_expected_terminal_us, failure_expected_terminal_us;
static cerberus_c5_wifi_relation_bank_t relation_proof, published_relation_proof;
static esp_err_t relation_before_error, relation_after_error, published_relation_error;
static uint8_t published_relation_generation;
static cerberus_c5_wifi_result_t wifi_source[CERBERUS_WIFI_SCAN_MAX_RESULTS];
static cerberus_s3_wifi_observation_t wifi_timing_source[CERBERUS_WIFI_SCAN_MAX_RESULTS];
static cerberus_c5_ble_result_t ble_source[CERBERUS_BLE_SCAN_MAX_RESULTS];
static cerberus_s3_ble_focus_t focus_source[CERBERUS_BLE_SCAN_MAX_RESULTS];
static cerberus_c5_ble_advertisement_t advertisement_source[CERBERUS_BLE_SCAN_MAX_RESULTS];
static uint32_t observation_source[CERBERUS_BLE_SCAN_MAX_RESULTS];
static cerberus_ieee802154_beacon_scan_result_payload_t beacon_source[CERBERUS_IEEE802154_BEACON_MAX_RESULTS];
enum failure_point {
    FAIL_NONE, FAIL_WIFI_RESULT, FAIL_WIFI_END, FAIL_BLE_RESULT, FAIL_BLE_FOCUS,
    FAIL_BLE_ADV, FAIL_BLE_TIME, FAIL_BLE_END, FAIL_BEACON_RESULT, FAIL_BEACON_END,
};
static enum failure_point failure;
static uint8_t failure_index = 1U;

/* Actual capture code calls these adapter boundaries. The dedicated relation
 * UI suite exercises their SPI implementation; here verify AP commit ordering,
 * full-width proof forwarding and failure isolation from the old AP bank. */
static bool relation_supported = true;
static bool wifi_relation_supported(void) { return relation_supported; }
esp_err_t c5_spi_link_wifi_relation_begin_check(cerberus_c5_wifi_relation_reader_t *reader,
    uint8_t generation, uint32_t deadline)
{
    assert(reader && generation == wifi_scan_status.generation);
    memset(reader, 0, sizeof(*reader)); reader->generation = generation;
    reader->deadline_ms = deadline;
    ++relation_before_calls;
    relation_result_start = wifi_result_calls;
    relation_status_start = wifi_end_calls;
    return ESP_OK;
}
esp_err_t c5_spi_link_wifi_relation_begin_check_expected(cerberus_c5_wifi_relation_reader_t *reader,
    const cerberus_c5_wifi_relation_bank_t *proof, uint32_t deadline)
{
    assert(!relation_before_error && !memcmp(proof, &relation_proof, sizeof(*proof)));
    assert(wifi_result_calls == relation_result_start + wifi_scan_status.stored_count);
    assert(wifi_end_calls == relation_status_start + 1U);
    memset(reader, 0, sizeof(*reader)); reader->expect_bank = true;
    reader->deadline_ms = deadline;
    ++relation_after_calls;
    return ESP_OK;
}
esp_err_t c5_spi_link_wifi_relation_step(cerberus_c5_wifi_relation_reader_t *reader,
    cerberus_c5_wifi_relation_bank_t *proof, bool *complete,
    cerberus_c5_relation_continue_fn proceed, void *context)
{
    (void)proceed; (void)context;
    physical_attempt();
    *complete = false; memset(proof, 0, sizeof(*proof));
    esp_err_t error = reader->expect_bank ? relation_after_error : relation_before_error;
    if (error) { failure_expected_terminal_us = clock_peek(); return error; }
    if (fence_stalled) return ESP_OK;
    if (++reader->phase < 3U) return ESP_OK;
    *proof = relation_proof; *complete = true; return ESP_OK;
}
void c5_spi_link_wifi_relation_cancel(cerberus_c5_wifi_relation_reader_t *reader)
{ reader->error = ESP_ERR_NOT_FINISHED; }
static void wifi_relation_publish(uint8_t generation,
    const cerberus_c5_wifi_relation_bank_t *proof, esp_err_t error)
{
    const live_scan_record_t *r = &live_records[SENTINEL_SCAN_WIFI];
    assert(r->captured && r->saved_generation == generation &&
        r->count == wifi_scan_status.stored_count);
    assert(wifi_end_calls == relation_status_start + 1U);
    if (r->count) assert(!memcmp(live_store->wifi, wifi_source, r->count * sizeof(wifi_source[0])));
    ++relation_publications;
    published_relation_generation = generation;
    published_relation_proof = *proof;
    published_relation_error = error;
    publish_expected_terminal_us = clock_peek();
}

static void *heap_caps_calloc(size_t count, size_t bytes, unsigned capabilities)
{
    assert(capabilities == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (inspect_allocation_readout) {
        cerberus_wifi_readout_diagnostics_t snapshot = readout();
        assert(snapshot.state == CERBERUS_WIFI_READOUT_READING && snapshot.step_calls == 0U);
        assert(snapshot.rows_total == wifi_scan_status.stored_count && snapshot.rows_copied == 0U);
    }
    clock_extra_us += clock_allocation_us;
    ++allocation_calls;
    if (allocation_calls == fail_allocation_call) return NULL;
    assert(allocation_calls < sizeof(allocations) / sizeof(allocations[0]));
    void *pointer = calloc(count, bytes);
    assert(pointer);
    allocations[allocation_calls].pointer = pointer;
    allocations[allocation_calls].bytes = count * bytes;
    allocations[allocation_calls].live = true;
    ++live_allocations;
    return pointer;
}

static void heap_caps_free(void *pointer)
{
    if (!pointer) return;
    for (size_t i = 1U; i <= allocation_calls; ++i) {
        if (allocations[i].live && allocations[i].pointer == pointer) {
            allocations[i].live = false;
            --live_allocations;
            ++freed_allocations;
            free(pointer);
            return;
        }
    }
    assert(!"unowned or already freed allocation");
}

static void wifi_commit_cached_results(const cerberus_c5_wifi_result_t *rows, uint16_t count, uint8_t generation)
{
    assert(generation == wifi_scan_status.generation && count == wifi_scan_status.stored_count);
    assert(!memcmp(rows, wifi_source, count * sizeof(*rows))); ++refresh_calls;
    clock_extra_us += clock_commit_us;
}
static esp_err_t refresh_wifi_defense_survey(void) { return ESP_ERR_NOT_SUPPORTED; }

esp_err_t c5_spi_link_wifi_scan_result_begin(cerberus_c5_wifi_scan_reader_t *reader,
    uint8_t generation, uint8_t index, uint32_t deadline)
{
    assert(generation == wifi_scan_status.generation && index < wifi_scan_status.stored_count);
    memset(reader, 0, sizeof(*reader)); reader->generation = generation;
    reader->index = index; reader->deadline_ms = deadline;
    return ESP_OK;
}
esp_err_t c5_spi_link_wifi_scan_result_step(cerberus_c5_wifi_scan_reader_t *reader,
    cerberus_c5_wifi_result_t *out, cerberus_s3_wifi_observation_t *observation, bool *complete)
{
    physical_attempt(); *complete = false;
    memset(out, 0, sizeof(*out)); memset(observation, 0, sizeof(*observation));
    if (row_stalled) return ESP_OK;
    if (row_copy_only) { ++reader->copied; return ESP_OK; }
    if (++reader->phase < 3U) return ESP_OK;
    ++result_calls; ++wifi_result_calls;
    if (failure == FAIL_WIFI_RESULT && reader->index == failure_index) {
        failure_expected_terminal_us = clock_peek(); return ESP_FAIL;
    }
    *observation = wifi_timing_source[reader->index];
    *out = wifi_source[reader->index]; *complete = true; return ESP_OK;
}
void c5_spi_link_wifi_scan_result_cancel(cerberus_c5_wifi_scan_reader_t *reader)
{ reader->error = ESP_ERR_NOT_FINISHED; }
esp_err_t c5_spi_link_get_wifi_scan_status_once(uint32_t deadline, cerberus_wifi_scan_status_payload_t *out)
{
    (void)deadline; physical_attempt(); ++wifi_end_calls;
    ++end_status_calls;
    if (failure == FAIL_WIFI_END) { failure_expected_terminal_us = clock_peek(); return ESP_FAIL; }
    *out = wifi_end; return ESP_OK;
}
/* Model only the exact native boundary. Production capture/final fencing and
 * allocation/publication are compiled below; actual scanner copies and mutex
 * ownership have independent SDK-event tests. No generic BLE getter exists in
 * this fixture, so an accidental unscoped fallback fails to link. */
static void mutate_passive_fence(cerberus_s3_ble_passive_state_t *state)
{
    switch (passive_fence_fault) {
    case 1U: state->epoch ^= UINT64_C(1) << 48U; break;
    case 2U: ++state->scan_epoch; break;
    case 3U: ++state->host_epoch; break;
    case 4U: ++state->started_ms; break;
    case 5U: state->owned = true; break;
    case 6U: state->stopping = true; break;
    case 7U: state->error = ESP_ERR_INVALID_STATE; break;
    case 8U: state->scan.state = CERBERUS_BLE_SCAN_RUNNING; break;
    default: break;
    }
}
esp_err_t cerberus_s3_ble_scanner_passive_copy_row(uint64_t epoch, uint8_t index,
    cerberus_s3_ble_discovery_record_t *row, cerberus_s3_ble_passive_state_t *state)
{
    assert(row && state);
    memset(row, 0, sizeof(*row)); memset(state, 0, sizeof(*state));
    if (!epoch || epoch != passive_source.epoch) return ESP_ERR_NOT_FOUND;
    assert(index < ble_scan_status.stored_count);
    ++result_calls;
    if (index == failure_index && (failure == FAIL_BLE_RESULT || failure == FAIL_BLE_FOCUS ||
        failure == FAIL_BLE_ADV || failure == FAIL_BLE_TIME)) return ESP_FAIL;
    row->peer = ble_source[index]; row->focus = focus_source[index];
    row->advertisement = advertisement_source[index]; row->observed_ms = observation_source[index];
    row->generation = ble_scan_status.generation; row->index = index;
    *state = passive_source;
    if (passive_fault_at_row && index == 1U) mutate_passive_fence(state);
    return ESP_OK;
}
esp_err_t cerberus_s3_ble_scanner_passive_get_state(uint64_t epoch,
    cerberus_s3_ble_passive_state_t *state)
{
    assert(state); memset(state, 0, sizeof(*state)); ++end_status_calls;
    if (failure == FAIL_BLE_END) return ESP_FAIL;
    if (!epoch || epoch != passive_source.epoch) return ESP_ERR_NOT_FOUND;
    *state = passive_source; state->scan = ble_end;
    if (!passive_fault_at_row) mutate_passive_fence(state);
    return ESP_OK;
}
#ifndef CERBERUS_SCAN_STORE_EXTERNAL_PASSIVE_CONTROL
esp_err_t cerberus_s3_ble_scanner_passive_start(uint64_t previous,
    cerberus_s3_ble_passive_state_t *state)
{ (void)previous; (void)state; assert(!"snapshot fixture must not start BLE"); return ESP_ERR_NOT_SUPPORTED; }
esp_err_t cerberus_s3_ble_scanner_passive_stop(uint64_t epoch,
    cerberus_s3_ble_passive_state_t *state)
{ (void)epoch; (void)state; assert(!"snapshot fixture must not stop BLE"); return ESP_ERR_NOT_SUPPORTED; }
esp_err_t cerberus_s3_ble_scanner_passive_release(uint64_t epoch)
{ (void)epoch; assert(!"snapshot fixture must not release BLE"); return ESP_ERR_NOT_SUPPORTED; }
#endif
esp_err_t c5_spi_link_get_ieee802154_beacon_scan_result(uint8_t generation,
    uint8_t index, cerberus_ieee802154_beacon_scan_result_payload_t *out)
{
    assert(generation == ieee802154_beacon_status.generation &&
        index < ieee802154_beacon_status.stored_count);
    ++result_calls;
    if (failure == FAIL_BEACON_RESULT && index == failure_index) return ESP_FAIL;
    *out = beacon_source[index]; return ESP_OK;
}
esp_err_t c5_spi_link_get_ieee802154_beacon_scan_status(
    cerberus_ieee802154_beacon_scan_status_payload_t *out)
{
    ++end_status_calls;
    if (failure == FAIL_BEACON_END) return ESP_FAIL;
    *out = beacon_end; return ESP_OK;
}
esp_err_t c5_spi_link_get_ieee802154_energy_scan_result(uint8_t generation,
    cerberus_ieee802154_energy_scan_result_payload_t *out)
{ (void)generation; (void)out; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t c5_spi_link_get_ieee802154_activity_scan_result(uint8_t generation,
    uint8_t index, cerberus_ieee802154_activity_scan_result_payload_t *out)
{ (void)generation; (void)index; (void)out; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t c5_spi_link_get_ieee802154_activity_scan_status(
    cerberus_ieee802154_activity_scan_status_payload_t *out)
{ (void)out; return ESP_ERR_NOT_SUPPORTED; }

#include "scan_display_capture_impl.h"

/* Ordinary tests use the real monotonic clock for the simulated UI tick.
 * Dedicated fault/timing tests below call production directly instead. */
static bool tick_at(uint32_t now, bool allowed, cerberus_wifi_readout_cancel_reason_t reason)
{
    clock_ms = now;
    return live_wifi_capture_tick(now, allowed, reason);
}

/* Drive distinct UI iterations, asserting the physical-attempt budget at every
 * boundary rather than treating a multi-step transaction as one callback. */
static bool capture_all(sentinel_scan_tool_t tool, uint32_t now)
{
    if (tool != SENTINEL_SCAN_WIFI) return live_capture(tool, now);
    unsigned before = relation_publications;
    (void)live_capture(tool, now);
    for (unsigned ticks = 0; live_wifi_capture_busy() && ticks < 256U; ++ticks) {
        assert(live_wifi_capture_defers_c5());
        unsigned attempts = physical_attempts;
        (void)tick_at(now, true, CERBERUS_WIFI_READOUT_CANCEL_NONE);
        assert(physical_attempts - attempts <= LIVE_WIFI_BATCH_MAX_STEPS);
    }
    assert(!live_wifi_capture_busy());
    return relation_publications != before;
}

/* Actual scheduler/session integration. Only UI painting, RF command delivery
 * and the polling adapter are simulated; the production transfer, command
 * scheduling, cancellation and ownership decisions run unchanged. */
static bool live_ble_session_initialized, ui_capability_blocked, wifi_selection_valid;
static bool live_scan_observation_valid = true, live_ble_observation_valid = true;
static bool live_radio_busy, c5_online = true;
static unsigned live_last_poll, live_frame, ieee802154_mode, start_commands, stop_commands;
static struct { bool attack; } field_menu;
static cerberus_wifi_defense_status_payload_t wifi_defense_status;
static void ble_discovery_tick(uint64_t now, bool asleep) { (void)now; (void)asleep; }
static bool ble_discovery_page(void) { return false; }
static bool watch_ble_requested(void) { return false; }
static bool cerberus_ble_discovery_session_released(const void *session) { (void)session; return true; }
static sentinel_scan_tool_t watch_requested_tool(void) { return SENTINEL_SCAN_NONE; }
#include "scan_display_page_tool_impl.h"
static bool live_is_view(void) { return current_page == UI_PAGE_WIFI && wifi_mode == UI_WIFI_MODE_LIST; }
static void live_make_view(void) {}
static esp_err_t live_paint(bool full) { (void)full; return ESP_OK; }
static esp_err_t draw_current_page(void) { return ESP_OK; }
#define ESP_ERROR_CHECK(call) assert((call) == ESP_OK)
static bool live_observe(uint32_t now)
{
    if (live_wifi_capture_defers_c5()) return false;
    live_status(SENTINEL_SCAN_WIFI, ESP_OK, wifi_scan_status.state,
        wifi_scan_status.generation, 0, wifi_scan_status.error_code);
    return live_capture(SENTINEL_SCAN_WIFI, now);
}
static esp_err_t live_execute(sentinel_scan_command_t command)
{
    assert(command.tool == SENTINEL_SCAN_WIFI);
    if (command.kind == SENTINEL_SCAN_COMMAND_START) {
        ++start_commands;
        wifi_scan_status.state = SENTINEL_SCAN_RUNNING;
    } else {
        assert(command.kind == SENTINEL_SCAN_COMMAND_STOP);
        ++stop_commands; wifi_scan_status.state = SENTINEL_SCAN_IDLE;
    }
    return ESP_OK;
}
#include "scan_display_tick_impl.h"
static void scan_at(uint32_t now, bool asleep)
{
    clock_ms = now;
    live_scan_tick(now, asleep);
}



static void reset(void)
{
    live_wifi_capture_reset(); live_wifi_worked = false;
    memset(&live_session, 0, sizeof(live_session));
    memset(&live_ble_session, 0, sizeof(live_ble_session));
    live_ble_forget_lease();
    assert(live_ble_recon_released());
    live_ble_sleeping = false;
    live_ble_capture_epoch = 0U; memset(&live_ble_capture_state, 0, sizeof(live_ble_capture_state));
    live_ble_lease_error = ESP_OK;
    memset(&passive_source, 0, sizeof(passive_source));
    passive_next_epoch = UINT64_C(0x1234567800000000);
    passive_fence_fault = 0U; passive_fault_at_row = false;
    ble_scan_checked = false; ble_scan_error = ESP_OK;
    live_ble_session_initialized = false;
    live_last_poll = live_frame = start_commands = stop_commands = 0;
    ui_capability_blocked = wifi_selection_valid = live_radio_busy = false;
    heap_caps_free(live_ble_evidence); live_ble_evidence = NULL;
    heap_caps_free(live_store); live_store = NULL;
    assert(live_allocations == 0U);
    memset(allocations, 0, sizeof(allocations));
    allocation_calls = live_allocations = freed_allocations = fail_allocation_call = 0U;
    result_calls = end_status_calls = refresh_calls = physical_attempts = 0U;
    row_stalled = fence_stalled = row_copy_only = false;
    io_cancel = IO_CANCEL_NONE; io_cancel_at = 0U;
    wifi_result_calls = wifi_end_calls = 0U;
    relation_before_calls = relation_after_calls = relation_publications = 0U;
    relation_result_start = relation_status_start = 0U;
    publish_expected_terminal_us = failure_expected_terminal_us = 0;
    relation_before_error = relation_after_error = published_relation_error = ESP_OK;
    memset(&relation_proof, 0, sizeof(relation_proof));
    memset(&published_relation_proof, 0, sizeof(published_relation_proof));
    memset(live_records, 0, sizeof(live_records));
    memset(live_page, 0, sizeof(live_page));
    memset(live_ble_focus_bits, 0, sizeof(live_ble_focus_bits));
    live_ble_order_dirty = false;
    live_capture_pending = 0; live_status_changed = false;
    live_ble_published_ms = live_wifi_published_us = 0; clock_ms = 0;
    clock_extra_us = clock_advance_us = clock_io_us = clock_allocation_us = clock_commit_us = 0;
    clock_last_us = 0; inspect_allocation_readout = false;
    clock_script_count = clock_script_next = 0U;
    current_page = UI_PAGE_WIFI; wifi_mode = UI_WIFI_MODE_LIST; ble_mode = UI_BLE_MODE_MENU;
    failure = FAIL_NONE; failure_index = 1U; relation_supported = true;
}

static void prepare(sentinel_scan_tool_t tool, uint8_t generation, uint8_t count)
{
    if (tool == SENTINEL_SCAN_WIFI) {
        ++live_session.arm_generation;
        live_session.active_arm_generation = live_session.arm_generation;
        live_session.desired = live_session.active = SENTINEL_SCAN_WIFI;
    }
    live_records[tool].valid = true;
    live_records[tool].state = SENTINEL_SCAN_COMPLETE;
    live_records[tool].generation = generation;
    live_records[tool].error = ESP_OK;
    if (tool == SENTINEL_SCAN_WIFI) {
        wifi_scan_status = (cerberus_wifi_scan_status_payload_t){
            .generation = generation, .state = CERBERUS_WIFI_SCAN_COMPLETE, .stored_count = count};
        wifi_end = wifi_scan_status;
        relation_proof.status.generation = generation;
        relation_proof.status.epoch = UINT64_C(0x1234567800000000) + generation;
        relation_proof.request_started_us = UINT64_C(0x2345678900000000) + generation;
        relation_proof.source_now_us = UINT64_C(0x3456789a00000000) + generation;
        for (size_t i = 0U; i < CERBERUS_WIFI_SCAN_MAX_RESULTS; ++i) {
            memset(&wifi_source[i], 0, sizeof(wifi_source[i]));
            wifi_source[i].bssid[0] = generation;
            wifi_source[i].bssid[1] = (uint8_t)i;
            wifi_source[i].channel = 1U;
            snprintf(wifi_source[i].ssid, sizeof(wifi_source[i].ssid), "wifi-%u-%u", generation, (unsigned)i);
            memset(&wifi_timing_source[i], 0, sizeof(wifi_timing_source[i]));
            wifi_timing_source[i].request_started_us = UINT64_C(4294967296000) + 1000U * generation + i;
            wifi_timing_source[i].source_age_ms = 100U * generation + (uint32_t)i;
            wifi_timing_source[i].valid = (i % 2U) == 0U;
        }
    } else if (tool == SENTINEL_SCAN_BLE) {
        ble_scan_status = (cerberus_ble_scan_status_payload_t){
            .generation = generation, .state = CERBERUS_BLE_SCAN_COMPLETE, .stored_count = count};
        ble_end = ble_scan_status;
        ++passive_next_epoch;
        passive_source = (cerberus_s3_ble_passive_state_t){
            .epoch = passive_next_epoch, .started_ms = UINT64_C(4294967296),
            .source_now_ms = UINT64_C(4294967296) + 10000U,
            .scan = ble_scan_status, .scan_epoch = (uint32_t)passive_next_epoch,
            .host_epoch = 17U, .attached = true};
        live_ble_lease_epoch = passive_source.epoch;
        live_ble_lease_state = passive_source;
        live_ble_stop_pending = false;
        for (size_t i = 0U; i < CERBERUS_BLE_SCAN_MAX_RESULTS; ++i) {
            memset(&ble_source[i], 0, sizeof(ble_source[i]));
            ble_source[i].address[0] = generation; ble_source[i].address[1] = (uint8_t)i;
            ble_source[i].address_type = 1U; ble_source[i].rssi = -60;
            snprintf(ble_source[i].name, sizeof(ble_source[i].name), "ble-%u-%u", generation, (unsigned)i);
            focus_source[i] = (cerberus_s3_ble_focus_t){.category_bits = (uint8_t)(1U << (i % 6U))};
            advertisement_source[i] = (cerberus_c5_ble_advertisement_t){
                .address_type = 1U, .length = 3U, .data = {2U,1U,6U}};
            memcpy(advertisement_source[i].address, ble_source[i].address, 6U);
            observation_source[i] = 100U * generation + (uint32_t)i;
        }
    } else if (tool == SENTINEL_SCAN_BEACON) {
        ieee802154_beacon_status = (cerberus_ieee802154_beacon_scan_status_payload_t){
            .generation = generation, .state = CERBERUS_IEEE802154_BEACON_SCAN_COMPLETE, .stored_count = count};
        beacon_end = ieee802154_beacon_status;
        for (size_t i = 0U; i < CERBERUS_IEEE802154_BEACON_MAX_RESULTS; ++i) {
            memset(&beacon_source[i], (int)(generation + i), sizeof(beacon_source[i]));
        }
    } else assert(false);
}

static void check_first_and_reuse(void)
{
    reset(); prepare(SENTINEL_SCAN_WIFI, 1U, 2U);
    current_page = UI_PAGE_WIFI; wifi_mode = UI_WIFI_MODE_BASELINE;
    assert(capture_all(SENTINEL_SCAN_WIFI, 1000U));
    assert(allocation_calls == 1U && live_allocations == 1U &&
        allocations[1].bytes == sizeof(*live_store));
    assert(refresh_calls == 1U);
    cerberus_scan_display_store_t *pool = live_store;
    cerberus_c5_wifi_result_t retained_wifi[2];
    cerberus_s3_wifi_observation_t retained_wifi_timing[2];
    memcpy(retained_wifi, live_store->wifi, sizeof(retained_wifi));
    memcpy(retained_wifi_timing, live_store->wifi_observations, sizeof(retained_wifi_timing));
    assert(memcmp(live_store->wifi, wifi_source, sizeof(retained_wifi)) == 0);
    assert(memcmp(retained_wifi_timing, wifi_timing_source, sizeof(retained_wifi_timing)) == 0);
    current_page = UI_PAGE_BLE;
    prepare(SENTINEL_SCAN_BLE, 2U, 2U);
    cerberus_c5_ble_result_t original_ble[2]; memcpy(original_ble, ble_source, sizeof(original_ble));
    clock_ms = INT64_C(4294967296) + 2000;
    assert(capture_all(SENTINEL_SCAN_BLE, 2000U));
    assert(live_ble_published_ms == clock_ms); /* Independent full-width publication clock. */
    assert(live_ble_order_dirty);
    assert(live_store == pool && allocation_calls == 2U && live_allocations == 2U);
    assert(memcmp(retained_wifi, live_store->wifi, sizeof(retained_wifi)) == 0);
    assert(memcmp(retained_wifi_timing, live_store->wifi_observations, sizeof(retained_wifi_timing)) == 0);
    assert(memcmp(original_ble, ble_source, sizeof(original_ble)) == 0);
    assert(memcmp(original_ble, live_store->ble, sizeof(original_ble)) == 0);
    assert(live_ble_evidence[1].observed_ms == 201U &&
        live_ble_evidence[1].generation == 2U && live_ble_evidence[1].index == 1U);
    assert(memcmp(&live_ble_evidence[1].advertisement, &advertisement_source[1], sizeof(advertisement_source[1])) == 0);
    current_page = UI_PAGE_IEEE802154;
    prepare(SENTINEL_SCAN_BEACON, 3U, 2U);
    assert(capture_all(SENTINEL_SCAN_BEACON, 3000U));
    assert(live_store == pool && allocation_calls == 2U && live_allocations == 2U);
    assert(memcmp(retained_wifi, live_store->wifi, sizeof(retained_wifi)) == 0);
    assert(memcmp(retained_wifi_timing, live_store->wifi_observations, sizeof(retained_wifi_timing)) == 0);
    assert(memcmp(original_ble, live_store->ble, sizeof(original_ble)) == 0);
    assert(memcmp(live_store->beacons, beacon_source, 2U * sizeof(beacon_source[0])) == 0);
    current_page = UI_PAGE_WATCH;
    assert(live_store_acquire() && live_store == pool && allocation_calls == 2U);
    unsigned previous_calls = result_calls;
    assert(!capture_all(SENTINEL_SCAN_BLE, 4000U)); /* already captured generation */
    assert(result_calls == previous_calls && allocation_calls == 2U);
    assert(live_ble_published_ms == INT64_C(4294967296) + 2000);
}

static void check_empty_and_limits(void)
{
    const sentinel_scan_tool_t tools[] = {SENTINEL_SCAN_WIFI,SENTINEL_SCAN_BLE,SENTINEL_SCAN_BEACON};
    const uint8_t limits[] = {CERBERUS_WIFI_SCAN_MAX_RESULTS,CERBERUS_BLE_SCAN_MAX_RESULTS,CERBERUS_IEEE802154_BEACON_MAX_RESULTS};
    for (size_t i = 0U; i < 3U; ++i) {
        reset(); prepare(tools[i], 1U, 0U);
        clock_ms = 1000;
        assert(capture_all(tools[i], 1000U));
        if (tools[i] == SENTINEL_SCAN_BLE) assert(live_ble_published_ms == 1000);
        assert(!live_ble_evidence && result_calls == 0U);
        assert(allocation_calls == (tools[i] == SENTINEL_SCAN_WIFI ? 1U : 0U));
        assert(live_records[tools[i]].captured && live_records[tools[i]].count == 0U);
        prepare(tools[i], 2U, (uint8_t)(limits[i] + 1U));
        assert(!capture_all(tools[i], 2000U));
        assert(live_records[tools[i]].error == ESP_ERR_INVALID_RESPONSE &&
            live_records[tools[i]].saved_generation == 1U && allocation_calls == (tools[i] == SENTINEL_SCAN_WIFI ? 1U : 0U));
        prepare(tools[i], 3U, limits[i]);
        live_page[tools[i]] = 250U;
        assert(capture_all(tools[i], 3000U));
        assert(live_records[tools[i]].count == limits[i]);
        assert(live_page[tools[i]] == (limits[i] - 1U) / 4U);
        cerberus_scan_display_store_t *pool = live_store;
        prepare(tools[i], 4U, 0U);
        clock_ms = 4000;
        assert(capture_all(tools[i], 4000U));
        assert(live_store == pool && live_records[tools[i]].count == 0U && live_page[tools[i]] == 0U);
        if (tools[i] == SENTINEL_SCAN_BLE) assert(!live_ble_evidence && live_allocations == 1U && live_ble_published_ms == 4000);
    }
}

static void check_allocation_failures(void)
{
    const sentinel_scan_tool_t tools[] = {SENTINEL_SCAN_WIFI,SENTINEL_SCAN_BLE,SENTINEL_SCAN_BEACON};
    for (size_t i = 0U; i < 3U; ++i) {
        reset(); prepare(tools[i], 1U, 2U); fail_allocation_call = 1U;
        assert(!capture_all(tools[i], 1000U));
        assert(!live_store && !live_records[tools[i]].captured && result_calls == 0U &&
            live_records[tools[i]].error == ESP_ERR_NO_MEM && live_allocations == 0U);
        prepare(tools[i], 1U, 2U); fail_allocation_call = 0U;
        assert(capture_all(tools[i], 2000U));
        assert(live_records[tools[i]].captured && live_records[tools[i]].captured_ms == 2000U);
    }
    reset(); prepare(SENTINEL_SCAN_BLE, 1U, 2U); fail_allocation_call = 2U;
    assert(!capture_all(SENTINEL_SCAN_BLE, 1000U));
    assert(live_store && !live_ble_evidence && live_allocations == 1U &&
        !live_records[SENTINEL_SCAN_BLE].captured && result_calls == 0U);
    prepare(SENTINEL_SCAN_BLE, 1U, 2U); fail_allocation_call = 0U;
    assert(capture_all(SENTINEL_SCAN_BLE, 2000U));
    cerberus_findmy_monitor_record_t *evidence = live_ble_evidence;
    prepare(SENTINEL_SCAN_BLE, 2U, 3U); fail_allocation_call = allocation_calls + 1U;
    assert(!capture_all(SENTINEL_SCAN_BLE, 3000U));
    assert(live_ble_evidence == evidence && live_allocations == 2U &&
        live_records[SENTINEL_SCAN_BLE].saved_generation == 1U &&
        live_records[SENTINEL_SCAN_BLE].count == 2U);
}

static void check_failed_capture_retains_published(void)
{
    const sentinel_scan_tool_t tools[] = {SENTINEL_SCAN_WIFI,SENTINEL_SCAN_BLE,SENTINEL_SCAN_BEACON};
    for (size_t t = 0U; t < 3U; ++t) {
        for (unsigned variant = 0U; variant < (t == 1U ? 9U : 5U); ++variant) {
            reset(); prepare(tools[t], 7U, 2U);
            clock_ms = INT64_C(4294967296) + 5000;
            assert(capture_all(tools[t], 5000U));
            int64_t published_before = live_ble_published_ms;
            int64_t wifi_published_before = live_wifi_published_us;
            cerberus_scan_display_store_t retained = *live_store;
            cerberus_findmy_monitor_record_t evidence[2];
            cerberus_findmy_monitor_record_t *evidence_pointer = live_ble_evidence;
            uint8_t focus_bits[sizeof(live_ble_focus_bits)];
            memcpy(focus_bits, live_ble_focus_bits, sizeof(focus_bits));
            if (evidence_pointer) memcpy(evidence, evidence_pointer, sizeof(evidence));
            size_t allocations_before = live_allocations;
            live_ble_order_dirty = false;
            prepare(tools[t], 8U, 3U);
            if (t == 0U) {
                if (variant == 0U) failure = FAIL_WIFI_RESULT;
                if (variant == 1U) failure = FAIL_WIFI_END;
                if (variant == 2U) ++wifi_end.generation;
                if (variant == 3U) wifi_end.state = CERBERUS_WIFI_SCAN_RUNNING;
                if (variant == 4U) ++wifi_end.stored_count;
            } else if (t == 1U) {
                const enum failure_point points[] = {FAIL_BLE_RESULT,FAIL_BLE_FOCUS,FAIL_BLE_ADV,FAIL_BLE_TIME,FAIL_BLE_END};
                if (variant < 5U) failure = points[variant];
                if (variant == 5U) ++ble_end.generation;
                if (variant == 6U) ble_end.state = CERBERUS_BLE_SCAN_RUNNING;
                if (variant == 7U) ++ble_end.stored_count;
                if (variant == 8U) --ble_end.stored_count;
            } else {
                if (variant == 0U) failure = FAIL_BEACON_RESULT;
                if (variant == 1U) failure = FAIL_BEACON_END;
                if (variant == 2U) ++beacon_end.generation;
                if (variant == 3U) beacon_end.state = CERBERUS_IEEE802154_BEACON_SCAN_RUNNING;
                if (variant == 4U) ++beacon_end.stored_count;
            }
            clock_ms += 1000;
            assert(!capture_all(tools[t], 6000U));
            assert(live_ble_published_ms == published_before);
            assert(live_wifi_published_us == wifi_published_before);
            assert(live_records[tools[t]].saved_generation == 7U &&
                live_records[tools[t]].count == 2U && live_records[tools[t]].captured_ms == 5000U);
            assert(memcmp(retained.wifi, live_store->wifi, sizeof(retained.wifi)) == 0);
            assert(memcmp(retained.wifi_observations, live_store->wifi_observations,
                sizeof(retained.wifi_observations)) == 0);
            assert(memcmp(retained.ble, live_store->ble, sizeof(retained.ble)) == 0);
            assert(memcmp(retained.beacons, live_store->beacons, sizeof(retained.beacons)) == 0);
            assert(live_ble_evidence == evidence_pointer && live_allocations == allocations_before);
            assert(!live_ble_order_dirty);
            assert(memcmp(focus_bits, live_ble_focus_bits, sizeof(focus_bits)) == 0);
            if (evidence_pointer) assert(memcmp(evidence, evidence_pointer, sizeof(evidence)) == 0);
        }
    }
}

static void check_exact_ble_publication_fence(void)
{
    /* A source can be replaced/reset between row copies or just before commit,
     * even while the public generation still matches. Nothing from a partial
     * bank may replace the prior published evidence or its capture identity. */
    for (unsigned phase = 0U; phase < 2U; ++phase) {
        for (unsigned fault = 1U; fault <= 8U; ++fault) {
            reset(); prepare(SENTINEL_SCAN_BLE, 7U, 2U); clock_ms = 1000;
            assert(capture_all(SENTINEL_SCAN_BLE, 1000U));
            const uint64_t saved_epoch = live_ble_capture_epoch;
            const cerberus_s3_ble_passive_state_t saved_state = live_ble_capture_state;
            cerberus_findmy_monitor_record_t *saved = live_ble_evidence;
            cerberus_findmy_monitor_record_t contents[2]; memcpy(contents, saved, sizeof(contents));
            cerberus_c5_ble_result_t rows[2]; memcpy(rows, live_store->ble, sizeof(rows));
            prepare(SENTINEL_SCAN_BLE, 7U, 3U);
            live_capture_pending |= UINT32_C(1) << SENTINEL_SCAN_BLE;
            assert(live_ble_lease_epoch != saved_epoch);
            passive_fence_fault = fault; passive_fault_at_row = phase == 0U; clock_ms = 2000;
            assert(!capture_all(SENTINEL_SCAN_BLE, 2000U));
            assert(live_records[SENTINEL_SCAN_BLE].error == ESP_ERR_INVALID_STATE);
            assert(live_ble_capture_epoch == saved_epoch && live_ble_evidence == saved);
            assert(!memcmp(&live_ble_capture_state, &saved_state, sizeof(saved_state)));
            assert(!memcmp(saved, contents, sizeof(contents)));
            assert(!memcmp(live_store->ble, rows, sizeof(rows)));
            assert(live_ble_published_ms == 1000 && live_records[SENTINEL_SCAN_BLE].captured_ms == 1000U);
            assert(live_allocations == 2U && live_records[SENTINEL_SCAN_BLE].count == 2U);
        }
    }
    printf("Exact BLE publication: 16 mid-copy/final-fence faults retain the previous bank and full capture identity\n");
}

static void check_same_generation_cycle(void)
{
    const sentinel_scan_tool_t tools[]={SENTINEL_SCAN_WIFI,SENTINEL_SCAN_BLE,SENTINEL_SCAN_BEACON};
    const enum failure_point failures[]={FAIL_WIFI_RESULT,FAIL_BLE_RESULT,FAIL_BEACON_RESULT};
    for(unsigned t=0;t<3;++t){
        sentinel_scan_tool_t tool=tools[t];uint32_t bit=UINT32_C(1)<<tool;
        reset();prepare(tool,7,2);clock_ms=1000;assert(capture_all(tool,1000));
        cerberus_scan_display_store_t frozen=*live_store;
        int64_t published=live_ble_published_ms;
        size_t allocations_before=allocation_calls;
        /* A complete poll is not a new sweep, even after a query failure. */
        live_status(tool,ESP_OK,SENTINEL_SCAN_COMPLETE,7,0,0);
        assert(!(live_capture_pending&bit)&&!capture_all(tool,1100));
        live_status(tool,ESP_FAIL,SENTINEL_SCAN_IDLE,0,0,0);
        live_status(tool,ESP_OK,SENTINEL_SCAN_COMPLETE,7,0,0);
        assert(!(live_capture_pending&bit)&&!capture_all(tool,1200));
        assert(allocation_calls==allocations_before&&live_ble_published_ms==published);
        /* A new witnessed cycle can wrap back to the saved generation. */
        live_status(tool,ESP_OK,SENTINEL_SCAN_RUNNING,7,0,0);
        assert((live_capture_pending&bit)&&!capture_all(tool,1300));
        assert(live_records[tool].captured&&live_records[tool].saved_generation==7&&live_records[tool].count==2);
        assert(!memcmp(&frozen,live_store,sizeof(frozen))&&live_ble_published_ms==published);
        prepare(tool,7,1);memset(wifi_source[0].ssid,'N',8);memset(ble_source[0].name,'N',8);beacon_source[0].channel=26;
        wifi_timing_source[0].request_started_us+=1000000U;wifi_timing_source[0].source_age_ms=11;
        observation_source[0]=1500;clock_ms=2000;
        live_status(tool,ESP_OK,SENTINEL_SCAN_COMPLETE,7,0,0);
        failure=failures[t];failure_index=0;
        assert(!capture_all(tool,2000)&&(live_capture_pending&bit));
        assert(live_records[tool].count==2&&live_records[tool].captured_ms==1000&&live_ble_published_ms==published);
        assert(!memcmp(frozen.wifi,live_store->wifi,sizeof(frozen.wifi)));
        assert(!memcmp(frozen.wifi_observations,live_store->wifi_observations,sizeof(frozen.wifi_observations)));
        assert(!memcmp(frozen.ble,live_store->ble,sizeof(frozen.ble)));
        assert(!memcmp(frozen.beacons,live_store->beacons,sizeof(frozen.beacons)));
        failure=FAIL_NONE;failure_index=1;
        if(tool==SENTINEL_SCAN_WIFI){
            assert(!capture_all(tool,2000)); /* Same failed attempt stays terminal. */
            ++live_session.arm_generation;
            live_session.active_arm_generation = live_session.arm_generation;
        }
        live_status(tool,ESP_OK,SENTINEL_SCAN_COMPLETE,7,0,0);
        assert(capture_all(tool,2000)&&!(live_capture_pending&bit));
        assert(live_records[tool].count==1&&live_records[tool].captured_ms==2000);
        if(tool==SENTINEL_SCAN_WIFI){
            assert(!memcmp(live_store->wifi,wifi_source,sizeof(wifi_source[0])));
            assert(!memcmp(live_store->wifi_observations,wifi_timing_source,sizeof(wifi_timing_source[0])));
        }
        if(tool==SENTINEL_SCAN_BLE)assert(live_ble_evidence[0].observed_ms==1500&&live_ble_published_ms==2000);
        if(tool==SENTINEL_SCAN_BEACON)assert(live_store->beacons[0].channel==26);
        allocations_before=allocation_calls;clock_ms=2500;
        live_status(tool,ESP_OK,SENTINEL_SCAN_COMPLETE,7,0,0);
        assert(!capture_all(tool,2500)&&allocation_calls==allocations_before);
        assert(live_records[tool].captured_ms==2000);
        /* An observed different cycle remains pending if its byte later wraps,
         * even when the final running state was missed between polls. */
        live_status(tool,ESP_OK,SENTINEL_SCAN_IDLE,6,0,0);assert(live_capture_pending&bit);
        prepare(tool,7,0);clock_ms=3000;live_status(tool,ESP_OK,SENTINEL_SCAN_COMPLETE,7,0,0);
        assert(capture_all(tool,3000)&&!(live_capture_pending&bit)&&live_records[tool].count==0);
        if(tool==SENTINEL_SCAN_BLE)assert(!live_ble_evidence&&live_ble_published_ms==3000);
    }
}

static void check_relation_binding_publication(void)
{
    const esp_err_t errors[] = {ESP_OK, ESP_ERR_NOT_SUPPORTED, ESP_ERR_TIMEOUT, ESP_ERR_INVALID_RESPONSE};
    for (unsigned phase = 0U; phase < 2U; ++phase) {
        for (unsigned i = 0U; i < sizeof(errors) / sizeof(errors[0]); ++i) {
            reset(); prepare(SENTINEL_SCAN_WIFI, 241U, 2U);
            if (!phase) relation_before_error = errors[i];
            else relation_after_error = errors[i];
            bool committed = capture_all(SENTINEL_SCAN_WIFI, 1234U);
            assert(committed == (errors[i] == ESP_OK));
            assert(relation_before_calls == 1U);
            assert(relation_after_calls == (relation_before_error == ESP_OK));
            assert(relation_publications == (committed ? 1U : 0U));
            if (committed) {
                assert(published_relation_error == ESP_OK && published_relation_generation == 241U);
                assert(!memcmp(&published_relation_proof, &relation_proof, sizeof(relation_proof)));
                assert(!memcmp(live_store->wifi, wifi_source, 2U * sizeof(wifi_source[0])));
            } else {
                assert(!live_records[SENTINEL_SCAN_WIFI].captured);
                assert(live_records[SENTINEL_SCAN_WIFI].error == errors[i]);
            }
            /* Supported-peer proof failures never silently weaken binding or
             * repeatedly retry a whole bank on the same completed poll. */
            assert(!capture_all(SENTINEL_SCAN_WIFI, 2345U));
            assert(relation_before_calls == 1U);

        }
    }
    for (unsigned why = 0U; why < 6U; ++why) {
        reset(); prepare(SENTINEL_SCAN_WIFI, 7U, 2U);
        assert(capture_all(SENTINEL_SCAN_WIFI, 1000U));
        const cerberus_c5_wifi_relation_bank_t frozen = published_relation_proof;
        prepare(SENTINEL_SCAN_WIFI, 8U, 3U);
        if (why == 0U) failure = FAIL_WIFI_RESULT;
        if (why == 1U) failure = FAIL_WIFI_END;
        if (why == 2U) ++wifi_end.generation;
        if (why == 3U) wifi_end.state = CERBERUS_WIFI_SCAN_RUNNING;
        if (why == 4U) ++wifi_end.stored_count;
        if (why == 5U) wifi_scan_status.stored_count = CERBERUS_WIFI_SCAN_MAX_RESULTS + 1U;
        assert(!capture_all(SENTINEL_SCAN_WIFI, 2000U));
        assert(relation_publications == 1U && relation_after_calls == 1U);
        assert(!memcmp(&published_relation_proof, &frozen, sizeof(frozen)));
        assert(live_records[SENTINEL_SCAN_WIFI].saved_generation == 7U);
    }
    /* Empty and byte-wrapped AP banks still carry their own exact epoch. */
    reset(); prepare(SENTINEL_SCAN_WIFI, 255U, 0U);
    assert(capture_all(SENTINEL_SCAN_WIFI, 1U));
    prepare(SENTINEL_SCAN_WIFI, 0U, 0U);
    relation_proof.status.epoch += UINT64_C(1) << 40;
    assert(capture_all(SENTINEL_SCAN_WIFI, 2U));
    assert(relation_publications == 2U && published_relation_generation == 0U &&
        published_relation_proof.status.epoch == relation_proof.status.epoch);
    reset(); prepare(SENTINEL_SCAN_WIFI, 3U, 2U); relation_supported = false;
    assert(capture_all(SENTINEL_SCAN_WIFI, 1U));
    assert(!relation_before_calls && !relation_after_calls && relation_publications == 1U);
    assert(published_relation_error == ESP_ERR_NOT_SUPPORTED);
    reset(); prepare(SENTINEL_SCAN_BLE, 3U, 0U);
    assert(capture_all(SENTINEL_SCAN_BLE, 1U));
    assert(!relation_before_calls && !relation_after_calls && !relation_publications);
}

static void check_incremental_isolation_and_cancel(void)
{
    reset(); prepare(SENTINEL_SCAN_WIFI, 1U, 2U);
    assert(capture_all(SENTINEL_SCAN_WIFI, 100U));
    cerberus_c5_wifi_result_t frozen[2]; memcpy(frozen, live_store->wifi, sizeof(frozen));
    prepare(SENTINEL_SCAN_WIFI, 2U, CERBERUS_WIFI_SCAN_MAX_RESULTS);
    assert(live_capture(SENTINEL_SCAN_WIFI, 200U));
    assert(relation_publications == 1U);
    bool interleaved = false;
    for (unsigned tick = 0; live_wifi_capture_busy() && tick < 256U; ++tick) {
        unsigned previous = physical_attempts;
        (void)tick_at(201U + tick, true, CERBERUS_WIFI_READOUT_CANCEL_NONE);
        assert(physical_attempts - previous <= LIVE_WIFI_BATCH_MAX_STEPS);
        if (live_wifi_capture_busy()) assert(!memcmp(frozen, live_store->wifi, sizeof(frozen)));
        if (!interleaved && live_wifi_capture_busy() && live_store->wifi_transfer.next >= 1U &&
            live_store->wifi_transfer.next < live_store->wifi_transfer.count) {
            cerberus_c5_wifi_result_t staged = live_store->wifi_staging[0];
            prepare(SENTINEL_SCAN_BLE, 3U, CERBERUS_BLE_SCAN_MAX_RESULTS);
            assert(capture_all(SENTINEL_SCAN_BLE, 250U));
            prepare(SENTINEL_SCAN_BEACON, 4U, CERBERUS_IEEE802154_BEACON_MAX_RESULTS);
            assert(capture_all(SENTINEL_SCAN_BEACON, 250U));
            assert(!memcmp(&staged, &live_store->wifi_staging[0], sizeof(staged)));
            interleaved = true;
        }
    }
    assert(interleaved && relation_publications == 2U && !live_wifi_capture_busy());
    assert(!memcmp(live_store->wifi, wifi_source, sizeof(wifi_source)));
    assert(!memcmp(live_store->wifi_observations, wifi_timing_source, sizeof(wifi_timing_source)));

    for (unsigned phase = LIVE_WIFI_BEFORE; phase <= LIVE_WIFI_COMMIT; ++phase) {
        for (unsigned reason = 0; reason < 6U; ++reason) {
            reset(); prepare(SENTINEL_SCAN_WIFI, 1U, 2U);
            assert(capture_all(SENTINEL_SCAN_WIFI, 100U));
            memcpy(frozen, live_store->wifi, sizeof(frozen));
            prepare(SENTINEL_SCAN_WIFI, 2U, 2U); assert(live_capture(SENTINEL_SCAN_WIFI, 200U));
            for (unsigned tick = 0; live_store->wifi_transfer.phase != phase && tick < 32U; ++tick)
                (void)live_wifi_capture_tick_impl(200U, true, CERBERUS_WIFI_READOUT_CANCEL_NONE);
            assert(live_store->wifi_transfer.phase == phase);
            const cerberus_wifi_readout_diagnostics_t previous = readout();
            const uint16_t rows = live_store->wifi_transfer.next;
            if (reason == 1U) live_session.paused = true;
            if (reason == 2U) ++live_session.arm_generation;
            unsigned attempts = physical_attempts;
            if (reason == 4U) live_wifi_capture_reset();
            else if (reason == 5U) live_status(SENTINEL_SCAN_WIFI, ESP_OK,
                SENTINEL_SCAN_RUNNING, 3U, 0U, ESP_OK);
            else assert(tick_at(200U, reason != 0U && reason != 3U,
                reason == 3U ? CERBERUS_WIFI_READOUT_CANCEL_SLEEP : CERBERUS_WIFI_READOUT_CANCEL_PAGE));
            assert(!live_wifi_capture_busy() && physical_attempts == attempts);
            assert(relation_publications == 1U && !memcmp(frozen, live_store->wifi, sizeof(frozen)));
            const unsigned reasons[] = {CERBERUS_WIFI_READOUT_CANCEL_PAGE, CERBERUS_WIFI_READOUT_CANCEL_STOP,
                CERBERUS_WIFI_READOUT_CANCEL_SUPERSEDED, CERBERUS_WIFI_READOUT_CANCEL_SLEEP,
                CERBERUS_WIFI_READOUT_CANCEL_RESET, CERBERUS_WIFI_READOUT_CANCEL_SUPERSEDED};
            const cerberus_wifi_readout_diagnostics_t terminal = readout();
            assert(terminal.state == CERBERUS_WIFI_READOUT_CANCELLED && terminal.cancel_reason == reasons[reason]);
            assert(terminal.cancelled == previous.cancelled + 1U && terminal.failed == previous.failed &&
                terminal.completed == previous.completed && terminal.attempts == previous.attempts &&
                terminal.phase == phase && terminal.rows_copied == rows && terminal.error == ESP_OK);
            assert(terminal.step_calls == previous.step_calls + (reason < 4U));
            assert(!tick_at(201U, true, CERBERUS_WIFI_READOUT_CANCEL_NONE));
            assert_readout_unchanged(terminal);
            if (reason < 4U) assert(!live_capture(SENTINEL_SCAN_WIFI, 201U));
        }
    }
    reset(); prepare(SENTINEL_SCAN_WIFI, 1U, 2U);
    assert(live_capture(SENTINEL_SCAN_WIFI, UINT32_MAX - 4U));
    assert(tick_at(7995U, true, CERBERUS_WIFI_READOUT_CANCEL_NONE)); /* Exact modular deadline. */
    assert(!live_wifi_capture_busy() && live_wifi_error == ESP_ERR_TIMEOUT && !relation_publications);
    assert(readout().state == CERBERUS_WIFI_READOUT_FAILED && readout().error == ESP_ERR_TIMEOUT);
}

static void check_actual_scheduler(void)
{
    reset(); prepare(SENTINEL_SCAN_WIFI, 1U, 2U);
    cerberus_wifi_readout_diagnostics_t before = readout();
    scan_at(1000U, false);
    assert(live_wifi_capture_busy() && !start_commands && !relation_publications);
    assert(readout().attempts == before.attempts + 1U && readout().step_calls == 0U);
    for (unsigned tick = 1001U; live_wifi_capture_busy() && tick < 1256U; ++tick) {
        unsigned attempts = physical_attempts;
        scan_at(tick, false);
        assert(physical_attempts - attempts <= LIVE_WIFI_BATCH_MAX_STEPS);
    }
    assert(!live_wifi_capture_busy() && relation_publications == 1U);
    assert(live_wifi_worked && live_wifi_capture_defers_c5() && !start_commands);
    scan_at(1600U, false); /* A later polling tick releases the drained sweep. */
    assert(live_session.completed && live_session.active == SENTINEL_SCAN_NONE);
    cerberus_wifi_readout_diagnostics_t terminal = readout();
    assert(terminal.state == CERBERUS_WIFI_READOUT_COMPLETE && terminal.rows_copied == 2U &&
        terminal.completed == before.completed + 1U && terminal.phase == LIVE_WIFI_COMMIT);
    /* Explicit Scan Again sees an old COMPLETE first. It starts a new scan,
     * without relatching a readout of that old bank under the new arm. */
    sentinel_scan_session_pause(&live_session, false);
    scan_at(2200U, false);
    assert(start_commands == 1U && !live_wifi_capture_busy() &&
        live_session.active == SENTINEL_SCAN_WIFI && !live_session.completed);
    assert_readout_unchanged(terminal);

    for (unsigned exit = 0; exit < 3U; ++exit) {
        reset(); prepare(SENTINEL_SCAN_WIFI, 1U, 2U);
        scan_at(1000U, false);
        assert(live_wifi_capture_busy());
        before = readout();
        if (exit == 0U) sentinel_scan_session_pause(&live_session, true);
        if (exit == 1U) current_page = UI_PAGE_BLE;
        unsigned attempts = physical_attempts;
        scan_at(1001U, exit == 2U);
        assert(!live_wifi_capture_busy() && physical_attempts == attempts);
        assert(!relation_publications && !start_commands && !stop_commands);
        terminal = readout();
        const unsigned reasons[] = {CERBERUS_WIFI_READOUT_CANCEL_STOP,
            CERBERUS_WIFI_READOUT_CANCEL_PAGE, CERBERUS_WIFI_READOUT_CANCEL_SLEEP};
        assert(terminal.state == CERBERUS_WIFI_READOUT_CANCELLED &&
            terminal.cancel_reason == reasons[exit] && terminal.phase == LIVE_WIFI_BEFORE &&
            terminal.rows_copied == 0U && terminal.cancelled == before.cancelled + 1U &&
            terminal.step_calls == before.step_calls + 1U);
        scan_at(2000U, exit == 2U);
        assert(!live_wifi_capture_busy() && !relation_publications && !start_commands);
        assert_readout_unchanged(terminal);
    }
    reset(); prepare(SENTINEL_SCAN_WIFI, 1U, 2U); failure = FAIL_WIFI_RESULT;
    scan_at(1000U, false);
    for (unsigned tick = 1001U; live_wifi_capture_busy() && tick < 1256U; ++tick)
        scan_at(tick, false);
    assert(live_wifi_error == ESP_FAIL && !relation_publications);
    terminal = readout();
    assert(terminal.state == CERBERUS_WIFI_READOUT_FAILED && terminal.error == ESP_FAIL &&
        terminal.phase == LIVE_WIFI_ROW && terminal.rows_copied == failure_index);
    scan_at(2000U, false);
    assert(!live_wifi_capture_busy() && live_records[SENTINEL_SCAN_WIFI].error == ESP_FAIL);
    assert_readout_unchanged(terminal);
    sentinel_scan_session_pause(&live_session, false);
    scan_at(3000U, false);
    assert(start_commands == 1U && !live_wifi_capture_busy());
    assert_readout_unchanged(terminal);
}

/* Baseline is a local inspector. Exercise the actual page-tool selector and
 * scheduler, including same-desired ownership that select_snapshot alone does
 * not reconfigure. The command adapter would count any accidental RF start. */
static void check_baseline_scheduler(void)
{
    for (unsigned existing = 0U; existing < 2U; ++existing) {
        reset(); prepare(SENTINEL_SCAN_WIFI, 4U, 2U);
        if (existing) { clock_ms = 42; assert(capture_all(SENTINEL_SCAN_WIFI, 42U)); }
        const unsigned publications = relation_publications;
        const int64_t published = live_wifi_published_us;
        live_session.active = live_session.desired = SENTINEL_SCAN_NONE;
        live_session.completed = false;
        wifi_mode = UI_WIFI_MODE_BASELINE;
        for (uint32_t now = 1000U; now < 5000U; now += 500U) scan_at(now, false);
        assert(live_page_tool() == SENTINEL_SCAN_WIFI && live_session.completed);
        assert(!start_commands && !stop_commands && !live_wifi_capture_busy());
        assert(relation_publications == publications && live_wifi_published_us == published);
        assert(live_records[SENTINEL_SCAN_WIFI].captured == (existing != 0U));
        /* Sleeping and waking this inspector never rearms a scan. */
        scan_at(5000U, true); scan_at(5500U, false);
        assert(!start_commands && !stop_commands && live_session.completed);
    }
    for (unsigned ending = 0U; ending < 3U; ++ending) {
        reset(); prepare(SENTINEL_SCAN_WIFI, 5U, 2U);
        wifi_mode = UI_WIFI_MODE_BASELINE;
        wifi_scan_status.state = live_records[SENTINEL_SCAN_WIFI].state = SENTINEL_SCAN_RUNNING;
        if (ending == 2U) sentinel_scan_session_pause(&live_session, true);
        const uint32_t arm = live_session.arm_generation;
        scan_at(1000U, false); scan_at(1500U, false);
        assert(live_session.active == SENTINEL_SCAN_WIFI && live_session.completed &&
            live_session.arm_generation == arm && !start_commands && !stop_commands);
        wifi_scan_status.state = ending == 1U ? SENTINEL_SCAN_ERROR : SENTINEL_SCAN_COMPLETE;
        wifi_scan_status.error_code = ending == 1U ? ESP_FAIL : ESP_OK;
        scan_at(2000U, false);
        for (uint32_t now = 2001U; live_wifi_capture_busy() && now < 2256U; ++now)
            scan_at(now, false);
        scan_at(3000U, false); scan_at(4000U, false);
        assert(live_session.active == SENTINEL_SCAN_NONE && live_session.completed);
        assert(!start_commands && !stop_commands && !live_wifi_capture_busy());
        assert(relation_publications == (ending == 0U ? 1U : 0U));
        if (ending == 2U) assert(live_session.paused);
    }
    /* Failure, explicit Stop and sleep during a later transfer retain the old
     * bank and its full-width commit timestamp; Baseline does not retry RF. */
    for (unsigned ending = 0U; ending < 3U; ++ending) {
        reset(); prepare(SENTINEL_SCAN_WIFI, 6U, 2U); clock_ms = 51;
        assert(capture_all(SENTINEL_SCAN_WIFI, 51U));
        cerberus_c5_wifi_result_t retained[2]; memcpy(retained, live_store->wifi, sizeof(retained));
        const int64_t published = live_wifi_published_us;
        prepare(SENTINEL_SCAN_WIFI, 7U, 3U); clock_ms = 100;
        wifi_mode = UI_WIFI_MODE_BASELINE;
        if (ending == 0U) failure = FAIL_WIFI_RESULT;
        scan_at(1000U, false); assert(live_wifi_capture_busy());
        if (ending == 1U) sentinel_scan_session_pause(&live_session, true);
        scan_at(1001U, ending == 2U);
        for (uint32_t now = 1002U; live_wifi_capture_busy() && now < 1256U; ++now)
            scan_at(now, false);
        scan_at(2000U, false); scan_at(3000U, false);
        assert(!start_commands && !stop_commands && !live_wifi_capture_busy());
        assert(relation_publications == 1U && live_wifi_published_us == published);
        assert(!memcmp(retained, live_store->wifi, sizeof(retained)));
    }
}

/* Reach each phase through the unchanged production phase machine. Batches may
 * legitimately cross these local-only boundaries, so phase-specific fault
 * placement must not depend on a visible UI iteration ending there. */
static void position_wifi_phase(unsigned phase)
{
    for (unsigned steps = 0U; live_store->wifi_transfer.phase != phase && steps < 256U; ++steps) {
        const unsigned attempts = physical_attempts;
        (void)live_wifi_capture_tick_impl(100U, true, CERBERUS_WIFI_READOUT_CANCEL_NONE);
        assert(physical_attempts - attempts <= 1U);
    }
    assert(live_wifi_capture_busy() && live_store->wifi_transfer.phase == phase);
}

static void check_bounded_batches(void)
{
    assert(LIVE_WIFI_BATCH_MAX_STEPS == 4U && LIVE_WIFI_BATCH_BUDGET_US == 2000U);
    reset(); prepare(SENTINEL_SCAN_WIFI, 41U, 2U);
    clock_ms = 100; assert(live_capture(SENTINEL_SCAN_WIFI, 100U));
    const cerberus_wifi_readout_diagnostics_t before = readout();
    assert(!live_wifi_capture_tick(100U, true, CERBERUS_WIFI_READOUT_CANCEL_NONE));
    /* Three fence reads and one local begin consume four phase steps. */
    assert(physical_attempts == 3U && live_store->wifi_transfer.phase == LIVE_WIFI_ROW &&
        live_store->wifi_transfer.next == 0U && live_store->wifi_transfer.row.phase == 0U);
    assert(live_wifi_worked && live_wifi_capture_defers_c5());
    assert(readout().step_calls == before.step_calls + 1U);
    assert(!live_wifi_capture_tick(100U, true, CERBERUS_WIFI_READOUT_CANCEL_NONE));
    /* A complete row plus the next local begin fits one batch. A frozen clock
     * must not allow a fifth phase step or any part of the next row. */
    assert(physical_attempts == 6U && live_store->wifi_transfer.next == 1U &&
        live_store->wifi_transfer.phase == LIVE_WIFI_ROW && live_store->wifi_transfer.row.phase == 0U);
    assert(!relation_publications && live_wifi_worked && readout().step_calls == 2U);
    unsigned ticks = 2U;
    while (live_wifi_capture_busy() && ticks < 20U) {
        const unsigned attempts = physical_attempts;
        (void)live_wifi_capture_tick(100U, true, CERBERUS_WIFI_READOUT_CANCEL_NONE);
        assert(physical_attempts - attempts <= LIVE_WIFI_BATCH_MAX_STEPS);
        ++ticks;
    }
    assert(ticks == 5U && physical_attempts == 13U && relation_publications == 1U && refresh_calls == 1U);
    assert(readout().step_calls == ticks && readout().state == CERBERUS_WIFI_READOUT_COMPLETE);
    /* Final fence followed by local commit must retain the C5 deferral flag. */
    assert(live_wifi_worked && live_wifi_capture_defers_c5());
    cerberus_wifi_readout_diagnostics_t terminal = readout();
    assert(!live_wifi_capture_tick(101U, true, CERBERUS_WIFI_READOUT_CANCEL_NONE));
    assert(!live_wifi_worked && !live_wifi_capture_defers_c5());
    assert_readout_unchanged(terminal);

    const int64_t io_cost[] = {0, 666, 1000, 1999, 2000, 2001, 50000};
    const unsigned attempts_expected[] = {3U, 3U, 2U, 2U, 1U, 1U, 1U};
    for (unsigned i = 0U; i < sizeof(io_cost) / sizeof(io_cost[0]); ++i) {
        reset(); prepare(SENTINEL_SCAN_WIFI, 42U, 2U);
        clock_ms = 100; assert(live_capture(SENTINEL_SCAN_WIFI, 100U)); clock_io_us = io_cost[i];
        (void)live_wifi_capture_tick(100U, true, CERBERUS_WIFI_READOUT_CANCEL_NONE);
        assert(physical_attempts == attempts_expected[i] && !relation_publications && live_wifi_capture_busy());
        const cerberus_wifi_readout_diagnostics_t value = readout();
        assert(value.step_calls == 1U && value.max_step_us == (uint64_t)io_cost[i] * attempts_expected[i]);
        /* 1999us may admit another call; this is an admission budget, not a
         * preemption promise. A 2ms or slower first call admits none after it. */
        if (io_cost[i] >= LIVE_WIFI_BATCH_BUDGET_US)
            assert(live_store->wifi_transfer.fence.phase == 1U);
    }

    for (unsigned row = 0U; row < 2U; ++row) {
        reset(); prepare(SENTINEL_SCAN_WIFI, 43U, 2U);
        clock_ms = 100; assert(live_capture(SENTINEL_SCAN_WIFI, 100U));
        position_wifi_phase(row ? LIVE_WIFI_ROW : LIVE_WIFI_BEFORE);
        const unsigned attempts = physical_attempts;
        row_stalled = row != 0U; fence_stalled = !row;
        (void)live_wifi_capture_tick(100U, true, CERBERUS_WIFI_READOUT_CANCEL_NONE);
        assert(physical_attempts == attempts + 1U && live_wifi_capture_busy() && !relation_publications);
        assert(readout().step_calls == 1U && readout().max_step_us == 0U);
        /* One unchanged transport response yields immediately, even with a
         * frozen clock. A later absolute deadline still terminates it. */
        clock_ms = 8100;
        (void)live_wifi_capture_tick(8100U, true, CERBERUS_WIFI_READOUT_CANCEL_NONE);
        assert(physical_attempts == attempts + 1U && live_wifi_error == ESP_ERR_TIMEOUT);
    }
    reset(); prepare(SENTINEL_SCAN_WIFI, 44U, 2U);
    clock_ms = 100; assert(live_capture(SENTINEL_SCAN_WIFI, 100U)); position_wifi_phase(LIVE_WIFI_ROW);
    const unsigned attempts = physical_attempts;
    row_copy_only = true;
    (void)live_wifi_capture_tick(100U, true, CERBERUS_WIFI_READOUT_CANCEL_NONE);
    assert(physical_attempts == attempts + 4U && live_store->wifi_transfer.row.copied == 4U &&
        live_store->wifi_transfer.row.phase == 0U && !live_store->wifi_transfer.next);

    for (unsigned bad_clock = 0U; bad_clock < 2U; ++bad_clock) {
        reset(); prepare(SENTINEL_SCAN_WIFI, 45U, 2U);
        clock_ms = bad_clock ? 100 : -1;
        assert(live_capture(SENTINEL_SCAN_WIFI, 100U));
        clock_io_us = bad_clock ? -1 : 0;
        (void)live_wifi_capture_tick(100U, true, CERBERUS_WIFI_READOUT_CANCEL_NONE);
        assert(physical_attempts == bad_clock && !live_wifi_capture_busy() && !relation_publications &&
            live_wifi_error == ESP_ERR_INVALID_STATE);
        const cerberus_wifi_readout_diagnostics_t value = readout();
        assert(value.step_calls == 1U && !value.timing_valid && !value.elapsed_us && !value.max_step_us);
    }
    for (unsigned wrap = 0U; wrap < 2U; ++wrap) {
        reset(); prepare(SENTINEL_SCAN_WIFI, 46U, 2U);
        const uint32_t begin = wrap ? UINT32_MAX - 4U : 100U;
        clock_ms = begin;
        assert(live_capture(SENTINEL_SCAN_WIFI, begin)); clock_io_us = 1000;
        const uint32_t deadline = live_store->wifi_transfer.deadline_ms;
        clock_ms = (int64_t)begin + 7999;
        (void)live_wifi_capture_tick(deadline - 1U, true, CERBERUS_WIFI_READOUT_CANCEL_NONE);
        assert(physical_attempts == 1U && !live_wifi_capture_busy() && !relation_publications &&
            live_wifi_error == ESP_ERR_TIMEOUT && readout().state == CERBERUS_WIFI_READOUT_FAILED);
    }
    /* The caller may carry an old UI-loop timestamp. The actual clock must
     * prevent both the first exchange and an entirely local late commit. */
    const unsigned stale_phases[] = {LIVE_WIFI_BEFORE, LIVE_WIFI_COMMIT};
    for (unsigned i = 0U; i < 2U; ++i) {
        reset(); prepare(SENTINEL_SCAN_WIFI, 46U, 2U);
        clock_ms = 100; assert(live_capture(SENTINEL_SCAN_WIFI, 100U));
        position_wifi_phase(stale_phases[i]);
        const unsigned attempts_before = physical_attempts;
        clock_ms = 8100;
        assert(live_wifi_capture_tick(100U, true, CERBERUS_WIFI_READOUT_CANCEL_NONE));
        assert(physical_attempts == attempts_before && !relation_publications && !refresh_calls &&
            live_wifi_error == ESP_ERR_TIMEOUT && readout().phase == stale_phases[i]);
    }
    /* A recovered final timestamp must not hide an intermediate rollback.
     * A rollback only at the wrapper end must also fail unfinished work. */
    for (unsigned final_only = 0U; final_only < 2U; ++final_only) {
        reset(); prepare(SENTINEL_SCAN_WIFI, 46U, 2U);
        clock_ms = 100; assert(live_capture(SENTINEL_SCAN_WIFI, 100U));
        fence_stalled = true;
        const int64_t samples[][4] = {{100000, 99999, 100000, 100000},
            {100000, 100000, 99999, 100000}};
        memcpy(clock_script, samples[final_only], sizeof(samples[0])); clock_script_count = 4U;
        assert(live_wifi_capture_tick(100U, true, CERBERUS_WIFI_READOUT_CANCEL_NONE));
        assert(physical_attempts == 1U && !live_wifi_capture_busy() && !relation_publications &&
            live_wifi_error == ESP_ERR_INVALID_STATE && !readout().timing_valid &&
            readout().step_calls == 1U && readout().state == CERBERUS_WIFI_READOUT_FAILED);
    }
    /* Commit is terminal. A later bad measurement invalidates timing without
     * retroactively discarding an already validated publication. */
    reset(); prepare(SENTINEL_SCAN_WIFI, 46U, 2U);
    clock_ms = 100; assert(live_capture(SENTINEL_SCAN_WIFI, 100U));
    position_wifi_phase(LIVE_WIFI_COMMIT);
    const int64_t committed_rollback[] = {100000, 100000, 100000, 99999, 100000};
    memcpy(clock_script, committed_rollback, sizeof(committed_rollback)); clock_script_count = 5U;
    assert(live_wifi_capture_tick(100U, true, CERBERUS_WIFI_READOUT_CANCEL_NONE));
    assert(relation_publications == 1U && refresh_calls == 1U && !live_wifi_capture_busy() &&
        readout().state == CERBERUS_WIFI_READOUT_COMPLETE && !readout().timing_valid);
    /* Empty legacy result banks still validate final status before committing.
     * Its trailing local commit must not clear the prior exchange marker. */
    reset(); prepare(SENTINEL_SCAN_WIFI, 47U, 0U); relation_supported = false;
    clock_ms = 100; assert(live_capture(SENTINEL_SCAN_WIFI, 100U));
    assert(live_wifi_capture_tick(100U, true, CERBERUS_WIFI_READOUT_CANCEL_NONE));
    assert(!live_wifi_capture_busy() && physical_attempts == 1U && relation_publications == 1U &&
        live_wifi_worked && readout().rows_copied == 0U && readout().step_calls == 1U);
}

static void check_batch_boundary_cancellation(void)
{
    const unsigned phases[] = {LIVE_WIFI_BEFORE, LIVE_WIFI_ROW, LIVE_WIFI_STATUS, LIVE_WIFI_AFTER};
    for (unsigned p = 0U; p < sizeof(phases) / sizeof(phases[0]); ++p) {
        for (unsigned reason = IO_CANCEL_STOP; reason <= IO_CANCEL_GENERATION; ++reason) {
            reset(); prepare(SENTINEL_SCAN_WIFI, 48U, 2U);
            assert(capture_all(SENTINEL_SCAN_WIFI, 100U));
            cerberus_c5_wifi_result_t retained[2]; memcpy(retained, live_store->wifi, sizeof(retained));
            const int64_t published = live_wifi_published_us;
            prepare(SENTINEL_SCAN_WIFI, 49U, 2U);
            clock_ms = 100; assert(live_capture(SENTINEL_SCAN_WIFI, 100U)); position_wifi_phase(phases[p]);
            const unsigned attempts = physical_attempts;
            const uint16_t copied = live_store->wifi_transfer.next;
            io_cancel = (enum io_cancel_kind)reason; io_cancel_at = attempts + 1U;
            assert(live_wifi_capture_tick(100U, true, CERBERUS_WIFI_READOUT_CANCEL_NONE));
            assert(physical_attempts == attempts + 1U && !live_wifi_capture_busy() && live_wifi_worked);
            assert(relation_publications == 1U && live_wifi_published_us == published &&
                !memcmp(retained, live_store->wifi, sizeof(retained)));
            const cerberus_wifi_readout_diagnostics_t value = readout();
            assert(value.state == CERBERUS_WIFI_READOUT_CANCELLED && value.step_calls == 1U &&
                value.rows_copied == copied && value.cancel_reason == (reason == IO_CANCEL_STOP ?
                    CERBERUS_WIFI_READOUT_CANCEL_STOP : CERBERUS_WIFI_READOUT_CANCEL_SUPERSEDED));
            assert(!live_wifi_capture_tick(101U, true, CERBERUS_WIFI_READOUT_CANCEL_NONE));
            assert_readout_unchanged(value);
        }
    }
}

static void check_batch_error_stops(void)
{
    const unsigned phases[] = {LIVE_WIFI_BEFORE, LIVE_WIFI_ROW, LIVE_WIFI_STATUS, LIVE_WIFI_AFTER};
    for (unsigned p = 0U; p < sizeof(phases) / sizeof(phases[0]); ++p) {
        reset(); prepare(SENTINEL_SCAN_WIFI, 50U, 2U);
        assert(capture_all(SENTINEL_SCAN_WIFI, 100U));
        cerberus_c5_wifi_result_t retained[2]; memcpy(retained, live_store->wifi, sizeof(retained));
        const int64_t published = live_wifi_published_us;
        prepare(SENTINEL_SCAN_WIFI, 51U, 2U);
        clock_ms = 100; assert(live_capture(SENTINEL_SCAN_WIFI, 100U));
        position_wifi_phase(phases[p]);
        if (p == 0U) relation_before_error = ESP_ERR_INVALID_RESPONSE;
        if (p == 1U) { failure = FAIL_WIFI_RESULT; failure_index = 0U; }
        if (p == 2U) failure = FAIL_WIFI_END;
        if (p == 3U) relation_after_error = ESP_ERR_INVALID_RESPONSE;
        const unsigned attempts = physical_attempts;
        assert(live_wifi_capture_tick(100U, true, CERBERUS_WIFI_READOUT_CANCEL_NONE));
        assert(physical_attempts == attempts + (p == 1U ? 3U : 1U) &&
            !live_wifi_capture_busy() && live_wifi_worked && live_wifi_capture_defers_c5());
        assert(relation_publications == 1U && live_wifi_published_us == published &&
            !memcmp(retained, live_store->wifi, sizeof(retained)));
        const cerberus_wifi_readout_diagnostics_t value = readout();
        assert(value.state == CERBERUS_WIFI_READOUT_FAILED && value.phase == phases[p] &&
            value.step_calls == 1U && value.rows_copied == (p < 2U ? 0U : 2U));
        assert(!live_wifi_capture_tick(101U, true, CERBERUS_WIFI_READOUT_CANCEL_NONE));
        assert(!live_capture(SENTINEL_SCAN_WIFI, 101U));
        assert_readout_unchanged(value);
    }
}

/* These assertions run the production capture and its real diagnostics module.
 * Synthetic clock/I/O durations verify hook placement and accounting, not
 * physical device latency or screen/touch presentation. */
static void check_capture_readout(void)
{
    reset(); prepare(SENTINEL_SCAN_WIFI, 31U, 2U);
    clock_ms = 123; clock_advance_us = 7; clock_io_us = 311;
    clock_allocation_us = 97; clock_commit_us = 1103; inspect_allocation_readout = true;
    cerberus_wifi_readout_diagnostics_t before = readout();
    const uint64_t began = (uint64_t)clock_peek();
    assert(live_capture(SENTINEL_SCAN_WIFI, 123U));
    cerberus_wifi_readout_diagnostics_t snapshot = readout();
    assert(snapshot.state == CERBERUS_WIFI_READOUT_READING && snapshot.attempts == before.attempts + 1U &&
        snapshot.rows_total == 2U && snapshot.rows_copied == 0U && snapshot.phase == 0U &&
        snapshot.step_calls == 0U && snapshot.timing_valid && snapshot.sampled_us == began);
    uint64_t expected_max = 0U, expected_gap = 0U, prior_start = 0U;
    unsigned measured_steps = 0U;
    while (live_wifi_capture_busy()) {
        assert(measured_steps < 64U);
        clock_extra_us += 700;
        const uint64_t start = (uint64_t)clock_peek();
        const unsigned attempts = physical_attempts;
        (void)live_wifi_capture_tick(124U + measured_steps, true, CERBERUS_WIFI_READOUT_CANCEL_NONE);
        assert(physical_attempts - attempts <= LIVE_WIFI_BATCH_MAX_STEPS);
        const uint64_t finish = (uint64_t)clock_last_us;
        if (finish - start > expected_max) expected_max = finish - start;
        if (measured_steps && start - prior_start > expected_gap) expected_gap = start - prior_start;
        prior_start = start; ++measured_steps;
        snapshot = readout();
        assert(snapshot.step_calls == measured_steps && snapshot.max_step_us == expected_max &&
            snapshot.max_tick_gap_us == expected_gap && snapshot.sampled_us == finish && snapshot.timing_valid);
        if (!live_wifi_capture_busy()) {
            assert(snapshot.state == CERBERUS_WIFI_READOUT_COMPLETE && snapshot.phase == LIVE_WIFI_COMMIT &&
                snapshot.rows_copied == 2U && snapshot.elapsed_us == (uint64_t)publish_expected_terminal_us - began);
            assert(live_wifi_published_us >= (int64_t)start && live_wifi_published_us < (int64_t)finish);
            assert(snapshot.elapsed_us + began == (uint64_t)live_wifi_published_us +
                (uint64_t)clock_commit_us + (uint64_t)clock_advance_us);
            assert(snapshot.max_step_us >= (uint64_t)clock_commit_us);
        } else {
            assert(snapshot.state == CERBERUS_WIFI_READOUT_READING && snapshot.elapsed_us == finish - began &&
                snapshot.phase == live_store->wifi_transfer.phase &&
                snapshot.rows_copied == live_store->wifi_transfer.next);
        }
    }
    assert(snapshot.completed == before.completed + 1U && snapshot.failed == before.failed &&
        snapshot.cancelled == before.cancelled && snapshot.error == ESP_OK);
    clock_extra_us += 999999;
    assert(!live_wifi_capture_tick(900U, true, CERBERUS_WIFI_READOUT_CANCEL_NONE));
    assert(!live_capture(SENTINEL_SCAN_WIFI, 900U));
    assert_readout_unchanged(snapshot);

    /* OOM is an attempt with a failure timestamp, without any transfer step. */
    reset(); prepare(SENTINEL_SCAN_WIFI, 32U, 2U);
    clock_ms = 500; clock_advance_us = 7; clock_allocation_us = 97;
    inspect_allocation_readout = true; fail_allocation_call = 1U;
    before = readout();
    assert(live_capture(SENTINEL_SCAN_WIFI, 500U));
    snapshot = readout();
    assert(!live_store && !physical_attempts && snapshot.state == CERBERUS_WIFI_READOUT_FAILED &&
        snapshot.error == ESP_ERR_NO_MEM && snapshot.attempts == before.attempts + 1U &&
        snapshot.failed == before.failed + 1U && snapshot.completed == before.completed &&
        snapshot.cancelled == before.cancelled && snapshot.phase == 0U && snapshot.rows_copied == 0U &&
        snapshot.rows_total == 2U && snapshot.step_calls == 0U && snapshot.max_step_us == 0U &&
        snapshot.max_tick_gap_us == 0U && snapshot.elapsed_us == 104U && snapshot.sampled_us == 500104U);
    assert(!live_capture(SENTINEL_SCAN_WIFI, 501U));
    assert(!live_wifi_capture_tick(501U, true, CERBERUS_WIFI_READOUT_CANCEL_NONE));
    assert_readout_unchanged(snapshot);

    reset(); prepare(SENTINEL_SCAN_WIFI, 33U, CERBERUS_WIFI_SCAN_MAX_RESULTS + 1U);
    before = readout(); assert(live_capture(SENTINEL_SCAN_WIFI, 1U));
    snapshot = readout();
    assert(snapshot.state == CERBERUS_WIFI_READOUT_FAILED && snapshot.error == ESP_ERR_INVALID_RESPONSE &&
        snapshot.failed == before.failed + 1U && !snapshot.step_calls && !allocation_calls && !physical_attempts);

    /* Both proof fences, a row and final status preserve their exact failure
     * phase/progress after cancellation clears the transfer storage. */
    const unsigned phases[] = {LIVE_WIFI_BEFORE, LIVE_WIFI_ROW, LIVE_WIFI_STATUS, LIVE_WIFI_AFTER};
    const unsigned copied[] = {0U, 1U, 2U, 2U};
    for (unsigned which = 0U; which < 4U; ++which) {
        reset(); prepare(SENTINEL_SCAN_WIFI, 34U, 2U); clock_advance_us = 7; clock_io_us = 311;
        before = readout();
        if (which == 0U) relation_before_error = ESP_ERR_TIMEOUT;
        if (which == 1U) failure = FAIL_WIFI_RESULT;
        if (which == 2U) failure = FAIL_WIFI_END;
        if (which == 3U) relation_after_error = ESP_ERR_INVALID_RESPONSE;
        assert(!capture_all(SENTINEL_SCAN_WIFI, 1U));
        snapshot = readout();
        assert(snapshot.state == CERBERUS_WIFI_READOUT_FAILED && snapshot.failed == before.failed + 1U &&
            snapshot.completed == before.completed && snapshot.cancelled == before.cancelled &&
            snapshot.phase == phases[which] && snapshot.rows_copied == copied[which] && snapshot.rows_total == 2U &&
            snapshot.error == live_wifi_error && snapshot.timing_valid && snapshot.max_step_us >= 325U &&
            snapshot.elapsed_us == (uint64_t)failure_expected_terminal_us && snapshot.step_calls > 0U);
        assert(!capture_all(SENTINEL_SCAN_WIFI, 2U));
        assert_readout_unchanged(snapshot);
    }
}

int main(void)
{
    /* Lifecycle is exercised by the actual Recon composite; this suite keeps
     * all helper bodies compilable while testing atomic publication. */
    (void)live_ble_observe; (void)live_ble_execute; (void)live_ble_capture_validate;
    check_first_and_reuse();
    check_empty_and_limits();
    check_allocation_failures();
    check_failed_capture_retains_published();
    check_exact_ble_publication_fence();
    check_same_generation_cycle();
    check_relation_binding_publication();
    check_incremental_isolation_and_cancel();
    check_actual_scheduler();
    check_baseline_scheduler();
    check_bounded_batches();
    check_batch_boundary_cancellation();
    check_batch_error_stops();
    check_capture_readout();
    reset();
    printf("Production scan snapshot store: PSRAM-only %zu-byte pool; four-step/2ms admission batches, fresh absolute deadlines, clock/progress faults, publication isolation, cancellation, actual scheduler and whole-batch telemetry: pass\n", sizeof(cerberus_scan_display_store_t));
    return 0;
}
