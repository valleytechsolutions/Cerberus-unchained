#include "s3_wifi_scanner.h"
#include "s3_wifi_owner.h"
#include "s3_ble_host.h"
#include "cerberus_protocol.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

typedef struct {
    uint32_t generation;
    uint32_t completed_ms;
    uint16_t total_count;
    uint16_t dropped_count;
    uint8_t count;
    bool truncated;
    cerberus_s3_wifi_scan_row_t rows[CERBERUS_S3_WIFI_SCAN_CAPACITY];
} scan_bank_t;

_Static_assert(sizeof(wifi_ap_record_t) <= 256U, "Review transient SDK scan memory");
_Static_assert(sizeof(scan_bank_t) <= 1536U, "Review bounded S3 scan-bank memory");

static portMUX_TYPE scan_lock = portMUX_INITIALIZER_UNLOCKED;
static cerberus_s3_wifi_scan_status_t scan_status;
static scan_bank_t *scan_retained, *scan_pending;
static wifi_ap_record_t *scan_sdk_rows;
static bool scan_launching, scan_active, scan_cancelled, scan_done;
static uint32_t scan_next_job, scan_started_ms, scan_event_status;

static uint32_t scan_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void scan_copy_status(cerberus_s3_wifi_scan_status_t *out)
{
    *out = scan_status;
    if (scan_active) out->elapsed_ms = scan_now_ms() - scan_started_ms;
    if (scan_retained) {
        out->generation = scan_retained->generation;
        out->completed_ms = scan_retained->completed_ms;
        out->total_count = scan_retained->total_count;
        out->dropped_count = scan_retained->dropped_count;
        out->count = scan_retained->count;
        out->truncated = scan_retained->truncated;
    }
}

esp_err_t cerberus_s3_wifi_scanner_get_status(cerberus_s3_wifi_scan_status_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&scan_lock);
    scan_copy_status(out);
    portEXIT_CRITICAL(&scan_lock);
    return ESP_OK;
}

esp_err_t cerberus_s3_wifi_scanner_get_result(uint32_t generation, uint8_t index,
    cerberus_s3_wifi_scan_row_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    portENTER_CRITICAL(&scan_lock);
    bool valid = generation && scan_retained && scan_retained->generation == generation &&
        index < scan_retained->count;
    if (valid) *out = scan_retained->rows[index];
    portEXIT_CRITICAL(&scan_lock);
    return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t cerberus_s3_wifi_scanner_stop(uint32_t job_id)
{
    if (!job_id) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&scan_lock);
    esp_err_t error = job_id == scan_status.job_id ? ESP_OK : ESP_ERR_INVALID_STATE;
    if (error == ESP_OK && scan_active) {
        scan_cancelled = true;
        scan_status.state = CERBERUS_S3_WIFI_SCAN_STOPPING;
    }
    if (error == ESP_OK && scan_status.cleanup_error != ESP_OK)
        error = scan_status.cleanup_error;
    portEXIT_CRITICAL(&scan_lock);
    return error;
}

static bool scan_should_cancel(void)
{
    portENTER_CRITICAL(&scan_lock);
    bool cancelled = scan_cancelled;
    portEXIT_CRITICAL(&scan_lock);
    return cancelled;
}

static void scan_completed(void *context, esp_event_base_t base, int32_t id, void *event)
{
    if (base != WIFI_EVENT || id != WIFI_EVENT_SCAN_DONE || !event) return;
    const wifi_event_sta_scan_done_t *done = event;
    portENTER_CRITICAL(&scan_lock);
    if (scan_active && scan_status.job_id == (uint32_t)(uintptr_t)context) {
        scan_event_status = done->status;
        scan_done = true;
    }
    portEXIT_CRITICAL(&scan_lock);
}

static bool scan_record_valid(const wifi_ap_record_t *record)
{
    unsigned any = 0U;
    for (unsigned i = 0; i < 6U; ++i) any |= record->bssid[i];
    return any && !(record->bssid[0] & 1U) && record->primary >= 1U && record->primary <= 14U;
}

