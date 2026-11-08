#pragma once

#include "keyboard.h"
#include "ssid_list_session.h"
#include "esp_random.h"

/* The view owns copies and drafts. The session owns the working list and SD
 * job, so leaving this page cannot turn imported names into radio targets. */
typedef enum { SSID_UI_NONE, SSID_UI_REDRAW, SSID_UI_EXIT, SSID_UI_OPEN_FILES } ssid_list_ui_action_t;
typedef enum { SSID_UI_LIST, SSID_UI_DETAIL, SSID_UI_TEXT, SSID_UI_HEX,
    SSID_UI_OPTIONS, SSID_UI_CONFIRM, SSID_UI_IMPORT } ssid_list_ui_page_t;
typedef enum { SSID_UI_REMOVE_ONE, SSID_UI_REMOVE_SELECTED, SSID_UI_CLEAR,
    SSID_UI_REPLACE } ssid_list_ui_confirm_t;
typedef struct {
    cerberus_ssid_list_t model;
    cerberus_wifi_marauder_ssid_t preview;
    cerberus_ssid_list_session_status_t status;
    cerberus_keyboard_t keyboard;
    cerberus_wifi_marauder_ssid_row_t detail;
    uint64_t painted_revision, edit_revision, preview_id, painted_preview_id, confirm_preview_id;
    uint8_t page, index, painted_count, painted[4], generate_count;
    uint8_t draft[32], draft_length;
    char text[33], hex[65], message[49];
    uint8_t hex_length;
    ssid_list_ui_page_t view, confirm_return;
    ssid_list_ui_confirm_t confirm;
    bool painted_valid, adding, closing, append_unique, painted_privacy;
    esp_err_t error;
} ssid_list_ui_store_t;

static bool ssid_list_ui_inside(uint16_t x, uint16_t y, uint16_t left,
    uint16_t top, uint16_t width, uint16_t height)
{ return x >= left && x < left + width && y >= top && y < top + height; }

static void ssid_list_ui_message(ssid_list_ui_store_t *s, const char *text)
{ snprintf(s->message, sizeof(s->message), "%s", text); }

static bool ssid_list_ui_printable(const uint8_t *bytes, uint8_t length)
{
    for (uint8_t i = 0; i < length; ++i)
        if (bytes[i] < 32U || bytes[i] > 126U) return false;
    return true;
}

static void ssid_list_ui_name(char text[33], const cerberus_wifi_marauder_ssid_row_t *row)
{
    if (privacy_addresses_masked()) { snprintf(text, 33, "[NAME HIDDEN]"); return; }
    if (!row->length) { snprintf(text, 33, "[EMPTY NAME]"); return; }
    for (uint8_t i = 0; i < row->length; ++i)
        text[i] = row->bytes[i] >= 32U && row->bytes[i] <= 126U ? (char)row->bytes[i] : '.';
    text[row->length] = 0;
}

static uint8_t ssid_list_ui_selected(const cerberus_ssid_list_t *model)
{
    uint8_t count = 0;
    for (uint8_t i = 0; i < model->document.count; ++i)
        if (model->selected & (UINT64_C(1) << i)) ++count;
    return count;
}

static esp_err_t ssid_list_ui_bytes(uint16_t y, const uint8_t *bytes, uint8_t length)
{
    if (privacy_addresses_masked()) return draw_centered(y, "[NAME BYTES HIDDEN]", UI_MUTED, 1);
    if (!length) return draw_centered(y, "[EMPTY / 0 BYTES]", UI_MUTED, 1);
    for (uint8_t start = 0; start < length; start += 8U) {
        char line[25]; size_t used = 0;
        for (uint8_t i = start; i < length && i < start + 8U; ++i)
            used += (size_t)snprintf(line + used, sizeof(line) - used, "%02X%s", bytes[i],
                i + 1U < length && i + 1U < start + 8U ? " " : "");
        ESP_RETURN_ON_ERROR(draw_centered(y, line, UI_TEXT, 1), TAG, "SSID bytes");
        y += 18U;
    }
    return ESP_OK;
}

static void ssid_list_ui_encode_hex(ssid_list_ui_store_t *s)
{
    static const char digits[] = "0123456789ABCDEF";
    for (uint8_t i = 0; i < s->draft_length; ++i) {
        s->hex[i * 2U] = digits[s->draft[i] >> 4];
        s->hex[i * 2U + 1U] = digits[s->draft[i] & 15U];
    }
    s->hex_length = s->draft_length * 2U; s->hex[s->hex_length] = 0;
}

