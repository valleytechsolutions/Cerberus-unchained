#pragma once

/* Offline JSON presentation only. The committed pool is immutable between
 * worker publications; accepting a job revokes every painted/selected reference.
 * Imported packets have no receiver, address type, clock or ADV/RSP identity. */
#include "airtag_json.h"

typedef enum { AIRTAG_JSON_LIST, AIRTAG_JSON_RAW, AIRTAG_JSON_JOB,
    AIRTAG_JSON_ERROR, AIRTAG_JSON_CONFIRM } airtag_json_ui_page_t;
typedef enum { AIRTAG_JSON_NONE, AIRTAG_JSON_REDRAW, AIRTAG_JSON_BACK,
    AIRTAG_JSON_SAVE, AIRTAG_JSON_EXPORT, AIRTAG_JSON_FIND_LIVE } airtag_json_ui_action_t;
typedef struct {
    cerberus_airtag_json_t document;
    cerberus_airtag_json_record_t painted[4], selected;
    cerberus_airtag_json_export_stats_t stats;
    uint64_t job_id;
    uint16_t raw_page;
    uint8_t page, painted_count, painted_indexes[4], selected_index;
    bool has_document, paint_valid, selected_valid, job_pending, exporting, saving, write_started;
    airtag_json_ui_page_t view;
    esp_err_t error;
    char file_name[256], failure_name[256];
} airtag_json_ui_store_t;

static inline void airtag_json_ui_invalidate(airtag_json_ui_store_t *s)
{ s->paint_valid = s->selected_valid = false; s->painted_count = 0U; }

static inline void airtag_json_ui_error(airtag_json_ui_store_t *s, esp_err_t error)
{
    if (!s) return;
    airtag_json_ui_invalidate(s); s->job_pending = false;
    s->error = error; s->view = AIRTAG_JSON_ERROR;
}

static inline void airtag_json_ui_name(airtag_json_ui_store_t *s, const char *name)
{
    size_t i = 0U;
    for (; i + 1U < sizeof(s->file_name) && name && name[i]; ++i)
        s->file_name[i] = (uint8_t)name[i] >= 32U && (uint8_t)name[i] <= 126U ? name[i] : '.';
    s->file_name[i] = 0;
}

static inline esp_err_t airtag_json_ui_footer(const char *right)
{
    ESP_RETURN_ON_ERROR(draw_button(8, 426, 96, 44, "BACK"), TAG, "JSON back");
    return right ? draw_button(112, 426, 200, 44, right) : ESP_OK;
}

