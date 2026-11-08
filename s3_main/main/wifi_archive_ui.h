#pragma once

/* Historical presentation only: no clock, radio, storage or live target lookup.
 * The document is immutable between typed worker publications. Small indexes
 * group received C-string SSIDs; receiver observations are never merged. */
#include "wifi_archive.h"

typedef enum { WIFI_ARCHIVE_LIST, WIFI_ARCHIVE_DETAIL, WIFI_ARCHIVE_JOB,
    WIFI_ARCHIVE_ERROR } wifi_archive_ui_page_t;
typedef enum { WIFI_ARCHIVE_NONE, WIFI_ARCHIVE_REDRAW, WIFI_ARCHIVE_BACK,
    WIFI_ARCHIVE_SAVE, WIFI_ARCHIVE_NEW_SCAN, WIFI_ARCHIVE_EXPORT } wifi_archive_ui_action_t;
typedef struct {
    cerberus_wifi_archive_t document;
    cerberus_wifi_archive_row_t selected;
    uint64_t job_id;
    uint8_t indexes[64], painted[4], count, page, painted_count;
    uint8_t source, band, sort;
    char group_ssid[33];
    bool groups, group_focus, has_document, selected_valid, paint_valid;
    bool job_pending, release_pending, saving, write_started;
    wifi_archive_ui_page_t view;
    esp_err_t error;
    char file_name[256], failure_name[256];
} wifi_archive_ui_store_t;

static void wifi_archive_ui_invalidate(wifi_archive_ui_store_t *s)
{ s->paint_valid = s->selected_valid = false; s->painted_count = 0U; }

static void wifi_archive_ui_error(wifi_archive_ui_store_t *s, esp_err_t error)
{
    wifi_archive_ui_invalidate(s); s->job_pending = s->release_pending = false;
    s->error = error; s->view = WIFI_ARCHIVE_ERROR;
}

static void wifi_archive_ui_name(char *out, size_t size, const char *name)
{
    size_t i = 0U;
    for (; i + 1U < size && name && name[i]; ++i)
        out[i] = (uint8_t)name[i] >= 32U && (uint8_t)name[i] <= 126U ? name[i] : '.';
    if (size) out[i] = 0;
}

static const char *wifi_archive_ui_source(uint8_t source)
{ return source == CERBERUS_WIFI_ARCHIVE_SOURCE_S3 ? "S3" : "C5"; }

static const char *wifi_archive_ui_coverage(uint8_t band)
{
    return band == CERBERUS_WIFI_ARCHIVE_COVERAGE_2G ? "2.4G" :
        band == CERBERUS_WIFI_ARCHIVE_COVERAGE_5G ? "5G" :
        band == CERBERUS_WIFI_ARCHIVE_COVERAGE_ALL ? "ALL" : "NONE";
}

static bool wifi_archive_ui_matches(const wifi_archive_ui_store_t *s,
    const cerberus_wifi_archive_row_t *r)
{
    return (!s->source || s->source == r->source) &&
        (!s->band || (s->band == 1U ? r->channel <= 14U : r->channel > 14U)) &&
        (!s->group_focus || !strcmp(s->group_ssid, r->ssid));
}

static bool wifi_archive_ui_same_group(const cerberus_wifi_archive_row_t *a,
    const cerberus_wifi_archive_row_t *b)
{ return a->ssid[0] && !strcmp(a->ssid, b->ssid); }

static int wifi_archive_ui_rank(const wifi_archive_ui_store_t *s, uint8_t index)
{
    const cerberus_wifi_archive_row_t *r = &s->document.rows[index];
    int best = r->rssi_valid ? r->rssi : -128;
    if (s->groups && !s->group_focus && r->ssid[0])
        for (uint8_t i = 0U; i < s->document.count; ++i) {
            const cerberus_wifi_archive_row_t *other = &s->document.rows[i];
            if (wifi_archive_ui_matches(s, other) && wifi_archive_ui_same_group(r, other) &&
                other->rssi_valid && other->rssi > best) best = other->rssi;
        }
    return best;
}

