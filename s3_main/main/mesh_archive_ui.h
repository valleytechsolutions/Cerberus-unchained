#pragma once

/* Historical values only. This view never owns a radio/controller, constructs
 * a live target, or reads the filesystem. The serialized archive worker owns IO;
 * the existing Files browser owns listing and bounded CSV text viewing. */
#include "mesh_archive.h"

typedef struct {
    mesh_archive_t document;
    mesh_ui_painted_t painted[MESH_UI_ROWS], selected;
    uint64_t job;
    uint8_t group, page, count, painted_count, raw_page;
    bool has_document, detail, exporting;
    esp_err_t error;
    char name[CERBERUS_STORAGE_NAME_MAX], pending_name[CERBERUS_STORAGE_NAME_MAX];
    char csv_name[CERBERUS_STORAGE_NAME_MAX];
} mesh_archive_ui_store_t;

static mesh_archive_ui_store_t *mesh_archive_view;
static esp_err_t mesh_archive_open_error;

static bool mesh_archive_is_file(const char *name)
{
    if (!name) return false;
    size_t n = strlen(name);
    return n > 4U && name[n - 4U] == '.' &&
        (name[n - 3U] == 'M' || name[n - 3U] == 'm') &&
        (name[n - 2U] == 'S' || name[n - 2U] == 's') &&
        (name[n - 1U] == 'H' || name[n - 1U] == 'h');
}

static esp_err_t mesh_archive_save_scan(const mesh_scan_t *scan, uint64_t *job)
{
    *job = 0U;
    mesh_archive_t *document = heap_caps_calloc(1U, sizeof(*document),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!document) return ESP_ERR_NO_MEM;
    esp_err_t error = mesh_archive_from_scan(scan, document) ?
        cerberus_ble_archive_io_save_mesh_archive(document, job) : ESP_ERR_INVALID_STATE;
    /* Accepted jobs copy before returning; no worker points into this value. */
    heap_caps_free(document);
    return error;
}

static void mesh_archive_dispose(void)
{
    if (!mesh_archive_view) return;
    if (mesh_archive_view->job) {
        cerberus_ble_archive_io_cancel(mesh_archive_view->job);
        cerberus_ble_archive_io_release(mesh_archive_view->job);
    }
    heap_caps_free(mesh_archive_view);
    mesh_archive_view = NULL;
}

static void mesh_archive_files(void)
{
    mesh_archive_dispose();
    current_page = UI_PAGE_FILES;
    file_mode = UI_FILE_MODE_LIST;
    snprintf(file_path, sizeof(file_path), "%s", CERBERUS_STORAGE_MESH_ARCHIVE_PATH);
    file_page_offset = 0U;
    (void)refresh_file_listing(false);
}

static void mesh_archive_back(void)
{
    if (mesh_archive_view && mesh_archive_view->detail && !mesh_archive_view->job) {
        mesh_archive_view->detail = false;
        mesh_archive_view->painted_count = 0U;
    } else {
        mesh_archive_dispose();
        current_page = UI_PAGE_FILES;
        file_mode = UI_FILE_MODE_LIST;
        (void)refresh_file_listing(false);
    }
}