static bool ssid_list_ui_decode_hex(ssid_list_ui_store_t *s)
{
    static const char digits[] = "0123456789ABCDEF";
    if (s->hex_length & 1U) { ssid_list_ui_message(s, "FINISH THE BYTE / TWO HEX DIGITS"); return false; }
    uint8_t bytes[32] = {0};
    for (uint8_t i = 0; i < s->hex_length; i += 2U) {
        const char *a = strchr(digits, s->hex[i]);
        const char *b = strchr(digits, s->hex[i + 1U]);
        if (!a || !b) return false;
        bytes[i / 2U] = (uint8_t)((a - digits) * 16 + b - digits);
    }
    memcpy(s->draft, bytes, sizeof(bytes)); s->draft_length = s->hex_length / 2U;
    return true;
}

static bool ssid_list_ui_text_begin(ssid_list_ui_store_t *s)
{
    if (!ssid_list_ui_printable(s->draft, s->draft_length)) {
        ssid_list_ui_message(s, "BINARY NAME / USE HEX TO KEEP ITS BYTES"); return false;
    }
    char initial[33] = {0}; memcpy(initial, s->draft, s->draft_length);
    s->error = cerberus_keyboard_begin(&s->keyboard, s->text, sizeof(s->text), initial);
    if (s->error != ESP_OK) return false;
    s->view = SSID_UI_TEXT; return true;
}

static void ssid_list_ui_begin_edit(ssid_list_ui_store_t *s, bool adding, bool hex)
{
    if (privacy_addresses_masked()) {
        ssid_list_ui_message(s, "TURN OFF PRIVACY TO EDIT NAME BYTES"); return;
    }
    memset(s->draft, 0, sizeof(s->draft)); s->draft_length = 0;
    s->adding = adding; s->edit_revision = s->painted_revision;
    if (!adding) {
        const cerberus_wifi_marauder_ssid_row_t *row = &s->model.document.rows[s->index];
        memcpy(s->draft, row->bytes, row->length); s->draft_length = row->length;
    }
    s->message[0] = 0;
    if (hex || !ssid_list_ui_text_begin(s)) { ssid_list_ui_encode_hex(s); s->view = SSID_UI_HEX; }
    s->painted_valid = false;
}

static esp_err_t ssid_list_ui_refresh(ssid_list_ui_store_t *s)
{
    esp_err_t error = cerberus_ssid_list_session_poll(&s->status);
    if (error != ESP_OK) return error;
    error = cerberus_ssid_list_session_copy(&s->model);
    if (error != ESP_OK) return error;
    if (s->view == SSID_UI_DETAIL && (s->index >= s->model.document.count ||
        memcmp(&s->detail, &s->model.document.rows[s->index], sizeof(s->detail)))) {
        s->view = SSID_UI_LIST; ssid_list_ui_message(s, "LIST CHANGED / SELECT THE NAME AGAIN");
    }
    if (s->status.preview_ready) {
        error = cerberus_ssid_list_session_copy_preview(&s->preview, &s->preview_id);
        if (error != ESP_OK) return error;
        if (s->view == SSID_UI_LIST) { s->view = SSID_UI_IMPORT; s->page = 0; }
    } else if (s->view == SSID_UI_IMPORT ||
        (s->view == SSID_UI_CONFIRM && s->confirm == SSID_UI_REPLACE)) {
        memset(&s->preview,0,sizeof(s->preview)); s->preview_id = 0;
        s->view = SSID_UI_LIST; s->page = 0;
        ssid_list_ui_message(s,"IMPORT NO LONGER AVAILABLE / LIST RETAINED");
    }
    if (s->page && s->page * 4U >= (s->view == SSID_UI_IMPORT ?
        s->preview.count : s->model.document.count)) s->page = 0;
    return ESP_OK;
}

static void ssid_list_ui_init(ssid_list_ui_store_t *s)
{
    memset(s, 0, sizeof(*s)); s->generate_count = 1;
    s->error = cerberus_ssid_list_session_open();
    if (s->error == ESP_OK) s->error = ssid_list_ui_refresh(s);
}

static void ssid_list_ui_import(ssid_list_ui_store_t *s)
{
    s->painted_valid = false; s->page = 0; s->view = SSID_UI_LIST;
    s->message[0] = 0; s->error = ssid_list_ui_refresh(s);
}

static void ssid_list_ui_result(ssid_list_ui_store_t *s, esp_err_t error)
{
    s->error = error; s->painted_valid = false;
    if (error == ESP_OK) s->message[0] = 0;
    else if (error == ESP_ERR_INVALID_STATE) ssid_list_ui_message(s, "LIST CHANGED OR BUSY / REVIEW AND RETRY");
    else if (error == ESP_ERR_INVALID_SIZE) ssid_list_ui_message(s, "LIST FULL / LIMIT IS 64 NAMES, 32 BYTES");
    else if (error == ESP_ERR_NOT_SUPPORTED) ssid_list_ui_message(s, "LF / 0A CANNOT BE SAVED IN SSID TEXT");
    else ssid_list_ui_message(s, "OPERATION FAILED / WORKING LIST RETAINED");
}

