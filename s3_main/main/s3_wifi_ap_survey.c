#include "s3_wifi_ap_survey.h"
#include "s3_wifi_owner.h"
#include "s3_ble_host.h"
#include "wifi_ap_survey_protocol.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <string.h>

/* The Wi-Fi receive callback runs in its SDK task, not an ISR. Both banks
 * remain allocated until that callback drains and SDK cleanup finishes. */
typedef struct {
    cerberus_ap_survey_table_t table;
    cerberus_ap_survey_snapshot_t pinned;
} s3_ap_survey_banks_t;

typedef struct {
    cerberus_ap_survey_start_t request;
    s3_ap_survey_banks_t *banks;
    uint64_t deadline_us, clock_floor;
    uint32_t projection_counter, projection, inflight;
    esp_err_t error, tune_error;
    uint8_t state, channel, next_channel;
    bool held, accepting, stop, release, data_ready, clock_invalid;
    bool initialized, started, callback, disable_pending, worker;
} s3_ap_survey_context_t;

static portMUX_TYPE survey_lock = portMUX_INITIALIZER_UNLOCKED;
static s3_ap_survey_context_t *survey_context;
static uint64_t survey_epoch = 1U;
static bool survey_launching, survey_command;
static atomic_uint survey_admission;
static uint32_t survey_admission_counter;

static void survey_first_error(s3_ap_survey_context_t *c, esp_err_t error)
{
    if (c->error == ESP_OK && error != ESP_OK) c->error = error;
}

static void survey_close_locked(s3_ap_survey_context_t *c, int64_t now)
{
    c->accepting = false;
    c->channel = 0U;
    atomic_store_explicit(&survey_admission, 0U, memory_order_release);
    if (c->data_ready && c->banks->table.accepting) {
        uint64_t end = now >= 0 ? (uint64_t)now : c->clock_floor;
        if (end < c->banks->table.armed_us) end = c->banks->table.armed_us;
        (void)cerberus_ap_survey_disarm(&c->banks->table, c->request.session_id,
            c->banks->table.arm_token, end);
    }
}

static void survey_stop_locked(s3_ap_survey_context_t *c, int64_t now)
{
    c->stop = true;
    survey_close_locked(c, now);
    if (c->held) c->state = CERBERUS_AP_SURVEY_STOPPING;
}

static bool survey_clock_locked(s3_ap_survey_context_t *c, int64_t now)
{
    if (now < 0 || now > INT64_MAX - (int64_t)CERBERUS_AP_SURVEY_RETENTION_MS * 1000 ||
        (uint64_t)now < c->clock_floor) {
        c->clock_invalid = true;
        survey_first_error(c, ESP_ERR_INVALID_STATE);
        survey_stop_locked(c, now);
        if (!c->held) c->release = true;
        return false;
    }
    c->clock_floor = (uint64_t)now;
    return true;
}

static void survey_expire_locked(s3_ap_survey_context_t *c, int64_t now)
{
    if (!survey_clock_locked(c, now)) return;
    if ((uint64_t)now >= c->deadline_us) {
        if (c->held) {
            survey_first_error(c, ESP_ERR_TIMEOUT);
            survey_stop_locked(c, now);
        } else {
            c->release = true;
        }
    }
}

static void survey_control_locked(const s3_ap_survey_context_t *c,
    cerberus_ap_survey_control_t *out)
{
    memset(out, 0, sizeof(*out));
    out->session_id = c->request.session_id;
    out->error_code = c->error != ESP_OK ? c->error : c->tune_error;
    out->state = c->state;
    out->flags = CERBERUS_AP_SURVEY_BOUND;
    if (c->held) out->flags |= CERBERUS_AP_SURVEY_HELD;
    if (c->accepting) out->flags |= CERBERUS_AP_SURVEY_RECEIVING;
    if (c->data_ready && !c->release && !c->clock_invalid) {
        out->flags |= CERBERUS_AP_SURVEY_DATA_READY;
        out->revision = c->banks->table.live.revision;
        out->ap_count = c->banks->table.live.ap_count;
        out->relation_count = c->banks->table.live.relation_count;
        out->channel = c->channel;
    }
}

