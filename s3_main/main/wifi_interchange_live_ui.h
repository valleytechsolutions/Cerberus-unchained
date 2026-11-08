#pragma once

/* One optional typed offline view. Conversion and SD operations remain in the
 * existing serialized worker; native .WFI data stays intact beneath exports. */
#include "wifi_interchange_ui.h"
#include "ble_archive_io.h"

static wifi_interchange_ui_store_t *wifi_interchange_view;
static bool wifi_interchange_opened, wifi_interchange_from_archive;
static esp_err_t wifi_interchange_open_error;
static ui_page_t wifi_interchange_return_page;

static bool wifi_interchange_suffix(const char *name, const char *suffix)
{
    if (!name) return false;
    size_t n = strlen(name), m = strlen(suffix);
    if (n <= m) return false;
    for (size_t i = 0; i < m; ++i) {
        char c = name[n - m + i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (c != suffix[i]) return false;
    }
    return true;
}

static bool wifi_interchange_can_open(const char *name, bool ap_json)
{ return wifi_interchange_suffix(name, ".LOG") || wifi_interchange_suffix(name, ap_json ? ".JSON" : ".TXT"); }

static bool wifi_interchange_is_open(void) { return wifi_interchange_opened; }

static void wifi_interchange_dispose(void)
{
    if (wifi_interchange_view) {
        if (wifi_interchange_view->job_pending) {
            cerberus_ble_archive_io_cancel(wifi_interchange_view->job_id);
            cerberus_ble_archive_io_release(wifi_interchange_view->job_id);
        }
        heap_caps_free(wifi_interchange_view); wifi_interchange_view = NULL;
    }
    wifi_interchange_opened = false;
}

static void wifi_interchange_close(bool redraw)
{
    wifi_interchange_dispose(); current_page = wifi_interchange_return_page;
    if (redraw) ESP_ERROR_CHECK(draw_current_page());
}

static void wifi_interchange_enter(bool from_archive)
{
    wifi_interchange_dispose(); wifi_interchange_return_page = current_page;
    wifi_interchange_from_archive = from_archive;
    if (!from_archive) wifi_archive_dispose();
    current_page = UI_PAGE_WIFI_ARCHIVE; wifi_interchange_opened = true;
    wifi_interchange_view = heap_caps_calloc(1U, sizeof(*wifi_interchange_view), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    wifi_interchange_open_error = wifi_interchange_view ? ESP_OK : ESP_ERR_NO_MEM;
}

static void wifi_interchange_accept(esp_err_t error, uint64_t job)
{
    if (error == ESP_OK && job) {
        wifi_interchange_invalidate(wifi_interchange_view);
        wifi_interchange_view->job_id = job; wifi_interchange_view->job_pending = true;
        wifi_interchange_view->write_started = wifi_interchange_view->saving;
        wifi_interchange_view->view = WIFI_INTERCHANGE_JOB; wifi_interchange_view->error = ESP_OK;
    } else wifi_interchange_error(wifi_interchange_view, error == ESP_OK ? ESP_ERR_INVALID_RESPONSE : error);
}

static void wifi_interchange_open_file(const char *path, const char *name, bool ap_json)
{
    if (!wifi_interchange_can_open(name, ap_json)) return;
    wifi_interchange_enter(false);
    if (wifi_interchange_view) {
        wifi_interchange_view->ap_json = ap_json;
        uint64_t job = 0;
        esp_err_t error = ap_json ? cerberus_ble_archive_io_load_ap_json(path, name, &job) :
            cerberus_ble_archive_io_load_ssid_text(path, name, &job);
        wifi_interchange_accept(error, job);
    }
    ESP_ERROR_CHECK(draw_detail_page());
}

static void wifi_interchange_open_export(void)
{
    if (!wifi_archive_view || !wifi_archive_view->has_document || wifi_archive_view->job_pending ||
        wifi_archive_view->release_pending) return;
    wifi_interchange_enter(true);
    if (wifi_interchange_view) wifi_interchange_view->view = WIFI_INTERCHANGE_CHOICE;
    ESP_ERROR_CHECK(draw_detail_page());
}

static esp_err_t wifi_interchange_draw(void)
{ return wifi_interchange_ui_draw(wifi_interchange_view, wifi_interchange_open_error); }

static void wifi_interchange_tap(uint16_t x, uint16_t y)
{
    wifi_interchange_action_t action = wifi_interchange_ui_tap(wifi_interchange_view, x, y);
    if (action == WIFI_INTERCHANGE_BACK) { wifi_interchange_close(true); return; }
    if (action == WIFI_INTERCHANGE_EDIT_COPY) {
        esp_err_t error = ssid_lists_import_document(&wifi_interchange_view->document.ssid,
            wifi_interchange_from_archive ? wifi_archive_return_page : wifi_interchange_return_page);
        if (error == ESP_OK) {
            if (wifi_interchange_from_archive) {
                wifi_mode = wifi_archive_return_mode;
                wifi_archive_dispose();
            }
            wifi_interchange_dispose();
        } else wifi_interchange_error(wifi_interchange_view, error);
        ESP_ERROR_CHECK(draw_detail_page());
        return;
    }
    if (action == WIFI_INTERCHANGE_SAVE || action == WIFI_INTERCHANGE_EXPORT_AP || action == WIFI_INTERCHANGE_EXPORT_SSID) {
        wifi_interchange_ui_store_t *s = wifi_interchange_view;
        uint64_t job = 0;
        esp_err_t error = ESP_ERR_INVALID_STATE;
        s->saving = true; s->exporting = action != WIFI_INTERCHANGE_SAVE;
        s->write_started = s->stats_valid = false; s->failure_name[0] = 0;
        s->result = CERBERUS_WIFI_MARAUDER_OK; memset(&s->stats, 0, sizeof(s->stats));
        if (s->exporting) {
            s->ap_json = action == WIFI_INTERCHANGE_EXPORT_AP;
            if (wifi_interchange_from_archive && wifi_archive_view && wifi_archive_view->has_document &&
                !wifi_archive_view->job_pending && !wifi_archive_view->release_pending)
                error = s->ap_json ? cerberus_ble_archive_io_export_ap_json(&wifi_archive_view->document, &job) :
                    cerberus_ble_archive_io_export_ssid_text(&wifi_archive_view->document, &job);
        } else error = s->ap_json ? cerberus_ble_archive_io_save_ap_json(&s->document.ap, &job) :
            cerberus_ble_archive_io_save_ssid_text(&s->document.ssid, &job);
        wifi_interchange_accept(error, job);
    }
    if (action != WIFI_INTERCHANGE_NONE) ESP_ERROR_CHECK(draw_detail_page());
}

static bool wifi_interchange_tick(bool asleep)
{
    if (!wifi_interchange_opened) return false;
    if (current_page != UI_PAGE_WIFI_ARCHIVE) { wifi_interchange_dispose(); return false; }
    if (asleep) {
        bool native = wifi_interchange_from_archive;
        wifi_interchange_close(false);
        if (native) wifi_archive_close(false);
        return true;
    }
    wifi_interchange_ui_store_t *s = wifi_interchange_view;
    if (!s || !s->job_pending) return true;
    cerberus_ble_archive_io_status_t status;
    cerberus_ble_archive_io_get_status(&status);
    const uint64_t job = s->job_id;
    if (status.job_id != job || status.worker_active) return true;
    if (status.state != CERBERUS_BLE_ARCHIVE_IO_COMPLETE && status.state != CERBERUS_BLE_ARCHIVE_IO_ERROR &&
        status.state != CERBERUS_BLE_ARCHIVE_IO_CANCELED) return true;
    s->stats = status.wifi_export_stats; s->stats_valid = s->exporting;
    s->result = status.wifi_result;
    esp_err_t error = status.error;
    if (status.state == CERBERUS_BLE_ARCHIVE_IO_COMPLETE && status.result_ready &&
        status.format == (s->ap_json ? CERBERUS_BLE_ARCHIVE_IO_FORMAT_AP_JSON : CERBERUS_BLE_ARCHIVE_IO_FORMAT_SSID_TEXT)) {
        error = s->ap_json ? cerberus_ble_archive_io_copy_ap_json(job, &s->document.ap) :
            cerberus_ble_archive_io_copy_ssid_text(job, &s->document.ssid);
        if (error == ESP_OK) {
            wifi_interchange_invalidate(s); s->job_pending = false; s->has_document = true;
            s->page = s->detail_page = 0; s->view = WIFI_INTERCHANGE_LIST; s->error = ESP_OK;
            wifi_interchange_name(s->file_name, sizeof(s->file_name), status.file_name); s->failure_name[0] = 0;
        }
    } else if (error == ESP_OK) error = ESP_ERR_INVALID_RESPONSE;
    if (error != ESP_OK) {
        wifi_interchange_name(s->failure_name, sizeof(s->failure_name), status.file_name);
        wifi_interchange_error(s, error);
    }
    cerberus_ble_archive_io_release(job); ESP_ERROR_CHECK(draw_detail_page());
    return true;
}
