#pragma once

/* One passive monitor reached from the Wi-Fi menu or the AP capture selector.
 * The shared capture worker owns reception; this page owns copied scope and
 * presentation only. A Stop retains evidence until explicit release. */
#include "wifi_frame_session.h"
#include "esp_heap_caps.h"

enum { FRAME_SCOPE_ALL, FRAME_SCOPE_2G, FRAME_SCOPE_5G, FRAME_SCOPE_FIXED, FRAME_SCOPE_SELECTED, FRAME_SCOPE_AP };
enum { FRAME_VIEW_OPTIONS, FRAME_VIEW_SUMMARY, FRAME_VIEW_ROWS };
enum { FRAME_EXIT_STAY, FRAME_EXIT_NEXT, FRAME_EXIT_BACK, FRAME_EXIT_HIDDEN };
typedef struct {
    cerberus_c5_wifi_result_t target;
    char ssid[33];
    cerberus_wifi_capture_plan_t plan;
    cerberus_s3_frame_options_t request;
    cerberus_s3_frame_status_t status;
    cerberus_s3_frame_row_t rows[2];
    cerberus_s3_frame_page_t page;
    uint32_t painted_ms;
    uint16_t offset;
    esp_err_t error, read_error;
    uint8_t scope, run_scope, channel, run_channel, view, exit, target_index, filter, run_filter;
    bool save, started, start_pending, stop_pending, barrier, painted, target_dual_return;
} frame_ui_store_t;
static frame_ui_store_t *frame_ui;
static bool frame_ui_opened, frame_ui_from_menu;
static uint8_t frame_ui_initial_filter;
static const uint8_t frame_ui_modes[] = {CERBERUS_WIFI_CAPTURE_MODE_RAW, CERBERUS_WIFI_CAPTURE_MODE_PROBE, CERBERUS_WIFI_CAPTURE_MODE_DEAUTH};
static const char *const frame_ui_filter_names[] = {"RAW FRAMES", "PROBES", "DEAUTH / DISASSOC"};

static bool frame_ui_is_open(void) { return frame_ui_opened; }
static bool frame_ui_busy(void) { return frame_ui && frame_ui->status.capture.busy; }
static bool frame_ui_pending(void)
{ return frame_ui && (frame_ui->start_pending || frame_ui->stop_pending || frame_ui->barrier || frame_ui->exit); }
static bool frame_ui_supported(void)
{ return c5_online && c5_status.firmware_version >= CERBERUS_WIFI_FRAME_SCOPE_MIN_FIRMWARE; }
static const char *frame_ui_back_label(void)
{ return frame_ui_pending() && frame_ui && frame_ui->exit ? "STOPPING" : frame_ui_from_menu ? "RX TOOLS" : "TARGET"; }

static void frame_ui_allocate(void)
{
    if (frame_ui) return;
    frame_ui = heap_caps_calloc(1U, sizeof(*frame_ui), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!frame_ui) return;
    frame_ui->view = FRAME_VIEW_OPTIONS;
    frame_ui->filter = frame_ui_initial_filter;
    frame_ui->save = true;
    frame_ui->scope = frame_ui_from_menu ? FRAME_SCOPE_ALL : FRAME_SCOPE_AP;
    frame_ui->channel = frame_ui_from_menu ? 1U : capture_target.channel;
    if (!frame_ui_from_menu) {
        frame_ui->target = capture_target;
        frame_ui->target_index = wifi_selected_index;
        frame_ui->target_dual_return = dual_wifi_return_to_view;
        memcpy(frame_ui->ssid, capture_ssid, sizeof(frame_ui->ssid));
    }
}

static void frame_ui_open(bool menu, uint8_t filter_index)
{
    if (frame_ui_opened || eapol_ui_is_open() || sae_ui_is_open() || filter_index >= 3U) return;
    frame_ui_opened = true; frame_ui_from_menu = menu; frame_ui_initial_filter = filter_index;
    frame_ui_allocate();
    capture_mode_index = filter_index;
    wifi_mode = UI_WIFI_MODE_CAPTURE;
}

static void frame_ui_stop(void)
{
    if (!frame_ui) return;
    frame_ui->start_pending = false;
    frame_ui->stop_pending = true;
    cerberus_s3_frame_stop();
}

