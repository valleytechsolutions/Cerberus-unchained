#include "gps_recorder.h"
#include "storage.h"
#include "wardrive_gps_uart.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#define GPS_RECORDER_QUEUE 4U
#define GPS_RECORDER_STACK 4096U

typedef struct {
    cerberus_wardrive_gps_snapshot_t point;
    char label[CERBERUS_GPX_LABEL_BYTES + 1U];
} gps_recorder_point_t;

typedef struct {
    uint64_t job;
    gps_recorder_point_t points[GPS_RECORDER_QUEUE], pending;
    char row[CERBERUS_GPX_ROW_CAPACITY];
    cerberus_gps_track_stats_t track;
    unsigned head, count;
    cerberus_gpx_mode_t mode;
    bool stop;
} gps_recorder_t;

static portMUX_TYPE gps_recorder_lock = portMUX_INITIALIZER_UNLOCKED;
static gps_recorder_t *gps_recording;
static cerberus_gps_recorder_status_t gps_recording_status;
static uint64_t gps_recording_sequence;

static uint64_t gps_recorder_now(void)
{
    int64_t us = esp_timer_get_time();
    return us >= 0 ? (uint64_t)us / 1000U : UINT64_MAX;
}

static void gps_recorder_fail(esp_err_t error)
{
    portENTER_CRITICAL(&gps_recorder_lock);
    if (gps_recording_status.error == ESP_OK) gps_recording_status.error = error;
    gps_recording_status.state = CERBERUS_GPS_LOG_STOPPING;
    if (gps_recording) gps_recording->stop = true;
    portEXIT_CRITICAL(&gps_recorder_lock);
}