static int wifi_archive_ui_compare(const wifi_archive_ui_store_t *s, uint8_t a, uint8_t b)
{
    if (s->sort == 2U) {
        int ar = wifi_archive_ui_rank(s, a), br = wifi_archive_ui_rank(s, b);
        if (ar != br) return ar > br ? -1 : 1;
    }
    if (s->sort) {
        char an[33], bn[33];
        wifi_archive_ui_name(an, sizeof(an), s->document.rows[a].ssid);
        wifi_archive_ui_name(bn, sizeof(bn), s->document.rows[b].ssid);
        int result = strcmp(an, bn);
        if (result) return result;
    }
    return a < b ? -1 : a > b;
}

static void wifi_archive_ui_order(wifi_archive_ui_store_t *s)
{
    s->count = 0U;
    if (!s->has_document || s->document.count > 64U) return;
    for (uint8_t i = 0U; i < s->document.count; ++i) {
        const cerberus_wifi_archive_row_t *r = &s->document.rows[i];
        if (!wifi_archive_ui_matches(s, r)) continue;
        bool duplicate = false;
        if (s->groups && !s->group_focus)
            for (uint8_t j = 0U; j < s->count; ++j)
                if (wifi_archive_ui_same_group(r, &s->document.rows[s->indexes[j]])) { duplicate = true; break; }
        if (duplicate) continue;
        uint8_t insert = s->count;
        while (insert && wifi_archive_ui_compare(s, i, s->indexes[insert - 1U]) < 0) {
            s->indexes[insert] = s->indexes[insert - 1U]; --insert;
        }
        s->indexes[insert] = i; ++s->count;
    }
    if ((unsigned)s->page * 4U >= s->count) s->page = 0U;
}

static esp_err_t wifi_archive_ui_footer(const char *right)
{
    ESP_RETURN_ON_ERROR(draw_button(8, 426, 96, 44, "BACK"), TAG, "Wi-Fi file back");
    if (right) {
        ESP_RETURN_ON_ERROR(draw_small_button(112, 426, 96, 44, "SAVE COPY"), TAG, "Wi-Fi file copy");
        return draw_small_button(216, 426, 96, 44, "EXPORT");
    }
    return ESP_OK;
}