static void ssid_list_ui_edit(ssid_list_ui_store_t *s, cerberus_ssid_list_edit_kind_t kind,
    uint8_t index, bool selected)
{
    const cerberus_ssid_list_edit_t command = {.kind=kind,.index=index,.selected=selected};
    ssid_list_ui_result(s, cerberus_ssid_list_session_edit(s->painted_revision, &command));
}

static void ssid_list_ui_submit(ssid_list_ui_store_t *s)
{
    for (uint8_t i = 0; i < s->draft_length; ++i) if (s->draft[i] == '\n') {
        ssid_list_ui_message(s, "LF / 0A CANNOT BE SAVED IN SSID TEXT"); return;
    }
    const cerberus_ssid_list_edit_t command = {.kind=s->adding ? CERBERUS_SSID_EDIT_ADD :
        CERBERUS_SSID_EDIT_NAME,.index=s->index,.bytes=s->draft,.length=s->draft_length};
    ssid_list_ui_result(s, cerberus_ssid_list_session_edit(s->edit_revision, &command));
    if (s->error == ESP_OK) { s->view = SSID_UI_LIST; s->page = 0; }
}

static void ssid_list_ui_confirm(ssid_list_ui_store_t *s, ssid_list_ui_confirm_t operation)
{
    s->confirm = operation; s->confirm_return = s->view;
    s->confirm_preview_id = s->painted_preview_id;
    s->edit_revision = s->painted_revision; s->view = SSID_UI_CONFIRM; s->painted_valid = false;
}

static ssid_list_ui_action_t ssid_list_ui_back(ssid_list_ui_store_t *s)
{
    if (s->closing) return SSID_UI_NONE;
    if (s->view == SSID_UI_TEXT || s->view == SSID_UI_HEX) {
        s->view = s->adding ? SSID_UI_LIST : SSID_UI_DETAIL;
        memset(s->draft, 0, sizeof(s->draft)); memset(s->text, 0, sizeof(s->text));
        memset(s->hex, 0, sizeof(s->hex)); s->message[0] = 0;
    } else if (s->view == SSID_UI_CONFIRM) s->view = s->confirm_return;
    else if (s->view == SSID_UI_IMPORT) {
        cerberus_ssid_list_session_discard_preview(); s->preview_id = 0;
        s->view = SSID_UI_LIST; s->page = 0;
    } else if (s->view != SSID_UI_LIST) { s->view = SSID_UI_LIST; s->page = 0; }
    else {
        s->closing = true; cerberus_ssid_list_session_close();
    }
    s->painted_valid = false; return SSID_UI_REDRAW;
}

static ssid_list_ui_action_t ssid_list_ui_tick(ssid_list_ui_store_t *s, bool visible, bool asleep)
{
    if ((!visible || asleep) && !s->closing) {
        s->closing = true; s->painted_valid = false;
        memset(s->draft, 0, sizeof(s->draft)); memset(s->text, 0, sizeof(s->text));
        memset(s->hex, 0, sizeof(s->hex)); cerberus_ssid_list_session_close();
    }
    cerberus_ssid_list_session_status_t previous = s->status;
    esp_err_t error = cerberus_ssid_list_session_poll(&s->status);
    if (error != ESP_OK) { s->error = error; return SSID_UI_NONE; }
    if (s->closing) return !s->status.busy ? SSID_UI_EXIT : SSID_UI_NONE;
    if (privacy_addresses_masked() && (s->view == SSID_UI_TEXT || s->view == SSID_UI_HEX)) {
        (void)ssid_list_ui_back(s); ssid_list_ui_message(s, "EDIT CANCELED / PRIVACY IS ON"); return SSID_UI_REDRAW;
    }
    if (s->status.preview_ready && s->view == SSID_UI_LIST) {
        ssid_list_ui_import(s); return SSID_UI_REDRAW;
    }
    return memcmp(&previous, &s->status, sizeof(previous)) ? SSID_UI_REDRAW : SSID_UI_NONE;
}

static esp_err_t ssid_list_ui_footer(const ssid_list_ui_store_t *s)
{
    const char *back = s->closing ? "WAIT" : s->view == SSID_UI_TEXT || s->view == SSID_UI_HEX ?
        "CANCEL" : "BACK";
    ESP_RETURN_ON_ERROR(draw_small_button(8, 426, 96, 44, back), TAG, "SSID back");
    if (s->closing) return ESP_OK;
    if (s->view == SSID_UI_LIST)
        return draw_small_button(112, 426, 200, 44, "LIST OPTIONS");
    if (s->view == SSID_UI_TEXT) {
        ESP_RETURN_ON_ERROR(draw_small_button(112, 426, 96, 44, "HEX"), TAG, "SSID hex mode");
        return draw_small_button(216, 426, 96, 44, "USE EMPTY");
    }
    if (s->view == SSID_UI_HEX) return draw_small_button(112, 426, 200, 44, "USE NAME");
    return ESP_OK;
}

