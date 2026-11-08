#include "ssid_list_session.h"
#include "ble_archive_io.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdatomic.h>
#include <string.h>

typedef struct {
    cerberus_ssid_list_t list;
    cerberus_wifi_marauder_ssid_t *preview;
    cerberus_ssid_list_session_status_t status;
    uint64_t save_revision;
    bool saving, handled;
} ssid_list_store_t;

static SemaphoreHandle_t list_mutex;
static ssid_list_store_t *list_store;
static uint64_t last_preview_id;
static atomic_bool close_requested;

static esp_err_t take_list(void)
{
    if (!list_mutex) return ESP_ERR_INVALID_STATE;
    return xSemaphoreTake(list_mutex, pdMS_TO_TICKS(250)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

static esp_err_t model_error(cerberus_ssid_list_result_t result)
{
    return result == CERBERUS_SSID_LIST_OK ? ESP_OK :
        result == CERBERUS_SSID_LIST_LIMIT ? ESP_ERR_INVALID_SIZE :
        result == CERBERUS_SSID_LIST_UNREPRESENTABLE ? ESP_ERR_NOT_SUPPORTED :
        result == CERBERUS_SSID_LIST_STALE || result == CERBERUS_SSID_LIST_EXHAUSTED ?
        ESP_ERR_INVALID_STATE : ESP_ERR_INVALID_ARG;
}

static void discard_preview(ssid_list_store_t *s)
{
    heap_caps_free(s->preview); s->preview = NULL;
    s->status.preview_id = 0U; s->status.preview_count = 0U; s->status.preview_ready = false;
}

static void fail_job(ssid_list_store_t *s, esp_err_t error)
{
    s->status.error = error; s->status.state = CERBERUS_SSID_SESSION_ERROR;
    discard_preview(s); s->handled = true;
}

static bool terminal(cerberus_ble_archive_io_state_t state)
{
    return state == CERBERUS_BLE_ARCHIVE_IO_COMPLETE || state == CERBERUS_BLE_ARCHIVE_IO_ERROR ||
        state == CERBERUS_BLE_ARCHIVE_IO_CANCELED;
}

static void poll_locked(ssid_list_store_t *s)
{
    bool closing = atomic_load_explicit(&close_requested, memory_order_acquire);
    s->status.closing = closing;
    if (closing) {
        discard_preview(s);
        if (s->status.state == CERBERUS_SSID_SESSION_PREVIEW)
            s->status.state = CERBERUS_SSID_SESSION_CANCELED;
    }
    uint64_t job = s->status.job_id;
    if (!job) return;
    cerberus_ble_archive_io_status_t io;
    cerberus_ble_archive_io_get_status(&io);
    if (closing) {
        cerberus_ble_archive_io_cancel(job);
        cerberus_ble_archive_io_release(job);
    }
    if (io.job_id != job) {
        /* The serialized worker cannot assign a newer ID before releasing its
         * old context. An older/empty status supplies no such evidence. */
        s->status.error = ESP_ERR_INVALID_RESPONSE;
        if (io.job_id > job) {
            fail_job(s, ESP_ERR_INVALID_STATE); s->status.job_id = 0U;
        }
        return;
    }
    cerberus_ble_archive_io_operation_t expected = s->saving ?
        CERBERUS_BLE_ARCHIVE_IO_SAVE_SSID_TEXT : CERBERUS_BLE_ARCHIVE_IO_LOAD_SSID_TEXT;
    if (io.operation != expected || io.format != CERBERUS_BLE_ARCHIVE_IO_FORMAT_SSID_TEXT) {
        fail_job(s, ESP_ERR_INVALID_RESPONSE);
        cerberus_ble_archive_io_cancel(job);
        cerberus_ble_archive_io_release(job);
    } else if (!io.worker_active && !s->handled) {
        s->handled = true;
        if (!terminal(io.state)) fail_job(s, ESP_ERR_INVALID_RESPONSE);
        else if (io.state == CERBERUS_BLE_ARCHIVE_IO_COMPLETE && io.error == ESP_OK) {
            if (s->saving) {
                if (!memchr(io.file_name, '\0', sizeof(io.file_name)) || !io.file_name[0])
                    fail_job(s, ESP_ERR_INVALID_RESPONSE);
                else {
                    memcpy(s->status.file_name, io.file_name, sizeof(s->status.file_name));
                    s->status.saved_revision = s->save_revision;
                    /* A stale revision means the frozen copy was saved, while
                     * newer edits remain dirty. It is not a failed file save. */
                    (void)cerberus_ssid_list_saved(&s->list, s->save_revision);
                    s->status.error = ESP_OK; s->status.state = CERBERUS_SSID_SESSION_COMPLETE;
                }
            } else if (closing) {
                s->status.error = ESP_OK; s->status.state = CERBERUS_SSID_SESSION_CANCELED;
            } else if (!io.result_ready || !s->preview || last_preview_id == UINT64_MAX) {
                fail_job(s, ESP_ERR_INVALID_STATE);
            } else {
                esp_err_t error = cerberus_ble_archive_io_copy_ssid_text(job, s->preview);
                if (error == ESP_OK && cerberus_wifi_marauder_ssid_validate(s->preview) != CERBERUS_WIFI_MARAUDER_OK)
                    error = ESP_ERR_INVALID_RESPONSE;
                if (error != ESP_OK) fail_job(s, error);
                else {
                    s->status.preview_id = ++last_preview_id;
                    s->status.preview_count = s->preview->count; s->status.preview_ready = true;
                    s->status.error = ESP_OK; s->status.state = CERBERUS_SSID_SESSION_PREVIEW;
                }
            }
        } else if (io.state == CERBERUS_BLE_ARCHIVE_IO_CANCELED) {
            discard_preview(s); s->status.error = ESP_OK; s->status.state = CERBERUS_SSID_SESSION_CANCELED;
        } else fail_job(s, io.error == ESP_OK ? ESP_ERR_INVALID_RESPONSE : io.error);
    }
    if (!io.worker_active || s->handled || closing) {
        cerberus_ble_archive_io_release(job);
        cerberus_ble_archive_io_get_status(&io);
        if (io.job_id > job) {
            if (!s->handled) fail_job(s, ESP_ERR_INVALID_STATE);
            s->status.job_id = 0U;
        } else if (s->handled && io.job_id == job && !io.worker_active && !io.result_ready)
            s->status.job_id = 0U;
        /* Completion can race this release readback. Keep the identity until
         * the next poll handles that terminal outcome, even if the worker has
         * already disposed its copied buffer. */
    }
}

static void snapshot(ssid_list_store_t *s, cerberus_ssid_list_session_status_t *out)
{
    s->status.revision = s->list.revision; s->status.selected = s->list.selected;
    s->status.count = s->list.document.count; s->status.dirty = s->list.dirty;
    s->status.allocated = true; s->status.busy = s->status.job_id != 0U;
    if (out) *out = s->status;
}

esp_err_t cerberus_ssid_list_session_open(void)
{
    /* The UI owns initialization; worker callbacks never enter this service. */
    if (!list_mutex) list_mutex = xSemaphoreCreateMutex();
    if (!list_mutex) return ESP_ERR_NO_MEM;
    esp_err_t error = take_list();
    if (error != ESP_OK) return error;
    if (!list_store) {
        list_store = heap_caps_calloc(1U, sizeof(*list_store), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (list_store) cerberus_ssid_list_init(&list_store->list);
        else error = ESP_ERR_NO_MEM;
    }
    if (list_store) {
        poll_locked(list_store);
        if (list_store->status.job_id) error = ESP_ERR_INVALID_STATE;
        else {
            atomic_store_explicit(&close_requested, false, memory_order_release);
            list_store->status.closing = false;
        }
    }
    xSemaphoreGive(list_mutex); return error;
}

esp_err_t cerberus_ssid_list_session_copy(cerberus_ssid_list_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    esp_err_t error = take_list();
    if (error != ESP_OK) return error;
    if (!list_store) error = ESP_ERR_INVALID_STATE;
    else *out = list_store->list;
    xSemaphoreGive(list_mutex); return error;
}

esp_err_t cerberus_ssid_list_session_edit(uint64_t revision, const cerberus_ssid_list_edit_t *edit)
{
    if (!edit) return ESP_ERR_INVALID_ARG;
    esp_err_t error = take_list();
    if (error != ESP_OK) return error;
    ssid_list_store_t *s = list_store;
    if (!s || atomic_load_explicit(&close_requested, memory_order_acquire)) error = ESP_ERR_INVALID_STATE;
    else {
        cerberus_ssid_list_result_t result = CERBERUS_SSID_LIST_STALE;
        if (revision && revision == s->list.revision) switch (edit->kind) {
        case CERBERUS_SSID_EDIT_ADD: result = cerberus_ssid_list_add(&s->list, edit->bytes, edit->length); break;
        case CERBERUS_SSID_EDIT_NAME: result = cerberus_ssid_list_edit(&s->list, edit->index, edit->bytes, edit->length); break;
        case CERBERUS_SSID_EDIT_REMOVE: result = cerberus_ssid_list_remove(&s->list, edit->index); break;
        case CERBERUS_SSID_EDIT_REMOVE_SELECTED: result = cerberus_ssid_list_remove_selected(&s->list); break;
        case CERBERUS_SSID_EDIT_CLEAR: result = cerberus_ssid_list_clear(&s->list); break;
        case CERBERUS_SSID_EDIT_SELECT: result = cerberus_ssid_list_select(&s->list, edit->index, edit->selected); break;
        case CERBERUS_SSID_EDIT_SELECT_ALL: result = cerberus_ssid_list_select_all(&s->list, edit->selected); break;
        case CERBERUS_SSID_EDIT_GENERATE: result = cerberus_ssid_list_generate(&s->list, edit->count, edit->bytes, edit->length); break;
        default: result = CERBERUS_SSID_LIST_INVALID; break;
        }
        s->status.model_result = result; error = model_error(result);
    }
    xSemaphoreGive(list_mutex); return error;
}

static bool available(ssid_list_store_t *s)
{ return s && !s->status.job_id && !s->preview && !atomic_load_explicit(&close_requested, memory_order_acquire); }

esp_err_t cerberus_ssid_list_session_load(const char *path, const char *name)
{
    if (!path || !name) return ESP_ERR_INVALID_ARG;
    esp_err_t error = take_list();
    if (error != ESP_OK) return error;
    ssid_list_store_t *s = list_store;
    if (!available(s) || last_preview_id == UINT64_MAX) error = ESP_ERR_INVALID_STATE;
    else if (!(s->preview = heap_caps_calloc(1U, sizeof(*s->preview), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)))
        error = ESP_ERR_NO_MEM;
    else {
        uint64_t job = 0U;
        error = cerberus_ble_archive_io_load_ssid_text(path, name, &job);
        if (job) {
            s->saving = false; s->handled = false; s->status.job_id = job;
            s->status.state = CERBERUS_SSID_SESSION_LOADING; s->status.error = error;
            poll_locked(s);
        } else { discard_preview(s); if (error == ESP_OK) error = ESP_ERR_INVALID_RESPONSE; }
    }
    xSemaphoreGive(list_mutex); return error;
}

esp_err_t cerberus_ssid_list_session_prepare_document(const cerberus_wifi_marauder_ssid_t *document)
{
    if (!document || cerberus_wifi_marauder_ssid_validate(document) != CERBERUS_WIFI_MARAUDER_OK)
        return ESP_ERR_INVALID_ARG;
    esp_err_t error = take_list();
    if (error != ESP_OK) return error;
    ssid_list_store_t *s = list_store;
    if (!available(s) || last_preview_id == UINT64_MAX) error = ESP_ERR_INVALID_STATE;
    else if (!(s->preview = heap_caps_calloc(1U, sizeof(*s->preview), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)))
        error = ESP_ERR_NO_MEM;
    else {
        *s->preview = *document; s->status.preview_id = ++last_preview_id;
        s->status.preview_count = document->count; s->status.preview_ready = true;
        s->status.error = ESP_OK; s->status.state = CERBERUS_SSID_SESSION_PREVIEW;
    }
    xSemaphoreGive(list_mutex); return error;
}

esp_err_t cerberus_ssid_list_session_copy_preview(cerberus_wifi_marauder_ssid_t *out, uint64_t *id)
{
    if (!out || !id) return ESP_ERR_INVALID_ARG;
    esp_err_t error = take_list();
    if (error != ESP_OK) return error;
    if (!list_store || !list_store->status.preview_ready || !list_store->preview ||
        atomic_load_explicit(&close_requested, memory_order_acquire)) error = ESP_ERR_INVALID_STATE;
    else { *out = *list_store->preview; *id = list_store->status.preview_id; }
    xSemaphoreGive(list_mutex); return error;
}

esp_err_t cerberus_ssid_list_session_apply_preview(uint64_t id, uint64_t revision, bool append, bool unique)
{
    esp_err_t error = take_list();
    if (error != ESP_OK) return error;
    ssid_list_store_t *s = list_store;
    if (!s || s->status.job_id || !s->preview || !s->status.preview_ready || !id || id != s->status.preview_id ||
        atomic_load_explicit(&close_requested, memory_order_acquire)) error = ESP_ERR_INVALID_STATE;
    else {
        cerberus_ssid_list_result_t result = revision && revision == s->list.revision ?
            (append ? cerberus_ssid_list_append(&s->list, s->preview, unique) :
                cerberus_ssid_list_replace(&s->list, s->preview)) : CERBERUS_SSID_LIST_STALE;
        s->status.model_result = result; error = model_error(result);
        if (error == ESP_OK) { discard_preview(s); s->status.state = CERBERUS_SSID_SESSION_IDLE; s->status.error = ESP_OK; }
    }
    xSemaphoreGive(list_mutex); return error;
}

void cerberus_ssid_list_session_discard_preview(void)
{
    if (take_list() != ESP_OK) return;
    if (list_store && !list_store->status.job_id) {
        discard_preview(list_store);
        if (list_store->status.state == CERBERUS_SSID_SESSION_PREVIEW) list_store->status.state = CERBERUS_SSID_SESSION_IDLE;
    }
    xSemaphoreGive(list_mutex);
}

esp_err_t cerberus_ssid_list_session_save(uint64_t revision)
{
    esp_err_t error = take_list();
    if (error != ESP_OK) return error;
    ssid_list_store_t *s = list_store;
    if (!available(s) || !revision || revision != s->list.revision) error = ESP_ERR_INVALID_STATE;
    else {
        uint64_t job = 0U;
        error = cerberus_ble_archive_io_save_ssid_text(&s->list.document, &job);
        if (job) {
            s->saving = true; s->handled = false; s->save_revision = revision;
            s->status.job_id = job; s->status.state = CERBERUS_SSID_SESSION_SAVING;
            s->status.error = error; s->status.file_name[0] = '\0';
            poll_locked(s);
        } else if (error == ESP_OK) error = ESP_ERR_INVALID_RESPONSE;
    }
    xSemaphoreGive(list_mutex); return error;
}

esp_err_t cerberus_ssid_list_session_poll(cerberus_ssid_list_session_status_t *out)
{
    if (out) memset(out, 0, sizeof(*out));
    if (!list_mutex) return ESP_OK;
    esp_err_t error = take_list();
    if (error != ESP_OK) return error;
    if (list_store) { poll_locked(list_store); snapshot(list_store, out); }
    xSemaphoreGive(list_mutex); return ESP_OK;
}

void cerberus_ssid_list_session_close(void)
{
    atomic_store_explicit(&close_requested, true, memory_order_release);
    (void)cerberus_ssid_list_session_poll(NULL);
}
