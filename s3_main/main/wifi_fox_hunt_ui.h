#pragma once
/* Private UI-task controller. Target identity is copied before the source
 * receiver is stopped; the source detail stays intact until Back completes. */
#include "wifi_fox_hunt_scan.h"
#include "esp_heap_caps.h"

typedef struct {
    wifi_fox_hunt_scan_t scan;
    uint64_t now_ms, paint_ms;
    esp_err_t error;
    uint8_t target[6], label[32], label_length, channel, origin, search_band;
    bool start_pending, started, closing, return_origin, painted;
} wifi_fox_hunt_ui_store_t;
static wifi_fox_hunt_ui_store_t *wifi_fox_hunt_ui;
static const wifi_fox_hunt_transport_t wifi_fox_hunt_transport = {
    .info = c5_spi_link_wifi_fox_hunt_info,
    .start = c5_spi_link_wifi_fox_hunt_start,
    .status = c5_spi_link_wifi_fox_hunt_status,
    .stop = c5_spi_link_wifi_fox_hunt_stop,
    .signal = c5_spi_link_wifi_fox_hunt_signal,
};

static bool wifi_fox_hunt_supported(void)
{ return c5_online && c5_status.firmware_version >= CERBERUS_WIFI_FOX_HUNT_MIN_FIRMWARE; }

static bool wifi_fox_hunt_ui_busy(void)
{
    /* The previous owner's polling must stay enabled while START is waiting
     * for that owner to drain. INFO onward belongs to this controller. */
    return wifi_fox_hunt_ui && wifi_fox_hunt_ui->scan.phase != WIFI_FOX_OFF;
}

static bool wifi_fox_hunt_holds_source(uint8_t origin)
{ return wifi_fox_hunt_ui && wifi_fox_hunt_ui->origin == origin; }

static bool wifi_fox_hunt_previous_released(void)
{
    wifi_fox_hunt_ui_store_t *s = wifi_fox_hunt_ui;
    if (!s) return true;
    if (s->origin == UI_WIFI_MODE_PINESCAN && pinescan_busy()) return false;
    if (s->origin == UI_WIFI_MODE_MULTISSID && multissid_busy()) return false;
    cerberus_dual_wifi_status_t status;
    cerberus_dual_wifi_scan_get_status(&status);
    return dual_wifi_live_released() && !dual_wifi_ui_busy(&status);
}

static esp_err_t wifi_fox_hunt_open(const uint8_t target[6], uint8_t channel,
    const uint8_t *label, size_t length, uint8_t origin)
{
    if (!target || (!label && length) || length > 32U ||
        (origin != UI_WIFI_MODE_TARGET && origin != UI_WIFI_MODE_OBSERVED_CLIENTS &&
         origin != UI_WIFI_MODE_PINESCAN && origin != UI_WIFI_MODE_MULTISSID)) return ESP_ERR_INVALID_ARG;
    cerberus_wifi_fox_hunt_start_t request = {.session_id = 1U, .initial_channel = channel,
        .search_band = CERBERUS_WIFI_FOX_HUNT_BAND_BOTH};
    memcpy(request.target, target, 6U);
    if (!cerberus_wifi_fox_hunt_start_valid(&request)) return ESP_ERR_INVALID_ARG;
    if (!wifi_fox_hunt_supported()) return ESP_ERR_NOT_SUPPORTED;
    if (wifi_fox_hunt_ui) return ESP_ERR_INVALID_STATE;
    wifi_fox_hunt_ui_store_t *s = heap_caps_calloc(1U, sizeof(*s), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s) return ESP_ERR_NO_MEM;
    memcpy(s->target, target, 6U);
    if (length) memcpy(s->label, label, length);
    s->label_length = (uint8_t)length; s->channel = channel; s->origin = origin;
    s->search_band = origin == UI_WIFI_MODE_OBSERVED_CLIENTS ? CERBERUS_WIFI_FOX_HUNT_BAND_BOTH :
        channel <= 14U ? CERBERUS_WIFI_FOX_HUNT_BAND_2G : CERBERUS_WIFI_FOX_HUNT_BAND_5G;
    wifi_fox_hunt_ui = s;
    dual_wifi_live_cancel_handoff(); dual_wifi_ui_leave(); wifi_relation_cancel();
    sentinel_scan_session_select(&live_session, SENTINEL_SCAN_NONE);
    sentinel_scan_session_select(&live_ble_session, SENTINEL_SCAN_NONE);
    if (origin == UI_WIFI_MODE_PINESCAN) pinescan_request_stop();
    if (origin == UI_WIFI_MODE_MULTISSID) multissid_request_stop();
    current_page = UI_PAGE_WIFI; wifi_mode = UI_WIFI_MODE_FOX_HUNT;
    return ESP_OK;
}