static uint8_t scan_security(wifi_auth_mode_t authmode)
{
    switch (authmode) {
    case WIFI_AUTH_OPEN: return CERBERUS_WIFI_SECURITY_OPEN;
    case WIFI_AUTH_WEP: return CERBERUS_WIFI_SECURITY_WEP;
    case WIFI_AUTH_WPA_PSK: return CERBERUS_WIFI_SECURITY_WPA;
    case WIFI_AUTH_WPA2_PSK: return CERBERUS_WIFI_SECURITY_WPA2;
    case WIFI_AUTH_WPA_WPA2_PSK: return CERBERUS_WIFI_SECURITY_WPA_WPA2;
    case WIFI_AUTH_WPA3_PSK: return CERBERUS_WIFI_SECURITY_WPA3;
    case WIFI_AUTH_WPA2_WPA3_PSK: return CERBERUS_WIFI_SECURITY_WPA2_WPA3;
    case WIFI_AUTH_OWE: return CERBERUS_WIFI_SECURITY_OWE;
    case WIFI_AUTH_ENTERPRISE:
    case WIFI_AUTH_WPA3_ENT_192:
    case WIFI_AUTH_WPA3_ENTERPRISE:
    case WIFI_AUTH_WPA2_WPA3_ENTERPRISE:
    case WIFI_AUTH_WPA_ENTERPRISE: return CERBERUS_WIFI_SECURITY_ENTERPRISE;
    default: return CERBERUS_WIFI_SECURITY_OTHER;
    }
}

static void scan_convert_rows(scan_bank_t *bank, const wifi_ap_record_t *rows,
    uint16_t count, uint16_t total, uint32_t job_id)
{
    bank->generation = job_id;
    bank->completed_ms = scan_now_ms();
    bank->total_count = total;
    bank->truncated = total > count;
    for (unsigned i = 0; i < count; ++i) {
        const wifi_ap_record_t *record = &rows[i];
        if (!scan_record_valid(record)) continue;
        bool duplicate = false;
        for (unsigned j = 0; j < bank->count; ++j) {
            if (!memcmp(bank->rows[j].bssid, record->bssid, 6U)) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;
        cerberus_s3_wifi_scan_row_t *row = &bank->rows[bank->count++];
        memcpy(row->bssid, record->bssid, sizeof(row->bssid));
        while (row->ssid_length < sizeof(row->ssid) && record->ssid[row->ssid_length]) {
            row->ssid[row->ssid_length] = record->ssid[row->ssid_length];
            ++row->ssid_length;
        }
        row->channel = record->primary;
        row->security = scan_security(record->authmode);
        row->rssi = record->rssi;
        row->source = CERBERUS_S3_WIFI_SCAN_SOURCE_S3;
    }
    bank->dropped_count = total > bank->count ? (uint16_t)(total - bank->count) : 0U;
}

typedef struct {
    bool scan_started, clear_needed, wifi_started, registered, initialized;
    esp_event_handler_instance_t handler;
} scan_cleanup_t;

static esp_err_t scan_cleanup_step(scan_cleanup_t *cleanup)
{
    if (cerberus_s3_wifi_owner() != CERBERUS_S3_WIFI_SCAN) return ESP_ERR_INVALID_STATE;
    esp_err_t error;
    if (cleanup->scan_started) {
        error = esp_wifi_scan_stop();
        if (error != ESP_OK && error != ESP_ERR_WIFI_NOT_STARTED) return error;
        cleanup->scan_started = false;
    }
    if (cleanup->clear_needed) {
        error = esp_wifi_clear_ap_list();
        /* A stopped driver cannot service this API. Deinit below still has
         * to acknowledge resource release before we give up the owner. */
        if (error != ESP_OK && error != ESP_ERR_WIFI_NOT_STARTED) return error;
        cleanup->clear_needed = false;
    }
    if (cleanup->wifi_started) {
        error = esp_wifi_stop();
        if (error != ESP_OK && error != ESP_ERR_WIFI_NOT_STARTED) return error;
        cleanup->wifi_started = false;
    }
    if (cleanup->registered) {
        error = esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, cleanup->handler);
        if (error != ESP_OK) return error;
        cleanup->registered = false;
    }
    if (cleanup->initialized) {
        error = esp_wifi_deinit();
        if (error != ESP_OK) return error;
        cleanup->initialized = false;
    }
    return cerberus_s3_wifi_release(CERBERUS_S3_WIFI_SCAN);
}

