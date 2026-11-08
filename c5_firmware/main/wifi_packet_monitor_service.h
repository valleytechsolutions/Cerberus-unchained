/* Private to wifi_scanner.c: shares its STA radio, RX callback and mutex. */
#include "wifi_packet_monitor_session.h"
#include "wifi_channel_analyzer_core.h"

/* Only one fixed-channel workflow can own the shared receiver. */
static union {
    cerberus_wifi_packet_monitor_ring_t monitor;
    cerberus_wifi_channel_analyzer_ring_t analyzer;
} packet_receive_history;
#define packet_monitor_ring packet_receive_history.monitor
#define channel_analyzer_ring packet_receive_history.analyzer
static bool channel_analyzer_mode;
static atomic_bool channel_analyzer_clock_invalid;
static wifi_promiscuous_filter_t channel_analyzer_previous_ctrl_filter;
/* RX accounting and latest hint are protected by packet_monitor_mux. */
static uint64_t channel_analyzer_receive_session;
static uint32_t channel_analyzer_frames;
static bool channel_analyzer_saturated;
static cerberus_wifi_channel_analyzer_hint_t channel_analyzer_hint;
static int64_t channel_analyzer_hint_us;
/* Immutable readout projection is protected by scanner_mutex. */
static cerberus_wifi_channel_analyzer_hint_t channel_analyzer_projection;
static uint32_t channel_analyzer_projection_id;
static bool channel_analyzer_projection_valid;

/* Protected by scanner_mutex. IDs are not reused within a boot. The random
 * boot seed reduces collisions across resets; it is not persistent identity. */
static uint64_t packet_monitor_session_epoch, packet_monitor_session_id;
static int64_t packet_monitor_lease_clock_us, packet_monitor_lease_deadline_us;
static bool packet_monitor_control_busy, packet_monitor_cleanup_retry;

#include "wifi_fox_hunt_receiver.h"
#include "wifi_packet_monitor_record_service.h"

static void packet_monitor_session_init_locked(void)
{
    if (packet_monitor_session_epoch) return;
    packet_monitor_session_epoch = ((uint64_t)esp_random() << 32) | esp_random();
    if (!packet_monitor_session_epoch || packet_monitor_session_epoch == UINT64_MAX)
        packet_monitor_session_epoch = UINT64_MAX / 2U;
}

static uint32_t packet_monitor_elapsed_locked(void)
{
    int64_t now = esp_timer_get_time();
    if (packet_monitor_started_us < 0 || now < packet_monitor_started_us) return 0U;
    uint64_t elapsed = (uint64_t)(now - packet_monitor_started_us) / 1000U;
    return elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
}

static void packet_monitor_expire_locked(int64_t now)
{
    if (!packet_monitor_session_id ||
        packet_monitor_status.state != CERBERUS_WIFI_PACKET_MONITOR_RUNNING ||
        atomic_load_explicit(&packet_monitor_cancelled, memory_order_acquire)) return;
    bool invalid_clock = now < 0 || now < packet_monitor_lease_clock_us;
    if (invalid_clock && (channel_analyzer_mode || wifi_fox_hunt_mode))
        atomic_store_explicit(&channel_analyzer_clock_invalid, true, memory_order_release);
    if (invalid_clock || now >= packet_monitor_lease_deadline_us)
        atomic_store_explicit(&packet_monitor_cancelled, true, memory_order_release);
    else packet_monitor_lease_clock_us = now;
}

static void packet_monitor_check_expiry(void)
{
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    packet_monitor_expire_locked(esp_timer_get_time());
    xSemaphoreGive(scanner_mutex);
}

