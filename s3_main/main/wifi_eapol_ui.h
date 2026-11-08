#pragma once

/* One passive monitor reached from the Wi-Fi menu or the AP capture selector.
 * The shared capture worker owns reception; this page owns copied scope and
 * presentation only. A Stop retains evidence until explicit release. */
#include "wifi_eapol_session.h"
#include "esp_heap_caps.h"

enum { EAPOL_SCOPE_ALL, EAPOL_SCOPE_2G, EAPOL_SCOPE_5G, EAPOL_SCOPE_FIXED, EAPOL_SCOPE_SELECTED, EAPOL_SCOPE_AP };
enum { EAPOL_VIEW_OPTIONS, EAPOL_VIEW_SUMMARY, EAPOL_VIEW_ROWS, EAPOL_VIEW_CONTEXT };
enum { EAPOL_EXIT_STAY, EAPOL_EXIT_NEXT, EAPOL_EXIT_BACK, EAPOL_EXIT_HIDDEN };
typedef struct {
    cerberus_c5_wifi_result_t target;
    char ssid[33];
    cerberus_wifi_capture_plan_t plan;
    cerberus_s3_eapol_options_t request;
    cerberus_s3_eapol_status_t status;
    cerberus_s3_eapol_row_t rows[2];
    cerberus_s3_eapol_context_row_t contexts[2];
    cerberus_s3_eapol_page_t page;
    uint32_t painted_ms;
    uint16_t offset, context_offset;
    esp_err_t error, read_error;
    uint8_t scope, run_scope, channel, run_channel, view, exit, target_index;
    bool save, started, start_pending, stop_pending, barrier, painted, target_dual_return;
} eapol_ui_store_t;
static eapol_ui_store_t *eapol_ui;
static bool eapol_ui_opened, eapol_ui_from_menu;

static bool eapol_ui_is_open(void) { return eapol_ui_opened; }
static bool eapol_ui_busy(void) { return eapol_ui && eapol_ui->status.capture.busy; }
static bool eapol_ui_pending(void)
{ return eapol_ui && (eapol_ui->start_pending || eapol_ui->stop_pending || eapol_ui->barrier || eapol_ui->exit); }
static bool eapol_ui_supported(void)
{ return c5_online && c5_status.firmware_version >= CERBERUS_WIFI_EAPOL_PLAN_MIN_FIRMWARE; }
static const char *eapol_ui_back_label(void)
{ return eapol_ui_pending() && eapol_ui && eapol_ui->exit ? "STOPPING" : eapol_ui_from_menu ? "AUTH MENU" : "TARGET"; }

static void eapol_ui_allocate(void)
{
    if (eapol_ui) return;
    eapol_ui = heap_caps_calloc(1U, sizeof(*eapol_ui), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!eapol_ui) return;
    eapol_ui->view = EAPOL_VIEW_OPTIONS;
    eapol_ui->scope = eapol_ui_from_menu ? EAPOL_SCOPE_ALL : EAPOL_SCOPE_AP;
    eapol_ui->channel = eapol_ui_from_menu ? 1U : capture_target.channel;
    if (!eapol_ui_from_menu) {
        eapol_ui->target = capture_target;
        eapol_ui->target_index = wifi_selected_index;
        eapol_ui->target_dual_return = dual_wifi_return_to_view;
        memcpy(eapol_ui->ssid, capture_ssid, sizeof(eapol_ui->ssid));
    }
}

static void eapol_ui_open(bool menu)
{
    if (eapol_ui_opened || sae_ui_is_open()) return;
    eapol_ui_opened = true; eapol_ui_from_menu = menu;
    eapol_ui_allocate();
    capture_mode_index = 3U;
    wifi_mode = UI_WIFI_MODE_CAPTURE;
}

static bool eapol_ui_previous_released(void)
{
    cerberus_dual_wifi_status_t dual;
    cerberus_s3_capture_status_t capture;
    cerberus_dual_wifi_scan_get_status(&dual);
    cerberus_s3_capture_status(&capture);
    return dual_wifi_live_released() && !dual_wifi_ui_busy(&dual) && !capture.busy;
}


static void eapol_ui_stop(void)
{
    if (!eapol_ui) return;
    eapol_ui->start_pending = false;
    eapol_ui->stop_pending = true;
    cerberus_s3_eapol_stop();
}

