/* Included by ui.c after its draw helpers. This companion view deliberately
 * does not insert C5 packets into S3's evidence or action namespace. Archives
 * retain their receiver identity and the provenance frozen with each row. */
#pragma once
#include "dual_ble_scan.h"
#include "dual_ble_list.h"
#include "esp_heap_caps.h"
#include "ble_receive_info_ui.h"
#include "dual_ble_hint_ui.h"

enum { DUAL_BLE_UI_NONE, DUAL_BLE_UI_REDRAW, DUAL_BLE_UI_BACK_S3,
    DUAL_BLE_UI_TOGGLE, DUAL_BLE_UI_FOX, DUAL_BLE_UI_BACK_MONITOR,
    DUAL_BLE_UI_SELECT, DUAL_BLE_UI_RETRY_S3, DUAL_BLE_UI_RETRY_C5 };
static cerberus_dual_ble_scan_t *dual_ble_ui_store;
static cerberus_dual_ble_row_t dual_ble_ui_painted[4], dual_ble_ui_selected;
static cerberus_dual_ble_provenance_t dual_ble_ui_painted_provenance[4], dual_ble_ui_selected_provenance;
static uint8_t dual_ble_ui_painted_count, dual_ble_ui_offset;
static uint8_t dual_ble_ui_visible_count;
static cerberus_dual_ble_sort_t dual_ble_ui_sort = CERBERUS_DUAL_BLE_SORT_SIGNAL;
static bool dual_ble_ui_recent_only;
static uint8_t dual_ble_ui_target[6], dual_ble_ui_type;
static uint8_t dual_ble_ui_family;
/* Explicit session preference; boot/default stays passive. */
static bool dual_ble_ui_response_requests, dual_ble_ui_response_mode;
static bool dual_ble_ui_response_tab;
static int dual_ble_ui_mode = -1;
static bool dual_ble_ui_attempted, dual_ble_ui_detail;
static bool dual_ble_ui_receive_info;
static bool dual_ble_ui_hints;
static uint8_t dual_ble_ui_hint_page, dual_ble_ui_hint_pages;
static bool dual_ble_ui_from_unified, dual_ble_ui_from_monitor;
static bool dual_ble_ui_selection_pending;
static esp_err_t dual_ble_ui_error;

/* Cancel deferred navigation only. A frozen selected proof is deliberately
 * retained; neither this hook nor selection performs owner/transport work. */
static void dual_ble_ui_cancel_selection(void)
{
    dual_ble_ui_selection_pending = false;
    dual_ble_ui_painted_count = 0U;
}

static uint64_t dual_ble_ui_clock(void *context)
{ (void)context; return (uint64_t)(esp_timer_get_time() / 1000LL); }

static esp_err_t dual_ble_ui_exchange(void *context, uint8_t command,
    const void *request, size_t request_size, void *response, size_t response_size)
{
    (void)context;
    return c5_spi_link_dual_ble_exchange(command, request, request_size, response, response_size);
}

static void dual_ble_ui_retry(void)
{
    dual_ble_ui_attempted = false; dual_ble_ui_error = ESP_OK;
    if (dual_ble_ui_store) cerberus_dual_ble_stop(dual_ble_ui_store);
}

/* Cancel only our desired session and pending reservation. No exchange or
 * clock read here: the single global coordinator tick drains any owned job.
 * Repeated hidden-page cleanup must not reset the controller's retry delay. */
static void dual_ble_ui_cancel(void)
{
    bool changed = dual_ble_ui_mode != -1 || dual_ble_ui_family != 0U ||
        dual_ble_ui_response_mode;
    dual_ble_ui_mode = -1;
    dual_ble_ui_family = 0U;
    dual_ble_ui_response_mode = false;
    dual_ble_ui_attempted = false;
    if (changed) dual_ble_ui_error = ESP_OK;
    if (dual_ble_ui_store && (changed ||
        (!dual_ble_ui_store->stop_requested &&
         (dual_ble_ui_store->enabled || dual_ble_ui_store->start.job_id))))
        cerberus_dual_ble_stop(dual_ble_ui_store);
}

/* Family is part of the desired owner binding. Switching it drains the old
 * C5 job before starting another; retained/painted rows stay immutable. Mixed
 * Recon/Wardriving retain their existing radio roles. */