static void survey_receive(void *buffer, wifi_promiscuous_pkt_type_t type)
{
    uint32_t token = atomic_load_explicit(&survey_admission, memory_order_acquire);
    int64_t entered = esp_timer_get_time();
    if (!buffer || !token) return;
    const wifi_promiscuous_pkt_t *packet = buffer;
    portENTER_CRITICAL(&survey_lock);
    s3_ap_survey_context_t *c = survey_context;
    if (!c || !c->accepting || c->stop ||
        token != atomic_load_explicit(&survey_admission, memory_order_acquire) ||
        !survey_clock_locked(c, esp_timer_get_time())) {
        portEXIT_CRITICAL(&survey_lock);
        return;
    }
    if (entered < 0) {
        (void)survey_clock_locked(c, entered);
        portEXIT_CRITICAL(&survey_lock);
        return;
    }
    if ((uint64_t)entered < c->banks->table.armed_us) {
        portEXIT_CRITICAL(&survey_lock);
        return;
    }
    ++c->inflight;
    uint64_t session = c->request.session_id;
    portEXIT_CRITICAL(&survey_lock);

    /* The SDK owns this complete frame until the callback returns. Parse it
     * without holding the receiver lock; Stop keeps the context until drain. */
    cerberus_ap_survey_decoded_t decoded = {0};
    bool metadata = packet->rx_ctrl.rx_state == 0U && packet->rx_ctrl.sig_len >= 4U &&
        (type == WIFI_PKT_MGMT || type == WIFI_PKT_DATA);
    if (metadata) {
        (void)cerberus_ap_survey_decode(packet->payload,
            packet->rx_ctrl.sig_len - 4U, packet->rx_ctrl.channel,
            packet->rx_ctrl.rssi, (uint64_t)entered, &decoded);
    }
    portENTER_CRITICAL(&survey_lock);
    if (metadata && c == survey_context && c->accepting && !c->stop &&
        token == atomic_load_explicit(&survey_admission, memory_order_acquire)) {
        (void)cerberus_ap_survey_commit(&c->banks->table, session, token,
            packet->rx_ctrl.channel, (uint64_t)entered, &decoded);
        if (c->banks->table.live.flags & CERBERUS_AP_SURVEY_SNAPSHOT_EXHAUSTED) {
            survey_first_error(c, ESP_ERR_INVALID_STATE);
            survey_stop_locked(c, esp_timer_get_time());
        }
    }
    --c->inflight;
    portEXIT_CRITICAL(&survey_lock);
}

static bool survey_cancelled(s3_ap_survey_context_t *c)
{
    portENTER_CRITICAL(&survey_lock);
    survey_expire_locked(c, esp_timer_get_time());
    if (c->banks->table.live.flags & CERBERUS_AP_SURVEY_SNAPSHOT_EXHAUSTED) {
        survey_first_error(c, ESP_ERR_INVALID_STATE);
        survey_stop_locked(c, esp_timer_get_time());
    }
    bool stop = c->stop;
    portEXIT_CRITICAL(&survey_lock);
    return stop;
}

static esp_err_t survey_cleanup(s3_ap_survey_context_t *c)
{
    if (cerberus_s3_wifi_owner() != CERBERUS_S3_WIFI_SURVEY) return ESP_ERR_INVALID_STATE;
    esp_err_t error;
    if (c->disable_pending) {
        error = esp_wifi_set_promiscuous(false);
        if (error != ESP_OK) return error;
        c->disable_pending = false;
    }
    if (c->callback) {
        error = esp_wifi_set_promiscuous_rx_cb(NULL);
        if (error != ESP_OK) return error;
        c->callback = false;
    }
    if (c->started) {
        error = esp_wifi_stop();
        if (error != ESP_OK && error != ESP_ERR_WIFI_NOT_STARTED) return error;
        c->started = false;
    }
    if (c->initialized) {
        error = esp_wifi_deinit();
        if (error != ESP_OK) return error;
        c->initialized = false;
    }
    return cerberus_s3_wifi_release(CERBERUS_S3_WIFI_SURVEY);
}