static void eapol_ui_close(bool back)
{
    /* The targetless shortcut has no unrelated generic capture scope. */
    if (!back && eapol_ui_from_menu) return;
    if (!eapol_ui) {
        eapol_ui_opened = false;
        if (back) wifi_mode = eapol_ui_from_menu ? UI_WIFI_MODE_AUTH : UI_WIFI_MODE_TARGET;
        else capture_mode_index = 4U;
        return;
    }
    if (back || !eapol_ui->exit) eapol_ui->exit = back ? EAPOL_EXIT_BACK : EAPOL_EXIT_NEXT;
    eapol_ui_stop();
}

static bool eapol_ui_freeze(eapol_ui_store_t *s)
{
    cerberus_wifi_capture_plan_clear(&s->plan);
    s->plan.dwell_ms = CERBERUS_WIFI_CAPTURE_PLAN_EAPOL_DWELL_MS;
    s->request = (cerberus_s3_eapol_options_t){.save_pcap = s->save};
    if (s->scope == EAPOL_SCOPE_FIXED || s->scope == EAPOL_SCOPE_AP) {
        s->request.channel = s->scope == EAPOL_SCOPE_AP ? s->target.channel : s->channel;
        if (!cerberus_wifi_capture_plan_channel_bit(s->request.channel)) return false;
        s->request.filter_bssid = s->scope == EAPOL_SCOPE_AP;
        if (s->request.filter_bssid) memcpy(s->request.bssid, s->target.bssid, 6U);
    } else {
        if (s->scope == EAPOL_SCOPE_SELECTED) {
            if (!wifi_target_count) return false;
            for (unsigned i = 0; i < wifi_target_count; ++i)
                if (!cerberus_wifi_capture_plan_add_target(&s->plan,
                    wifi_targets[i].ap.bssid, wifi_targets[i].ap.channel)) return false;
        } else s->plan.channel_mask = cerberus_wifi_capture_plan_band_mask(s->scope);
        if (!cerberus_wifi_capture_plan_valid(&s->plan)) return false;
        s->request.plan = &s->plan;
    }
    return true;
}

static void eapol_ui_start(void)
{
    if (!eapol_ui) eapol_ui_allocate();
    eapol_ui_store_t *s = eapol_ui;
    if (!s || s->status.capture.busy || eapol_ui_pending() || eapol_ui_selecting) return;
    if (!eapol_ui_supported()) { s->error = ESP_ERR_NOT_SUPPORTED; return; }
    if (!eapol_ui_freeze(s)) { s->error = ESP_ERR_INVALID_ARG; return; }
    s->start_pending = s->barrier = true; s->error = ESP_OK;
    dual_wifi_live_cancel_handoff(); dual_wifi_ui_leave();
    sentinel_scan_session_select(&live_session, SENTINEL_SCAN_NONE);
    sentinel_scan_session_select(&live_ble_session, SENTINEL_SCAN_NONE);
}

static void eapol_ui_choose_aps(void)
{
    eapol_ui_store_t *s = eapol_ui;
    if (!s || eapol_ui_busy() || eapol_ui_pending()) return;
    eapol_ui_selecting = true; wifi_attack_selecting = true;
    dual_wifi_return_to_view = false;
    dual_wifi_live_cancel_handoff(); dual_wifi_ui_leave();
    wifi_mode = UI_WIFI_MODE_LIST;
    auth_monitor_picker_begin();
}

static void eapol_ui_selection_done(void)
{
    if (!eapol_ui_selecting) return;
    eapol_ui_selecting = false; wifi_attack_selecting = false;
    sentinel_scan_session_select(&live_session, SENTINEL_SCAN_NONE);
    if (eapol_ui && !eapol_ui->exit) {
        eapol_ui->scope = EAPOL_SCOPE_SELECTED; eapol_ui->view = EAPOL_VIEW_OPTIONS;
        eapol_ui->error = ESP_OK;
        wifi_mode = UI_WIFI_MODE_CAPTURE;
    }
}