static inline esp_err_t airtag_json_ui_draw(airtag_json_ui_store_t *s, esp_err_t open_error)
{
    if (s) { s->paint_valid = false; s->painted_count = 0U; }
    ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(0, 0, 320, 480, UI_BACKGROUND), TAG, "JSON clear");
    ESP_RETURN_ON_ERROR(draw_header("AIRTAG JSON"), TAG, "JSON title");
    ESP_RETURN_ON_ERROR(draw_centered(56, "HISTORICAL FILE DATA", UI_MUTED, 1), TAG, "JSON scope");
    char line[54];
    if (!s || s->view == AIRTAG_JSON_ERROR) {
        esp_err_t error = s ? s->error : open_error;
        ESP_RETURN_ON_ERROR(draw_centered(98, "FILE OPERATION FAILED", UI_TEXT, 1), TAG, "JSON error");
        snprintf(line, sizeof(line), "%.48s", esp_err_to_name(error));
        ESP_RETURN_ON_ERROR(draw_centered(122, line, UI_MUTED, 1), TAG, "JSON code");
        if (s && s->exporting) {
            ESP_RETURN_ON_ERROR(draw_centered(158, error == ESP_ERR_NOT_FOUND ? "NO COMPLETE ELIGIBLE FINDMY PACKETS" :
                error == ESP_ERR_INVALID_SIZE ? "EXPORT LIMIT / NO PARTIAL FILE" : "EXPORT DID NOT COMPLETE", UI_TEXT, 1), TAG, "JSON failure");
            snprintf(line, sizeof(line), "ELIGIBLE %u / FILTERED %u / INCOMPLETE %u",
                s->stats.eligible, s->stats.filtered, s->stats.incomplete);
            ESP_RETURN_ON_ERROR(draw_centered(186, line, UI_MUTED, 1), TAG, "JSON statistics");
        }
        if (s && s->failure_name[0]) {
            snprintf(line, sizeof(line), "%.48s", s->failure_name);
            ESP_RETURN_ON_ERROR(draw_centered(226, line, UI_MUTED, 1), TAG, "JSON failure path");
        }
        if (s && s->write_started && s->failure_name[0])
            ESP_RETURN_ON_ERROR(draw_centered(270, "FAILED SAVE MAY LEAVE A .TMP FILE", UI_MUTED, 1), TAG, "JSON failure retain");
        if (s && s->has_document)
            ESP_RETURN_ON_ERROR(draw_button(48, 374, 224, 44, "VIEW PREVIOUS"), TAG, "JSON previous");
        return airtag_json_ui_footer(NULL);
    }
    if (s->view == AIRTAG_JSON_CONFIRM) {
        ESP_RETURN_ON_ERROR(draw_centered(96, "EXPORT FINDMY PACKETS FROM THIS .BLE", UI_TEXT, 1), TAG, "JSON confirm");
        ESP_RETURN_ON_ERROR(draw_centered(126, "ALL SAVED ROWS / NOT THE LIST FILTER", UI_MUTED, 1), TAG, "JSON all rows");
        ESP_RETURN_ON_ERROR(draw_centered(158, "COMPLETE QUALIFYING ADV + MATCHED RSP", UI_TEXT, 1), TAG, "JSON packet scope");
        ESP_RETURN_ON_ERROR(draw_centered(182, "SEPARATE RECORDS / DUPLICATES KEPT", UI_MUTED, 1), TAG, "JSON no merge");
        ESP_RETURN_ON_ERROR(draw_centered(216, "JSON KEEPS ONLY MAC + PAYLOAD BYTES", UI_TEXT, 1), TAG, "JSON loss");
        ESP_RETURN_ON_ERROR(draw_centered(240, "LOSES SOURCE, ADDRESS TYPE, TIME, RSSI", UI_MUTED, 1), TAG, "JSON metadata loss");
        ESP_RETURN_ON_ERROR(draw_centered(264, "ADV / RESPONSE IDENTITY IS ALSO LOST", UI_MUTED, 1), TAG, "JSON packet loss");
        ESP_RETURN_ON_ERROR(draw_centered(300, "LIMIT: 64 RECORDS / 8192 PAYLOAD BYTES", UI_MUTED, 1), TAG, "JSON limits");
        ESP_RETURN_ON_ERROR(draw_centered(324, "INCOMPLETE / UNPROVEN PACKETS SKIPPED", UI_MUTED, 1), TAG, "JSON omission");
        ESP_RETURN_ON_ERROR(draw_centered(348, "SAVE DIRECTORY: /CERBERUS/BLE", UI_MUTED, 1), TAG, "JSON output path");
        return airtag_json_ui_footer("EXPORT JSON");
    }
    if (s->view == AIRTAG_JSON_JOB) {
        ESP_RETURN_ON_ERROR(draw_centered(140, s->exporting ? "EXPORTING FINDMY JSON" :
            s->saving ? "WRITING JSON COPY" : "READING JSON", UI_TEXT, 1), TAG, "JSON pending");
        ESP_RETURN_ON_ERROR(draw_centered(184, "BACK CANCELS THE VIEW", UI_MUTED, 1), TAG, "JSON cancel");
        if (s->saving) {
            ESP_RETURN_ON_ERROR(draw_centered(210, "AN IN-FLIGHT SAVE MAY STILL FINISH", UI_MUTED, 1), TAG, "JSON save scope");
            ESP_RETURN_ON_ERROR(draw_centered(246, "SAVE DIRECTORY: /CERBERUS/BLE", UI_MUTED, 1), TAG, "JSON output directory");
        }
        return airtag_json_ui_footer(NULL);
    }
    if (s->view == AIRTAG_JSON_RAW && s->selected_valid) {
        const cerberus_airtag_json_record_t *r = &s->selected;
        char address[18];
        cerberus_privacy_format_ble_identifier(r->address, privacy_addresses_masked(), address, sizeof(address));
        snprintf(line, sizeof(line), "RECORD %u / %s", s->selected_index + 1U, address);
        ESP_RETURN_ON_ERROR(draw_centered(86, line, UI_TEXT, 1), TAG, "JSON selected");
        ESP_RETURN_ON_ERROR(draw_centered(108, "TYPE / SOURCE / TIME / RSSI UNKNOWN", UI_MUTED, 1), TAG, "JSON unknown");
        ESP_RETURN_ON_ERROR(draw_centered(130, "OPAQUE PAYLOAD / ADV OR RSP UNKNOWN", UI_MUTED, 1), TAG, "JSON packet unknown");
        const unsigned pages = r->length ? (r->length + 127U) / 128U : 1U;
        if (s->raw_page >= pages) s->raw_page = 0U;
        const unsigned offset = s->raw_page * 128U;
        snprintf(line, sizeof(line), "%u BYTES / OFFSET %u / PAGE %u/%u", r->length, offset, s->raw_page + 1U, pages);
        ESP_RETURN_ON_ERROR(draw_centered(152, line, UI_MUTED, 1), TAG, "JSON raw count");
        if (privacy_addresses_masked()) {
            ESP_RETURN_ON_ERROR(draw_centered(224, "RAW PAYLOAD HIDDEN BY ADDRESS PRIVACY", UI_TEXT, 1), TAG, "JSON privacy");
        } else if (!r->length) {
            ESP_RETURN_ON_ERROR(draw_centered(224, "EMPTY PAYLOAD RETAINED", UI_MUTED, 1), TAG, "JSON empty payload");
        } else {
            for (unsigned row = 0U; row < 8U && offset + row * 16U < r->length; ++row) {
                unsigned used = 0U;
                for (unsigned col = 0U; col < 16U && offset + row * 16U + col < r->length; ++col)
                    used += (unsigned)snprintf(line + used, sizeof(line) - used, "%s%02X", col ? " " : "",
                        s->document.payload[r->offset + offset + row * 16U + col]);
                ESP_RETURN_ON_ERROR(cerberus_display_draw_text(16, 180U + row * 20U, line, UI_TEXT, UI_BACKGROUND, 1), TAG, "JSON raw bytes");
            }
        }
        ESP_RETURN_ON_ERROR(draw_button(8, 374, 148, 44, "PREV BYTES"), TAG, "JSON raw prev");
        ESP_RETURN_ON_ERROR(draw_button(164, 374, 148, 44, "NEXT BYTES"), TAG, "JSON raw next");
        return airtag_json_ui_footer("FIND LIVE");
    }
    snprintf(line, sizeof(line), "%.48s", s->file_name);
    ESP_RETURN_ON_ERROR(draw_centered(80, line, UI_TEXT, 1), TAG, "JSON filename");
    ESP_RETURN_ON_ERROR(draw_centered(102, "TYPE / SOURCE / TIME / RSSI UNKNOWN", UI_MUTED, 1), TAG, "JSON unknown list");
    if (s->exporting) snprintf(line, sizeof(line), "PACKETS %u / FILTERED %u / INCOMPLETE %u",
        s->stats.eligible, s->stats.filtered, s->stats.incomplete);
    else snprintf(line, sizeof(line), "FILE ORDER / DUPLICATES PRESERVED");
    ESP_RETURN_ON_ERROR(draw_centered(124, line, UI_MUTED, 1), TAG, "JSON stats");
    const unsigned pages = s->document.count ? (s->document.count + 3U) / 4U : 1U;
    if (s->page >= pages) s->page = 0U;
    for (unsigned row = 0U; row < 4U && s->page * 4U + row < s->document.count; ++row) {
        const unsigned index = s->page * 4U + row;
        const cerberus_airtag_json_record_t *r = &s->document.records[index];
        const uint16_t y = (uint16_t)(148U + row * 50U);
        char address[18];
        cerberus_privacy_format_ble_identifier(r->address, privacy_addresses_masked(), address, sizeof(address));
        ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(8, y, 304, 46, UI_PANEL), TAG, "JSON row");
        snprintf(line, sizeof(line), "%02u  %s", index + 1U, address);
        ESP_RETURN_ON_ERROR(cerberus_display_draw_text(16, y + 8U, line, UI_TEXT, UI_PANEL, 1), TAG, "JSON row address");
        snprintf(line, sizeof(line), "%u PAYLOAD BYTES / PACKET KIND UNKNOWN", r->length);
        ESP_RETURN_ON_ERROR(cerberus_display_draw_text(16, y + 27U, line, UI_MUTED, UI_PANEL, 1), TAG, "JSON row scope");
        s->painted[row] = *r; s->painted_indexes[row] = (uint8_t)index; ++s->painted_count;
    }
    if (!s->document.count)
        ESP_RETURN_ON_ERROR(draw_centered(222, "EMPTY JSON ARRAY / NO RECORDS", UI_TEXT, 1), TAG, "JSON empty");
    snprintf(line, sizeof(line), "%u/64 RECORDS / %u/8192 B / PAGE %u/%u",
        s->document.count, s->document.payload_used, s->page + 1U, pages);
    ESP_RETURN_ON_ERROR(draw_centered(354, line, UI_MUTED, 1), TAG, "JSON capacity");
    ESP_RETURN_ON_ERROR(draw_button(8, 374, 148, 44, "PREV"), TAG, "JSON prev");
    ESP_RETURN_ON_ERROR(draw_button(164, 374, 148, 44, "NEXT"), TAG, "JSON next");
    ESP_RETURN_ON_ERROR(airtag_json_ui_footer("SAVE COPY .LOG"), TAG, "JSON footer");
    s->paint_valid = true;
    return ESP_OK;
}

