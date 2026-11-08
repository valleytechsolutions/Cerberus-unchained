/* Private C5 selected-source service, included by ieee802154_scanner.c.
 * Bulk storage is optional. SDK operations/disposal use the existing worker.
 * scanner_mutex -> mesh_mux lock order; callbacks never acquire the mutex. */
#include "mesh_mac.h"

typedef struct {
    cerberus_mesh_locate_projection_t live, pinned;
    uint64_t lease_us, retention_us, clock_high_us;
    uint32_t pin, pin_counter;
    uint8_t phase, state, rx_channel;
    bool cancelled, release_requested, retiring, sdk_attempted;
    esp_err_t primary_error, cleanup_error;
} mesh_locate_context_t;
static mesh_locate_context_t *locate_context;
static uint64_t locate_session;
static cerberus_mesh_locate_start_t locate_request;
static bool another_radio_operation_is_active(void);
static bool mesh_legacy_running_locked(void);

enum { LOCATE_SETUP, LOCATE_ARM, LOCATE_RECEIVE, LOCATE_CLEANUP, LOCATE_STOPPED };

static uint32_t IRAM_ATTR locate_increment(uint32_t value, mesh_locate_context_t *c)
{
    if (value < UINT32_MAX) ++value;
    if (value == UINT32_MAX) c->live.flags |= CERBERUS_MESH_LOCATE_COUNTERS_SATURATED;
    return value;
}
static void IRAM_ATTR locate_clock_fault(mesh_locate_context_t *c)
{
    c->live.flags |= CERBERUS_MESH_LOCATE_PROJECTION_CLOCK_INVALID;
    c->cancelled = true;
    c->rx_channel = 0U;
    if (c->primary_error == ESP_OK) c->primary_error = ESP_ERR_INVALID_STATE;
    if (c->phase != LOCATE_STOPPED) c->state = CERBERUS_MESH_LOCATE_STOPPING;
    else c->retiring = true;
}
static void IRAM_ATTR locate_receive(uint8_t *frame, const esp_ieee802154_frame_info_t *info)
{
    /* Copy only the selector/session, never carry a context pointer across the
     * unlocked parser. Stop/release/replacement may retire it while parsing. */
    cerberus_mesh_locate_start_t target;
    uint64_t callback_us = 0U;
    portENTER_CRITICAL_ISR(&mesh_mux);
    mesh_locate_context_t *c = locate_context;
    if (!c || !c->rx_channel || c->cancelled) {
        portEXIT_CRITICAL_ISR(&mesh_mux);
        return;
    }
    target = locate_request;
    int64_t sampled = esp_timer_get_time();
    if (sampled < 0 || (uint64_t)sampled < c->clock_high_us ||
        (uint64_t)sampled > INT64_MAX - (uint64_t)CERBERUS_MESH_LOCATE_RETENTION_MS * 1000U) {
        c->live.callbacks = locate_increment(c->live.callbacks, c);
        c->live.invalid_metadata = locate_increment(c->live.invalid_metadata, c);
        locate_clock_fault(c);
        portEXIT_CRITICAL_ISR(&mesh_mux);
        return;
    }
    callback_us = (uint64_t)sampled;
    c->clock_high_us = callback_us;
    portEXIT_CRITICAL_ISR(&mesh_mux);

    bool metadata = frame && info && frame[0] >= 4U && frame[0] <= 127U &&
        info->channel == target.channel;
    mesh_mac_observation_t mac;
    mesh_mac_result_t parsed = MESH_MAC_INVALID_ARGUMENT;
    if (metadata) parsed = mesh_mac_parse(frame + 1U, (size_t)frame[0] - 2U, &mac);
    bool match = parsed == MESH_MAC_HEADER_OK && mac.source.mode == target.address_mode &&
        mac.source.value == target.address && !mac.source.broadcast && !mac.source.short_sentinel &&
        (mac.source_pan.origin != MESH_MAC_PAN_UNKNOWN) == (target.pan_known != 0U) &&
        (!target.pan_known || mac.source_pan.value == target.pan);

    portENTER_CRITICAL_ISR(&mesh_mux);
    c = locate_context;
    if (c && locate_session == target.session_id && c->rx_channel == target.channel && !c->cancelled) {
        c->live.callbacks = locate_increment(c->live.callbacks, c);
        if (!metadata) c->live.invalid_metadata = locate_increment(c->live.invalid_metadata, c);
        else if (parsed == MESH_MAC_UNSUPPORTED) c->live.unsupported = locate_increment(c->live.unsupported, c);
        else if (parsed != MESH_MAC_HEADER_OK) c->live.malformed = locate_increment(c->live.malformed, c);
        else if (!match) c->live.filtered_nonmatch = locate_increment(c->live.filtered_nonmatch, c);
        else if (c->live.match_sequence == CERBERUS_MESH_LOCATE_SEQUENCE_MAX) {
            c->live.invalid_metadata = locate_increment(c->live.invalid_metadata, c);
            c->live.flags |= CERBERUS_MESH_LOCATE_SEQUENCE_EXHAUSTED;
            c->cancelled = true;
            c->rx_channel = 0U;
            if (c->primary_error == ESP_OK) c->primary_error = ESP_ERR_INVALID_STATE;
        } else {
            ++c->live.match_sequence;
            c->live.matches = c->live.match_sequence;
            c->live.last_match_us = callback_us;
            c->live.rssi = info->rssi;
            c->live.lqi = info->lqi;
            c->live.flags |= CERBERUS_MESH_LOCATE_MATCH_TIME_VALID |
                CERBERUS_MESH_LOCATE_RSSI_VALID | CERBERUS_MESH_LOCATE_LQI_VALID;
            c->live.mac_length = (uint8_t)(frame[0] - 2U);
            memset(c->live.mac, 0, sizeof(c->live.mac));
            memcpy(c->live.mac, frame + 1U, c->live.mac_length);
            if (c->live.match_sequence == CERBERUS_MESH_LOCATE_SEQUENCE_MAX) {
                c->live.flags |= CERBERUS_MESH_LOCATE_SEQUENCE_EXHAUSTED;
                c->cancelled = true;
                c->rx_channel = 0U;
                if (c->primary_error == ESP_OK) c->primary_error = ESP_ERR_INVALID_STATE;
            }
        }
    }
    portEXIT_CRITICAL_ISR(&mesh_mux);
}
static bool locate_start_valid(const cerberus_mesh_locate_start_t *q)
{
    return q && q->version == CERBERUS_MESH_LOCATE_VERSION && q->session_id &&
        q->channel >= 11U && q->channel <= 26U && q->pan_known <= 1U &&
        (q->pan_known || q->pan == 0U) &&
        (q->address_mode == MESH_MAC_ADDRESS_EXT64 ||
         (q->address_mode == MESH_MAC_ADDRESS_SHORT16 && q->address < UINT64_C(0xfffe)));
}
/* Both locks held. Never compare a pre-lock sample against shared high water. */
static bool locate_now_locked(mesh_locate_context_t *c, uint64_t *now)
{
    int64_t source = esp_timer_get_time();
    if (source < 0 || (uint64_t)source < c->clock_high_us ||
        (uint64_t)source > INT64_MAX - (uint64_t)CERBERUS_MESH_LOCATE_RETENTION_MS * 1000U) {
        locate_clock_fault(c);
        return false;
    }
    *now = (uint64_t)source;
    c->clock_high_us = *now;
    if (c->phase == LOCATE_STOPPED) {
        if (*now >= c->retention_us) c->retiring = true;
    } else if (*now >= c->lease_us) {
        c->cancelled = true;
        c->rx_channel = 0U;
        c->state = CERBERUS_MESH_LOCATE_STOPPING;
        if (c->primary_error == ESP_OK) c->primary_error = ESP_ERR_TIMEOUT;
    }
    return true;
}
static void locate_retention_locked(mesh_locate_context_t *c, uint64_t now)
{
    if (c->phase == LOCATE_STOPPED && !c->retiring && !c->release_requested)
        c->retention_us = now + (uint64_t)CERBERUS_MESH_LOCATE_RETENTION_MS * 1000U;
}
static esp_err_t locate_source_error(const mesh_locate_context_t *c)
{
    return c->cleanup_error != ESP_OK ? c->cleanup_error : c->primary_error;
}
/* Caller holds scanner_mutex; ISR lock closes all projection races. */
static void locate_reply_locked(uint64_t token, bool info, esp_err_t error,
    cerberus_mesh_locate_control_t *out)
{
    memset(out, 0, sizeof(*out));
    out->session_id = info ? mesh_epoch : token;
    out->error_code = error;
    portENTER_CRITICAL(&mesh_mux);
    mesh_locate_context_t *c = locate_context;
    if (c && (info || token == locate_session)) {
        out->flags = CERBERUS_MESH_LOCATE_BOUND | CERBERUS_MESH_LOCATE_HELD;
        if (!c->retiring && !c->release_requested) out->flags |= CERBERUS_MESH_LOCATE_DATA_READY;
        if (c->rx_channel && !c->cancelled && c->state == CERBERUS_MESH_LOCATE_RECEIVING)
            out->flags |= CERBERUS_MESH_LOCATE_RX_ACTIVE;
        if (c->live.flags & CERBERUS_MESH_LOCATE_PROJECTION_CLOCK_INVALID) out->flags |= CERBERUS_MESH_LOCATE_CLOCK_INVALID;
        if (c->live.flags & CERBERUS_MESH_LOCATE_COUNTERS_SATURATED) out->flags |= CERBERUS_MESH_LOCATE_SATURATED;
        if (c->live.flags & CERBERUS_MESH_LOCATE_SEQUENCE_EXHAUSTED) out->flags |= CERBERUS_MESH_LOCATE_EXHAUSTED;
        out->sequence = c->live.match_sequence;
        out->state = c->state;
        out->channel = c->rx_channel;
        out->version = CERBERUS_MESH_LOCATE_VERSION;
        out->projection_bytes = (uint8_t)(CERBERUS_MESH_LOCATE_HEADER_BYTES + c->live.mac_length);
        if (error == ESP_OK) out->error_code = locate_source_error(c);
    } else if (info) {
        out->version = CERBERUS_MESH_LOCATE_VERSION;
        out->projection_bytes = CERBERUS_MESH_LOCATE_HEADER_BYTES;
        if (mesh_context || mesh_legacy_running_locked()) {
            out->flags = CERBERUS_MESH_LOCATE_HELD;
            out->state = CERBERUS_MESH_LOCATE_QUEUED;
        }
    }
    portEXIT_CRITICAL(&mesh_mux);
}
void cerberus_mesh_locate_info(cerberus_mesh_locate_control_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!scanner_mutex) { out->error_code = ESP_ERR_INVALID_STATE; return; }
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    locate_reply_locked(0U, true, ESP_OK, out);
    xSemaphoreGive(scanner_mutex);
}
esp_err_t cerberus_mesh_locate_start(const cerberus_mesh_locate_start_t *q,
    cerberus_mesh_locate_control_t *out)
{
    if (!q || !out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out)); out->session_id = q->session_id;
    if (!scanner_mutex || !scanner_task_handle) { out->error_code = ESP_ERR_INVALID_STATE; return ESP_ERR_INVALID_STATE; }
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    if (q->session_id && q->session_id == locate_session) {
        esp_err_t error = memcmp(q, &locate_request, sizeof(*q)) ? ESP_ERR_INVALID_ARG :
            locate_context ? ESP_OK : ESP_ERR_NOT_FOUND;
        locate_reply_locked(q->session_id, false, error, out);
        xSemaphoreGive(scanner_mutex); return error;
    }
    esp_err_t error = !locate_start_valid(q) ? ESP_ERR_INVALID_ARG :
        locate_context || mesh_context || mesh_legacy_running_locked() || mesh_epoch == UINT64_MAX ||
        q->session_id != mesh_epoch + 1U ? ESP_ERR_INVALID_STATE : ESP_OK;
    xSemaphoreGive(scanner_mutex);
    mesh_locate_context_t *candidate = NULL;
    if (error == ESP_OK) {
        candidate = heap_caps_calloc(1U, sizeof(*candidate), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!candidate) error = ESP_ERR_NO_MEM;
    }
    if (error == ESP_OK && (cerberus_c5_test_running() || another_radio_operation_is_active())) error = ESP_ERR_INVALID_STATE;
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    int64_t source = esp_timer_get_time();
    if (error == ESP_OK && (locate_context || mesh_context || mesh_legacy_running_locked() ||
        mesh_epoch == UINT64_MAX || q->session_id != mesh_epoch + 1U || source < 0 ||
        (uint64_t)source > INT64_MAX - (uint64_t)CERBERUS_MESH_LOCATE_RETENTION_MS * 1000U)) error = ESP_ERR_INVALID_STATE;
    if (error == ESP_OK) {
        mesh_epoch = locate_session = q->session_id;
        locate_request = *q;
        candidate->clock_high_us = (uint64_t)source;
        candidate->lease_us = (uint64_t)source + (uint64_t)CERBERUS_MESH_LOCATE_LEASE_MS * 1000U;
        candidate->state = CERBERUS_MESH_LOCATE_QUEUED;
        candidate->pin_counter = CERBERUS_MESH_LOCATE_PIN_BASE;
        candidate->live.version = CERBERUS_MESH_LOCATE_VERSION;
        candidate->live.address = q->address; candidate->live.address_mode = q->address_mode;
        candidate->live.pan = q->pan; candidate->live.pan_known = q->pan_known;
        portENTER_CRITICAL(&mesh_mux); locate_context = candidate; portEXIT_CRITICAL(&mesh_mux);
        candidate = NULL;
    }
    locate_reply_locked(q->session_id, false, error, out);
    xSemaphoreGive(scanner_mutex);
    if (candidate) heap_caps_free(candidate);
    if (error == ESP_OK) xTaskNotifyGive(scanner_task_handle);
    return error;
}
static esp_err_t locate_control(uint64_t token, unsigned action, cerberus_mesh_locate_control_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out)); out->session_id = token;
    if (!scanner_mutex) { out->error_code = ESP_ERR_INVALID_STATE; return ESP_ERR_INVALID_STATE; }
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    esp_err_t error = token && token == locate_session && (locate_context || action == 2U) ? ESP_OK : ESP_ERR_NOT_FOUND;
    portENTER_CRITICAL(&mesh_mux);
    mesh_locate_context_t *c = error == ESP_OK ? locate_context : NULL;
    if (c) {
        uint64_t now = 0U; bool valid = locate_now_locked(c, &now);
        if (action) {
            c->cancelled = true; c->rx_channel = 0U;
            if (action == 2U) c->release_requested = true;
            if (c->phase != LOCATE_STOPPED) c->state = CERBERUS_MESH_LOCATE_STOPPING;
        } else if (valid && !c->retiring && !c->release_requested) {
            if (c->phase != LOCATE_STOPPED && !c->cancelled)
                c->lease_us = now + (uint64_t)CERBERUS_MESH_LOCATE_LEASE_MS * 1000U;
            locate_retention_locked(c, now);
        }
    }
    portEXIT_CRITICAL(&mesh_mux);
    locate_reply_locked(token, false, error, out);
    xSemaphoreGive(scanner_mutex);
    if (c && action) xTaskNotifyGive(scanner_task_handle);
    return error;
}
esp_err_t cerberus_mesh_locate_status(uint64_t token, cerberus_mesh_locate_control_t *out)
{ return locate_control(token, 0U, out); }
esp_err_t cerberus_mesh_locate_stop(uint64_t token, cerberus_mesh_locate_control_t *out)
{ return locate_control(token, 1U, out); }
esp_err_t cerberus_mesh_locate_release(uint64_t token, cerberus_mesh_locate_control_t *out)
{ return locate_control(token, 2U, out); }
esp_err_t cerberus_mesh_locate_peek(uint64_t token, cerberus_mesh_locate_control_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out)); out->session_id = token;
    if (!scanner_mutex) { out->error_code = ESP_ERR_INVALID_STATE; return ESP_ERR_INVALID_STATE; }
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    esp_err_t error = token && token == locate_session && locate_context ? ESP_OK : ESP_ERR_NOT_FOUND;
    portENTER_CRITICAL(&mesh_mux);
    mesh_locate_context_t *c = error == ESP_OK ? locate_context : NULL;
    if (c) {
        uint64_t now = 0U; bool valid = locate_now_locked(c, &now);
        if (c->retiring || c->release_requested) error = ESP_ERR_NOT_FOUND;
        else if (c->pin_counter == UINT32_MAX) {
            c->live.flags |= CERBERUS_MESH_LOCATE_SEQUENCE_EXHAUSTED;
            c->cancelled = true; c->rx_channel = 0U;
            if (c->phase != LOCATE_STOPPED) c->state = CERBERUS_MESH_LOCATE_STOPPING;
            if (c->primary_error == ESP_OK) c->primary_error = ESP_ERR_INVALID_STATE;
            error = ESP_ERR_INVALID_STATE;
        } else {
            c->pinned = c->live; c->pin = ++c->pin_counter;
            c->pinned.state = c->state; c->pinned.channel = c->rx_channel;
            c->pinned.error_code = locate_source_error(c);
            c->pinned.copied_us = 0U;
            c->pinned.flags &= (uint8_t)~CERBERUS_MESH_LOCATE_COPY_TIME_VALID;
            if (valid && !(c->live.flags & CERBERUS_MESH_LOCATE_PROJECTION_CLOCK_INVALID)) {
                c->pinned.copied_us = now; c->pinned.flags |= CERBERUS_MESH_LOCATE_COPY_TIME_VALID;
            }
            if (valid) locate_retention_locked(c, now);
        }
    }
    portEXIT_CRITICAL(&mesh_mux);
    locate_reply_locked(token, false, error, out);
    if (error == ESP_OK) {
        /* Only the worker can retire this context, and it requires our mutex. */
        out->sequence = c->pin; out->state = c->pinned.state; out->channel = c->pinned.channel;
        out->projection_bytes = (uint8_t)(CERBERUS_MESH_LOCATE_HEADER_BYTES + c->pinned.mac_length);
        out->error_code = c->pinned.error_code;
        out->flags = CERBERUS_MESH_LOCATE_BOUND | CERBERUS_MESH_LOCATE_HELD | CERBERUS_MESH_LOCATE_DATA_READY;
        if (c->pinned.state == CERBERUS_MESH_LOCATE_RECEIVING && c->pinned.channel)
            out->flags |= CERBERUS_MESH_LOCATE_RX_ACTIVE;
        if (c->pinned.flags & CERBERUS_MESH_LOCATE_PROJECTION_CLOCK_INVALID) out->flags |= CERBERUS_MESH_LOCATE_CLOCK_INVALID;
        if (c->pinned.flags & CERBERUS_MESH_LOCATE_COUNTERS_SATURATED) out->flags |= CERBERUS_MESH_LOCATE_SATURATED;
        if (c->pinned.flags & CERBERUS_MESH_LOCATE_SEQUENCE_EXHAUSTED) out->flags |= CERBERUS_MESH_LOCATE_EXHAUSTED;
    }
    xSemaphoreGive(scanner_mutex);
    return error;
}
esp_err_t cerberus_mesh_locate_read(const cerberus_mesh_locate_read_t *q, cerberus_mesh_locate_chunk_t *out)
{
    if (!q || !out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out)); out->session_id = q->session_id; out->part = q->part;
    esp_err_t error = ESP_ERR_INVALID_STATE;
    if (scanner_mutex) {
        xSemaphoreTake(scanner_mutex, portMAX_DELAY); portENTER_CRITICAL(&mesh_mux);
        mesh_locate_context_t *c = locate_context;
        size_t offset = (size_t)q->part * CERBERUS_MESH_LOCATE_CHUNK_BYTES;
        error = q->pin <= CERBERUS_MESH_LOCATE_PIN_BASE || q->part >= CERBERUS_MESH_LOCATE_PARTS ?
            ESP_ERR_INVALID_ARG : !q->session_id || q->session_id != locate_session || !c ? ESP_ERR_NOT_FOUND : ESP_OK;
        if (error == ESP_OK) {
            size_t length = CERBERUS_MESH_LOCATE_HEADER_BYTES + c->pinned.mac_length;
            if (q->pin != c->pin || offset >= length) error = ESP_ERR_NOT_FOUND;
            else {
                uint64_t now = 0U; bool valid = locate_now_locked(c, &now);
                if (c->retiring || c->release_requested) error = ESP_ERR_NOT_FOUND;
                else {
                    size_t count = length - offset;
                    if (count > CERBERUS_MESH_LOCATE_CHUNK_BYTES) count = CERBERUS_MESH_LOCATE_CHUNK_BYTES;
                    memcpy(out->data, (const uint8_t *)&c->pinned + offset, count);
                    out->pin = q->pin;
                    if (valid) locate_retention_locked(c, now);
                }
            }
        }
        portEXIT_CRITICAL(&mesh_mux); xSemaphoreGive(scanner_mutex);
    }
    if (error != ESP_OK) memcpy(out->data, &error, sizeof(error));
    return error;
}
static void run_mesh_locate(void)
{
    for (;;) {
        xSemaphoreTake(scanner_mutex, portMAX_DELAY); portENTER_CRITICAL(&mesh_mux);
        mesh_locate_context_t *c = locate_context;
        if (!c) { portEXIT_CRITICAL(&mesh_mux); xSemaphoreGive(scanner_mutex); return; }
        uint64_t now = 0U; (void)locate_now_locked(c, &now);
        if (c->cancelled && c->phase < LOCATE_STOPPED) {
            c->rx_channel = 0U; c->state = CERBERUS_MESH_LOCATE_STOPPING;
            c->phase = LOCATE_CLEANUP;
        }
        uint8_t phase = c->phase;
        bool dispose = phase == LOCATE_STOPPED && (c->release_requested || c->retiring);
        if (dispose) locate_context = NULL;
        portEXIT_CRITICAL(&mesh_mux); xSemaphoreGive(scanner_mutex);
        if (dispose) { heap_caps_free(c); return; }
        esp_err_t error = ESP_OK;
        if (phase == LOCATE_SETUP) {
            c->sdk_attempted = true; error = esp_ieee802154_enable();
            if (error == ESP_OK) error = esp_ieee802154_set_coordinator(false);
            if (error == ESP_OK) error = esp_ieee802154_set_promiscuous(true);
            if (error == ESP_OK) error = esp_ieee802154_set_rx_when_idle(true);
            xSemaphoreTake(scanner_mutex, portMAX_DELAY); portENTER_CRITICAL(&mesh_mux);
            if (error != ESP_OK) { if (c->primary_error == ESP_OK) c->primary_error = error; c->cancelled = true; }
            c->phase = c->cancelled ? LOCATE_CLEANUP : LOCATE_ARM;
            if (c->cancelled) c->state = CERBERUS_MESH_LOCATE_STOPPING;
            portEXIT_CRITICAL(&mesh_mux); xSemaphoreGive(scanner_mutex);
            continue;
        }
        if (phase == LOCATE_ARM) {
            error = esp_ieee802154_set_channel(locate_request.channel);
            xSemaphoreTake(scanner_mutex, portMAX_DELAY); portENTER_CRITICAL(&mesh_mux);
            uint64_t armed = 0U; bool valid = locate_now_locked(c, &armed);
            bool receive = error == ESP_OK && valid && !c->cancelled;
            if (receive) { c->rx_channel = locate_request.channel; c->state = CERBERUS_MESH_LOCATE_QUEUED; }
            portEXIT_CRITICAL(&mesh_mux); xSemaphoreGive(scanner_mutex);
            if (receive) error = esp_ieee802154_receive();
            xSemaphoreTake(scanner_mutex, portMAX_DELAY); portENTER_CRITICAL(&mesh_mux);
            uint64_t started = 0U; valid = locate_now_locked(c, &started);
            if (error != ESP_OK) { if (c->primary_error == ESP_OK) c->primary_error = error; c->cancelled = true; }
            if (valid && !c->cancelled) { c->phase = LOCATE_RECEIVE; c->state = CERBERUS_MESH_LOCATE_RECEIVING; }
            else { c->rx_channel = 0U; c->phase = LOCATE_CLEANUP; c->state = CERBERUS_MESH_LOCATE_STOPPING; }
            portEXIT_CRITICAL(&mesh_mux); xSemaphoreGive(scanner_mutex);
            continue;
        }
        if (phase == LOCATE_CLEANUP) {
            esp_err_t idle_error = ESP_OK, sleep_error = ESP_OK;
            if (c->sdk_attempted) {
                idle_error = esp_ieee802154_set_rx_when_idle(false);
                sleep_error = esp_ieee802154_sleep(); error = esp_ieee802154_disable();
                if (error == ESP_OK) {
                    esp_err_t restore = esp_ieee802154_set_promiscuous(false);
                    if (idle_error == ESP_OK) idle_error = restore;
                }
            }
            xSemaphoreTake(scanner_mutex, portMAX_DELAY); portENTER_CRITICAL(&mesh_mux);
            c->cleanup_error = error;
            if (error == ESP_OK) {
                if (c->primary_error == ESP_OK) c->primary_error = idle_error != ESP_OK ? idle_error : sleep_error;
                uint64_t stopped = 0U; bool valid = locate_now_locked(c, &stopped);
                c->phase = LOCATE_STOPPED;
                c->state = c->primary_error == ESP_OK ? CERBERUS_MESH_LOCATE_COMPLETE : CERBERUS_MESH_LOCATE_ERROR;
                c->rx_channel = 0U; c->live.flags |= CERBERUS_MESH_LOCATE_STOPPED;
                if (valid) c->retention_us = stopped + (uint64_t)CERBERUS_MESH_LOCATE_RETENTION_MS * 1000U;
                else c->retiring = true;
            }
            portEXIT_CRITICAL(&mesh_mux); xSemaphoreGive(scanner_mutex);
            if (error == ESP_OK) continue;
        }
        TickType_t wait = pdMS_TO_TICKS(phase == LOCATE_CLEANUP ? 100U : 20U);
        (void)ulTaskNotifyTake(pdTRUE, wait ? wait : 1U);
    }
}
