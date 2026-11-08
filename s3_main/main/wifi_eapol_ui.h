#pragma once

/* The existing AP capture selector owns this page. Only fresh, verified C5
 * delivery reaches the S3 analysis service; scan-card RSSI stays historical. */
#include "wifi_eapol_session.h"
#include "esp_heap_caps.h"

enum { EAPOL_UI_STAY, EAPOL_UI_NEXT, EAPOL_UI_BACK, EAPOL_UI_HIDDEN };
typedef struct {
    cerberus_c5_wifi_result_t target;
    char ssid[33];
    cerberus_s3_eapol_status_t status;
    cerberus_s3_eapol_row_t rows[2];
    cerberus_s3_eapol_page_t page;
    uint32_t painted_ms;
    uint16_t offset;
    esp_err_t error, read_error;
    uint8_t exit;
    bool save, peers, start_pending, stop_pending, barrier, started, painted;
} eapol_ui_store_t;
static eapol_ui_store_t *eapol_ui;
static bool eapol_ui_opened;

static bool eapol_ui_is_open(void) { return eapol_ui_opened; }
static bool eapol_ui_busy(void) { return eapol_ui && eapol_ui->status.capture.busy; }
static bool eapol_ui_pending(void)
{ return eapol_ui && (eapol_ui->start_pending || eapol_ui->stop_pending || eapol_ui->barrier || eapol_ui->exit); }
static bool eapol_ui_supported(void)
{ return c5_online && c5_status.firmware_version >= CERBERUS_WIFI_EAPOL_MIN_FIRMWARE; }

static void eapol_ui_open(void)
{
    eapol_ui_opened = true;
    if (!eapol_ui) {
        eapol_ui = heap_caps_calloc(1U, sizeof(*eapol_ui), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (eapol_ui) {
            eapol_ui->target = capture_target;
            memcpy(eapol_ui->ssid, capture_ssid, sizeof(eapol_ui->ssid));
        }
    }
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
    if (!eapol_ui) {
        eapol_ui_opened = false;
        if (back) wifi_mode = UI_WIFI_MODE_TARGET;
        else ++capture_mode_index;
        return;
    }
    if (back || !eapol_ui->exit) eapol_ui->exit = back ? EAPOL_UI_BACK : EAPOL_UI_NEXT;
    eapol_ui_stop();
}

static void eapol_ui_start(void)
{
    if (!eapol_ui) eapol_ui_open();
    eapol_ui_store_t *s = eapol_ui;
    if (!s || s->status.capture.busy || eapol_ui_pending()) return;
    if (!eapol_ui_supported()) { s->error = ESP_ERR_NOT_SUPPORTED; return; }
    s->start_pending = s->barrier = true;
    s->error = ESP_OK;
    dual_wifi_live_cancel_handoff(); dual_wifi_ui_leave();
    sentinel_scan_session_select(&live_session, SENTINEL_SCAN_NONE);
    sentinel_scan_session_select(&live_ble_session, SENTINEL_SCAN_NONE);
}

static bool eapol_ui_tick(uint32_t now, bool visible)
{
    eapol_ui_store_t *s = eapol_ui;
    if (!eapol_ui_opened) return false;
    if (!s) {
        if (current_page != UI_PAGE_WIFI || wifi_mode != UI_WIFI_MODE_CAPTURE)
            eapol_ui_opened = false;
        return false;
    }
    if (!visible) {
        eapol_ui_stop();
        if (current_page != UI_PAGE_WIFI || wifi_mode != UI_WIFI_MODE_CAPTURE)
            s->exit = EAPOL_UI_HIDDEN;
    }
    if (s->barrier && eapol_ui_previous_released()) s->barrier = false;
    if (s->start_pending && !s->barrier && visible && !s->exit) {
        s->start_pending = false;
        s->error = eapol_ui_supported() ? cerberus_s3_eapol_start(
            s->target.channel, s->target.bssid, s->save) : ESP_ERR_NOT_SUPPORTED;
        if (s->error == ESP_OK) { s->started = true; s->offset = 0U; }
    }
    /* The service lock is bounded, so retry a missed Stop until the exact
     * session reports released instead of leaving a visible page stuck. */
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
            heap_caps_free(s); eapol_ui = NULL; eapol_ui_opened = false;
            capture_has_last_status = false;
            if (current_page == UI_PAGE_WIFI && wifi_mode == UI_WIFI_MODE_CAPTURE) {
                if (exit == EAPOL_UI_BACK) wifi_mode = UI_WIFI_MODE_TARGET;
                else if (exit == EAPOL_UI_NEXT) ++capture_mode_index;
            }
            return visible;
        }
        /* Busy is normal while Stop drains. A failed status or release stays
         * visible and never becomes a false claim that the radio is idle. */
        if (error != ESP_ERR_INVALID_STATE || !s->status.capture.busy) s->error = error;
    }
    if (visible && (!s->painted || (uint32_t)(now - s->painted_ms) >= 250U)) {
        s->painted = true; s->painted_ms = now; return true;
    }
    return false;
}

