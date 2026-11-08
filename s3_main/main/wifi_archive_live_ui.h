#pragma once

/* One optional historical view, sharing the existing serialized file worker.
 * Capture freezes a coherent retained publication before cancel/drain. No file
 * row/index/generation is ever assigned to an active AP tool or live bank. */
#include "wifi_archive_ui.h"
#include "ble_archive_io.h"

static wifi_archive_ui_store_t *wifi_archive_view;
static bool wifi_archive_opened;
static esp_err_t wifi_archive_open_error;
static ui_page_t wifi_archive_return_page;
static ui_wifi_mode_t wifi_archive_return_mode;

static bool wifi_archive_is_file(const char *name)
{
    if (!name) return false;
    const size_t n = strlen(name);
    return n > 4U && name[n - 4U] == '.' &&
        (name[n - 3U] == 'w' || name[n - 3U] == 'W') &&
        (name[n - 2U] == 'f' || name[n - 2U] == 'F') &&
        (name[n - 1U] == 'i' || name[n - 1U] == 'I');
}

static void wifi_archive_dispose(void)
{
    if (wifi_archive_view) {
        if (wifi_archive_view->job_pending) {
            cerberus_ble_archive_io_cancel(wifi_archive_view->job_id);
            cerberus_ble_archive_io_release(wifi_archive_view->job_id);
        }
        heap_caps_free(wifi_archive_view); wifi_archive_view = NULL;
    }
    wifi_archive_opened = false;
}

static void wifi_archive_close(bool redraw)
{
    wifi_archive_dispose(); current_page = wifi_archive_return_page;
    if (current_page == UI_PAGE_WIFI) {
        wifi_mode = wifi_archive_return_mode;
        if (wifi_mode == UI_WIFI_MODE_DUAL) dual_wifi_live_open(false);
    }
    if (redraw) ESP_ERROR_CHECK(draw_current_page());
}

