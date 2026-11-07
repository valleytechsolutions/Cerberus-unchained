/* Included by the monitor service. scanner_mutex owns commands and lifetime;
 * packet_monitor_mux fences the RX pointer and its in-flight callback count. */
#include "wifi_packet_monitor_record.h"

typedef struct {
    cerberus_monitor_record_store_t store;
    int64_t retention_us, clock_floor_us;
    uint32_t inflight;
    bool release_requested;
} packet_monitor_record_context_t;

static packet_monitor_record_context_t *packet_monitor_record_context;
static bool packet_monitor_record_requested;
static uint64_t packet_monitor_record_released_session;
static bool packet_monitor_held_locked(void);

static bool packet_monitor_record_retained(void)
{
    portENTER_CRITICAL(&packet_monitor_mux);
    bool retained = packet_monitor_record_context != NULL;
    portEXIT_CRITICAL(&packet_monitor_mux);
    return retained;
}

static bool packet_monitor_record_callbacks_idle(void)
{
    portENTER_CRITICAL(&packet_monitor_mux);
    bool idle = !packet_monitor_record_context || !packet_monitor_record_context->inflight;
    portEXIT_CRITICAL(&packet_monitor_mux);
    return idle;
}

static void packet_monitor_record_close_admission(void)
{
    portENTER_CRITICAL(&packet_monitor_mux);
    packet_monitor_receiving = false;
    packet_monitor_record_context_t *c = packet_monitor_record_context;
    portEXIT_CRITICAL(&packet_monitor_mux);
    if (c) cerberus_monitor_record_stop(&c->store);
}

static bool packet_monitor_record_faulted(void)
{
    packet_monitor_record_context_t *c = packet_monitor_record_context;
    return c && (atomic_load_explicit(&c->store.clock_invalid, memory_order_acquire) ||
        atomic_load_explicit(&c->store.exhausted, memory_order_acquire));
}

/* Called with scanner_mutex. Expiry never renews an old lease or frees a
 * receiver still in cleanup. Only the worker retires callback-visible storage. */
static void packet_monitor_record_clock_locked(int64_t now, bool renew)
{
    packet_monitor_record_context_t *c = packet_monitor_record_context;
    if (!c) return;
    bool bad = now < c->clock_floor_us || now < 0 ||
        now > INT64_MAX - (int64_t)CERBERUS_MONITOR_RECORD_RETENTION_MS * 1000;
    if (bad) {
        atomic_store_explicit(&c->store.clock_invalid, true, memory_order_release);
        atomic_store_explicit(&packet_monitor_cancelled, true, memory_order_release);
        c->release_requested = true;
    } else {
        c->clock_floor_us = now;
        if (!packet_monitor_held_locked()) {
            if (c->retention_us && now >= c->retention_us) c->release_requested = true;
            if (!c->release_requested && (!c->retention_us || renew))
                c->retention_us = now + (int64_t)CERBERUS_MONITOR_RECORD_RETENTION_MS * 1000;
        }
    }
}

static void packet_monitor_record_worker_tick(void)
{
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    packet_monitor_record_clock_locked(esp_timer_get_time(), false);
    packet_monitor_record_context_t *c = packet_monitor_record_context;
    if (c && c->release_requested && !packet_monitor_held_locked() &&
        packet_monitor_record_callbacks_idle()) {
        packet_monitor_record_released_session = c->store.session;
        portENTER_CRITICAL(&packet_monitor_mux);
        packet_monitor_record_context = NULL;
        portEXIT_CRITICAL(&packet_monitor_mux);
    } else c = NULL;
    xSemaphoreGive(scanner_mutex);
    if (c) heap_caps_free(c);
}

static esp_err_t packet_monitor_record_create_locked(uint64_t session, int64_t now)
{
    if (packet_monitor_record_context || !session || now < 0) return ESP_ERR_INVALID_STATE;
    size_t bytes = sizeof(packet_monitor_record_context_t);
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < bytes + 32768U)
        return ESP_ERR_NO_MEM;
    packet_monitor_record_context_t *c = heap_caps_calloc(1U, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!c) return ESP_ERR_NO_MEM;
    if (!cerberus_monitor_record_init(&c->store, session)) {
        heap_caps_free(c);
        return ESP_ERR_INVALID_STATE;
    }
    c->clock_floor_us = now;
    portENTER_CRITICAL(&packet_monitor_mux);
    packet_monitor_record_context = c;
    portEXIT_CRITICAL(&packet_monitor_mux);
    return ESP_OK;
}

static void packet_monitor_record_reply_locked(uint64_t session, esp_err_t error,
    cerberus_packet_count_control_t *out)
{
    memset(out, 0, sizeof(*out));
    out->session_id = session;
    out->error_code = error;
    packet_monitor_record_context_t *c = packet_monitor_record_context;
    if (!c || !session || session != c->store.session) return;
    out->flags = CERBERUS_PACKET_COUNT_BOUND | CERBERUS_PACKET_COUNT_RECORDING;
    bool held = packet_monitor_held_locked();
    if (held) out->flags |= CERBERUS_PACKET_COUNT_HELD;
    portENTER_CRITICAL(&packet_monitor_mux);
    bool receiving = packet_monitor_receiving;
    portEXIT_CRITICAL(&packet_monitor_mux);
    if (receiving) out->flags |= CERBERUS_PACKET_COUNT_RX_ACTIVE;
    if (cerberus_wifi_capture_ring_peek(&c->store.ring)) out->flags |= CERBERUS_PACKET_COUNT_DATA_READY;
    out->state = held ? receiving ? CERBERUS_PACKET_COUNT_RUNNING : CERBERUS_PACKET_COUNT_STOPPING :
        packet_monitor_status.error_code == ESP_OK ? CERBERUS_PACKET_COUNT_COMPLETE : CERBERUS_PACKET_COUNT_ERROR;
    out->channel = packet_monitor_status.channel;
    if (error == ESP_OK) out->error_code = packet_monitor_status.error_code;
}

