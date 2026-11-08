#include "device_info.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <string.h>

struct cerberus_device_info_job {
    atomic_bool cancel, done;
    cerberus_device_info_t snapshot;
    cerberus_device_identity_reader_t reader;
};

static void device_info_worker(void *argument)
{
    cerberus_device_info_job_t *job = argument;
    cerberus_device_info_t *s = &job->snapshot;
    s->c5_error = ESP_ERR_NOT_FINISHED;
    s->storage_error = ESP_ERR_NOT_FINISHED;
    s->connection_error = ESP_ERR_NOT_FINISHED;
    int64_t now = esp_timer_get_time();
    s->sampled_ms = now >= 0 ? (uint64_t)now / 1000U : 0;
    cerberus_device_identity_collect(&s->s3, CERBERUS_C5_FIRMWARE_VERSION);
    s->s3_error = cerberus_device_identity_valid(&s->s3) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
    cerberus_system_health_snapshot(&s->health);
    cerberus_battery_get_status(&s->battery);
    s->psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s->psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!atomic_load_explicit(&job->cancel, memory_order_acquire))
        s->storage_error = cerberus_storage_get_status(&s->storage);
    now = esp_timer_get_time();
    if (now < 0) s->c5_error = ESP_ERR_INVALID_STATE;
    if (now >= 0 && !atomic_load_explicit(&job->cancel, memory_order_acquire)) {
        c5_spi_link_device_identity_begin(&job->reader, (uint32_t)((uint64_t)now / 1000U) + 5000U);
        bool complete = false;
        do {
            s->c5_error = c5_spi_link_device_identity_step(&job->reader, &s->c5, &complete);
            if (s->c5_error != ESP_OK || complete) break;
            vTaskDelay(pdMS_TO_TICKS(1));
        } while (!atomic_load_explicit(&job->cancel, memory_order_acquire));
        if (complete && s->c5_error == ESP_OK) s->peer = job->reader.peer;
    }
    if (s->c5_error == ESP_OK && !atomic_load_explicit(&job->cancel, memory_order_acquire)) {
        s->connection_error = c5_spi_link_get_wifi_connection_status(&s->connection);
        if (s->connection_error == ESP_OK && s->connection.state > CERBERUS_WIFI_CONNECTION_ERROR)
            s->connection_error = ESP_ERR_INVALID_RESPONSE;
    }
    if (atomic_load_explicit(&job->cancel, memory_order_acquire)) {
        memset(s, 0, sizeof(*s));
        s->cancelled = true;
        s->s3_error = s->c5_error = s->storage_error = s->connection_error = ESP_ERR_NOT_FINISHED;
    }
    atomic_store_explicit(&job->done, true, memory_order_release);
    /* No access to job after publication: the UI may now free its storage. */
    vTaskDelete(NULL);
}

esp_err_t cerberus_device_info_start(cerberus_device_info_job_t **out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (*out) return ESP_ERR_INVALID_STATE;
    cerberus_device_info_job_t *job = heap_caps_calloc(1, sizeof(*job), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!job) return ESP_ERR_NO_MEM;
    atomic_init(&job->cancel, false);
    atomic_init(&job->done, false);
    if (xTaskCreate(device_info_worker, "device_info", 4096U, job, 2U, NULL) != pdPASS) {
        heap_caps_free(job);
        return ESP_ERR_NO_MEM;
    }
    *out = job;
    return ESP_OK;
}

bool cerberus_device_info_read(const cerberus_device_info_job_t *job, cerberus_device_info_t *out)
{
    if (!job || !out || !atomic_load_explicit(&job->done, memory_order_acquire)) return false;
    *out = job->snapshot;
    return true;
}

void cerberus_device_info_cancel(cerberus_device_info_job_t *job)
{
    if (job) atomic_store_explicit(&job->cancel, true, memory_order_release);
}

bool cerberus_device_info_release(cerberus_device_info_job_t **job)
{
    if (!job) return false;
    if (!*job) return true;
    if (!atomic_load_explicit(&(*job)->done, memory_order_acquire)) return false;
    heap_caps_free(*job);
    *job = NULL;
    return true;
}