static esp_err_t ssid_list_ui_draw_rows(ssid_list_ui_store_t *s, bool preview)
{
    const cerberus_wifi_marauder_ssid_t *document = preview ? &s->preview : &s->model.document;
    char line[54], name[33];
    if (!document->count) {
        ESP_RETURN_ON_ERROR(draw_centered(166, preview ? "EMPTY IMPORT / 0 NAMES" : "NO NAMES YET", UI_MUTED, 2), TAG, "SSID empty");
        ESP_RETURN_ON_ERROR(draw_centered(195, "ADD A NAME OR OPEN AN SSID TEXT FILE", UI_MUTED, 1), TAG, "SSID empty hint");
    }
    for (uint8_t slot = 0; slot < 4U; ++slot) {
        uint8_t index = (uint8_t)(s->page * 4U + slot);
        if (index >= document->count) break;
        uint16_t y = (uint16_t)(108U + slot * 48U);
        ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(8, y, 304, 44, UI_PANEL), TAG, "SSID row");
        ssid_list_ui_name(name, &document->rows[index]);
        ESP_RETURN_ON_ERROR(cerberus_display_draw_text(52, y + 6U, name, UI_TEXT, UI_PANEL, 1), TAG, "SSID name");
        snprintf(line, sizeof(line), "#%u / %u BYTES", index + 1U, document->rows[index].length);
        ESP_RETURN_ON_ERROR(cerberus_display_draw_text(52, y + 25U, line, UI_MUTED, UI_PANEL, 1), TAG, "SSID row length");
        ESP_RETURN_ON_ERROR(cerberus_display_draw_text(16, y + 16U, preview ? ">" :
            s->model.selected & (UINT64_C(1) << index) ? "[X]" : "[ ]", UI_GOOD, UI_PANEL, 1), TAG, "SSID select");
        s->painted[s->painted_count++] = index;
    }
    snprintf(line, sizeof(line), "PAGE %u / %u", s->page + 1U,
        document->count ? (document->count + 3U) / 4U : 1U);
    ESP_RETURN_ON_ERROR(draw_small_button(8, 304, 80, 36, "PREV"), TAG, "SSID prev");
    ESP_RETURN_ON_ERROR(draw_centered(317, line, UI_MUTED, 1), TAG, "SSID page");
    return draw_small_button(232, 304, 80, 36, "NEXT");
}

