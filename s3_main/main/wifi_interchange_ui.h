#pragma once

/* Offline interchange presentation. The immutable typed document supplies all
 * values; no current receiver, clock, live AP selection or radio is consulted. */
#include "wifi_marauder.h"

typedef enum { WIFI_INTERCHANGE_LIST, WIFI_INTERCHANGE_DETAIL, WIFI_INTERCHANGE_CHOICE,
    WIFI_INTERCHANGE_JOB, WIFI_INTERCHANGE_ERROR } wifi_interchange_page_t;
typedef enum { WIFI_INTERCHANGE_NONE, WIFI_INTERCHANGE_REDRAW, WIFI_INTERCHANGE_BACK,
    WIFI_INTERCHANGE_SAVE, WIFI_INTERCHANGE_EXPORT_AP, WIFI_INTERCHANGE_EXPORT_SSID,
    WIFI_INTERCHANGE_EDIT_COPY } wifi_interchange_action_t;
typedef struct {
    union { cerberus_wifi_marauder_ap_t ap; cerberus_wifi_marauder_ssid_t ssid; } document;
    cerberus_wifi_marauder_ap_row_t selected;
    cerberus_wifi_marauder_export_stats_t stats;
    uint64_t job_id;
    uint8_t page, detail_page, painted[4], painted_count, selected_index;
    bool ap_json, has_document, selected_valid, paint_valid, job_pending;
    bool saving, exporting, write_started, stats_valid;
    wifi_interchange_page_t view;
    cerberus_wifi_marauder_result_t result;
    esp_err_t error;
    char file_name[256], failure_name[256];
} wifi_interchange_ui_store_t;

static void wifi_interchange_invalidate(wifi_interchange_ui_store_t *s)
{ s->paint_valid = s->selected_valid = false; s->painted_count = 0; }

static void wifi_interchange_error(wifi_interchange_ui_store_t *s, esp_err_t error)
{ wifi_interchange_invalidate(s); s->job_pending = false; s->error = error; s->view = WIFI_INTERCHANGE_ERROR; }

static void wifi_interchange_text(char *out, size_t capacity, const uint8_t *bytes, size_t length)
{
    size_t n = length < capacity - 1U ? length : capacity - 1U;
    for (size_t i = 0; i < n; ++i) out[i] = bytes[i] >= 32U && bytes[i] <= 126U ? (char)bytes[i] : '.';
    out[n] = 0;
}

static void wifi_interchange_name(char *out, size_t capacity, const char *name)
{ wifi_interchange_text(out, capacity, (const uint8_t *)name, name ? strlen(name) : 0U); }

static uint8_t wifi_interchange_count(const wifi_interchange_ui_store_t *s)
{ return s->ap_json ? s->document.ap.count : s->document.ssid.count; }

static esp_err_t wifi_interchange_footer(bool save)
{
    ESP_RETURN_ON_ERROR(draw_button(8, 426, 96, 44, "BACK"), TAG, "interchange back");
    return save ? draw_button(112, 426, 200, 44, "SAVE COPY") : ESP_OK;
}

static esp_err_t wifi_interchange_hex(uint16_t y, const uint8_t *bytes, uint8_t length)
{
    char line[48];
    if (!length) return draw_centered(y, "[EMPTY]", UI_MUTED, 1);
    for (unsigned start = 0; start < length; start += 12U) {
        size_t used = 0;
        for (unsigned i = start; i < length && i < start + 12U; ++i)
            used += (size_t)snprintf(line + used, sizeof(line) - used, "%02X%s", bytes[i],
                i + 1U < length && i + 1U < start + 12U ? " " : "");
        ESP_RETURN_ON_ERROR(draw_centered(y, line, UI_TEXT, 1), TAG, "interchange bytes");
        y += 18U;
    }
    return ESP_OK;
}

static uint8_t wifi_interchange_detail_pages(const wifi_interchange_ui_store_t *s)
{ return !s->ap_json ? 1U : (uint8_t)(2U + (s->selected.station_count ? (s->selected.station_count + 7U) / 8U : 1U)); }

