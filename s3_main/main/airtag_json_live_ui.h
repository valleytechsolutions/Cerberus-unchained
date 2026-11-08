/* UI-only offline adapter. The existing serialized archive worker owns SD IO.
 * This optional PSRAM model is never installed in a live BLE bank. */
#include "airtag_json_ui.h"
#include "ble_archive_io.h"

static airtag_json_ui_store_t *airtag_json_view;
static bool airtag_json_opened, airtag_json_from_archive;
static esp_err_t airtag_json_open_error;
static ui_page_t airtag_json_return_page;
static ui_ble_mode_t airtag_json_return_mode;

static bool airtag_json_is_file(const char *name)
{
    if (!name) return false;
    const size_t n = strlen(name);
    const char *p = n >= 4U ? name + n - 4U : name;
    if (n > 4U && p[0] == '.' && (p[1] == 'l' || p[1] == 'L') &&
        (p[2] == 'o' || p[2] == 'O') && (p[3] == 'g' || p[3] == 'G')) return true;
    p = n >= 5U ? name + n - 5U : name;
    return n > 5U && p[0] == '.' && (p[1] == 'j' || p[1] == 'J') &&
        (p[2] == 's' || p[2] == 'S') && (p[3] == 'o' || p[3] == 'O') && (p[4] == 'n' || p[4] == 'N');
}

static bool airtag_json_is_open(void) { return airtag_json_opened; }

static void airtag_json_dispose(void)
{
    if (saved_ble_lookup_holds_files()) return;
    if (airtag_json_view) {
        if (airtag_json_view->job_pending) {
            cerberus_ble_archive_io_cancel(airtag_json_view->job_id);
            cerberus_ble_archive_io_release(airtag_json_view->job_id);
        }
        heap_caps_free(airtag_json_view); airtag_json_view = NULL;
    }
    airtag_json_opened = false;
}

static void airtag_json_close(bool redraw)
{
    airtag_json_dispose();
    current_page = airtag_json_return_page;
    if (current_page == UI_PAGE_BLE) ble_mode = airtag_json_return_mode;
    if (redraw) ESP_ERROR_CHECK(draw_current_page());
}

