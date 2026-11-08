#pragma once

/* One passive monitor reached from the Wi-Fi menu or the AP capture selector.
 * The shared capture worker owns reception; this page owns copied scope and
 * presentation only. A Stop retains evidence until explicit release. */
#include "wifi_sae_session.h"
#include "esp_heap_caps.h"

enum { SAE_SCOPE_ALL, SAE_SCOPE_2G, SAE_SCOPE_5G, SAE_SCOPE_FIXED, SAE_SCOPE_SELECTED, SAE_SCOPE_AP };
enum { SAE_VIEW_OPTIONS, SAE_VIEW_SUMMARY, SAE_VIEW_ROWS };
enum { SAE_EXIT_STAY, SAE_EXIT_NEXT, SAE_EXIT_BACK, SAE_EXIT_HIDDEN };
typedef struct {
    cerberus_c5_wifi_result_t target;
    char ssid[33];
    cerberus_wifi_capture_plan_t plan;
    cerberus_s3_sae_options_t request;
    cerberus_s3_sae_status_t status;
    cerberus_s3_sae_row_t rows[2];
    cerberus_s3_sae_page_t page;
    uint32_t painted_ms;
    uint16_t offset;
    esp_err_t error, read_error;
    uint8_t scope, run_scope, channel, run_channel, view, exit, target_index;
    bool save, started, start_pending, stop_pending, barrier, painted, target_dual_return;
} sae_ui_store_t;
static sae_ui_store_t *sae_ui;
static bool sae_ui_opened, sae_ui_from_menu;

static bool sae_ui_is_open(void) { return sae_ui_opened; }
static bool sae_ui_busy(void) { return sae_ui && sae_ui->status.capture.busy; }
static bool sae_ui_pending(void)
{ return sae_ui && (sae_ui->start_pending || sae_ui->stop_pending || sae_ui->barrier || sae_ui->exit); }
static bool sae_ui_supported(void)
{ return c5_online && c5_status.firmware_version >= CERBERUS_WIFI_CAPTURE_PLAN_MIN_FIRMWARE; }
static const char *sae_ui_back_label(void)
{ return sae_ui_pending() && sae_ui && sae_ui->exit ? "STOPPING" : sae_ui_from_menu ? "RX TOOLS" : "TARGET"; }

static void sae_ui_allocate(void)
{
    if (sae_ui) return;
    sae_ui = heap_caps_calloc(1U, sizeof(*sae_ui), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!sae_ui) return;
    sae_ui->view = SAE_VIEW_OPTIONS;
    sae_ui->scope = sae_ui_from_menu ? SAE_SCOPE_ALL : SAE_SCOPE_AP;
    sae_ui->channel = sae_ui_from_menu ? 1U : capture_target.channel;
    if (!sae_ui_from_menu) {
        sae_ui->target = capture_target;
        sae_ui->target_index = wifi_selected_index;
        sae_ui->target_dual_return = dual_wifi_return_to_view;
        memcpy(sae_ui->ssid, capture_ssid, sizeof(sae_ui->ssid));
    }
}

static void sae_ui_open(bool menu)
{
    if (sae_ui_opened || eapol_ui_is_open() || frame_ui_is_open()) return;
    sae_ui_opened = true; sae_ui_from_menu = menu;
    sae_ui_allocate();
    capture_mode_index = 4U;
    wifi_mode = UI_WIFI_MODE_CAPTURE;
}

static void sae_ui_stop(void)
{
    if (!sae_ui) return;
    sae_ui->start_pending = false;
    sae_ui->stop_pending = true;
    cerberus_s3_sae_stop();
}

static void sae_ui_close(bool back)
{
    /* The targetless shortcut has no unrelated generic capture scope. */
    if (!back && sae_ui_from_menu) return;
    if (!sae_ui) {
        sae_ui_opened = false;
        if (back) wifi_mode = sae_ui_from_menu ? UI_WIFI_MODE_RX_TOOLS : UI_WIFI_MODE_TARGET;
        else capture_mode_index = 0U;
        return;
    }
    if (back || !sae_ui->exit) sae_ui->exit = back ? SAE_EXIT_BACK : SAE_EXIT_NEXT;
    sae_ui_stop();
}

