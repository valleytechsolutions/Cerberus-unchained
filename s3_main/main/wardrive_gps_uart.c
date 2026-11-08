#include "wardrive_gps_uart.h"
#include "settings.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <limits.h>
#include <stdatomic.h>
#include <string.h>

#define GPS_UART UART_NUM_2
#define GPS_RX_BUFFER_BYTES CERBERUS_WARDRIVE_GPS_RX_BUFFER_BYTES
#define GPS_TASK_STACK_BYTES CERBERUS_WARDRIVE_GPS_TASK_STACK_BYTES
#define GPS_READ_CHUNK_BYTES 256U
#define GPS_READ_WAIT_MS 20U
#define GPS_STOP_GRACE_MS 1000U
#define GPS_SNAPSHOT_WAIT_MS 25U

typedef struct {
    cerberus_wardrive_gps_trace_row_t rows[CERBERUS_WARDRIVE_GPS_TRACE_CAPACITY];
    uint32_t observed_lines, evicted_lines, overflow_lines, discarded_lines;
    uint8_t count, head;
} gps_trace_store_t;
static SemaphoreHandle_t gps_lock;
static cerberus_wardrive_gps_core_t gps_core;
static bool uart_active, task_running, stop_requested, start_in_progress;
static esp_err_t last_uart_error;
static uint64_t receiver_epoch, clock_highwater;
static bool clock_seen, receipt_clock_valid;
static gps_trace_store_t *trace_store;
static uint64_t trace_token, trace_token_counter, trace_sequence;
static cerberus_gps_config_t receiver_config;
static const uint32_t detect_bauds[]={115200U,9600U,38400U,57600U,19200U,4800U,230400U,460800U,921600U};
static uint32_t active_baud,received_bytes,baud_changes;
static uint64_t baud_started_ms;
static uint8_t baud_index;
static bool detecting_baud;
static atomic_flag gps_control=ATOMIC_FLAG_INIT;
static unsigned gps_consumers;
static esp_err_t gps_native_stop(void);

static void increment(uint32_t *value)
{ if (*value != UINT32_MAX) ++*value; }

/* gps_lock held. Invalid clocks never refresh source freshness. */
static bool clock_ms(uint64_t *out)
{
    int64_t raw = esp_timer_get_time();
    if (raw < 0) { *out = UINT64_MAX; return false; }
    uint64_t value = (uint64_t)(raw / 1000LL);
    if (clock_seen && value < clock_highwater) { *out = UINT64_MAX; return false; }
    clock_seen = true; clock_highwater = value; *out = value;
    return true;
}

static void observe_line(void *unused, const uint8_t *payload,
    uint8_t retained_length, uint32_t total_length, uint64_t received_ms,
    cerberus_wardrive_gps_line_result_t result,
    cerberus_wardrive_gps_termination_t termination, bool payload_complete)
{
    (void)unused; /* Synchronous core callback, under existing gps_lock. */
    if (!trace_store) return;
    increment(&trace_store->observed_lines);
    if (result == CERBERUS_WARDRIVE_GPS_LINE_OVERFLOW) increment(&trace_store->overflow_lines);
    if (result == CERBERUS_WARDRIVE_GPS_LINE_OVERFLOW || result == CERBERUS_WARDRIVE_GPS_LINE_CONTROL ||
        result == CERBERUS_WARDRIVE_GPS_LINE_FRAME_INVALID) increment(&trace_store->discarded_lines);
    if (trace_sequence == UINT64_MAX) return;
    uint8_t index = (uint8_t)((trace_store->head + trace_store->count) % CERBERUS_WARDRIVE_GPS_TRACE_CAPACITY);
    if (trace_store->count == CERBERUS_WARDRIVE_GPS_TRACE_CAPACITY) {
        index = trace_store->head;
        trace_store->head = (uint8_t)((trace_store->head + 1U) % CERBERUS_WARDRIVE_GPS_TRACE_CAPACITY);
        increment(&trace_store->evicted_lines);
    } else ++trace_store->count;
    cerberus_wardrive_gps_trace_row_t *row = &trace_store->rows[index];
    memset(row, 0, sizeof(*row));
    row->sequence = ++trace_sequence; row->receiver_epoch = receiver_epoch;
    row->received_ms = receipt_clock_valid ? received_ms : 0U;
    row->total_length = total_length; row->retained_length = retained_length;
    row->result = (uint8_t)result; row->termination = (uint8_t)termination;
    row->flags = (payload_complete ? CERBERUS_WARDRIVE_GPS_TRACE_PAYLOAD_COMPLETE : 0U) |
        (receipt_clock_valid ? CERBERUS_WARDRIVE_GPS_TRACE_RECEIPT_VALID : 0U);
    if (retained_length) memcpy(row->payload, payload, retained_length);
}

