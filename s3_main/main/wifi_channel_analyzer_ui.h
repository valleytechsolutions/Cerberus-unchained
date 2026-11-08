/* Private to ui.c. The C5 owns reception; this optional S3 workspace owns the
 * retained timeline and navigation. Never abandon a pending radio release. */
#pragma once

#include "wifi_channel_analyzer_scan.h"
#include "wifi_packet_monitor_scan.h"
#include "esp_heap_caps.h"

typedef enum {
    CHANNEL_ANALYZER_NONE, CHANNEL_ANALYZER_RETUNE, CHANNEL_ANALYZER_RESTART,
    CHANNEL_ANALYZER_STOP, CHANNEL_ANALYZER_BACK,
} channel_analyzer_action_t;

typedef struct {
    wifi_channel_analyzer_scan_t scan;
    channel_analyzer_action_t action;
    uint64_t now_ms, paint_ms;
    uint8_t next_channel;
    bool painted, dirty;
} channel_analyzer_ui_store_t;

static channel_analyzer_ui_store_t *channel_analyzer_ui;
static esp_err_t channel_analyzer_open_error;
static const wifi_channel_analyzer_transport_t channel_analyzer_transport = {
    .info = c5_spi_link_wifi_channel_analyzer_info,
    .start = c5_spi_link_wifi_channel_analyzer_start,
    .status = c5_spi_link_wifi_channel_analyzer_status,
    .stop = c5_spi_link_wifi_channel_analyzer_stop,
    .sample = c5_spi_link_wifi_channel_analyzer_sample,
    .hint_peek = c5_spi_link_wifi_channel_analyzer_hint_peek,
    .hint_read = c5_spi_link_wifi_channel_analyzer_hint_read,
};

static bool channel_analyzer_ui_busy(void)
{
    return channel_analyzer_ui &&
        (channel_analyzer_ui->scan.phase != WIFI_ANALYZER_OFF ||
         channel_analyzer_ui->action != CHANNEL_ANALYZER_NONE);
}

static bool channel_analyzer_prepare_sleep(void)
{
    channel_analyzer_ui_store_t *s = channel_analyzer_ui;
    if (!s) return true;
    if (s->action != CHANNEL_ANALYZER_BACK) s->action = CHANNEL_ANALYZER_NONE;
    wifi_channel_analyzer_scan_stop(&s->scan);
    return !channel_analyzer_ui_busy();
}

static void channel_analyzer_start(uint8_t channel, bool keep_history)
{
    wifi_channel_analyzer_scan_t *scan = &channel_analyzer_ui->scan;
    scan->channel = channel;
    if (c5_status.firmware_version < CERBERUS_WIFI_CHANNEL_ANALYZER_MIN_FIRMWARE) {
        scan->error = ESP_ERR_NOT_SUPPORTED;
        return;
    }
    (void)wifi_channel_analyzer_scan_start(scan, channel, keep_history);
}

