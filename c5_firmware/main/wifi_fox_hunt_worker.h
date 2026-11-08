/* Uses the existing receive worker and restore path; no task or packet queue. */
static esp_err_t wifi_fox_hunt_prepare_channels(void)
{
    wifi_country_t country = {0};
    esp_err_t error = esp_wifi_get_country(&country);
    if (error != ESP_OK) return error;
    if (!country.schan || country.schan > 14U || !country.nchan ||
        (unsigned)country.schan + country.nchan > 15U) return ESP_ERR_INVALID_STATE;
    wifi_fox_hunt_channel_count = 0U;
    if (wifi_fox_hunt_request.search_band & CERBERUS_WIFI_FOX_HUNT_BAND_2G)
        for (unsigned ch = country.schan; ch < (unsigned)country.schan + country.nchan; ++ch)
            wifi_fox_hunt_channels[wifi_fox_hunt_channel_count++] = (uint8_t)ch;
    if (wifi_fox_hunt_request.search_band & CERBERUS_WIFI_FOX_HUNT_BAND_5G)
        for (unsigned i = 0; i < sizeof(cerberus_wifi_fox_hunt_5g_channels); ++i) {
            if (country.policy == WIFI_COUNTRY_POLICY_MANUAL && country.wifi_5g_channel_mask &&
                !(country.wifi_5g_channel_mask & (UINT32_C(1) << (i + 1U)))) continue;
            wifi_fox_hunt_channels[wifi_fox_hunt_channel_count++] = cerberus_wifi_fox_hunt_5g_channels[i];
        }
    for (unsigned i = 0; i < wifi_fox_hunt_channel_count; ++i)
        if (wifi_fox_hunt_channels[i] == wifi_fox_hunt_request.initial_channel) return ESP_OK;
    return ESP_ERR_INVALID_ARG;
}

static esp_err_t wifi_fox_hunt_arm(uint8_t channel, int64_t *armed)
{
    portENTER_CRITICAL(&packet_monitor_mux);
    int64_t now = esp_timer_get_time();
    uint32_t token = atomic_load_explicit(&wifi_fox_hunt_admission_token, memory_order_relaxed);
    esp_err_t error = ESP_OK;
    if (now < 0 || now < wifi_fox_hunt_clock_us || token == UINT32_MAX) error = ESP_ERR_INVALID_STATE;
    else if (!atomic_load_explicit(&packet_monitor_cancelled, memory_order_acquire)) {
        wifi_fox_hunt_clock_us = wifi_fox_hunt_armed_us = now;
        packet_monitor_receive_channel = channel;
        atomic_store_explicit(&wifi_fox_hunt_admission_token, token + 1U, memory_order_release);
        packet_monitor_receiving = true;
        *armed = now;
    }
    portEXIT_CRITICAL(&packet_monitor_mux);
    return error;
}

static esp_err_t wifi_fox_hunt_run(void)
{
    int64_t tuned = 0;
    esp_err_t error = wifi_fox_hunt_arm(wifi_fox_hunt_request.initial_channel, &tuned);
    if (error != ESP_OK) return error;
    const int64_t initial = tuned;
    unsigned next = 0U, attempted = 0U, successful = 0U;
    for (unsigned i = 0; i < wifi_fox_hunt_channel_count; ++i)
        if (wifi_fox_hunt_channels[i] == wifi_fox_hunt_request.initial_channel)
            next = (i + 1U) % wifi_fox_hunt_channel_count;
    while (!atomic_load_explicit(&packet_monitor_cancelled, memory_order_acquire)) {
        /* Relative delay prevents a slow tune or SDK callback causing catch-up
         * hops. Failed channels also consume a worker interval. */
        vTaskDelay(pdMS_TO_TICKS(50U));
        packet_monitor_check_expiry();
        portENTER_CRITICAL(&packet_monitor_mux);
        int64_t now = esp_timer_get_time();
        bool valid = now >= wifi_fox_hunt_clock_us && now >= tuned;
        if (valid) wifi_fox_hunt_clock_us = now;
        bool cancelled = atomic_load_explicit(&packet_monitor_cancelled, memory_order_acquire);
        int64_t last = wifi_fox_hunt_observed_us >= 0 ? wifi_fox_hunt_observed_us : initial;
        bool lost = valid && now >= last && now - last >= (int64_t)CERBERUS_WIFI_FOX_HUNT_LOST_MS * 1000;
        bool searching = wifi_fox_hunt_searching;
        bool receiving = packet_monitor_receiving;
        if (lost && !cancelled) wifi_fox_hunt_searching = true;
        bool hop = lost && !cancelled && (!searching || !receiving ||
            now - tuned >= (int64_t)CERBERUS_WIFI_FOX_HUNT_DWELL_MS * 1000);
        if (hop) packet_monitor_receiving = false;
        portEXIT_CRITICAL(&packet_monitor_mux);
        if (!valid) return ESP_ERR_INVALID_STATE;
        if (cancelled) break;
        if (!lost) attempted = successful = 0U;
        if (!hop) continue;
        if (attempted == wifi_fox_hunt_channel_count) {
            portENTER_CRITICAL(&packet_monitor_mux);
            if (wifi_fox_hunt_sweeps != UINT16_MAX) ++wifi_fox_hunt_sweeps;
            portEXIT_CRITICAL(&packet_monitor_mux);
            attempted = successful = 0U;
        }
        error = esp_wifi_set_promiscuous(false);
        if (error != ESP_OK) return error;
        packet_monitor_disable_pending = false;
        uint8_t channel = wifi_fox_hunt_channels[next];
        next = (next + 1U) % wifi_fox_hunt_channel_count;
        error = esp_wifi_set_band(channel <= 14U ? WIFI_BAND_2G : WIFI_BAND_5G);
        if (error == ESP_OK) error = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
        ++attempted;
        if (error != ESP_OK) {
            portENTER_CRITICAL(&packet_monitor_mux);
            wifi_fox_hunt_skipped = true;
            wifi_fox_hunt_tune_error = error;
            portEXIT_CRITICAL(&packet_monitor_mux);
        } else {
            if (atomic_load_explicit(&packet_monitor_cancelled, memory_order_acquire)) break;
            packet_monitor_disable_pending = true;
            error = esp_wifi_set_promiscuous(true);
            if (error != ESP_OK) return error;
            error = wifi_fox_hunt_arm(channel, &tuned);
            if (error != ESP_OK) return error;
            ++successful;
        }
        if (attempted == wifi_fox_hunt_channel_count && !successful)
            return error == ESP_OK ? ESP_ERR_INVALID_STATE : error;
    }
    return ESP_OK;
}