static void packet_monitor_receive(const wifi_promiscuous_pkt_t *packet,
    wifi_promiscuous_pkt_type_t type)
{
    if (!packet || (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA && type != WIFI_PKT_CTRL) ||
        packet->rx_ctrl.rx_state != 0U) return;
    portENTER_CRITICAL(&packet_monitor_mux);
    bool admitted = packet_monitor_receiving &&
        packet->rx_ctrl.channel == packet_monitor_receive_channel;
    bool analyzer = admitted && channel_analyzer_mode;
    uint64_t analyzer_session = analyzer ? channel_analyzer_receive_session : 0U;
    if (analyzer) {
        if (channel_analyzer_frames != UINT32_MAX) ++channel_analyzer_frames;
        if (channel_analyzer_frames == UINT32_MAX) channel_analyzer_saturated = true;
    }
    bool monitor = admitted && !analyzer && !wifi_fox_hunt_mode && packet->rx_ctrl.sig_len >= 28U &&
        (type == WIFI_PKT_MGMT || type == WIFI_PKT_DATA);
    packet_monitor_record_context_t *record = monitor ? packet_monitor_record_context : NULL;
    if (record) ++record->inflight;
    if (monitor && type == WIFI_PKT_MGMT)
        cerberus_wifi_packet_monitor_observe(&packet_monitor_counts,
            packet->payload, packet->rx_ctrl.sig_len - 4U);
    portEXIT_CRITICAL(&packet_monitor_mux);
    if (analyzer && type == WIFI_PKT_MGMT && packet->rx_ctrl.sig_len >= 4U) {
        int64_t received_us = esp_timer_get_time();
        cerberus_wifi_channel_analyzer_hint_t hint;
        if (received_us >= 0 && cerberus_wifi_channel_analyzer_beacon(packet->payload,
                packet->rx_ctrl.sig_len - 4U, packet->rx_ctrl.channel, packet->rx_ctrl.rssi, &hint)) {
            portENTER_CRITICAL(&packet_monitor_mux);
            if (packet_monitor_receiving && channel_analyzer_mode &&
                channel_analyzer_receive_session == analyzer_session &&
                packet_monitor_receive_channel == hint.channel) {
                channel_analyzer_hint = hint;
                channel_analyzer_hint_us = received_us;
            }
            portEXIT_CRITICAL(&packet_monitor_mux);
        }
    }
    if (record) {
        /* IDF defines the payload by sig_len; exclude FCS for both supported
         * callback types. CTRL/MISC and error callbacks supply no bytes here. */
        (void)cerberus_monitor_record_push(&record->store, packet->payload,
            packet->rx_ctrl.sig_len - 4U, packet->rx_ctrl.channel,
            packet->rx_ctrl.rssi, esp_timer_get_time());
        portENTER_CRITICAL(&packet_monitor_mux);
        --record->inflight;
        portEXIT_CRITICAL(&packet_monitor_mux);
    }
}

/* Keep the reservation until both RX and the prior Wi-Fi band/channel are
 * restored. A failed restore leaves a retryable fault instead of allowing a
 * dual-band AP scan to inherit the monitor's fixed channel. */
static esp_err_t packet_monitor_restore_radio(void)
{
    if (packet_monitor_disable_pending) {
        esp_err_t error = esp_wifi_set_promiscuous(false);
        if (error != ESP_OK) return error;
        packet_monitor_disable_pending = false;
    }
    if (!packet_monitor_record_callbacks_idle()) return ESP_ERR_NOT_FINISHED;
    if (packet_monitor_restore_pending) {
        esp_err_t error = esp_wifi_set_band_mode(
            packet_monitor_previous_band_mode);
        if (error != ESP_OK) return error;
        if (packet_monitor_previous_channel != 0U) {
            error = esp_wifi_set_channel(packet_monitor_previous_channel,
                packet_monitor_previous_second);
            if (error != ESP_OK) return error;
        }
        error = esp_wifi_set_promiscuous_filter(&packet_monitor_previous_filter);
        if (error != ESP_OK) return error;
        if (channel_analyzer_mode || wifi_fox_hunt_mode) {
            error = esp_wifi_set_promiscuous_ctrl_filter(&channel_analyzer_previous_ctrl_filter);
            if (error != ESP_OK) return error;
        }
        packet_monitor_restore_pending = false;
    }
    atomic_store_explicit(&packet_monitor_radio_reserved, false,
        memory_order_release);
    return ESP_OK;
}