static esp_err_t wifi_interchange_detail(const wifi_interchange_ui_store_t *s)
{
    char line[54], text[33], address[18];
    if (!s->ap_json) {
        const cerberus_wifi_marauder_ssid_row_t *row = &s->document.ssid.rows[s->selected_index];
        wifi_interchange_text(text, sizeof(text), row->bytes, row->length);
        ESP_RETURN_ON_ERROR(draw_centered(112, row->length ? text : "[EMPTY NAME]", UI_TEXT, 1), TAG, "SSID name");
        snprintf(line, sizeof(line), "RECORD %u / %u NAME BYTES", s->selected_index + 1U, row->length);
        ESP_RETURN_ON_ERROR(draw_centered(146, line, UI_MUTED, 1), TAG, "SSID length");
        ESP_RETURN_ON_ERROR(draw_centered(182, "EXACT NAME BYTES / HEX", UI_MUTED, 1), TAG, "SSID hex label");
        ESP_RETURN_ON_ERROR(wifi_interchange_hex(208, row->bytes, row->length), TAG, "SSID hex");
        ESP_RETURN_ON_ERROR(draw_centered(300, s->document.ssid.final_lf ?
            "FILE ENDS WITH LF" : "FILE HAS NO FINAL LF", UI_MUTED, 1), TAG, "SSID framing");
        ESP_RETURN_ON_ERROR(draw_centered(328, "NO AP, SIGNAL OR SECURITY DATA", UI_MUTED, 1), TAG, "SSID scope");
    } else {
        const cerberus_wifi_marauder_ap_row_t *r = &s->selected;
        if (!s->detail_page) {
            wifi_interchange_text(text, sizeof(text), r->essid, r->essid_length);
            ESP_RETURN_ON_ERROR(draw_centered(106, !(r->present & CERBERUS_WIFI_MARAUDER_AP_ESSID) ?
                "[SSID NOT PROVIDED]" : r->essid_length ? text : "[EMPTY SSID]", UI_TEXT, 1), TAG, "AP SSID");
            if (r->present & CERBERUS_WIFI_MARAUDER_AP_BSSID)
                cerberus_privacy_format_wifi_identifier(r->bssid, privacy_addresses_masked(), address, sizeof(address));
            else snprintf(address, sizeof(address), "NOT PROVIDED");
            snprintf(line, sizeof(line), "BSSID: %s", address);
            ESP_RETURN_ON_ERROR(draw_centered(132, line, UI_TEXT, 1), TAG, "AP BSSID");
            if (r->present & CERBERUS_WIFI_MARAUDER_AP_CHANNEL) snprintf(line, sizeof(line), "CHANNEL: %u", r->channel);
            else snprintf(line, sizeof(line), "CHANNEL: NOT PROVIDED");
            ESP_RETURN_ON_ERROR(draw_centered(158, line, UI_TEXT, 1), TAG, "AP channel");
            if (r->present & CERBERUS_WIFI_MARAUDER_AP_RSSI) snprintf(line, sizeof(line), "STORED RSSI: %ld / AGE UNKNOWN", (long)r->rssi);
            else snprintf(line, sizeof(line), "RSSI: NOT PROVIDED");
            ESP_RETURN_ON_ERROR(draw_centered(184, line, UI_TEXT, 1), TAG, "AP signal");
            if (r->present & CERBERUS_WIFI_MARAUDER_AP_SEC) snprintf(line, sizeof(line), "MARAUDER SECURITY CODE: %u", r->security);
            else snprintf(line, sizeof(line), "SECURITY: NOT PROVIDED");
            ESP_RETURN_ON_ERROR(draw_centered(210, line, UI_TEXT, 1), TAG, "AP security");
            if (r->present & CERBERUS_WIFI_MARAUDER_AP_PACKETS) snprintf(line, sizeof(line), "STORED PACKET COUNT: %lu", (unsigned long)r->packets);
            else snprintf(line, sizeof(line), "PACKET COUNT: NOT PROVIDED");
            ESP_RETURN_ON_ERROR(draw_centered(236, line, UI_TEXT, 1), TAG, "AP packets");
            snprintf(line, sizeof(line), "WPS: %s", !(r->present & CERBERUS_WIFI_MARAUDER_AP_WPS) ?
                "NOT PROVIDED" : r->wps ? "TRUE" : "FALSE");
            ESP_RETURN_ON_ERROR(draw_centered(262, line, UI_TEXT, 1), TAG, "AP WPS");
            ESP_RETURN_ON_ERROR(draw_centered(288, r->present & CERBERUS_WIFI_MARAUDER_AP_MAN ?
                "STORED MANUFACTURER TEXT" : "MANUFACTURER: NOT PROVIDED", UI_MUTED, 1), TAG, "AP manufacturer");
            if (r->present & CERBERUS_WIFI_MARAUDER_AP_MAN) {
                wifi_interchange_text(text, sizeof(text), r->man, r->man_length);
                ESP_RETURN_ON_ERROR(draw_centered(308, r->man_length ? text : "[EMPTY]", UI_TEXT, 1), TAG, "AP manufacturer first");
                if (r->man_length > 32U) {
                    wifi_interchange_text(text, sizeof(text), r->man + 32U, r->man_length - 32U);
                    ESP_RETURN_ON_ERROR(draw_centered(326, text, UI_TEXT, 1), TAG, "AP manufacturer rest");
                }
            }
        } else if (s->detail_page == 1U) {
            ESP_RETURN_ON_ERROR(draw_centered(106, "SSID BYTES / HEX", UI_MUTED, 1), TAG, "AP name hex label");
            if (r->present & CERBERUS_WIFI_MARAUDER_AP_ESSID)
                ESP_RETURN_ON_ERROR(wifi_interchange_hex(126, r->essid, r->essid_length), TAG, "AP name hex");
            else ESP_RETURN_ON_ERROR(draw_centered(126, "NOT PROVIDED", UI_MUTED, 1), TAG, "AP name absent");
            ESP_RETURN_ON_ERROR(draw_centered(190, "MANUFACTURER BYTES / HEX", UI_MUTED, 1), TAG, "AP manufacturer hex label");
            if (r->present & CERBERUS_WIFI_MARAUDER_AP_MAN)
                ESP_RETURN_ON_ERROR(wifi_interchange_hex(210, r->man, r->man_length), TAG, "AP manufacturer hex");
            else ESP_RETURN_ON_ERROR(draw_centered(210, "NOT PROVIDED", UI_MUTED, 1), TAG, "AP manufacturer absent");
        } else {
            snprintf(line, sizeof(line), "STORED STATIONS: %u", r->station_count);
            ESP_RETURN_ON_ERROR(draw_centered(106, line, UI_TEXT, 1), TAG, "AP station count");
            if (!(r->present & CERBERUS_WIFI_MARAUDER_AP_STATIONS))
                ESP_RETURN_ON_ERROR(draw_centered(160, "STATIONS NOT PROVIDED", UI_MUTED, 1), TAG, "AP stations absent");
            else if (!r->station_count)
                ESP_RETURN_ON_ERROR(draw_centered(160, "EMPTY STATION LIST", UI_MUTED, 1), TAG, "AP stations empty");
            for (unsigned n = 0, i = (s->detail_page - 2U) * 8U; n < 8U && i < r->station_count; ++n, ++i) {
                cerberus_privacy_format_wifi_identifier(s->document.ap.stations[r->station_offset + i],
                    privacy_addresses_masked(), address, sizeof(address));
                snprintf(line, sizeof(line), "%u  %s", i + 1U, address);
                ESP_RETURN_ON_ERROR(draw_centered((uint16_t)(132U + n * 26U), line, UI_TEXT, 1), TAG, "AP station");
            }
        }
    }
    snprintf(line, sizeof(line), "DETAIL PAGE %u/%u", s->detail_page + 1U, wifi_interchange_detail_pages(s));
    return draw_centered(352, line, UI_MUTED, 1);
}