static void survey_worker(void *arg)
{
    s3_ap_survey_context_t *c = arg;
    esp_err_t error = ESP_OK;
    if (!survey_cancelled(c)) error = cerberus_s3_wifi_prepare(CERBERUS_S3_WIFI_SURVEY);
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    init.static_rx_buf_num = 4;
    init.dynamic_rx_buf_num = 8;
    init.dynamic_tx_buf_num = 4;
    init.rx_ba_win = 4;
    init.ampdu_rx_enable = init.ampdu_tx_enable = init.amsdu_tx_enable = 0;
    if (error == ESP_OK && !survey_cancelled(c)) {
        error = esp_wifi_init(&init);
        c->initialized = error == ESP_OK;
    }
    if (error == ESP_OK && c->initialized && !survey_cancelled(c)) error = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (error == ESP_OK && c->initialized && !survey_cancelled(c)) error = esp_wifi_set_mode(WIFI_MODE_NULL);
    if (error == ESP_OK && c->initialized && !survey_cancelled(c)) {
        error = esp_wifi_start();
        c->started = error == ESP_OK;
    }
    uint64_t effective = 0U;
    if (error == ESP_OK && c->started && !survey_cancelled(c)) {
        wifi_country_t country = {0};
        error = esp_wifi_get_country(&country);
        if (error == ESP_OK) {
            unsigned end = (unsigned)country.schan + country.nchan;
            if (!country.schan || !country.nchan || country.schan > 14U || end > 15U) {
                error = ESP_ERR_INVALID_STATE;
            } else {
                for (unsigned channel = country.schan; channel < end; ++channel)
                    effective |= c->request.channel_mask & (UINT64_C(1) << (channel - 1U));
                if (!effective) error = ESP_ERR_NOT_SUPPORTED;
            }
        }
        if (error == ESP_OK) {
            portENTER_CRITICAL(&survey_lock);
            c->data_ready = cerberus_ap_survey_begin(&c->banks->table,
                c->request.session_id, c->request.channel_mask, effective);
            if (!c->data_ready) error = ESP_ERR_INVALID_STATE;
            portEXIT_CRITICAL(&survey_lock);
        }
    }
    wifi_promiscuous_filter_t filter = {.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA};
    if (error == ESP_OK && c->started && !survey_cancelled(c)) error = esp_wifi_set_promiscuous_filter(&filter);
    if (error == ESP_OK && c->started && !survey_cancelled(c)) {
        error = esp_wifi_set_promiscuous_rx_cb(survey_receive);
        c->callback = error == ESP_OK;
    }
    unsigned planned = 0U, failures = 0U, pass_channels = 0U;
    for (unsigned i = 0U; i < 14U; ++i) planned += (effective & (UINT64_C(1) << i)) != 0U;
    while (error == ESP_OK && c->callback && !survey_cancelled(c)) {
        unsigned chosen = 0U;
        for (unsigned i = 0U; i < 14U; ++i) {
            unsigned index = (c->next_channel + i) % 14U;
            if (effective & (UINT64_C(1) << index)) { chosen = index; break; }
        }
        c->next_channel = (uint8_t)((chosen + 1U) % 14U);
        uint8_t channel = (uint8_t)(chosen + 1U);
        portENTER_CRITICAL(&survey_lock);
        survey_close_locked(c, esp_timer_get_time());
        portEXIT_CRITICAL(&survey_lock);
        error = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
        portENTER_CRITICAL(&survey_lock);
        bool accounted = cerberus_ap_survey_note_channel(&c->banks->table, channel, error == ESP_OK);
        if (!accounted) error = ESP_ERR_INVALID_STATE;
        if (error != ESP_OK && c->tune_error == ESP_OK) c->tune_error = error;
        portEXIT_CRITICAL(&survey_lock);
        if (error != ESP_OK) {
            if (!accounted || ++failures >= planned) break;
            error = ESP_OK;
            if (++pass_channels >= planned) {
                portENTER_CRITICAL(&survey_lock);
                cerberus_ap_survey_note_pass(&c->banks->table);
                portEXIT_CRITICAL(&survey_lock);
                pass_channels = 0U;
            }
            vTaskDelay(pdMS_TO_TICKS(25));
            continue;
        }
        failures = 0U;
        if (!c->disable_pending && !survey_cancelled(c)) {
            c->disable_pending = true;
            error = esp_wifi_set_promiscuous(true);
        }
        portENTER_CRITICAL(&survey_lock);
        int64_t now = esp_timer_get_time();
        if (error == ESP_OK && !c->stop && survey_clock_locked(c, now)) {
            if (survey_admission_counter == UINT32_MAX ||
                !cerberus_ap_survey_arm(&c->banks->table, c->request.session_id,
                    (uint64_t)survey_admission_counter + 1U, channel, (uint64_t)now)) {
                error = ESP_ERR_INVALID_STATE;
            } else {
                c->channel = channel;
                c->accepting = true;
                c->state = CERBERUS_AP_SURVEY_RUNNING;
                atomic_store_explicit(&survey_admission, ++survey_admission_counter, memory_order_release);
            }
        }
        uint64_t until = now >= 0 ? (uint64_t)now + CERBERUS_AP_SURVEY_DWELL_MS * UINT64_C(1000) : 0U;
        portEXIT_CRITICAL(&survey_lock);
        while (error == ESP_OK && !survey_cancelled(c)) {
            int64_t sample = esp_timer_get_time();
            if (sample < 0 || (uint64_t)sample >= until) break;
            vTaskDelay(pdMS_TO_TICKS(25));
        }
        if (error == ESP_OK && !survey_cancelled(c)) {
            until = (uint64_t)esp_timer_get_time() + CERBERUS_AP_SURVEY_DWELL_MS * UINT64_C(1000);
            portENTER_CRITICAL(&survey_lock);
            if (++pass_channels >= planned) {
                cerberus_ap_survey_note_pass(&c->banks->table);
                pass_channels = 0U;
            }
            portEXIT_CRITICAL(&survey_lock);
            /* A single-channel plan never retunes or interrupts admission. */
            while (planned == 1U && !survey_cancelled(c)) {
                vTaskDelay(pdMS_TO_TICKS(25));
                int64_t sample = esp_timer_get_time();
                if (sample >= 0 && (uint64_t)sample >= until) {
                    portENTER_CRITICAL(&survey_lock);
                    cerberus_ap_survey_note_pass(&c->banks->table);
                    portEXIT_CRITICAL(&survey_lock);
                    until = (uint64_t)sample + CERBERUS_AP_SURVEY_DWELL_MS * UINT64_C(1000);
                }
            }
        }
    }
    portENTER_CRITICAL(&survey_lock);
    survey_first_error(c, error);
    survey_stop_locked(c, esp_timer_get_time());
    portEXIT_CRITICAL(&survey_lock);
    for (;;) {
        portENTER_CRITICAL(&survey_lock);
        bool inflight = c->inflight != 0U;
        portEXIT_CRITICAL(&survey_lock);
        if (!inflight) break;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    do {
        error = survey_cleanup(c);
        if (error != ESP_OK) {
            portENTER_CRITICAL(&survey_lock);
            survey_first_error(c, error);
            portEXIT_CRITICAL(&survey_lock);
            vTaskDelay(pdMS_TO_TICKS(250));
        }
    } while (error != ESP_OK);
    portENTER_CRITICAL(&survey_lock);
    c->held = false;
    c->state = c->error == ESP_OK ? CERBERUS_AP_SURVEY_COMPLETE : CERBERUS_AP_SURVEY_ERROR;
    int64_t now = esp_timer_get_time();
    if (survey_clock_locked(c, now))
        c->deadline_us = (uint64_t)now + CERBERUS_AP_SURVEY_RETENTION_MS * UINT64_C(1000);
    portEXIT_CRITICAL(&survey_lock);
    for (;;) {
        portENTER_CRITICAL(&survey_lock);
        survey_expire_locked(c, esp_timer_get_time());
        bool dispose = c->release && !survey_command && !c->inflight;
        if (dispose) survey_context = NULL;
        portEXIT_CRITICAL(&survey_lock);
        if (dispose) break;
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    if (c) heap_caps_free(c->banks);
    heap_caps_free(c);
    vTaskDelete(NULL);
}

static esp_err_t survey_start(const cerberus_ap_survey_start_t *request,
    cerberus_ap_survey_control_t *reply)
{
    if (!request->session_id || !request->channel_mask || request->channel_mask >> 14U ||
        request->dwell_ms != CERBERUS_AP_SURVEY_DWELL_MS || request->reserved)
        return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&survey_lock);
    if (survey_context) {
        s3_ap_survey_context_t *c = survey_context;
        bool same = request->session_id == c->request.session_id;
        if (same) survey_control_locked(c, reply);
        bool exact = same && !memcmp(request, &c->request, sizeof(*request));
        portEXIT_CRITICAL(&survey_lock);
        return exact ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    if (survey_launching || survey_epoch == UINT64_MAX || request->session_id != survey_epoch + 1U) {
        portEXIT_CRITICAL(&survey_lock);
        return request->session_id == survey_epoch ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_STATE;
    }
    survey_launching = true;
    portEXIT_CRITICAL(&survey_lock);

    esp_err_t error = ESP_OK;
    bool held = false;
    s3_ap_survey_context_t *c = NULL;
    if (cerberus_s3_ble_host_owner() != CERBERUS_S3_BLE_OWNER_NONE) error = ESP_ERR_INVALID_STATE;
    if (error == ESP_OK && heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 65536U)
        error = ESP_ERR_NO_MEM;
    if (error == ESP_OK) {
        c = heap_caps_calloc(1U, sizeof(*c), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!c) error = ESP_ERR_NO_MEM;
    }
    if (error == ESP_OK) {
        c->banks = heap_caps_calloc(1U, sizeof(*c->banks), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!c->banks) error = ESP_ERR_NO_MEM;
    }
    int64_t now = esp_timer_get_time();
    if (error == ESP_OK && (now < 0 || now > INT64_MAX - (int64_t)CERBERUS_AP_SURVEY_RETENTION_MS * 1000))
        error = ESP_ERR_INVALID_STATE;
    if (error == ESP_OK) {
        error = cerberus_s3_wifi_claim(CERBERUS_S3_WIFI_SURVEY);
        held = error == ESP_OK;
    }
    if (error == ESP_OK && cerberus_s3_ble_host_owner() != CERBERUS_S3_BLE_OWNER_NONE)
        error = ESP_ERR_INVALID_STATE;
    if (error != ESP_OK) {
        if (held) {
            esp_err_t released = cerberus_s3_wifi_release(CERBERUS_S3_WIFI_SURVEY);
            held = released != ESP_OK;
            if (held) error = released;
        }
        if (!held) {
            if (c) heap_caps_free(c->banks);
            heap_caps_free(c);
            portENTER_CRITICAL(&survey_lock);
            survey_launching = false;
            portEXIT_CRITICAL(&survey_lock);
            return error;
        }
    }
    c->request = *request;
    c->state = CERBERUS_AP_SURVEY_QUEUED;
    c->held = true;
    c->clock_floor = (uint64_t)now;
    c->deadline_us = (uint64_t)now + CERBERUS_AP_SURVEY_LEASE_MS * UINT64_C(1000);
    c->worker = error == ESP_OK;
    portENTER_CRITICAL(&survey_lock);
    survey_context = c;
    survey_epoch = request->session_id;
    survey_launching = false;
    portEXIT_CRITICAL(&survey_lock);
    if (error != ESP_OK || xTaskCreate(survey_worker, "s3_ap_survey", 4096, c, 3, NULL) != pdPASS) {
        c->worker = false;
        esp_err_t released = survey_cleanup(c);
        if (released == ESP_OK) {
            portENTER_CRITICAL(&survey_lock);
            survey_context = NULL;
            portEXIT_CRITICAL(&survey_lock);
            if (c) heap_caps_free(c->banks);
            heap_caps_free(c);
            return error != ESP_OK ? error : ESP_ERR_NO_MEM;
        }
        portENTER_CRITICAL(&survey_lock);
        c->error = released;
        survey_stop_locked(c, now);
        survey_control_locked(c, reply);
        portEXIT_CRITICAL(&survey_lock);
        return released;
    }
    portENTER_CRITICAL(&survey_lock);
    survey_control_locked(c, reply);
    portEXIT_CRITICAL(&survey_lock);
    return ESP_OK;
}

bool cerberus_s3_wifi_ap_survey_active(void)
{
    portENTER_CRITICAL(&survey_lock);
    bool active = survey_launching || (survey_context && survey_context->held);
    portEXIT_CRITICAL(&survey_lock);
    return active;
}

esp_err_t cerberus_s3_wifi_ap_survey_snapshot(uint64_t session,
    cerberus_ap_survey_snapshot_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!session) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&survey_lock);
    s3_ap_survey_context_t *c = survey_context;
    int64_t now = esp_timer_get_time();
    esp_err_t error = ESP_ERR_NOT_FOUND;
    if (c && session == c->request.session_id) {
        survey_expire_locked(c, now);
        error = ESP_ERR_INVALID_STATE;
        if (c->data_ready && !c->release && !c->clock_invalid &&
            cerberus_ap_survey_snapshot(&c->banks->table, (uint64_t)now, out)) {
            error = ESP_OK;
            if (!c->held)
                c->deadline_us = (uint64_t)now + CERBERUS_AP_SURVEY_RETENTION_MS * UINT64_C(1000);
        }
    }
    portEXIT_CRITICAL(&survey_lock);
    if (error == ESP_OK && !cerberus_ap_survey_snapshot_valid(out)) {
        memset(out, 0, sizeof(*out));
        error = ESP_ERR_INVALID_RESPONSE;
    }
    return error;
}