static void dual_ble_ui_sync_request(int mode, uint8_t family,
    const uint8_t address[6], uint8_t type, uint64_t now)
{
    if (mode < -1 || mode > CERBERUS_DUAL_BLE_FAMILY ||
        (mode == CERBERUS_DUAL_BLE_TRACK && (!address || type > 3U)) ||
        (mode == CERBERUS_DUAL_BLE_FAMILY && (!family || family > 6U))) mode = -1;
    if (mode < 0) {
        dual_ble_ui_cancel();
        if (dual_ble_ui_store) cerberus_dual_ble_tick(dual_ble_ui_store, now);
        return;
    }
    if (mode != CERBERUS_DUAL_BLE_FAMILY) family = 0U;
    bool response = dual_ble_ui_response_requests &&
        (mode == CERBERUS_DUAL_BLE_DISCOVERY || mode == CERBERUS_DUAL_BLE_FAMILY);
    bool changed = response != dual_ble_ui_response_mode || mode != dual_ble_ui_mode || (mode == CERBERUS_DUAL_BLE_TRACK &&
        (type != dual_ble_ui_type || memcmp(address, dual_ble_ui_target, 6U))) ||
        family != dual_ble_ui_family;
    if (changed) {
        /* Changing discovery preferences is not Retry. Keep a failed source
         * latched while remembering the configuration an explicit Retry will
         * use; healthy sources still drain and rebind normally. */
        esp_err_t previous_error = dual_ble_ui_error ? dual_ble_ui_error :
            dual_ble_ui_store ? dual_ble_ui_store->error : ESP_OK;
        bool retain_failure = dual_ble_ui_attempted && previous_error != ESP_OK &&
            (dual_ble_ui_mode == CERBERUS_DUAL_BLE_DISCOVERY || dual_ble_ui_mode == CERBERUS_DUAL_BLE_FAMILY) &&
            (mode == CERBERUS_DUAL_BLE_DISCOVERY || mode == CERBERUS_DUAL_BLE_FAMILY);
        dual_ble_ui_mode = mode; dual_ble_ui_attempted = retain_failure;
        dual_ble_ui_error = retain_failure ? previous_error : ESP_OK;
        dual_ble_ui_family = family; dual_ble_ui_response_mode = response;
        if (mode == CERBERUS_DUAL_BLE_TRACK) {
            memcpy(dual_ble_ui_target, address, 6U); dual_ble_ui_type = type;
        }
        if (dual_ble_ui_store) cerberus_dual_ble_stop(dual_ble_ui_store);
    }
    if (mode >= 0 && !dual_ble_ui_store && !dual_ble_ui_attempted) {
        dual_ble_ui_store = heap_caps_calloc(1U, sizeof(*dual_ble_ui_store), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!dual_ble_ui_store) { dual_ble_ui_error = ESP_ERR_NO_MEM; dual_ble_ui_attempted = true; }
        else {
            const cerberus_dual_ble_ops_t ops = {NULL, dual_ble_ui_exchange, dual_ble_ui_clock};
            cerberus_dual_ble_init(dual_ble_ui_store, &ops);
        }
    }
    if (!dual_ble_ui_store) return;
    cerberus_dual_ble_tick(dual_ble_ui_store, now);
    if (mode >= 0 && !dual_ble_ui_attempted && cerberus_dual_ble_released(dual_ble_ui_store)) {
        dual_ble_ui_attempted = true;
        dual_ble_ui_error = response ? cerberus_dual_ble_begin_response(dual_ble_ui_store, family) :
            mode == CERBERUS_DUAL_BLE_FAMILY ?
            cerberus_dual_ble_begin_family(dual_ble_ui_store, family) :
            cerberus_dual_ble_begin(dual_ble_ui_store, (uint8_t)mode,
            mode == CERBERUS_DUAL_BLE_TRACK ? dual_ble_ui_target : NULL,
            mode == CERBERUS_DUAL_BLE_TRACK ? dual_ble_ui_type : 0U);
    }
}

static void dual_ble_ui_sync(int mode, const uint8_t address[6], uint8_t type, uint64_t now)
{ dual_ble_ui_sync_request(mode, 0U, address, type, now); }

static void dual_ble_ui_sync_family(uint8_t family, uint64_t now)
{
    dual_ble_ui_sync_request(family ? CERBERUS_DUAL_BLE_FAMILY : CERBERUS_DUAL_BLE_DISCOVERY,
        family, NULL, 0U, now);
}

static void dual_ble_ui_get_view(cerberus_dual_ble_view_t *out)
{
    cerberus_dual_ble_view(dual_ble_ui_store, out);
    if (out && dual_ble_ui_error) out->error = dual_ble_ui_error;
}

static bool dual_ble_ui_released(void)
{ return cerberus_dual_ble_released(dual_ble_ui_store); }

static bool dual_ble_ui_apply_selection(void)
{
    if (!dual_ble_ui_selection_pending || !dual_ble_live_discovery_released() ||
        !dual_ble_ui_released()) return false;
    dual_ble_ui_selection_pending = false;
    dual_ble_ui_detail = true;
    return true;
}

/* A local cached view only: rendering never advances the owner or does SPI. */
static inline void dual_ble_ui_get_analyzer_view(cerberus_dual_ble_analyzer_view_t *out)
{
    cerberus_dual_ble_analyzer_view(dual_ble_ui_store, out);
    if (out && dual_ble_ui_error) out->error = dual_ble_ui_error;
}

static void dual_ble_ui_open(bool reset_detail)
{
    dual_ble_ui_cancel_selection();
    dual_ble_ui_from_unified = false;
    dual_ble_ui_from_monitor = false;
    if (reset_detail) {
        dual_ble_ui_detail = false; dual_ble_ui_receive_info = false; dual_ble_ui_hints = false;
        dual_ble_ui_hint_page = dual_ble_ui_hint_pages = 0U; dual_ble_ui_response_tab = false;
    }
}

static void dual_ble_ui_select_frozen_from(const cerberus_dual_ble_row_t *row,
    const cerberus_dual_ble_provenance_t *provenance, bool from_monitor)
{
    /* A UI-owned copy of exactly the displayed source proof. No scanner or
     * clock query may replace its identity, payload or receive metadata. */
    dual_ble_ui_selected = *row;
    dual_ble_ui_selected_provenance = *provenance;
    dual_ble_ui_selection_pending = false;
    dual_ble_ui_detail = true;
    dual_ble_ui_receive_info = false;
    dual_ble_ui_hints = false; dual_ble_ui_hint_page = dual_ble_ui_hint_pages = 0U; dual_ble_ui_response_tab = false;
    dual_ble_ui_from_unified = true;
    dual_ble_ui_from_monitor = from_monitor;
}