static esp_err_t ssid_list_ui_draw_body(ssid_list_ui_store_t *s)
{
    char line[54];
    if (s->closing) {
        ESP_RETURN_ON_ERROR(draw_centered(150,"WAITING FOR SD WORKER",UI_TEXT,1),TAG,"SSID closing");
        ESP_RETURN_ON_ERROR(draw_centered(186,"IN-FLIGHT SAVE MAY STILL COMPLETE",UI_MUTED,1),TAG,"SSID save closing");
        ESP_RETURN_ON_ERROR(draw_centered(218,"WORKING LIST STAYS IN MEMORY",UI_MUTED,1),TAG,"SSID retained");
        return ssid_list_ui_footer(s);
    }
    if (s->view == SSID_UI_TEXT) {
        ESP_RETURN_ON_ERROR(cerberus_keyboard_draw(&s->keyboard, "SSID / BYTES"), TAG, "SSID text keyboard");
        if (s->message[0]) {
            ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(0,396,320,20,UI_BACKGROUND),TAG,"SSID text note background");
            ESP_RETURN_ON_ERROR(draw_centered(402,s->message,s->error ? UI_BAD : UI_MUTED,1),TAG,"SSID text note");
        }
        return ssid_list_ui_footer(s);
    }
    if (s->view == SSID_UI_HEX) {
        snprintf(line, sizeof(line), "%u / 64 HEX DIGITS / TWO PER BYTE", s->hex_length);
        ESP_RETURN_ON_ERROR(draw_centered(56, line, UI_MUTED, 1), TAG, "SSID hex length");
        for (unsigned row = 0; row < 4U; ++row) {
            size_t length = s->hex_length > row * 16U ? s->hex_length - row * 16U : 0U;
            if (length > 16U) length = 16U;
            memcpy(line, s->hex + row * 16U, length); line[length] = 0;
            ESP_RETURN_ON_ERROR(draw_centered((uint16_t)(88U + row * 22U), line, UI_TEXT, 2), TAG, "SSID hex draft");
        }
        for (uint8_t key = 0; key < 16U; ++key) {
            char label[2] = {"0123456789ABCDEF"[key],0};
            ESP_RETURN_ON_ERROR(draw_small_button((uint16_t)(12U + key % 4U * 76U),
                (uint16_t)(184U + key / 4U * 40U), 68, 34, label), TAG, "SSID hex key");
        }
        ESP_RETURN_ON_ERROR(draw_small_button(8, 350, 96, 38, "DEL"), TAG, "SSID hex delete");
        ESP_RETURN_ON_ERROR(draw_small_button(112, 350, 96, 38, "CLEAR"), TAG, "SSID hex clear");
        ESP_RETURN_ON_ERROR(draw_small_button(216, 350, 96, 38, "TEXT"), TAG, "SSID text mode");
    } else if (s->view == SSID_UI_LIST || s->view == SSID_UI_IMPORT) {
        bool importing = s->view == SSID_UI_IMPORT;
        snprintf(line, sizeof(line), importing ? "IMPORT %u NAMES / WORKING LIST %u" :
            "%u / 64 NAMES / %u SELECTED", importing ? s->preview.count : s->model.document.count,
            importing ? s->model.document.count : ssid_list_ui_selected(&s->model));
        ESP_RETURN_ON_ERROR(draw_centered(56, line, UI_TEXT, 1), TAG, "SSID count");
        ESP_RETURN_ON_ERROR(draw_centered(78, importing ? "PREVIEW ONLY / CHOOSE HOW TO IMPORT" :
            s->model.dirty ? "UNSAVED CHANGES / NAMES ONLY, NO AP TARGETS" :
                "NAMES ONLY / NO AP, SIGNAL OR RADIO STATE", UI_MUTED, 1), TAG, "SSID scope");
        ESP_RETURN_ON_ERROR(ssid_list_ui_draw_rows(s, importing), TAG, "SSID rows");
        if (importing) {
            ESP_RETURN_ON_ERROR(draw_small_button(8, 350, 144, 40, "REPLACE LIST"), TAG, "SSID replace");
            ESP_RETURN_ON_ERROR(draw_small_button(160, 350, 152, 40, "APPEND"), TAG, "SSID append");
            ESP_RETURN_ON_ERROR(draw_small_button(112, 426, 200, 44,
                s->append_unique ? "APPEND: UNIQUE" : "APPEND: KEEP ALL"), TAG, "SSID duplicate choice");
        } else {
            ESP_RETURN_ON_ERROR(draw_small_button(8, 350, 96, 40, "ADD"), TAG, "SSID add");
            ESP_RETURN_ON_ERROR(draw_small_button(112, 350, 96, 40, "OPEN FILE"), TAG, "SSID file");
            ESP_RETURN_ON_ERROR(draw_small_button(216, 350, 96, 40, "SAVE COPY"), TAG, "SSID save");
        }
    } else if (s->view == SSID_UI_DETAIL) {
        if (s->index >= s->model.document.count) return ESP_ERR_INVALID_STATE;
        const cerberus_wifi_marauder_ssid_row_t *row = &s->model.document.rows[s->index];
        char name[33]; ssid_list_ui_name(name, row);
        ESP_RETURN_ON_ERROR(draw_centered(70, name, UI_TEXT, 1), TAG, "SSID selected name");
        snprintf(line, sizeof(line), "NAME %u / %u BYTES / EXACT HEX", s->index + 1U, row->length);
        ESP_RETURN_ON_ERROR(draw_centered(102, line, UI_MUTED, 1), TAG, "SSID selected length");
        ESP_RETURN_ON_ERROR(ssid_list_ui_bytes(134, row->bytes, row->length), TAG, "SSID selected bytes");
        ESP_RETURN_ON_ERROR(draw_centered(224, "EDITED NAMES DO NOT CHANGE SCAN RESULTS", UI_MUTED, 1), TAG, "SSID edit scope");
        ESP_RETURN_ON_ERROR(draw_small_button(8, 258, 144, 44, "EDIT TEXT"), TAG, "SSID text edit");
        ESP_RETURN_ON_ERROR(draw_small_button(160, 258, 152, 44, "EDIT HEX"), TAG, "SSID hex edit");
        ESP_RETURN_ON_ERROR(draw_small_button(8, 318, 144, 44,
            s->model.selected & (UINT64_C(1) << s->index) ? "DESELECT" : "SELECT"), TAG, "SSID selected");
        ESP_RETURN_ON_ERROR(draw_small_button(160, 318, 152, 44, "REMOVE"), TAG, "SSID remove");
    } else if (s->view == SSID_UI_OPTIONS) {
        snprintf(line, sizeof(line), "%u NAMES / %u SELECTED", s->model.document.count, ssid_list_ui_selected(&s->model));
        ESP_RETURN_ON_ERROR(draw_centered(62, line, UI_TEXT, 1), TAG, "SSID options count");
        ESP_RETURN_ON_ERROR(draw_small_button(8, 98, 144, 44, "SELECT ALL"), TAG, "SSID all");
        ESP_RETURN_ON_ERROR(draw_small_button(160, 98, 152, 44, "SELECT NONE"), TAG, "SSID none");
        ESP_RETURN_ON_ERROR(draw_small_button(8, 158, 304, 44, "REMOVE SELECTED"), TAG, "SSID remove selected");
        ESP_RETURN_ON_ERROR(draw_small_button(8, 218, 304, 44, "CLEAR WORKING LIST"), TAG, "SSID clear list");
        snprintf(line, sizeof(line), "GENERATE %u / CERB-XXXXXXXX", s->generate_count);
        ESP_RETURN_ON_ERROR(draw_centered(286, line, UI_MUTED, 1), TAG, "SSID generator");
        ESP_RETURN_ON_ERROR(draw_small_button(8, 312, 64, 44, "-"), TAG, "SSID generate less");
        ESP_RETURN_ON_ERROR(draw_small_button(80, 312, 160, 44, "ADD GENERATED"), TAG, "SSID generate add");
        ESP_RETURN_ON_ERROR(draw_small_button(248, 312, 64, 44, "+"), TAG, "SSID generate more");
    } else if (s->view == SSID_UI_CONFIRM) {
        const char *title = s->confirm == SSID_UI_REMOVE_ONE ? "REMOVE THIS NAME?" :
            s->confirm == SSID_UI_REMOVE_SELECTED ? "REMOVE SELECTED NAMES?" :
            s->confirm == SSID_UI_CLEAR ? "CLEAR WORKING LIST?" : "REPLACE WORKING LIST?";
        ESP_RETURN_ON_ERROR(draw_centered(120, title, UI_TEXT, 1), TAG, "SSID confirm title");
        ESP_RETURN_ON_ERROR(draw_centered(160, "SAVED SD FILES ARE UNCHANGED", UI_MUTED, 1), TAG, "SSID confirm SD");
        ESP_RETURN_ON_ERROR(draw_centered(184, "UNSAVED EDITS MAY BE LOST", UI_MUTED, 1), TAG, "SSID confirm dirty");
        ESP_RETURN_ON_ERROR(draw_small_button(32, 254, 256, 52, "CONFIRM"), TAG, "SSID confirm");
    }
    const char *note = s->message;
    if (!note[0] && s->status.state == CERBERUS_SSID_SESSION_ERROR) {
        snprintf(line,sizeof(line),"SD: %.44s",esp_err_to_name(s->status.error)); note = line;
    }
    if (!note[0] && s->status.busy) note = "SD JOB ACTIVE / LIST REMAINS AVAILABLE";
    if (note[0]) ESP_RETURN_ON_ERROR(draw_centered(402, note,
        s->error || s->status.error ? UI_BAD : UI_MUTED, 1), TAG, "SSID note");
    else if (s->status.file_name[0] && s->status.state == CERBERUS_SSID_SESSION_COMPLETE) {
        snprintf(line, sizeof(line), "SAVED %.40s", s->status.file_name);
        ESP_RETURN_ON_ERROR(draw_centered(402, line, UI_GOOD, 1), TAG, "SSID saved");
    }
    return ssid_list_ui_footer(s);
}