static esp_err_t wifi_interchange_ui_draw(wifi_interchange_ui_store_t *s, esp_err_t open_error)
{
    if (s) { s->paint_valid = false; s->painted_count = 0; }
    ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(0, 0, 320, 480, UI_BACKGROUND), TAG, "interchange clear");
    ESP_RETURN_ON_ERROR(draw_header("WI-FI FILES"), TAG, "interchange header");
    ESP_RETURN_ON_ERROR(draw_centered(54, "HISTORICAL FILE / SOURCE AND TIME UNKNOWN", UI_MUTED, 1), TAG, "interchange scope");
    char line[54], text[33];
    if (!s || s->view == WIFI_INTERCHANGE_ERROR) {
        ESP_RETURN_ON_ERROR(draw_centered(112, "FILE OPERATION FAILED", UI_TEXT, 1), TAG, "interchange failed");
        ESP_RETURN_ON_ERROR(draw_centered(144, esp_err_to_name(s ? s->error : open_error), UI_MUTED, 1), TAG, "interchange error");
        if (s && s->result == CERBERUS_WIFI_MARAUDER_UNREPRESENTABLE)
            ESP_RETURN_ON_ERROR(draw_centered(170, s->ap_json ? "NAME IS NOT VALID UTF-8 FOR JSON" :
                "A NAME CONTAINS A LINE BREAK", UI_MUTED, 1), TAG, "interchange representation");
        if (s && s->failure_name[0]) {
            snprintf(line, sizeof(line), "%.48s", s->failure_name);
            ESP_RETURN_ON_ERROR(draw_centered(196, line, UI_MUTED, 1), TAG, "interchange failed name");
            if (s->write_started) ESP_RETURN_ON_ERROR(draw_centered(230, "FAILED SAVE MAY LEAVE A .TMP FILE", UI_MUTED, 1), TAG, "interchange temporary");
        }
        if (s && s->has_document) ESP_RETURN_ON_ERROR(draw_button(48, 374, 224, 44, "VIEW RETAINED LIST"), TAG, "interchange retained");
        return wifi_interchange_footer(false);
    }
    if (s->view == WIFI_INTERCHANGE_CHOICE) {
        ESP_RETURN_ON_ERROR(draw_centered(110, "EXPORT THE COMPLETE SAVED LIST", UI_TEXT, 1), TAG, "export scope");
        ESP_RETURN_ON_ERROR(draw_centered(142, "SOURCE, TIMING AND COVERAGE ARE OMITTED", UI_MUTED, 1), TAG, "export loss");
        ESP_RETURN_ON_ERROR(draw_centered(174, "AP JSON KEEPS AVAILABLE AP FIELDS", UI_MUTED, 1), TAG, "AP export scope");
        ESP_RETURN_ON_ERROR(draw_centered(198, "SSID TEXT KEEPS NAMES ONLY", UI_MUTED, 1), TAG, "SSID export scope");
        ESP_RETURN_ON_ERROR(draw_centered(224, "ORDER AND DUPLICATES ARE KEPT", UI_MUTED, 1), TAG, "export duplicates");
        ESP_RETURN_ON_ERROR(draw_button(48, 266, 224, 48, "EXPORT AP JSON"), TAG, "AP export");
        ESP_RETURN_ON_ERROR(draw_button(48, 330, 224, 48, "EXPORT SSID TEXT"), TAG, "SSID export");
        return wifi_interchange_footer(false);
    }
    if (s->view == WIFI_INTERCHANGE_JOB) {
        ESP_RETURN_ON_ERROR(draw_centered(136, s->exporting ? "EXPORTING WI-FI LIST" :
            s->saving ? "SAVING FILE COPY" : "READING WI-FI FILE", UI_TEXT, 1), TAG, "interchange pending");
        ESP_RETURN_ON_ERROR(draw_centered(180, "BACK CANCELS THE VIEW", UI_MUTED, 1), TAG, "interchange cancel");
        if (s->saving) ESP_RETURN_ON_ERROR(draw_centered(216, "AN IN-FLIGHT SAVE MAY STILL FINISH", UI_MUTED, 1), TAG, "interchange save outcome");
        return wifi_interchange_footer(false);
    }
    snprintf(line, sizeof(line), "%.48s", s->file_name);
    ESP_RETURN_ON_ERROR(draw_centered(76, line, UI_TEXT, 1), TAG, "interchange file");
    if (s->view == WIFI_INTERCHANGE_DETAIL && s->selected_valid) {
        ESP_RETURN_ON_ERROR(wifi_interchange_detail(s), TAG, "interchange detail");
    } else {
        const uint8_t count = wifi_interchange_count(s);
        snprintf(line, sizeof(line), "%s / %u RECORDS", s->ap_json ? "AP JSON" : "SSID TEXT", count);
        ESP_RETURN_ON_ERROR(draw_centered(104, line, UI_TEXT, 1), TAG, "interchange count");
        if (s->stats_valid && s->ap_json) snprintf(line, sizeof(line), "UNKNOWN SEC %u / RSSI OMITTED %u", s->stats.security_unmapped, s->stats.rssi_omitted);
        else if (s->stats_valid) snprintf(line, sizeof(line), "NAMES %u / ABSENT SKIPPED %u", s->stats.records, s->stats.names_absent);
        else if (s->ap_json) snprintf(line, sizeof(line), "UNRETAINED JSON FIELDS: %u", s->document.ap.ignored_members);
        else snprintf(line, sizeof(line), "%s", s->document.ssid.final_lf ? "FILE ENDS WITH LF" : "FILE HAS NO FINAL LF");
        ESP_RETURN_ON_ERROR(draw_centered(128, line, UI_MUTED, 1), TAG, "interchange losses");
        ESP_RETURN_ON_ERROR(draw_centered(150, "ORIGINAL ORDER / DUPLICATES KEPT", UI_MUTED, 1), TAG, "interchange order");
        for (unsigned n = 0, i = s->page * 4U; n < 4U && i < count; ++n, ++i) {
            uint16_t y = (uint16_t)(174U + n * 44U);
            ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(8, y, 304, 40, UI_PANEL), TAG, "interchange row");
            if (s->ap_json) {
                const cerberus_wifi_marauder_ap_row_t *r = &s->document.ap.rows[i];
                wifi_interchange_text(text, sizeof(text), r->essid, r->essid_length);
                if (!(r->present & CERBERUS_WIFI_MARAUDER_AP_ESSID)) snprintf(text, sizeof(text), "[SSID NOT PROVIDED]");
                else if (!r->essid_length) snprintf(text, sizeof(text), "[EMPTY SSID]");
                if (r->present & CERBERUS_WIFI_MARAUDER_AP_BSSID) {
                    char address[18];
                    cerberus_privacy_format_wifi_identifier(r->bssid, privacy_addresses_masked(), address, sizeof(address));
                    snprintf(line, sizeof(line), "RECORD %u / %s", i + 1U, address);
                } else snprintf(line, sizeof(line), "RECORD %u / BSSID NOT PROVIDED", i + 1U);
            } else {
                const cerberus_wifi_marauder_ssid_row_t *r = &s->document.ssid.rows[i];
                wifi_interchange_text(text, sizeof(text), r->bytes, r->length);
                if (!r->length) snprintf(text, sizeof(text), "[EMPTY NAME]");
                snprintf(line, sizeof(line), "RECORD %u / %u NAME BYTES", i + 1U, r->length);
            }
            ESP_RETURN_ON_ERROR(cerberus_display_draw_text(14, y + 5U, text, UI_TEXT, UI_PANEL, 1), TAG, "interchange name");
            ESP_RETURN_ON_ERROR(cerberus_display_draw_text(14, y + 23U, line, UI_MUTED, UI_PANEL, 1), TAG, "interchange meta");
            s->painted[n] = (uint8_t)i; ++s->painted_count;
        }
        if (!count) ESP_RETURN_ON_ERROR(draw_centered(236, "EMPTY FILE LIST", UI_MUTED, 1), TAG, "interchange empty");
        snprintf(line, sizeof(line), "PAGE %u/%u", s->page + 1U, count ? (count + 3U) / 4U : 1U);
        ESP_RETURN_ON_ERROR(draw_centered(354, line, UI_MUTED, 1), TAG, "interchange page");
    }
    ESP_RETURN_ON_ERROR(draw_button(8, 374, s->ap_json ? 148 : 96, 44, "PREV"), TAG, "interchange previous");
    ESP_RETURN_ON_ERROR(draw_button(s->ap_json ? 164 : 112, 374, s->ap_json ? 148 : 96, 44, "NEXT"), TAG, "interchange next");
    if (!s->ap_json) ESP_RETURN_ON_ERROR(draw_small_button(216, 374, 96, 44, "EDIT COPY"), TAG, "interchange edit");
    ESP_RETURN_ON_ERROR(wifi_interchange_footer(true), TAG, "interchange footer");
    if (s->view == WIFI_INTERCHANGE_LIST) s->paint_valid = true;
    return ESP_OK;
}