static void wifi_archive_enter(void)
{
    wifi_archive_dispose();
    wifi_archive_return_page = current_page; wifi_archive_return_mode = wifi_mode;
    current_page = UI_PAGE_WIFI_ARCHIVE; wifi_archive_opened = true;
    wifi_archive_view = heap_caps_calloc(1U, sizeof(*wifi_archive_view), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    wifi_archive_open_error = wifi_archive_view ? ESP_OK : ESP_ERR_NO_MEM;
}

static void wifi_archive_accept(esp_err_t error, uint64_t job)
{
    if (!wifi_archive_view) return;
    if (error == ESP_OK && job) {
        wifi_archive_ui_invalidate(wifi_archive_view);
        wifi_archive_view->job_id = job; wifi_archive_view->job_pending = true;
        wifi_archive_view->write_started = wifi_archive_view->saving;
        wifi_archive_view->view = WIFI_ARCHIVE_JOB; wifi_archive_view->error = ESP_OK;
    } else wifi_archive_ui_error(wifi_archive_view, error == ESP_OK ? ESP_ERR_INVALID_RESPONSE : error);
}

static void wifi_archive_start_save(void)
{
    uint64_t job = 0U;
    wifi_archive_view->release_pending = false; wifi_archive_view->saving = true;
    wifi_archive_view->write_started = false; wifi_archive_view->failure_name[0] = 0;
    esp_err_t error = cerberus_ble_archive_io_save_wifi(&wifi_archive_view->document, &job);
    wifi_archive_accept(error, job);
}

static void wifi_archive_save_live(bool selected_only)
{
    if (!dual_wifi_ui_store || !dual_wifi_ui_store->cache_valid ||
        (selected_only && !dual_wifi_ui_store->save_mask)) return;
    const uint32_t generation = dual_wifi_ui_store->generation;
    const uint64_t mask = selected_only ? dual_wifi_ui_store->save_mask : UINT64_MAX;
    wifi_archive_enter();
    if (wifi_archive_view) {
        esp_err_t error = cerberus_dual_wifi_scan_capture_archive(esp_timer_get_time(), mask,
            &wifi_archive_view->document);
        if (error == ESP_OK && wifi_archive_view->document.publication.generation != generation)
            error = ESP_ERR_INVALID_STATE;
        if (error == ESP_OK) {
            wifi_archive_view->has_document = true;
            wifi_archive_ui_name(wifi_archive_view->file_name, sizeof(wifi_archive_view->file_name), "UNSAVED WI-FI LIST");
            wifi_archive_view->release_pending = wifi_archive_view->saving = true;
            wifi_archive_view->view = WIFI_ARCHIVE_JOB;
        } else wifi_archive_ui_error(wifi_archive_view, error);
    }
    /* Revoke pending Start/handoff even if capture/allocation failed. Only the
     * dual coordinator's accepted work is stopped; global ticks drain it. */
    dual_wifi_live_cancel_handoff(); dual_wifi_ui_leave();
    ESP_ERROR_CHECK(draw_detail_page());
}

static void wifi_archive_open_file(const char *path, const char *name)
{
    if (!wifi_archive_is_file(name)) return;
    wifi_archive_enter();
    if (wifi_archive_view) {
        uint64_t job = 0U;
        esp_err_t error = cerberus_ble_archive_io_load_wifi(path, name, &job);
        wifi_archive_accept(error, job);
    }
    ESP_ERROR_CHECK(draw_detail_page());
}

static esp_err_t wifi_archive_draw(void)
{
    if (wifi_interchange_is_open()) return wifi_interchange_draw();
    return wifi_archive_ui_draw(wifi_archive_view, wifi_archive_open_error);
}

static void wifi_archive_tap(uint16_t x, uint16_t y)
{
    if (wifi_interchange_is_open()) { wifi_interchange_tap(x, y); return; }
    wifi_archive_ui_action_t action = wifi_archive_ui_tap(wifi_archive_view, x, y);
    if (action == WIFI_ARCHIVE_BACK) { wifi_archive_close(true); return; }
    if (action == WIFI_ARCHIVE_NEW_SCAN) {
        wifi_archive_dispose(); dual_wifi_live_open(true);
        ESP_ERROR_CHECK(draw_detail_page()); return;
    }
    if (action == WIFI_ARCHIVE_SAVE) wifi_archive_start_save();
    if (action == WIFI_ARCHIVE_EXPORT) { wifi_interchange_open_export(); return; }
    if (action != WIFI_ARCHIVE_NONE) ESP_ERROR_CHECK(draw_detail_page());
}

static void wifi_archive_tick(bool asleep)
{
    if (wifi_interchange_tick(asleep)) return;
    if (!wifi_archive_opened) return;
    if (current_page != UI_PAGE_WIFI_ARCHIVE) { wifi_archive_dispose(); return; }
    if (asleep) { wifi_archive_close(false); return; }
    if (!wifi_archive_view) return;
    if (wifi_archive_view->release_pending) {
        cerberus_dual_wifi_status_t status;
        cerberus_dual_wifi_scan_get_status(&status);
        if (dual_wifi_ui_busy(&status)) return;
        wifi_archive_start_save(); ESP_ERROR_CHECK(draw_detail_page()); return;
    }
    if (!wifi_archive_view->job_pending) return;
    cerberus_ble_archive_io_status_t status;
    cerberus_ble_archive_io_get_status(&status);
    uint64_t job = wifi_archive_view->job_id;
    if (status.job_id != job || status.worker_active) return;
    if (status.state != CERBERUS_BLE_ARCHIVE_IO_COMPLETE && status.state != CERBERUS_BLE_ARCHIVE_IO_ERROR &&
        status.state != CERBERUS_BLE_ARCHIVE_IO_CANCELED) return;
    esp_err_t error = status.error;
    if (status.state == CERBERUS_BLE_ARCHIVE_IO_COMPLETE && status.result_ready &&
        status.format == CERBERUS_BLE_ARCHIVE_IO_FORMAT_WIFI) {
        error = cerberus_ble_archive_io_copy_wifi(job, &wifi_archive_view->document);
        if (error == ESP_OK) {
            wifi_archive_ui_invalidate(wifi_archive_view);
            wifi_archive_view->has_document = true; wifi_archive_view->job_pending = false;
            wifi_archive_view->group_focus = false; wifi_archive_view->groups = false;
            wifi_archive_view->page = wifi_archive_view->source = wifi_archive_view->band = wifi_archive_view->sort = 0U;
            wifi_archive_view->view = WIFI_ARCHIVE_LIST; wifi_archive_view->error = ESP_OK;
            wifi_archive_ui_name(wifi_archive_view->file_name, sizeof(wifi_archive_view->file_name), status.file_name);
            wifi_archive_view->failure_name[0] = 0;
        }
    } else if (error == ESP_OK) error = ESP_ERR_INVALID_RESPONSE;
    if (error != ESP_OK) {
        wifi_archive_ui_name(wifi_archive_view->failure_name, sizeof(wifi_archive_view->failure_name), status.file_name);
        wifi_archive_ui_error(wifi_archive_view, error);
    }
    cerberus_ble_archive_io_release(job);
    ESP_ERROR_CHECK(draw_detail_page());
}
