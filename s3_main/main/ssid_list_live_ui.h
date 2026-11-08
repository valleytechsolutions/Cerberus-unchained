#pragma once

#include "ssid_list_ui.h"

static ssid_list_ui_store_t *ssid_lists_view;
static esp_err_t ssid_lists_open_error;
static ui_page_t ssid_lists_return_page = UI_PAGE_TOOLS;
static ui_page_t ssid_lists_parent_page = UI_PAGE_MAIN;
static bool ssid_lists_file_picker;
static bool ssid_lists_return_to_dual;

static esp_err_t ssid_lists_ensure(void)
{
    if (ssid_lists_view) return ESP_OK;
    ssid_lists_view = heap_caps_calloc(1U, sizeof(*ssid_lists_view),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ssid_lists_open_error = ssid_lists_view ? ESP_OK : ESP_ERR_NO_MEM;
    if (ssid_lists_view) ssid_list_ui_init(ssid_lists_view);
    return ssid_lists_open_error;
}

static void ssid_lists_open(void)
{
    if (ssid_lists_view && ssid_lists_view->closing) return;
    ssid_lists_return_page = current_page;
    ssid_lists_parent_page = tool_parent_page;
    ssid_lists_return_to_dual = dual_wifi_return_to_view;
    ssid_lists_file_picker = false;
    (void)ssid_lists_ensure();
    current_page = UI_PAGE_SSID_LIST;
    ESP_ERROR_CHECK(draw_detail_page());
}

static esp_err_t ssid_lists_draw(void)
{
    if (ssid_lists_ensure() == ESP_OK) return ssid_list_ui_draw(ssid_lists_view);
    ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(0, 0, 320, 480, UI_BACKGROUND), TAG, "SSID clear");
    ESP_RETURN_ON_ERROR(draw_header("SSID LISTS"), TAG, "SSID header");
    ESP_RETURN_ON_ERROR(draw_centered(170, "NO MEMORY / RETURN AND RETRY", UI_BAD, 1), TAG, "SSID unavailable");
    return draw_button(8, 426, 304, 44, "BACK");
}

static void ssid_lists_action(ssid_list_ui_action_t action)
{
    if (action == SSID_UI_EXIT) {
        heap_caps_free(ssid_lists_view); ssid_lists_view = NULL;
        ssid_lists_file_picker = false;
        /* Hidden/sleep cleanup must not redirect a different screen. */
        if (current_page != UI_PAGE_SSID_LIST) return;
        current_page = ssid_lists_return_page;
        tool_parent_page = ssid_lists_parent_page;
        dual_wifi_return_to_view = ssid_lists_return_to_dual;
        if (current_page == UI_PAGE_WIFI && wifi_mode == UI_WIFI_MODE_DUAL)
            dual_wifi_live_open(false);
        ESP_ERROR_CHECK(draw_current_page());
    } else if (action == SSID_UI_OPEN_FILES) {
        ssid_lists_file_picker = true;
        tool_parent_page = UI_PAGE_SSID_LIST;
        file_mode = UI_FILE_MODE_LIST;
        file_page_offset = 0U;
        file_listing_checked = false;
        snprintf(file_path, sizeof(file_path), "%s", CERBERUS_STORAGE_WIFI_ARCHIVE_PATH);
        current_page = UI_PAGE_FILES;
        ESP_ERROR_CHECK(draw_detail_page());
    } else if (action == SSID_UI_REDRAW && current_page == UI_PAGE_SSID_LIST) {
        ESP_ERROR_CHECK(draw_detail_page());
    }
}

static void ssid_lists_tap(uint16_t x, uint16_t y)
{
    if (!ssid_lists_view) {
        if (x >= 8U && x < 312U && y >= 426U && y < 470U) ssid_lists_action(SSID_UI_EXIT);
        return;
    }
    ssid_lists_action(ssid_list_ui_tap(ssid_lists_view, x, y));
}

static void ssid_lists_tick(bool asleep)
{
    if (!ssid_lists_view) return;
    if (ssid_lists_file_picker && !asleep && !ssid_lists_view->closing) {
        if (current_page == UI_PAGE_FILES) {
            /* A rejected start can still carry an owned worker ID. Drain it
             * while the picker remains open without closing the workspace. */
            (void)cerberus_ssid_list_session_poll(&ssid_lists_view->status);
            return;
        }
        ssid_lists_file_picker = false;
    }
    ssid_list_ui_action_t action = ssid_list_ui_tick(ssid_lists_view,
        current_page == UI_PAGE_SSID_LIST, asleep);
    /* Wake shows the list again; a sleep cleanup never navigates. */
    if (asleep && action == SSID_UI_EXIT) {
        heap_caps_free(ssid_lists_view); ssid_lists_view = NULL;
        ssid_lists_file_picker = false;
        return;
    }
    ssid_lists_action(action);
}

static bool ssid_lists_pick_file(const char *path, const char *name)
{
    if (!ssid_lists_file_picker) return false;
    if (!wifi_interchange_can_open(name, false)) {
        file_set_action_status("SELECT AN SSID .TXT OR .LOG FILE", false);
        ESP_ERROR_CHECK(draw_detail_page());
        return true;
    }
    esp_err_t error = cerberus_ssid_list_session_load(path, name);
    if (error != ESP_OK) {
        file_set_action_status(esp_err_to_name(error), false);
        ESP_ERROR_CHECK(draw_detail_page());
        return true;
    }
    ssid_list_ui_import(ssid_lists_view);
    ssid_lists_file_picker = false;
    current_page = UI_PAGE_SSID_LIST;
    ESP_ERROR_CHECK(draw_detail_page());
    return true;
}

static esp_err_t ssid_lists_import_document(const cerberus_wifi_marauder_ssid_t *document,
    ui_page_t return_page)
{
    if (ssid_lists_view && ssid_lists_view->closing) return ESP_ERR_INVALID_STATE;
    esp_err_t error = ssid_lists_ensure();
    if (error != ESP_OK) return error;
    error = cerberus_ssid_list_session_prepare_document(document);
    if (error != ESP_OK) return error;
    if (!ssid_lists_file_picker) {
        ssid_lists_return_page = return_page;
        ssid_lists_parent_page = tool_parent_page;
        ssid_lists_return_to_dual = dual_wifi_return_to_view;
    }
    ssid_lists_file_picker = false;
    ssid_list_ui_import(ssid_lists_view);
    current_page = UI_PAGE_SSID_LIST;
    return ESP_OK;
}

static esp_err_t ssid_lists_import_name(const uint8_t *bytes, size_t length, ui_page_t return_page)
{
    if (ssid_lists_view && ssid_lists_view->closing) return ESP_ERR_INVALID_STATE;
    if ((!bytes && length) || length > CERBERUS_WIFI_MARAUDER_SSID_CAPACITY) return ESP_ERR_INVALID_ARG;
    esp_err_t error = ssid_lists_ensure();
    if (error != ESP_OK) return error;
    /* The service copies this draft before the view refreshes its preview. */
    cerberus_wifi_marauder_ssid_t *document = &ssid_lists_view->preview;
    memset(document, 0, sizeof(*document));
    document->count = 1U; document->final_lf = true;
    document->rows[0].length = (uint8_t)length;
    if (length) memcpy(document->rows[0].bytes, bytes, length);
    return ssid_lists_import_document(document, return_page);
}