static void gps_reader(void *unused)
{
    (void)unused;
    uint8_t bytes[GPS_READ_CHUNK_BYTES];
    for (;;) {
        xSemaphoreTake(gps_lock, portMAX_DELAY);
        bool stopping = stop_requested;
        xSemaphoreGive(gps_lock);
        if (stopping) break;
        int count = uart_read_bytes(GPS_UART, bytes, sizeof(bytes), pdMS_TO_TICKS(GPS_READ_WAIT_MS));
        xSemaphoreTake(gps_lock, portMAX_DELAY);
        if (count < 0 || count > (int)sizeof(bytes)) {
            last_uart_error = count < 0 ? ESP_FAIL : ESP_ERR_INVALID_RESPONSE;
            stop_requested = true;
            xSemaphoreGive(gps_lock);
            break;
        }
        if (count > 0 && !stop_requested) {
            uint64_t received_ms;
            receipt_clock_valid = clock_ms(&received_ms);
            cerberus_wardrive_gps_core_feed_observed(&gps_core, bytes, (size_t)count,
                received_ms, trace_store ? observe_line : NULL, NULL);
            received_bytes=UINT32_MAX-received_bytes<(uint32_t)count?UINT32_MAX:received_bytes+(uint32_t)count;
        }
        uint64_t probe_now=0U;
        bool probe_clock=clock_ms(&probe_now);
        if(detecting_baud&&gps_core.valid_sentences>=2U)detecting_baud=false;
        /* A cold candidate gets two seconds, long enough for a1Hz receiver.
         * A verified baud is retained through loss of fix; only loss of NMEA
         * for10s resumes discovery. This does not change receiver settings. */
        if(!receiver_config.baud&&!detecting_baud&&probe_clock&&gps_core.sentence_seen&&
            probe_now>=gps_core.last_sentence_ms&&probe_now-gps_core.last_sentence_ms>=10000U){
            detecting_baud=true;baud_started_ms=probe_now;
            cerberus_wardrive_gps_core_reset(&gps_core);
        }
        bool retune=!stop_requested&&detecting_baud&&probe_clock&&
            probe_now>=baud_started_ms&&probe_now-baud_started_ms>=2000U;
        uint32_t next_baud=active_baud;
        if(retune){
            baud_index=(uint8_t)((baud_index+1U)%(sizeof(detect_bauds)/sizeof(detect_bauds[0])));
            next_baud=detect_bauds[baud_index];
        }
        xSemaphoreGive(gps_lock);
        if(retune){
            esp_err_t changed=uart_set_baudrate(GPS_UART,next_baud);
            if(changed==ESP_OK)changed=uart_flush_input(GPS_UART);
            xSemaphoreTake(gps_lock,portMAX_DELAY);
            if(changed!=ESP_OK){last_uart_error=changed;stop_requested=true;}
            else {active_baud=next_baud;increment(&baud_changes);baud_started_ms=probe_now;
                cerberus_wardrive_gps_core_reset(&gps_core);}
            xSemaphoreGive(gps_lock);
        }
    }
    xSemaphoreTake(gps_lock, portMAX_DELAY); task_running = false; xSemaphoreGive(gps_lock);
    vTaskDelete(NULL);
}

static esp_err_t setup_failed(esp_err_t error)
{
    xSemaphoreTake(gps_lock, portMAX_DELAY);
    last_uart_error = error; start_in_progress = false; stop_requested = true;
    xSemaphoreGive(gps_lock);
    esp_err_t cleanup = gps_native_stop();
    return cleanup != ESP_OK ? cleanup : error;
}

