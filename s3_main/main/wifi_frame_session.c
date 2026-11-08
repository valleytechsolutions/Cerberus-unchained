#include "wifi_frame_session.h"
#include "c5_spi_link.h"
#include "cerberus_protocol.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

typedef struct {
    uint32_t first_sequence;
    uint64_t first_ms, last_ms;
} frame_delivery_t;

typedef struct {
    cerberus_wifi_frame_model_t model;
    cerberus_s3_capture_status_t capture;
    frame_delivery_t events[CERBERUS_WIFI_FRAME_MAX_EVENTS];
    uint64_t delivered_ms;
    uint32_t generation, rejected_provenance;
    bool delivered;
} frame_store_t;

static SemaphoreHandle_t frame_mutex;
static frame_store_t *frame_store;

static uint64_t local_ms(void)
{
    int64_t value = esp_timer_get_time();
    return value < 0 ? UINT64_MAX : (uint64_t)value / 1000U;
}

static uint32_t age_ms(uint64_t now, uint64_t then)
{
    if (now == UINT64_MAX || then == UINT64_MAX || now < then) return UINT32_MAX;
    uint64_t age = now - then;
    return age >= UINT32_MAX ? UINT32_MAX : (uint32_t)age;
}

static esp_err_t take_frame(void)
{
    if (!frame_mutex) return ESP_ERR_INVALID_STATE;
    return xSemaphoreTake(frame_mutex, pdMS_TO_TICKS(250)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void refresh_capture(frame_store_t *store)
{
    cerberus_s3_capture_status_t current;
    cerberus_s3_capture_status(&current);
    if (current.generation == store->generation) store->capture = current;
    else if (store->capture.busy) {
        /* A new shared-worker generation proves this observer's cleanup ended. */
        store->capture.busy = false;
        store->capture.state = CERBERUS_S3_CAPTURE_ERROR;
        store->capture.error_code = ESP_ERR_INVALID_STATE;
    }
}

static void receive_frame(const cerberus_s3_capture_observation_t *metadata,
    const uint8_t *bytes, size_t length, void *context)
{
    if (!metadata || !bytes || !context || !frame_mutex) return;
    if (xSemaphoreTake(frame_mutex, portMAX_DELAY) != pdTRUE) return;
    frame_store_t *store = context;
    if (store != frame_store) { xSemaphoreGive(frame_mutex); return; }
    if (!metadata->generation || metadata->generation != store->generation ||
        metadata->observed_us > UINT64_C(0xffffffffffff) ||
        (store->model.planned ? !(store->model.plan.channel_mask &
            cerberus_wifi_capture_plan_channel_bit(metadata->channel)) :
            metadata->channel != store->model.channel)) {
        if (store->rejected_provenance != UINT32_MAX) ++store->rejected_provenance;
        xSemaphoreGive(frame_mutex); return;
    }
    cerberus_wifi_frame_observation_t observation = {
        .observed_us = metadata->observed_us, .sequence = metadata->sequence,
        .channel = metadata->channel, .rssi = metadata->rssi_dbm,
    };
    cerberus_wifi_frame_result_t result = cerberus_wifi_frame_observe(
        &store->model, &observation, bytes, length);
    if (result.delivered) {
        store->delivered = true; store->delivered_ms = local_ms();
        if (result.event_index >= 0 && result.event_index < (int)CERBERUS_WIFI_FRAME_MAX_EVENTS) {
            unsigned index = (unsigned)result.event_index;
            frame_delivery_t *delivery = &store->events[index];
            uint32_t first = store->model.records[index].first_sequence;
            if (delivery->first_sequence != first) {
                delivery->first_sequence = first; delivery->first_ms = store->delivered_ms;
            }
            delivery->last_ms = store->delivered_ms;
        }
    }
    xSemaphoreGive(frame_mutex);
}

esp_err_t cerberus_s3_frame_init(void)
{
    if (!frame_mutex) frame_mutex = xSemaphoreCreateMutex();
    return frame_mutex ? ESP_OK : ESP_ERR_NO_MEM;
}

static bool valid_options(const cerberus_s3_frame_options_t *o)
{
    static const uint8_t zero[6];
    if (!o || (o->mode_mask != CERBERUS_WIFI_CAPTURE_MODE_RAW &&
        o->mode_mask != CERBERUS_WIFI_CAPTURE_MODE_PROBE &&
        o->mode_mask != CERBERUS_WIFI_CAPTURE_MODE_DEAUTH)) return false;
    if (o->plan) return !o->channel && !o->filter_bssid &&
        cerberus_wifi_capture_plan_valid(o->plan) &&
        cerberus_wifi_capture_plan_mode_valid(o->mode_mask, o->plan->dwell_ms) &&
        (o->mode_mask != CERBERUS_WIFI_CAPTURE_MODE_PROBE || !o->plan->target_count);
    return cerberus_wifi_capture_plan_channel_bit(o->channel) &&
        (!o->filter_bssid || (!(o->bssid[0] & 1U) && memcmp(o->bssid, zero, 6U) &&
            o->mode_mask != CERBERUS_WIFI_CAPTURE_MODE_PROBE));
}

esp_err_t cerberus_s3_frame_start(const cerberus_s3_frame_options_t *options)
{
    if (!valid_options(options)) return ESP_ERR_INVALID_ARG;
    esp_err_t error = take_frame();
    if (error != ESP_OK) return error;
    cerberus_s3_capture_status_t current;
    cerberus_s3_capture_status(&current);
    if (current.busy) { xSemaphoreGive(frame_mutex); return ESP_ERR_INVALID_STATE; }
    cerberus_c5_status_t peer = {0};
    error = c5_spi_link_get_status(&peer);
    if (error == ESP_OK && peer.firmware_version < CERBERUS_WIFI_FRAME_SCOPE_MIN_FIRMWARE)
        error = ESP_ERR_NOT_SUPPORTED;
    if (error != ESP_OK) { xSemaphoreGive(frame_mutex); return error; }
    frame_store_t *pending = heap_caps_calloc(1U, sizeof(*pending), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pending) { xSemaphoreGive(frame_mutex); return ESP_ERR_NO_MEM; }
    if (!cerberus_wifi_frame_init(&pending->model, options->channel, options->mode_mask,
        options->filter_bssid, options->bssid, options->plan)) {
        heap_caps_free(pending); xSemaphoreGive(frame_mutex); return ESP_ERR_INVALID_ARG;
    }
    cerberus_s3_capture_options_t capture = {
        .channel = options->channel, .mode_mask = options->mode_mask,
        .filter_bssid = options->filter_bssid, .save_pcap = options->save_pcap,
        .hopping = options->plan != NULL,
        .plan = options->plan ? &pending->model.plan : NULL,
        .observer = receive_frame, .observer_context = pending,
    };
    if (options->filter_bssid) memcpy(capture.bssid, options->bssid, 6U);
    uint32_t generation = 0U;
    error = cerberus_s3_capture_start_options_generation(&capture, &generation);
    if (error == ESP_OK) {
        pending->generation = generation;
        pending->capture = (cerberus_s3_capture_status_t){
            .state = CERBERUS_S3_CAPTURE_STARTING, .generation = generation, .busy = true,
            .channel = capture.channel, .mode_mask = capture.mode_mask,
            .filter_bssid = capture.filter_bssid, .save_pcap = capture.save_pcap,
            .hopping = capture.hopping, .planned = capture.plan != NULL,
        };
        refresh_capture(pending);
        frame_store_t *previous = frame_store;
        frame_store = pending;
        /* Early worker callbacks wait on this mutex until generation and
         * context are bound. The previous worker was fully idle above. */
        heap_caps_free(previous);
    } else heap_caps_free(pending);
    xSemaphoreGive(frame_mutex); return error;
}

void cerberus_s3_frame_stop(void)
{
    if (take_frame() != ESP_OK) return;
    if (frame_store) {
        refresh_capture(frame_store);
        if (frame_store->capture.busy)
            (void)cerberus_s3_capture_stop_generation(frame_store->generation);
    }
    xSemaphoreGive(frame_mutex);
}

esp_err_t cerberus_s3_frame_status(cerberus_s3_frame_status_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out)); out->delivered_age_ms = UINT32_MAX;
    esp_err_t error = take_frame();
    if (error != ESP_OK) return error;
    if (frame_store) {
        frame_store_t *store = frame_store;
        refresh_capture(store);
        out->capture = store->capture; out->summary = store->model.summary;
        out->generation = store->generation; out->rejected_provenance = store->rejected_provenance;
        out->delivered = store->delivered;
        if (store->delivered) out->delivered_age_ms = age_ms(local_ms(), store->delivered_ms);
        out->channel = store->model.channel; out->mode_mask = store->model.mode_mask;
        memcpy(out->bssid, store->model.bssid, 6U);
        out->plan = store->model.plan; out->planned = store->model.planned;
        out->filter_bssid = store->model.filter_bssid;
    }
    xSemaphoreGive(frame_mutex); return ESP_OK;
}