static void channel_analyzer_open(void)
{
    wifi_mode = UI_WIFI_MODE_CHANNEL_ANALYZER;
    if (!channel_analyzer_ui) {
        channel_analyzer_ui = heap_caps_calloc(1U, sizeof(*channel_analyzer_ui),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        channel_analyzer_open_error = channel_analyzer_ui ? ESP_OK : ESP_ERR_NO_MEM;
    }
    if (!channel_analyzer_ui) return;
    channel_analyzer_ui->painted = false;
    if (channel_analyzer_ui_busy()) return;
    channel_analyzer_start(1U, false);
}

static void channel_analyzer_request(channel_analyzer_action_t action)
{
    channel_analyzer_ui_store_t *s = channel_analyzer_ui;
    if (!s) { if (action == CHANNEL_ANALYZER_BACK) wifi_mode = UI_WIFI_MODE_MENU; return; }
    /* Back supersedes a pending channel change. Repeated taps cannot re-arm RF
     * or restart cleanup while an exact session remains owned. */
    if (s->action != CHANNEL_ANALYZER_NONE && action != CHANNEL_ANALYZER_BACK) return;
    s->action = action;
    wifi_channel_analyzer_scan_stop(&s->scan);
}

static void channel_analyzer_step(int direction)
{
    channel_analyzer_ui_store_t *s = channel_analyzer_ui;
    if (!s || s->action != CHANNEL_ANALYZER_NONE) return;
    uint16_t channel = s->scan.channel ? s->scan.channel : 1U;
    do {
        channel = direction > 0 ? (channel == 165U ? 1U : channel + 1U) :
            (channel == 1U ? 165U : channel - 1U);
    } while (!cerberus_wifi_packet_monitor_channel_valid((uint8_t)channel));
    s->next_channel = (uint8_t)channel;
    channel_analyzer_request(CHANNEL_ANALYZER_RETUNE);
}

static void channel_analyzer_tap(uint16_t x, uint16_t y)
{
    if (y < UI_WIFI_CONTROLS_Y || y >= UI_WIFI_CONTROLS_Y + 42U) return;
    if (x >= 8U && x < 76U) channel_analyzer_step(-1);
    else if (x >= 84U && x < 152U) channel_analyzer_step(1);
    else if (x >= 160U && x < 312U && channel_analyzer_ui)
        channel_analyzer_request(channel_analyzer_ui->scan.phase == WIFI_ANALYZER_OFF ?
            CHANNEL_ANALYZER_RESTART : CHANNEL_ANALYZER_STOP);
}

static bool channel_analyzer_tick(uint64_t now_ms, bool visible)
{
    channel_analyzer_ui_store_t *s = channel_analyzer_ui;
    if (!s) return false;
    s->now_ms = now_ms;
    if (!visible) {
        /* Sleep cancels a queued retune/restart and keeps the stopped history.
         * A wake never silently resumes reception. */
        (void)channel_analyzer_prepare_sleep();
    }
    bool changed = wifi_channel_analyzer_scan_tick(&s->scan, now_ms, &channel_analyzer_transport);
    s->dirty |= changed;
    if (s->scan.phase == WIFI_ANALYZER_OFF && s->action != CHANNEL_ANALYZER_NONE) {
        channel_analyzer_action_t action = s->action;
        s->action = CHANNEL_ANALYZER_NONE;
        if (action == CHANNEL_ANALYZER_BACK) {
            heap_caps_free(s);
            channel_analyzer_ui = NULL;
            if (current_page == UI_PAGE_WIFI && wifi_mode == UI_WIFI_MODE_CHANNEL_ANALYZER)
                wifi_mode = UI_WIFI_MODE_MENU;
            return true;
        }
        if (visible && (action == CHANNEL_ANALYZER_RETUNE || action == CHANNEL_ANALYZER_RESTART))
            channel_analyzer_start(action == CHANNEL_ANALYZER_RETUNE ? s->next_channel : s->scan.channel, true);
        s->dirty = true;
    }
    if (!visible && s->scan.phase == WIFI_ANALYZER_OFF &&
        (current_page != UI_PAGE_WIFI || wifi_mode != UI_WIFI_MODE_CHANNEL_ANALYZER)) {
        heap_caps_free(s);
        channel_analyzer_ui = NULL;
        return false;
    }
    /* Rates arrive more often than the LCD needs repainting. Hint age still
     * advances while a stopped result is retained. */
    if (!visible || (s->painted && now_ms >= s->paint_ms && now_ms - s->paint_ms < 250U)) return false;
    bool age_changed = !s->painted || now_ms < s->paint_ms || now_ms / 1000U != s->paint_ms / 1000U;
    if (!s->dirty && !age_changed) return false;
    s->paint_ms = now_ms;
    s->painted = true;
    s->dirty = false;
    return true;
}

static uint32_t channel_analyzer_rate(const wifi_channel_analyzer_entry_t *entry)
{
    if (!entry->window_us) return 0U;
    uint64_t rate = (uint64_t)entry->frames * 1000000U / entry->window_us;
    return rate > UINT32_MAX ? UINT32_MAX : (uint32_t)rate;
}

static esp_err_t channel_analyzer_graph(const wifi_channel_analyzer_scan_t *scan, uint32_t peak)
{
    ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(8, 145, 304, 96, UI_PANEL), TAG, "analyzer plot");
    ESP_RETURN_ON_ERROR(cerberus_display_draw_text(14, 149, "RECENT RECEIVE WINDOWS / GAPS IN GRAY",
        UI_MUTED, UI_PANEL, 1), TAG, "analyzer axis");
    if (!scan->history_count)
        return cerberus_display_draw_text(50, 194, "NO COMPLETED WINDOWS YET", UI_MUTED, UI_PANEL, 1);
    if (!peak) peak = 1U;
    for (unsigned i = 0U; i < scan->history_count; ++i) {
        wifi_channel_analyzer_entry_t entry;
        if (!wifi_channel_analyzer_scan_history(scan, i, &entry)) continue;
        uint16_t x = (uint16_t)(14U + (uint32_t)i * 292U / scan->history_count);
        if (entry.kind == WIFI_ANALYZER_GAP || entry.kind == WIFI_ANALYZER_CHANNEL) {
            ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(x, 174, 1, 64,
                entry.kind == WIFI_ANALYZER_CHANNEL ? UI_BAD : UI_MUTED), TAG, "analyzer gap");
            if (entry.kind == WIFI_ANALYZER_CHANNEL) {
                char marker[8];
                snprintf(marker, sizeof(marker), "CH%u", (unsigned)entry.channel);
                ESP_RETURN_ON_ERROR(cerberus_display_draw_text(x > 276U ? 276U : x, 162,
                    marker, UI_BAD, UI_PANEL, 1), TAG, "analyzer channel marker");
            }
            continue;
        }
        uint32_t rate = channel_analyzer_rate(&entry);
        uint16_t height = (uint16_t)((uint64_t)rate * 62U / peak);
        if (height > 62U) height = 62U;
        /* A measured zero gets a baseline pixel. Missing windows never do. */
        ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(x, (uint16_t)(237U - height), 1,
            (uint16_t)(height + 1U), UI_GOOD), TAG, "analyzer rate");
    }
    return ESP_OK;
}

