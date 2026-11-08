/* Private to wifi_scanner.c. Shares its STA radio, RX callback and mutex. */

/* Scoped ownership and one immutable metric projection, not a second bank.
 * Access under scanner_mutex except the existing atomic cancellation flag. */
static uint64_t channel_survey_session_epoch;
static uint64_t channel_survey_session_id;
static int64_t channel_survey_lease_deadline_us;
static int64_t channel_survey_lease_clock_us;
static bool channel_survey_control_busy;
static uint32_t channel_survey_pin_counter;
static uint32_t channel_survey_pin;
static uint64_t channel_survey_pin_session;
static uint8_t channel_survey_pin_kind, channel_survey_pin_index;
static uint8_t channel_survey_pin_bytes[22];

static void channel_survey_session_init_locked(void)
{
    /* Only boot initialization calls this. No persistence/eternal uniqueness
     * claim: the random seed can collide across boots. Within boot no reuse. */
    channel_survey_session_epoch = ((uint64_t)esp_random() << 32) | esp_random();
    if (!channel_survey_session_epoch || channel_survey_session_epoch == UINT64_MAX)
        channel_survey_session_epoch = UINT64_MAX / 2U;
}

static bool channel_survey_held_locked(void)
{
    return channel_survey_status.state == CERBERUS_WIFI_CHANNEL_SURVEY_RUNNING ||
        atomic_load_explicit(&channel_survey_radio_reserved, memory_order_acquire);
}

static bool channel_survey_expire_locked(int64_t now)
{
    if (!channel_survey_session_id || channel_survey_status.state != CERBERUS_WIFI_CHANNEL_SURVEY_RUNNING ||
        atomic_load_explicit(&channel_survey_cancelled, memory_order_acquire)) return false;
    if (now < 0 || now < channel_survey_lease_clock_us ||
        now >= channel_survey_lease_deadline_us) {
        atomic_store_explicit(&channel_survey_cancelled, true, memory_order_release);
        return true;
    }
    channel_survey_lease_clock_us = now;
    return false;
}

static void channel_survey_check_expiry(void)
{
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    (void)channel_survey_expire_locked(esp_timer_get_time());
    xSemaphoreGive(scanner_mutex);
}

static uint32_t channel_survey_elapsed_locked(void)
{
    int64_t now = esp_timer_get_time();
    if (now < channel_survey_started_us || channel_survey_started_us < 0) return 0U;
    uint64_t ms = (uint64_t)(now - channel_survey_started_us) / 1000U;
    return ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
}

static const uint8_t channel_survey_24[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
};
static const uint8_t channel_survey_5[] = {
    36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116,
    120, 124, 128, 132, 136, 140, 144, 149, 153, 157, 161, 165,
};
_Static_assert(sizeof(channel_survey_5) <= CERBERUS_WIFI_CHANNEL_SURVEY_MAX_RESULTS,
    "channel survey result array is too small");

static void channel_survey_receive(const wifi_promiscuous_pkt_t *packet,
    wifi_promiscuous_pkt_type_t type)
{
    if (!packet || type == WIFI_PKT_MISC || packet->rx_ctrl.rx_state != 0 ||
        packet->rx_ctrl.sig_len <= 4U) return;
    portENTER_CRITICAL(&channel_survey_mux);
    if (channel_survey_receiving &&
        packet->rx_ctrl.channel == channel_survey_receive_channel) {
        if (channel_survey_packet_count < UINT32_MAX) {
            ++channel_survey_packet_count;
            channel_survey_rssi_sum += packet->rx_ctrl.rssi;
        }
        if (packet->rx_ctrl.rssi > channel_survey_strongest_rssi)
            channel_survey_strongest_rssi = packet->rx_ctrl.rssi;
    }
    portEXIT_CRITICAL(&channel_survey_mux);
}