static void dual_ble_ui_select_frozen(const cerberus_dual_ble_row_t *row,
    const cerberus_dual_ble_provenance_t *provenance)
{ dual_ble_ui_select_frozen_from(row, provenance, false); }

static bool dual_ble_ui_selected_peer(cerberus_c5_ble_result_t *out)
{
    if (!out || !dual_ble_ui_detail) return false;
    memset(out, 0, sizeof(*out));
    memcpy(out->address, dual_ble_ui_selected.address, 6U);
    out->address_type = dual_ble_ui_selected.address_type;
    if (cerberus_dual_ble_list_response_name(&dual_ble_ui_selected))
        cerberus_dual_ble_list_label(&dual_ble_ui_selected, out->name);
    else memcpy(out->name, dual_ble_ui_selected.name, sizeof(out->name));
    out->name[sizeof(out->name) - 1U] = '\0';
    out->rssi = dual_ble_ui_selected.rssi;
    out->manufacturer_id = dual_ble_ui_selected.manufacturer_id;
    /* Address and display fields only: no connection or sound eligibility. */
    return true;
}

/* This is the envelope copied with the selected original, never current scan
 * configuration or aggregate peer hints. Drawing does no radio/transport work. */
static esp_err_t dual_ble_ui_draw_receive(const cerberus_dual_ble_row_t *row)
{
    return ble_receive_info_ui_draw(&row->receive, row->raw_length, 230U);
}

static void dual_ble_ui_label(const cerberus_dual_ble_row_t *row,
    char out[CERBERUS_BLE_NAME_MAX_LENGTH + 1U])
{
    if (!row->name[0] && !cerberus_dual_ble_list_response_name(row) && privacy_addresses_masked())
        memcpy(out, "Unnamed device", sizeof("Unnamed device"));
    else cerberus_dual_ble_list_label(row, out);
}

