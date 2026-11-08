static void wifi_fox_hunt_reply_locked(uint64_t requested, bool info, esp_err_t error,
    cerberus_wifi_fox_hunt_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->session_id = info ? packet_monitor_session_epoch : requested;
    bool bound = wifi_fox_hunt_mode && packet_monitor_session_id &&
        out->session_id == packet_monitor_session_id;
    if (!bound) {
        if (info && packet_monitor_held_locked()) out->flags = CERBERUS_WIFI_FOX_HUNT_HELD;
        out->error_code = info ? (packet_monitor_held_locked() ? ESP_ERR_INVALID_STATE : ESP_OK) : error;
        return;
    }
    out->flags = CERBERUS_WIFI_FOX_HUNT_BOUND;
    out->state = packet_monitor_status.state;
    if (packet_monitor_held_locked()) out->flags |= CERBERUS_WIFI_FOX_HUNT_HELD;
    bool cancelled = atomic_load_explicit(&packet_monitor_cancelled, memory_order_acquire);
    if (cancelled) out->flags |= CERBERUS_WIFI_FOX_HUNT_CANCELLED;
    portENTER_CRITICAL(&packet_monitor_mux);
    out->latest_sequence = wifi_fox_hunt_latest.sequence;
    out->last_target_channel = wifi_fox_hunt_latest.channel;
    out->sweeps = wifi_fox_hunt_sweeps;
    if (out->sweeps == UINT16_MAX) out->flags |= CERBERUS_WIFI_FOX_HUNT_SWEEPS_SATURATED;
    if (wifi_fox_hunt_skipped) out->flags |= CERBERUS_WIFI_FOX_HUNT_CHANNELS_SKIPPED;
    if (!cancelled && out->state == CERBERUS_WIFI_PACKET_MONITOR_RUNNING) {
        if (packet_monitor_receiving) {
            out->flags |= CERBERUS_WIFI_FOX_HUNT_RECEIVING;
            out->current_channel = packet_monitor_receive_channel;
        }
        if (wifi_fox_hunt_searching) out->flags |= CERBERUS_WIFI_FOX_HUNT_SEARCHING;
    }
    out->error_code = error != ESP_OK ? error : packet_monitor_status.error_code != ESP_OK ?
        packet_monitor_status.error_code : wifi_fox_hunt_tune_error;
    portEXIT_CRITICAL(&packet_monitor_mux);
}

void cerberus_wifi_fox_hunt_info(cerberus_wifi_fox_hunt_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!scanner_mutex) {out->error_code = ESP_ERR_INVALID_STATE; return;}
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    packet_monitor_session_init_locked();
    packet_monitor_expire_locked(esp_timer_get_time());
    wifi_fox_hunt_reply_locked(0U, true, ESP_OK, out);
    xSemaphoreGive(scanner_mutex);
}

esp_err_t cerberus_wifi_fox_hunt_start(const cerberus_wifi_fox_hunt_start_t *request,
    cerberus_wifi_fox_hunt_status_t *out)
{
    if (!request || !out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out)); out->session_id = request->session_id;
    if (!scanner_mutex) {out->error_code = ESP_ERR_INVALID_STATE; return ESP_ERR_INVALID_STATE;}
    esp_err_t error = cerberus_wifi_fox_hunt_start_valid(request) ?
        packet_monitor_start_internal(request->initial_channel, request->session_id, false, false, request) :
        ESP_ERR_INVALID_ARG;
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    wifi_fox_hunt_reply_locked(request->session_id, false, error, out);
    xSemaphoreGive(scanner_mutex);
    return error;
}

static esp_err_t wifi_fox_hunt_control(uint64_t session, bool stop, cerberus_wifi_fox_hunt_status_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out)); out->session_id = session;
    if (!scanner_mutex) {out->error_code = ESP_ERR_INVALID_STATE; return ESP_ERR_INVALID_STATE;}
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    esp_err_t error = wifi_fox_hunt_mode && session && session == packet_monitor_session_id ?
        ESP_OK : ESP_ERR_NOT_FOUND;
    bool notify = false;
    if (error == ESP_OK) {
        int64_t now = esp_timer_get_time();
        packet_monitor_expire_locked(now);
        if (stop) {
            atomic_store_explicit(&packet_monitor_cancelled, true, memory_order_release);
            packet_monitor_record_close_admission();
            if (packet_monitor_status.state == CERBERUS_WIFI_PACKET_MONITOR_ERROR && packet_monitor_held_locked())
                packet_monitor_cleanup_retry = true;
            notify = packet_monitor_held_locked();
        } else if (packet_monitor_status.state == CERBERUS_WIFI_PACKET_MONITOR_RUNNING &&
            !atomic_load_explicit(&packet_monitor_cancelled, memory_order_acquire)) {
            if (now > INT64_MAX - (int64_t)CERBERUS_WIFI_PACKET_MONITOR_LEASE_MS * 1000) {
                atomic_store_explicit(&channel_analyzer_clock_invalid, true, memory_order_release);
                atomic_store_explicit(&packet_monitor_cancelled, true, memory_order_release);
            } else packet_monitor_lease_deadline_us = now + (int64_t)CERBERUS_WIFI_PACKET_MONITOR_LEASE_MS * 1000;
        }
    }
    wifi_fox_hunt_reply_locked(session, false, error, out);
    xSemaphoreGive(scanner_mutex);
    if (notify) xTaskNotifyGive(scanner_task_handle);
    return error;
}

esp_err_t cerberus_wifi_fox_hunt_status(uint64_t session, cerberus_wifi_fox_hunt_status_t *out)
{return wifi_fox_hunt_control(session, false, out);}
esp_err_t cerberus_wifi_fox_hunt_stop(uint64_t session, cerberus_wifi_fox_hunt_status_t *out)
{return wifi_fox_hunt_control(session, true, out);}

esp_err_t cerberus_wifi_fox_hunt_signal(uint64_t session, cerberus_wifi_fox_hunt_signal_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!scanner_mutex) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    bool valid = wifi_fox_hunt_mode && session && session == packet_monitor_session_id;
    if (valid) {
        int64_t now = esp_timer_get_time();
        packet_monitor_expire_locked(now);
        portENTER_CRITICAL(&packet_monitor_mux);
        *out = wifi_fox_hunt_latest;
        out->age_ms = cerberus_wifi_fox_hunt_age(now, wifi_fox_hunt_observed_us);
        portEXIT_CRITICAL(&packet_monitor_mux);
    }
    xSemaphoreGive(scanner_mutex);
    return valid ? ESP_OK : ESP_ERR_NOT_FOUND;
}