static inline airtag_json_ui_action_t airtag_json_ui_tap(airtag_json_ui_store_t *s, uint16_t x, uint16_t y)
{
    if (x >= 8U && x < 104U && y >= 426U && y < 470U) {
        if (s && s->view == AIRTAG_JSON_RAW) {
            s->view = AIRTAG_JSON_LIST; airtag_json_ui_invalidate(s); return AIRTAG_JSON_REDRAW;
        }
        return AIRTAG_JSON_BACK;
    }
    if (!s || s->job_pending) return AIRTAG_JSON_NONE;
    if (s->view == AIRTAG_JSON_CONFIRM && x >= 112U && x < 312U && y >= 426U && y < 470U)
        return AIRTAG_JSON_EXPORT;
    if (!s->has_document) return AIRTAG_JSON_NONE;
    if (s->view == AIRTAG_JSON_RAW && s->selected_valid &&
        x >= 112U && x < 312U && y >= 426U && y < 470U) return AIRTAG_JSON_FIND_LIVE;
    if (s->view == AIRTAG_JSON_ERROR && x >= 48U && x < 272U && y >= 374U && y < 418U) {
        s->view = AIRTAG_JSON_LIST; return AIRTAG_JSON_REDRAW;
    }
    if (s->view == AIRTAG_JSON_LIST) {
        if (x >= 112U && x < 312U && y >= 426U && y < 470U) return AIRTAG_JSON_SAVE;
        if (y >= 374U && y < 418U && ((x >= 8U && x < 156U) || (x >= 164U && x < 312U))) {
            if (x < 156U) { if (s->page) --s->page; }
            else if ((s->page + 1U) * 4U < s->document.count) ++s->page;
            airtag_json_ui_invalidate(s); return AIRTAG_JSON_REDRAW;
        }
        if (s->paint_valid && x >= 8U && x < 312U && y >= 148U && y < 348U) {
            const unsigned row = (y - 148U) / 50U;
            if ((y - 148U) % 50U >= 46U || row >= s->painted_count) return AIRTAG_JSON_NONE;
            s->selected = s->painted[row]; s->selected_index = s->painted_indexes[row];
            s->selected_valid = true; s->paint_valid = false; s->raw_page = 0U;
            s->view = AIRTAG_JSON_RAW; return AIRTAG_JSON_REDRAW;
        }
    } else if (s->view == AIRTAG_JSON_RAW && s->selected_valid && y >= 374U && y < 418U) {
        if (x >= 8U && x < 156U) { if (s->raw_page) --s->raw_page; }
        else if (x >= 164U && x < 312U) {
            if ((s->raw_page + 1U) * 128U < s->selected.length) ++s->raw_page;
        } else return AIRTAG_JSON_NONE;
        return AIRTAG_JSON_REDRAW;
    }
    return AIRTAG_JSON_NONE;
}