static esp_err_t dual_ble_ui_draw(bool requested)
{
    char line[64], identifier[24], name[CERBERUS_BLE_NAME_MAX_LENGTH + 1U];
    uint8_t painted_count = 0U;
    uint8_t hint_pages = 0U;
    dual_ble_ui_painted_count = 0U;
    dual_ble_ui_hint_pages = 0U;
    const uint64_t now = dual_ble_ui_clock(NULL);
    cerberus_dual_ble_view_t view; dual_ble_ui_get_view(&view);
    ESP_RETURN_ON_ERROR(draw_small_button(8, 50, dual_ble_ui_detail ? 304 : 148, 30,
        dual_ble_ui_from_monitor ? "BACK TO MONITOR" :
        dual_ble_ui_detail ? "BACK TO BLE LIST" : "BACK TO BLE"), TAG, "C5 BLE back");
    if (!dual_ble_ui_detail)
        ESP_RETURN_ON_ERROR(draw_small_button(164, 50, 148, 30,
            dual_ble_ui_response_requests ? "RESPONSE REQS >" : "PASSIVE >"), TAG, "C5 acquisition");
    if (dual_ble_ui_detail) {
        const cerberus_dual_ble_row_t *row = &dual_ble_ui_selected;
        dual_ble_ui_label(row, name);
        snprintf(line, sizeof(line), cerberus_dual_ble_list_response_name(row) ? "C5 RSP NAME / %s" : "C5 / %s", name);
        ESP_RETURN_ON_ERROR(draw_centered(96, line, UI_TEXT, 1), TAG, "C5 BLE name");
        cerberus_privacy_format_ble_identifier(row->address, privacy_addresses_masked(), identifier, sizeof(identifier));
        ESP_RETURN_ON_ERROR(draw_centered(118, identifier, UI_TEXT, 1), TAG, "C5 BLE address");
        snprintf(line, sizeof(line), "TYPE %u / ORIGINAL ADV REPORTS %u", row->address_type, row->reports);
        ESP_RETURN_ON_ERROR(draw_centered(140, line, UI_MUTED, 1), TAG, "C5 BLE type");
        const bool response_present = cerberus_dual_ble_response_present(row, &dual_ble_ui_selected_provenance);
        uint32_t age;
        bool known_age = dual_ble_ui_response_tab ? response_present &&
            cerberus_dual_ble_response_age(row, now, &age) : cerberus_dual_ble_list_age(row, now, &age);
        if (dual_ble_ui_response_tab) {
            if (response_present && (row->response.flags & CERBERUS_DUAL_BLE_RESPONSE_RSSI_VALID))
                snprintf(line, sizeof(line), "RSP %d dBm", row->response.rssi);
            else snprintf(line, sizeof(line), "RSP RSSI N/A");
        } else if (cerberus_dual_ble_list_rssi_valid(row)) snprintf(line, sizeof(line), "LAST ADV %d dBm", row->rssi);
        else snprintf(line, sizeof(line), "LAST ADV RSSI N/A");
        size_t used = strlen(line);
        if (known_age) snprintf(line + used, sizeof(line) - used, " / AGE <= %lu ms%s", (unsigned long)age,
            age > CERBERUS_DUAL_BLE_RECENT_MS ? " / RECENCY?" : "");
        else snprintf(line + used, sizeof(line) - used, " / AGE UNKNOWN");
        ESP_RETURN_ON_ERROR(draw_centered(162, line, UI_MUTED, 1), TAG, "C5 BLE timing");
        ESP_RETURN_ON_ERROR(draw_small_button(8, 174, 148, 22,
            dual_ble_ui_response_tab ? "ORIGINAL ADV" : "ORIGINAL ADV *"), TAG, "C5 original source");
        ESP_RETURN_ON_ERROR(draw_small_button(164, 174, 148, 22,
            dual_ble_ui_response_tab ? "MATCHED RSP *" : "MATCHED RSP"), TAG, "C5 response source");
        ESP_RETURN_ON_ERROR(draw_small_button(8, 198, 96, 26,
            !dual_ble_ui_receive_info && !dual_ble_ui_hints ?
                (dual_ble_ui_response_tab ? "RAW RSP *" : "RAW ADV *") : (dual_ble_ui_response_tab ? "RAW RSP" : "RAW ADV")), TAG, "C5 raw tab");
        ESP_RETURN_ON_ERROR(draw_small_button(112, 198, 96, 26,
            dual_ble_ui_receive_info ? "RX INFO *" : "RX INFO"), TAG, "C5 metadata tab");
        ESP_RETURN_ON_ERROR(draw_small_button(216, 198, 96, 26,
            dual_ble_ui_hints ? "HINTS *" : "HINTS"), TAG, "C5 hint tab");
        if (dual_ble_ui_response_tab && !response_present) {
            bool known_none = row->response.state == CERBERUS_DUAL_BLE_RESPONSE_NONE &&
                dual_ble_ui_selected_provenance.boot_id && dual_ble_ui_selected_provenance.epoch &&
                dual_ble_ui_selected_provenance.index < CERBERUS_DUAL_BLE_CAPACITY &&
                cerberus_dual_ble_response_source_valid(&row->response, &row->receive,
                    row->raw_length, row->event_type, row->raw_flags, row->address, row->address_type,
                    row->family.version ? row->family.family_code : 0U);
            ESP_RETURN_ON_ERROR(draw_centered(238, known_none ? "NO ADMITTED MATCHED RESPONSE" :
                "RESPONSE EVIDENCE UNKNOWN", UI_MUTED, 1), TAG, "C5 response state");
            ESP_RETURN_ON_ERROR(draw_centered(260, "ORIGINAL ADV IS KEPT SEPARATE", UI_MUTED, 1), TAG, "C5 response separation");
            ESP_RETURN_ON_ERROR(draw_centered(282, "NO CLAIM THAT NO RESPONSE WAS SENT", UI_MUTED, 1), TAG, "C5 response limit");
        } else if (dual_ble_ui_response_tab && dual_ble_ui_hints) {
            ESP_RETURN_ON_ERROR(dual_ble_response_hint_ui_draw(row, &dual_ble_ui_selected_provenance,
                dual_ble_ui_hint_page, &hint_pages), TAG, "C5 response hints");
        } else if (dual_ble_ui_hints) {
            ESP_RETURN_ON_ERROR(dual_ble_hint_ui_draw(row, &dual_ble_ui_selected_provenance,
                dual_ble_ui_hint_page, &hint_pages), TAG, "C5 original hints");
        } else if (dual_ble_ui_receive_info) {
            ESP_RETURN_ON_ERROR(dual_ble_ui_response_tab ?
                ble_receive_info_ui_draw(&row->response.receive, row->response.raw_length, 230U) :
                dual_ble_ui_draw_receive(row), TAG, "C5 receive metadata");
        } else {
            uint8_t raw_length = dual_ble_ui_response_tab ? row->response.raw_length : row->raw_length;
            const uint8_t *raw = dual_ble_ui_response_tab ? row->response.raw : row->raw;
            snprintf(line, sizeof(line), dual_ble_ui_response_tab ? "MATCHED RSP %u B / FLAGS %02X" :
                "ORIGINAL ADV %u B / FLAGS %02X", raw_length,
                dual_ble_ui_response_tab ? row->response.raw_flags : row->raw_flags);
            ESP_RETURN_ON_ERROR(draw_centered(234, line, UI_MUTED, 1), TAG, "C5 BLE packet");
            for (unsigned part = 0; part < 4U; ++part) {
                size_t n = 0U; line[0] = '\0';
                for (unsigned i = part * 8U; i < raw_length && i < (part + 1U) * 8U; ++i)
                    n += (size_t)snprintf(line + n, sizeof(line) - n, "%02X ", raw[i]);
                ESP_RETURN_ON_ERROR(draw_centered((uint16_t)(254U + part * 14U), line, UI_TEXT, 1), TAG, "C5 BLE bytes");
            }
        }
        ESP_RETURN_ON_ERROR(draw_small_button(8, 316, 148, 38,
            dual_ble_ui_from_monitor ? "BACK TO MONITOR" :
            dual_ble_ui_from_unified ? "BACK TO BLE LIST" : "BACK TO C5 LIST"), TAG, "C5 BLE list back");
        ESP_RETURN_ON_ERROR(draw_small_button(164, 316, 148, 38, "FOX THIS ADDRESS"), TAG, "C5 BLE fox");
    } else {
        uint8_t order[CERBERUS_DUAL_BLE_CAPACITY];
        dual_ble_ui_visible_count = cerberus_dual_ble_list_order(dual_ble_ui_store ? dual_ble_ui_store->rows : NULL,
            view.count, dual_ble_ui_sort, dual_ble_ui_recent_only, now, order);
        if (dual_ble_ui_response_requests)
            snprintf(line, sizeof(line), "C5 RESPONSE REQUESTS / %u OF %u", dual_ble_ui_visible_count, view.count);
        else if (dual_ble_ui_mode == CERBERUS_DUAL_BLE_FAMILY)
            snprintf(line, sizeof(line), "C5 FOCUSED / %u OF %u RETAINED", dual_ble_ui_visible_count, view.count);
        else if (view.live_updates) snprintf(line, sizeof(line), "C5 LIVE / %u OF %u / %lu UPDATES",
            dual_ble_ui_visible_count, view.count, (unsigned long)view.snapshots);
        else snprintf(line, sizeof(line), "C5 WHOLE SWEEP / %u OF %u", dual_ble_ui_visible_count, view.count);
        ESP_RETURN_ON_ERROR(draw_centered(84, line, UI_TEXT, 1), TAG, "C5 BLE source");
        if (dual_ble_ui_response_requests && view.error == ESP_ERR_NOT_SUPPORTED)
            snprintf(line, sizeof(line), "REQUESTS UNAVAILABLE / S3 AVAILABLE");
        else if (dual_ble_ui_mode == CERBERUS_DUAL_BLE_FAMILY && view.error == ESP_ERR_NOT_SUPPORTED)
            snprintf(line, sizeof(line), "FOCUS UNAVAILABLE / S3 VIEW AVAILABLE");
        else if (view.error == ESP_ERR_NO_MEM) snprintf(line, sizeof(line), "C5 NO MEMORY / S3 VIEW AVAILABLE");
        else if (view.error) snprintf(line, sizeof(line), "C5 ERROR %ld / S3 VIEW AVAILABLE", (long)view.error);
        else if (!requested && !view.enabled && !view.receiving && !view.reading && !view.stopping)
            snprintf(line, sizeof(line), "STOPPED / RETAINED %s", view.published_response ? "REQUESTS" : "PASSIVE");
        else if (dual_ble_ui_response_requests != view.requested_response || view.stopping)
            snprintf(line, sizeof(line), "MODE CHANGING / RETAINED %s", view.published_response ? "REQUESTS" : "PASSIVE");
        else if (view.published_response != dual_ble_ui_response_requests && view.count)
            snprintf(line, sizeof(line), "RETAINED %s / REQUESTED %s", view.published_response ? "REQUESTS" : "PASSIVE",
                dual_ble_ui_response_requests ? "REQUESTS" : "PASSIVE");
        else if (dual_ble_ui_mode == CERBERUS_DUAL_BLE_FAMILY)
            snprintf(line, sizeof(line), "FOCUS %s / %lu RF SWEEPS / ROWS RETAINED",
                view.stopping || view.requested_family != dual_ble_ui_family ? "CHANGING" :
                view.receiving ? "LIVE" : view.reading ? "READ" : view.enabled ? "WAIT" : "STOPPED",
                (unsigned long)view.sweeps);
        else snprintf(line, sizeof(line), "%s / %lu RF SWEEPS", view.stopping ? "STOPPING" :
            view.reading && view.receiving ? "RECEIVING + READING" : view.reading ? "READING" :
            view.receiving ? "RECEIVING" : view.enabled ? "WAITING" : "STOPPED", (unsigned long)view.sweeps);
        ESP_RETURN_ON_ERROR(draw_centered(99, line, view.error ? UI_BAD : UI_MUTED, 1), TAG, "C5 BLE status");
        const char *sort_label = dual_ble_ui_sort == CERBERUS_DUAL_BLE_SORT_SIGNAL ? "SIGNAL >" :
            dual_ble_ui_sort == CERBERUS_DUAL_BLE_SORT_NAME ? "NAME A-Z >" : "SCAN ORDER >";
        ESP_RETURN_ON_ERROR(draw_small_button(8, 114, 148, 30,
            ble_discovery_s3_retryable() ? "RETRY S3" : sort_label), TAG, "C5 BLE sort/retry");
        ESP_RETURN_ON_ERROR(draw_small_button(164, 114, 148, 30,
            dual_ble_live_c5_retryable() ? "RETRY C5" : dual_ble_ui_recent_only ?
            "WITHIN 30S >" : "ALL AGES >"), TAG, "C5 BLE recent/retry");
        if (dual_ble_ui_offset >= dual_ble_ui_visible_count) dual_ble_ui_offset = 0U;
        for (uint8_t i = 0U; i < 4U; ++i) {
            cerberus_dual_ble_row_t *row = &dual_ble_ui_painted[i];
            if (dual_ble_ui_offset + i >= dual_ble_ui_visible_count ||
                !cerberus_dual_ble_copy_row(dual_ble_ui_store, order[dual_ble_ui_offset + i], row,
                    &dual_ble_ui_painted_provenance[i])) break;
            uint16_t y = (uint16_t)(150U + i * 42U);
            ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(8, y, 304, 40, UI_PANEL), TAG, "C5 BLE row");
            dual_ble_ui_label(row, name);
            ESP_RETURN_ON_ERROR(cerberus_display_draw_text(14, y + 3U, name, UI_TEXT, UI_PANEL, 1), TAG, "C5 BLE row name");
            cerberus_privacy_format_ble_identifier(row->address, privacy_addresses_masked(), identifier, sizeof(identifier));
            snprintf(line, sizeof(line), "%s T%u%s", identifier, row->address_type,
                cerberus_dual_ble_list_response_name(row) ? " RSP NAME" : "");
            ESP_RETURN_ON_ERROR(cerberus_display_draw_text(14, y + 15U, line, UI_MUTED, UI_PANEL, 1), TAG, "C5 BLE row address");
            if (cerberus_dual_ble_list_rssi_valid(row)) snprintf(line, sizeof(line), "C5 %d dBm", row->rssi);
            else snprintf(line, sizeof(line), "C5 RSSI N/A");
            uint32_t age;
            size_t used = strlen(line);
            if (cerberus_dual_ble_list_age(row, now, &age)) {
                /* Round upwards; a bound >30s does not prove actual staleness. */
                uint32_t seconds = age / 1000U + (age % 1000U != 0U);
                snprintf(line + used, sizeof(line) - used, " / AGE<=%lus%s", (unsigned long)seconds,
                    age > CERBERUS_DUAL_BLE_RECENT_MS ? " RECENCY?" : "");
            } else snprintf(line + used, sizeof(line) - used, " / AGE UNKNOWN");
            ESP_RETURN_ON_ERROR(cerberus_display_draw_text(14, y + 27U, line, UI_MUTED, UI_PANEL, 1), TAG, "C5 BLE row age");
            ++painted_count;
        }
        if (!dual_ble_ui_visible_count) ESP_RETURN_ON_ERROR(draw_centered(208,
            dual_ble_ui_response_requests && view.error == ESP_ERR_NOT_SUPPORTED ?
            "NO C5 RESULTS / REQUESTS UNAVAILABLE" :
            dual_ble_ui_mode == CERBERUS_DUAL_BLE_FAMILY && view.error == ESP_ERR_NOT_SUPPORTED ?
            "NO C5 RESULTS / FOCUS UNAVAILABLE" : view.count ?
            "NO CONFIRMED RECENT C5 RESULTS" : view.live_updates ? "WAITING FOR C5 DISCOVERIES" :
            "WAITING FOR A COMPLETED C5 SWEEP", UI_MUTED, 1), TAG, "C5 BLE empty");
        if (view.inventory_known) snprintf(line, sizeof(line), "BANK REPLACED %lu / DROPPED %lu",
            (unsigned long)view.inventory_replaced, (unsigned long)view.inventory_dropped);
        else snprintf(line, sizeof(line), "BANK INVENTORY COUNTS UNAVAILABLE");
        ESP_RETURN_ON_ERROR(draw_centered(318, line, UI_MUTED, 1), TAG, "C5 BLE inventory");
        ESP_RETURN_ON_ERROR(draw_small_button(8, 326, 148, 32, "PREVIOUS"), TAG, "C5 BLE previous");
        ESP_RETURN_ON_ERROR(draw_small_button(164, 326, 148, 32, "NEXT"), TAG, "C5 BLE next");
        if (dual_ble_ui_selection_pending) snprintf(line, sizeof(line), "RELEASING RECEIVERS FOR DETAIL");
        else if (dual_ble_ui_response_requests) snprintf(line, sizeof(line), "1M REQUESTS / CODED PASSIVE");
        else if (view.has_first_result) snprintf(line, sizeof(line), "FIRST RESULTS IN %lu ms", (unsigned long)view.first_result_ms);
        else snprintf(line, sizeof(line), "NO NEW RESULTS THIS RUN");
        ESP_RETURN_ON_ERROR(draw_centered(360, line, UI_MUTED, 1), TAG, "C5 BLE reader latency");
    }
    ESP_RETURN_ON_ERROR(draw_button(20, 370, 280, 40,
        dual_ble_ui_from_monitor ? "BACK TO MONITOR" :
        dual_ble_ui_detail ? "BACK TO BLE LIST" :
        dual_ble_ui_selection_pending ? "CANCEL SELECTION" :
        !requested && (!dual_ble_live_discovery_released() || !dual_ble_ui_released()) ? "STOPPING BOTH RECEIVERS" :
        requested ? "STOP BOTH RECEIVERS" : "START BOTH RECEIVERS"), TAG, "C5 BLE control");
    dual_ble_ui_painted_count = dual_ble_ui_selection_pending ? 0U : painted_count;
    dual_ble_ui_hint_pages = hint_pages;
    return ESP_OK;
}

