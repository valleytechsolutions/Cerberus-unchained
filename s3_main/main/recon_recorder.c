#include "recon_recorder.h"
#include "storage.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#define RECON_RECORDER_STACK_BYTES 6144U
#define RECON_RECORDER_MIN_FREE_INTERNAL (24U * 1024U)
#define RECON_RECORDER_MIN_LARGEST_INTERNAL (RECON_RECORDER_STACK_BYTES + 2048U)
#define RECON_RECORDER_SYNC_US UINT64_C(2000000)

typedef struct {
    cerberus_recon_journal_t journal;
    char row[CERBERUS_RECON_CSV_CAPACITY];
    char manifest[CERBERUS_RECON_MANIFEST_CAPACITY];
    bool stop_requested;
} recon_recorder_context_t;

/* The queue and format buffers exist only while recording, in PSRAM. A short
 * lock covers bounded copies/counters, never formatting, allocation or I/O. */
static portMUX_TYPE recorder_lock = portMUX_INITIALIZER_UNLOCKED;
static recon_recorder_context_t *recorder_context;
static cerberus_recon_recorder_status_t recorder_status;
static uint64_t recorder_last_session, recorder_next_sequence;

static uint64_t recorder_now(void)
{
    int64_t value = esp_timer_get_time();
    return value > 0 ? (uint64_t)value : 0U;
}

static void recorder_copy_counters_locked(const recon_recorder_context_t *context)
{
    recorder_status.counters = context->journal.counters;
    recorder_status.queued = context->journal.count;
}

static void recorder_error_locked(recon_recorder_context_t *context, esp_err_t error)
{
    if (recorder_status.error == ESP_OK) recorder_status.error = error;
    recorder_status.state = CERBERUS_RECON_RECORDER_STOPPING;
    context->stop_requested = true;
}

static void recorder_complete(recon_recorder_context_t *context,
    cerberus_recon_recorder_state_t state, uint64_t ended_us)
{
    portENTER_CRITICAL(&recorder_lock);
    recorder_copy_counters_locked(context);
    recorder_status.state = state;
    recorder_status.ended_us = ended_us;
    recorder_status.worker_active = false;
    recorder_context = NULL;
    portEXIT_CRITICAL(&recorder_lock);
    heap_caps_free(context);
}