esp_err_t cerberus_s3_frame_page(uint16_t offset, cerberus_s3_frame_row_t *rows,
    size_t capacity, cerberus_s3_frame_page_t *page)
{
    if (page) memset(page, 0, sizeof(*page));
    if (rows && capacity <= 4U) memset(rows, 0, capacity * sizeof(*rows));
    if (!page || (capacity && !rows) || capacity > 4U) return ESP_ERR_INVALID_ARG;
    esp_err_t error = take_frame();
    if (error != ESP_OK) return error;
    if (frame_store) {
        frame_store_t *store = frame_store;
        page->generation = store->generation; page->total = store->model.summary.count;
        uint64_t now = local_ms();
        for (uint16_t i = offset; i < page->total && page->copied < capacity; ++i) {
            cerberus_s3_frame_row_t *row = &rows[page->copied++];
            row->generation = store->generation; row->evidence = store->model.records[i];
            row->first_received_age_ms = age_ms(now, store->events[i].first_ms);
            row->last_received_age_ms = age_ms(now, store->events[i].last_ms);
        }
    }
    xSemaphoreGive(frame_mutex); return ESP_OK;
}

esp_err_t cerberus_s3_frame_release(void)
{
    esp_err_t error = take_frame();
    if (error != ESP_OK) return error;
    if (frame_store) {
        refresh_capture(frame_store);
        if (frame_store->capture.busy) error = ESP_ERR_INVALID_STATE;
        else { heap_caps_free(frame_store); frame_store = NULL; }
    }
    xSemaphoreGive(frame_mutex); return error;
}