static wifi_interchange_action_t wifi_interchange_ui_tap(wifi_interchange_ui_store_t *s, uint16_t x, uint16_t y)
{
    if (x >= 8U && x < 104U && y >= 426U && y < 470U) {
        if (s && s->view == WIFI_INTERCHANGE_DETAIL) {
            s->view = WIFI_INTERCHANGE_LIST; wifi_interchange_invalidate(s); return WIFI_INTERCHANGE_REDRAW;
        }
        return WIFI_INTERCHANGE_BACK;
    }
    if (!s || s->job_pending) return WIFI_INTERCHANGE_NONE;
    if (s->view == WIFI_INTERCHANGE_CHOICE) {
        if (x >= 48U && x < 272U && y >= 266U && y < 314U) return WIFI_INTERCHANGE_EXPORT_AP;
        if (x >= 48U && x < 272U && y >= 330U && y < 378U) return WIFI_INTERCHANGE_EXPORT_SSID;
        return WIFI_INTERCHANGE_NONE;
    }
    if (!s->has_document) return WIFI_INTERCHANGE_NONE;
    if (s->view == WIFI_INTERCHANGE_ERROR) {
        if (x >= 48U && x < 272U && y >= 374U && y < 418U) { s->view = WIFI_INTERCHANGE_LIST; return WIFI_INTERCHANGE_REDRAW; }
        return WIFI_INTERCHANGE_NONE;
    }
    if (x >= 112U && x < 312U && y >= 426U && y < 470U) return WIFI_INTERCHANGE_SAVE;
    if (y >= 374U && y < 418U) {
        if (!s->ap_json && x >= 216U && x < 312U) return WIFI_INTERCHANGE_EDIT_COPY;
        uint8_t *page = s->view == WIFI_INTERCHANGE_DETAIL ? &s->detail_page : &s->page;
        unsigned pages = s->view == WIFI_INTERCHANGE_DETAIL ? wifi_interchange_detail_pages(s) : (wifi_interchange_count(s) + 3U) / 4U;
        if (x >= 8U && x < (s->ap_json ? 156U : 104U)) { if (*page) --*page; }
        else if (x >= (s->ap_json ? 164U : 112U) && x < (s->ap_json ? 312U : 208U)) { if (*page + 1U < pages) ++*page; }
        else return WIFI_INTERCHANGE_NONE;
        s->paint_valid = false; return WIFI_INTERCHANGE_REDRAW;
    }
    if (s->view == WIFI_INTERCHANGE_LIST && s->paint_valid && x >= 8U && x < 312U &&
        y >= 174U && y < 350U && (y - 174U) % 44U < 40U) {
        unsigned n = (y - 174U) / 44U;
        if (n >= s->painted_count) return WIFI_INTERCHANGE_NONE;
        s->selected_index = s->painted[n];
        if (s->ap_json) s->selected = s->document.ap.rows[s->selected_index];
        s->selected_valid = true; s->paint_valid = false; s->detail_page = 0; s->view = WIFI_INTERCHANGE_DETAIL;
        return WIFI_INTERCHANGE_REDRAW;
    }
    return WIFI_INTERCHANGE_NONE;
}