static esp_err_t channel_analyzer_run_windows(uint64_t session, uint8_t generation, uint8_t channel)
{
    portENTER_CRITICAL(&packet_monitor_mux);
    int64_t began = esp_timer_get_time();
    channel_analyzer_receive_session = session;
    channel_analyzer_frames = 0U;
    channel_analyzer_saturated = false;
    packet_monitor_receive_channel = channel;
    packet_monitor_receiving = began >= 0 && !atomic_load_explicit(&packet_monitor_cancelled,memory_order_acquire);
    portEXIT_CRITICAL(&packet_monitor_mux);
    if (began < 0) return ESP_ERR_INVALID_STATE;
    TickType_t last = xTaskGetTickCount();
    while (!atomic_load_explicit(&packet_monitor_cancelled,memory_order_acquire)) {
        vTaskDelayUntil(&last,pdMS_TO_TICKS(CERBERUS_WIFI_CHANNEL_ANALYZER_TARGET_MS));
        packet_monitor_check_expiry();
        portENTER_CRITICAL(&packet_monitor_mux);
        int64_t now = esp_timer_get_time();
        bool valid = now > began && (uint64_t)(now-began) <= UINT32_MAX;
        bool cancelled = atomic_load_explicit(&packet_monitor_cancelled,memory_order_acquire);
        uint32_t duration = valid ? (uint32_t)(now-began) : 0U;
        uint32_t frames = channel_analyzer_frames;
        uint8_t flags = channel_analyzer_saturated ? CERBERUS_WIFI_CHANNEL_ANALYZER_SATURATED : 0U;
        bool complete = valid && !cancelled && duration >= CERBERUS_WIFI_CHANNEL_ANALYZER_TARGET_MS*1000U;
        if (complete) {
            channel_analyzer_frames=0U;channel_analyzer_saturated=false;began=now;
        }
        portEXIT_CRITICAL(&packet_monitor_mux);
        if (!valid) return ESP_ERR_INVALID_STATE;
        if (cancelled) break;
        if (!complete) continue;
        xSemaphoreTake(scanner_mutex,portMAX_DELAY);
        bool same = channel_analyzer_mode && packet_monitor_session_id==session &&
            packet_monitor_status.generation==generation &&
            packet_monitor_status.state==CERBERUS_WIFI_PACKET_MONITOR_RUNNING;
        bool stored = same && cerberus_wifi_channel_analyzer_push(&channel_analyzer_ring,
            frames,duration,channel,flags);
        xSemaphoreGive(scanner_mutex);
        if (!stored) return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

#include "wifi_fox_hunt_worker.h"

/* One exclusive scanner job. Stop keeps the radio reserved until cleanup. */
static void packet_monitor_run(void)
{
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    bool running = packet_monitor_status.state ==
        CERBERUS_WIFI_PACKET_MONITOR_RUNNING;
    bool retry = packet_monitor_cleanup_retry;
    packet_monitor_cleanup_retry = false;
    packet_monitor_expire_locked(esp_timer_get_time());
    uint8_t generation = packet_monitor_status.generation;
    uint64_t session = packet_monitor_session_id;
    uint8_t channel = packet_monitor_status.channel;
    bool analyzer = channel_analyzer_mode;
    bool fox = wifi_fox_hunt_mode;
    xSemaphoreGive(scanner_mutex);
    if (!running && !retry) return;

    esp_err_t error = ESP_OK;
    bool promiscuous = false;
    if (running && !atomic_load_explicit(&packet_monitor_cancelled,
            memory_order_acquire)) {
        if (fox) error = wifi_fox_hunt_prepare_channels();
        if (error == ESP_OK) error = esp_wifi_get_band_mode(
            &packet_monitor_previous_band_mode);
        if (error == ESP_OK)
            error = esp_wifi_get_channel(&packet_monitor_previous_channel,
                &packet_monitor_previous_second);
        if (error == ESP_OK)
            error = esp_wifi_get_promiscuous_filter(&packet_monitor_previous_filter);
        if (error == ESP_OK && (analyzer || fox))
            error = esp_wifi_get_promiscuous_ctrl_filter(&channel_analyzer_previous_ctrl_filter);
        if (error == ESP_OK) {
            packet_monitor_restore_pending = true;
            error = esp_wifi_set_band(channel <= 14U ?
                WIFI_BAND_2G : WIFI_BAND_5G);
        }
        if (error == ESP_OK)
            error = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
        wifi_promiscuous_filter_t filter = {
            .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT |
                ((packet_monitor_record_requested || analyzer || fox) ? WIFI_PROMIS_FILTER_MASK_DATA : 0U) |
                ((analyzer || fox) ? WIFI_PROMIS_FILTER_MASK_CTRL : 0U),
        };
        if (error == ESP_OK) error = esp_wifi_set_promiscuous_filter(&filter);
        if (error == ESP_OK && (analyzer || fox)) {
            wifi_promiscuous_filter_t controls = {.filter_mask = WIFI_PROMIS_CTRL_FILTER_MASK_ALL};
            error = esp_wifi_set_promiscuous_ctrl_filter(&controls);
        }
        if (error == ESP_OK) {
            packet_monitor_disable_pending = true;
            error = esp_wifi_set_promiscuous(true);
            promiscuous = error == ESP_OK;
        }
    }
    if (promiscuous && analyzer) error = channel_analyzer_run_windows(session,generation,channel);
    if (promiscuous && fox) error = wifi_fox_hunt_run();
    if (promiscuous && !analyzer && !fox) {
        portENTER_CRITICAL(&packet_monitor_mux);
        packet_monitor_counts = (cerberus_wifi_packet_monitor_counters_t){0};
        packet_monitor_receive_channel = channel;
        packet_monitor_receiving = !atomic_load_explicit(&packet_monitor_cancelled, memory_order_acquire);
        portEXIT_CRITICAL(&packet_monitor_mux);
        TickType_t last_sample = xTaskGetTickCount();
        while (!atomic_load_explicit(&packet_monitor_cancelled,
                memory_order_acquire)) {
            vTaskDelayUntil(&last_sample,
                pdMS_TO_TICKS(CERBERUS_WIFI_PACKET_MONITOR_SAMPLE_MS));
            packet_monitor_check_expiry();
            if (packet_monitor_record_faulted())
                atomic_store_explicit(&packet_monitor_cancelled, true, memory_order_release);
            if (atomic_load_explicit(&packet_monitor_cancelled,
                    memory_order_acquire)) break;
            portENTER_CRITICAL(&packet_monitor_mux);
            cerberus_wifi_packet_monitor_counters_t counts =
                packet_monitor_counts;
            packet_monitor_counts =
                (cerberus_wifi_packet_monitor_counters_t){0};
            portEXIT_CRITICAL(&packet_monitor_mux);
            xSemaphoreTake(scanner_mutex, portMAX_DELAY);
            uint32_t elapsed_ms = packet_monitor_elapsed_locked();
            if (packet_monitor_session_id == session &&
                packet_monitor_status.generation == generation &&
                packet_monitor_status.state ==
                    CERBERUS_WIFI_PACKET_MONITOR_RUNNING) {
                cerberus_wifi_packet_monitor_ring_push(&packet_monitor_ring,
                    generation, channel, &counts, elapsed_ms);
                packet_monitor_status.latest_sequence =
                    packet_monitor_ring.latest_sequence;
                packet_monitor_status.stored_count =
                    packet_monitor_ring.stored_count;
                packet_monitor_status.elapsed_ms = elapsed_ms;
            }
            xSemaphoreGive(scanner_mutex);
        }
    }
    packet_monitor_record_close_admission();
    if ((analyzer || fox) && atomic_load_explicit(&channel_analyzer_clock_invalid, memory_order_acquire))
        error = ESP_ERR_INVALID_STATE;
    if (packet_monitor_record_faulted() && error == ESP_OK) error = ESP_ERR_INVALID_STATE;
    esp_err_t cleanup = packet_monitor_restore_radio();
    if (cleanup != ESP_OK) error = cleanup;
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    if (packet_monitor_session_id == session &&
        packet_monitor_status.generation == generation) {
        packet_monitor_status.elapsed_ms = packet_monitor_elapsed_locked();
        packet_monitor_status.error_code = error;
        packet_monitor_status.state = error == ESP_OK
            ? CERBERUS_WIFI_PACKET_MONITOR_IDLE
            : CERBERUS_WIFI_PACKET_MONITOR_ERROR;
        packet_monitor_record_clock_locked(esp_timer_get_time(), false);
    }
    xSemaphoreGive(scanner_mutex);
}

bool cerberus_wifi_packet_monitor_active(void)
{
    return atomic_load_explicit(&packet_monitor_radio_reserved,
        memory_order_acquire);
}

static bool packet_monitor_held_locked(void)
{
    return packet_monitor_status.state == CERBERUS_WIFI_PACKET_MONITOR_RUNNING ||
        atomic_load_explicit(&packet_monitor_radio_reserved, memory_order_acquire);
}

static esp_err_t packet_monitor_start_internal(uint8_t channel, uint64_t session, bool recording, bool analyzer,
    const cerberus_wifi_fox_hunt_start_t *fox)
{
    if (fox ? !cerberus_wifi_fox_hunt_start_valid(fox) : !cerberus_wifi_packet_monitor_channel_valid(channel))
        return ESP_ERR_INVALID_ARG;
    if (!scanner_mutex || !scanner_task_handle)
        return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    if (session) packet_monitor_session_init_locked();
    if (session && session == packet_monitor_session_id) {
        packet_monitor_expire_locked(esp_timer_get_time());
        bool same = packet_monitor_status.channel == channel && packet_monitor_record_requested == recording &&
            channel_analyzer_mode == analyzer && wifi_fox_hunt_mode == (fox != NULL) &&
            (!fox || memcmp(&wifi_fox_hunt_request, fox, sizeof(*fox)) == 0);
        xSemaphoreGive(scanner_mutex);
        return same ? ESP_OK : ESP_ERR_INVALID_ARG;
    }
    if (session &&
        (packet_monitor_session_epoch == UINT64_MAX ||
         session != packet_monitor_session_epoch + 1U)) {
        xSemaphoreGive(scanner_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    /* Consume a valid request even when busy. A delayed replay of a rejected
     * Start must not become a new scan after the competing owner finishes. */
    if (session) packet_monitor_session_epoch = session;
    if (packet_monitor_control_busy || packet_monitor_held_locked() || packet_monitor_record_context) {
        bool legacy_replay = !fox && !wifi_fox_hunt_mode && !analyzer && !channel_analyzer_mode && !session && !packet_monitor_session_id &&
            packet_monitor_status.state == CERBERUS_WIFI_PACKET_MONITOR_RUNNING &&
            packet_monitor_status.channel == channel;
        xSemaphoreGive(scanner_mutex);
        return legacy_replay ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    packet_monitor_control_busy = true;
    xSemaphoreGive(scanner_mutex);
    esp_err_t error = ESP_OK;
    if (cerberus_c5_test_running() ||
        atomic_load_explicit(&mac_radio_reserved, memory_order_acquire))
        error = ESP_ERR_INVALID_STATE;
    if (error == ESP_OK) error = cerberus_wifi_channel_survey_preempt();
    if (error == ESP_OK && another_radio_operation_is_active())
        error = ESP_ERR_INVALID_STATE;
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    int64_t now = esp_timer_get_time();
    if (error == ESP_OK && (now < 0 || (session &&
        now > INT64_MAX - (int64_t)CERBERUS_WIFI_PACKET_MONITOR_LEASE_MS * 1000)))
        error = ESP_ERR_INVALID_STATE;
    if (error == ESP_OK && (!scanner_ready || (atomic_load_explicit(&probe_sniff_radio_reserved,memory_order_acquire)||(atomic_load_explicit(&ap_survey_radio_reserved,memory_order_acquire)||atomic_load_explicit(&packet_count_radio_reserved,memory_order_acquire))) || station_reserved || atomic_load_explicit(&mac_radio_reserved,memory_order_acquire) || atomic_load_explicit(&csi_radio_reserved,memory_order_acquire) ||
        scan_status.state == CERBERUS_WIFI_SCAN_RUNNING ||
        defense_status.state == CERBERUS_WIFI_DEFENSE_RUNNING ||
        wifi_connection_is_active_locked() ||
        packet_monitor_held_locked())) error = ESP_ERR_INVALID_STATE;
    if (error == ESP_OK && recording) {
        if (now > INT64_MAX - (int64_t)CERBERUS_MONITOR_RECORD_RETENTION_MS * 1000)
            error = ESP_ERR_INVALID_STATE;
        else error = packet_monitor_record_create_locked(session, now);
    }
    packet_monitor_control_busy = false;
    if (error != ESP_OK) {
        xSemaphoreGive(scanner_mutex);
        return error;
    }
    uint8_t generation = (uint8_t)(packet_monitor_status.generation + 1U);
    if (generation == 0U) generation = 1U;
    memset(&packet_receive_history, 0, sizeof(packet_receive_history));
    portENTER_CRITICAL(&packet_monitor_mux);
    channel_analyzer_mode=analyzer;
    wifi_fox_hunt_mode=fox != NULL;
    wifi_fox_hunt_request=fox ? *fox : (cerberus_wifi_fox_hunt_start_t){0};
    wifi_fox_hunt_latest=(cerberus_wifi_fox_hunt_signal_t){.session_id=session,
        .age_ms=UINT32_MAX,.rssi=CERBERUS_WIFI_FOX_HUNT_RSSI_UNKNOWN};
    wifi_fox_hunt_observed_us=-1;wifi_fox_hunt_clock_us=now;wifi_fox_hunt_armed_us=now;
    wifi_fox_hunt_sweeps=0U;wifi_fox_hunt_searching=wifi_fox_hunt_skipped=false;
    wifi_fox_hunt_tune_error=ESP_OK;wifi_fox_hunt_channel_count=0U;
    channel_analyzer_receive_session=0U;
    memset(&channel_analyzer_hint,0,sizeof(channel_analyzer_hint));
    channel_analyzer_hint_us=-1;
    portEXIT_CRITICAL(&packet_monitor_mux);
    channel_analyzer_projection_id=0U;channel_analyzer_projection_valid=false;
    atomic_store_explicit(&channel_analyzer_clock_invalid, false, memory_order_release);
    packet_monitor_restore_pending = false;
    packet_monitor_disable_pending = false;
    packet_monitor_previous_channel = 0U;
    packet_monitor_status = (cerberus_wifi_packet_monitor_status_payload_t){
        .state = CERBERUS_WIFI_PACKET_MONITOR_RUNNING,
        .generation = generation,
        .channel = channel,
        .sample_ms = CERBERUS_WIFI_PACKET_MONITOR_SAMPLE_MS,
    };
    packet_monitor_started_us = now;
    packet_monitor_session_id = session;
    packet_monitor_record_requested = recording;
    packet_monitor_cleanup_retry = false;
    if (session) {
        packet_monitor_lease_clock_us = now;
        packet_monitor_lease_deadline_us = now +
            (int64_t)CERBERUS_WIFI_PACKET_MONITOR_LEASE_MS * 1000;
    }
    atomic_store_explicit(&packet_monitor_cancelled, false, memory_order_release);
    atomic_store_explicit(&packet_monitor_radio_reserved, true,
        memory_order_release);
    xSemaphoreGive(scanner_mutex);
    xTaskNotifyGive(scanner_task_handle);
    return ESP_OK;
}

esp_err_t cerberus_wifi_packet_monitor_start(uint8_t channel)
{
    return packet_monitor_start_internal(channel, 0U, false, false, NULL);
}

esp_err_t cerberus_wifi_packet_monitor_stop(uint8_t generation)
{
    if (!scanner_mutex) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    if (packet_monitor_session_id || packet_monitor_control_busy ||
        generation == 0U || generation != packet_monitor_status.generation) {
        xSemaphoreGive(scanner_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (packet_monitor_status.state == CERBERUS_WIFI_PACKET_MONITOR_RUNNING) {
        atomic_store_explicit(&packet_monitor_cancelled, true,
            memory_order_release);
        xSemaphoreGive(scanner_mutex);
        return ESP_OK;
    }
    bool held = atomic_load_explicit(&packet_monitor_radio_reserved,
        memory_order_acquire);
    if (!held) {
        if (packet_monitor_status.state == CERBERUS_WIFI_PACKET_MONITOR_ERROR) {
            packet_monitor_status.state = CERBERUS_WIFI_PACKET_MONITOR_IDLE;
            packet_monitor_status.error_code = ESP_OK;
        }
        xSemaphoreGive(scanner_mutex);
        return ESP_OK;
    }
    esp_err_t error = packet_monitor_restore_radio();
    if (error == ESP_OK) {
        packet_monitor_status.state = CERBERUS_WIFI_PACKET_MONITOR_IDLE;
        packet_monitor_status.error_code = ESP_OK;
    } else {
        packet_monitor_status.state = CERBERUS_WIFI_PACKET_MONITOR_ERROR;
        packet_monitor_status.error_code = error;
    }
    xSemaphoreGive(scanner_mutex);
    return error;
}

void cerberus_wifi_packet_monitor_get_status(
    cerberus_wifi_packet_monitor_status_payload_t *status)
{
    if (!status) return;
    memset(status, 0, sizeof(*status));
    if (!scanner_mutex) {
        status->state = CERBERUS_WIFI_PACKET_MONITOR_ERROR;
        status->error_code = ESP_ERR_INVALID_STATE;
        return;
    }
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    if (channel_analyzer_mode || wifi_fox_hunt_mode) {
        status->state=CERBERUS_WIFI_PACKET_MONITOR_ERROR;
        status->error_code=ESP_ERR_INVALID_STATE;
        xSemaphoreGive(scanner_mutex);
        return;
    }
    *status = packet_monitor_status;
    if (status->state == CERBERUS_WIFI_PACKET_MONITOR_RUNNING)
        status->elapsed_ms = packet_monitor_elapsed_locked();
    xSemaphoreGive(scanner_mutex);
}

esp_err_t cerberus_wifi_packet_monitor_get_sample(uint8_t generation,
    uint16_t sequence, cerberus_wifi_packet_monitor_sample_payload_t *sample)
{
    if (!sample || !scanner_mutex) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    bool valid = !packet_monitor_session_id &&
        generation == packet_monitor_status.generation &&
        cerberus_wifi_packet_monitor_ring_get(&packet_monitor_ring,
            sequence, sample);
    xSemaphoreGive(scanner_mutex);
    return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static void packet_monitor_session_reply_locked(uint64_t requested, bool info,
    esp_err_t error, cerberus_wifi_packet_monitor_session_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->session_id = info ? packet_monitor_session_epoch : requested;
    bool bound = !channel_analyzer_mode && !wifi_fox_hunt_mode && packet_monitor_session_id && out->session_id == packet_monitor_session_id;
    if (channel_analyzer_mode || wifi_fox_hunt_mode) {
        if (info && packet_monitor_held_locked()) out->flags=CERBERUS_WIFI_PACKET_MONITOR_HELD;
        out->error_code=info ? (packet_monitor_held_locked() ? ESP_ERR_INVALID_STATE : ESP_OK) : ESP_ERR_NOT_FOUND;
        return;
    }
    if (info || bound) {
        out->state = packet_monitor_status.state;
        out->channel = packet_monitor_status.channel;
        out->stored_count = packet_monitor_status.stored_count;
        out->latest_sequence = packet_monitor_status.latest_sequence;
        out->elapsed_ms = out->state == CERBERUS_WIFI_PACKET_MONITOR_RUNNING ?
            packet_monitor_elapsed_locked() : packet_monitor_status.elapsed_ms;
        if (bound) out->flags |= CERBERUS_WIFI_PACKET_MONITOR_BOUND;
        if (bound && packet_monitor_record_context)
            out->flags |= CERBERUS_WIFI_PACKET_MONITOR_RECORDING;
        if (packet_monitor_held_locked()) out->flags |= CERBERUS_WIFI_PACKET_MONITOR_HELD;
        if (bound && atomic_load_explicit(&packet_monitor_cancelled, memory_order_acquire))
            out->flags |= CERBERUS_WIFI_PACKET_MONITOR_CANCELLED;
        out->error_code = error == ESP_OK ? packet_monitor_status.error_code : error;
    } else out->error_code = error;
}

void cerberus_wifi_packet_monitor_session_info(cerberus_wifi_packet_monitor_session_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!scanner_mutex) { out->error_code = ESP_ERR_INVALID_STATE; return; }
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    packet_monitor_session_init_locked();
    packet_monitor_expire_locked(esp_timer_get_time());
    packet_monitor_session_reply_locked(0U, true, ESP_OK, out);
    xSemaphoreGive(scanner_mutex);
}

esp_err_t cerberus_wifi_packet_monitor_session_start(
    const cerberus_wifi_packet_monitor_session_start_t *request,
    cerberus_wifi_packet_monitor_session_status_t *out)
{
    if (!request || !out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out)); out->session_id = request->session_id;
    if (!scanner_mutex) { out->error_code = ESP_ERR_INVALID_STATE; return ESP_ERR_INVALID_STATE; }
    esp_err_t error = request->session_id ?
        packet_monitor_start_internal(request->channel, request->session_id, false, false, NULL) : ESP_ERR_INVALID_ARG;
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    packet_monitor_session_reply_locked(request->session_id, false, error, out);
    xSemaphoreGive(scanner_mutex);
    return error;
}

esp_err_t cerberus_wifi_packet_monitor_record_start(
    const cerberus_wifi_packet_monitor_session_start_t *request,
    cerberus_wifi_packet_monitor_session_status_t *out)
{
    if (!request || !out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out)); out->session_id = request->session_id;
    if (!scanner_mutex) { out->error_code = ESP_ERR_INVALID_STATE; return ESP_ERR_INVALID_STATE; }
    esp_err_t error = request->session_id ?
        packet_monitor_start_internal(request->channel, request->session_id, true, false, NULL) : ESP_ERR_INVALID_ARG;
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    packet_monitor_session_reply_locked(request->session_id, false, error, out);
    xSemaphoreGive(scanner_mutex);
    return error;
}

static esp_err_t packet_monitor_session_control(uint64_t session, bool stop,
    cerberus_wifi_packet_monitor_session_status_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out)); out->session_id = session;
    if (!scanner_mutex) { out->error_code = ESP_ERR_INVALID_STATE; return ESP_ERR_INVALID_STATE; }
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    esp_err_t error = !channel_analyzer_mode && !wifi_fox_hunt_mode && session && session == packet_monitor_session_id ? ESP_OK : ESP_ERR_NOT_FOUND;
    bool notify = false;
    if (error == ESP_OK) {
        int64_t now = esp_timer_get_time();
        packet_monitor_expire_locked(now);
        if (stop) {
            atomic_store_explicit(&packet_monitor_cancelled, true, memory_order_release);
            packet_monitor_record_close_admission();
            if (packet_monitor_status.state == CERBERUS_WIFI_PACKET_MONITOR_ERROR &&
                packet_monitor_held_locked()) packet_monitor_cleanup_retry = true;
            notify = packet_monitor_held_locked();
        } else if (packet_monitor_status.state == CERBERUS_WIFI_PACKET_MONITOR_RUNNING &&
            !atomic_load_explicit(&packet_monitor_cancelled, memory_order_acquire)) {
            if (now > INT64_MAX - (int64_t)CERBERUS_WIFI_PACKET_MONITOR_LEASE_MS * 1000)
                atomic_store_explicit(&packet_monitor_cancelled, true, memory_order_release);
            else packet_monitor_lease_deadline_us = now +
                (int64_t)CERBERUS_WIFI_PACKET_MONITOR_LEASE_MS * 1000;
        }
    }
    packet_monitor_session_reply_locked(session, false, error, out);
    xSemaphoreGive(scanner_mutex);
    if (notify) xTaskNotifyGive(scanner_task_handle);
    return error;
}

esp_err_t cerberus_wifi_packet_monitor_session_status(uint64_t session,
    cerberus_wifi_packet_monitor_session_status_t *out)
{
    return packet_monitor_session_control(session, false, out);
}

esp_err_t cerberus_wifi_packet_monitor_session_stop(uint64_t session,
    cerberus_wifi_packet_monitor_session_status_t *out)
{
    return packet_monitor_session_control(session, true, out);
}

esp_err_t cerberus_wifi_packet_monitor_session_sample(
    const cerberus_wifi_packet_monitor_session_read_t *request,
    cerberus_wifi_packet_monitor_session_sample_t *out)
{
    if (!request || !out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!scanner_mutex) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    bool valid = !channel_analyzer_mode && !wifi_fox_hunt_mode && request->session_id && request->session_id == packet_monitor_session_id &&
        cerberus_wifi_packet_monitor_ring_get(&packet_monitor_ring, request->sequence, &out->sample);
    if (valid) out->session_id = request->session_id;
    else memset(out, 0, sizeof(*out));
    xSemaphoreGive(scanner_mutex);
    return valid ? ESP_OK : ESP_ERR_NOT_FOUND;
}

#include "wifi_channel_analyzer_service.h"

#include "wifi_fox_hunt_service.h"