static int dual_ble_ui_tap(uint16_t x, uint16_t y)
{
    if (y >= 50U && y < 80U) {
        if (x >= 8U && x < (dual_ble_ui_detail ? 312U : 156U))
            return dual_ble_ui_from_monitor ? DUAL_BLE_UI_BACK_MONITOR : DUAL_BLE_UI_BACK_S3;
        if (!dual_ble_ui_detail && x >= 164U && x < 312U) {
            dual_ble_ui_response_requests = !dual_ble_ui_response_requests;
            dual_ble_ui_painted_count = 0U;
            return DUAL_BLE_UI_REDRAW;
        }
    }
    if (x >= 20U && x < 300U && y >= 370U && y < 410U)
        return dual_ble_ui_from_monitor ? DUAL_BLE_UI_BACK_MONITOR :
            dual_ble_ui_detail ? DUAL_BLE_UI_BACK_S3 : DUAL_BLE_UI_TOGGLE;
    if (dual_ble_ui_detail) {
        if (y >= 174U && y < 196U) {
            if (x >= 8U && x < 156U) dual_ble_ui_response_tab = false;
            else if (x >= 164U && x < 312U) dual_ble_ui_response_tab = true;
            else return DUAL_BLE_UI_NONE;
            dual_ble_ui_hint_page = dual_ble_ui_hint_pages = 0U;
            return DUAL_BLE_UI_REDRAW;
        }
        if (y >= 198U && y < 224U) {
            if (x >= 8U && x < 104U) { dual_ble_ui_receive_info = false; dual_ble_ui_hints = false; }
            else if (x >= 112U && x < 208U) { dual_ble_ui_receive_info = true; dual_ble_ui_hints = false; }
            else if (x >= 216U && x < 312U) { dual_ble_ui_receive_info = false; dual_ble_ui_hints = true; }
            else return DUAL_BLE_UI_NONE;
            dual_ble_ui_hint_pages = 0U;
            return DUAL_BLE_UI_REDRAW;
        }
        if (dual_ble_ui_hints && dual_ble_ui_hint_pages > 1U &&
            x >= 8U && x < 312U && y >= 294U && y < 312U) {
            dual_ble_ui_hint_page = (uint8_t)((dual_ble_ui_hint_page + 1U) % dual_ble_ui_hint_pages);
            dual_ble_ui_hint_pages = 0U;
            return DUAL_BLE_UI_REDRAW;
        }
        if (y >= 316U && y < 354U) {
            if (x >= 8U && x < 156U) {
                if (dual_ble_ui_from_monitor) return DUAL_BLE_UI_BACK_MONITOR;
                if (dual_ble_ui_from_unified) return DUAL_BLE_UI_BACK_S3;
                dual_ble_ui_detail = false; return DUAL_BLE_UI_REDRAW;
            }
            if (x >= 164U && x < 312U) return DUAL_BLE_UI_FOX;
        }
    } else {
        if (y >= 114U && y < 144U) {
            if (x >= 8U && x < 156U && ble_discovery_s3_retryable()) return DUAL_BLE_UI_RETRY_S3;
            if (x >= 164U && x < 312U && dual_ble_live_c5_retryable()) return DUAL_BLE_UI_RETRY_C5;
            if (x >= 8U && x < 156U) dual_ble_ui_sort =
                (cerberus_dual_ble_sort_t)(((unsigned)dual_ble_ui_sort + 1U) % CERBERUS_DUAL_BLE_SORT_COUNT);
            else if (x >= 164U && x < 312U) dual_ble_ui_recent_only = !dual_ble_ui_recent_only;
            else return DUAL_BLE_UI_NONE;
            dual_ble_ui_offset = 0U;
            return DUAL_BLE_UI_REDRAW;
        }
        if (x >= 8U && x < 312U && y >= 150U && y < 318U) {
            uint8_t row = (uint8_t)((y - 150U) / 42U);
            if (row < dual_ble_ui_painted_count && (y - 150U) % 42U < 40U) {
                dual_ble_ui_selected = dual_ble_ui_painted[row];
                dual_ble_ui_selected_provenance = dual_ble_ui_painted_provenance[row];
                dual_ble_ui_detail = false;
                dual_ble_ui_selection_pending = true;
                dual_ble_ui_painted_count = 0U;
                dual_ble_ui_from_unified = dual_ble_ui_from_monitor = false;
                dual_ble_ui_receive_info = false;
                dual_ble_ui_hints = false; dual_ble_ui_hint_page = dual_ble_ui_hint_pages = 0U; dual_ble_ui_response_tab = false;
                return DUAL_BLE_UI_SELECT;
            }
        }
        if (y >= 326U && y < 358U) {
            if (x >= 8U && x < 156U && dual_ble_ui_offset >= 4U) dual_ble_ui_offset -= 4U;
            else if (x >= 164U && x < 312U && dual_ble_ui_offset + 4U < dual_ble_ui_visible_count)
                dual_ble_ui_offset += 4U;
            else return DUAL_BLE_UI_NONE;
            return DUAL_BLE_UI_REDRAW;
        }
    }
    return DUAL_BLE_UI_NONE;
}