static esp_err_t ssid_list_ui_draw(ssid_list_ui_store_t *s)
{
    s->painted_valid = false; s->painted_count = 0;
    esp_err_t error = ssid_list_ui_refresh(s);
    if (privacy_addresses_masked() && (s->view == SSID_UI_TEXT || s->view == SSID_UI_HEX))
        (void)ssid_list_ui_back(s);
    ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(0, 0, 320, 480, UI_BACKGROUND), TAG, "SSID background");
    ESP_RETURN_ON_ERROR(draw_header(s->view == SSID_UI_TEXT ? "SSID TEXT" :
        s->view == SSID_UI_HEX ? "SSID HEX" : "SSID LISTS"), TAG, "SSID header");
    if (error != ESP_OK) {
        s->error = error;
        ESP_RETURN_ON_ERROR(draw_centered(160, "WORKSPACE UNAVAILABLE", UI_BAD, 1), TAG, "SSID unavailable");
        ESP_RETURN_ON_ERROR(draw_centered(192, esp_err_to_name(error), UI_BAD, 1), TAG, "SSID error");
        return ssid_list_ui_footer(s);
    }
    ESP_RETURN_ON_ERROR(ssid_list_ui_draw_body(s), TAG, "SSID body");
    s->painted_revision = s->model.revision; s->painted_preview_id = s->preview_id;
    s->painted_privacy = privacy_addresses_masked(); s->painted_valid = true;
    return ESP_OK;
}