static void mesh_archive_open_file(const char *path, const char *name)
{
    if (!path || !mesh_archive_is_file(name)) return;
    if (mesh_archive_view && mesh_archive_view->job) return;
    current_page = UI_PAGE_MESH_ARCHIVE;
    if (!mesh_archive_view)
        mesh_archive_view = heap_caps_calloc(1U, sizeof(*mesh_archive_view),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    mesh_archive_open_error = mesh_archive_view ? ESP_OK : ESP_ERR_NO_MEM;
    mesh_archive_ui_store_t *s = mesh_archive_view;
    if (s) {
        s->painted_count = 0U;
        s->detail = s->exporting = false;
        snprintf(s->pending_name, sizeof(s->pending_name), "%s", name);
        s->error = cerberus_ble_archive_io_load_mesh_archive(path, name, &s->job);
        if (s->error == ESP_OK && !s->job) s->error = ESP_ERR_INVALID_RESPONSE;
    }
    /* Retain the original Files directory, including OPEN MAP's direct path. */
    size_t n = strnlen(path, sizeof(file_path));
    if (n < sizeof(file_path)) memmove(file_path, path, n + 1U);
    ESP_ERROR_CHECK(draw_detail_page());
}

static void mesh_archive_tick(bool asleep)
{
    if (!mesh_archive_view) return;
    if (current_page != UI_PAGE_MESH_ARCHIVE || asleep) {
        mesh_archive_dispose();
        if (asleep && current_page == UI_PAGE_MESH_ARCHIVE) {
            current_page = UI_PAGE_FILES;
            file_mode = UI_FILE_MODE_LIST;
        }
        return;
    }
    mesh_archive_ui_store_t *s = mesh_archive_view;
    if (!s->job) return;
    cerberus_ble_archive_io_status_t status;
    cerberus_ble_archive_io_get_status(&status);
    if (status.job_id == s->job && status.worker_active) return;
    s->error = status.job_id == s->job ? status.error : ESP_ERR_NOT_FOUND;
    s->painted_count = 0U;
    if (status.job_id == s->job && status.state == CERBERUS_BLE_ARCHIVE_IO_COMPLETE &&
        status.error == ESP_OK) {
        if (s->exporting && status.operation == CERBERUS_BLE_ARCHIVE_IO_EXPORT_MESH_ARCHIVE_CSV &&
            status.format == CERBERUS_BLE_ARCHIVE_IO_FORMAT_MESH_REPORT) {
            snprintf(s->csv_name, sizeof(s->csv_name), "%s", status.file_name);
        } else if (!s->exporting && status.operation == CERBERUS_BLE_ARCHIVE_IO_LOAD_MESH_ARCHIVE &&
            status.format == CERBERUS_BLE_ARCHIVE_IO_FORMAT_MESH_ARCHIVE && status.result_ready) {
            s->error = cerberus_ble_archive_io_copy_mesh_archive(s->job, &s->document);
            if (s->error == ESP_OK) {
                s->has_document = true;
                s->group = s->page = s->raw_page = 0U;
                s->detail = false;
                s->csv_name[0] = '\0';
                snprintf(s->name, sizeof(s->name), "%s", s->pending_name);
            }
        } else s->error = ESP_ERR_INVALID_RESPONSE;
    } else if (s->error == ESP_OK) s->error = ESP_ERR_INVALID_RESPONSE;
    cerberus_ble_archive_io_release(s->job);
    s->job = 0U;
    ESP_ERROR_CHECK(draw_detail_page());
}

static esp_err_t mesh_archive_draw(void)
{
    mesh_archive_ui_store_t *s = mesh_archive_view;
    if (s) s->painted_count = 0U;
    ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(0, 0, 320, 480, UI_BACKGROUND), TAG, "map clear");
    ESP_RETURN_ON_ERROR(draw_header("SAVED MESH MAP"), TAG, "map title");
    ESP_RETURN_ON_ERROR(draw_button(8, 430, 304, 42,
        s && s->detail && !s->job ? "BACK TO MAP" : "BACK TO FILES"), TAG, "map back");
    if (!s) return draw_centered(182, mesh_archive_open_error == ESP_ERR_NO_MEM ?
        "NO PSRAM / BACK TO FILES" : "MAP UNAVAILABLE", UI_BAD, 1);
    if (s->detail) return mesh_ui_detail_value(&s->selected, s->raw_page, s->error, true, false);
    char line[64];
    if (s->has_document && s->document.final_summary.error_code != ESP_OK)
        snprintf(line, sizeof(line), "HISTORICAL / SOURCE ERROR %ld",
            (long)s->document.final_summary.error_code);
    else snprintf(line, sizeof(line), "HISTORICAL / NO LIVE RADIO CONTROL");
    ESP_RETURN_ON_ERROR(draw_centered(52, line,
        s->has_document && s->document.final_summary.error_code ? UI_BAD : UI_MUTED, 1), TAG, "map scope");
    snprintf(line, sizeof(line), "%.48s", s->has_document ? s->name : s->pending_name);
    for (char *p = line; *p; ++p) if ((unsigned char)*p < 32U || (unsigned char)*p > 126U) *p = '?';
    ESP_RETURN_ON_ERROR(draw_centered(70, line, UI_TEXT, 1), TAG, "map filename");
    const mesh_observation_t *m = &s->document.model;
    bool clock_fault = (s->document.final_summary.flags & CERBERUS_MESH_OBSERVATION_SUMMARY_CLOCK_INVALID) != 0U;
    bool partial = s->document.final_summary.tuned_mask != s->document.final_summary.requested_mask;
    snprintf(line, sizeof(line), "%s%sRAW FIRST %u/64", clock_fault ? "C5 CLOCK FAULT / " : "",
        partial ? "PARTIAL CHANNELS / " : "", s->has_document ? m->witness_count : 0U);
    ESP_RETURN_ON_ERROR(draw_centered(88, line, UI_MUTED, 1), TAG, "map raw limits");
    const char *tabs[] = {"PAN CONTEXTS", "ADDRESSES", "TRAFFIC"};
    for (unsigned i = 0U; i < 3U; ++i) {
        uint16_t x = (uint16_t)(8U + i * 104U);
        ESP_RETURN_ON_ERROR(mesh_ui_button(x, 108, 96, 30, tabs[i], s->has_document && !s->job), TAG, "map tab");
        if (s->group == i) ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(x + 2U, 135, 92, 2, UI_GOOD), TAG, "map underline");
    }
    s->count = !s->has_document ? 0U : s->group == 0U ? m->pan_count :
        s->group == 1U ? m->address_count : m->edge_count;
    if (s->page * MESH_UI_ROWS >= s->count) s->page = 0U;
    uint8_t painted = 0U;
    if (s->has_document)
        ESP_RETURN_ON_ERROR(mesh_ui_rows_draw(m, s->group, s->page, s->count,
            s->painted, &painted), TAG, "map rows");
    if (!s->count) ESP_RETURN_ON_ERROR(draw_centered(220, s->job ? "LOADING MAP" :
        "NO RETAINED OBSERVATIONS", UI_MUTED, 1), TAG, "map empty");
    snprintf(line, sizeof(line), "PAN %u/16 / ADDR %u/64 / TRAFFIC %u/64", m->pan_count, m->address_count, m->edge_count);
    ESP_RETURN_ON_ERROR(draw_centered(320, line, UI_MUTED, 1), TAG, "map capacity");
    bool source_saturated = (s->document.final_summary.flags & CERBERUS_MESH_OBSERVATION_COUNTERS_SATURATED) != 0U;
    snprintf(line, sizeof(line), source_saturated ? "C5 >= DROP %lu / INVALID %lu" : "C5 QUEUE DROP %lu / INVALID %lu",
        (unsigned long)s->document.final_summary.queue_drops,
        (unsigned long)s->document.final_summary.invalid_callbacks);
    ESP_RETURN_ON_ERROR(draw_centered(334, line, UI_MUTED, 1), TAG, "map loss");
    snprintf(line, sizeof(line), "OMIT P%lu A%lu E%lu", (unsigned long)m->counters.pan_omissions,
        (unsigned long)m->counters.address_omissions, (unsigned long)m->counters.edge_omissions);
    ESP_RETURN_ON_ERROR(draw_centered(348, line, UI_MUTED, 1), TAG, "map omissions");
    if (s->error != ESP_OK) snprintf(line, sizeof(line), "%s: %.32s", s->has_document ? "KEPT MAP" : "LOAD ERROR", esp_err_to_name(s->error));
    else if (s->job) snprintf(line, sizeof(line), "%s / BACK CANCELS", s->exporting ? "EXPORTING CSV" : "LOADING MAP");
    else if (m->counters.saturated || source_saturated) snprintf(line, sizeof(line), "COUNTERS SATURATED / VALUES ARE BOUNDS");
    else snprintf(line, sizeof(line), "RAW OMIT %lu / PARSE %lu+%lu", (unsigned long)m->counters.witness_omissions,
        (unsigned long)m->counters.malformed, (unsigned long)m->counters.unsupported);
    ESP_RETURN_ON_ERROR(draw_centered(362, line, s->error ? UI_BAD : UI_MUTED, 1), TAG, "map message");
    snprintf(line, sizeof(line), "PAGE %u / %u", s->page + 1U, s->count ? (s->count + MESH_UI_ROWS - 1U) / MESH_UI_ROWS : 1U);
    ESP_RETURN_ON_ERROR(draw_centered(374, line, UI_MUTED, 1), TAG, "map page");
    ESP_RETURN_ON_ERROR(mesh_ui_button(8, 382, 56, 36, "PREV", !s->job && s->page > 0U), TAG, "map prev");
    ESP_RETURN_ON_ERROR(mesh_ui_button(72, 382, 56, 36, "NEXT", !s->job && (s->page + 1U) * MESH_UI_ROWS < s->count), TAG, "map next");
    ESP_RETURN_ON_ERROR(mesh_ui_button(136, 382, 176, 36, s->job ? "WORKING" : s->csv_name[0] ? "OPEN CSV" : "EXPORT CSV",
        s->has_document && !s->job), TAG, "map export");
    if (!s->job) s->painted_count = painted;
    return ESP_OK;
}