esp_err_t cerberus_s3_wifi_ap_survey_exchange(uint8_t command, const void *input,
    size_t input_size, void *output, size_t output_size)
{
    size_t required = cerberus_ap_survey_request_bytes(command);
    if (!output || required == SIZE_MAX || input_size != required ||
        output_size != cerberus_ap_survey_reply_bytes(command) || (input_size && !input))
        return ESP_ERR_INVALID_ARG;
    if (input_size) {
        uintptr_t a = (uintptr_t)input, b = (uintptr_t)output;
        if (a > UINTPTR_MAX - input_size || b > UINTPTR_MAX - output_size ||
            (a < b + output_size && b < a + input_size)) return ESP_ERR_INVALID_ARG;
    }
    memset(output, 0, output_size);
    uint64_t session = 0U;
    if (input_size) memcpy(&session, input, sizeof(session));
    cerberus_ap_survey_control_t reply = {.session_id = session};
    portENTER_CRITICAL(&survey_lock);
    if (survey_command) { portEXIT_CRITICAL(&survey_lock); return ESP_ERR_INVALID_STATE; }
    survey_command = true;
    s3_ap_survey_context_t *c = survey_context;
    if (c) survey_expire_locked(c, esp_timer_get_time());
    portEXIT_CRITICAL(&survey_lock);
    if (c && !c->worker && c->held) {
        esp_err_t cleaned = survey_cleanup(c);
        portENTER_CRITICAL(&survey_lock);
        if (cleaned == ESP_OK) { c->held = false; c->state = CERBERUS_AP_SURVEY_ERROR; }
        else survey_first_error(c, cleaned);
        portEXIT_CRITICAL(&survey_lock);
    }
    esp_err_t error = ESP_OK;
    if (command == CERBERUS_AP_SURVEY_COMMAND_START) {
        cerberus_ap_survey_start_t request;
        memcpy(&request, input, sizeof(request));
        error = survey_start(&request, &reply);
    } else {
        portENTER_CRITICAL(&survey_lock);
        c = survey_context;
        int64_t now = esp_timer_get_time();
        if (c) survey_expire_locked(c, now);
        if (command == CERBERUS_AP_SURVEY_COMMAND_INFO) {
            if (c) survey_control_locked(c, &reply);
            else {
                reply.session_id = survey_epoch;
                if (survey_launching || cerberus_s3_wifi_owner() != CERBERUS_S3_WIFI_NONE) {
                    reply.flags = CERBERUS_AP_SURVEY_HELD;
                    reply.state = CERBERUS_AP_SURVEY_QUEUED;
                }
            }
        } else if (!session || !c || session != c->request.session_id) {
            error = command == CERBERUS_AP_SURVEY_COMMAND_RELEASE && session == survey_epoch ? ESP_OK : ESP_ERR_NOT_FOUND;
        } else {
            if (command == CERBERUS_AP_SURVEY_COMMAND_RENEW && c->held && !c->stop && !c->release)
                c->deadline_us = (uint64_t)now + CERBERUS_AP_SURVEY_LEASE_MS * UINT64_C(1000);
            else if (command == CERBERUS_AP_SURVEY_COMMAND_STOP) survey_stop_locked(c, now);
            else if (command == CERBERUS_AP_SURVEY_COMMAND_RELEASE) {
                c->release = true;
                c->projection = 0U;
                survey_stop_locked(c, now);
            }
            survey_control_locked(c, &reply);
        }
        if (command == CERBERUS_AP_SURVEY_COMMAND_SNAPSHOT_BEGIN) {
            cerberus_ap_survey_projection_t projection = {.session_id = session};
            if (error == ESP_OK && c && session == c->request.session_id) {
                c->projection = 0U;
                if (!c->data_ready || c->release || c->clock_invalid) error = ESP_ERR_INVALID_STATE;
                else if (c->projection_counter == UINT32_MAX) {
                    error = ESP_ERR_INVALID_STATE;
                    survey_first_error(c, error);
                    survey_stop_locked(c, now);
                } else if (!cerberus_ap_survey_snapshot(&c->banks->table, (uint64_t)now, &c->banks->pinned)) {
                    error = ESP_ERR_INVALID_STATE;
                } else {
                    c->projection = ++c->projection_counter;
                    projection.projection = c->projection;
                    projection.length = sizeof(c->banks->pinned);
                    projection.ap_count = c->banks->pinned.ap_count;
                    projection.relation_count = c->banks->pinned.relation_count;
                    projection.flags = c->banks->pinned.flags;
                }
            }
            projection.error_code = error;
            memcpy(output, &projection, sizeof(projection));
        } else if (command == CERBERUS_AP_SURVEY_COMMAND_SNAPSHOT_READ) {
            cerberus_ap_survey_read_t request;
            memcpy(&request, input, sizeof(request));
            cerberus_ap_survey_chunk_t *chunk = output;
            if (error == ESP_OK && c && session == c->request.session_id) {
                if (c->release || !request.projection || request.projection != c->projection)
                    error = ESP_ERR_INVALID_STATE;
                else if (request.offset % CERBERUS_AP_SURVEY_CHUNK_BYTES || request.offset >= sizeof(c->banks->pinned))
                    error = ESP_ERR_INVALID_ARG;
                else {
                    chunk->session_id = session;
                    chunk->projection = request.projection;
                    chunk->offset = request.offset;
                    size_t take = sizeof(c->banks->pinned) - request.offset;
                    if (take > sizeof(chunk->data)) take = sizeof(chunk->data);
                    memcpy(chunk->data, (const uint8_t *)&c->banks->pinned + request.offset, take);
                }
            }
            if (error != ESP_OK) cerberus_ap_survey_read_error(chunk, session, request.offset, error);
        }
        if (c && session == c->request.session_id && error == ESP_OK && !c->held &&
            !c->release && !c->clock_invalid &&
            (command == CERBERUS_AP_SURVEY_COMMAND_STATUS || command == CERBERUS_AP_SURVEY_COMMAND_RENEW ||
             command == CERBERUS_AP_SURVEY_COMMAND_SNAPSHOT_BEGIN || command == CERBERUS_AP_SURVEY_COMMAND_SNAPSHOT_READ))
            c->deadline_us = (uint64_t)now + CERBERUS_AP_SURVEY_RETENTION_MS * UINT64_C(1000);
        portEXIT_CRITICAL(&survey_lock);
    }
    /* The command reservation pins this immutable image while validation
     * runs outside the RX lock. Reception may continue updating the live bank. */
    if (command == CERBERUS_AP_SURVEY_COMMAND_SNAPSHOT_BEGIN && error == ESP_OK &&
        !cerberus_ap_survey_snapshot_valid(&c->banks->pinned)) {
        error = ESP_ERR_INVALID_RESPONSE;
        cerberus_ap_survey_projection_t failed = {.session_id = session, .error_code = error};
        memcpy(output, &failed, sizeof(failed));
        portENTER_CRITICAL(&survey_lock);
        c->projection = 0U;
        portEXIT_CRITICAL(&survey_lock);
    }
    if (command != CERBERUS_AP_SURVEY_COMMAND_SNAPSHOT_BEGIN && command != CERBERUS_AP_SURVEY_COMMAND_SNAPSHOT_READ) {
        if (error != ESP_OK) reply.error_code = error;
        memcpy(output, &reply, sizeof(reply));
    }
    portENTER_CRITICAL(&survey_lock);
    survey_command = false;
    c = survey_context;
    bool dispose = c && !c->worker && !c->held && c->release;
    if (dispose) survey_context = NULL;
    portEXIT_CRITICAL(&survey_lock);
    if (dispose) {
        heap_caps_free(c->banks);
        heap_caps_free(c);
    }
    return ESP_OK;
}
