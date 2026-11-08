#include "ssid_list_session.h"
#include "ble_archive_io.h"
#include "esp_heap_caps.h"
#include "freertos/semphr.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks, live, allocations, frees, starts, copies, cancels, releases;
static int mutex_token, locked;
static bool fail_mutex = true, fail_lock, fail_alloc, fail_copy, fail_start, assigned_failure;
static bool held, release_requested, early_complete, finish_on_release;
static unsigned lost_status, ignore_release;
static uint64_t next_job = 100U, last_canceled, last_released;
static size_t largest_allocation;
static cerberus_ble_archive_io_status_t io;
static cerberus_wifi_marauder_ssid_t loaded, saved;
#define CHECK(expression) do { ++checks; assert(expression); } while (0)

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return fail_mutex ? NULL : &mutex_token; }
int xSemaphoreTake(SemaphoreHandle_t mutex, uint32_t ticks)
{
    CHECK(mutex == &mutex_token && !locked && ticks == 250U);
    if (fail_lock) return 0;
    locked = 1; return 1;
}
int xSemaphoreGive(SemaphoreHandle_t mutex)
{ CHECK(mutex == &mutex_token && locked); locked = 0; return 1; }
void *heap_caps_calloc(size_t count, size_t size, unsigned caps)
{
    CHECK(locked && caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (fail_alloc) return NULL;
    void *p = calloc(count, size); CHECK(p != NULL);
    ++live; ++allocations; CHECK(live <= 2U);
    if (count * size > largest_allocation) largest_allocation = count * size;
    return p;
}
void heap_caps_free(void *pointer)
{ if (pointer) { CHECK(locked && live > 1U); --live; ++frees; free(pointer); } }

static void finish(bool success)
{
    CHECK(io.worker_active);
    io.worker_active = false;
    io.state = success ? CERBERUS_BLE_ARCHIVE_IO_COMPLETE : CERBERUS_BLE_ARCHIVE_IO_ERROR;
    io.error = success ? ESP_OK : ESP_FAIL;
    io.result_ready = success && !release_requested;
    held = io.result_ready;
    if (io.operation == CERBERUS_BLE_ARCHIVE_IO_SAVE_SSID_TEXT)
        strcpy(io.file_name, success ? "S00101.TXT" : "S00101.TMP");
}
static esp_err_t begin(cerberus_ble_archive_io_operation_t operation, uint64_t *job)
{
    CHECK(locked && job != NULL); *job = 0U; ++starts;
    if (held || io.worker_active) return ESP_ERR_INVALID_STATE;
    if (fail_start) return ESP_ERR_NO_MEM;
    io = (cerberus_ble_archive_io_status_t){.job_id = ++next_job, .operation = operation,
        .format = CERBERUS_BLE_ARCHIVE_IO_FORMAT_SSID_TEXT, .state = CERBERUS_BLE_ARCHIVE_IO_STARTING,
        .worker_active = true};
    held = true; release_requested = false; *job = io.job_id;
    if (assigned_failure) { finish(false); io.error = ESP_ERR_NO_MEM; return ESP_ERR_NO_MEM; }
    if (early_complete) finish(true);
    return ESP_OK;
}
esp_err_t cerberus_ble_archive_io_load_ssid_text(const char *path, const char *name, uint64_t *job)
{ CHECK(!strcmp(path, "WIFI") && !strcmp(name, "source.txt")); return begin(CERBERUS_BLE_ARCHIVE_IO_LOAD_SSID_TEXT, job); }
esp_err_t cerberus_ble_archive_io_save_ssid_text(const cerberus_wifi_marauder_ssid_t *document, uint64_t *job)
{
    CHECK(cerberus_wifi_marauder_ssid_validate(document) == CERBERUS_WIFI_MARAUDER_OK);
    saved = *document; return begin(CERBERUS_BLE_ARCHIVE_IO_SAVE_SSID_TEXT, job);
}
void cerberus_ble_archive_io_get_status(cerberus_ble_archive_io_status_t *out)
{ CHECK(locked); if (lost_status) { --lost_status; memset(out, 0, sizeof(*out)); } else *out = io; }
esp_err_t cerberus_ble_archive_io_copy_ssid_text(uint64_t job, cerberus_wifi_marauder_ssid_t *out)
{
    ++copies; CHECK(locked && job == io.job_id && !io.worker_active && held && io.result_ready);
    if (fail_copy) return ESP_ERR_TIMEOUT;
    *out = loaded; return ESP_OK;
}
void cerberus_ble_archive_io_cancel(uint64_t job)
{
    CHECK(locked && job); ++cancels; last_canceled = job;
    if (job == io.job_id && io.worker_active) io.cancel_requested = true;
}
void cerberus_ble_archive_io_release(uint64_t job)
{
    CHECK(locked && job); ++releases; last_released = job;
    if (ignore_release) { --ignore_release; return; }
    if (job != io.job_id) return;
    if (io.worker_active) {
        release_requested = true; io.cancel_requested = true;
        if (finish_on_release) { finish_on_release = false; finish(true); }
    }
    else { held = false; io.result_ready = false; }
}

static cerberus_ssid_list_session_status_t status(void)
{
    cerberus_ssid_list_session_status_t out;
    CHECK(cerberus_ssid_list_session_poll(&out) == ESP_OK); return out;
}
static cerberus_ssid_list_t snapshot(void)
{
    cerberus_ssid_list_t out;
    CHECK(cerberus_ssid_list_session_copy(&out) == ESP_OK);
    CHECK(cerberus_ssid_list_valid(&out)); return out;
}
static void same(const cerberus_ssid_list_t *before)
{ cerberus_ssid_list_t after = snapshot(); CHECK(!memcmp(before, &after, sizeof(after))); }
static void document(cerberus_wifi_marauder_ssid_t *out, const char *text)
{ CHECK(cerberus_wifi_marauder_ssid_decode((const uint8_t *)text, strlen(text), out) == CERBERUS_WIFI_MARAUDER_OK); }
static void add(const char *name)
{
    cerberus_ssid_list_t current = snapshot();
    cerberus_ssid_list_edit_t edit = {.kind = CERBERUS_SSID_EDIT_ADD, .bytes = (const uint8_t *)name, .length = strlen(name)};
    CHECK(cerberus_ssid_list_session_edit(current.revision, &edit) == ESP_OK);
}
static void apply(bool append, bool unique)
{
    cerberus_ssid_list_session_status_t current = status();
    CHECK(current.preview_ready && !current.busy);
    CHECK(cerberus_ssid_list_session_apply_preview(current.preview_id, current.revision, append, unique) == ESP_OK);
}
static void load(void)
{ CHECK(cerberus_ssid_list_session_load("WIFI", "source.txt") == ESP_OK); CHECK(status().busy); }

int main(void)
{
    CHECK(!status().allocated);
    CHECK(cerberus_ssid_list_session_open() == ESP_ERR_NO_MEM);
    fail_mutex = false; fail_alloc = true;
    CHECK(cerberus_ssid_list_session_open() == ESP_ERR_NO_MEM); CHECK(!status().allocated);
    fail_alloc = false; CHECK(cerberus_ssid_list_session_open() == ESP_OK);
    CHECK(live == 1U && snapshot().revision == 1U && !snapshot().dirty);
    unsigned allocated_once = allocations;
    CHECK(cerberus_ssid_list_session_open() == ESP_OK && allocations == allocated_once);

    add("first"); cerberus_ssid_list_t before = snapshot();
    cerberus_ssid_list_edit_t edit = {.kind = CERBERUS_SSID_EDIT_REMOVE, .index = 0U};
    CHECK(cerberus_ssid_list_session_edit(before.revision - 1U, &edit) == ESP_ERR_INVALID_STATE); same(&before);
    edit.kind = CERBERUS_SSID_EDIT_NAME; edit.bytes = (const uint8_t *)"bad\nname"; edit.length = 8U;
    CHECK(cerberus_ssid_list_session_edit(before.revision, &edit) == ESP_ERR_NOT_SUPPORTED); same(&before);
    edit.kind = CERBERUS_SSID_EDIT_SELECT; edit.selected = true;
    CHECK(cerberus_ssid_list_session_edit(before.revision, &edit) == ESP_OK);
    CHECK(snapshot().selected == 1U && snapshot().dirty);

    document(&loaded, "second\nsecond\n"); before = snapshot();
    fail_alloc = true; CHECK(cerberus_ssid_list_session_prepare_document(&loaded) == ESP_ERR_NO_MEM); same(&before);
    fail_alloc = false; CHECK(cerberus_ssid_list_session_prepare_document(&loaded) == ESP_OK);
    cerberus_wifi_marauder_ssid_t preview; uint64_t preview_id = 0U;
    CHECK(cerberus_ssid_list_session_copy_preview(&preview, &preview_id) == ESP_OK);
    CHECK(preview_id && !memcmp(&preview, &loaded, sizeof(preview))); same(&before);
    loaded.rows[0].bytes[0] = 'X';
    CHECK(cerberus_ssid_list_session_copy_preview(&preview, &preview_id) == ESP_OK && preview.rows[0].bytes[0] == 's');
    CHECK(cerberus_ssid_list_session_apply_preview(preview_id + 1U, before.revision, false, false) == ESP_ERR_INVALID_STATE);
    CHECK(cerberus_ssid_list_session_apply_preview(preview_id, before.revision - 1U, false, false) == ESP_ERR_INVALID_STATE); same(&before);
    apply(true, true); CHECK(snapshot().document.count == 2U && snapshot().selected == 1U && live == 1U);
    CHECK(cerberus_ssid_list_session_apply_preview(preview_id, snapshot().revision, false, false) == ESP_ERR_INVALID_STATE);

    before = snapshot(); CHECK(cerberus_ssid_list_session_save(before.revision) == ESP_OK);
    CHECK(status().busy && !memcmp(&saved, &before.document, sizeof(saved)));
    add("later"); CHECK(saved.count == 2U && snapshot().document.count == 3U);
    finish(true); cerberus_ssid_list_session_status_t state = status();
    CHECK(!state.busy && state.state == CERBERUS_SSID_SESSION_COMPLETE && state.dirty && state.saved_revision == before.revision);
    CHECK(!strcmp(state.file_name, "S00101.TXT"));
    before = snapshot(); CHECK(cerberus_ssid_list_session_save(before.revision) == ESP_OK);
    finish(true); state = status(); CHECK(!state.busy && !state.dirty && state.revision > before.revision);
    cerberus_ssid_list_t clean = snapshot(); (void)status(); same(&clean);

    add("unsaved"); before = snapshot(); CHECK(cerberus_ssid_list_session_save(before.revision) == ESP_OK);
    finish(false); state = status(); CHECK(!state.busy && state.error == ESP_FAIL && state.dirty); same(&before);
    fail_start = true; CHECK(cerberus_ssid_list_session_save(before.revision) == ESP_ERR_NO_MEM);
    CHECK(!status().busy); same(&before); fail_start = false;
    assigned_failure = true; CHECK(cerberus_ssid_list_session_save(before.revision) == ESP_ERR_NO_MEM);
    CHECK(!status().busy && !held); same(&before); assigned_failure = false;

    unsigned starts_before = starts;
    CHECK(cerberus_ssid_list_session_save(before.revision - 1U) == ESP_ERR_INVALID_STATE && starts == starts_before);
    fail_start = true; CHECK(cerberus_ssid_list_session_load("WIFI", "source.txt") == ESP_ERR_NO_MEM);
    CHECK(!status().busy && live == 1U); same(&before); fail_start = false;
    assigned_failure = true; CHECK(cerberus_ssid_list_session_load("WIFI", "source.txt") == ESP_ERR_NO_MEM);
    CHECK(!status().busy && live == 1U); same(&before); assigned_failure = false;

    document(&loaded, "loaded\n"); load(); before = snapshot();
    CHECK(cerberus_ssid_list_session_copy_preview(&preview, &preview_id) == ESP_ERR_INVALID_STATE); same(&before);
    finish(true); ignore_release = 1U; state = status();
    CHECK(state.busy && state.preview_ready && held);
    CHECK(cerberus_ssid_list_session_apply_preview(state.preview_id, state.revision, false, false) == ESP_ERR_INVALID_STATE);
    unsigned copied_once = copies; state = status(); CHECK(!state.busy && copies == copied_once);
    apply(false, false); CHECK(snapshot().document.count == 1U && snapshot().selected == 0U);

    load(); before = snapshot(); finish(true); fail_copy = true; state = status();
    CHECK(!state.busy && !state.preview_ready && state.error == ESP_ERR_TIMEOUT && live == 1U); same(&before); fail_copy = false;
    load(); finish(true); loaded.rows[0].length = 33U; state = status();
    CHECK(!state.preview_ready && !state.busy && state.error == ESP_ERR_INVALID_RESPONSE); same(&before);
    document(&loaded, "ok\n"); load(); finish(false); CHECK(status().error == ESP_FAIL); same(&before);

    /* Contradictory same-job metadata cannot become an import. The exact job
     * still needs cleanup, even after its typed result has been rejected. */
    load(); uint64_t malformed_job = status().job_id;
    io.format = CERBERUS_BLE_ARCHIVE_IO_FORMAT_BLE; state = status();
    CHECK(state.busy && state.error == ESP_ERR_INVALID_RESPONSE && !state.preview_ready);
    CHECK(last_canceled == malformed_job && last_released == malformed_job);
    finish(true); state = status(); CHECK(!state.busy && !state.preview_ready); same(&before);
    load(); io.worker_active = false; io.result_ready = false; held = false;
    io.state = CERBERUS_BLE_ARCHIVE_IO_CANCELED; state = status();
    CHECK(!state.busy && state.state == CERBERUS_SSID_SESSION_CANCELED); same(&before);

    /* Newer ownership is never canceled or released by this service. */
    load(); uint64_t owned = status().job_id; unsigned old_cancels = cancels, old_releases = releases;
    io.job_id = ++next_job; io.format = CERBERUS_BLE_ARCHIVE_IO_FORMAT_BLE;
    state = status(); CHECK(!state.busy && state.error == ESP_ERR_INVALID_STATE);
    CHECK(cancels == old_cancels && releases == old_releases); same(&before);
    CHECK(cerberus_ssid_list_session_save(before.revision) == ESP_ERR_INVALID_STATE);
    io.worker_active = false; io.result_ready = false; held = false;

    /* A lost status cannot prove cleanup; close still uses the exact token. */
    load(); owned = status().job_id; lost_status = 1U; state = status();
    CHECK(state.busy && state.job_id == owned && state.error == ESP_ERR_INVALID_RESPONSE);
    fail_lock = true; cerberus_ssid_list_session_close(); fail_lock = false;
    state = status(); CHECK(state.busy && state.closing && io.cancel_requested && last_canceled == owned && last_released == owned);
    CHECK(cerberus_ssid_list_session_open() == ESP_ERR_INVALID_STATE);
    finish(true); state = status(); CHECK(!state.busy && !state.preview_ready && live == 1U); same(&before);
    CHECK(cerberus_ssid_list_session_open() == ESP_OK && !status().closing); same(&before);

    /* Cancellation cannot erase a successful in-flight Save acknowledgement. */
    CHECK(cerberus_ssid_list_session_save(before.revision) == ESP_OK); owned = status().job_id;
    cerberus_ssid_list_session_close(); CHECK(status().busy && last_released == owned);
    finish(true); state = status(); CHECK(!state.busy && state.state == CERBERUS_SSID_SESSION_COMPLETE && !state.dirty);
    CHECK(cerberus_ssid_list_session_open() == ESP_OK);

    add("release race"); before = snapshot();
    CHECK(cerberus_ssid_list_session_save(before.revision) == ESP_OK);
    finish_on_release = true; cerberus_ssid_list_session_close();
    state = status(); CHECK(!state.busy && state.state == CERBERUS_SSID_SESSION_COMPLETE && !state.dirty);
    CHECK(state.saved_revision == before.revision);
    CHECK(cerberus_ssid_list_session_open() == ESP_OK);

    /* Explicit preview discard and Back preserve all working bytes. */
    before = snapshot(); CHECK(cerberus_ssid_list_session_prepare_document(&loaded) == ESP_OK);
    preview_id = status().preview_id; cerberus_ssid_list_session_discard_preview(); CHECK(!status().preview_ready); same(&before);
    CHECK(cerberus_ssid_list_session_prepare_document(&loaded) == ESP_OK && status().preview_id > preview_id);
    cerberus_ssid_list_session_close(); CHECK(!status().busy && !status().preview_ready && live == 1U);
    CHECK(status().state == CERBERUS_SSID_SESSION_CANCELED); same(&before);
    CHECK(cerberus_ssid_list_session_open() == ESP_OK); same(&before);

    /* Capacity failure is atomic and keeps the pending preview reviewable. */
    cerberus_wifi_marauder_ssid_t full = {0}; full.count = 64U; full.final_lf = true;
    for (unsigned i = 0; i < 64U; ++i) { full.rows[i].length = 1U; full.rows[i].bytes[0] = (uint8_t)(32U + i); }
    CHECK(cerberus_ssid_list_session_prepare_document(&full) == ESP_OK); apply(false, false); before = snapshot();
    CHECK(cerberus_ssid_list_session_prepare_document(&loaded) == ESP_OK); state = status();
    CHECK(cerberus_ssid_list_session_apply_preview(state.preview_id, state.revision, true, false) == ESP_ERR_INVALID_SIZE);
    CHECK(status().preview_ready); same(&before); cerberus_ssid_list_session_discard_preview();

    /* Real model commands remain revision guarded through the session. */
    edit = (cerberus_ssid_list_edit_t){.kind = CERBERUS_SSID_EDIT_SELECT_ALL, .selected = true};
    CHECK(cerberus_ssid_list_session_edit(before.revision, &edit) == ESP_OK && snapshot().selected == UINT64_MAX);
    edit.kind = CERBERUS_SSID_EDIT_REMOVE_SELECTED;
    CHECK(cerberus_ssid_list_session_edit(snapshot().revision, &edit) == ESP_OK && snapshot().document.count == 0U);
    const uint8_t entropy[8] = {1,2,3,4,5,6,7,8};
    edit = (cerberus_ssid_list_edit_t){.kind = CERBERUS_SSID_EDIT_GENERATE, .bytes = entropy, .length = sizeof(entropy), .count = 2U};
    CHECK(cerberus_ssid_list_session_edit(snapshot().revision, &edit) == ESP_OK && snapshot().document.count == 2U);
    early_complete = true; before = snapshot(); CHECK(cerberus_ssid_list_session_save(before.revision) == ESP_OK);
    CHECK(!status().busy && !snapshot().dirty); early_complete = false;

    fail_lock = true; memset(&state, 0x5a, sizeof(state));
    CHECK(cerberus_ssid_list_session_poll(&state) == ESP_ERR_TIMEOUT);
    CHECK(!state.allocated && !state.busy && !state.revision); fail_lock = false;
    CHECK(live == 1U && allocations == frees + 1U && !locked && !held);
    printf("SSID session: %u checks; persistent workspace %zu bytes; exact IO ownership verified.\n", checks, largest_allocation);
    return 0;
}