static void mesh_archive_tap(uint16_t x, uint16_t y)
{
    mesh_archive_ui_store_t *s = mesh_archive_view;
    if (x >= 8U && x < 312U && y >= 430U && y < 472U) {
        mesh_archive_back(); ESP_ERROR_CHECK(draw_detail_page()); return;
    }
    if (!s || s->job || !s->has_document) return;
    if (s->detail) {
        if (x >= 80U && x < 240U && y >= 376U && y < 412U &&
            s->selected.value.witness_valid && s->selected.value.witness.input.length > 64U &&
            !privacy_addresses_masked()) s->raw_page ^= 1U;
    } else if (y >= 108U && y < 138U) {
        for (unsigned i = 0U; i < 3U; ++i) if (x >= 8U + i * 104U && x < 104U + i * 104U) {
            s->group = (uint8_t)i; s->page = s->painted_count = 0U;
        }
    } else if (x >= 8U && x < 312U && y >= 145U && y < 313U) {
        unsigned slot = (y - 145U) / 42U;
        if (slot < s->painted_count && (y - 145U) % 42U < 39U) {
            s->selected = s->painted[slot]; s->detail = true;
            s->raw_page = s->painted_count = 0U;
        }
    } else if (y >= 382U && y < 418U) {
        if (x >= 8U && x < 64U && s->page) { --s->page; s->painted_count = 0U; }
        else if (x >= 72U && x < 128U && (s->page + 1U) * MESH_UI_ROWS < s->count) { ++s->page; s->painted_count = 0U; }
        else if (x >= 136U && x < 312U) {
            if (s->csv_name[0]) {
                char name[CERBERUS_STORAGE_NAME_MAX]; memcpy(name, s->csv_name, sizeof(name));
                mesh_archive_dispose(); current_page = UI_PAGE_FILES;
                snprintf(file_path, sizeof(file_path), "%s", CERBERUS_STORAGE_MESH_REPORT_PATH);
                open_text_file(name);
                return;
            }
            s->error = cerberus_ble_archive_io_export_mesh_archive_csv(&s->document, &s->job);
            if (s->error == ESP_OK && !s->job) s->error = ESP_ERR_INVALID_RESPONSE;
            s->exporting = true; s->painted_count = 0U;
        }
    }
    ESP_ERROR_CHECK(draw_detail_page());
}