static void frame_ui_close(bool back)
{
    /* The targetless shortcut has no unrelated generic capture scope. */
    if (!back && frame_ui_from_menu) return;
    if (!frame_ui) {
        frame_ui_opened = false;
        if (back) wifi_mode = frame_ui_from_menu ? UI_WIFI_MODE_RX_TOOLS : UI_WIFI_MODE_TARGET;
        else capture_mode_index = 3U;
        return;
    }
    if (back || !frame_ui->exit) frame_ui->exit = back ? FRAME_EXIT_BACK : FRAME_EXIT_NEXT;
    frame_ui_stop();
}

static bool frame_ui_freeze(frame_ui_store_t *s)
{
    cerberus_wifi_capture_plan_clear(&s->plan);
    s->plan.dwell_ms = s->filter == 0U ? CERBERUS_WIFI_CAPTURE_PLAN_RAW_DWELL_MS : CERBERUS_WIFI_CAPTURE_PLAN_DWELL_MS;
    s->request = (cerberus_s3_frame_options_t){.save_pcap = s->save, .mode_mask = frame_ui_modes[s->filter]};
    if (s->scope == FRAME_SCOPE_FIXED || s->scope == FRAME_SCOPE_AP) {
        s->request.channel = s->scope == FRAME_SCOPE_AP ? s->target.channel : s->channel;
        if (!cerberus_wifi_capture_plan_channel_bit(s->request.channel)) return false;
        s->request.filter_bssid = s->scope == FRAME_SCOPE_AP && s->filter != 1U;
        if (s->request.filter_bssid) memcpy(s->request.bssid, s->target.bssid, 6U);
    } else {
        if (s->scope == FRAME_SCOPE_SELECTED) {
            if (!wifi_target_count) return false;
            for (unsigned i = 0; i < wifi_target_count; ++i)
                if (!cerberus_wifi_capture_plan_add_target(&s->plan,
                    wifi_targets[i].ap.bssid, wifi_targets[i].ap.channel)) return false;
        } else s->plan.channel_mask = cerberus_wifi_capture_plan_band_mask(s->scope);
        /* Probe requests can have a wildcard BSSID. Selected probes use
         * only the copied channel union, never an AP identity filter. */
        if (s->filter == 1U) { s->plan.target_count = 0U; memset(s->plan.bssids, 0, sizeof(s->plan.bssids)); }
        if (!cerberus_wifi_capture_plan_valid(&s->plan)) return false;
        s->request.plan = &s->plan;
    }
    return true;
}

static void frame_ui_start(void)
{
    if (!frame_ui) frame_ui_allocate();
    frame_ui_store_t *s = frame_ui;
    if (!s || s->status.capture.busy || frame_ui_pending() || frame_ui_selecting) return;
    if (!frame_ui_supported()) { s->error = ESP_ERR_NOT_SUPPORTED; return; }
    if (!frame_ui_freeze(s)) { s->error = ESP_ERR_INVALID_ARG; return; }
    s->start_pending = s->barrier = true; s->error = ESP_OK;
    dual_wifi_live_cancel_handoff(); dual_wifi_ui_leave();
    sentinel_scan_session_select(&live_session, SENTINEL_SCAN_NONE);
    sentinel_scan_session_select(&live_ble_session, SENTINEL_SCAN_NONE);
}

static void frame_ui_choose_aps(void)
{
    frame_ui_store_t *s = frame_ui;
    if (!s || frame_ui_busy() || frame_ui_pending()) return;
    frame_ui_selecting = true; wifi_attack_selecting = true;
    dual_wifi_return_to_view = false;
    dual_wifi_live_cancel_handoff(); dual_wifi_ui_leave();
    wifi_mode = UI_WIFI_MODE_LIST;
    capture_picker_begin();
}

static void frame_ui_selection_done(void)
{
    if (!frame_ui_selecting) return;
    frame_ui_selecting = false; wifi_attack_selecting = false;
    sentinel_scan_session_select(&live_session, SENTINEL_SCAN_NONE);
    if (frame_ui && !frame_ui->exit) {
        frame_ui->scope = FRAME_SCOPE_SELECTED; frame_ui->view = FRAME_VIEW_OPTIONS;
        frame_ui->error = ESP_OK;
        wifi_mode = UI_WIFI_MODE_CAPTURE;
    }
}