static esp_err_t gps_native_start(void)
{
    cerberus_gps_config_t selected=cerberus_settings_gps_get();
    if(!cerberus_gps_config_valid(&selected))return ESP_ERR_INVALID_ARG;
    /* Integrated Wardriving serializes first-use mutex initialization. */
    if (!gps_lock) {
        gps_lock = xSemaphoreCreateMutex();
        if (!gps_lock) return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(gps_lock, portMAX_DELAY);
    bool cleanup_needed = uart_active && stop_requested && !start_in_progress;
    xSemaphoreGive(gps_lock);
    if (cleanup_needed) {
        esp_err_t cleanup = gps_native_stop();
        if (cleanup != ESP_OK) return cleanup;
    }
    xSemaphoreTake(gps_lock, portMAX_DELAY);
    if (uart_active || task_running || start_in_progress || receiver_epoch == UINT64_MAX) {
        xSemaphoreGive(gps_lock); return ESP_ERR_INVALID_STATE;
    }
    start_in_progress = true; stop_requested = false; last_uart_error = ESP_OK;
    xSemaphoreGive(gps_lock);
    esp_err_t error = uart_driver_install(GPS_UART, GPS_RX_BUFFER_BYTES, 0, 0, NULL, 0);
    if (error != ESP_OK) {
        xSemaphoreTake(gps_lock, portMAX_DELAY);
        start_in_progress = false; last_uart_error = error;
        xSemaphoreGive(gps_lock); return error;
    }
    xSemaphoreTake(gps_lock, portMAX_DELAY);
    uart_active = true; /* Retain driver ownership through every later failure. */
    ++receiver_epoch; clock_seen = false;
    receiver_config=selected;baud_index=0U;
    active_baud=selected.baud?selected.baud:detect_bauds[0];
    detecting_baud=selected.baud==0U;received_bytes=baud_changes=0U;
    baud_started_ms=(uint64_t)(esp_timer_get_time()/1000LL);
    cerberus_wardrive_gps_core_reset(&gps_core);
    if (trace_store) memset(trace_store, 0, sizeof(*trace_store));
    xSemaphoreGive(gps_lock);
    const uart_config_t config = {.baud_rate=(int)active_baud,.data_bits=UART_DATA_8_BITS,
        .parity=UART_PARITY_DISABLE,.stop_bits=UART_STOP_BITS_1,
        .flow_ctrl=UART_HW_FLOWCTRL_DISABLE,.source_clk=UART_SCLK_DEFAULT};
    error = uart_param_config(GPS_UART, &config);
    if (error == ESP_OK) error = uart_set_pin(GPS_UART, UART_PIN_NO_CHANGE, selected.rx_gpio,
        UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (error == ESP_OK) error = uart_flush_input(GPS_UART);
    if (error != ESP_OK) return setup_failed(error);
    xSemaphoreTake(gps_lock, portMAX_DELAY);
    bool cancelled = stop_requested;
    if (!cancelled) task_running = true;
    xSemaphoreGive(gps_lock);
    if (cancelled) return setup_failed(ESP_ERR_INVALID_STATE);
    if (xTaskCreate(gps_reader, "wardrive_gps", GPS_TASK_STACK_BYTES, NULL, 3, NULL) != pdPASS) {
        xSemaphoreTake(gps_lock, portMAX_DELAY); task_running = false; xSemaphoreGive(gps_lock);
        return setup_failed(ESP_ERR_NO_MEM);
    }
    xSemaphoreTake(gps_lock, portMAX_DELAY); start_in_progress = false; xSemaphoreGive(gps_lock);
    return ESP_OK;
}

static esp_err_t gps_native_stop(void)
{
    if (!gps_lock) return ESP_OK;
    xSemaphoreTake(gps_lock, portMAX_DELAY);
    stop_requested = true;
    bool active = uart_active, starting = start_in_progress;
    xSemaphoreGive(gps_lock);
    if (starting) return ESP_ERR_NOT_FINISHED;
    if (!active) return ESP_OK;
    /* Bounded polls also terminate on an invalid/backward system clock. */
    for (unsigned waited = 0U;; waited += 20U) {
        xSemaphoreTake(gps_lock, portMAX_DELAY);
        bool running = task_running;
        xSemaphoreGive(gps_lock);
        if (!running) break;
        if (waited >= GPS_STOP_GRACE_MS) {
            xSemaphoreTake(gps_lock, portMAX_DELAY); last_uart_error = ESP_ERR_TIMEOUT; xSemaphoreGive(gps_lock);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    esp_err_t error = uart_driver_delete(GPS_UART);
    xSemaphoreTake(gps_lock, portMAX_DELAY);
    if (error == ESP_OK) { uart_active = false; stop_requested = false; }
    else last_uart_error = error;
    xSemaphoreGive(gps_lock);
    return error;
}

/* Session and screen preview share one reader. A screen never stops the GPS
 * beneath a logger, and starting logging preserves the already acquired fix.
 * Failed cleanup retains its consumer; only that consumer can retry release. */
static esp_err_t gps_consumer_set(unsigned consumer,bool enabled)
{
    if(atomic_flag_test_and_set_explicit(&gps_control,memory_order_acquire))return ESP_ERR_NOT_FINISHED;
    esp_err_t error=ESP_OK;
    bool healthy=false,cleanup=false;
    if(gps_lock){
        xSemaphoreTake(gps_lock,portMAX_DELAY);
        healthy=uart_active&&task_running&&!stop_requested;
        cleanup=stop_requested;
        xSemaphoreGive(gps_lock);
    }
    if(enabled){
        if(gps_consumers==consumer&&cleanup)error=gps_native_start();
        else if(gps_consumers&consumer)error=ESP_ERR_INVALID_STATE;
        else if(!gps_consumers)error=gps_native_start();
        else if(!healthy)error=ESP_ERR_INVALID_STATE;
        if(error==ESP_OK)gps_consumers|=consumer;
        /* A failed native start can retain cleanup ownership. */
        else if(!gps_consumers&&gps_lock){
            xSemaphoreTake(gps_lock,portMAX_DELAY);
            if(uart_active||task_running||start_in_progress)gps_consumers|=consumer;
            xSemaphoreGive(gps_lock);
        }
    }else if(gps_consumers&consumer){
        if(gps_consumers==consumer)error=gps_native_stop();
        if(error==ESP_OK)gps_consumers&=~consumer;
    }
    atomic_flag_clear_explicit(&gps_control,memory_order_release);
    return error;
}
esp_err_t cerberus_wardrive_gps_uart_start(void){return gps_consumer_set(1U,true);}
esp_err_t cerberus_wardrive_gps_uart_stop(void){return gps_consumer_set(1U,false);}
esp_err_t cerberus_wardrive_gps_uart_preview(bool enabled){return gps_consumer_set(2U,enabled);}
esp_err_t cerberus_wardrive_gps_uart_record(bool enabled){return gps_consumer_set(4U,enabled);}

esp_err_t cerberus_wardrive_gps_uart_recent_time(cerberus_wardrive_gps_time_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!gps_lock) return ESP_ERR_NOT_FOUND;
    if (xSemaphoreTake(gps_lock, pdMS_TO_TICKS(GPS_SNAPSHOT_WAIT_MS)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    uint64_t now;
    bool valid = clock_ms(&now) && receipt_clock_valid && last_uart_error == ESP_OK &&
        gps_core.fix_live && gps_core.utc_seen &&
        gps_core.last_fix_ms >= gps_core.last_utc_ms &&
        gps_core.last_fix_ms - gps_core.last_utc_ms <= 1000U && now >= gps_core.last_utc_ms &&
        now - gps_core.last_utc_ms <= CERBERUS_WARDRIVE_GPS_TIME_HOLDOVER_MS;
    if (valid) {
        memcpy(out->utc, gps_core.utc, sizeof(out->utc));
        out->received_ms = gps_core.last_utc_ms;
    }
    xSemaphoreGive(gps_lock);
    return valid ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static void snapshot_locked(cerberus_wardrive_gps_snapshot_t *out, bool *valid_clock)
{
    uint64_t now;
    bool valid = clock_ms(&now);
    cerberus_wardrive_gps_core_snapshot(&gps_core, now, out);
    if (!valid || !uart_active || !task_running || start_in_progress || stop_requested) {
        uint32_t sentences=out->valid_sentences, bad=out->bad_checksums, malformed=out->malformed_sentences;
        uint32_t overflow=out->overflow_lines, fixes=out->fix_updates;
        cerberus_wardrive_gps_core_snapshot(NULL, 0U, out);
        out->valid_sentences=sentences; out->bad_checksums=bad; out->malformed_sentences=malformed;
        out->overflow_lines=overflow; out->fix_updates=fixes;
    }
    if (valid_clock) *valid_clock = valid;
}

esp_err_t cerberus_wardrive_gps_uart_get_state(cerberus_wardrive_gps_uart_state_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out)); cerberus_wardrive_gps_core_snapshot(NULL, 0U, &out->gps);
    if (!gps_lock) return ESP_OK;
    if (xSemaphoreTake(gps_lock, pdMS_TO_TICKS(GPS_SNAPSHOT_WAIT_MS)) != pdTRUE) return ESP_ERR_TIMEOUT;
    snapshot_locked(&out->gps, &out->clock_valid);
    out->receiver_epoch=receiver_epoch; out->driver_active=uart_active; out->reader_running=task_running;
    out->starting=start_in_progress; out->stopping=stop_requested && (uart_active || task_running || start_in_progress);
    out->error=uart_active || task_running || start_in_progress ? last_uart_error : ESP_OK;
    out->historical_error=last_uart_error;
    out->config=receiver_config;out->active_baud=active_baud;
    out->received_bytes=received_bytes;out->baud_changes=baud_changes;out->detecting_baud=detecting_baud;
    xSemaphoreGive(gps_lock); return ESP_OK;
}

void cerberus_wardrive_gps_uart_snapshot(cerberus_wardrive_gps_snapshot_t *out, esp_err_t *uart_error)
{
    if (!out) return;
    cerberus_wardrive_gps_core_snapshot(NULL, 0U, out);
    if (uart_error) *uart_error=ESP_OK;
    if (!gps_lock) return;
    xSemaphoreTake(gps_lock, portMAX_DELAY);
    snapshot_locked(out, NULL);
    if (uart_error && (uart_active || task_running || start_in_progress)) *uart_error=last_uart_error;
    xSemaphoreGive(gps_lock);
}

esp_err_t cerberus_wardrive_gps_uart_trace_begin(uint64_t *observer_token)
{
    if (!observer_token) return ESP_ERR_INVALID_ARG;
    *observer_token=0U;
    if (!gps_lock) return ESP_ERR_NOT_FINISHED;
    if (xSemaphoreTake(gps_lock, pdMS_TO_TICKS(GPS_SNAPSHOT_WAIT_MS)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    bool ready=uart_active && task_running && !stop_requested && !start_in_progress;
    bool busy=trace_store || trace_token_counter==UINT64_MAX;
    uint64_t epoch=receiver_epoch;
    xSemaphoreGive(gps_lock);
    if (!ready) return ESP_ERR_NOT_FINISHED;
    if (busy) return ESP_ERR_INVALID_STATE;
    gps_trace_store_t *candidate=heap_caps_calloc(1U,sizeof(*candidate),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if (!candidate) return ESP_ERR_NO_MEM;
    if (xSemaphoreTake(gps_lock,pdMS_TO_TICKS(GPS_SNAPSHOT_WAIT_MS))!=pdTRUE) {
        heap_caps_free(candidate); return ESP_ERR_TIMEOUT;
    }
    ready=uart_active && task_running && !stop_requested && !start_in_progress && receiver_epoch==epoch;
    busy=trace_store || trace_token_counter==UINT64_MAX;
    if (ready && !busy) {trace_store=candidate;trace_token=++trace_token_counter;*observer_token=trace_token;}
    xSemaphoreGive(gps_lock);
    if (!*observer_token) heap_caps_free(candidate);
    return *observer_token ? ESP_OK : ready ? ESP_ERR_INVALID_STATE : ESP_ERR_NOT_FINISHED;
}

esp_err_t cerberus_wardrive_gps_uart_trace_read(uint64_t observer_token, bool mask,
    cerberus_wardrive_gps_trace_snapshot_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out,0,sizeof(*out));
    if (!observer_token || !gps_lock) return ESP_ERR_NOT_FOUND;
    if (xSemaphoreTake(gps_lock,pdMS_TO_TICKS(GPS_SNAPSHOT_WAIT_MS))!=pdTRUE) return ESP_ERR_TIMEOUT;
    if (!trace_store || trace_token!=observer_token) {xSemaphoreGive(gps_lock);return ESP_ERR_NOT_FOUND;}
    out->observer_token=trace_token;out->receiver_epoch=receiver_epoch;
    out->observed_lines=trace_store->observed_lines;out->evicted_lines=trace_store->evicted_lines;
    out->overflow_lines=trace_store->overflow_lines;out->discarded_lines=trace_store->discarded_lines;
    out->count=trace_store->count;out->capturing=uart_active && task_running && !start_in_progress &&
        !stop_requested && trace_sequence!=UINT64_MAX;
    out->sequence_exhausted=trace_sequence==UINT64_MAX;
    for (uint8_t i=0U;i<trace_store->count;++i) {
        out->rows[i]=trace_store->rows[(trace_store->head+i)%CERBERUS_WARDRIVE_GPS_TRACE_CAPACITY];
        if (mask) {memset(out->rows[i].payload,0,sizeof(out->rows[i].payload));out->rows[i].retained_length=0U;
            out->rows[i].flags|=CERBERUS_WARDRIVE_GPS_TRACE_MASKED;}
    }
    xSemaphoreGive(gps_lock);return ESP_OK;
}

esp_err_t cerberus_wardrive_gps_uart_trace_end(uint64_t observer_token)
{
    if (!observer_token || !gps_lock) return ESP_ERR_NOT_FOUND;
    if (xSemaphoreTake(gps_lock,pdMS_TO_TICKS(GPS_SNAPSHOT_WAIT_MS))!=pdTRUE) return ESP_ERR_TIMEOUT;
    if (!trace_store || observer_token!=trace_token) {xSemaphoreGive(gps_lock);return ESP_ERR_NOT_FOUND;}
    gps_trace_store_t *released=trace_store;trace_store=NULL;trace_token=0U;
    xSemaphoreGive(gps_lock);heap_caps_free(released);return ESP_OK;
}