static esp_err_t channel_analyzer_draw(void)
{
    ESP_RETURN_ON_ERROR(draw_centered(51, "CHANNEL ANALYZER", UI_GOOD, 2), TAG, "analyzer title");
    ESP_RETURN_ON_ERROR(draw_centered(72, "C5 RECEIVER / S3 ANALYSIS", UI_MUTED, 1), TAG, "analyzer roles");
    channel_analyzer_ui_store_t *s = channel_analyzer_ui;
    if (!s) return draw_centered(176, channel_analyzer_open_error == ESP_ERR_NO_MEM ?
        "NO PSRAM FOR ANALYZER / USE BACK" : "ANALYZER STOPPED / USE BACK", UI_BAD, 1);
    wifi_channel_analyzer_scan_t *scan = &s->scan;
    char text[64];
    unsigned channel = scan->channel ? scan->channel : 1U;
    snprintf(text, sizeof(text), "CH %u / %s", channel, channel <= 14U ? "2.4 GHZ" : "5 GHZ");
    ESP_RETURN_ON_ERROR(draw_centered(88, text, UI_TEXT, 2), TAG, "analyzer channel");
    if (scan->error == ESP_ERR_NOT_SUPPORTED) snprintf(text, sizeof(text), "C5 UPDATE REQUIRED / ANALYZER UNSUPPORTED");
    else if (s->action != CHANNEL_ANALYZER_NONE || scan->phase == WIFI_ANALYZER_STOP || scan->phase == WIFI_ANALYZER_DRAIN) {
        if (scan->error != ESP_OK) snprintf(text, sizeof(text), "C5 RELEASE RETRY: %.30s", esp_err_to_name(scan->error));
        else if (scan->transport_error != ESP_OK) snprintf(text, sizeof(text), "LINK RELEASE RETRY: %.28s", esp_err_to_name(scan->transport_error));
        else snprintf(text, sizeof(text), "RELEASING C5%s", s->action == CHANNEL_ANALYZER_BACK ? " / RETURNING TO MENU" : "");
    }
    else if (scan->error != ESP_OK) snprintf(text, sizeof(text), "C5: %.43s", esp_err_to_name(scan->error));
    else if (scan->transport_error != ESP_OK) snprintf(text, sizeof(text), "LINK: %.41s", esp_err_to_name(scan->transport_error));
    else if (scan->phase == WIFI_ANALYZER_INFO || scan->phase == WIFI_ANALYZER_START) snprintf(text, sizeof(text), "WAITING FOR C5 RECEIVER");
    else snprintf(text, sizeof(text), "%s / CHANNEL-WIDE CALLBACKS",
        scan->phase == WIFI_ANALYZER_OFF ? "STOPPED" : "LISTENING");
    ESP_RETURN_ON_ERROR(draw_centered(110, text, scan->error == ESP_OK && scan->transport_error == ESP_OK ? UI_MUTED : UI_BAD, 1), TAG, "analyzer state");

    wifi_channel_analyzer_summary_t summary;
    wifi_channel_analyzer_scan_summary(scan, &summary);
    wifi_channel_analyzer_entry_t latest = {0};
    bool have_latest = scan->history_count &&
        wifi_channel_analyzer_scan_history(scan, scan->history_count - 1U, &latest) && latest.kind == WIFI_ANALYZER_WINDOW;
    if (summary.current_valid && have_latest)
        snprintf(text, sizeof(text), "%s %s%lu/S / %s%lu FR / %luMS",
            scan->phase == WIFI_ANALYZER_OFF ? "LAST" : "NOW",
            latest.flags || summary.current_rate == UINT32_MAX ? ">=" : "", (unsigned long)summary.current_rate,
            latest.flags ? ">=" : "", (unsigned long)latest.frames,
            (unsigned long)(latest.window_us / 1000U));
    else snprintf(text, sizeof(text), "NOW -- / NO CURRENT COMPLETED WINDOW");
    ESP_RETURN_ON_ERROR(draw_centered(128, text, UI_TEXT, 1), TAG, "analyzer current");
    ESP_RETURN_ON_ERROR(channel_analyzer_graph(scan, summary.peak_rate), TAG, "analyzer history");
    if (summary.windows) snprintf(text, sizeof(text), "PEAK %s%lu/S / AVG %s%lu/S",
        summary.lower_bound || summary.rate_limited ? ">=" : "", (unsigned long)summary.peak_rate,
        summary.lower_bound || summary.rate_limited ? ">=" : "", (unsigned long)summary.average_rate);
    else snprintf(text, sizeof(text), "PEAK -- / AVG --");
    ESP_RETURN_ON_ERROR(draw_centered(249, text, UI_TEXT, 1), TAG, "analyzer summary");
    snprintf(text, sizeof(text), "%u RETAINED WINDOWS / %u GAPS / AVG BY TIME",
        (unsigned)summary.windows, (unsigned)summary.gap_count);
    ESP_RETURN_ON_ERROR(draw_centered(263, text, UI_MUTED, 1), TAG, "analyzer summary scope");

    uint32_t age = wifi_channel_analyzer_scan_hint_age(scan, s->now_ms);
    if (!scan->hint_valid) snprintf(text, sizeof(text), "LATEST BEACON / NONE OBSERVED");
    else if (age == UINT32_MAX) snprintf(text, sizeof(text), "LATEST BEACON / AGE UNKNOWN");
    else snprintf(text, sizeof(text), "LATEST BEACON / %luS AGO%s", (unsigned long)(age / 1000U), age >= 5000U ? " / STALE" : "");
    ESP_RETURN_ON_ERROR(draw_centered(281, text, UI_MUTED, 1), TAG, "analyzer hint age");
    if (scan->hint_valid) {
        char name[129], identifier[18];
        size_t length = wifi_channel_analyzer_scan_ssid_display(&scan->hint, name, sizeof(name));
        if (!length) { snprintf(name, sizeof(name), "<HIDDEN SSID>"); length = strlen(name); }
        for (unsigned row = 0U; row < 3U; ++row) {
            size_t offset = row * 48U;
            if (offset >= length) break;
            snprintf(text, sizeof(text), "%.48s", name + offset);
            ESP_RETURN_ON_ERROR(cerberus_display_draw_text(16, (uint16_t)(296U + row * 11U), text,
                UI_TEXT, UI_BACKGROUND, 1), TAG, "analyzer SSID");
        }
        cerberus_privacy_format_wifi_identifier(scan->hint.bssid, privacy_addresses_masked(), identifier, sizeof(identifier));
        snprintf(text, sizeof(text), "%s / CH %u", identifier, (unsigned)scan->hint.channel);
        ESP_RETURN_ON_ERROR(draw_centered(333, text, UI_TEXT, 1), TAG, "analyzer BSSID");
        snprintf(text, sizeof(text), "RSSI %d DBM / BEACON HINT, NOT IDENTITY", (int)scan->hint.rssi);
        ESP_RETURN_ON_ERROR(draw_centered(347, text, UI_MUTED, 1), TAG, "analyzer RSSI");
    }
    ESP_RETURN_ON_ERROR(draw_small_button(8, UI_WIFI_CONTROLS_Y, 68, 42, "CH -"), TAG, "analyzer previous");
    ESP_RETURN_ON_ERROR(draw_small_button(84, UI_WIFI_CONTROLS_Y, 68, 42, "CH +"), TAG, "analyzer next");
    return draw_small_button(160, UI_WIFI_CONTROLS_Y, 152, 42,
        s->action != CHANNEL_ANALYZER_NONE ? "STOPPING" : scan->phase == WIFI_ANALYZER_OFF ? "RESTART" : "STOP");
}
