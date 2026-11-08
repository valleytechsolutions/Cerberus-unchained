#pragma once
/* A saved address only selects a fresh passive search. Keep the original
 * document/selection intact until discovery and any subsequent Fox Hunt end. */
#include "saved_ble_lookup.h"
#include "ble_rssi.h"

typedef struct {
    cerberus_saved_ble_lookup_t lookup;
    cerberus_saved_ble_lookup_candidate_t painted[4], selected;
    airtag_json_ui_store_t *json_origin;
    ble_archive_ui_store_t *archive_origin;
    uint64_t now_ms, paint_ms;
    ui_ble_mode_t return_mode;
    esp_err_t error;
    uint8_t address[6], page, painted_count;
    bool json, start_pending, barrier, closing, handoff, fox, started, painted_valid;
} saved_ble_lookup_ui_store_t;
static saved_ble_lookup_ui_store_t *saved_ble_lookup_ui;
static bool saved_ble_lookup_opened, saved_ble_lookup_sleep_retained;
static esp_err_t saved_ble_lookup_open_error;

static bool saved_ble_lookup_is_open(void)
{ return saved_ble_lookup_opened && (!saved_ble_lookup_ui || !saved_ble_lookup_ui->fox); }
static bool saved_ble_lookup_holds_files(void)
{ return saved_ble_lookup_opened; }
static bool saved_ble_lookup_ui_busy(void)
{ return saved_ble_lookup_ui && !cerberus_saved_ble_lookup_released(&saved_ble_lookup_ui->lookup); }

static void saved_ble_lookup_free(void)
{
    heap_caps_free(saved_ble_lookup_ui); saved_ble_lookup_ui = NULL;
    saved_ble_lookup_opened = false;
}

static bool saved_ble_lookup_origin_valid(const saved_ble_lookup_ui_store_t *s)
{
    if (s->json) return airtag_json_view == s->json_origin && airtag_json_opened &&
        airtag_json_view && airtag_json_view->has_document && airtag_json_view->selected_valid &&
        airtag_json_view->view == AIRTAG_JSON_RAW &&
        !memcmp(airtag_json_view->selected.address, s->address, 6U) &&
        ble_archive_view == s->archive_origin;
    return ble_archive_view == s->archive_origin && ble_archive_view &&
        ble_archive_view->has_document && ble_archive_view->selected_valid &&
        ble_archive_view->view == BLE_ARCHIVE_UI_DETAIL &&
        !memcmp(ble_archive_view->selected.address, s->address, 6U);
}