static void wifi_fox_hunt_back(void)
{
    wifi_fox_hunt_ui_store_t *s = wifi_fox_hunt_ui;
    if (!s) { wifi_mode = UI_WIFI_MODE_MENU; return; }
    s->closing = s->return_origin = true; s->start_pending = false;
    wifi_fox_hunt_scan_stop(&s->scan);
}

static bool wifi_fox_hunt_prepare_sleep(void)
{
    wifi_fox_hunt_ui_store_t *s = wifi_fox_hunt_ui;
    if (!s) return true;
    s->start_pending = false;
    wifi_fox_hunt_scan_stop(&s->scan);
    return !wifi_fox_hunt_ui_busy() && wifi_fox_hunt_previous_released();
}

static void wifi_fox_hunt_tap(uint16_t x, uint16_t y)
{
    if (x < 8U || x >= 312U) return;
    if (y >= UI_BACK_Y && y < UI_BACK_Y + UI_BACK_HEIGHT) { wifi_fox_hunt_back(); return; }
    wifi_fox_hunt_ui_store_t *s = wifi_fox_hunt_ui;
    if (!s || s->closing || y < 366U || y >= 408U) return;
    if (s->start_pending || s->scan.phase != WIFI_FOX_OFF) {
        s->start_pending = false;
        wifi_fox_hunt_scan_stop(&s->scan);
    } else if (!wifi_fox_hunt_supported()) s->error = ESP_ERR_NOT_SUPPORTED;
    else { s->start_pending = true; s->error = ESP_OK; }
}

static void wifi_fox_hunt_tick(uint64_t now_ms, bool visible)
{
    wifi_fox_hunt_ui_store_t *s = wifi_fox_hunt_ui;
    if (!s) return;
    s->now_ms = now_ms;
    if (!visible) {
        (void)wifi_fox_hunt_prepare_sleep();
        if (current_page != UI_PAGE_WIFI || wifi_mode != UI_WIFI_MODE_FOX_HUNT) s->closing = true;
    }
    if (s->start_pending && visible && !s->closing && wifi_fox_hunt_previous_released()) {
        s->start_pending = false;
        if (!wifi_fox_hunt_supported()) s->error = ESP_ERR_NOT_SUPPORTED;
        else if (!wifi_fox_hunt_scan_start(&s->scan, s->target, s->channel,
            s->search_band, s->started)) s->error = ESP_ERR_INVALID_STATE;
        else s->started = true;
    }
    (void)wifi_fox_hunt_scan_tick(&s->scan, now_ms, &wifi_fox_hunt_transport);
    if (s->closing && s->scan.phase == WIFI_FOX_OFF && wifi_fox_hunt_previous_released()) {
        uint8_t origin = s->origin;
        bool back = s->return_origin;
        heap_caps_free(s); wifi_fox_hunt_ui = NULL;
        if (back && current_page == UI_PAGE_WIFI && wifi_mode == UI_WIFI_MODE_FOX_HUNT) {
            wifi_mode = origin;
            if (visible) ESP_ERROR_CHECK(draw_detail_page());
        }
        return;
    }
    if (visible && (!s->painted || now_ms < s->paint_ms || now_ms - s->paint_ms >= 250U)) {
        s->paint_ms = now_ms; s->painted = true;
        ESP_ERROR_CHECK(draw_detail_page());
    }
}

static const char *wifi_fox_hunt_origin(uint8_t origin)
{
    switch (origin) {
    case UI_WIFI_MODE_TARGET: return "AP";
    case UI_WIFI_MODE_OBSERVED_CLIENTS: return "CLIENT";
    case UI_WIFI_MODE_PINESCAN: return "PINESCAN";
    case UI_WIFI_MODE_MULTISSID: return "MULTISSID";
    default: return "WI-FI";
    }
}