static esp_err_t wifi_archive_ui_draw(wifi_archive_ui_store_t *s, esp_err_t open_error)
{
    if (s) { s->paint_valid = false; s->painted_count = 0U; }
    ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(0, 0, 320, 480, UI_BACKGROUND), TAG, "Wi-Fi file clear");
    ESP_RETURN_ON_ERROR(draw_header("SAVED WI-FI LIST"), TAG, "Wi-Fi file header");
    char line[54];
    ESP_RETURN_ON_ERROR(draw_centered(54, "HISTORICAL OBSERVATIONS / NOT LIVE TARGETS", UI_MUTED, 1), TAG, "Wi-Fi history");
    if (!s || s->view == WIFI_ARCHIVE_ERROR) {
        ESP_RETURN_ON_ERROR(draw_centered(112, "FILE OPERATION FAILED", UI_TEXT, 1), TAG, "Wi-Fi file failed");
        ESP_RETURN_ON_ERROR(draw_centered(144, esp_err_to_name(s ? s->error : open_error), UI_MUTED, 1), TAG, "Wi-Fi file error");
        if (s && s->failure_name[0]) {
            snprintf(line, sizeof(line), "%.48s", s->failure_name);
            ESP_RETURN_ON_ERROR(draw_centered(208, line, UI_MUTED, 1), TAG, "Wi-Fi failed name");
            if (s->write_started)
                ESP_RETURN_ON_ERROR(draw_centered(246, "FAILED SAVE MAY LEAVE A .TMP FILE", UI_MUTED, 1), TAG, "Wi-Fi temporary");
        }
        if (s && s->has_document)
            ESP_RETURN_ON_ERROR(draw_button(48, 374, 224, 44, "VIEW RETAINED LIST"), TAG, "Wi-Fi retained");
        return wifi_archive_ui_footer(NULL);
    }
    if (s->view == WIFI_ARCHIVE_JOB) {
        ESP_RETURN_ON_ERROR(draw_centered(136, s->release_pending ? "RELEASING OWNED SCAN" :
            s->saving ? "WRITING WI-FI LIST" : "READING WI-FI LIST", UI_TEXT, 1), TAG, "Wi-Fi file pending");
        ESP_RETURN_ON_ERROR(draw_centered(180, "BACK CANCELS THE VIEW", UI_MUTED, 1), TAG, "Wi-Fi cancel");
        if (s->saving) {
            ESP_RETURN_ON_ERROR(draw_centered(212, "AN IN-FLIGHT SAVE MAY STILL FINISH", UI_MUTED, 1), TAG, "Wi-Fi save outcome");
            ESP_RETURN_ON_ERROR(draw_centered(252, "/CERBERUS/WIFI / NEW .WFI FILE", UI_MUTED, 1), TAG, "Wi-Fi save path");
        }
        return wifi_archive_ui_footer(NULL);
    }
    snprintf(line, sizeof(line), "%.48s", s->file_name);
    ESP_RETURN_ON_ERROR(draw_centered(76, line, UI_TEXT, 1), TAG, "Wi-Fi filename");
    if (s->view == WIFI_ARCHIVE_DETAIL && s->selected_valid) {
        const cerberus_wifi_archive_row_t *r = &s->selected;
        char name[33], address[18];
        wifi_archive_ui_name(name, sizeof(name), r->ssid);
        ESP_RETURN_ON_ERROR(draw_centered(104, name[0] ? name : "[HIDDEN SSID]", UI_TEXT, 1), TAG, "Wi-Fi saved SSID");
        cerberus_privacy_format_wifi_identifier(r->bssid, privacy_addresses_masked(), address, sizeof(address));
        ESP_RETURN_ON_ERROR(draw_centered(130, address, UI_TEXT, 2), TAG, "Wi-Fi saved BSSID");
        snprintf(line, sizeof(line), "%s / %s / CHANNEL %u / %s", wifi_archive_ui_source(r->source),
            r->channel <= 14U ? "2.4G" : "5G", r->channel, wifi_security_text(r->security));
        ESP_RETURN_ON_ERROR(draw_centered(160, line, UI_TEXT, 1), TAG, "Wi-Fi saved source");
        if (r->rssi_valid) snprintf(line, sizeof(line), "SIGNAL %d dBm / AGE UNKNOWN", r->rssi);
        else snprintf(line, sizeof(line), "SIGNAL UNKNOWN / AGE UNKNOWN");
        ESP_RETURN_ON_ERROR(draw_centered(184, line, UI_MUTED, 1), TAG, "Wi-Fi saved signal");
        if (r->source == CERBERUS_WIFI_ARCHIVE_SOURCE_S3)
            snprintf(line, sizeof(line), "S3 FRAME PRESENCE / RF AGE UNKNOWN");
        else if (r->frame_age_upper_valid)
            snprintf(line, sizeof(line), "C5 FRAME AGE AT SAVE <= %llu ms", (unsigned long long)r->frame_age_upper_at_save_ms);
        else snprintf(line, sizeof(line), "C5 FRAME AGE AT SAVE UNKNOWN");
        ESP_RETURN_ON_ERROR(draw_centered(212, line, UI_MUTED, 1), TAG, "Wi-Fi saved age");
        snprintf(line, sizeof(line), "ORIGINAL COVERAGE: S3 %s / C5 %s",
            wifi_archive_ui_coverage(s->document.publication.s3_coverage),
            wifi_archive_ui_coverage(s->document.publication.c5_coverage));
        ESP_RETURN_ON_ERROR(draw_centered(268, line, UI_MUTED, 1), TAG, "Wi-Fi saved coverage");
        snprintf(line, sizeof(line), "ORIGINAL SCAN: S3 %u / C5 %u OBSERVATIONS",
            s->document.publication.s3_count, s->document.publication.c5_count);
        ESP_RETURN_ON_ERROR(draw_centered(292, line, UI_MUTED, 1), TAG, "Wi-Fi original count");
        snprintf(line, sizeof(line), "SAVE COPY KEEPS ALL %u FILE OBSERVATIONS", s->document.count);
        ESP_RETURN_ON_ERROR(draw_centered(340, line, UI_MUTED, 1), TAG, "Wi-Fi copy scope");
        ESP_RETURN_ON_ERROR(draw_button(8, 374, 304, 44, "NEW SCAN"), TAG, "Wi-Fi new scan");
        return wifi_archive_ui_footer("SAVE COPY .WFI");
    }
    wifi_archive_ui_order(s);
    ESP_RETURN_ON_ERROR(draw_button(8, 96, 148, 30, s->groups ? "APS" : "APS *"), TAG, "Wi-Fi AP tab");
    ESP_RETURN_ON_ERROR(draw_button(164, 96, 148, 30, s->groups ? "SSIDS *" : "SSIDS"), TAG, "Wi-Fi SSID tab");
    static const char *sources[] = {"ALL RX", "S3", "C5"};
    static const char *bands[] = {"ALL BANDS", "2.4 GHZ", "5 GHZ"};
    static const char *sorts[] = {"FILE ORDER", "NAME A-Z", "SIGNAL"};
    ESP_RETURN_ON_ERROR(draw_small_button(8, 132, 96, 32, sources[s->source]), TAG, "Wi-Fi source filter");
    ESP_RETURN_ON_ERROR(draw_small_button(112, 132, 96, 32, bands[s->band]), TAG, "Wi-Fi band filter");
    ESP_RETURN_ON_ERROR(draw_small_button(216, 132, 96, 32, sorts[s->sort]), TAG, "Wi-Fi order");
    for (unsigned n = 0U; n < 4U && s->page * 4U + n < s->count; ++n) {
        const uint8_t index = s->indexes[s->page * 4U + n];
        const cerberus_wifi_archive_row_t *r = &s->document.rows[index];
        char name[33]; wifi_archive_ui_name(name, sizeof(name), r->ssid);
        uint16_t y = (uint16_t)(174U + n * 44U);
        ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(8, y, 304, 40, UI_PANEL), TAG, "Wi-Fi saved row");
        ESP_RETURN_ON_ERROR(cerberus_display_draw_text(14, y + 5U, name[0] ? name : "[HIDDEN SSID]", UI_TEXT, UI_PANEL, 1), TAG, "Wi-Fi row name");
        if (s->groups && !s->group_focus && r->ssid[0]) {
            unsigned s3 = 0U, c5 = 0U;
            for (uint8_t j = 0U; j < s->document.count; ++j) {
                const cerberus_wifi_archive_row_t *other = &s->document.rows[j];
                if (wifi_archive_ui_matches(s, other) && wifi_archive_ui_same_group(r, other)) {
                    if (other->source == CERBERUS_WIFI_ARCHIVE_SOURCE_S3) ++s3; else ++c5;
                }
            }
            snprintf(line, sizeof(line), "%u OBSERVATIONS / S3 %u / C5 %u", s3 + c5, s3, c5);
        } else if (r->rssi_valid) snprintf(line, sizeof(line), "%s / CH %u / %d dBm / %s",
            wifi_archive_ui_source(r->source), r->channel, r->rssi, wifi_security_text(r->security));
        else snprintf(line, sizeof(line), "%s / CH %u / SIGNAL UNKNOWN / %s",
            wifi_archive_ui_source(r->source), r->channel, wifi_security_text(r->security));
        ESP_RETURN_ON_ERROR(cerberus_display_draw_text(14, y + 23U, line, UI_MUTED, UI_PANEL, 1), TAG, "Wi-Fi row meta");
        s->painted[n] = index; ++s->painted_count;
    }
    if (!s->count) ESP_RETURN_ON_ERROR(draw_centered(236, "NO OBSERVATIONS IN THIS VIEW", UI_MUTED, 1), TAG, "Wi-Fi empty");
    snprintf(line, sizeof(line), "%u %s / PAGE %u/%u%s", s->count,
        s->groups && !s->group_focus ? "SSID ROWS" : "OBSERVATIONS", s->page + 1U,
        s->count ? (s->count + 3U) / 4U : 1U, s->document.subset ? " / SAVED SUBSET" : "");
    ESP_RETURN_ON_ERROR(draw_centered(354, line, UI_MUTED, 1), TAG, "Wi-Fi page count");
    ESP_RETURN_ON_ERROR(draw_button(8, 374, 148, 44, "PREV"), TAG, "Wi-Fi previous");
    ESP_RETURN_ON_ERROR(draw_button(164, 374, 148, 44, "NEXT"), TAG, "Wi-Fi next");
    ESP_RETURN_ON_ERROR(wifi_archive_ui_footer("SAVE COPY .WFI"), TAG, "Wi-Fi file footer");
    s->paint_valid = true;
    return ESP_OK;
}

