#include "wifi_eapol_session.h"
#include "c5_spi_link.h"
#include "cerberus_protocol.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

typedef struct {
    cerberus_wifi_eapol_table_t table;
    cerberus_s3_capture_status_t capture;
    uint32_t generation, rejected_provenance;
    uint8_t bssid[6], channel;
    struct {
        uint8_t station[6];
        uint32_t first_sequence;
        uint64_t first_received_ms, last_received_ms;
        bool valid;
    } delivery[CERBERUS_WIFI_EAPOL_MAX_PEERS];
} eapol_store_t;

static SemaphoreHandle_t eapol_mutex;
static eapol_store_t *eapol_store;

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

static esp_err_t take_eapol(void)
{
    if (!eapol_mutex) return ESP_ERR_INVALID_STATE;
    return xSemaphoreTake(eapol_mutex, pdMS_TO_TICKS(250)) == pdTRUE ?
        ESP_OK : ESP_ERR_TIMEOUT;
}

static void refresh_capture(eapol_store_t *store)
{
    cerberus_s3_capture_status_t current;
    cerberus_s3_capture_status(&current);
    if (current.generation == store->generation) store->capture = current;
    else if (store->capture.busy) {
        /* The shared capture worker can change generations only after all
         * observer calls and radio/file cleanup for this generation finish. */
        store->capture.busy = false;
        store->capture.state = CERBERUS_S3_CAPTURE_ERROR;
        store->capture.error_code = ESP_ERR_INVALID_STATE;
    }
}

static void receive_eapol(const cerberus_s3_capture_observation_t *metadata,
    const uint8_t *frame, size_t length, void *context)
{
    if (!metadata || !frame || !context || !eapol_mutex) return;
    /* Other holders only configure an inactive session or copy a small page.
     * Complete ingestion before the capture worker ACKs its pinned record. */
    if (xSemaphoreTake(eapol_mutex, portMAX_DELAY) != pdTRUE) return;
    eapol_store_t *store = context;
    if (store != eapol_store) {
        xSemaphoreGive(eapol_mutex);
        return;
    }
    if (!metadata->generation || metadata->generation != store->generation ||
        metadata->channel != store->channel ||
        metadata->observed_us > UINT64_C(0xffffffffffff)) {
        if (store->rejected_provenance != UINT32_MAX) ++store->rejected_provenance;
        xSemaphoreGive(eapol_mutex);
        return;
    }
    cerberus_wifi_eapol_observation_t observation = {
        .observed_us = metadata->observed_us, .sequence = metadata->sequence,
        .channel = metadata->channel, .rssi = metadata->rssi_dbm,
    };
    int index = cerberus_wifi_eapol_observe(&store->table, &observation, frame, length);
    if (index >= 0 && index < (int)CERBERUS_WIFI_EAPOL_MAX_PEERS) {
        const cerberus_wifi_eapol_record_t *record = &store->table.records[index];
        uint64_t received = local_ms();
        if (!store->delivery[index].valid ||
            store->delivery[index].first_sequence != record->first_sequence ||
            memcmp(store->delivery[index].station, record->station, 6U)) {
            memcpy(store->delivery[index].station, record->station, 6U);
            store->delivery[index].first_sequence = record->first_sequence;
            store->delivery[index].first_received_ms = received;
            store->delivery[index].valid = true;
        }
        store->delivery[index].last_received_ms = received;
    }
    xSemaphoreGive(eapol_mutex);
}