static void eapol_ui_tap(uint16_t x, uint16_t y)
{
    if (x < 12U || x >= 308U) return;
    eapol_ui_store_t *s = eapol_ui;
    if (s && s->exit) return;
    if (y >= 382U && y < 412U) {
        if (s && (s->status.capture.busy || eapol_ui_pending())) eapol_ui_stop();
        else eapol_ui_start();
    } else if (s && y >= 350U && y < 374U) {
        if (x < 156U && !s->status.capture.busy && !eapol_ui_pending())
            s->save = !s->save;
        else if (x >= 164U) s->peers = !s->peers;
    } else if (s && s->peers && y >= 298U && y < 324U) {
        if (x < 84U && s->offset >= 2U) s->offset -= 2U;
        else if (x >= 236U && s->offset + 2U < s->page.total) s->offset += 2U;
    }
}

static esp_err_t eapol_ui_line(uint16_t y, const char *text, uint16_t color)
{
    char bounded[49];
    snprintf(bounded, sizeof(bounded), "%.48s", text);
    return draw_centered(y, bounded, color, 1);
}

static const char *eapol_ui_limit(uint32_t value)
{ return value == UINT32_MAX ? ">=" : ""; }

static esp_err_t eapol_ui_draw(void)
{
    eapol_ui_store_t *s = eapol_ui;
    const cerberus_c5_wifi_result_t *target = s ? &s->target : &capture_target;
    char line[96], identifier[24];
    ESP_RETURN_ON_ERROR(draw_centered(54, "CAPTURE THIS AP", UI_TEXT, 2), TAG, "EAPOL title");
    ESP_RETURN_ON_ERROR(eapol_ui_line(78, s ? s->ssid : capture_ssid, UI_TEXT), TAG, "EAPOL SSID");
    cerberus_privacy_format_wifi_identifier(target->bssid,
        privacy_addresses_masked(), identifier, sizeof(identifier));
    snprintf(line, sizeof(line), "%s / CH %u", identifier, target->channel);
    ESP_RETURN_ON_ERROR(eapol_ui_line(96, line, UI_MUTED), TAG, "EAPOL target");
    ESP_RETURN_ON_ERROR(eapol_ui_line(114,
        s && (s->status.capture.save_pcap || (!s->started && s->save)) ?
        "C5 RX / S3 ANALYSIS / PCAP HAS NO TIMES" : "C5 RECEIVE / S3 ANALYSIS",
        UI_TEXT), TAG, "EAPOL roles");
    if (s) snprintf(line, sizeof(line), "RX %s%lu / DROP %s%lu + %s%lu",
        s->status.capture.received == UINT32_MAX ? ">=" : "", (unsigned long)s->status.capture.received,
        s->status.capture.dropped_full == UINT32_MAX ? ">=" : "", (unsigned long)s->status.capture.dropped_full,
        s->status.capture.dropped_other >= UINT16_MAX ? ">=" : "", (unsigned long)s->status.capture.dropped_other);
    else snprintf(line, sizeof(line), "NO MEMORY / BACK OR RETRY START");
    ESP_RETURN_ON_ERROR(eapol_ui_line(132, line, s ? UI_MUTED : UI_BAD), TAG, "EAPOL receiver");
    ESP_RETURN_ON_ERROR(cerberus_ui_widget_draw_button((cerberus_ui_rect_t){12,150,296,40},
        "EAPOL MONITOR", 1, !capture_busy(), UI_WIDGET_THEME), TAG, "EAPOL selector");
    if (!s) return cerberus_ui_widget_draw_button((cerberus_ui_rect_t){12,382,296,30},
        "RETRY START", 1, true, UI_WIDGET_THEME);
    const cerberus_s3_capture_status_t *c = &s->status.capture;
    const cerberus_wifi_eapol_summary_t *sum = &s->status.summary;
    esp_err_t error = s->read_error != ESP_OK ? s->read_error :
        c->error_code != ESP_OK ? c->error_code : s->error;
    const char *phase = s->exit ? "STOPPING / RELEASING" : s->barrier ? "WAITING FOR RADIOS" :
        c->state == CERBERUS_S3_CAPTURE_STARTING ? "STARTING" :
        c->state == CERBERUS_S3_CAPTURE_STOPPING ? "STOPPING / DRAINING" :
        c->busy ? "LISTENING / RECEIVE ONLY" : s->started ? "STOPPED / RESULTS RETAINED" : "READY / RECEIVE ONLY";
    if (error == ESP_ERR_NOT_SUPPORTED) snprintf(line, sizeof(line), "REQUIRES C5 CAPABILITY %u", CERBERUS_WIFI_EAPOL_MIN_FIRMWARE);
    else if (error != ESP_OK) snprintf(line, sizeof(line), "%s: %.28s", c->busy ? "STOPPING" : "ERROR", esp_err_to_name(error));
    else snprintf(line, sizeof(line), "%s", phase);
    ESP_RETURN_ON_ERROR(eapol_ui_line(200, line, error != ESP_OK ? UI_BAD : UI_GOOD), TAG, "EAPOL state");
    if (!s->peers) {
        snprintf(line, sizeof(line), "M1 %s%lu / M2 %s%lu", eapol_ui_limit(sum->phase[0]),
            (unsigned long)sum->phase[0], eapol_ui_limit(sum->phase[1]), (unsigned long)sum->phase[1]);
        ESP_RETURN_ON_ERROR(draw_centered(220, line, UI_TEXT, strlen(line) <= 26U ? 2U : 1U), TAG, "EAPOL M1 M2");
        snprintf(line, sizeof(line), "M3 %s%lu / M4 %s%lu", eapol_ui_limit(sum->phase[2]),
            (unsigned long)sum->phase[2], eapol_ui_limit(sum->phase[3]), (unsigned long)sum->phase[3]);
        ESP_RETURN_ON_ERROR(draw_centered(238, line, UI_TEXT, strlen(line) <= 26U ? 2U : 1U), TAG, "EAPOL M3 M4");
        snprintf(line, sizeof(line), "OBSERVED EXCHANGES %s%lu", eapol_ui_limit(sum->exchanges), (unsigned long)sum->exchanges);
        ESP_RETURN_ON_ERROR(eapol_ui_line(256, line, UI_TEXT), TAG, "EAPOL exchanges");
        snprintf(line, sizeof(line), "PMKID FRAMES %s%lu / PEERS %u", eapol_ui_limit(sum->pmkid_frames),
            (unsigned long)sum->pmkid_frames, sum->count);
        ESP_RETURN_ON_ERROR(eapol_ui_line(274, line, UI_TEXT), TAG, "EAPOL PMKID");
        snprintf(line, sizeof(line), "OTHER %s%lu / MALFORMED %s%lu", eapol_ui_limit(sum->other), (unsigned long)sum->other,
            eapol_ui_limit(sum->malformed), (unsigned long)sum->malformed);
        ESP_RETURN_ON_ERROR(eapol_ui_line(292, line, UI_MUTED), TAG, "EAPOL rejected");
        if (c->save_pcap) snprintf(line, sizeof(line), "SD WRITTEN %s%lu / %llu KB",
            c->written == UINT32_MAX ? ">=" : "", (unsigned long)c->written,
            (unsigned long long)(c->bytes_written / 1024U));
        else snprintf(line, sizeof(line), "RETRIES %s%lu / PEER LIMIT %s%lu", eapol_ui_limit(sum->retries), (unsigned long)sum->retries,
            eapol_ui_limit(sum->peer_dropped), (unsigned long)sum->peer_dropped);
        ESP_RETURN_ON_ERROR(eapol_ui_line(310, line, UI_MUTED), TAG, "EAPOL retries");
    } else {
        esp_err_t result = cerberus_s3_eapol_page(s->offset, s->rows, 2U, &s->page);
        bool valid = result == ESP_OK && s->page.generation == s->status.generation;
        if (!valid || !s->page.copied) {
            ESP_RETURN_ON_ERROR(eapol_ui_line(244, result != ESP_OK ? "PEER COPY UNAVAILABLE" :
                !valid ? "SESSION CHANGED / REFRESHING" : "NO EAPOL PEERS OBSERVED", UI_MUTED), TAG, "EAPOL empty");
        } else for (unsigned i = 0; i < s->page.copied; ++i) {
            const cerberus_s3_eapol_row_t *row = &s->rows[i];
            const cerberus_wifi_eapol_record_t *r = &row->evidence;
            uint16_t top = (uint16_t)(212U + i * 40U);
            cerberus_privacy_format_wifi_identifier(r->station, privacy_addresses_masked(), identifier, sizeof(identifier));
            snprintf(line, sizeof(line), "%s / EXCH %s%lu", identifier,
                r->exchanges == UINT32_MAX ? ">=" : "", (unsigned long)r->exchanges);
            ESP_RETURN_ON_ERROR(eapol_ui_line(top, line, UI_TEXT), TAG, "EAPOL peer");
            char phase_name[12];
            if (r->last_phase >= CERBERUS_WIFI_EAPOL_M1 && r->last_phase <= CERBERUS_WIFI_EAPOL_M4)
                snprintf(phase_name, sizeof(phase_name), "LAST M%u", r->last_phase);
            else snprintf(phase_name, sizeof(phase_name), "LAST KEY");
            if (r->rssi_valid) snprintf(line, sizeof(line), "%s / C5 CH %u / %d DBM", phase_name, r->channel, r->rssi);
            else snprintf(line, sizeof(line), "%s / C5 CH %u / RSSI UNKNOWN", phase_name, r->channel);
            ESP_RETURN_ON_ERROR(eapol_ui_line(top + 12U, line, UI_MUTED), TAG, "EAPOL peer RF");
            if (row->last_received_age_ms == UINT32_MAX) snprintf(line, sizeof(line), "DELIVERY AGE UNKNOWN / PMKID %s%lu",
                r->pmkid_frames == UINT32_MAX ? ">=" : "", (unsigned long)r->pmkid_frames);
            else snprintf(line, sizeof(line), "DELIVERED %lu.%03luS AGO / PMKID %s%lu",
                (unsigned long)(row->last_received_age_ms / 1000U),
                (unsigned long)(row->last_received_age_ms % 1000U),
                r->pmkid_frames == UINT32_MAX ? ">=" : "", (unsigned long)r->pmkid_frames);
            ESP_RETURN_ON_ERROR(eapol_ui_line(top + 24U, line, UI_MUTED), TAG, "EAPOL peer delivery");
        }
        ESP_RETURN_ON_ERROR(cerberus_ui_widget_draw_button((cerberus_ui_rect_t){12,298,72,26},
            "PREV", 1, s->offset >= 2U, UI_WIDGET_THEME), TAG, "EAPOL prev");
        snprintf(line, sizeof(line), "%u / %u PEERS", s->offset + s->page.copied, s->page.total);
        ESP_RETURN_ON_ERROR(eapol_ui_line(307, line, UI_MUTED), TAG, "EAPOL page");
        ESP_RETURN_ON_ERROR(cerberus_ui_widget_draw_button((cerberus_ui_rect_t){236,298,72,26},
            "NEXT", 1, valid && s->offset + 2U < s->page.total, UI_WIDGET_THEME), TAG, "EAPOL next");
    }
    ESP_RETURN_ON_ERROR(eapol_ui_line(326, "OBSERVED ONLY / AUTHENTICATION NOT VERIFIED", UI_MUTED), TAG, "EAPOL scope");
    if (c->file_name[0]) snprintf(line, sizeof(line), "FILE %.31s", c->file_name);
    else if (s->status.rejected_provenance) snprintf(line, sizeof(line), "PROVENANCE REJECTED %lu", (unsigned long)s->status.rejected_provenance);
    else snprintf(line, sizeof(line), "PMKID PRESENCE / VALUES NOT DISPLAYED");
    ESP_RETURN_ON_ERROR(eapol_ui_line(338, line, UI_MUTED), TAG, "EAPOL file");
    ESP_RETURN_ON_ERROR(cerberus_ui_widget_draw_button((cerberus_ui_rect_t){12,350,144,24},
        s->save ? "SAVE PCAP ON" : "SAVE PCAP OFF", 1, !c->busy && !eapol_ui_pending(), UI_WIDGET_THEME), TAG, "EAPOL save");
    ESP_RETURN_ON_ERROR(cerberus_ui_widget_draw_button((cerberus_ui_rect_t){164,350,144,24},
        s->peers ? "SUMMARY" : "PEERS", 1, !s->exit, UI_WIDGET_THEME), TAG, "EAPOL peers");
    return cerberus_ui_widget_draw_button((cerberus_ui_rect_t){12,382,296,30},
        c->busy || eapol_ui_pending() ? "STOP" : s->started ? "RESTART" : "START",
        1, !s->exit && (c->busy || eapol_ui_pending() || eapol_ui_supported()), UI_WIDGET_THEME);
}