typedef struct {
    cerberus_dual_ble_view_t state;
    cerberus_dual_ble_track_view_t track;
    bool requested, bound, sample_current_job;
} dual_ble_ui_fox_view_t;

/* The page's copied typed address is authoritative, even while the previous
 * requested TRACK is draining. This getter performs no owner/transport work.
 * A sample can survive automatic job rollover; its count still belongs to the
 * captured job, never to a new job that has not seen the target. */
static void dual_ble_ui_fox_view(const uint8_t address[6], uint8_t type,
    uint64_t now, bool allow_samples, dual_ble_ui_fox_view_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!allow_samples || !address || type > 3U) return;
    out->requested = dual_ble_ui_mode == CERBERUS_DUAL_BLE_TRACK &&
        dual_ble_ui_type == type && !memcmp(dual_ble_ui_target, address, 6U);
    if (!out->requested) return;
    out->state.error = dual_ble_ui_error;
    out->bound = dual_ble_ui_store && dual_ble_ui_store->start.mode == CERBERUS_DUAL_BLE_TRACK &&
        dual_ble_ui_store->start.address_type == type &&
        !memcmp(dual_ble_ui_store->start.address, address, 6U);
    if (!out->bound) return;
    dual_ble_ui_get_view(&out->state);
    if (!dual_ble_ui_store->has_track || !dual_ble_ui_store->track.epoch ||
        !dual_ble_ui_store->track.job_id) return;
    cerberus_dual_ble_track_view(dual_ble_ui_store, now, &out->track);
    if (out->track.rssi < -127 || out->track.rssi > 20) {
        out->track.seen = out->track.fresh = false;
    }
    out->sample_current_job = dual_ble_ui_store->track.epoch == dual_ble_ui_store->remote.epoch &&
        dual_ble_ui_store->track.job_id == dual_ble_ui_store->start.job_id &&
        dual_ble_ui_store->track.job_id == dual_ble_ui_store->remote.job_id;
}