esp_err_t cerberus_s3_eapol_init(void)
{
    if (!eapol_mutex) eapol_mutex = xSemaphoreCreateMutex();
    return eapol_mutex ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t cerberus_s3_eapol_start(uint8_t channel, const uint8_t bssid[6], bool save_pcap)
{
    static const uint8_t zero[6];
    if (!bssid || (bssid[0] & 1U) || !memcmp(bssid, zero, 6U) ||
        !channel || (channel > 14U && (channel < 32U || channel > 177U)))
        return ESP_ERR_INVALID_ARG;
    esp_err_t error = take_eapol();
    if (error != ESP_OK) return error;
    cerberus_s3_capture_status_t current;
    cerberus_s3_capture_status(&current);
    if (current.busy) {
        xSemaphoreGive(eapol_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    cerberus_c5_status_t peer = {0};
    error = c5_spi_link_get_status(&peer);
    if (error == ESP_OK && peer.firmware_version < CERBERUS_WIFI_EAPOL_MIN_FIRMWARE)
        error = ESP_ERR_NOT_SUPPORTED;
    if (error != ESP_OK) {
        xSemaphoreGive(eapol_mutex);
        return error;
    }
    eapol_store_t *pending = heap_caps_calloc(1U, sizeof(*pending),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pending) {
        xSemaphoreGive(eapol_mutex);
        return ESP_ERR_NO_MEM;
    }
    if (!cerberus_wifi_eapol_init(&pending->table, bssid, channel)) {
        heap_caps_free(pending);
        xSemaphoreGive(eapol_mutex);
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(pending->bssid, bssid, 6U);
    pending->channel = channel;
    cerberus_s3_capture_options_t options = {
        .channel = channel, .mode_mask = CERBERUS_WIFI_CAPTURE_MODE_EAPOL,
        .filter_bssid = true, .save_pcap = save_pcap,
        .observer = receive_eapol, .observer_context = pending,
    };
    memcpy(options.bssid, bssid, 6U);
    uint32_t generation = 0U;
    error = cerberus_s3_capture_start_options_generation(&options, &generation);
    if (error == ESP_OK) {
        pending->generation = generation;
        pending->capture = (cerberus_s3_capture_status_t){
            .state = CERBERUS_S3_CAPTURE_STARTING, .generation = generation,
            .busy = true, .channel = channel, .mode_mask = options.mode_mask,
            .filter_bssid = true, .save_pcap = save_pcap,
        };
        refresh_capture(pending);
        eapol_store_t *previous = eapol_store;
        eapol_store = pending;
        /* The old capture was idle before reservation. An early new callback
         * waits on this mutex until its exact generation/store is installed. */
        heap_caps_free(previous);
    } else heap_caps_free(pending);
    xSemaphoreGive(eapol_mutex);
    return error;
}

void cerberus_s3_eapol_stop(void)
{
    if (take_eapol() != ESP_OK) return;
    if (eapol_store) {
        refresh_capture(eapol_store);
        if (eapol_store->capture.busy)
            (void)cerberus_s3_capture_stop_generation(eapol_store->generation);
    }
    xSemaphoreGive(eapol_mutex);
}

esp_err_t cerberus_s3_eapol_status(cerberus_s3_eapol_status_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    esp_err_t error = take_eapol();
    if (error != ESP_OK) return error;
    if (eapol_store) {
        refresh_capture(eapol_store);
        out->capture = eapol_store->capture;
        out->summary = eapol_store->table.summary;
        out->generation = eapol_store->generation;
        out->rejected_provenance = eapol_store->rejected_provenance;
        memcpy(out->bssid, eapol_store->bssid, 6U);
        out->channel = eapol_store->channel;
    }
    xSemaphoreGive(eapol_mutex);
    return ESP_OK;
}

esp_err_t cerberus_s3_eapol_page(uint16_t offset, cerberus_s3_eapol_row_t *rows,
    size_t capacity, cerberus_s3_eapol_page_t *page)
{
    if (page) memset(page, 0, sizeof(*page));
    if (rows && capacity <= 4U) memset(rows, 0, capacity * sizeof(*rows));
    if (!page || (capacity && !rows) || capacity > 4U) return ESP_ERR_INVALID_ARG;
    esp_err_t error = take_eapol();
    if (error != ESP_OK) return error;
    if (eapol_store) {
        eapol_store_t *store = eapol_store;
        page->generation = store->generation;
        page->total = store->table.summary.count;
        uint64_t now = local_ms();
        for (uint16_t i = offset; i < page->total && page->copied < capacity; ++i) {
            cerberus_s3_eapol_row_t *row = &rows[page->copied++];
            row->evidence = store->table.records[i];
            row->generation = store->generation;
            row->first_received_age_ms = store->delivery[i].valid ?
                delivery_age(now, store->delivery[i].first_received_ms) : UINT32_MAX;
            row->last_received_age_ms = store->delivery[i].valid ?
                delivery_age(now, store->delivery[i].last_received_ms) : UINT32_MAX;
        }
    }
    xSemaphoreGive(eapol_mutex);
    return ESP_OK;
}

esp_err_t cerberus_s3_eapol_release(void)
{
    esp_err_t error = take_eapol();
    if (error != ESP_OK) return error;
    if (eapol_store) {
        refresh_capture(eapol_store);
        if (eapol_store->capture.busy) error = ESP_ERR_INVALID_STATE;
        else {
            heap_caps_free(eapol_store);
            eapol_store = NULL;
        }
    }
    xSemaphoreGive(eapol_mutex);
    return error;
}