static unsigned wifi_fox_hunt_strength(int rssi)
{ return rssi <= -100 ? 0U : rssi >= -30 ? 70U : (unsigned)(rssi + 100); }

static esp_err_t wifi_fox_hunt_graph(const wifi_fox_hunt_scan_t *scan)
{
    ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(8, 274, 304, 68, UI_PANEL), TAG, "fox history");
    ESP_RETURN_ON_ERROR(cerberus_display_draw_text(14, 279, "RECENT SIGNAL UPDATES / GAPS IN GRAY",
        UI_MUTED, UI_PANEL, 1), TAG, "fox history axis");
    if (!scan->history_count) return cerberus_display_draw_text(65, 309, "NO TARGET SIGNAL RECEIVED", UI_MUTED, UI_PANEL, 1);
    for (unsigned i = 0U; i < scan->history_count; ++i) {
        wifi_fox_hunt_entry_t entry;
        if (!wifi_fox_hunt_scan_history(scan, i, &entry)) continue;
        unsigned x = 14U + i * 292U / scan->history_count;
        if (entry.kind == WIFI_FOX_GAP) {
            ESP_RETURN_ON_ERROR(cerberus_display_fill_rect((uint16_t)x, 292, 1, 46, UI_MUTED), TAG, "fox gap");
        } else {
            unsigned height = wifi_fox_hunt_strength(entry.rssi) * 44U / 70U;
            ESP_RETURN_ON_ERROR(cerberus_display_fill_rect((uint16_t)x, (uint16_t)(337U - height), 1,
                (uint16_t)(height + 1U), UI_GOOD), TAG, "fox signal");
        }
    }
    return ESP_OK;
}