static void gps_recorder_worker(void *argument)
{
    gps_recorder_t *c = argument;
    bool opened = false, pending = false, canceled;
    esp_err_t error = ESP_OK;
    /* NOT_FINISHED means another short GPS control operation owns the boundary.
     * Failed native starts may retain cleanup; release is always attempted. */
    for (;;) {
        portENTER_CRITICAL(&gps_recorder_lock);
        canceled = c->stop && !c->count;
        portEXIT_CRITICAL(&gps_recorder_lock);
        if (canceled) { error = ESP_OK; break; }
        error = cerberus_wardrive_gps_uart_record(true);
        if (error != ESP_ERR_NOT_FINISHED) break;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    uint64_t last_now = gps_recorder_now(), last_track_sample = 0;
    if (error == ESP_OK && last_now == UINT64_MAX) error = ESP_ERR_INVALID_STATE;
    while (error == ESP_OK) {
        bool stopping, have = false;
        portENTER_CRITICAL(&gps_recorder_lock);
        stopping = c->stop;
        if (c->count) {
            c->pending = c->points[c->head];
            c->head = (c->head + 1U) % GPS_RECORDER_QUEUE;
            --c->count;
            gps_recording_status.queued = c->count;
            have = pending = true;
        }
        portEXIT_CRITICAL(&gps_recorder_lock);
        if (stopping && !have) break;
        uint64_t now = gps_recorder_now();
        if (now == UINT64_MAX || now < last_now) { error = ESP_ERR_INVALID_STATE; break; }
        last_now = now;
        cerberus_wardrive_gps_uart_state_t receiver = {0};
        bool ready = false;
        if (!have) {
            esp_err_t read = cerberus_wardrive_gps_uart_get_state(&receiver);
            ready = read == ESP_OK && cerberus_wardrive_gps_start_ready(true, &receiver) &&
                cerberus_gpx_point_valid(&receiver.gps);
            if (read != ESP_OK && read != ESP_ERR_TIMEOUT) { error = read; break; }
            if (read == ESP_OK && receiver.error != ESP_OK) { error = receiver.error; break; }
            portENTER_CRITICAL(&gps_recorder_lock);
            gps_recording_status.fix_ready = ready;
            if (!c->stop) gps_recording_status.state = ready ? CERBERUS_GPS_LOG_RECORDING : CERBERUS_GPS_LOG_WAITING;
            portEXIT_CRITICAL(&gps_recorder_lock);
            if (!ready) cerberus_gps_track_gap(&c->track);
        }
        bool segment = false;
        cerberus_gps_track_stats_t next = c->track;
        if (c->mode == CERBERUS_GPX_TRACK && ready && !stopping &&
            (!last_track_sample || now - last_track_sample >= 1000U)) {
            last_track_sample = now;
            have = cerberus_gps_track_observe(&next, &receiver.gps, &segment);
            if (have) { c->pending.point = receiver.gps; c->pending.label[0] = '\0'; pending = true; }
        }
        if (!have) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        uint32_t written;
        portENTER_CRITICAL(&gps_recorder_lock);
        written = gps_recording_status.written;
        portEXIT_CRITICAL(&gps_recorder_lock);
        if (written == UINT32_MAX) { error = ESP_ERR_INVALID_SIZE; break; }
        size_t length = 0;
        const char *prefix = segment && written ? cerberus_gpx_segment_break() : "";
        size_t prefix_length = strlen(prefix);
        memcpy(c->row, prefix, prefix_length);
        if (!cerberus_gpx_point(c->row + prefix_length, sizeof(c->row) - prefix_length,
            &length, c->mode, &c->pending.point, written + 1U, c->pending.label)) {
            error = ESP_ERR_INVALID_RESPONSE; break;
        }
        if (!opened) {
            do {
                error = cerberus_storage_gps_begin(c->job, c->mode);
                if (error == ESP_ERR_TIMEOUT) vTaskDelay(pdMS_TO_TICKS(50));
            } while (error == ESP_ERR_TIMEOUT);
            opened = error == ESP_OK;
        }
        if (error != ESP_OK) break;
        do {
            error = cerberus_storage_gps_append(c->job, c->row, prefix_length + length);
            if (error == ESP_ERR_TIMEOUT) vTaskDelay(pdMS_TO_TICKS(50));
        } while (error == ESP_ERR_TIMEOUT);
        if (error != ESP_OK) break;
        c->track = next;
        portENTER_CRITICAL(&gps_recorder_lock);
        ++gps_recording_status.written;
        gps_recording_status.track = c->track;
        portEXIT_CRITICAL(&gps_recorder_lock);
        memset(&c->pending, 0, sizeof(c->pending));
        pending = false;
    }
    if (error != ESP_OK) gps_recorder_fail(error);
    portENTER_CRITICAL(&gps_recorder_lock);
    c->stop = true;
    gps_recording_status.state = CERBERUS_GPS_LOG_STOPPING;
    gps_recording_status.discarded = c->count + (pending ? 1U : 0U);
    gps_recording_status.queued = 0;
    uint32_t written = gps_recording_status.written;
    portEXIT_CRITICAL(&gps_recorder_lock);
    if (opened) {
        char name[CERBERUS_STORAGE_GPS_NAME_CAPACITY]; uint64_t bytes;
        do {
            error = cerberus_storage_gps_finish(c->job, written != 0U, name, &bytes);
            if (error == ESP_ERR_TIMEOUT) vTaskDelay(pdMS_TO_TICKS(50));
        } while (error == ESP_ERR_TIMEOUT);
        portENTER_CRITICAL(&gps_recorder_lock);
        gps_recording_status.bytes = bytes;
        memcpy(gps_recording_status.file_name, name, sizeof(name));
        portEXIT_CRITICAL(&gps_recorder_lock);
        if (error != ESP_OK) gps_recorder_fail(error);
    }
    do {
        error = cerberus_wardrive_gps_uart_record(false);
        if (error != ESP_OK) {
            if (error != ESP_ERR_NOT_FINISHED) gps_recorder_fail(error);
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    } while (error != ESP_OK);
    uint64_t ended = gps_recorder_now();
    portENTER_CRITICAL(&gps_recorder_lock);
    gps_recording = NULL;
    gps_recording_status.ended_ms = ended == UINT64_MAX ? last_now : ended;
    gps_recording_status.state = gps_recording_status.error == ESP_OK ? CERBERUS_GPS_LOG_STOPPED : CERBERUS_GPS_LOG_ERROR;
    gps_recording_status.busy = false;
    portEXIT_CRITICAL(&gps_recorder_lock);
    memset(c, 0, sizeof(*c));
    heap_caps_free(c);
    vTaskDelete(NULL);
}

esp_err_t cerberus_gps_recorder_start(cerberus_gpx_mode_t mode, uint64_t *job)
{
    if (job) *job = 0;
    if (!job || !cerberus_gpx_header(mode)) return ESP_ERR_INVALID_ARG;
    uint64_t now = gps_recorder_now();
    if (now == UINT64_MAX) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&gps_recorder_lock);
    if (gps_recording_status.busy || gps_recording_sequence == UINT64_MAX) {
        portEXIT_CRITICAL(&gps_recorder_lock);
        return ESP_ERR_INVALID_STATE;
    }
    uint64_t next = ++gps_recording_sequence;
    gps_recording_status = (cerberus_gps_recorder_status_t){.job=next, .mode=mode,
        .state=CERBERUS_GPS_LOG_WAITING, .busy=true, .started_ms=now};
    portEXIT_CRITICAL(&gps_recorder_lock);
    gps_recorder_t *c = NULL;
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= 16U * 1024U &&
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= GPS_RECORDER_STACK + 2048U)
        c = heap_caps_calloc(1, sizeof(*c), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (c) {
        c->job = next; c->mode = mode;
        portENTER_CRITICAL(&gps_recorder_lock);
        c->stop = gps_recording_status.state == CERBERUS_GPS_LOG_STOPPING;
        gps_recording = c;
        portEXIT_CRITICAL(&gps_recorder_lock);
        if (xTaskCreate(gps_recorder_worker, "gps_log", GPS_RECORDER_STACK, c, 3U, NULL) == pdPASS) {
            *job = next; return ESP_OK;
        }
    }
    portENTER_CRITICAL(&gps_recorder_lock);
    gps_recording = NULL;
    gps_recording_status.busy = false;
    gps_recording_status.state = CERBERUS_GPS_LOG_ERROR;
    gps_recording_status.error = ESP_ERR_NO_MEM;
    portEXIT_CRITICAL(&gps_recorder_lock);
    heap_caps_free(c);
    return ESP_ERR_NO_MEM;
}

esp_err_t cerberus_gps_recorder_mark(uint64_t job, const char *label)
{
    if (!job || !cerberus_gpx_label_valid(label)) return ESP_ERR_INVALID_ARG;
    cerberus_wardrive_gps_uart_state_t receiver;
    esp_err_t error = cerberus_wardrive_gps_uart_get_state(&receiver);
    if (error != ESP_OK) return error;
    if (!cerberus_wardrive_gps_start_ready(true, &receiver) || !cerberus_gpx_point_valid(&receiver.gps))
        return ESP_ERR_NOT_FINISHED;
    portENTER_CRITICAL(&gps_recorder_lock);
    gps_recorder_t *c = gps_recording;
    if (!c || c->job != job || c->stop || c->mode != CERBERUS_GPX_POINTS || !gps_recording_status.busy ||
        gps_recording_status.state == CERBERUS_GPS_LOG_STOPPING)
        error = ESP_ERR_INVALID_STATE;
    else if (c->count == GPS_RECORDER_QUEUE) {
        if (gps_recording_status.rejected != UINT32_MAX) ++gps_recording_status.rejected;
        error = ESP_ERR_NO_MEM;
    } else {
        gps_recorder_point_t *slot = &c->points[(c->head + c->count) % GPS_RECORDER_QUEUE];
        slot->point = receiver.gps;
        memset(slot->label, 0, sizeof(slot->label));
        if (label) memcpy(slot->label, label, strlen(label));
        gps_recording_status.queued = ++c->count;
    }
    portEXIT_CRITICAL(&gps_recorder_lock);
    return error;
}

esp_err_t cerberus_gps_recorder_stop(uint64_t job)
{
    portENTER_CRITICAL(&gps_recorder_lock);
    esp_err_t error = ESP_ERR_INVALID_STATE;
    if (job && gps_recording_status.job == job) {
        if (gps_recording) gps_recording->stop = true;
        if (gps_recording_status.busy) gps_recording_status.state = CERBERUS_GPS_LOG_STOPPING;
        error = ESP_OK;
    }
    portEXIT_CRITICAL(&gps_recorder_lock);
    return error;
}

void cerberus_gps_recorder_status(cerberus_gps_recorder_status_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&gps_recorder_lock);*out = gps_recording_status;portEXIT_CRITICAL(&gps_recorder_lock);
}

bool cerberus_gps_recorder_busy(void)
{
    portENTER_CRITICAL(&gps_recorder_lock);
    bool busy = gps_recording_status.busy;
    portEXIT_CRITICAL(&gps_recorder_lock);
    return busy;
}