static ssid_list_ui_action_t ssid_list_ui_tap(ssid_list_ui_store_t *s, uint16_t x, uint16_t y)
{
    if (ssid_list_ui_inside(x,y,8,426,96,44)) return ssid_list_ui_back(s);
    if (s->closing || !s->painted_valid) return SSID_UI_NONE;
    if (s->painted_privacy != privacy_addresses_masked()) { s->painted_valid = false; return SSID_UI_REDRAW; }
    cerberus_ssid_list_session_status_t live;
    if (cerberus_ssid_list_session_poll(&live) != ESP_OK || live.revision != s->painted_revision ||
        (s->view == SSID_UI_IMPORT && live.preview_id != s->painted_preview_id)) {
        s->painted_valid = false; ssid_list_ui_message(s,"LIST CHANGED / REVIEW AND RETRY"); return SSID_UI_REDRAW;
    }
    if (s->view == SSID_UI_TEXT) {
        if (ssid_list_ui_inside(x,y,112,426,96,44)) {
            s->draft_length = (uint8_t)s->keyboard.length; memset(s->draft,0,sizeof(s->draft));
            memcpy(s->draft,s->text,s->draft_length); ssid_list_ui_encode_hex(s); s->view = SSID_UI_HEX;
        } else if (ssid_list_ui_inside(x,y,216,426,96,44)) {
            memset(s->draft,0,sizeof(s->draft)); s->draft_length = 0; ssid_list_ui_submit(s);
        } else {
            cerberus_keyboard_action_t action;
            s->error = cerberus_keyboard_handle_tap(&s->keyboard,x,y,&action);
            if (s->error != ESP_OK || action == CERBERUS_KEYBOARD_ACTION_NONE) return SSID_UI_NONE;
            if (action == CERBERUS_KEYBOARD_ACTION_SUBMIT) {
                memset(s->draft,0,sizeof(s->draft)); s->draft_length = (uint8_t)s->keyboard.length;
                memcpy(s->draft,s->text,s->draft_length); ssid_list_ui_submit(s);
            }
        }
    } else if (s->view == SSID_UI_HEX) {
        bool handled = false;
        for (uint8_t key = 0; key < 16U; ++key) if (ssid_list_ui_inside(x,y,
            (uint16_t)(12U + key % 4U * 76U),(uint16_t)(184U + key / 4U * 40U),68,34)) {
            if (s->hex_length < 64U) { s->hex[s->hex_length++] = "0123456789ABCDEF"[key]; s->hex[s->hex_length] = 0; }
            handled = true; break;
        }
        if (!handled && ssid_list_ui_inside(x,y,8,350,96,38)) { if (s->hex_length) s->hex[--s->hex_length] = 0; }
        else if (!handled && ssid_list_ui_inside(x,y,112,350,96,38)) { s->hex_length = 0; s->hex[0] = 0; }
        else if (!handled && ssid_list_ui_inside(x,y,216,350,96,38)) { if (ssid_list_ui_decode_hex(s)) (void)ssid_list_ui_text_begin(s); }
        else if (!handled && ssid_list_ui_inside(x,y,112,426,200,44)) { if (ssid_list_ui_decode_hex(s)) ssid_list_ui_submit(s); }
        else if (!handled) return SSID_UI_NONE;
    } else if (s->view == SSID_UI_LIST || s->view == SSID_UI_IMPORT) {
        bool importing = s->view == SSID_UI_IMPORT;
        uint8_t count = importing ? s->preview.count : s->model.document.count;
        if (ssid_list_ui_inside(x,y,8,304,80,36)) { if (s->page) --s->page; }
        else if (ssid_list_ui_inside(x,y,232,304,80,36)) { if ((s->page + 1U) * 4U < count) ++s->page; }
        else if (!importing && ssid_list_ui_inside(x,y,8,350,96,40)) ssid_list_ui_begin_edit(s,true,false);
        else if (!importing && ssid_list_ui_inside(x,y,112,350,96,40)) {
            if (!s->status.busy) { s->painted_valid = false; return SSID_UI_OPEN_FILES; }
            ssid_list_ui_message(s,"WAIT FOR THE CURRENT SD JOB");
        } else if (!importing && ssid_list_ui_inside(x,y,216,350,96,40))
            ssid_list_ui_result(s,cerberus_ssid_list_session_save(s->painted_revision));
        else if (ssid_list_ui_inside(x,y,112,426,200,44)) {
            if (importing) s->append_unique = !s->append_unique;
            else s->view = SSID_UI_OPTIONS;
        } else if (importing && ssid_list_ui_inside(x,y,8,350,144,40)) ssid_list_ui_confirm(s,SSID_UI_REPLACE);
        else if (importing && ssid_list_ui_inside(x,y,160,350,152,40)) {
            ssid_list_ui_result(s,cerberus_ssid_list_session_apply_preview(s->painted_preview_id,
                s->painted_revision,true,s->append_unique));
            if (s->error == ESP_OK) { s->view = SSID_UI_LIST; s->page = 0; }
        } else if (!importing) {
            bool hit = false;
            for (uint8_t slot = 0; slot < s->painted_count; ++slot) if (ssid_list_ui_inside(x,y,8,(uint16_t)(108U+slot*48U),304,44)) {
                s->index = s->painted[slot];
                if (x < 48U) ssid_list_ui_edit(s,CERBERUS_SSID_EDIT_SELECT,s->index,
                    !(s->model.selected & (UINT64_C(1) << s->index)));
                else { s->detail = s->model.document.rows[s->index]; s->view = SSID_UI_DETAIL; }
                hit = true; break;
            }
            if (!hit) return SSID_UI_NONE;
        } else return SSID_UI_NONE;
    } else if (s->view == SSID_UI_DETAIL) {
        if (ssid_list_ui_inside(x,y,8,258,144,44)) ssid_list_ui_begin_edit(s,false,false);
        else if (ssid_list_ui_inside(x,y,160,258,152,44)) ssid_list_ui_begin_edit(s,false,true);
        else if (ssid_list_ui_inside(x,y,8,318,144,44)) ssid_list_ui_edit(s,CERBERUS_SSID_EDIT_SELECT,s->index,
            !(s->model.selected & (UINT64_C(1) << s->index)));
        else if (ssid_list_ui_inside(x,y,160,318,152,44)) ssid_list_ui_confirm(s,SSID_UI_REMOVE_ONE);
        else return SSID_UI_NONE;
    } else if (s->view == SSID_UI_OPTIONS) {
        if (ssid_list_ui_inside(x,y,8,98,144,44)) ssid_list_ui_edit(s,CERBERUS_SSID_EDIT_SELECT_ALL,0,true);
        else if (ssid_list_ui_inside(x,y,160,98,152,44)) ssid_list_ui_edit(s,CERBERUS_SSID_EDIT_SELECT_ALL,0,false);
        else if (ssid_list_ui_inside(x,y,8,158,304,44)) ssid_list_ui_confirm(s,SSID_UI_REMOVE_SELECTED);
        else if (ssid_list_ui_inside(x,y,8,218,304,44)) ssid_list_ui_confirm(s,SSID_UI_CLEAR);
        else if (ssid_list_ui_inside(x,y,8,312,64,44)) { if (s->generate_count > 1U) --s->generate_count; }
        else if (ssid_list_ui_inside(x,y,248,312,64,44)) { if (s->generate_count < 64U) ++s->generate_count; }
        else if (ssid_list_ui_inside(x,y,80,312,160,44)) {
            uint8_t entropy[64U * CERBERUS_SSID_LIST_ENTROPY_PER_NAME];
            size_t length = s->generate_count * CERBERUS_SSID_LIST_ENTROPY_PER_NAME;
            esp_fill_random(entropy,length);
            const cerberus_ssid_list_edit_t command = {.kind=CERBERUS_SSID_EDIT_GENERATE,
                .bytes=entropy,.length=length,.count=s->generate_count};
            ssid_list_ui_result(s,cerberus_ssid_list_session_edit(s->painted_revision,&command));
        } else return SSID_UI_NONE;
    } else if (s->view == SSID_UI_CONFIRM) {
        if (!ssid_list_ui_inside(x,y,32,254,256,52)) return SSID_UI_NONE;
        if (s->confirm == SSID_UI_REPLACE) ssid_list_ui_result(s,
            cerberus_ssid_list_session_apply_preview(s->confirm_preview_id,s->edit_revision,false,false));
        else {
            const cerberus_ssid_list_edit_t command = {.kind=s->confirm == SSID_UI_REMOVE_ONE ? CERBERUS_SSID_EDIT_REMOVE :
                s->confirm == SSID_UI_REMOVE_SELECTED ? CERBERUS_SSID_EDIT_REMOVE_SELECTED : CERBERUS_SSID_EDIT_CLEAR,.index=s->index};
            ssid_list_ui_result(s,cerberus_ssid_list_session_edit(s->edit_revision,&command));
        }
        if (s->error == ESP_OK) { s->view = SSID_UI_LIST; s->page = 0; }
    } else return SSID_UI_NONE;
    s->painted_valid = false; return SSID_UI_REDRAW;
}