static void saved_ble_lookup_open(bool json)
{
    if (saved_ble_lookup_opened || current_page != UI_PAGE_BLE_ARCHIVE) return;
    const uint8_t *address;
    if (json) {
        if (!airtag_json_opened || !airtag_json_view || !airtag_json_view->has_document ||
            !airtag_json_view->selected_valid || airtag_json_view->job_pending ||
            airtag_json_view->view != AIRTAG_JSON_RAW) return;
        address = airtag_json_view->selected.address;
    } else {
        if (!ble_archive_view || !ble_archive_view->has_document || !ble_archive_view->selected_valid ||
            ble_archive_view->job_pending || ble_archive_view->release_pending || ble_archive_menu ||
            ble_archive_view->view != BLE_ARCHIVE_UI_DETAIL) return;
        address = ble_archive_view->selected.address;
    }
    saved_ble_lookup_ui = heap_caps_calloc(1U, sizeof(*saved_ble_lookup_ui), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    saved_ble_lookup_open_error = saved_ble_lookup_ui ? ESP_OK : ESP_ERR_NO_MEM;
    saved_ble_lookup_opened = true; saved_ble_lookup_sleep_retained = false;
    if (saved_ble_lookup_ui) {
        saved_ble_lookup_ui_store_t *s = saved_ble_lookup_ui;
        s->json = json; s->json_origin = airtag_json_view; s->archive_origin = ble_archive_view;
        s->return_mode = ble_mode;
        memcpy(s->address, address, 6U);
        const cerberus_dual_ble_ops_t ops = {NULL, dual_ble_ui_exchange, dual_ble_ui_clock};
        cerberus_saved_ble_lookup_init(&s->lookup, &ops);
    }
    ESP_ERROR_CHECK(draw_detail_page());
}

static bool saved_ble_lookup_previous_released(void)
{
    cerberus_dual_wifi_status_t wifi;
    cerberus_dual_wifi_scan_get_status(&wifi);
    return dual_wifi_live_released() && !dual_wifi_ui_busy(&wifi) &&
        cerberus_s3_ble_host_owner() == CERBERUS_S3_BLE_OWNER_NONE;
}

static void saved_ble_lookup_cancel(saved_ble_lookup_ui_store_t *s, bool close)
{
    if (!s) return;
    s->start_pending = s->handoff = s->painted_valid = false;
    s->painted_count = 0U; s->closing |= close;
    cerberus_saved_ble_lookup_stop(&s->lookup);
}

static void saved_ble_lookup_tap(uint16_t x, uint16_t y)
{
    if (x < 8U || x >= 312U) return;
    saved_ble_lookup_ui_store_t *s = saved_ble_lookup_ui;
    if (y >= 426U && y < 470U) {
        if (!s) saved_ble_lookup_free();
        else saved_ble_lookup_cancel(s, true);
        ESP_ERROR_CHECK(draw_detail_page()); return;
    }
    if (!s || s->closing || s->fox) return;
    if (y >= 366U && y < 408U) {
        if (x >= 84U && x < 236U) {
            cerberus_saved_ble_lookup_view_t view;
            cerberus_saved_ble_lookup_view(&s->lookup, s->now_ms, &view);
            if (s->start_pending || s->barrier || s->handoff || view.enabled || view.stopping)
                saved_ble_lookup_cancel(s, false);
            else {
                s->start_pending = s->barrier = true; s->error = ESP_OK;
                s->painted_valid = false; s->painted_count = 0U;
                ble_discovery_leave(); dual_wifi_live_cancel_handoff(); dual_wifi_ui_leave();
                sentinel_scan_session_select(&live_session, SENTINEL_SCAN_NONE);
                sentinel_scan_session_select(&live_ble_session, SENTINEL_SCAN_NONE);
            }
        } else if (!s->handoff && !s->barrier && ((x >= 8U && x < 76U) || x >= 244U)) {
            cerberus_saved_ble_lookup_view_t view;
            cerberus_saved_ble_lookup_view(&s->lookup, s->now_ms, &view);
            if (x < 76U && s->page) --s->page;
            else if (x >= 244U && (s->page + 1U) * 4U < view.count) ++s->page;
            s->painted_valid = false; s->painted_count = 0U;
        }
    } else if (!s->barrier && !s->handoff && s->painted_valid && y >= 144U && y < 328U) {
        unsigned row = (y - 144U) / 46U;
        if ((y - 144U) % 46U >= 42U || row >= s->painted_count) return;
        int64_t clock = esp_timer_get_time();
        if (clock < 0 || !cerberus_saved_ble_lookup_accepts(&s->lookup, &s->painted[row], (uint64_t)clock / 1000U)) {
            s->error = ESP_ERR_NOT_FOUND;
        } else {
            s->selected = s->painted[row]; s->handoff = true; s->error = ESP_OK;
            s->painted_valid = false; s->painted_count = 0U;
            cerberus_saved_ble_lookup_stop(&s->lookup);
        }
    }
    ESP_ERROR_CHECK(draw_detail_page());
}

/* Called only after the existing Fox controller proves both exact owners are
 * released. A hidden old lookup cannot return over a newer screen. */
static bool saved_ble_lookup_fox_return(bool asleep)
{
    saved_ble_lookup_ui_store_t *s = saved_ble_lookup_ui;
    if (!s || !s->fox) return false;
    bool restore = current_page == UI_PAGE_BLE && ble_mode == UI_BLE_MODE_FOX_HUNT &&
        saved_ble_lookup_origin_valid(s);
    ui_ble_mode_t return_mode = s->return_mode;
    saved_ble_lookup_free();
    if (restore) {
        current_page = UI_PAGE_BLE_ARCHIVE; ble_mode = return_mode;
        saved_ble_lookup_sleep_retained = asleep;
        if (!asleep) ESP_ERROR_CHECK(draw_detail_page());
    } else if (current_page == UI_PAGE_BLE && ble_mode == UI_BLE_MODE_FOX_HUNT) {
        /* A corrupted/replaced origin must not leave the coordinator seeing
         * an active Fox page after its final release. */
        ble_mode = UI_BLE_MODE_MENU;
        if (!asleep) ESP_ERROR_CHECK(draw_detail_page());
    }
    return true;
}

static bool saved_ble_lookup_tick(bool asleep)
{
    if (!saved_ble_lookup_opened) {
        /* Canceling on sleep keeps the selected record until wake, including
         * JSON's ordinarily disposable offline page. No RF intent survives. */
        if (saved_ble_lookup_sleep_retained && asleep && current_page == UI_PAGE_BLE_ARCHIVE) return true;
        saved_ble_lookup_sleep_retained = false; return false;
    }
    saved_ble_lookup_ui_store_t *s = saved_ble_lookup_ui;
    bool visible = current_page == UI_PAGE_BLE_ARCHIVE;
    if (!s) {
        if (asleep || !visible) { saved_ble_lookup_free(); saved_ble_lookup_sleep_retained = asleep && visible; }
        return true;
    }
    if (s->fox) return true;
    int64_t clock = esp_timer_get_time();
    if (clock >= 0) s->now_ms = (uint64_t)clock / 1000U;
    else s->error = ESP_ERR_INVALID_STATE;
    if (asleep || !visible || clock < 0 || !saved_ble_lookup_origin_valid(s)) saved_ble_lookup_cancel(s, true);
    if (s->barrier && saved_ble_lookup_previous_released()) s->barrier = false;
    if (s->start_pending && !s->barrier && !s->closing) {
        s->start_pending = false;
        if (cerberus_saved_ble_lookup_start(&s->lookup, s->address)) { s->started = true; s->page = 0U; }
        else s->error = ESP_ERR_INVALID_STATE;
    }
    cerberus_saved_ble_lookup_tick(&s->lookup, s->now_ms);
    bool released = cerberus_saved_ble_lookup_released(&s->lookup);
    if (s->closing && !s->barrier && released) {
        saved_ble_lookup_free(); saved_ble_lookup_sleep_retained = asleep && visible;
        if (visible && !asleep) ESP_ERROR_CHECK(draw_detail_page());
        return true;
    }
    if (s->handoff && released) {
        s->handoff = false;
        if (!cerberus_saved_ble_lookup_accepts(&s->lookup, &s->selected, s->now_ms)) s->error = ESP_ERR_NOT_FOUND;
        else {
            cerberus_c5_ble_result_t peer = s->selected.peer;
            peer.rssi = CERBERUS_BLE_ARCHIVE_RSSI_UNAVAILABLE;
            if (ble_fox_hunt_open_peer(&peer, s->selected.source == 2U)) {
                s->fox = true; current_page = UI_PAGE_BLE;
                ESP_ERROR_CHECK(draw_detail_page()); return true;
            }
            s->error = ESP_ERR_INVALID_STATE;
        }
    }
    if (visible && !asleep && (s->now_ms < s->paint_ms || s->now_ms - s->paint_ms >= 250U)) {
        s->paint_ms = s->now_ms; ESP_ERROR_CHECK(draw_detail_page());
    }
    return true;
}

static esp_err_t saved_ble_lookup_draw(void)
{
    saved_ble_lookup_ui_store_t *s = saved_ble_lookup_ui;
    if (s) { s->painted_valid = false; s->painted_count = 0U; }
    ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(0, 0, 320, 480, UI_BACKGROUND), TAG, "saved lookup clear");
    ESP_RETURN_ON_ERROR(draw_header("FIND SAVED ADDRESS"), TAG, "saved lookup title");
    if (!s) {
        ESP_RETURN_ON_ERROR(draw_centered(154, "LOOKUP WORKSPACE UNAVAILABLE", UI_TEXT, 1), TAG, "lookup allocation");
        ESP_RETURN_ON_ERROR(draw_centered(178, esp_err_to_name(saved_ble_lookup_open_error), UI_BAD, 1), TAG, "lookup allocation error");
        return draw_button(8, 426, 304, 44, "BACK TO RECORD");
    }
    char text[64], address[18];
    cerberus_privacy_format_ble_identifier(s->address, privacy_addresses_masked(), address, sizeof(address));
    ESP_RETURN_ON_ERROR(draw_centered(56, address, UI_TEXT, 2), TAG, "lookup address");
    ESP_RETURN_ON_ERROR(draw_centered(78, "FILE DATA STAYS HISTORICAL / RECEIVE ONLY", UI_MUTED, 1), TAG, "lookup scope");
    cerberus_saved_ble_lookup_view_t view;
    cerberus_saved_ble_lookup_view(&s->lookup, s->now_ms, &view);
    for (unsigned i = 0; i < 2U; ++i) {
        const cerberus_saved_ble_lookup_source_t *source = &view.source[i];
        const char *state = s->barrier ? "WAITING FOR PREVIOUS OWNER" : source->error ? esp_err_to_name(source->error) :
            source->receiving ? "RECEIVING" : source->attached ? "RELEASING / STARTING" : view.enabled ? "WAITING" : "STOPPED";
        snprintf(text, sizeof(text), "%s: %.29s / %lu SWEEPS", i ? "C5" : "S3", state, (unsigned long)source->sweeps);
        ESP_RETURN_ON_ERROR(draw_centered((uint16_t)(98U + i * 16U), text, source->error ? UI_BAD : UI_MUTED, 1), TAG, "lookup source state");
    }
    ESP_RETURN_ON_ERROR(draw_centered(132, s->closing ? "STOPPING / RETURNING TO RECORD" : s->handoff ?
        "RELEASING BOTH / FOX HUNT NEXT" : "TAP A FRESH ROW TO OPEN FOX HUNT", UI_TEXT, 1), TAG, "lookup selection");
    unsigned pages = view.count ? (view.count + 3U) / 4U : 1U;
    if (s->page >= pages) s->page = 0U;
    for (uint8_t row = 0U; row < 4U && s->page * 4U + row < view.count; ++row) {
        cerberus_saved_ble_lookup_candidate_t candidate;
        if (!cerberus_saved_ble_lookup_candidate(&s->lookup, (uint8_t)(s->page * 4U + row), s->now_ms, &candidate)) break;
        uint16_t y = (uint16_t)(144U + row * 46U);
        ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(8, y, 304, 42, UI_PANEL), TAG, "lookup row");
        static const char *types[] = {"PUBLIC", "RANDOM", "PUBLIC ID", "RANDOM ID"};
        const char *type = candidate.peer.address_type < 4U ? types[candidate.peer.address_type] : "UNKNOWN";
        snprintf(text, sizeof(text), "%s / TYPE %u %s / %s", candidate.source == 1U ? "S3" : "C5",
            candidate.peer.address_type, type, candidate.fresh ? "FRESH" : "STALE");
        ESP_RETURN_ON_ERROR(cerberus_display_draw_text(16, y + 6U, text, candidate.fresh ? UI_GOOD : UI_MUTED, UI_PANEL, 1), TAG, "lookup row identity");
        if (!cerberus_ble_rssi_valid(candidate.peer.rssi)) snprintf(text, sizeof(text), "RSSI UNAVAILABLE / FRESH ADV ONLY");
        else if (candidate.age_ms == UINT32_MAX) snprintf(text, sizeof(text), "%d DBM %s / ADV AGE UNKNOWN", candidate.peer.rssi,
            candidate.source == 1U ? "SWEEP PEAK" : "ORIGINAL ADV");
        else snprintf(text, sizeof(text), "%d DBM %s / ADV AGE %s%lu.%03luS", candidate.peer.rssi,
            candidate.source == 1U ? "SWEEP PEAK" : "ORIGINAL", candidate.source == 2U ? "<=" : "",
            (unsigned long)(candidate.age_ms / 1000U), (unsigned long)(candidate.age_ms % 1000U));
        ESP_RETURN_ON_ERROR(cerberus_display_draw_text(16, y + 25U, text, UI_MUTED, UI_PANEL, 1), TAG, "lookup row signal");
        s->painted[row] = candidate; ++s->painted_count;
    }
    if (!view.count) ESP_RETURN_ON_ERROR(draw_centered(222, s->started ? "NO MATCHING ADDRESS OBSERVED" : "START A FRESH PASSIVE SEARCH", UI_MUTED, 1), TAG, "lookup empty");
    snprintf(text, sizeof(text), "%u CANDIDATES / PAGE %u/%u", view.count, s->page + 1U, pages);
    ESP_RETURN_ON_ERROR(draw_centered(336, text, UI_MUTED, 1), TAG, "lookup count");
    if (s->error == ESP_ERR_NOT_FOUND) snprintf(text, sizeof(text), "OBSERVATION EXPIRED / SELECT A FRESH ROW");
    else if (s->error) snprintf(text, sizeof(text), "LOOKUP: %.42s", esp_err_to_name(s->error));
    else snprintf(text, sizeof(text), "MATCHING ADDRESS DOES NOT PROVE DEVICE ID");
    ESP_RETURN_ON_ERROR(draw_centered(351, text, s->error ? UI_BAD : UI_MUTED, 1), TAG, "lookup note");
    ESP_RETURN_ON_ERROR(draw_button(8, 366, 68, 42, "PREV"), TAG, "lookup previous");
    ESP_RETURN_ON_ERROR(draw_button(84, 366, 152, 42, s->closing || s->handoff || view.stopping ? "STOPPING" :
        s->barrier || view.enabled ? "STOP" : "START"), TAG, "lookup control");
    ESP_RETURN_ON_ERROR(draw_button(244, 366, 68, 42, "NEXT"), TAG, "lookup next");
    ESP_RETURN_ON_ERROR(draw_button(8, 426, 304, 44, "BACK TO RECORD"), TAG, "lookup back");
    s->painted_valid = !s->closing && !s->handoff && !s->barrier;
    return ESP_OK;
}
