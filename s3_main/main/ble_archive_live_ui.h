/* Private UI adapter, included after the live discovery definitions. The UI
 * owns the displayed documents; the worker owns a separate immutable copy. */
#include "ble_archive_ui.h"
#include "ble_archive_io.h"
#include "ble_archive_capture.h"
#include "dual_ble_archive_capture.h"

enum { BLE_ARCHIVE_BOTH, BLE_ARCHIVE_S3_SELECTED, BLE_ARCHIVE_C5_SELECTED };
#define BLE_ARCHIVE_RELEASE_TIMEOUT_MS UINT64_C(12000)

static ble_archive_ui_store_t *ble_archive_view;
static esp_err_t ble_archive_open_error;
static bool ble_archive_menu;
static ui_page_t ble_archive_return_page = UI_PAGE_BLE;
static ui_ble_mode_t ble_archive_return_mode = UI_BLE_MODE_MENU;

static bool ble_archive_is_file(const char *name)
{
    if (!name) return false;
    size_t length = strlen(name);
    if (length < 5U) return false;
    const char *suffix = name + length - 4U;
    return suffix[0] == '.' && (suffix[1] == 'B' || suffix[1] == 'b') &&
        (suffix[2] == 'L' || suffix[2] == 'l') && (suffix[3] == 'E' || suffix[3] == 'e');
}