static bool frame_ui_tick(uint32_t now, bool visible)
{
    frame_ui_store_t *s = frame_ui;
    if (!frame_ui_opened) return false;
    bool picking = current_page == UI_PAGE_WIFI && wifi_mode == UI_WIFI_MODE_LIST && frame_ui_selecting;
    bool here = current_page == UI_PAGE_WIFI && wifi_mode == UI_WIFI_MODE_CAPTURE;
    if (!s) {
        if (!here) frame_ui_opened = false;
        return false;
    }
    if (!visible) {
        frame_ui_stop();
        if (!here && !picking) {
            s->exit = FRAME_EXIT_HIDDEN;
            if (frame_ui_selecting) { frame_ui_selecting = false; wifi_attack_selecting = false; }
        }
    }
    if (s->barrier && eapol_ui_previous_released()) s->barrier = false;
    if (s->start_pending && !s->barrier && visible && !s->exit) {
        s->start_pending = false;
        s->error = frame_ui_supported() ? cerberus_s3_frame_start(&s->request) : ESP_ERR_NOT_SUPPORTED;
        if (s->error == ESP_OK) {
            s->started = true; s->offset = 0U; s->view = FRAME_VIEW_SUMMARY;
            s->run_scope = s->scope; s->run_filter = s->filter; s->run_channel = s->request.channel;
        }
    }
    /* Stop takes a bounded service lock. A missed request is retried on the
     * next UI tick; only a successful status/release proves cleanup ended. */
    if (s->stop_pending || s->exit) cerberus_s3_frame_stop();
    cerberus_s3_frame_status_t status;
    s->read_error = cerberus_s3_frame_status(&status);
    if (s->read_error == ESP_OK) {
        s->status = status;
        if (!status.capture.busy) s->stop_pending = false;
    }
    if (s->exit && !s->barrier) {
        esp_err_t error = cerberus_s3_frame_release();
        if (error == ESP_OK) {
            uint8_t exit = s->exit;
            if (here && (exit == FRAME_EXIT_BACK || exit == FRAME_EXIT_NEXT) && !frame_ui_from_menu) {
                wifi_selected_result = s->target; wifi_selected_index = s->target_index;
                wifi_selection_valid = true;
                dual_wifi_return_to_view = s->target_dual_return;
                capture_target = s->target; memcpy(capture_ssid, s->ssid, sizeof(capture_ssid));
            }
            heap_caps_free(s); frame_ui = NULL; frame_ui_opened = false;
            if (here) {
                if (exit == FRAME_EXIT_BACK) wifi_mode = frame_ui_from_menu ? UI_WIFI_MODE_RX_TOOLS : UI_WIFI_MODE_TARGET;
                else if (exit == FRAME_EXIT_NEXT) capture_mode_index = 3U;
            }
            return visible;
        }
        if (error != ESP_ERR_INVALID_STATE || !s->status.capture.busy) s->error = error;
    }
    if (visible && (!s->painted || (uint32_t)(now - s->painted_ms) >= 250U)) {
        s->painted = true; s->painted_ms = now; return true;
    }
    return false;
}

static void frame_ui_channel_step(frame_ui_store_t *s, bool next)
{
    unsigned index = 0U;
    for (; index < CERBERUS_WIFI_CAPTURE_PLAN_CHANNELS; ++index)
        if (cerberus_wifi_capture_plan_channel(index) == s->channel) break;
    index = next ? (index + 1U) % CERBERUS_WIFI_CAPTURE_PLAN_CHANNELS :
        (index + CERBERUS_WIFI_CAPTURE_PLAN_CHANNELS - 1U) % CERBERUS_WIFI_CAPTURE_PLAN_CHANNELS;
    s->channel = cerberus_wifi_capture_plan_channel(index);
}