static void recon_recorder_worker(void *argument)
{
    recon_recorder_context_t *context = argument;
    bool canceled;
    portENTER_CRITICAL(&recorder_lock);
    canceled = context->stop_requested && context->journal.count == 0U && recorder_status.error == ESP_OK;
    portEXIT_CRITICAL(&recorder_lock);
    if (canceled) {
        recorder_complete(context, CERBERUS_RECON_RECORDER_CANCELED, recorder_now());
        vTaskDelete(NULL);
        return;
    }

    char session_name[7] = {0};
    const char *header = cerberus_recon_csv_header();
    esp_err_t error = cerberus_storage_recon_begin(header, strlen(header),
        session_name, sizeof(session_name));
    portENTER_CRITICAL(&recorder_lock);
    memcpy(recorder_status.session_name, session_name, sizeof(session_name));
    if (error != ESP_OK) {
        recorder_error_locked(context, error);
        cerberus_recon_journal_abandon(&context->journal);
    } else if (!context->stop_requested) {
        recorder_status.state = CERBERUS_RECON_RECORDER_RECORDING;
    }
    portEXIT_CRITICAL(&recorder_lock);
    if (error != ESP_OK) {
        /* begin guarantees cleanup on failure, including partial creation. */
        recorder_complete(context, CERBERUS_RECON_RECORDER_ERROR, recorder_now());
        vTaskDelete(NULL);
        return;
    }

    uint64_t last_worker_us = recorder_now();
    uint64_t last_sync = last_worker_us;
    portENTER_CRITICAL(&recorder_lock);
    if (!last_worker_us || last_worker_us < context->journal.last_consumed_us)
        recorder_error_locked(context, ESP_ERR_INVALID_STATE);
    portEXIT_CRITICAL(&recorder_lock);
    for (;;) {
        cerberus_recon_record_t row;
        bool have_row = false, stopping;
        portENTER_CRITICAL(&recorder_lock);
        error = recorder_status.error;
        if (error != ESP_OK) cerberus_recon_journal_abandon(&context->journal);
        const cerberus_recon_record_t *head = cerberus_recon_journal_peek(&context->journal);
        if (head != NULL && error == ESP_OK) { row = *head; have_row = true; }
        stopping = context->stop_requested;
        recorder_copy_counters_locked(context);
        portEXIT_CRITICAL(&recorder_lock);
        if (!have_row && stopping) break;

        if (have_row) {
            size_t length = cerberus_recon_row_format(&row, context->row, sizeof(context->row));
            error = length ? cerberus_storage_recon_append(context->row, length) : ESP_ERR_INVALID_RESPONSE;
            portENTER_CRITICAL(&recorder_lock);
            if (error == ESP_OK && !cerberus_recon_journal_commit(&context->journal, row.sequence))
                error = ESP_ERR_INVALID_STATE;
            if (error != ESP_OK) recorder_error_locked(context, error);
            recorder_copy_counters_locked(context);
            portEXIT_CRITICAL(&recorder_lock);
        }
        uint64_t now = recorder_now();
        portENTER_CRITICAL(&recorder_lock);
        bool valid_clock = now && now >= last_worker_us && now >= context->journal.last_consumed_us;
        portEXIT_CRITICAL(&recorder_lock);
        if (valid_clock) last_worker_us = now;
        else if (error == ESP_OK) error = ESP_ERR_INVALID_STATE;
        if (error == ESP_OK && now - last_sync >= RECON_RECORDER_SYNC_US) {
            error = cerberus_storage_recon_sync();
            if (error == ESP_OK) last_sync = now;
        }
        if (error != ESP_OK) {
            portENTER_CRITICAL(&recorder_lock);
            recorder_error_locked(context, error);
            portEXIT_CRITICAL(&recorder_lock);
        }
        vTaskDelay(have_row ? 1U : pdMS_TO_TICKS(20U));
    }

    cerberus_recon_summary_t summary = {0};
    uint64_t ended_us = recorder_now();
    portENTER_CRITICAL(&recorder_lock);
    if (!ended_us || ended_us < last_worker_us || ended_us < context->journal.last_consumed_us) {
        recorder_error_locked(context, ESP_ERR_INVALID_STATE);
        ended_us = 0U; /* Unknown end time; do not fabricate a valid timestamp. */
    }
    error = recorder_status.error;
    summary.session_id = context->journal.session_id;
    summary.started_us = context->journal.started_us;
    summary.ended_us = ended_us;
    summary.last_sequence = context->journal.last_sequence;
    summary.counters = context->journal.counters;
    summary.queued = context->journal.count;
    summary.state = error == ESP_OK ? CERBERUS_RECON_MANIFEST_COMPLETE : CERBERUS_RECON_MANIFEST_ERROR;
    summary.error_code = error;
    memcpy(summary.session_name, recorder_status.session_name, sizeof(summary.session_name));
    recorder_status.state = CERBERUS_RECON_RECORDER_STOPPING;
    portEXIT_CRITICAL(&recorder_lock);
    size_t length = cerberus_recon_manifest_format(&summary, context->manifest, sizeof(context->manifest));
    if (!length && error == ESP_OK) error = ESP_ERR_INVALID_RESPONSE;
    /* finish always releases its lease/handles, even with an invalid manifest
     * or a previous append/sync error. It is called once; no partial-row retry. */
    esp_err_t finish_error = cerberus_storage_recon_finish(context->manifest, length, error == ESP_OK);
    if (error == ESP_OK) error = finish_error;
    portENTER_CRITICAL(&recorder_lock);
    if (error != ESP_OK && recorder_status.error == ESP_OK) recorder_status.error = error;
    portEXIT_CRITICAL(&recorder_lock);
    recorder_complete(context, error == ESP_OK ? CERBERUS_RECON_RECORDER_COMPLETE :
        CERBERUS_RECON_RECORDER_ERROR, ended_us);
    vTaskDelete(NULL);
}