static esp_err_t wifi_fox_hunt_draw(void)
{
    ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(0, 0, 320, 480, UI_BACKGROUND), TAG, "fox background");
    ESP_RETURN_ON_ERROR(draw_header("WI-FI FOX HUNT"), TAG, "fox title");
    wifi_fox_hunt_ui_store_t *s = wifi_fox_hunt_ui;
    if (!s) return draw_button(8, UI_BACK_Y, 304, UI_BACK_HEIGHT, "WI-FI MENU");
    ESP_RETURN_ON_ERROR(draw_centered(54, "C5 RECEIVER / S3 SIGNAL HISTORY", UI_MUTED, 1), TAG, "fox roles");
    char text[64], name[129], identifier[18];
    snprintf(text, sizeof(text), "FROM %s / SELECTED TRANSMITTER", wifi_fox_hunt_origin(s->origin));
    ESP_RETURN_ON_ERROR(draw_centered(70, text, UI_MUTED, 1), TAG, "fox source");
    /* Beacon detail and Fox Hunt use the same byte-preserving label display. */
    size_t length = pinescan_escape(s->label, s->label_length, name, sizeof(name));
    if (!length) { snprintf(name, sizeof(name), "<HIDDEN SSID>"); length = strlen(name); }
    for (unsigned row = 0U; row < 3U && row * 48U < length; ++row) {
        snprintf(text, sizeof(text), "%.48s", name + row * 48U);
        ESP_RETURN_ON_ERROR(cerberus_display_draw_text(16, (uint16_t)(87U + row * 11U), text,
            UI_TEXT, UI_BACKGROUND, 1), TAG, "fox target label");
    }
    cerberus_privacy_format_wifi_identifier(s->target, privacy_addresses_masked(), identifier, sizeof(identifier));
    snprintf(text, sizeof(text), "TA %s / START CH %u", identifier, (unsigned)s->channel);
    ESP_RETURN_ON_ERROR(draw_centered(125, text, UI_TEXT, 1), TAG, "fox target");
    wifi_fox_hunt_view_t view;
    wifi_fox_hunt_scan_view(&s->scan, s->now_ms, &view);
    bool stopping = s->closing || s->scan.phase == WIFI_FOX_STOP || s->scan.phase == WIFI_FOX_FINAL;
    const char *state = stopping ? "STOPPING" : s->start_pending ? "WAITING FOR RADIO" :
        s->error ? "ERROR" : !s->started ? "READY" :
        view.state == WIFI_FOX_TRACKING ? "TRACKING" : view.state == WIFI_FOX_SEARCHING ? "SEARCHING" :
        view.state == WIFI_FOX_WAITING ? "WAITING FOR SIGNAL" : view.state == WIFI_FOX_STALE ? "SIGNAL STALE" :
        view.state == WIFI_FOX_FAULT ? "RADIO ERROR" : "STOPPED";
    ESP_RETURN_ON_ERROR(draw_centered(148, state, view.current ? UI_GOOD : UI_TEXT, 2), TAG, "fox state");
    if (view.current) snprintf(text, sizeof(text), "%d DBM", (int)view.rssi);
    else snprintf(text, sizeof(text), "-- DBM");
    ESP_RETURN_ON_ERROR(draw_centered(177, text, view.current ? UI_GOOD : UI_MUTED, 3), TAG, "fox current signal");
    ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(16, 210, 288, 12, UI_PANEL), TAG, "fox meter");
    unsigned strength = view.current ? wifi_fox_hunt_strength(view.rssi) * 288U / 70U : 0U;
    if (strength) ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(16, 210, (uint16_t)strength, 12, UI_GOOD), TAG, "fox meter value");
    if (view.have_signal && view.age_ms != UINT32_MAX)
        snprintf(text, sizeof(text), "LAST %d DBM / CH %u / AGE <= %lu.%03luS", (int)view.rssi,
            (unsigned)view.observed_channel, (unsigned long)(view.age_ms / 1000U), (unsigned long)(view.age_ms % 1000U));
    else if (view.have_signal) snprintf(text, sizeof(text), "LAST %d DBM / CH %u / AGE UNKNOWN", (int)view.rssi, (unsigned)view.observed_channel);
    else snprintf(text, sizeof(text), "NO TARGET OBSERVATION / AGE UNKNOWN");
    ESP_RETURN_ON_ERROR(draw_centered(233, text, UI_TEXT, 1), TAG, "fox last observation");
    if (view.listening_channel) snprintf(text, sizeof(text), "LAST RX CH %u / SEARCH SWEEPS %s%u", (unsigned)view.listening_channel,
        view.sweeps_saturated ? ">=" : "", (unsigned)view.sweeps);
    else snprintf(text, sizeof(text), "RX -- / SEARCH SWEEPS %s%u", view.sweeps_saturated ? ">=" : "", (unsigned)view.sweeps);
    ESP_RETURN_ON_ERROR(draw_centered(248, text, UI_MUTED, 1), TAG, "fox channel");
    ESP_RETURN_ON_ERROR(draw_centered(262, "SIGNAL STRENGTH IS NOT DISTANCE", UI_MUTED, 1), TAG, "fox scope");
    ESP_RETURN_ON_ERROR(wifi_fox_hunt_graph(&s->scan), TAG, "fox graph");
    esp_err_t error = s->error ? s->error : s->scan.error ? s->scan.error : s->scan.transport_error;
    if (error == ESP_ERR_NOT_SUPPORTED) snprintf(text, sizeof(text), "C5 UPDATE REQUIRED / FOX HUNT UNSUPPORTED");
    else if (error) snprintf(text, sizeof(text), "%s: %.31s", stopping ? "RELEASE RETRY" : "RADIO", esp_err_to_name(error));
    else if (view.channels_skipped) snprintf(text, sizeof(text), "SOME CHANNELS SKIPPED: %.26s", esp_err_to_name(view.warning));
    else snprintf(text, sizeof(text), "PASSIVE / SEARCH %s AFTER LOSS",
        s->search_band == CERBERUS_WIFI_FOX_HUNT_BAND_BOTH ? "2.4 + 5 GHZ" :
        s->search_band == CERBERUS_WIFI_FOX_HUNT_BAND_2G ? "2.4 GHZ" : "5 GHZ");
    ESP_RETURN_ON_ERROR(draw_centered(350, text, error || view.channels_skipped ? UI_BAD : UI_MUTED, 1), TAG, "fox warning");
    ESP_RETURN_ON_ERROR(draw_button(8, 366, 304, 42, stopping ? "STOPPING" :
        s->start_pending || s->scan.phase != WIFI_FOX_OFF ? "STOP" : s->started ? "RESTART" : "START"), TAG, "fox control");
    snprintf(text, sizeof(text), "BACK TO %s", wifi_fox_hunt_origin(s->origin));
    return draw_button(8, UI_BACK_Y, 304, UI_BACK_HEIGHT, text);
}