static void frame_ui_tap(uint16_t x, uint16_t y)
{
    if (x < 12U || x >= 308U) return;
    frame_ui_store_t *s = frame_ui;
    if (s && s->exit) return;
    if (y >= 382U && y < 412U) {
        if (frame_ui_busy() || frame_ui_pending()) frame_ui_stop();
        else frame_ui_start();
        return;
    }
    if (!s) return;
    bool idle = !frame_ui_busy() && !frame_ui_pending();
    if (y >= 350U && y < 374U) {
        if (x < 156U && idle) s->view = FRAME_VIEW_OPTIONS;
        else if (x >= 164U) s->view = s->view == FRAME_VIEW_ROWS ? FRAME_VIEW_SUMMARY : FRAME_VIEW_ROWS;
    } else if (s->view == FRAME_VIEW_OPTIONS && idle) {
        if (y >= 202U && y < 230U) { s->filter = (uint8_t)((s->filter + 1U) % 3U); s->error = ESP_OK; }
        else if (y >= 236U && y < 264U) {
            s->scope = (uint8_t)((s->scope + 1U) % (frame_ui_from_menu ? 5U : 6U));
            s->error = ESP_OK;
        } else if (y >= 270U && y < 298U) {
            if (s->scope == FRAME_SCOPE_SELECTED) frame_ui_choose_aps();
            else if (s->scope == FRAME_SCOPE_FIXED && (x < 108U || x >= 212U)) frame_ui_channel_step(s, x >= 212U);
        } else if (y >= 304U && y < 332U) s->save = !s->save;
    } else if (s->view == FRAME_VIEW_ROWS && y >= 326U && y < 344U) {
        uint16_t *offset = &s->offset;
        if (x < 84U && *offset >= 2U) *offset -= 2U;
        else if (x >= 236U && *offset + 2U < s->page.total) *offset += 2U;
    }
}

static esp_err_t frame_ui_line(uint16_t y, const char *text, uint16_t color)
{
    char bounded[53]; snprintf(bounded, sizeof(bounded), "%.52s", text);
    return draw_centered(y, bounded, color, 1);
}
static const char *frame_ui_limit(uint32_t value) { return value == UINT32_MAX ? ">=" : ""; }
static unsigned frame_ui_bits(uint64_t value)
{ unsigned count = 0; while (value) { value &= value - 1U; ++count; } return count; }
static const char *frame_ui_scope_name(unsigned scope, unsigned filter)
{
    static const char *const names[] = {"ALL / 2.4 + 5 GHZ", "2.4 GHZ", "5 GHZ", "FIXED CHANNEL", "SELECTED APS", "THIS AP"};
    if (filter == 1U && scope == FRAME_SCOPE_SELECTED) return "SELECTED CHANNELS";
    if (filter == 1U && scope == FRAME_SCOPE_AP) return "AP CHANNEL";
    return scope < sizeof(names) / sizeof(names[0]) ? names[scope] : "UNKNOWN SCOPE";
}