esp_err_t cerberus_recon_recorder_start(void)
{
    const uint64_t now = recorder_now();
    portENTER_CRITICAL(&recorder_lock);
    if (recorder_status.worker_active || recorder_context != NULL) {
        portEXIT_CRITICAL(&recorder_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (!now || recorder_last_session == UINT64_MAX) {
        recorder_status.state = CERBERUS_RECON_RECORDER_ERROR;
        recorder_status.error = ESP_ERR_INVALID_STATE;
        portEXIT_CRITICAL(&recorder_lock);
        return ESP_ERR_INVALID_STATE;
    }
    memset(&recorder_status, 0, sizeof(recorder_status));
    recorder_status.state = CERBERUS_RECON_RECORDER_STARTING;
    recorder_status.worker_active = true;
    recorder_status.session_id = ++recorder_last_session;
    recorder_status.started_us = now;
    recorder_next_sequence = 0U;
    const uint64_t session_id = recorder_status.session_id;
    portEXIT_CRITICAL(&recorder_lock);

    recon_recorder_context_t *context = NULL;
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= RECON_RECORDER_MIN_FREE_INTERNAL &&
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= RECON_RECORDER_MIN_LARGEST_INTERNAL)
        context = heap_caps_calloc(1U, sizeof(*context), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (context != NULL) {
        (void)cerberus_recon_journal_init(&context->journal, session_id, now);
        portENTER_CRITICAL(&recorder_lock);
        context->stop_requested = recorder_status.state == CERBERUS_RECON_RECORDER_STOPPING;
        recorder_context = context;
        portEXIT_CRITICAL(&recorder_lock);
        if (xTaskCreate(recon_recorder_worker, "recon_sd", RECON_RECORDER_STACK_BYTES,
                context, 2U, NULL) == pdPASS) return ESP_OK;
    }
    portENTER_CRITICAL(&recorder_lock);
    if (context != NULL) {
        cerberus_recon_journal_abandon(&context->journal);
        recorder_copy_counters_locked(context);
    }
    recorder_context = NULL;
    recorder_status.worker_active = false;
    recorder_status.state = CERBERUS_RECON_RECORDER_ERROR;
    recorder_status.error = ESP_ERR_NO_MEM;
    recorder_status.ended_us = now;
    portEXIT_CRITICAL(&recorder_lock);
    heap_caps_free(context);
    return ESP_ERR_NO_MEM;
}

void cerberus_recon_recorder_stop(void)
{
    portENTER_CRITICAL(&recorder_lock);
    if (recorder_status.worker_active) {
        recorder_status.state = CERBERUS_RECON_RECORDER_STOPPING;
        if (recorder_context != NULL) recorder_context->stop_requested = true;
    }
    portEXIT_CRITICAL(&recorder_lock);
}

bool cerberus_recon_recorder_offer(const cerberus_recon_record_t *record)
{
    const uint64_t now = recorder_now();
    portENTER_CRITICAL(&recorder_lock);
    recon_recorder_context_t *context = recorder_context;
    if (context == NULL || context->stop_requested || recorder_status.error != ESP_OK) {
        portEXIT_CRITICAL(&recorder_lock);
        return false;
    }
    if (recorder_next_sequence == UINT64_MAX) {
        recorder_error_locked(context, ESP_ERR_INVALID_STATE);
        portEXIT_CRITICAL(&recorder_lock);
        return false;
    }
    cerberus_recon_record_t row;
    bool valid = record != NULL && now && record->consumed_us <= now;
    if (valid) {
        row = *record;
        row.session_id = context->journal.session_id;
        row.sequence = ++recorder_next_sequence;
    }
    bool accepted = cerberus_recon_journal_offer(&context->journal, valid ? &row : NULL);
    recorder_copy_counters_locked(context);
    portEXIT_CRITICAL(&recorder_lock);
    return accepted;
}

void cerberus_recon_recorder_get_status(cerberus_recon_recorder_status_t *status)
{
    if (status == NULL) return;
    portENTER_CRITICAL(&recorder_lock);
    *status = recorder_status;
    portEXIT_CRITICAL(&recorder_lock);
}

const char *cerberus_recon_recorder_state_name(cerberus_recon_recorder_state_t state)
{
    static const char *const names[] = {"IDLE", "STARTING", "RECORDING", "STOPPING",
        "COMPLETE", "ERROR", "CANCELED"};
    return (unsigned)state < sizeof(names) / sizeof(names[0]) ? names[state] : "UNKNOWN";
}