esp_err_t cerberus_wifi_packet_monitor_record_exchange(uint8_t command,
    const void *request, size_t request_size, void *reply, size_t reply_size)
{
    bool raw = command == CERBERUS_MONITOR_RECORD_RAW_READ;
    if (!reply || reply_size != (raw ? sizeof(cerberus_packet_count_raw_chunk_t) :
        sizeof(cerberus_packet_count_control_t))) return ESP_ERR_INVALID_ARG;
    memset(reply, 0, reply_size);
    if (!request || request_size != cerberus_monitor_record_request_bytes(command))
        return ESP_ERR_INVALID_ARG;
    uint64_t session;
    memcpy(&session, request, sizeof(session));
    if (!scanner_mutex) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    packet_monitor_record_clock_locked(esp_timer_get_time(), false);
    packet_monitor_record_context_t *c = packet_monitor_record_context;
    bool bound = c && session && c->store.session == session && !c->release_requested;
    esp_err_t error = bound ? ESP_OK : ESP_ERR_NOT_FOUND;
    uint32_t pin = 0U;
    if (command == CERBERUS_MONITOR_RECORD_RELEASE) {
        if (c && session == c->store.session) {
            if (packet_monitor_held_locked() || !packet_monitor_record_callbacks_idle()) error = ESP_ERR_INVALID_STATE;
            else { c->release_requested = true; error = ESP_OK; }
        } else if (session && session == packet_monitor_record_released_session) error = ESP_OK;
    } else if (bound && command == CERBERUS_MONITOR_RECORD_PEEK) {
        cerberus_packet_count_peek_t q;
        memcpy(&q, request, sizeof(q));
        if (q.index || (q.kind != CERBERUS_PACKET_COUNT_READ_RAW_META &&
            q.kind != CERBERUS_PACKET_COUNT_READ_SUMMARY)) error = ESP_ERR_INVALID_ARG;
        else if (!cerberus_monitor_record_pin(&c->store, session,
            q.kind == CERBERUS_PACKET_COUNT_READ_SUMMARY, &pin))
            error = q.kind == CERBERUS_PACKET_COUNT_READ_RAW_META &&
                !cerberus_wifi_capture_ring_peek(&c->store.ring) ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_STATE;
    } else if (bound && command == CERBERUS_MONITOR_RECORD_RAW_ACK) {
        cerberus_packet_count_ack_t q;
        memcpy(&q, request, sizeof(q));
        if (cerberus_monitor_record_ack(&c->store, session, q.sequence)) pin = q.sequence;
        else error = ESP_ERR_INVALID_STATE;
    }
    if (command == CERBERUS_MONITOR_RECORD_READ || raw) {
        cerberus_packet_count_read_t q;
        memcpy(&q, request, sizeof(q));
        uint8_t *bytes = reply;
        memcpy(bytes, &session, 8U);
        memcpy(bytes + 12U, &q.part, 2U);
        if (bound && error == ESP_OK && !cerberus_monitor_record_read(&c->store,
            session, q.pin, q.part, raw, bytes + 14U)) error = ESP_ERR_INVALID_STATE;
        if (error == ESP_OK) memcpy(bytes + 8U, &q.pin, 4U);
        else cerberus_packet_count_put(bytes + 14U, (uint32_t)error, 4U);
    } else {
        packet_monitor_record_reply_locked(session, error, reply);
        cerberus_packet_count_control_t *out = reply;
        if (command == CERBERUS_MONITOR_RECORD_PEEK || command == CERBERUS_MONITOR_RECORD_RAW_ACK)
            out->sequence = error == ESP_OK ? pin : 0U;
        if (command == CERBERUS_MONITOR_RECORD_RELEASE && error == ESP_OK) {
            /* A release request is not a completed release until worker disposal. */
            if (c && session == c->store.session) out->flags |= CERBERUS_PACKET_COUNT_BOUND;
            else { memset(out, 0, sizeof(*out)); out->session_id = session; }
        }
    }
    if (bound && (error == ESP_OK || error == ESP_ERR_NOT_FOUND) &&
        (command == CERBERUS_MONITOR_RECORD_STATUS || command == CERBERUS_MONITOR_RECORD_PEEK ||
         command == CERBERUS_MONITOR_RECORD_READ || raw))
        packet_monitor_record_clock_locked(esp_timer_get_time(), true);
    xSemaphoreGive(scanner_mutex);
    if (command == CERBERUS_MONITOR_RECORD_RELEASE) xTaskNotifyGive(scanner_task_handle);
    return error;
}