static esp_err_t dual_ble_ui_fox_meter(uint16_t y, bool seen, int rssi, uint16_t color)
{
    ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(20, y, 280, 12, UI_PANEL), TAG, "Fox meter background failed");
    if (!seen) return ESP_OK;
    if (rssi < -100) rssi = -100;
    if (rssi > -30) rssi = -30;
    uint16_t width = (uint16_t)(((rssi + 100) * 280) / 70);
    return width ? cerberus_display_fill_rect(20, y, width, 12, color) : ESP_OK;
}

static esp_err_t dual_ble_ui_fox_retry(uint16_t x, const char *label, bool enabled)
{
    ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(x, 350, 148, 32, UI_PANEL), TAG, "Fox retry background failed");
    return cerberus_display_draw_text((uint16_t)(x + (148U - strlen(label) * 6U) / 2U), 362,
        label, enabled ? UI_TEXT : UI_MUTED, UI_PANEL, 1);
}

static esp_err_t dual_ble_ui_draw_fox(uint16_t y, uint64_t now,
    const uint8_t address[6], uint8_t type, bool allow_samples, const char *blocked_state)
{
    char line[64]; dual_ble_ui_fox_view_t view;
    dual_ble_ui_fox_view(address, type, now, allow_samples, &view);
    bool receiving = view.bound && view.state.receiving && !view.state.stopping && !view.state.error;
    bool recent = receiving && view.sample_current_job && view.track.fresh;
    const char *state = blocked_state ? blocked_state :
        view.state.error ? "C5 / ERROR" : view.state.stopping ? "C5 / RELEASING" :
        receiving ? "C5 / LISTENING" : view.requested ? "C5 / BETWEEN JOBS OR ATTACHING" : "C5 / WAITING";
    ESP_RETURN_ON_ERROR(draw_centered(y, state, view.state.error ? UI_BAD : recent ? UI_GOOD : UI_MUTED, 1), TAG, "C5 Fox state failed");
    if (view.track.seen) {
        if (view.track.age_upper_ms == UINT32_MAX)
            snprintf(line, sizeof(line), "C5 LAST %d dBm / AGE UNKNOWN", view.track.rssi);
        else snprintf(line, sizeof(line), "C5 LAST %d dBm / AGE <= %lu ms", view.track.rssi,
            (unsigned long)view.track.age_upper_ms);
    } else snprintf(line, sizeof(line), "C5 NO SIGNAL SAMPLE FOR THIS TARGET");
    ESP_RETURN_ON_ERROR(draw_centered((uint16_t)(y + 18U), line, recent ? UI_GOOD : UI_MUTED, 1), TAG, "C5 Fox sample failed");
    ESP_RETURN_ON_ERROR(dual_ble_ui_fox_meter((uint16_t)(y + 36U), view.track.seen, view.track.rssi,
        recent ? UI_GOOD : UI_MUTED), TAG, "C5 Fox meter failed");
    if (view.track.seen) snprintf(line, sizeof(line), "C5 ORIGINAL CALLBACKS %s%lu",
        view.track.reports == UINT32_MAX ? ">=" : "", (unsigned long)view.track.reports);
    else snprintf(line, sizeof(line), "C5 ORIGINAL CALLBACKS / NO SAMPLE");
    ESP_RETURN_ON_ERROR(draw_centered((uint16_t)(y + 56U), line, UI_TEXT, 1), TAG, "C5 Fox count failed");
    const char *meaning = !view.track.seen ? "PER JOB / SCAN RESPONSES EXCLUDED" :
        !view.sample_current_job ? "LAST JOB / RETAINED READING" :
        !receiving ? "LAST READING / RECEIVER NOT LISTENING" :
        view.track.fresh ? "CURRENT JOB / RECENT UPPER BOUND" : "CURRENT JOB / RECENCY UNCONFIRMED";
    ESP_RETURN_ON_ERROR(draw_centered((uint16_t)(y + 72U), meaning, UI_MUTED, 1), TAG, "C5 Fox meaning failed");
    if (view.state.error) snprintf(line, sizeof(line), "C5 ERROR %ld", (long)view.state.error);
    else snprintf(line, sizeof(line), "C5 COUNTS EXCLUDE SCAN RESPONSES");
    return draw_centered((uint16_t)(y + 88U), line, view.state.error ? UI_BAD : UI_MUTED, 1);
}