static void channel_survey_task(void *arg)
{
    (void)arg;
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        xSemaphoreTake(scanner_mutex, portMAX_DELAY);
        bool running = channel_survey_status.state == CERBERUS_WIFI_CHANNEL_SURVEY_RUNNING;
        uint8_t generation = channel_survey_status.generation;
        uint64_t session = channel_survey_session_id;
        (void)channel_survey_expire_locked(esp_timer_get_time());
        uint8_t band = channel_survey_status.band;
        uint8_t count = channel_survey_status.result_count;
        xSemaphoreGive(scanner_mutex);
        if (!running) continue;

        esp_err_t error = ESP_OK;
        bool promiscuous = false;
        if (!atomic_load_explicit(&channel_survey_cancelled, memory_order_acquire)) {
            wifi_promiscuous_filter_t filter = {
                .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT |
                    WIFI_PROMIS_FILTER_MASK_CTRL | WIFI_PROMIS_FILTER_MASK_DATA,
            };
            error = esp_wifi_set_promiscuous_filter(&filter);
            channel_survey_check_expiry();
            if (error == ESP_OK && !atomic_load_explicit(&channel_survey_cancelled, memory_order_acquire)) {
                error = esp_wifi_set_promiscuous(true);
                promiscuous = error == ESP_OK;
                if (promiscuous)
                    atomic_store_explicit(&channel_survey_radio_reserved, true,
                        memory_order_release);
            }
        }

        while (error == ESP_OK && promiscuous &&
            !atomic_load_explicit(&channel_survey_cancelled, memory_order_acquire)) {
            uint8_t successful_channels = 0;
            esp_err_t first_channel_error = ESP_OK;
            for (uint8_t index = 0; index < count; ++index) {
                channel_survey_check_expiry();
                if (atomic_load_explicit(&channel_survey_cancelled, memory_order_acquire)) break;
                /* This background observer yields to operations using the same C5 radio. */
                if (another_radio_operation_is_active() || cerberus_c5_test_running()) {
                    atomic_store_explicit(&channel_survey_cancelled, true, memory_order_release);
                    break;
                }
                uint8_t channel = channel_survey_channels[index];
                xSemaphoreTake(scanner_mutex, portMAX_DELAY);
                channel_survey_status.current_channel = channel;
                xSemaphoreGive(scanner_mutex);
                esp_err_t next = esp_wifi_set_band(
                    band == CERBERUS_WIFI_CHANNEL_SURVEY_BAND_24 ? WIFI_BAND_2G : WIFI_BAND_5G);
                channel_survey_check_expiry();
                if (atomic_load_explicit(&channel_survey_cancelled, memory_order_acquire)) break;
                if (next == ESP_OK)
                    next = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
                if (next != ESP_OK) {
                    if (first_channel_error == ESP_OK) first_channel_error = next;
                    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
                    channel_survey_status.completed_count = (uint8_t)(index + 1U);
                    xSemaphoreGive(scanner_mutex);
                    continue; /* Country and driver support determine usable channels. */
                }
                ++successful_channels;
                portENTER_CRITICAL(&channel_survey_mux);
                channel_survey_receive_channel = channel;
                channel_survey_packet_count = 0;
                channel_survey_rssi_sum = 0;
                channel_survey_strongest_rssi = -127;
                int64_t opened_us = esp_timer_get_time();
                channel_survey_receiving = opened_us >= 0;
                portEXIT_CRITICAL(&channel_survey_mux);
                if (opened_us < 0) { error = ESP_ERR_INVALID_STATE; break; }
                int64_t observed_us = opened_us;
                while (observed_us - opened_us < (int64_t)CERBERUS_WIFI_CHANNEL_SURVEY_DWELL_MS * 1000 &&
                    !atomic_load_explicit(&channel_survey_cancelled, memory_order_acquire)) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    channel_survey_check_expiry();
                    int64_t next_us = esp_timer_get_time();
                    if (next_us <= observed_us) { error = ESP_ERR_INVALID_STATE; break; }
                    observed_us = next_us;
                }
                uint32_t packets;
                int64_t rssi_sum;
                int8_t strongest;
                portENTER_CRITICAL(&channel_survey_mux);
                int64_t closed_us = esp_timer_get_time();
                channel_survey_receiving = false;
                packets = channel_survey_packet_count;
                rssi_sum = channel_survey_rssi_sum;
                strongest = channel_survey_strongest_rssi;
                portEXIT_CRITICAL(&channel_survey_mux);
                if (closed_us < observed_us) error = ESP_ERR_INVALID_STATE;
                if (error != ESP_OK) break;
                if (closed_us - opened_us >= (int64_t)CERBERUS_WIFI_CHANNEL_SURVEY_DWELL_MS * 1000 &&
                    !atomic_load_explicit(&channel_survey_cancelled, memory_order_acquire)) {
                    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
                    cerberus_wifi_channel_survey_result_payload_t *result =
                        &channel_survey_results[index];
                    uint32_t updated_ms = channel_survey_elapsed_locked();
                    if (cerberus_wifi_channel_survey_metrics_update(result,
                            packets, rssi_sum, strongest, (uint64_t)(closed_us - opened_us), updated_ms)) {
                        channel_survey_status.completed_count = (uint8_t)(index + 1U);
                        channel_survey_status.elapsed_ms = result->updated_ms;
                        channel_survey_status.total_packets =
                            UINT32_MAX - channel_survey_status.total_packets < packets
                            ? UINT32_MAX : channel_survey_status.total_packets + packets;
                    } else error = ESP_ERR_INVALID_STATE;
                    xSemaphoreGive(scanner_mutex);
                    if (error != ESP_OK) break;
                }
            }
            if (error != ESP_OK || atomic_load_explicit(&channel_survey_cancelled, memory_order_acquire)) break;
            if (successful_channels == 0U) {
                error = first_channel_error == ESP_OK ? ESP_ERR_NOT_SUPPORTED : first_channel_error;
                break;
            }
            xSemaphoreTake(scanner_mutex, portMAX_DELAY);
            if (channel_survey_status.total_sweeps < UINT16_MAX)
                ++channel_survey_status.total_sweeps;
            channel_survey_status.completed_count = 0;
            xSemaphoreGive(scanner_mutex);
        }

        if (session) atomic_store_explicit(&channel_survey_cancelled, true, memory_order_release);
        portENTER_CRITICAL(&channel_survey_mux);
        channel_survey_receiving = false;
        portEXIT_CRITICAL(&channel_survey_mux);
        if (promiscuous) {
            esp_err_t operation_error = error;
            esp_err_t cleanup;
            do {
                cleanup = esp_wifi_set_promiscuous(false);
                if (cleanup == ESP_OK) {
                    atomic_store_explicit(&channel_survey_radio_reserved, false,
                        memory_order_release);
                } else {
                    error = cleanup;
                    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
                    channel_survey_status.error_code = cleanup;
                    /* Scoped worker remains RUNNING while cleanup is held;
                     * no false IDLE or new owner between disable attempts. */
                    xSemaphoreGive(scanner_mutex);
                    if (session) vTaskDelay(pdMS_TO_TICKS(250));
                }
            } while (session && cleanup != ESP_OK);
            /* A recovered cleanup error is no longer a current driver fault. */
            if (cleanup == ESP_OK) error = operation_error;
        }
        xSemaphoreTake(scanner_mutex, portMAX_DELAY);
        if (channel_survey_status.generation == generation && channel_survey_session_id == session) {
            channel_survey_status.current_channel = 0U;
            channel_survey_status.elapsed_ms = channel_survey_elapsed_locked();
            channel_survey_status.error_code = error;
            channel_survey_status.state = error == ESP_OK
                ? CERBERUS_WIFI_CHANNEL_SURVEY_IDLE : CERBERUS_WIFI_CHANNEL_SURVEY_ERROR;
        }
        xSemaphoreGive(scanner_mutex);
    }
}