static bool eapol_ui_tick(uint32_t now, bool visible)
{
    eapol_ui_store_t *s = eapol_ui;
    if (!eapol_ui_opened) return false;
    bool picking = current_page == UI_PAGE_WIFI && wifi_mode == UI_WIFI_MODE_LIST && eapol_ui_selecting;
    bool here = current_page == UI_PAGE_WIFI && wifi_mode == UI_WIFI_MODE_CAPTURE;
    if (!s) {
        if (!here) eapol_ui_opened = false;
        return false;
    }
    if (!visible) {
        eapol_ui_stop();
        if (!here && !picking) {
            s->exit = EAPOL_EXIT_HIDDEN;
            if (eapol_ui_selecting) { eapol_ui_selecting = false; wifi_attack_selecting = false; }
        }
    }
    if (s->barrier && eapol_ui_previous_released()) s->barrier = false;
    if (s->start_pending && !s->barrier && visible && !s->exit) {
        s->start_pending = false;
        s->error = eapol_ui_supported() ? cerberus_s3_eapol_start_options(&s->request) : ESP_ERR_NOT_SUPPORTED;
        if (s->error == ESP_OK) {
            s->started = true; s->offset = s->context_offset = 0U; s->view = EAPOL_VIEW_SUMMARY;
            s->run_scope = s->scope; s->run_channel = s->request.channel;
        }
    }
    /* Stop takes a bounded service lock. A missed request is retried on the
     * next UI tick; only a successful status/release proves cleanup ended. */
    if (s->stop_pending || s->exit) cerberus_s3_eapol_stop();
    cerberus_s3_eapol_status_t status;
    s->read_error = cerberus_s3_eapol_status(&status);
    if (s->read_error == ESP_OK) {
        s->status = status;
        if (!status.capture.busy) s->stop_pending = false;
    }
    if (s->exit && !s->barrier) {
        esp_err_t error = cerberus_s3_eapol_release();
        if (error == ESP_OK) {
            uint8_t exit = s->exit;
            if (here && (exit == EAPOL_EXIT_BACK || exit == EAPOL_EXIT_NEXT) && !eapol_ui_from_menu) {
                wifi_selected_result = s->target; wifi_selected_index = s->target_index;
                wifi_selection_valid = true;
                dual_wifi_return_to_view = s->target_dual_return;
                capture_target = s->target; memcpy(capture_ssid, s->ssid, sizeof(capture_ssid));
            }
            heap_caps_free(s); eapol_ui = NULL; eapol_ui_opened = false;
            capture_has_last_status = false;
            if (here) {
                if (exit == EAPOL_EXIT_BACK) wifi_mode = eapol_ui_from_menu ? UI_WIFI_MODE_AUTH : UI_WIFI_MODE_TARGET;
                else if (exit == EAPOL_EXIT_NEXT) capture_mode_index = 4U;
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

static void eapol_ui_channel_step(eapol_ui_store_t *s, bool next)
{
    unsigned index = 0U;
    for (; index < CERBERUS_WIFI_CAPTURE_PLAN_CHANNELS; ++index)
        if (cerberus_wifi_capture_plan_channel(index) == s->channel) break;
    index = next ? (index + 1U) % CERBERUS_WIFI_CAPTURE_PLAN_CHANNELS :
        (index + CERBERUS_WIFI_CAPTURE_PLAN_CHANNELS - 1U) % CERBERUS_WIFI_CAPTURE_PLAN_CHANNELS;
    s->channel = cerberus_wifi_capture_plan_channel(index);
}

static void eapol_ui_tap(uint16_t x, uint16_t y)
{
    if (x < 12U || x >= 308U) return;
    eapol_ui_store_t *s = eapol_ui;
    if (s && s->exit) return;
    if (y >= 382U && y < 412U) {
        if (eapol_ui_busy() || eapol_ui_pending()) eapol_ui_stop();
        else eapol_ui_start();
        return;
    }
    if (!s) return;
    bool idle = !eapol_ui_busy() && !eapol_ui_pending();
    if (y >= 350U && y < 374U) {
        if (x < 108U && idle) s->view = EAPOL_VIEW_OPTIONS;
        else if (x >= 112U && x < 208U) s->view = s->view == EAPOL_VIEW_ROWS ? EAPOL_VIEW_SUMMARY : EAPOL_VIEW_ROWS;
        else if (x >= 212U) s->view = s->view == EAPOL_VIEW_CONTEXT ? EAPOL_VIEW_SUMMARY : EAPOL_VIEW_CONTEXT;
    } else if (s->view == EAPOL_VIEW_OPTIONS && idle) {
        if (y >= 216U && y < 244U) {
            s->scope = (uint8_t)((s->scope + 1U) % (eapol_ui_from_menu ? 5U : 6U));
            s->error = ESP_OK;
        } else if (y >= 252U && y < 280U) {
            if (s->scope == EAPOL_SCOPE_SELECTED) eapol_ui_choose_aps();
            else if (s->scope == EAPOL_SCOPE_FIXED && (x < 108U || x >= 212U)) eapol_ui_channel_step(s, x >= 212U);
        } else if (y >= 292U && y < 320U) s->save = !s->save;
    } else if ((s->view == EAPOL_VIEW_ROWS || s->view == EAPOL_VIEW_CONTEXT) && y >= 326U && y < 344U) {
        uint16_t *offset = s->view == EAPOL_VIEW_ROWS ? &s->offset : &s->context_offset;
        if (x < 84U && *offset >= 2U) *offset -= 2U;
        else if (x >= 236U && *offset + 2U < s->page.total) *offset += 2U;
    }
}

static esp_err_t eapol_ui_line(uint16_t y, const char *text, uint16_t color)
{
    char bounded[49]; snprintf(bounded, sizeof(bounded), "%.48s", text);
    return draw_centered(y, bounded, color, 1);
}
static const char *eapol_ui_limit(uint32_t value) { return value == UINT32_MAX ? ">=" : ""; }
static unsigned eapol_ui_bits(uint64_t value)
{ unsigned count = 0; while (value) { value &= value - 1U; ++count; } return count; }
static const char *eapol_ui_scope_name(unsigned scope)
{
    static const char *const names[] = {"ALL / 2.4 + 5 GHZ", "2.4 GHZ", "5 GHZ", "FIXED CHANNEL", "SELECTED APS", "THIS AP"};
    return scope < sizeof(names) / sizeof(names[0]) ? names[scope] : "UNKNOWN SCOPE";
}

/* Render raw SSID bytes without interpreting controls or invalid UTF-8. */
static void eapol_ui_ssid(const cerberus_wifi_eapol_context_t *context, char out[49])
{
    if (privacy_addresses_masked()) { snprintf(out, 49U, "SSID HIDDEN BY PRIVACY"); return; }
    if (!context->ssid_length) { snprintf(out, 49U, "SSID EMPTY / HIDDEN-LIKE"); return; }
    size_t used = 0U;
    memcpy(out, "SSID ", 5U); used = 5U;
    for (unsigned i = 0; i < context->ssid_length && i < sizeof(context->ssid); ++i) {
        uint8_t byte = context->ssid[i];
        size_t need = byte >= 32U && byte <= 126U && byte != '\\' ? 1U : 4U;
        if (used + need > 45U) { memcpy(out + used, "...", 3U); used += 3U; break; }
        if (need == 1U) out[used++] = (char)byte;
        else { snprintf(out + used, 49U - used, "\\x%02X", byte); used += 4U; }
    }
    out[used] = '\0';
}

static esp_err_t eapol_ui_draw(void)
{
    eapol_ui_store_t *s = eapol_ui;
    char line[100], source[24], destination[24];
    ESP_RETURN_ON_ERROR(draw_centered(54, "EAPOL MONITOR", UI_TEXT, 2), TAG, "EAPOL title");
    unsigned scope = s ? (s->view == EAPOL_VIEW_OPTIONS || !s->started ? s->scope : s->run_scope) :
        eapol_ui_from_menu ? EAPOL_SCOPE_ALL : EAPOL_SCOPE_AP;
    snprintf(line, sizeof(line), "%s%s", s && s->view == EAPOL_VIEW_OPTIONS && s->started ? "NEXT: " : "SCOPE: ", eapol_ui_scope_name(scope));
    ESP_RETURN_ON_ERROR(eapol_ui_line(78, line, UI_TEXT), TAG, "EAPOL scope");
    if (scope == EAPOL_SCOPE_AP) snprintf(line, sizeof(line), "%.32s / CH %u", s ? s->ssid : capture_ssid,
        s ? s->target.channel : capture_target.channel);
    else if (scope == EAPOL_SCOPE_SELECTED) snprintf(line, sizeof(line), "%u AP TARGETS / CHANNEL UNION",
        s && s->started && s->view != EAPOL_VIEW_OPTIONS ? s->status.plan.target_count : wifi_target_count);
    else if (scope == EAPOL_SCOPE_FIXED) snprintf(line, sizeof(line), "ALL BSSIDS / CHANNEL %u",
        s ? (s->view == EAPOL_VIEW_OPTIONS ? s->channel : s->run_channel) : 1U);
    else snprintf(line, sizeof(line), "ALL BSSIDS / %u MS SUCCESSFUL DWELL", CERBERUS_WIFI_CAPTURE_PLAN_EAPOL_DWELL_MS);
    ESP_RETURN_ON_ERROR(eapol_ui_line(96, line, UI_MUTED), TAG, "EAPOL target");
    ESP_RETURN_ON_ERROR(eapol_ui_line(114, s && s->view == EAPOL_VIEW_CONTEXT ? "BEACON CONTEXT / UNAUTHENTICATED" : s && s->status.capture.save_pcap ?
        "C5 RX / S3 ANALYSIS / PCAP HAS NO TIMES" : "C5 RECEIVE / S3 ANALYSIS", UI_TEXT), TAG, "EAPOL roles");
    const cerberus_s3_capture_status_t *c = s ? &s->status.capture : NULL;
    esp_err_t error = !s ? ESP_ERR_NO_MEM : s->read_error != ESP_OK ? s->read_error :
        c->error_code != ESP_OK ? c->error_code : s->error;
    if (error == ESP_ERR_NOT_SUPPORTED) snprintf(line, sizeof(line), "REQUIRES C5 CAPABILITY %u", CERBERUS_WIFI_EAPOL_PLAN_MIN_FIRMWARE);
    else if (error == ESP_ERR_INVALID_ARG) snprintf(line, sizeof(line), "SELECT VALID APS / CHANNELS BEFORE START");
    else if (error != ESP_OK) snprintf(line, sizeof(line), "%s: %.32s", c && c->busy ? "STOPPING" : "ERROR", esp_err_to_name(error));
    else snprintf(line, sizeof(line), "%s", s->exit ? "STOPPING / RELEASING" : s->barrier ? "WAITING FOR RADIOS" :
        c->state == CERBERUS_S3_CAPTURE_STARTING ? "STARTING" :
        c->state == CERBERUS_S3_CAPTURE_STOPPING ? "STOPPING / DRAINING" : c->busy ? "LISTENING / RECEIVE ONLY" :
        s->started ? "STOPPED / RESULTS RETAINED" : "READY / RECEIVE ONLY");
    ESP_RETURN_ON_ERROR(eapol_ui_line(132, line, error == ESP_OK ? UI_GOOD : UI_BAD), TAG, "EAPOL state");
    ESP_RETURN_ON_ERROR(cerberus_ui_widget_draw_button((cerberus_ui_rect_t){12,150,296,40},
        eapol_ui_from_menu ? "PASSIVE AUTHENTICATION MONITOR" : "EAPOL MONITOR / CHANGE FILTER", 1,
        !eapol_ui_from_menu && !capture_busy(), UI_WIDGET_THEME), TAG, "EAPOL selector");
    if (!s) return cerberus_ui_widget_draw_button((cerberus_ui_rect_t){12,382,296,30}, "RETRY START", 1, true, UI_WIDGET_THEME);
    bool idle = !c->busy && !eapol_ui_pending();
    if (s->view == EAPOL_VIEW_OPTIONS) {
        ESP_RETURN_ON_ERROR(eapol_ui_line(202, "SCOPE AND SAVE APPLY TO THE NEXT START", UI_MUTED), TAG, "EAPOL next");
        snprintf(line, sizeof(line), "SCOPE: %s >", eapol_ui_scope_name(s->scope));
        ESP_RETURN_ON_ERROR(cerberus_ui_widget_draw_button((cerberus_ui_rect_t){12,216,296,28}, line, 1, idle, UI_WIDGET_THEME), TAG, "EAPOL scope control");
        if (s->scope == EAPOL_SCOPE_FIXED) {
            ESP_RETURN_ON_ERROR(cerberus_ui_widget_draw_button((cerberus_ui_rect_t){12,252,96,28}, "CH -", 1, idle, UI_WIDGET_THEME), TAG, "EAPOL channel previous");
            snprintf(line, sizeof(line), "CH %u", s->channel);
            ESP_RETURN_ON_ERROR(eapol_ui_line(261, line, UI_TEXT), TAG, "EAPOL fixed channel");
            ESP_RETURN_ON_ERROR(cerberus_ui_widget_draw_button((cerberus_ui_rect_t){212,252,96,28}, "CH +", 1, idle, UI_WIDGET_THEME), TAG, "EAPOL channel next");
        } else if (s->scope == EAPOL_SCOPE_SELECTED) {
            snprintf(line, sizeof(line), "CHOOSE APS / %u SELECTED", wifi_target_count);
            ESP_RETURN_ON_ERROR(cerberus_ui_widget_draw_button((cerberus_ui_rect_t){12,252,296,28}, line, 1, idle, UI_WIDGET_THEME), TAG, "EAPOL choose targets");
        } else ESP_RETURN_ON_ERROR(eapol_ui_line(261, s->scope == EAPOL_SCOPE_AP ? "FIXED TO THE COPIED AP BSSID" :
            "COUNTRY / DRIVER DECIDE AVAILABLE CHANNELS", UI_MUTED), TAG, "EAPOL channel scope");
        ESP_RETURN_ON_ERROR(cerberus_ui_widget_draw_button((cerberus_ui_rect_t){12,292,296,28},
            s->save ? "SAVE PCAP ON" : "SAVE PCAP OFF", 1, idle, UI_WIDGET_THEME), TAG, "EAPOL save");
        ESP_RETURN_ON_ERROR(eapol_ui_line(331, s->save ? "PCAPNG / NO UTC OR FRAME TIMES" : "NO SD CARD REQUIRED", UI_MUTED), TAG, "EAPOL storage");
    } else if (s->view == EAPOL_VIEW_SUMMARY) {
        const cerberus_wifi_eapol_summary_t *sum = &s->status.summary;
        snprintf(line, sizeof(line), "M1 %s%lu / M2 %s%lu", eapol_ui_limit(sum->phase[0]), (unsigned long)sum->phase[0], eapol_ui_limit(sum->phase[1]), (unsigned long)sum->phase[1]);
        ESP_RETURN_ON_ERROR(eapol_ui_line(202, line, UI_TEXT), TAG, "EAPOL first phases");
        snprintf(line, sizeof(line), "M3 %s%lu / M4 %s%lu", eapol_ui_limit(sum->phase[2]), (unsigned long)sum->phase[2], eapol_ui_limit(sum->phase[3]), (unsigned long)sum->phase[3]);
        ESP_RETURN_ON_ERROR(eapol_ui_line(216, line, UI_TEXT), TAG, "EAPOL last phases");
        snprintf(line, sizeof(line), "OBSERVED EXCHANGES %s%lu / PAIRS %u", eapol_ui_limit(sum->exchanges), (unsigned long)sum->exchanges, sum->count);
        ESP_RETURN_ON_ERROR(eapol_ui_line(230, line, UI_TEXT), TAG, "EAPOL exchanges");
        snprintf(line, sizeof(line), "PMKID KDE FRAMES %s%lu", eapol_ui_limit(sum->pmkid_frames), (unsigned long)sum->pmkid_frames);
        ESP_RETURN_ON_ERROR(eapol_ui_line(244, line, UI_MUTED), TAG, "EAPOL evidence");
        snprintf(line, sizeof(line), "RX %s%lu / DROP %s%lu + %s%lu", eapol_ui_limit(c->received), (unsigned long)c->received,
            eapol_ui_limit(c->dropped_full), (unsigned long)c->dropped_full, c->dropped_other >= UINT16_MAX ? ">=" : "", (unsigned long)c->dropped_other);
        ESP_RETURN_ON_ERROR(eapol_ui_line(258, line, UI_MUTED), TAG, "EAPOL capture counts");
        if (c->planned && (!c->hop_valid || c->hop_error != ESP_OK)) snprintf(line, sizeof(line), "CHANNEL TELEMETRY UNAVAILABLE%s", c->hop_error ? " / RETRYING" : "");
        else if (c->planned && !c->busy) snprintf(line, sizeof(line), "STOPPED / SWEEPS %s%u / COVERAGE RETAINED", c->completed_sweeps == UINT16_MAX ? ">=" : "", c->completed_sweeps);
        else if (c->planned && c->channel) snprintf(line, sizeof(line), "RX CHANNEL %u / SWEEPS %s%u", c->channel, c->completed_sweeps == UINT16_MAX ? ">=" : "", c->completed_sweeps);
        else if (c->planned) snprintf(line, sizeof(line), "TUNING / NO CURRENT RX CHANNEL");
        else if (s->started) snprintf(line, sizeof(line), c->busy ? "FIXED RX CHANNEL %u" : "FIXED SCOPE CHANNEL %u", s->status.channel);
        else snprintf(line, sizeof(line), "NO RECEIVE SESSION STARTED");
        ESP_RETURN_ON_ERROR(eapol_ui_line(272, line, c->planned && !c->hop_valid ? UI_BAD : UI_TEXT), TAG, "EAPOL channels");
        if (c->planned && c->hop_valid && c->hop_error == ESP_OK) snprintf(line, sizeof(line), "THIS SWEEP: TUNED %u / TRIED %u / SKIPPED %u",
            eapol_ui_bits(c->applied_mask), eapol_ui_bits(c->attempted_mask), eapol_ui_bits(c->attempted_mask & ~c->applied_mask));
        else snprintf(line, sizeof(line), "RETRY %s%lu / MALFORMED %s%lu", eapol_ui_limit(sum->retries), (unsigned long)sum->retries, eapol_ui_limit(sum->malformed), (unsigned long)sum->malformed);
        ESP_RETURN_ON_ERROR(eapol_ui_line(286, line, UI_MUTED), TAG, "EAPOL coverage");
        if (c->save_pcap) snprintf(line, sizeof(line), "SD WRITTEN %s%lu / %llu KB", eapol_ui_limit(c->written), (unsigned long)c->written, (unsigned long long)(c->bytes_written / 1024U));
        else snprintf(line, sizeof(line), "PAIR LIMIT %s%lu / BEACON LIMIT %s%lu", eapol_ui_limit(sum->peer_dropped), (unsigned long)sum->peer_dropped, eapol_ui_limit(sum->beacon_dropped), (unsigned long)sum->beacon_dropped);
        ESP_RETURN_ON_ERROR(eapol_ui_line(300, line, UI_MUTED), TAG, "EAPOL storage");
        ESP_RETURN_ON_ERROR(eapol_ui_line(316, "OBSERVED SEQUENCES / NOT VERIFIED AUTH", UI_MUTED), TAG, "EAPOL interpretation");
        if (c->file_name[0]) snprintf(line, sizeof(line), "FILE %.31s", c->file_name);
        else snprintf(line, sizeof(line), "PROVENANCE REJECTED %s%lu", eapol_ui_limit(s->status.rejected_provenance), (unsigned long)s->status.rejected_provenance);
        ESP_RETURN_ON_ERROR(eapol_ui_line(332, line, UI_MUTED), TAG, "EAPOL file");
    } else {
        bool context = s->view == EAPOL_VIEW_CONTEXT;
        uint16_t offset = context ? s->context_offset : s->offset;
        esp_err_t result = context ? cerberus_s3_eapol_context_page(offset, s->contexts, 2U, &s->page) :
            cerberus_s3_eapol_page(offset, s->rows, 2U, &s->page);
        bool valid = result == ESP_OK && s->page.generation == s->status.generation;
        if (!valid || !s->page.copied) {
            ESP_RETURN_ON_ERROR(eapol_ui_line(248, result != ESP_OK ? "OBSERVATION COPY UNAVAILABLE" : !valid ? "SESSION CHANGED / REFRESHING" :
                context ? "NO BEACON CONTEXT OBSERVED" : "NO EAPOL PAIRS OBSERVED", UI_MUTED), TAG, "EAPOL empty");
        } else for (unsigned i = 0; i < s->page.copied; ++i) {
            uint16_t top = (uint16_t)(202U + 60U * i);
            uint32_t age;
            if (context) {
                const cerberus_s3_eapol_context_row_t *row = &s->contexts[i];
                const cerberus_wifi_eapol_context_t *r = &row->evidence;
                cerberus_privacy_format_wifi_identifier(r->bssid, privacy_addresses_masked(), source, sizeof(source));
                snprintf(line, sizeof(line), "BEACON AP %s", source);
                ESP_RETURN_ON_ERROR(eapol_ui_line(top, line, UI_TEXT), TAG, "EAPOL beacon AP");
                eapol_ui_ssid(r, line);
                ESP_RETURN_ON_ERROR(eapol_ui_line(top + 11U, line, UI_TEXT), TAG, "EAPOL beacon name");
                if (r->advertised_channel) snprintf(line, sizeof(line), "RX CH %u / ADVERTISED CH %u / CAP %04X", r->channel, r->advertised_channel, r->capabilities);
                else snprintf(line, sizeof(line), "RX CH %u / ADVERTISED CH ? / CAP %04X", r->channel, r->capabilities);
                ESP_RETURN_ON_ERROR(eapol_ui_line(top + 22U, line, UI_MUTED), TAG, "EAPOL beacon metadata");
                if (r->rssi_valid) snprintf(line, sizeof(line), "%d DBM / BEACON FRAMES %s%lu", r->rssi, eapol_ui_limit(r->frames), (unsigned long)r->frames);
                else snprintf(line, sizeof(line), "RSSI UNKNOWN / BEACON FRAMES %s%lu", eapol_ui_limit(r->frames), (unsigned long)r->frames);
                ESP_RETURN_ON_ERROR(eapol_ui_line(top + 33U, line, UI_MUTED), TAG, "EAPOL beacon receive");
                age = row->last_received_age_ms;
            } else {
                const cerberus_s3_eapol_row_t *row = &s->rows[i];
                const cerberus_wifi_eapol_record_t *r = &row->evidence;
                cerberus_privacy_format_wifi_identifier(r->bssid, privacy_addresses_masked(), source, sizeof(source));
                snprintf(line, sizeof(line), "AP %s", source);
                ESP_RETURN_ON_ERROR(eapol_ui_line(top, line, UI_TEXT), TAG, "EAPOL AP");
                cerberus_privacy_format_wifi_identifier(r->station, privacy_addresses_masked(), destination, sizeof(destination));
                snprintf(line, sizeof(line), "STA %s", destination);
                ESP_RETURN_ON_ERROR(eapol_ui_line(top + 11U, line, UI_TEXT), TAG, "EAPOL STA");
                char phase[12], signal[20];
                if (r->last_phase >= CERBERUS_WIFI_EAPOL_M1 && r->last_phase <= CERBERUS_WIFI_EAPOL_M4) snprintf(phase, sizeof(phase), "LAST M%u", r->last_phase);
                else snprintf(phase, sizeof(phase), "LAST KEY");
                if (r->rssi_valid) snprintf(signal, sizeof(signal), "%d DBM", r->rssi); else snprintf(signal, sizeof(signal), "RSSI UNKNOWN");
                snprintf(line, sizeof(line), "%s / C5 CH %u / %s", phase, r->channel, signal);
                ESP_RETURN_ON_ERROR(eapol_ui_line(top + 22U, line, UI_MUTED), TAG, "EAPOL pair RF");
                snprintf(line, sizeof(line), "EXCHANGES %s%lu / PMKID FRAMES %s%lu", eapol_ui_limit(r->exchanges), (unsigned long)r->exchanges, eapol_ui_limit(r->pmkid_frames), (unsigned long)r->pmkid_frames);
                ESP_RETURN_ON_ERROR(eapol_ui_line(top + 33U, line, UI_MUTED), TAG, "EAPOL pair counters");
                age = row->last_received_age_ms;
            }
            if (age == UINT32_MAX) snprintf(line, sizeof(line), "S3 %s DELIVERY AGE UNKNOWN", context ? "BEACON" : "KEY");
            else snprintf(line, sizeof(line), "S3 %s DELIVERY %lu.%03luS AGO", context ? "BEACON" : "KEY", (unsigned long)(age / 1000U), (unsigned long)(age % 1000U));
            ESP_RETURN_ON_ERROR(eapol_ui_line(top + 44U, line, UI_MUTED), TAG, "EAPOL delivery age");
        }
        if (context) {
            snprintf(line, sizeof(line), "BAD BEACONS %s%lu / LIMIT %s%lu", eapol_ui_limit(s->status.summary.beacon_malformed),
                (unsigned long)s->status.summary.beacon_malformed, eapol_ui_limit(s->status.summary.beacon_dropped), (unsigned long)s->status.summary.beacon_dropped);
            ESP_RETURN_ON_ERROR(eapol_ui_line(316, line, UI_MUTED), TAG, "EAPOL beacon exclusions");
        }
        ESP_RETURN_ON_ERROR(cerberus_ui_widget_draw_button((cerberus_ui_rect_t){12,326,72,18}, "PREV", 1, offset >= 2U, UI_WIDGET_THEME), TAG, "EAPOL prev");
        snprintf(line, sizeof(line), "%u / %u %s", offset + s->page.copied, s->page.total, context ? "APS" : "PAIRS");
        ESP_RETURN_ON_ERROR(eapol_ui_line(331, line, UI_MUTED), TAG, "EAPOL page");
        ESP_RETURN_ON_ERROR(cerberus_ui_widget_draw_button((cerberus_ui_rect_t){236,326,72,18}, "NEXT", 1, valid && offset + 2U < s->page.total, UI_WIDGET_THEME), TAG, "EAPOL next");
    }
    ESP_RETURN_ON_ERROR(cerberus_ui_widget_draw_button((cerberus_ui_rect_t){12,350,96,24}, "OPTIONS", 1, idle, UI_WIDGET_THEME), TAG, "EAPOL options");
    ESP_RETURN_ON_ERROR(cerberus_ui_widget_draw_button((cerberus_ui_rect_t){112,350,96,24}, s->view == EAPOL_VIEW_ROWS ? "SUMMARY" : "PEERS", 1, !s->exit, UI_WIDGET_THEME), TAG, "EAPOL peers");
    ESP_RETURN_ON_ERROR(cerberus_ui_widget_draw_button((cerberus_ui_rect_t){212,350,96,24}, s->view == EAPOL_VIEW_CONTEXT ? "SUMMARY" : "BEACONS", 1, !s->exit, UI_WIDGET_THEME), TAG, "EAPOL beacons");
    return cerberus_ui_widget_draw_button((cerberus_ui_rect_t){12,382,296,30}, c->busy || s->start_pending || s->stop_pending || s->barrier ? "STOP" :
        s->started ? "RESTART" : "START", 1, !s->exit && (c->busy || s->start_pending || s->stop_pending || s->barrier || eapol_ui_supported()), UI_WIDGET_THEME);
}