static esp_err_t frame_ui_draw(void)
{
    frame_ui_store_t *s = frame_ui;
    char line[100], source[24], destination[24];
    unsigned filter = s ? (s->view == FRAME_VIEW_OPTIONS || !s->started ? s->filter : s->run_filter) : frame_ui_initial_filter;
    unsigned scope = s ? (s->view == FRAME_VIEW_OPTIONS || !s->started ? s->scope : s->run_scope) : frame_ui_from_menu ? FRAME_SCOPE_ALL : FRAME_SCOPE_AP;
    ESP_RETURN_ON_ERROR(draw_centered(54, "FRAME CAPTURE", UI_TEXT, 2), TAG, "frame title");
    snprintf(line, sizeof(line), "%s / %s", frame_ui_filter_names[filter], frame_ui_scope_name(scope, filter));
    ESP_RETURN_ON_ERROR(frame_ui_line(78, line, UI_TEXT), TAG, "frame scope");
    if (scope == FRAME_SCOPE_AP) snprintf(line, sizeof(line), "%.32s / CH %u", s ? s->ssid : capture_ssid, s ? s->target.channel : capture_target.channel);
    else if (scope == FRAME_SCOPE_SELECTED) snprintf(line, sizeof(line), filter == 1U ? "CHANNEL UNION / ALL PROBE ADDRESSES" : "COPIED BSSID LIST / CHANNEL UNION");
    else if (scope == FRAME_SCOPE_FIXED) snprintf(line, sizeof(line), "ALL ADDRESSES / CHANNEL %u", s ? (s->view == FRAME_VIEW_OPTIONS ? s->channel : s->run_channel) : 1U);
    else snprintf(line, sizeof(line), "ALL ADDRESSES / %u MS RECEIVE DWELL", filter == 0U ? CERBERUS_WIFI_CAPTURE_PLAN_RAW_DWELL_MS : CERBERUS_WIFI_CAPTURE_PLAN_DWELL_MS);
    ESP_RETURN_ON_ERROR(frame_ui_line(96, line, UI_MUTED), TAG, "frame target");
    ESP_RETURN_ON_ERROR(frame_ui_line(114, s && s->status.capture.save_pcap ? "C5 RX / S3 ANALYSIS / PCAP HAS NO TIMES" : "C5 RECEIVE / S3 DELIVERED-FRAME ANALYSIS", UI_TEXT), TAG, "frame roles");
    const cerberus_s3_capture_status_t *c = s ? &s->status.capture : NULL;
    esp_err_t error = !s ? ESP_ERR_NO_MEM : s->read_error != ESP_OK ? s->read_error : c->error_code != ESP_OK ? c->error_code : s->error;
    if (error == ESP_ERR_NOT_SUPPORTED) snprintf(line, sizeof(line), "REQUIRES C5 CAPABILITY %u", CERBERUS_WIFI_FRAME_SCOPE_MIN_FIRMWARE);
    else if (error == ESP_ERR_INVALID_ARG) snprintf(line, sizeof(line), "SELECT VALID APS / CHANNELS BEFORE START");
    else if (error != ESP_OK) snprintf(line, sizeof(line), "ERROR: %.32s", esp_err_to_name(error));
    else snprintf(line, sizeof(line), "%s", s->exit ? "STOPPING / RELEASING" : s->barrier ? "WAITING FOR RADIOS" :
        c->state == CERBERUS_S3_CAPTURE_STARTING ? "STARTING" : c->state == CERBERUS_S3_CAPTURE_STOPPING ? "STOPPING / DRAINING" :
        c->busy ? "RECEIVE ONLY / BOUNDED CAPTURE" : s->started ? "STOPPED / RESULTS RETAINED" : "READY / RECEIVE ONLY");
    ESP_RETURN_ON_ERROR(frame_ui_line(132, line, error == ESP_OK ? UI_GOOD : UI_BAD), TAG, "frame state");
    ESP_RETURN_ON_ERROR(cerberus_ui_widget_draw_button((cerberus_ui_rect_t){12,150,296,40}, frame_ui_from_menu ? "PASSIVE FRAME CAPTURE" : "FRAME CAPTURE / CHANGE MONITOR", 1,
        !frame_ui_from_menu && !capture_busy(), UI_WIDGET_THEME), TAG, "frame selector");
    if (!s) return cerberus_ui_widget_draw_button((cerberus_ui_rect_t){12,382,296,30}, "RETRY START", 1, true, UI_WIDGET_THEME);
    bool idle = !c->busy && !frame_ui_pending();
    if (s->view == FRAME_VIEW_OPTIONS) {
        snprintf(line, sizeof(line), "FILTER: %s >", frame_ui_filter_names[s->filter]);
        ESP_RETURN_ON_ERROR(cerberus_ui_widget_draw_button((cerberus_ui_rect_t){12,202,296,28}, line, 1, idle, UI_WIDGET_THEME), TAG, "frame filter");
        snprintf(line, sizeof(line), "SCOPE: %s >", frame_ui_scope_name(s->scope, s->filter));
        ESP_RETURN_ON_ERROR(cerberus_ui_widget_draw_button((cerberus_ui_rect_t){12,236,296,28}, line, 1, idle, UI_WIDGET_THEME), TAG, "frame scope control");
        if (s->scope == FRAME_SCOPE_FIXED) {
            ESP_RETURN_ON_ERROR(cerberus_ui_widget_draw_button((cerberus_ui_rect_t){12,270,96,28}, "CH -", 1, idle, UI_WIDGET_THEME), TAG, "frame previous channel");
            snprintf(line, sizeof(line), "CH %u", s->channel);
            ESP_RETURN_ON_ERROR(frame_ui_line(279, line, UI_TEXT), TAG, "frame channel");
            ESP_RETURN_ON_ERROR(cerberus_ui_widget_draw_button((cerberus_ui_rect_t){212,270,96,28}, "CH +", 1, idle, UI_WIDGET_THEME), TAG, "frame next channel");
        } else if (s->scope == FRAME_SCOPE_SELECTED) {
            snprintf(line, sizeof(line), "CHOOSE APS / %u SELECTED", wifi_target_count);
            ESP_RETURN_ON_ERROR(cerberus_ui_widget_draw_button((cerberus_ui_rect_t){12,270,296,28}, line, 1, idle, UI_WIDGET_THEME), TAG, "frame AP picker");
        } else ESP_RETURN_ON_ERROR(frame_ui_line(279, s->filter == 1U ? "CHANNEL-WIDE REQUESTS + RESPONSES" : s->scope == FRAME_SCOPE_AP ? "FIXED TO THE COPIED AP BSSID" : "COUNTRY / DRIVER LIMIT CHANNEL COVERAGE", UI_MUTED), TAG, "frame scope explanation");
        ESP_RETURN_ON_ERROR(cerberus_ui_widget_draw_button((cerberus_ui_rect_t){12,304,296,28}, s->save ? "SAVE PCAP ON" : "SAVE PCAP OFF", 1, idle, UI_WIDGET_THEME), TAG, "frame save");
        ESP_RETURN_ON_ERROR(frame_ui_line(338, s->save ? "SD REQUIRED / 64 MB LIMIT / NO FRAME TIMES" : "NO SD CARD REQUIRED", UI_MUTED), TAG, "frame storage");
    } else if (s->view == FRAME_VIEW_SUMMARY) {
        const cerberus_wifi_frame_summary_t *sum = &s->status.summary;
        snprintf(line, sizeof(line), "C5 ADMITTED %s%lu / QUEUED %u", frame_ui_limit(c->received), (unsigned long)c->received, c->pending);
        ESP_RETURN_ON_ERROR(frame_ui_line(202, line, UI_TEXT), TAG, "frame admitted");
        snprintf(line, sizeof(line), "S3 DELIVERED %s%lu / SD WRITTEN %s%lu", frame_ui_limit(sum->received), (unsigned long)sum->received, frame_ui_limit(c->written), (unsigned long)c->written);
        ESP_RETURN_ON_ERROR(frame_ui_line(214, line, UI_TEXT), TAG, "frame delivered");
        snprintf(line, sizeof(line), "C5 DROPS %s%lu + %s%lu", frame_ui_limit(c->dropped_full), (unsigned long)c->dropped_full, c->dropped_other >= UINT16_MAX ? ">=" : "", (unsigned long)c->dropped_other);
        ESP_RETURN_ON_ERROR(frame_ui_line(226, line, UI_MUTED), TAG, "frame drops");
        snprintf(line, sizeof(line), "MGMT %s%lu / DATA %s%lu", frame_ui_limit(sum->management), (unsigned long)sum->management, frame_ui_limit(sum->data), (unsigned long)sum->data);
        ESP_RETURN_ON_ERROR(frame_ui_line(238, line, UI_TEXT), TAG, "frame families");
        snprintf(line, sizeof(line), "PROBE REQ %s%lu / RSP %s%lu", frame_ui_limit(sum->probe_requests), (unsigned long)sum->probe_requests, frame_ui_limit(sum->probe_responses), (unsigned long)sum->probe_responses);
        ESP_RETURN_ON_ERROR(frame_ui_line(250, line, UI_MUTED), TAG, "frame probes");
        snprintf(line, sizeof(line), "BEACONS %s%lu / EAPOL %s%lu", frame_ui_limit(sum->beacons), (unsigned long)sum->beacons, frame_ui_limit(sum->eapol_headers), (unsigned long)sum->eapol_headers);
        ESP_RETURN_ON_ERROR(frame_ui_line(262, line, UI_MUTED), TAG, "frame beacon keys");
        snprintf(line, sizeof(line), "DEAUTH %s%lu / DISASSOC %s%lu", frame_ui_limit(sum->deauth), (unsigned long)sum->deauth, frame_ui_limit(sum->disassoc), (unsigned long)sum->disassoc);
        ESP_RETURN_ON_ERROR(frame_ui_line(274, line, UI_MUTED), TAG, "frame disconnects");
        if (sum->extrema_valid) snprintf(line, sizeof(line), "DELIVERED RSSI LOW %d / HIGH %d DBM", sum->min_rssi, sum->max_rssi);
        else snprintf(line, sizeof(line), "DELIVERED RSSI RANGE UNKNOWN");
        ESP_RETURN_ON_ERROR(frame_ui_line(286, line, UI_MUTED), TAG, "frame signal");
        if (c->planned && (!c->hop_valid || c->hop_error != ESP_OK)) snprintf(line, sizeof(line), "CHANNEL TELEMETRY UNAVAILABLE");
        else if (c->planned && !c->busy) snprintf(line, sizeof(line), "STOPPED / SWEEPS %s%u / COVERAGE RETAINED", c->completed_sweeps == UINT16_MAX ? ">=" : "", c->completed_sweeps);
        else if (c->planned && c->channel) snprintf(line, sizeof(line), "RX CHANNEL %u / SWEEPS %s%u", c->channel, c->completed_sweeps == UINT16_MAX ? ">=" : "", c->completed_sweeps);
        else if (c->planned) snprintf(line, sizeof(line), "TUNING / NO CURRENT RX CHANNEL");
        else snprintf(line, sizeof(line), c->busy ? "FIXED RX CHANNEL %u" : "FIXED SCOPE CHANNEL %u", s->status.channel);
        ESP_RETURN_ON_ERROR(frame_ui_line(298, line, c->planned && !c->hop_valid ? UI_BAD : UI_TEXT), TAG, "frame coverage state");
        if (c->planned && c->hop_valid && c->hop_error == ESP_OK) snprintf(line, sizeof(line), "THIS SWEEP: TUNED %u / TRIED %u / SKIPPED %u", frame_ui_bits(c->applied_mask), frame_ui_bits(c->attempted_mask), frame_ui_bits(c->attempted_mask & ~c->applied_mask));
        else snprintf(line, sizeof(line), "RETRIES %s%lu / MALFORMED %s%lu", frame_ui_limit(sum->retries), (unsigned long)sum->retries, frame_ui_limit(sum->malformed), (unsigned long)sum->malformed);
        ESP_RETURN_ON_ERROR(frame_ui_line(310, line, UI_MUTED), TAG, "frame coverage counts");
        snprintf(line, sizeof(line), "CONTROL %s%lu / OTHER %s%lu", frame_ui_limit(sum->control), (unsigned long)sum->control, frame_ui_limit(sum->other), (unsigned long)sum->other);
        ESP_RETURN_ON_ERROR(frame_ui_line(322, line, UI_MUTED), TAG, "frame other counts");
        if (c->file_name[0]) snprintf(line, sizeof(line), "FILE %.31s", c->file_name);
        else snprintf(line, sizeof(line), "PROVENANCE REJECTED %s%lu", frame_ui_limit(s->status.rejected_provenance), (unsigned long)s->status.rejected_provenance);
        ESP_RETURN_ON_ERROR(frame_ui_line(334, line, UI_MUTED), TAG, "frame file");
    } else {
        esp_err_t result = cerberus_s3_frame_page(s->offset, s->rows, 2U, &s->page);
        bool valid = result == ESP_OK && s->page.generation == s->status.generation;
        if (!valid || !s->page.copied) ESP_RETURN_ON_ERROR(frame_ui_line(248, result != ESP_OK ? "EVENT COPY UNAVAILABLE" : !valid ? "SESSION CHANGED / REFRESHING" : "NO DISCONNECT HEADERS OBSERVED", UI_MUTED), TAG, "frame empty events");
        else for (unsigned i = 0; i < s->page.copied; ++i) {
            const cerberus_s3_frame_row_t *row = &s->rows[i];
            const cerberus_wifi_frame_record_t *r = &row->evidence;
            uint16_t top = (uint16_t)(202U + 60U * i);
            cerberus_privacy_format_wifi_identifier(r->transmitter, privacy_addresses_masked(), source, sizeof(source));
            cerberus_privacy_format_wifi_identifier(r->receiver, privacy_addresses_masked(), destination, sizeof(destination));
            snprintf(line, sizeof(line), "%s > %s", source, destination);
            ESP_RETURN_ON_ERROR(frame_ui_line(top, line, UI_TEXT), TAG, "frame addresses");
            cerberus_privacy_format_wifi_identifier(r->bssid, privacy_addresses_masked(), source, sizeof(source));
            snprintf(line, sizeof(line), "AP %s / %s", source, r->subtype == 12U ? "DEAUTH" : "DISASSOC");
            ESP_RETURN_ON_ERROR(frame_ui_line(top + 11U, line, UI_MUTED), TAG, "frame event family");
            if (r->protected_frame || r->fragmented || !r->reason_known) snprintf(line, sizeof(line), "REASON UNKNOWN / %s", r->protected_frame ? "PROTECTED" : r->fragmented ? "FRAGMENTED" : "UNDECODED BODY");
            else snprintf(line, sizeof(line), "REASON %u / OBSERVED %s%lu", r->reason, frame_ui_limit(r->frames), (unsigned long)r->frames);
            ESP_RETURN_ON_ERROR(frame_ui_line(top + 22U, line, UI_MUTED), TAG, "frame reason");
            if (r->rssi_valid) snprintf(line, sizeof(line), "C5 CH %u / %d DBM / RETRIES %s%lu", r->channel, r->rssi, frame_ui_limit(r->retries), (unsigned long)r->retries);
            else snprintf(line, sizeof(line), "C5 CH %u / RSSI UNKNOWN", r->channel);
            ESP_RETURN_ON_ERROR(frame_ui_line(top + 33U, line, UI_MUTED), TAG, "frame event receive");
            if (row->last_received_age_ms == UINT32_MAX) snprintf(line, sizeof(line), "S3 DELIVERY AGE UNKNOWN");
            else snprintf(line, sizeof(line), "S3 DELIVERY %lu.%03luS AGO", (unsigned long)(row->last_received_age_ms / 1000U), (unsigned long)(row->last_received_age_ms % 1000U));
            ESP_RETURN_ON_ERROR(frame_ui_line(top + 44U, line, UI_MUTED), TAG, "frame event age");
        }
        snprintf(line, sizeof(line), "REPLACED %s%lu / LIMIT %u", frame_ui_limit(s->status.summary.event_replaced), (unsigned long)s->status.summary.event_replaced, CERBERUS_WIFI_FRAME_MAX_EVENTS);
        ESP_RETURN_ON_ERROR(frame_ui_line(316, line, UI_MUTED), TAG, "frame event bounds");
        ESP_RETURN_ON_ERROR(cerberus_ui_widget_draw_button((cerberus_ui_rect_t){12,326,72,18}, "PREV", 1, s->offset >= 2U, UI_WIDGET_THEME), TAG, "frame prev");
        snprintf(line, sizeof(line), "%u / %u EVENTS", s->offset + s->page.copied, s->page.total);
        ESP_RETURN_ON_ERROR(frame_ui_line(331, line, UI_MUTED), TAG, "frame page");
        ESP_RETURN_ON_ERROR(cerberus_ui_widget_draw_button((cerberus_ui_rect_t){236,326,72,18}, "NEXT", 1, valid && s->offset + 2U < s->page.total, UI_WIDGET_THEME), TAG, "frame next");
    }
    ESP_RETURN_ON_ERROR(cerberus_ui_widget_draw_button((cerberus_ui_rect_t){12,350,144,24}, "OPTIONS", 1, idle, UI_WIDGET_THEME), TAG, "frame options");
    ESP_RETURN_ON_ERROR(cerberus_ui_widget_draw_button((cerberus_ui_rect_t){164,350,144,24}, s->view == FRAME_VIEW_ROWS ? "SUMMARY" : "EVENTS", 1, !s->exit, UI_WIDGET_THEME), TAG, "frame events");
    return cerberus_ui_widget_draw_button((cerberus_ui_rect_t){12,382,296,30}, c->busy || s->start_pending || s->stop_pending || s->barrier ? "STOP" : s->started ? "RESTART" : "START", 1,
        !s->exit && (c->busy || s->start_pending || s->stop_pending || s->barrier || frame_ui_supported()), UI_WIDGET_THEME);
}