static bool ble_archive_acquire(void)
{
    if (ble_archive_view) return true;
    ble_archive_view = heap_caps_calloc(1U, sizeof(*ble_archive_view),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ble_archive_open_error = ble_archive_view ? ESP_OK : ESP_ERR_NO_MEM;
    return ble_archive_view != NULL;
}

static void ble_archive_dispose(void)
{
    if (saved_ble_lookup_holds_files()) return;
    if (ble_archive_view) {
        if (ble_archive_view->job_id) {
            cerberus_ble_archive_io_cancel(ble_archive_view->job_id);
            cerberus_ble_archive_io_release(ble_archive_view->job_id);
        }
        heap_caps_free(ble_archive_view);
        ble_archive_view = NULL;
    }
    ble_archive_menu = false;
}

static void ble_archive_enter(void)
{
    if (current_page != UI_PAGE_BLE_ARCHIVE) {
        ble_archive_return_page = current_page;
        ble_archive_return_mode = ble_mode;
    }
    current_page = UI_PAGE_BLE_ARCHIVE;
    ble_archive_menu = false;
    (void)ble_archive_acquire();
}

static void ble_archive_return(void)
{
    ble_archive_dispose();
    current_page = ble_archive_return_page;
    if (current_page == UI_PAGE_BLE) ble_mode = ble_archive_return_mode;
    ESP_ERROR_CHECK(draw_current_page());
}

static void ble_archive_open_menu(void)
{
    if (saved_ble_lookup_holds_files()) return;
    ble_archive_enter();
    ble_archive_menu = ble_archive_view != NULL;
    ESP_ERROR_CHECK(draw_detail_page());
}

static esp_err_t ble_archive_capture(unsigned scope)
{
    if (!ble_archive_view) return ESP_ERR_NO_MEM;
    int64_t stamp = esp_timer_get_time();
    if (stamp < 0) return ESP_ERR_INVALID_STATE;
    cerberus_ble_archive_t *out = &ble_archive_view->incoming;
    memset(out, 0, sizeof(*out));
    out->saved_uptime_ms = (uint64_t)stamp / 1000U;
    if (scope == BLE_ARCHIVE_C5_SELECTED) {
        if (!dual_ble_ui_detail) return ESP_ERR_NOT_FOUND;
        if (!cerberus_dual_ble_archive_capture_row(&out->rows[0], &dual_ble_ui_selected,
                &dual_ble_ui_selected_provenance, out->saved_uptime_ms))
            return ESP_ERR_INVALID_RESPONSE;
        out->count = 1U;
    } else if (scope == BLE_ARCHIVE_S3_SELECTED) {
        if (!ble_selection_valid || !ble_selected_raw_frozen ||
            ble_findmy_selected.peer.address_type != ble_selected_result.address_type ||
            memcmp(ble_findmy_selected.peer.address, ble_selected_result.address, 6U))
            return ESP_ERR_NOT_FOUND;
        if (!cerberus_ble_archive_capture_row(&out->rows[0], &ble_findmy_selected.peer,
                &ble_findmy_selected.advertisement, ble_findmy_selected.generation,
                ble_findmy_selected.index, ble_selected_scan_epoch,
                ble_selected_scan_epoch != 0U, ble_selected_original_ms64,
                ble_selected_source_ceiling_ms64, ble_selected_original_time64_valid) ||
            !cerberus_ble_archive_capture_response(&out->rows[0], &ble_findmy_selected.focus,
                &ble_findmy_selected.advertisement, ble_findmy_selected.observed_ms))
            return ESP_ERR_INVALID_RESPONSE;
        out->count = 1U;
    } else if (scope == BLE_ARCHIVE_BOTH) {
        const cerberus_s3_ble_discovery_inventory_t *inventory = ble_discovery_live &&
            ble_discovery_live->inventory.initialized ? &ble_discovery_live->inventory : NULL;
        const uint8_t s3_count = inventory ? inventory->count : 0U;
        const uint8_t c5_count = dual_ble_ui_store ? dual_ble_ui_store->count : 0U;
        if (s3_count > CERBERUS_BLE_ARCHIVE_MAX_ROWS_PER_RECEIVER ||
            c5_count > CERBERUS_BLE_ARCHIVE_MAX_ROWS_PER_RECEIVER) return ESP_ERR_INVALID_SIZE;
        for (uint8_t i = 0; i < s3_count; ++i) {
            const cerberus_s3_ble_discovery_entry_t *entry = &inventory->entries[i];
            const cerberus_s3_ble_discovery_record_t *record = &entry->record;
            if (!cerberus_ble_archive_capture_row(&out->rows[i], &record->peer,
                    &record->advertisement, record->generation, record->index,
                    entry->epoch, true, entry->original_source_ms, entry->source_now_ms, true) ||
                !cerberus_ble_archive_capture_response(&out->rows[i], &record->focus,
                    &record->advertisement, record->observed_ms))
                return ESP_ERR_INVALID_RESPONSE;
        }
        out->count = s3_count;
        /* UI task owns both publications. No step/clock/SPI calls can replace
         * either bank during this bounded copy; uncommitted staging is ignored. */
        for (uint8_t i = 0U; i < c5_count; ++i) {
            cerberus_dual_ble_row_t row;
            cerberus_dual_ble_provenance_t provenance;
            if (!cerberus_dual_ble_copy_row(dual_ble_ui_store, i, &row, &provenance) ||
                !cerberus_dual_ble_archive_capture_row(&out->rows[out->count], &row,
                    &provenance, out->saved_uptime_ms)) return ESP_ERR_INVALID_RESPONSE;
            ++out->count;
        }
        if (!out->count) return ESP_ERR_NOT_FOUND;
    } else return ESP_ERR_INVALID_ARG;
    return cerberus_ble_archive_validate(out) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static void ble_archive_submit(void)
{
    if (!ble_archive_view || !ble_archive_view->release_pending) return;
    ble_archive_view->release_pending = false;
    uint64_t job = 0U;
    esp_err_t error = cerberus_ble_archive_io_save(&ble_archive_view->incoming, &job);
    if (ble_archive_view) {
        if (error == ESP_OK && !ble_archive_ui_begin(ble_archive_view, job, true)) {
            cerberus_ble_archive_io_cancel(job);
            cerberus_ble_archive_io_release(job);
            error = ESP_ERR_INVALID_STATE;
        }
        if (error != ESP_OK) ble_archive_ui_error(ble_archive_view, error);
    }
    ble_archive_open_error = error;
}

static void ble_archive_save(unsigned scope)
{
    if (ble_archive_view && (ble_archive_view->release_pending || ble_archive_view->job_pending)) return;
    ble_archive_enter();
    esp_err_t error = ble_archive_capture(scope);
    /* Freeze first, then cancel the acquisition/navigation owned by the page
     * that requested Save. Failure still cancels intent: an immediate Back
     * must not rearm pre-INFO work or apply an older pending row selection.
     * A stale ordinary request must never cancel another page's receiver. */
    if (ble_archive_return_page == UI_PAGE_BLE) {
        if (ble_archive_return_mode == UI_BLE_MODE_FINDMY_MONITOR)
            ble_findmy_request_stop(false, false);
        else if (ble_archive_return_mode == UI_BLE_MODE_LIST ||
            (ble_archive_return_mode == UI_BLE_MODE_C5 && !dual_ble_ui_from_monitor) ||
            (ble_archive_return_mode == UI_BLE_MODE_DETAIL && !ble_selected_from_monitor))
            ble_discovery_leave();
    }
    if (error == ESP_OK) {
        ble_archive_view->release_pending = true;
        ble_archive_view->release_started_ms = ble_archive_view->incoming.saved_uptime_ms;
        ble_archive_view->view = BLE_ARCHIVE_UI_JOB;
        ble_archive_view->saving = true;
        ble_archive_view->paint_valid = ble_archive_view->selected_valid = false;
        ble_archive_view->error = ESP_OK;
    } else if (ble_archive_view) ble_archive_ui_error(ble_archive_view, error);
    ble_archive_open_error = error;
    ESP_ERROR_CHECK(draw_detail_page());
}

static void ble_archive_save_selected(void) { ble_archive_save(BLE_ARCHIVE_S3_SELECTED); }
static void ble_archive_save_c5(bool selected)
{ ble_archive_save(selected ? BLE_ARCHIVE_C5_SELECTED : BLE_ARCHIVE_BOTH); }

static void ble_archive_open_file(const char *path, const char *name)
{
    if (saved_ble_lookup_holds_files()) return;
    ble_archive_enter();
    uint64_t job = 0U;
    esp_err_t error = ble_archive_view ? cerberus_ble_archive_io_load(path, name, &job) : ESP_ERR_NO_MEM;
    if (ble_archive_view) {
        if (error == ESP_OK && !ble_archive_ui_begin(ble_archive_view, job, false)) {
            cerberus_ble_archive_io_cancel(job);
            cerberus_ble_archive_io_release(job);
            error = ESP_ERR_INVALID_STATE;
        }
        if (error != ESP_OK) ble_archive_ui_error(ble_archive_view, error);
    }
    ble_archive_open_error = error;
    ESP_ERROR_CHECK(draw_detail_page());
}

static esp_err_t ble_archive_draw(void)
{
    if (saved_ble_lookup_is_open()) return saved_ble_lookup_draw();
    if (airtag_json_is_open()) return airtag_json_draw();
    if (!ble_archive_menu || !ble_archive_view)
        return ble_archive_ui_draw(ble_archive_view, ble_archive_open_error);
    ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(0, 0, 320, 480, UI_BACKGROUND), TAG, "archive clear");
    ESP_RETURN_ON_ERROR(draw_header("BLE SD SNAPSHOTS"), TAG, "archive header");
    ESP_RETURN_ON_ERROR(draw_centered(78, "SAVE RETAINED OBSERVATIONS FOR LATER", UI_TEXT, 1), TAG, "archive scope");
    ESP_RETURN_ON_ERROR(draw_centered(102, "ORIGINAL ADV + AVAILABLE MATCHED RESPONSE", UI_MUTED, 1), TAG, "archive source");
    ESP_RETURN_ON_ERROR(draw_button(24, 144, 272, 48, "SAVE BOTH RECEIVERS"), TAG, "archive list save");
    ESP_RETURN_ON_ERROR(draw_button(24, 208, 272, 48, "SAVE SELECTED S3 ROW"), TAG, "archive selected save");
    ESP_RETURN_ON_ERROR(draw_button(24, 272, 272, 48, "BROWSE SD FILES"), TAG, "archive browse");
    ESP_RETURN_ON_ERROR(draw_centered(346, "FILES: /CERBERUS/BLE/B00001.BLE", UI_MUTED, 1), TAG, "archive path");
    ESP_RETURN_ON_ERROR(draw_centered(370, "RETAINED S3 + C5 / UP TO 32 ROWS EACH", UI_MUTED, 1), TAG, "archive history");
    return draw_button(64, 426, 192, 44, "BACK TO BLUETOOTH");
}

static void ble_archive_tap(uint16_t x, uint16_t y)
{
    if (saved_ble_lookup_is_open()) { saved_ble_lookup_tap(x, y); return; }
    if (airtag_json_is_open()) { airtag_json_tap(x, y); return; }
    if (ble_archive_menu && ble_archive_view) {
        if (x >= 24U && x < 296U) {
            if (y >= 144U && y < 192U) { ble_archive_save(BLE_ARCHIVE_BOTH); return; }
            if (y >= 208U && y < 256U) { ble_archive_save(BLE_ARCHIVE_S3_SELECTED); return; }
            if (y >= 272U && y < 320U) {
                ble_archive_dispose();
                file_mode = UI_FILE_MODE_LIST;
                open_tool_page(UI_PAGE_FILES);
                ESP_ERROR_CHECK(draw_detail_page());
                return;
            }
        }
        if (x >= 64U && x < 256U && y >= 426U && y < 470U) ble_archive_return();
        return;
    }
    ble_archive_ui_action_t action = ble_archive_ui_tap(ble_archive_view, x, y);
    if (action == BLE_ARCHIVE_UI_BACK || action == BLE_ARCHIVE_UI_CANCEL) ble_archive_return();
    else if (action == BLE_ARCHIVE_UI_EXPORT) airtag_json_open_export();
    else if (action == BLE_ARCHIVE_UI_FIND_LIVE) saved_ble_lookup_open(false);
    else if (action == BLE_ARCHIVE_UI_REDRAW) ESP_ERROR_CHECK(draw_detail_page());
}

static void ble_archive_tick(bool asleep)
{
    if (saved_ble_lookup_tick(asleep)) return;
    if (airtag_json_tick(asleep)) return;
    if (current_page != UI_PAGE_BLE_ARCHIVE) { ble_archive_dispose(); return; }
    if (ble_archive_view && ble_archive_view->release_pending) {
        int64_t clock = esp_timer_get_time();
        uint64_t now = clock >= 0 ? (uint64_t)clock / 1000U : 0U;
        esp_err_t error = asleep || clock < 0 || now < ble_archive_view->release_started_ms ?
            ESP_ERR_INVALID_STATE : now - ble_archive_view->release_started_ms >= BLE_ARCHIVE_RELEASE_TIMEOUT_MS ?
            ESP_ERR_TIMEOUT : ESP_OK;
        if (error != ESP_OK) {
            ble_archive_view->release_pending = false;
            ble_archive_ui_error(ble_archive_view, error);
            ble_archive_open_error = error;
        } else if (dual_ble_live_discovery_released() && dual_ble_ui_released()) {
            ble_archive_submit();
        } else return; /* Global owner ticks continue release; this getter never drives RF. */
        if (!asleep) ESP_ERROR_CHECK(draw_detail_page());
        return;
    }
    if (!ble_archive_view || !ble_archive_view->job_pending) return;
    cerberus_ble_archive_io_status_t status;
    cerberus_ble_archive_io_get_status(&status);
    const uint64_t job = ble_archive_view->job_id;
    if (status.job_id != job || status.worker_active) return;
    if (status.state == CERBERUS_BLE_ARCHIVE_IO_COMPLETE && status.result_ready) {
        esp_err_t error = cerberus_ble_archive_io_copy_result(job, &ble_archive_view->incoming);
        if (error == ESP_OK)
            ble_archive_ui_complete(ble_archive_view, job, &ble_archive_view->incoming, status.file_name);
        else ble_archive_ui_fail(ble_archive_view, job, error);
    } else if (status.state == CERBERUS_BLE_ARCHIVE_IO_COMPLETE ||
               status.state == CERBERUS_BLE_ARCHIVE_IO_ERROR ||
               status.state == CERBERUS_BLE_ARCHIVE_IO_CANCELED) {
        ble_archive_ui_fail(ble_archive_view, job,
            status.error == ESP_OK ? ESP_ERR_INVALID_STATE : status.error);
    } else return;
    cerberus_ble_archive_io_release(job);
    if (!asleep) ESP_ERROR_CHECK(draw_detail_page());
}