static void airtag_json_enter(bool from_archive)
{
    airtag_json_dispose();
    airtag_json_return_page = current_page; airtag_json_return_mode = ble_mode;
    airtag_json_from_archive = from_archive;
    if (!from_archive) ble_archive_dispose();
    current_page = UI_PAGE_BLE_ARCHIVE; airtag_json_opened = true;
    airtag_json_view = heap_caps_calloc(1U, sizeof(*airtag_json_view), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    airtag_json_open_error = airtag_json_view ? ESP_OK : ESP_ERR_NO_MEM;
}

static void airtag_json_accept(esp_err_t error, uint64_t job)
{
    if (!airtag_json_view) return;
    if (error == ESP_OK && job) {
        airtag_json_ui_invalidate(airtag_json_view);
        airtag_json_view->job_id = job; airtag_json_view->job_pending = true;
        airtag_json_view->write_started = airtag_json_view->saving;
        airtag_json_view->view = AIRTAG_JSON_JOB; airtag_json_view->error = ESP_OK;
    } else airtag_json_ui_error(airtag_json_view, error == ESP_OK ? ESP_ERR_INVALID_RESPONSE : error);
}

static void airtag_json_open_file(const char *path, const char *name)
{
    if (saved_ble_lookup_holds_files()) return;
    if (!airtag_json_is_file(name)) return;
    airtag_json_enter(false);
    if (airtag_json_view) {
        uint64_t job = 0U;
        const esp_err_t error = cerberus_ble_archive_io_load_airtag_json(path, name, &job);
        airtag_json_accept(error, job);
    }
    ESP_ERROR_CHECK(draw_detail_page());
}

static void airtag_json_open_export(void)
{
    if (!ble_archive_view || !ble_archive_view->has_document || ble_archive_view->job_pending ||
        ble_archive_view->release_pending || ble_archive_menu) return;
    airtag_json_enter(true);
    if (airtag_json_view) { airtag_json_view->view = AIRTAG_JSON_CONFIRM; airtag_json_view->exporting = true; }
    ESP_ERROR_CHECK(draw_detail_page());
}

static esp_err_t airtag_json_draw(void)
{ return airtag_json_ui_draw(airtag_json_view, airtag_json_open_error); }

static void airtag_json_tap(uint16_t x, uint16_t y)
{
    const airtag_json_ui_action_t action = airtag_json_ui_tap(airtag_json_view, x, y);
    if (action == AIRTAG_JSON_FIND_LIVE) { saved_ble_lookup_open(true); return; }
    if (action == AIRTAG_JSON_BACK) { airtag_json_close(true); return; }
    if (action == AIRTAG_JSON_SAVE || action == AIRTAG_JSON_EXPORT) {
        uint64_t job = 0U;
        esp_err_t error = ESP_ERR_INVALID_STATE;
        airtag_json_view->saving = true;
        airtag_json_view->exporting = action == AIRTAG_JSON_EXPORT;
        airtag_json_view->write_started = false;
        airtag_json_view->failure_name[0] = 0;
        memset(&airtag_json_view->stats, 0, sizeof(airtag_json_view->stats));
        if (action == AIRTAG_JSON_EXPORT) {
            if (airtag_json_from_archive && ble_archive_view && ble_archive_view->has_document &&
                !ble_archive_view->job_pending && !ble_archive_view->release_pending)
                error = cerberus_ble_archive_io_export_airtag_json(&ble_archive_view->document, &job);
        } else error = cerberus_ble_archive_io_save_airtag_json(&airtag_json_view->document, &job);
        airtag_json_accept(error, job);
    }
    if (action != AIRTAG_JSON_NONE) ESP_ERROR_CHECK(draw_detail_page());
}

/* Returns true while it handled the shared archive page, including the close
 * on sleep. Hidden/abandoned jobs can finish storage but cannot reopen the UI. */
static bool airtag_json_tick(bool asleep)
{
    if (!airtag_json_opened) return false;
    if (current_page != UI_PAGE_BLE_ARCHIVE) { airtag_json_dispose(); return false; }
    if (asleep) { airtag_json_close(false); return true; }
    if (!airtag_json_view || !airtag_json_view->job_pending) return true;
    cerberus_ble_archive_io_status_t status;
    cerberus_ble_archive_io_get_status(&status);
    const uint64_t job = airtag_json_view->job_id;
    if (status.job_id != job || status.worker_active) return true;
    if (status.state != CERBERUS_BLE_ARCHIVE_IO_COMPLETE && status.state != CERBERUS_BLE_ARCHIVE_IO_ERROR &&
        status.state != CERBERUS_BLE_ARCHIVE_IO_CANCELED) return true;
    airtag_json_view->stats = status.export_stats;
    esp_err_t error = status.error;
    if (status.state == CERBERUS_BLE_ARCHIVE_IO_COMPLETE && status.result_ready &&
        status.format == CERBERUS_BLE_ARCHIVE_IO_FORMAT_AIRTAG_JSON) {
        /* Typed result copy is transactional: failure leaves this model intact.
         * The worker already performed complete codec validation. */
        error = cerberus_ble_archive_io_copy_airtag_json(job, &airtag_json_view->document);
        if (error == ESP_OK) {
            airtag_json_ui_name(airtag_json_view, status.file_name);
            airtag_json_view->failure_name[0] = 0;
            airtag_json_view->has_document = true; airtag_json_view->job_pending = false;
            airtag_json_ui_invalidate(airtag_json_view);
            airtag_json_view->page = 0U; airtag_json_view->raw_page = 0U;
            airtag_json_view->error = ESP_OK; airtag_json_view->view = AIRTAG_JSON_LIST;
        }
    } else if (error == ESP_OK) error = ESP_ERR_INVALID_RESPONSE;
    if (error != ESP_OK) {
        size_t i = 0U;
        for (; i + 1U < sizeof(airtag_json_view->failure_name) && status.file_name[i]; ++i)
            airtag_json_view->failure_name[i] = (uint8_t)status.file_name[i] >= 32U &&
                (uint8_t)status.file_name[i] <= 126U ? status.file_name[i] : '.';
        airtag_json_view->failure_name[i] = 0;
        airtag_json_ui_error(airtag_json_view, error);
    }
    cerberus_ble_archive_io_release(job);
    ESP_ERROR_CHECK(draw_detail_page());
    return true;
}