static void scan_worker(void *unused)
{
    (void)unused;
    bool initialized = false, started_wifi = false, registered = false, started_scan = false;
    bool complete = false;
    esp_event_handler_instance_t handler = NULL;
    uint32_t job_id = scan_status.job_id;
    scan_bank_t *pending = scan_pending;
    wifi_ap_record_t *records = scan_sdk_rows;
    esp_err_t error = ESP_OK;

    if (!scan_should_cancel()) error = cerberus_s3_wifi_prepare(CERBERUS_S3_WIFI_SCAN);
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    init.static_rx_buf_num = 4;
    init.dynamic_rx_buf_num = 8;
    init.dynamic_tx_buf_num = 4;
    init.rx_ba_win = 4;
    init.ampdu_rx_enable = 0;
    init.ampdu_tx_enable = 0;
    init.amsdu_tx_enable = 0;
    if (error == ESP_OK && !scan_should_cancel()) {
        error = esp_wifi_init(&init);
        initialized = error == ESP_OK;
    }
    if (error == ESP_OK && initialized) error = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (error == ESP_OK && initialized) error = esp_wifi_set_mode(WIFI_MODE_STA);
    if (error == ESP_OK && initialized) {
        error = esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE,
            scan_completed, (void *)(uintptr_t)job_id, &handler);
        registered = error == ESP_OK;
    }
    if (error == ESP_OK && initialized && !scan_should_cancel()) {
        error = esp_wifi_start();
        started_wifi = error == ESP_OK;
    }
    wifi_scan_config_t config = {
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_PASSIVE,
        .scan_time = {.passive = CERBERUS_S3_WIFI_SCAN_PASSIVE_DWELL_MS},
    };
    if (error == ESP_OK && started_wifi && !scan_should_cancel()) {
        error = esp_wifi_scan_start(&config, false);
        started_scan = error == ESP_OK;
    }
    uint32_t waiting_since = scan_now_ms();
    while (error == ESP_OK && started_scan && !scan_should_cancel()) {
        portENTER_CRITICAL(&scan_lock);
        bool done = scan_done;
        uint32_t driver_status = scan_event_status;
        portEXIT_CRITICAL(&scan_lock);
        if (done) {
            error = driver_status ? ESP_FAIL : ESP_OK;
            complete = error == ESP_OK;
            break;
        }
        if ((uint32_t)(scan_now_ms() - waiting_since) >= CERBERUS_S3_WIFI_SCAN_WAIT_MS) {
            error = ESP_ERR_TIMEOUT;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (error == ESP_OK && complete && !scan_should_cancel()) {
        uint16_t total = 0, count = CERBERUS_S3_WIFI_SCAN_CAPACITY;
        error = esp_wifi_scan_get_ap_num(&total);
        if (error == ESP_OK && total) error = esp_wifi_scan_get_ap_records(&count, records);
        else count = 0;
        if (error == ESP_OK && (count > CERBERUS_S3_WIFI_SCAN_CAPACITY || count > total))
            error = ESP_ERR_INVALID_RESPONSE;
        if (error == ESP_OK) scan_convert_rows(pending, records, count, total, job_id);
    }

    scan_cleanup_t remaining = {.scan_started = started_scan && !complete,
        .clear_needed = started_wifi, .wifi_started = started_wifi,
        .registered = registered, .initialized = initialized, .handler = handler};
    esp_err_t cleanup;
    while ((cleanup = scan_cleanup_step(&remaining)) != ESP_OK) {
        /* Keep the same worker and owner until teardown is acknowledged.
         * Successful stages are not repeated against a partly torn-down SDK. */
        portENTER_CRITICAL(&scan_lock);
        scan_status.state = CERBERUS_S3_WIFI_SCAN_STOPPING;
        scan_status.cleanup_error = cleanup;
        portEXIT_CRITICAL(&scan_lock);
        vTaskDelay(pdMS_TO_TICKS(CERBERUS_S3_WIFI_SCAN_CLEANUP_RETRY_MS));
    }

    scan_bank_t *old = NULL;
    portENTER_CRITICAL(&scan_lock);
    bool cancelled = scan_cancelled;
    if (error == ESP_OK && complete && !cancelled) {
        old = scan_retained;
        scan_retained = pending;
        pending = NULL;
    }
    scan_pending = NULL;
    scan_sdk_rows = NULL;
    scan_status.error_code = error;
    scan_status.cleanup_error = cleanup;
    scan_status.radio_owned = cleanup != ESP_OK;
    scan_status.elapsed_ms = scan_now_ms() - scan_started_ms;
    scan_status.state = error != ESP_OK ? CERBERUS_S3_WIFI_SCAN_ERROR :
        cancelled ? CERBERUS_S3_WIFI_SCAN_CANCELLED : CERBERUS_S3_WIFI_SCAN_COMPLETE;
    scan_active = false;
    portEXIT_CRITICAL(&scan_lock);
    heap_caps_free(records);
    heap_caps_free(pending);
    heap_caps_free(old);
    vTaskDelete(NULL);
}

esp_err_t cerberus_s3_wifi_scanner_start(cerberus_s3_wifi_scan_status_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    portENTER_CRITICAL(&scan_lock);
    bool busy = scan_active || scan_launching || scan_status.radio_owned || scan_next_job == UINT32_MAX;
    if (!busy) scan_launching = true;
    portEXIT_CRITICAL(&scan_lock);
    if (busy) return ESP_ERR_INVALID_STATE;

    esp_err_t error = cerberus_s3_wifi_claim(CERBERUS_S3_WIFI_SCAN);
    bool claimed = error == ESP_OK;
    if (error == ESP_OK && cerberus_s3_ble_host_owner() != CERBERUS_S3_BLE_OWNER_NONE)
        error = ESP_ERR_INVALID_STATE;
    if (error == ESP_OK && heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 65536U)
        error = ESP_ERR_NO_MEM;
    scan_bank_t *pending = NULL;
    wifi_ap_record_t *records = NULL;
    if (error == ESP_OK) {
        pending = heap_caps_calloc(1, sizeof(*pending), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        records = heap_caps_calloc(CERBERUS_S3_WIFI_SCAN_CAPACITY, sizeof(*records),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!pending || !records) error = ESP_ERR_NO_MEM;
    }
    if (error != ESP_OK) {
        heap_caps_free(pending);
        heap_caps_free(records);
        esp_err_t cleanup = claimed ? cerberus_s3_wifi_release(CERBERUS_S3_WIFI_SCAN) : ESP_OK;
        portENTER_CRITICAL(&scan_lock);
        if (cleanup != ESP_OK) {
            scan_status.state = CERBERUS_S3_WIFI_SCAN_ERROR;
            scan_status.error_code = error;
            scan_status.cleanup_error = cleanup;
            scan_status.radio_owned = true;
            scan_copy_status(out);
        }
        scan_launching = false;
        portEXIT_CRITICAL(&scan_lock);
        return error;
    }
    portENTER_CRITICAL(&scan_lock);
    ++scan_next_job;
    scan_status = (cerberus_s3_wifi_scan_status_t){.job_id = scan_next_job,
        .state = CERBERUS_S3_WIFI_SCAN_RUNNING, .radio_owned = true};
    scan_started_ms = scan_now_ms();
    scan_pending = pending;
    scan_sdk_rows = records;
    scan_cancelled = scan_done = false;
    scan_event_status = 0;
    scan_active = true;
    scan_launching = false;
    portEXIT_CRITICAL(&scan_lock);
    if (xTaskCreate(scan_worker, "s3_wifi_scan", 4096, NULL, 3, NULL) != pdPASS) {
        esp_err_t cleanup = cerberus_s3_wifi_release(CERBERUS_S3_WIFI_SCAN);
        portENTER_CRITICAL(&scan_lock);
        scan_pending = NULL;
        scan_sdk_rows = NULL;
        scan_active = false;
        scan_status.state = CERBERUS_S3_WIFI_SCAN_ERROR;
        scan_status.error_code = ESP_ERR_NO_MEM;
        scan_status.cleanup_error = cleanup;
        scan_status.radio_owned = cleanup != ESP_OK;
        portEXIT_CRITICAL(&scan_lock);
        heap_caps_free(pending);
        heap_caps_free(records);
        error = ESP_ERR_NO_MEM;
    }
    (void)cerberus_s3_wifi_scanner_get_status(out);
    return error;
}

size_t cerberus_s3_wifi_scanner_ssid_display(const cerberus_s3_wifi_scan_row_t *row,
    char *out, size_t capacity)
{
    if (capacity && !out) return 0U;
    if (capacity) out[0] = '\0';
    if (!row || row->ssid_length > sizeof(row->ssid)) return 0U;
    static const char hex[] = "0123456789ABCDEF";
    size_t used = 0U, written = 0U;
    bool full = false;
    for (unsigned i = 0; i < row->ssid_length; ++i) {
        uint8_t value = row->ssid[i];
        char token[4] = {'\\', 'x', hex[value >> 4U], hex[value & 15U]};
        unsigned count = 4U;
        if (value >= 0x20U && value <= 0x7eU && value != '\\') {
            token[0] = (char)value;
            count = 1U;
        }
        if (capacity && !full) {
            if (count <= capacity - 1U - written) {
                memcpy(out + written, token, count);
                written += count;
            } else full = true;
        }
        used += count;
    }
    if (capacity) out[written] = '\0';
    return used;
}