static bool sae_ui_freeze(sae_ui_store_t *s)
{
    cerberus_wifi_capture_plan_clear(&s->plan);
    s->request = (cerberus_s3_sae_options_t){.save_pcap = s->save};
    if (s->scope == SAE_SCOPE_FIXED || s->scope == SAE_SCOPE_AP) {
        s->request.channel = s->scope == SAE_SCOPE_AP ? s->target.channel : s->channel;
        if (!cerberus_wifi_capture_plan_channel_bit(s->request.channel)) return false;
        s->request.filter_bssid = s->scope == SAE_SCOPE_AP;
        if (s->request.filter_bssid) memcpy(s->request.bssid, s->target.bssid, 6U);
    } else {
        if (s->scope == SAE_SCOPE_SELECTED) {
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

static void sae_ui_start(void)
{
    if (!sae_ui) sae_ui_allocate();
    sae_ui_store_t *s = sae_ui;
    if (!s || s->status.capture.busy || sae_ui_pending() || sae_ui_selecting) return;
    if (!sae_ui_supported()) { s->error = ESP_ERR_NOT_SUPPORTED; return; }
    if (!sae_ui_freeze(s)) { s->error = ESP_ERR_INVALID_ARG; return; }
    s->start_pending = s->barrier = true; s->error = ESP_OK;
    dual_wifi_live_cancel_handoff(); dual_wifi_ui_leave();
    sentinel_scan_session_select(&live_session, SENTINEL_SCAN_NONE);
    sentinel_scan_session_select(&live_ble_session, SENTINEL_SCAN_NONE);
}

static void sae_ui_choose_aps(void)
{
    sae_ui_store_t *s = sae_ui;
    if (!s || sae_ui_busy() || sae_ui_pending()) return;
    sae_ui_selecting = true; wifi_attack_selecting = true;
    dual_wifi_return_to_view = false;
    dual_wifi_live_cancel_handoff(); dual_wifi_ui_leave();
    wifi_mode = UI_WIFI_MODE_LIST;
    capture_picker_begin();
}

static void sae_ui_selection_done(void)
{
    if (!sae_ui_selecting) return;
    sae_ui_selecting = false; wifi_attack_selecting = false;
    sentinel_scan_session_select(&live_session, SENTINEL_SCAN_NONE);
    if (sae_ui && !sae_ui->exit) {
        sae_ui->scope = SAE_SCOPE_SELECTED; sae_ui->view = SAE_VIEW_OPTIONS;
        sae_ui->error = ESP_OK;
        wifi_mode = UI_WIFI_MODE_CAPTURE;
    }
}

static bool sae_ui_tick(uint32_t now, bool visible)
{
    sae_ui_store_t *s = sae_ui;
    if (!sae_ui_opened) return false;
    bool picking = current_page == UI_PAGE_WIFI && wifi_mode == UI_WIFI_MODE_LIST && sae_ui_selecting;
    bool here = current_page == UI_PAGE_WIFI && wifi_mode == UI_WIFI_MODE_CAPTURE;
    if (!s) {
        if (!here) sae_ui_opened = false;
        return false;
    }
    if (!visible) {
        sae_ui_stop();
        if (!here && !picking) {
            s->exit = SAE_EXIT_HIDDEN;
            if (sae_ui_selecting) { sae_ui_selecting = false; wifi_attack_selecting = false; }
        }
    }
    if (s->barrier && eapol_ui_previous_released()) s->barrier = false;
    if (s->start_pending && !s->barrier && visible && !s->exit) {
        s->start_pending = false;
        s->error = sae_ui_supported() ? cerberus_s3_sae_start(&s->request) : ESP_ERR_NOT_SUPPORTED;
        if (s->error == ESP_OK) {
            s->started = true; s->offset = 0U; s->view = SAE_VIEW_SUMMARY;
            s->run_scope = s->scope; s->run_channel = s->request.channel;
        }
    }
    /* Stop takes a bounded service lock. A missed request is retried on the
     * next UI tick; only a successful status/release proves cleanup ended. */
    if (s->stop_pending || s->exit) cerberus_s3_sae_stop();
    cerberus_s3_sae_status_t status;
    s->read_error = cerberus_s3_sae_status(&status);
    if (s->read_error == ESP_OK) {
        s->status = status;
        if (!status.capture.busy) s->stop_pending = false;
    }
    if (s->exit && !s->barrier) {
        esp_err_t error = cerberus_s3_sae_release();
        if (error == ESP_OK) {
            uint8_t exit = s->exit;
            if (here && (exit == SAE_EXIT_BACK || exit == SAE_EXIT_NEXT) && !sae_ui_from_menu) {
                wifi_selected_result = s->target; wifi_selected_index = s->target_index;
                wifi_selection_valid = true;
                dual_wifi_return_to_view = s->target_dual_return;
                capture_target = s->target; memcpy(capture_ssid, s->ssid, sizeof(capture_ssid));
            }
            heap_caps_free(s); sae_ui = NULL; sae_ui_opened = false;
            if (here) {
                if (exit == SAE_EXIT_BACK) wifi_mode = sae_ui_from_menu ? UI_WIFI_MODE_RX_TOOLS : UI_WIFI_MODE_TARGET;
                else if (exit == SAE_EXIT_NEXT) capture_mode_index = 0U;
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

static void sae_ui_channel_step(sae_ui_store_t *s, bool next)
{
    unsigned index = 0U;
    for (; index < CERBERUS_WIFI_CAPTURE_PLAN_CHANNELS; ++index)
        if (cerberus_wifi_capture_plan_channel(index) == s->channel) break;
    index = next ? (index + 1U) % CERBERUS_WIFI_CAPTURE_PLAN_CHANNELS :
        (index + CERBERUS_WIFI_CAPTURE_PLAN_CHANNELS - 1U) % CERBERUS_WIFI_CAPTURE_PLAN_CHANNELS;
    s->channel = cerberus_wifi_capture_plan_channel(index);
}

static void sae_ui_tap(uint16_t x, uint16_t y)
{
    if (x < 12U || x >= 308U) return;
    sae_ui_store_t *s = sae_ui;
    if (s && s->exit) return;
    if (y >= 382U && y < 412U) {
        if (sae_ui_busy() || sae_ui_pending()) sae_ui_stop();
        else sae_ui_start();
        return;
    }
    if (!s) return;
    bool idle = !sae_ui_busy() && !sae_ui_pending();
    if (y >= 350U && y < 374U) {
        if (x < 156U && idle) s->view = SAE_VIEW_OPTIONS;
        else if (x >= 164U) s->view = s->view == SAE_VIEW_ROWS ? SAE_VIEW_SUMMARY : SAE_VIEW_ROWS;
    } else if (s->view == SAE_VIEW_OPTIONS && idle) {
        if (y >= 216U && y < 244U) {
            s->scope = (uint8_t)((s->scope + 1U) % (sae_ui_from_menu ? 5U : 6U));
            s->error = ESP_OK;
        } else if (y >= 252U && y < 280U) {
            if (s->scope == SAE_SCOPE_SELECTED) sae_ui_choose_aps();
            else if (s->scope == SAE_SCOPE_FIXED && (x < 108U || x >= 212U)) sae_ui_channel_step(s, x >= 212U);
        } else if (y >= 292U && y < 320U) s->save = !s->save;
    } else if (s->view == SAE_VIEW_ROWS && y >= 326U && y < 344U) {
        if (x < 84U && s->offset >= 2U) s->offset -= 2U;
        else if (x >= 236U && s->offset + 2U < s->page.total) s->offset += 2U;
    }
}

static esp_err_t sae_ui_line(uint16_t y, const char *text, uint16_t color)
{
    char bounded[49]; snprintf(bounded, sizeof(bounded), "%.48s", text);
    return draw_centered(y, bounded, color, 1);
}
static const char *sae_ui_limit(uint32_t value) { return value == UINT32_MAX ? ">=" : ""; }
static unsigned sae_ui_bits(uint64_t value)
{ unsigned count = 0; while (value) { value &= value - 1U; ++count; } return count; }
static const char *sae_ui_scope_name(unsigned scope)
{
    static const char *const names[] = {"ALL / 2.4 + 5 GHZ", "2.4 GHZ", "5 GHZ", "FIXED CHANNEL", "SELECTED APS", "THIS AP"};
    return scope < sizeof(names) / sizeof(names[0]) ? names[scope] : "UNKNOWN SCOPE";
}

static esp_err_t sae_ui_draw(void)
{
    sae_ui_store_t *s = sae_ui;
    char line[100], source[24], destination[24];
    ESP_RETURN_ON_ERROR(draw_centered(54, "SAE MONITOR", UI_TEXT, 2), TAG, "SAE title");
    unsigned scope = s ? (s->view == SAE_VIEW_OPTIONS || !s->started ? s->scope : s->run_scope) :
        sae_ui_from_menu ? SAE_SCOPE_ALL : SAE_SCOPE_AP;
    snprintf(line, sizeof(line), "%s%s", s && s->view == SAE_VIEW_OPTIONS && s->started ? "NEXT: " : "SCOPE: ", sae_ui_scope_name(scope));
    ESP_RETURN_ON_ERROR(sae_ui_line(78, line, UI_TEXT), TAG, "SAE scope");
    if (scope == SAE_SCOPE_AP) snprintf(line, sizeof(line), "%.32s / CH %u", s ? s->ssid : capture_ssid,
        s ? s->target.channel : capture_target.channel);
    else if (scope == SAE_SCOPE_SELECTED) snprintf(line, sizeof(line), "%u AP TARGETS / CHANNEL UNION",
        s && s->started && s->view != SAE_VIEW_OPTIONS ? s->status.plan.target_count : wifi_target_count);
    else if (scope == SAE_SCOPE_FIXED) snprintf(line, sizeof(line), "ALL BSSIDS / CHANNEL %u",
        s ? (s->view == SAE_VIEW_OPTIONS ? s->channel : s->run_channel) : 1U);
    else snprintf(line, sizeof(line), "ALL BSSIDS / 250 MS SUCCESSFUL DWELL");
    ESP_RETURN_ON_ERROR(sae_ui_line(96, line, UI_MUTED), TAG, "SAE target");
    ESP_RETURN_ON_ERROR(sae_ui_line(114, s && s->status.capture.save_pcap ?
        "C5 RX / S3 ANALYSIS / PCAP HAS NO TIMES" : "C5 RECEIVE / S3 ANALYSIS", UI_TEXT), TAG, "SAE roles");
    const cerberus_s3_capture_status_t *c = s ? &s->status.capture : NULL;
    esp_err_t error = !s ? ESP_ERR_NO_MEM : s->read_error != ESP_OK ? s->read_error :
        c->error_code != ESP_OK ? c->error_code : s->error;
    if (error == ESP_ERR_NOT_SUPPORTED) snprintf(line, sizeof(line), "REQUIRES C5 CAPABILITY %u", CERBERUS_WIFI_CAPTURE_PLAN_MIN_FIRMWARE);
    else if (error == ESP_ERR_INVALID_ARG) snprintf(line, sizeof(line), "SELECT VALID APS / CHANNELS BEFORE START");
    else if (error != ESP_OK) snprintf(line, sizeof(line), "%s: %.32s", c && c->busy ? "STOPPING" : "ERROR", esp_err_to_name(error));
    else snprintf(line, sizeof(line), "%s", s->exit ? "STOPPING / RELEASING" : s->barrier ? "WAITING FOR RADIOS" :
        c->state == CERBERUS_S3_CAPTURE_STARTING ? "STARTING" :
        c->state == CERBERUS_S3_CAPTURE_STOPPING ? "STOPPING / DRAINING" : c->busy ? "LISTENING / RECEIVE ONLY" :
        s->started ? "STOPPED / RESULTS RETAINED" : "READY / RECEIVE ONLY");
    ESP_RETURN_ON_ERROR(sae_ui_line(132, line, error == ESP_OK ? UI_GOOD : UI_BAD), TAG, "SAE state");
    ESP_RETURN_ON_ERROR(cerberus_ui_widget_draw_button((cerberus_ui_rect_t){12,150,296,40},
        sae_ui_from_menu ? "PASSIVE AUTHENTICATION MONITOR" : "SAE MONITOR / CHANGE FILTER", 1,
        !sae_ui_from_menu && !capture_busy(), UI_WIDGET_THEME), TAG, "SAE selector");
    if (!s) return cerberus_ui_widget_draw_button((cerberus_ui_rect_t){12,382,296,30}, "RETRY START", 1, true, UI_WIDGET_THEME);
    bool idle = !c->busy && !sae_ui_pending();
    if (s->view == SAE_VIEW_OPTIONS) {
        ESP_RETURN_ON_ERROR(sae_ui_line(202, "SCOPE AND SAVE APPLY TO THE NEXT START", UI_MUTED), TAG, "SAE next");
        snprintf(line, sizeof(line), "SCOPE: %s >", sae_ui_scope_name(s->scope));
        ESP_RETURN_ON_ERROR(cerberus_ui_widget_draw_button((cerberus_ui_rect_t){12,216,296,28}, line, 1, idle, UI_WIDGET_THEME), TAG, "SAE scope control");
        if (s->scope == SAE_SCOPE_FIXED) {
            ESP_RETURN_ON_ERROR(cerberus_ui_widget_draw_button((cerberus_ui_rect_t){12,252,96,28}, "CH -", 1, idle, UI_WIDGET_THEME), TAG, "SAE channel previous");
            snprintf(line, sizeof(line), "CH %u", s->channel);
            ESP_RETURN_ON_ERROR(sae_ui_line(261, line, UI_TEXT), TAG, "SAE fixed channel");
            ESP_RETURN_ON_ERROR(cerberus_ui_widget_draw_button((cerberus_ui_rect_t){212,252,96,28}, "CH +", 1, idle, UI_WIDGET_THEME), TAG, "SAE channel next");
        } else if (s->scope == SAE_SCOPE_SELECTED) {
            snprintf(line, sizeof(line), "CHOOSE APS / %u SELECTED", wifi_target_count);
            ESP_RETURN_ON_ERROR(cerberus_ui_widget_draw_button((cerberus_ui_rect_t){12,252,296,28}, line, 1, idle, UI_WIDGET_THEME), TAG, "SAE choose targets");
        } else ESP_RETURN_ON_ERROR(sae_ui_line(261, s->scope == SAE_SCOPE_AP ? "FIXED TO THE COPIED AP BSSID" :
            "COUNTRY / DRIVER DECIDE AVAILABLE CHANNELS", UI_MUTED), TAG, "SAE channel scope");
        ESP_RETURN_ON_ERROR(cerberus_ui_widget_draw_button((cerberus_ui_rect_t){12,292,296,28},
            s->save ? "SAVE PCAP ON" : "SAVE PCAP OFF", 1, idle, UI_WIDGET_THEME), TAG, "SAE save");
        ESP_RETURN_ON_ERROR(sae_ui_line(331, s->save ? "PCAPNG / NO UTC OR FRAME TIMES" : "NO SD CARD REQUIRED", UI_MUTED), TAG, "SAE storage");
    } else if (s->view == SAE_VIEW_SUMMARY) {
        const cerberus_wifi_sae_summary_t *sum = &s->status.summary;
        snprintf(line, sizeof(line), "OBSERVED %s%lu / PAIRS %u", sae_ui_limit(sum->observations), (unsigned long)sum->observations, sum->count);
        ESP_RETURN_ON_ERROR(sae_ui_line(202, line, UI_TEXT), TAG, "SAE observations");
        snprintf(line, sizeof(line), "STATUS 0 %s%lu / TOKEN REQ %s%lu", sae_ui_limit(sum->commits), (unsigned long)sum->commits,
            sae_ui_limit(sum->token_requests), (unsigned long)sum->token_requests);
        ESP_RETURN_ON_ERROR(sae_ui_line(216, line, UI_TEXT), TAG, "SAE status counts");
        snprintf(line, sizeof(line), "H2E %s%lu / SAE-PK %s%lu", sae_ui_limit(sum->h2e), (unsigned long)sum->h2e,
            sae_ui_limit(sum->sae_pk), (unsigned long)sum->sae_pk);
        ESP_RETURN_ON_ERROR(sae_ui_line(230, line, UI_MUTED), TAG, "SAE variants");
        snprintf(line, sizeof(line), "GROUP REJECT %s%lu / OTHER %s%lu", sae_ui_limit(sum->group_rejections), (unsigned long)sum->group_rejections,
            sae_ui_limit(sum->other_status), (unsigned long)sum->other_status);
        ESP_RETURN_ON_ERROR(sae_ui_line(244, line, UI_MUTED), TAG, "SAE other statuses");
        snprintf(line, sizeof(line), "RX %s%lu / DROP %s%lu + %s%lu", sae_ui_limit(c->received), (unsigned long)c->received,
            sae_ui_limit(c->dropped_full), (unsigned long)c->dropped_full, c->dropped_other >= UINT16_MAX ? ">=" : "", (unsigned long)c->dropped_other);
        ESP_RETURN_ON_ERROR(sae_ui_line(258, line, UI_MUTED), TAG, "SAE capture counts");
        if (c->planned && (!c->hop_valid || c->hop_error != ESP_OK)) snprintf(line, sizeof(line), "CHANNEL TELEMETRY UNAVAILABLE%s", c->hop_error ? " / RETRYING" : "");
        else if (c->planned && !c->busy) snprintf(line, sizeof(line), "STOPPED / SWEEPS %s%u / COVERAGE RETAINED",
            c->completed_sweeps == UINT16_MAX ? ">=" : "", c->completed_sweeps);
        else if (c->planned && c->channel) snprintf(line, sizeof(line), "RX CHANNEL %u / SWEEPS %s%u", c->channel,
            c->completed_sweeps == UINT16_MAX ? ">=" : "", c->completed_sweeps);
        else if (c->planned) snprintf(line, sizeof(line), "TUNING / NO CURRENT RX CHANNEL");
        else if (s->started) snprintf(line, sizeof(line), c->busy ? "FIXED RX CHANNEL %u" : "FIXED SCOPE CHANNEL %u", s->status.channel);
        else snprintf(line, sizeof(line), "NO RECEIVE SESSION STARTED");
        ESP_RETURN_ON_ERROR(sae_ui_line(272, line, c->planned && !c->hop_valid ? UI_BAD : UI_TEXT), TAG, "SAE channels");
        if (c->planned && c->hop_valid && c->hop_error == ESP_OK) snprintf(line, sizeof(line), "THIS SWEEP: TUNED %u / TRIED %u / SKIPPED %u",
            sae_ui_bits(c->applied_mask), sae_ui_bits(c->attempted_mask), sae_ui_bits(c->attempted_mask & ~c->applied_mask));
        else snprintf(line, sizeof(line), "RETRY %s%lu / SHORT %s%lu", sae_ui_limit(sum->retry_marked), (unsigned long)sum->retry_marked,
            sae_ui_limit(sum->short_bodies), (unsigned long)sum->short_bodies);
        ESP_RETURN_ON_ERROR(sae_ui_line(286, line, UI_MUTED), TAG, "SAE coverage");
        if (c->save_pcap) snprintf(line, sizeof(line), "SD WRITTEN %s%lu / %llu KB", sae_ui_limit(c->written), (unsigned long)c->written,
            (unsigned long long)(c->bytes_written / 1024U));
        else snprintf(line, sizeof(line), "MALFORMED %s%lu / PAIR LIMIT %s%lu", sae_ui_limit(sum->malformed), (unsigned long)sum->malformed,
            sae_ui_limit(sum->peer_dropped), (unsigned long)sum->peer_dropped);
        ESP_RETURN_ON_ERROR(sae_ui_line(300, line, UI_MUTED), TAG, "SAE storage counts");
        ESP_RETURN_ON_ERROR(sae_ui_line(316, "OBSERVED HEADERS / NOT VERIFIED AUTH", UI_MUTED), TAG, "SAE interpretation");
        if (c->file_name[0]) snprintf(line, sizeof(line), "FILE %.31s", c->file_name);
        else snprintf(line, sizeof(line), "NO TOKEN OR CRYPTOGRAPHIC VALUES DISPLAYED");
        ESP_RETURN_ON_ERROR(sae_ui_line(332, line, UI_MUTED), TAG, "SAE file");
    } else {
        esp_err_t result = cerberus_s3_sae_page(s->offset, s->rows, 2U, &s->page);
        bool valid = result == ESP_OK && s->page.generation == s->status.generation;
        if (!valid || !s->page.copied) {
            ESP_RETURN_ON_ERROR(sae_ui_line(248, result != ESP_OK ? "OBSERVATION COPY UNAVAILABLE" :
                !valid ? "SESSION CHANGED / REFRESHING" : "NO SAE PAIRS OBSERVED", UI_MUTED), TAG, "SAE empty");
        } else for (unsigned i = 0; i < s->page.copied; ++i) {
            const cerberus_s3_sae_row_t *row = &s->rows[i];
            const cerberus_wifi_sae_record_t *r = &row->evidence;
            uint16_t top = (uint16_t)(202U + 60U * i);
            cerberus_privacy_format_wifi_identifier(r->transmitter, privacy_addresses_masked(), source, sizeof(source));
            cerberus_privacy_format_wifi_identifier(r->receiver, privacy_addresses_masked(), destination, sizeof(destination));
            snprintf(line, sizeof(line), "%s > %s", source, destination);
            ESP_RETURN_ON_ERROR(sae_ui_line(top, line, UI_TEXT), TAG, "SAE pair");
            cerberus_privacy_format_wifi_identifier(r->bssid, privacy_addresses_masked(), source, sizeof(source));
            snprintf(line, sizeof(line), "AP %s / STATUS %u", source, r->status);
            ESP_RETURN_ON_ERROR(sae_ui_line(top + 11U, line, UI_MUTED), TAG, "SAE BSSID");
            if (r->group_present) snprintf(line, sizeof(line), "GROUP %u / FRAME %u / BODY %u B", r->group, r->frame_length, r->body_length);
            else snprintf(line, sizeof(line), "GROUP UNKNOWN / FRAME %u / BODY %u B", r->frame_length, r->body_length);
            ESP_RETURN_ON_ERROR(sae_ui_line(top + 22U, line, UI_MUTED), TAG, "SAE fields");
            if (r->rssi_valid) snprintf(line, sizeof(line), "C5 CH %u / %d DBM / OBS %s%lu", r->channel, r->rssi,
                sae_ui_limit(r->observations), (unsigned long)r->observations);
            else snprintf(line, sizeof(line), "C5 CH %u / RSSI UNKNOWN / OBS %s%lu", r->channel, sae_ui_limit(r->observations), (unsigned long)r->observations);
            ESP_RETURN_ON_ERROR(sae_ui_line(top + 33U, line, UI_MUTED), TAG, "SAE RF");
            if (row->last_received_age_ms == UINT32_MAX) snprintf(line, sizeof(line), "S3 DELIVERY AGE UNKNOWN");
            else snprintf(line, sizeof(line), "S3 DELIVERY %lu.%03luS AGO / %s BODY",
                (unsigned long)(row->last_received_age_ms / 1000U), (unsigned long)(row->last_received_age_ms % 1000U),
                r->flags & CERBERUS_WIFI_SAE_SHORT_BODY ? "SHORT" : "UNVERIFIED");
            ESP_RETURN_ON_ERROR(sae_ui_line(top + 44U, line, UI_MUTED), TAG, "SAE age");
        }
        ESP_RETURN_ON_ERROR(cerberus_ui_widget_draw_button((cerberus_ui_rect_t){12,326,72,18}, "PREV", 1, s->offset >= 2U, UI_WIDGET_THEME), TAG, "SAE prev");
        snprintf(line, sizeof(line), "%u / %u PAIRS", s->offset + s->page.copied, s->page.total);
        ESP_RETURN_ON_ERROR(sae_ui_line(331, line, UI_MUTED), TAG, "SAE page");
        ESP_RETURN_ON_ERROR(cerberus_ui_widget_draw_button((cerberus_ui_rect_t){236,326,72,18}, "NEXT", 1, valid && s->offset + 2U < s->page.total, UI_WIDGET_THEME), TAG, "SAE next");
    }
    ESP_RETURN_ON_ERROR(cerberus_ui_widget_draw_button((cerberus_ui_rect_t){12,350,144,24}, "OPTIONS", 1, idle, UI_WIDGET_THEME), TAG, "SAE options");
    ESP_RETURN_ON_ERROR(cerberus_ui_widget_draw_button((cerberus_ui_rect_t){164,350,144,24}, s->view == SAE_VIEW_ROWS ? "SUMMARY" : "ROWS", 1, !s->exit, UI_WIDGET_THEME), TAG, "SAE rows");
    return cerberus_ui_widget_draw_button((cerberus_ui_rect_t){12,382,296,30}, c->busy || s->start_pending || s->barrier ? "STOP" :
        s->started ? "RESTART" : "START", 1, !s->exit && (c->busy || s->start_pending || s->barrier || sae_ui_supported()), UI_WIDGET_THEME);
}