static wifi_archive_ui_action_t wifi_archive_ui_tap(wifi_archive_ui_store_t *s, uint16_t x, uint16_t y)
{
    if (x >= 8U && x < 104U && y >= 426U && y < 470U) {
        if (s && s->view == WIFI_ARCHIVE_DETAIL) {
            s->view = WIFI_ARCHIVE_LIST; wifi_archive_ui_invalidate(s); return WIFI_ARCHIVE_REDRAW;
        }
        if (s && s->view == WIFI_ARCHIVE_LIST && s->group_focus) {
            s->group_focus = false; s->page = 0; wifi_archive_ui_invalidate(s); return WIFI_ARCHIVE_REDRAW;
        }
        return WIFI_ARCHIVE_BACK;
    }
    if (!s || s->job_pending || s->release_pending || !s->has_document) return WIFI_ARCHIVE_NONE;
    if (s->view == WIFI_ARCHIVE_ERROR) {
        if (x >= 48U && x < 272U && y >= 374U && y < 418U) {
            s->view = WIFI_ARCHIVE_LIST; return WIFI_ARCHIVE_REDRAW;
        }
        return WIFI_ARCHIVE_NONE;
    }
    if (x >= 112U && x < 208U && y >= 426U && y < 470U) return WIFI_ARCHIVE_SAVE;
    if (x >= 216U && x < 312U && y >= 426U && y < 470U) return WIFI_ARCHIVE_EXPORT;
    if (s->view == WIFI_ARCHIVE_DETAIL)
        return x >= 8U && x < 312U && y >= 374U && y < 418U ? WIFI_ARCHIVE_NEW_SCAN : WIFI_ARCHIVE_NONE;
    if (s->view != WIFI_ARCHIVE_LIST) return WIFI_ARCHIVE_NONE;
    if (y >= 96U && y < 126U && ((x >= 8U && x < 156U) || (x >= 164U && x < 312U))) {
        s->groups = x >= 164U; s->group_focus = false; s->page = 0;
    } else if (y >= 132U && y < 164U) {
        if (x >= 8U && x < 104U) s->source = (uint8_t)((s->source + 1U) % 3U);
        else if (x >= 112U && x < 208U) s->band = (uint8_t)((s->band + 1U) % 3U);
        else if (x >= 216U && x < 312U) s->sort = (uint8_t)((s->sort + 1U) % 3U);
        else return WIFI_ARCHIVE_NONE;
        s->page = 0;
    } else if (y >= 374U && y < 418U) {
        if (x >= 8U && x < 156U) { if (s->page) --s->page; }
        else if (x >= 164U && x < 312U) { if ((s->page + 1U) * 4U < s->count) ++s->page; }
        else return WIFI_ARCHIVE_NONE;
    } else if (s->paint_valid && x >= 8U && x < 312U && y >= 174U && y < 350U && (y - 174U) % 44U < 40U) {
        const unsigned n = (y - 174U) / 44U;
        if (n >= s->painted_count) return WIFI_ARCHIVE_NONE;
        s->selected = s->document.rows[s->painted[n]];
        if (s->groups && !s->group_focus && s->selected.ssid[0]) {
            memcpy(s->group_ssid, s->selected.ssid, sizeof(s->group_ssid));
            s->group_focus = true; s->page = 0;
        } else {
            s->selected_valid = true; s->paint_valid = false; s->view = WIFI_ARCHIVE_DETAIL;
            return WIFI_ARCHIVE_REDRAW;
        }
    } else return WIFI_ARCHIVE_NONE;
    wifi_archive_ui_invalidate(s);
    return WIFI_ARCHIVE_REDRAW;
}
