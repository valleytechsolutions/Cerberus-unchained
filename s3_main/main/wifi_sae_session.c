#include "wifi_sae_session.h"
#include "c5_spi_link.h"
#include "cerberus_protocol.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

typedef struct {
    cerberus_wifi_sae_table_t table;
    cerberus_s3_capture_status_t capture;
    uint32_t generation, rejected_provenance;
    struct {
        uint64_t first_ms, last_ms;
        uint32_t first_sequence;
    } delivery[CERBERUS_WIFI_SAE_MAX_PEERS];
} sae_store_t;

static SemaphoreHandle_t sae_mutex;
static sae_store_t *sae_store;

static uint64_t local_ms(void)
{
    int64_t value = esp_timer_get_time();
    return value < 0 ? UINT64_MAX : (uint64_t)value / 1000U;
}

static uint32_t delivery_age(uint64_t now, uint64_t then)
{
    if (now == UINT64_MAX || then == UINT64_MAX || now < then) return UINT32_MAX;
    uint64_t age = now - then;
    return age >= UINT32_MAX ? UINT32_MAX : (uint32_t)age;
}

static esp_err_t take_sae(void)
{
    if (!sae_mutex) return ESP_ERR_INVALID_STATE;
    return xSemaphoreTake(sae_mutex, pdMS_TO_TICKS(250)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void refresh_capture(sae_store_t *store)
{
    cerberus_s3_capture_status_t current;
    cerberus_s3_capture_status(&current);
    if (current.generation == store->generation) store->capture = current;
    else if (store->capture.busy) {
        /* The shared worker cannot be replaced until the old generation's
         * observer, receiver drain and file close have all finished. */
        store->capture.busy = false;
        store->capture.state = CERBERUS_S3_CAPTURE_ERROR;
        store->capture.error_code = ESP_ERR_INVALID_STATE;
    }
}

static void receive_sae(const cerberus_s3_capture_observation_t *metadata,
    const uint8_t *frame, size_t length, void *context)
{
    if (!metadata || !frame || !context || !sae_mutex) return;
    /* Only bounded page copies or an inactive-session setup hold this mutex.
     * Finish interpretation before capture ACKs the pinned record. */
    if (xSemaphoreTake(sae_mutex, portMAX_DELAY) != pdTRUE) return;
    sae_store_t *store = context;
    if (store != sae_store) {
        xSemaphoreGive(sae_mutex);
        return;
    }
    if (!metadata->generation || metadata->generation != store->generation ||
        metadata->observed_us > UINT64_C(0xffffffffffff)) {
        if (store->rejected_provenance != UINT32_MAX) ++store->rejected_provenance;
        xSemaphoreGive(sae_mutex);
        return;
    }
    cerberus_wifi_sae_observation_t observation = {
        .observed_us = metadata->observed_us, .sequence = metadata->sequence,
        .channel = metadata->channel, .rssi = metadata->rssi_dbm,
    };
    int index = cerberus_wifi_sae_observe(&store->table, &observation, frame, length);
    if (index >= 0 && index < (int)CERBERUS_WIFI_SAE_MAX_PEERS) {
        uint64_t now = local_ms();
        uint32_t first = store->table.records[index].first_sequence;
        if (store->delivery[index].first_sequence != first) {
            store->delivery[index].first_sequence = first;
            store->delivery[index].first_ms = now;
        }
        store->delivery[index].last_ms = now;
    }
    xSemaphoreGive(sae_mutex);
}

esp_err_t cerberus_s3_sae_init(void)
{
    if (!sae_mutex) sae_mutex = xSemaphoreCreateMutex();
    return sae_mutex ? ESP_OK : ESP_ERR_NO_MEM;
}

static bool valid_options(const cerberus_s3_sae_options_t *options)
{
    static const uint8_t zero[6];
    if (!options) return false;
    if (options->plan) return !options->channel && !options->filter_bssid &&
        cerberus_wifi_capture_plan_valid(options->plan);
    return cerberus_wifi_capture_plan_channel_bit(options->channel) &&
        (!options->filter_bssid || (!(options->bssid[0] & 1U) &&
            memcmp(options->bssid, zero, 6U)));
}

esp_err_t cerberus_s3_sae_start(const cerberus_s3_sae_options_t *options)
{
    if (!valid_options(options)) return ESP_ERR_INVALID_ARG;
    esp_err_t error = take_sae();
    if (error != ESP_OK) return error;
    cerberus_s3_capture_status_t current;
    cerberus_s3_capture_status(&current);
    if (current.busy) {
        xSemaphoreGive(sae_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    cerberus_c5_status_t peer = {0};
    error = c5_spi_link_get_status(&peer);
    if (error == ESP_OK && peer.firmware_version < CERBERUS_WIFI_SAE_MONITOR_MIN_FIRMWARE)
        error = ESP_ERR_NOT_SUPPORTED;
    if (error != ESP_OK) {
        xSemaphoreGive(sae_mutex);
        return error;
    }
    sae_store_t *pending = heap_caps_calloc(1U, sizeof(*pending), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pending) {
        xSemaphoreGive(sae_mutex);
        return ESP_ERR_NO_MEM;
    }
    if (!cerberus_wifi_sae_init(&pending->table, options->channel,
            options->filter_bssid, options->bssid, options->plan)) {
        heap_caps_free(pending);
        xSemaphoreGive(sae_mutex);
        return ESP_ERR_INVALID_ARG;
    }
    cerberus_s3_capture_options_t capture = {
        .channel = options->channel, .mode_mask = CERBERUS_WIFI_CAPTURE_MODE_SAE_COMMIT,
        .filter_bssid = options->filter_bssid, .save_pcap = options->save_pcap,
        .hopping = options->plan != NULL,
        .plan = options->plan ? &pending->table.plan : NULL,
        .observer = receive_sae, .observer_context = pending,
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
        sae_store_t *previous = sae_store;
        sae_store = pending;
        /* Early observer calls wait on this mutex until the returned exact
         * generation is adopted. The previous worker was already idle. */
        heap_caps_free(previous);
    } else heap_caps_free(pending);
    xSemaphoreGive(sae_mutex);
    return error;
}

void cerberus_s3_sae_stop(void)
{
    if (take_sae() != ESP_OK) return;
    if (sae_store) {
        refresh_capture(sae_store);
        if (sae_store->capture.busy) (void)cerberus_s3_capture_stop_generation(sae_store->generation);
    }
    xSemaphoreGive(sae_mutex);
}

esp_err_t cerberus_s3_sae_status(cerberus_s3_sae_status_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    esp_err_t error = take_sae();
    if (error != ESP_OK) return error;
    if (sae_store) {
        refresh_capture(sae_store);
        out->capture = sae_store->capture;
        out->summary = sae_store->table.summary;
        out->plan = sae_store->table.plan;
        out->planned = sae_store->table.planned;
        out->filter_bssid = sae_store->table.filter_bssid;
        out->channel = sae_store->table.channel;
        memcpy(out->bssid, sae_store->table.bssid, 6U);
        out->generation = sae_store->generation;
        out->rejected_provenance = sae_store->rejected_provenance;
    }
    xSemaphoreGive(sae_mutex);
    return ESP_OK;
}

esp_err_t cerberus_s3_sae_page(uint16_t offset, cerberus_s3_sae_row_t *rows,
    size_t capacity, cerberus_s3_sae_page_t *page)
{
    if (page) memset(page, 0, sizeof(*page));
    if (rows && capacity <= 4U) memset(rows, 0, capacity * sizeof(*rows));
    if (!page || (capacity && !rows) || capacity > 4U) return ESP_ERR_INVALID_ARG;
    esp_err_t error = take_sae();
    if (error != ESP_OK) return error;
    if (sae_store) {
        page->generation = sae_store->generation;
        page->total = sae_store->table.summary.count;
        uint64_t now = local_ms();
        for (uint16_t i = offset; i < page->total && page->copied < capacity; ++i) {
            cerberus_s3_sae_row_t *row = &rows[page->copied++];
            row->generation = sae_store->generation;
            row->evidence = sae_store->table.records[i];
            row->first_received_age_ms = sae_store->delivery[i].first_sequence ?
                delivery_age(now, sae_store->delivery[i].first_ms) : UINT32_MAX;
            row->last_received_age_ms = sae_store->delivery[i].first_sequence ?
                delivery_age(now, sae_store->delivery[i].last_ms) : UINT32_MAX;
        }
    }
    xSemaphoreGive(sae_mutex);
    return ESP_OK;
}

esp_err_t cerberus_s3_sae_release(void)
{
    esp_err_t error = take_sae();
    if (error != ESP_OK) return error;
    if (sae_store) {
        refresh_capture(sae_store);
        if (sae_store->capture.busy) error = ESP_ERR_INVALID_STATE;
        else { heap_caps_free(sae_store); sae_store = NULL; }
    }
    xSemaphoreGive(sae_mutex);
    return error;
}