static esp_err_t channel_survey_preempt_internal(bool legacy)
{
    if (!scanner_mutex) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    if (channel_survey_control_busy || (legacy && channel_survey_session_id && channel_survey_held_locked())) {
        xSemaphoreGive(scanner_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    channel_survey_control_busy = true;
    bool scoped = channel_survey_session_id != 0U;
    bool running = channel_survey_status.state == CERBERUS_WIFI_CHANNEL_SURVEY_RUNNING;
    if (running) atomic_store_explicit(&channel_survey_cancelled, true, memory_order_release);
    xSemaphoreGive(scanner_mutex);
    uint32_t started = (uint32_t)(esp_timer_get_time() / 1000LL);
    while (running && (uint32_t)((esp_timer_get_time() / 1000LL) - started) < 1000U) {
        vTaskDelay(pdMS_TO_TICKS(10));
        xSemaphoreTake(scanner_mutex, portMAX_DELAY);
        running = channel_survey_status.state == CERBERUS_WIFI_CHANNEL_SURVEY_RUNNING;
        xSemaphoreGive(scanner_mutex);
    }
    esp_err_t error = running ? ESP_ERR_TIMEOUT : ESP_OK;
    if (!running && atomic_load_explicit(&channel_survey_radio_reserved, memory_order_acquire)) {
        /* Scoped cleanup belongs to its still-retained worker. The native
         * arbiter must never race a second SDK disable against that worker. */
        error = scoped ? ESP_ERR_TIMEOUT : esp_wifi_set_promiscuous(false);
        if (error == ESP_OK)
            atomic_store_explicit(&channel_survey_radio_reserved, false, memory_order_release);
    }
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    if (error == ESP_OK) {
        channel_survey_status.state = CERBERUS_WIFI_CHANNEL_SURVEY_IDLE;
        channel_survey_status.error_code = ESP_OK;
    } else channel_survey_status.error_code = error;
    channel_survey_control_busy = false;
    xSemaphoreGive(scanner_mutex);
    return error;
}

esp_err_t cerberus_wifi_channel_survey_preempt(void)
{
    return channel_survey_preempt_internal(false);
}

esp_err_t cerberus_wifi_channel_survey_legacy_stop(void)
{
    return channel_survey_preempt_internal(true);
}

static esp_err_t channel_survey_start_internal(uint8_t band, uint64_t session)
{
    if (band != CERBERUS_WIFI_CHANNEL_SURVEY_BAND_24 &&
        band != CERBERUS_WIFI_CHANNEL_SURVEY_BAND_5) return ESP_ERR_INVALID_ARG;
    if (!scanner_mutex || !channel_survey_task_handle) return ESP_ERR_INVALID_STATE;
    if (cerberus_c5_test_running() || another_radio_operation_is_active())
        return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    if (channel_survey_control_busy || (channel_survey_session_id && channel_survey_held_locked()) ||
        (session && (channel_survey_session_epoch == UINT64_MAX || session != channel_survey_session_epoch + 1U))) {
        xSemaphoreGive(scanner_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    int64_t now = esp_timer_get_time();
    if (session && (now < 0 || now > INT64_MAX - (int64_t)CERBERUS_WIFI_CHANNEL_SURVEY_SESSION_LEASE_MS * 1000)) {
        xSemaphoreGive(scanner_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (!scanner_ready || (atomic_load_explicit(&probe_sniff_radio_reserved,memory_order_acquire)||atomic_load_explicit(&packet_count_radio_reserved,memory_order_acquire)) || station_reserved || atomic_load_explicit(&mac_radio_reserved,memory_order_acquire) || atomic_load_explicit(&csi_radio_reserved,memory_order_acquire) ||
        scan_status.state == CERBERUS_WIFI_SCAN_RUNNING ||
        defense_status.state == CERBERUS_WIFI_DEFENSE_RUNNING ||
        wifi_connection_is_active_locked() ||
        atomic_load_explicit(&channel_survey_radio_reserved, memory_order_acquire)) {
        xSemaphoreGive(scanner_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (channel_survey_status.state == CERBERUS_WIFI_CHANNEL_SURVEY_RUNNING) {
        bool same = !session && channel_survey_status.band == band;
        xSemaphoreGive(scanner_mutex);
        return same ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    const uint8_t *channels = band == CERBERUS_WIFI_CHANNEL_SURVEY_BAND_24
        ? channel_survey_24 : channel_survey_5;
    uint8_t count = band == CERBERUS_WIFI_CHANNEL_SURVEY_BAND_24
        ? sizeof(channel_survey_24) : sizeof(channel_survey_5);
    memcpy(channel_survey_channels, channels, count);
    memset(channel_survey_results, 0, sizeof(channel_survey_results));
    uint8_t generation = (uint8_t)(channel_survey_status.generation + 1U);
    if (generation == 0U) generation = 1U;
    for (uint8_t index = 0; index < count; ++index) {
        channel_survey_results[index].generation = generation;
        channel_survey_results[index].index = index;
        channel_survey_results[index].channel = channels[index];
        channel_survey_results[index].average_rssi = -127;
        channel_survey_results[index].strongest_rssi = -127;
        channel_survey_results[index].dwell_ms = CERBERUS_WIFI_CHANNEL_SURVEY_DWELL_MS;
    }
    channel_survey_status = (cerberus_wifi_channel_survey_status_payload_t){
        .state = CERBERUS_WIFI_CHANNEL_SURVEY_RUNNING,
        .generation = generation,
        .band = band,
        .result_count = count,
        .dwell_ms = CERBERUS_WIFI_CHANNEL_SURVEY_DWELL_MS,
    };
    channel_survey_started_us = now;
    channel_survey_session_id = session;
    if (session) channel_survey_session_epoch = session;
    /* Legacy replacement also retires all previous scoped read authority. */
    channel_survey_pin = 0U;
    if (session) {
        channel_survey_lease_clock_us = now;
        channel_survey_lease_deadline_us = now + (int64_t)CERBERUS_WIFI_CHANNEL_SURVEY_SESSION_LEASE_MS * 1000;
    }
    atomic_store_explicit(&channel_survey_cancelled, false, memory_order_release);
    xSemaphoreGive(scanner_mutex);
    xTaskNotifyGive(channel_survey_task_handle);
    return ESP_OK;
}

esp_err_t cerberus_wifi_channel_survey_start(uint8_t band)
{
    return channel_survey_start_internal(band, 0U);
}

void cerberus_wifi_channel_survey_get_status(
    cerberus_wifi_channel_survey_status_payload_t *status)
{
    if (!status) return;
    memset(status, 0, sizeof(*status));
    if (!scanner_mutex) {
        status->state = CERBERUS_WIFI_CHANNEL_SURVEY_ERROR;
        status->error_code = ESP_ERR_INVALID_STATE;
        return;
    }
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    *status = channel_survey_status;
    if (status->state == CERBERUS_WIFI_CHANNEL_SURVEY_RUNNING)
        status->elapsed_ms = channel_survey_elapsed_locked();
    xSemaphoreGive(scanner_mutex);
}

esp_err_t cerberus_wifi_channel_survey_get_result(uint8_t generation, uint8_t index,
    cerberus_wifi_channel_survey_result_payload_t *result)
{
    if (!result || !scanner_mutex) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    if (generation != channel_survey_status.generation ||
        index >= channel_survey_status.result_count) {
        xSemaphoreGive(scanner_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    *result = channel_survey_results[index];
    xSemaphoreGive(scanner_mutex);
    return ESP_OK;
}

/* All response snapshots are built under the same lock as their authority. */
static void channel_survey_session_reply_locked(uint64_t requested, bool info,
    esp_err_t error, cerberus_wifi_channel_survey_session_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->session_id = info ? channel_survey_session_epoch : requested;
    bool bound = channel_survey_session_id && channel_survey_session_id == channel_survey_session_epoch &&
        (info || requested == channel_survey_session_id);
    if (info || bound) {
        out->state = channel_survey_status.state;
        out->band = channel_survey_status.band;
        out->generation = channel_survey_status.generation;
        out->result_count = channel_survey_status.result_count;
        out->completed_count = channel_survey_status.completed_count;
        out->total_sweeps = channel_survey_status.total_sweeps;
        out->dwell_ms = channel_survey_status.dwell_ms;
        if (bound) out->flags |= CERBERUS_WIFI_CHANNEL_SURVEY_SESSION_BOUND;
        if (channel_survey_held_locked()) out->flags |= CERBERUS_WIFI_CHANNEL_SURVEY_SESSION_RADIO_HELD;
        if (bound && atomic_load_explicit(&channel_survey_cancelled, memory_order_acquire))
            out->flags |= CERBERUS_WIFI_CHANNEL_SURVEY_SESSION_CANCELLED;
        out->error_code = error == ESP_OK ? channel_survey_status.error_code : error;
    } else out->error_code = error;
}

void cerberus_wifi_channel_survey_session_info(cerberus_wifi_channel_survey_session_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!scanner_mutex) { out->error_code = ESP_ERR_INVALID_STATE; return; }
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    channel_survey_session_reply_locked(0U, true, ESP_OK, out);
    xSemaphoreGive(scanner_mutex);
}

esp_err_t cerberus_wifi_channel_survey_session_start(
    const cerberus_wifi_channel_survey_session_start_t *request,
    cerberus_wifi_channel_survey_session_status_t *out)
{
    if (!request || !out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out)); out->session_id = request->session_id;
    if (!scanner_mutex) { out->error_code = ESP_ERR_INVALID_STATE; return ESP_ERR_INVALID_STATE; }
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    bool replay = request->session_id && request->session_id == channel_survey_session_id;
    if (replay) {
        esp_err_t error = request->band == channel_survey_status.band ? ESP_OK : ESP_ERR_INVALID_ARG;
        if (error == ESP_OK) (void)channel_survey_expire_locked(esp_timer_get_time());
        channel_survey_session_reply_locked(request->session_id, false, error, out);
        xSemaphoreGive(scanner_mutex);
        return error;
    }
    xSemaphoreGive(scanner_mutex);
    esp_err_t error = request->session_id ? channel_survey_start_internal(request->band, request->session_id) : ESP_ERR_INVALID_ARG;
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    channel_survey_session_reply_locked(request->session_id, false, error, out);
    xSemaphoreGive(scanner_mutex);
    return error;
}

static esp_err_t channel_survey_session_control(uint64_t session, bool stop,
    cerberus_wifi_channel_survey_session_status_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out)); out->session_id = session;
    if (!scanner_mutex) { out->error_code = ESP_ERR_INVALID_STATE; return ESP_ERR_INVALID_STATE; }
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    esp_err_t error = session && session == channel_survey_session_id ? ESP_OK : ESP_ERR_NOT_FOUND;
    if (error == ESP_OK) {
        int64_t now = esp_timer_get_time();
        (void)channel_survey_expire_locked(now);
        if (stop) atomic_store_explicit(&channel_survey_cancelled, true, memory_order_release);
        else if (channel_survey_status.state == CERBERUS_WIFI_CHANNEL_SURVEY_RUNNING &&
            !atomic_load_explicit(&channel_survey_cancelled, memory_order_acquire)) {
            if (now > INT64_MAX - (int64_t)CERBERUS_WIFI_CHANNEL_SURVEY_SESSION_LEASE_MS * 1000)
                atomic_store_explicit(&channel_survey_cancelled, true, memory_order_release);
            else channel_survey_lease_deadline_us = now + (int64_t)CERBERUS_WIFI_CHANNEL_SURVEY_SESSION_LEASE_MS * 1000;
        }
    }
    channel_survey_session_reply_locked(session, false, error, out);
    xSemaphoreGive(scanner_mutex);
    return error;
}

esp_err_t cerberus_wifi_channel_survey_session_stop(uint64_t session,
    cerberus_wifi_channel_survey_session_status_t *out)
{
    /* Nonblocking exact cancellation. The existing worker alone drains SDK
     * cleanup; subsequent scoped status reports the retained reservation. */
    return channel_survey_session_control(session, true, out);
}

esp_err_t cerberus_wifi_channel_survey_session_status(uint64_t session,
    cerberus_wifi_channel_survey_session_status_t *out)
{
    return channel_survey_session_control(session, false, out);
}

esp_err_t cerberus_wifi_channel_survey_session_read(
    const cerberus_wifi_channel_survey_session_read_t *request,
    cerberus_wifi_channel_survey_session_chunk_t *out)
{
    if (!request || !out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out)); out->session_id = request->session_id; out->part = request->part;
    esp_err_t error = ESP_OK;
    if (!scanner_mutex) error = ESP_ERR_INVALID_STATE;
    else {
        xSemaphoreTake(scanner_mutex, portMAX_DELAY);
        if (!request->session_id || request->session_id != channel_survey_session_id) error = ESP_ERR_NOT_FOUND;
        else if (request->part >= CERBERUS_WIFI_CHANNEL_SURVEY_READ_PARTS ||
            request->kind > CERBERUS_WIFI_CHANNEL_SURVEY_READ_RESULT ||
            (request->kind == CERBERUS_WIFI_CHANNEL_SURVEY_READ_STATUS && request->index != 0U) ||
            (request->kind == CERBERUS_WIFI_CHANNEL_SURVEY_READ_RESULT && request->index >= channel_survey_status.result_count))
            error = ESP_ERR_INVALID_ARG;
        else if (request->part == 0U) {
            if (request->pin) error = ESP_ERR_INVALID_ARG;
            else if (channel_survey_pin_counter == UINT32_MAX) error = ESP_ERR_INVALID_STATE;
            else {
                if (request->kind == CERBERUS_WIFI_CHANNEL_SURVEY_READ_STATUS) {
                    cerberus_wifi_channel_survey_status_payload_t frozen = channel_survey_status;
                    if (frozen.state == CERBERUS_WIFI_CHANNEL_SURVEY_RUNNING) frozen.elapsed_ms = channel_survey_elapsed_locked();
                    memcpy(channel_survey_pin_bytes, &frozen, sizeof(frozen));
                } else memcpy(channel_survey_pin_bytes, &channel_survey_results[request->index], sizeof(channel_survey_pin_bytes));
                channel_survey_pin = ++channel_survey_pin_counter;
                channel_survey_pin_session = request->session_id;
                channel_survey_pin_kind = request->kind; channel_survey_pin_index = request->index;
            }
        } else if (!request->pin || request->pin != channel_survey_pin || request->session_id != channel_survey_pin_session ||
            request->kind != channel_survey_pin_kind || request->index != channel_survey_pin_index) error = ESP_ERR_NOT_FOUND;
        if (error == ESP_OK) {
            size_t offset = (size_t)request->part * CERBERUS_WIFI_CHANNEL_SURVEY_READ_PART_BYTES;
            size_t count = sizeof(channel_survey_pin_bytes) - offset;
            if (count > sizeof(out->data)) count = sizeof(out->data);
            out->pin = channel_survey_pin;
            memcpy(out->data, channel_survey_pin_bytes + offset, count);
        }
        xSemaphoreGive(scanner_mutex);
    }
    if (error != ESP_OK) {
        uint32_t value = (uint32_t)error;
        for (unsigned i=0; i<4; ++i) out->data[i] = (uint8_t)(value >> (8U*i));
    }
    return error;
}
