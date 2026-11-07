/* Private to c5_spi_link.c. Every operation has one 20 ms budget and no
 * automatic replay. The caller retains its session after an ambiguous reply. */
static bool packet_monitor_session_control_valid(
    const cerberus_wifi_packet_monitor_session_status_t *r, bool info, uint64_t session)
{
    if (!r->session_id || (!info && r->session_id != session) ||
        (r->flags & ~(CERBERUS_WIFI_PACKET_MONITOR_BOUND |
            CERBERUS_WIFI_PACKET_MONITOR_HELD | CERBERUS_WIFI_PACKET_MONITOR_CANCELLED))) return false;
    if (!info && !(r->flags & CERBERUS_WIFI_PACKET_MONITOR_BOUND))
        return r->error_code != ESP_OK && !r->elapsed_ms && !r->latest_sequence &&
            !r->state && !r->channel && !r->stored_count && !r->flags;
    if (r->state > CERBERUS_WIFI_PACKET_MONITOR_ERROR ||
        r->stored_count > CERBERUS_WIFI_PACKET_MONITOR_SAMPLES ||
        (r->channel && !cerberus_wifi_packet_monitor_channel_valid(r->channel)) ||
        (!r->channel && (r->state || r->stored_count || r->latest_sequence || r->elapsed_ms || r->flags)) ||
        ((r->flags & CERBERUS_WIFI_PACKET_MONITOR_CANCELLED) && !(r->flags & CERBERUS_WIFI_PACKET_MONITOR_BOUND)) ||
        (r->state == CERBERUS_WIFI_PACKET_MONITOR_RUNNING && !(r->flags & CERBERUS_WIFI_PACKET_MONITOR_HELD)) ||
        (r->state == CERBERUS_WIFI_PACKET_MONITOR_IDLE && (r->flags & CERBERUS_WIFI_PACKET_MONITOR_HELD))) return false;
    return true;
}

static esp_err_t packet_monitor_session_gate_locked(uint32_t deadline, bool refresh)
{
    if (refresh || !peer_firmware_version) {
        cerberus_c5_status_t device = {0};
        esp_err_t error = get_status_v2_passive_locked(++sequence, deadline, &device);
        peer_firmware_version = error == ESP_OK ? device.firmware_version : 0U;
        if (error != ESP_OK) return error;
    }
    return peer_firmware_version >= CERBERUS_WIFI_PACKET_MONITOR_SESSION_MIN_FIRMWARE ?
        ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t packet_monitor_session_exchange(cerberus_spi_command_t command,
    uint64_t session, uint8_t channel, cerberus_wifi_packet_monitor_session_status_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    bool info = command == CERBERUS_SPI_COMMAND_WIFI_PACKET_MONITOR_SESSION_INFO;
    bool start = command == CERBERUS_SPI_COMMAND_WIFI_PACKET_MONITOR_SESSION_START;
    if ((!info && !session) || (start && !cerberus_wifi_packet_monitor_channel_valid(channel)))
        return ESP_ERR_INVALID_ARG;
    if (!link_mutex) return ESP_ERR_INVALID_STATE;
    int64_t begun = esp_timer_get_time();
    if (begun < 0) return ESP_ERR_INVALID_STATE;
    uint32_t deadline = (uint32_t)((uint64_t)begun / 1000U) + 20U;
    if (take_link_mutex(pdMS_TO_TICKS(20)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t error = packet_monitor_session_gate_locked(deadline, info || start);
    cerberus_wifi_packet_monitor_session_start_t query = {session, channel};
    if (error == ESP_OK) error = survey_session_frame_locked(command,
        info ? NULL : &query, info ? 0U : start ? sizeof(query) : sizeof(session),
        out, sizeof(*out), deadline);
    give_link_mutex();
    int64_t ended = esp_timer_get_time();
    if (error == ESP_OK && (ended < begun || (uint64_t)(ended - begun) >= 20000U)) error = ESP_ERR_TIMEOUT;
    if (error == ESP_OK && (!packet_monitor_session_control_valid(out, info, session) ||
        (start && (out->flags & CERBERUS_WIFI_PACKET_MONITOR_BOUND) && out->channel != channel)))
        error = ESP_ERR_INVALID_RESPONSE;
    if (error != ESP_OK) memset(out, 0, sizeof(*out));
    return error;
}

esp_err_t c5_spi_link_wifi_packet_monitor_session_info(cerberus_wifi_packet_monitor_session_status_t *out)
{ return packet_monitor_session_exchange(CERBERUS_SPI_COMMAND_WIFI_PACKET_MONITOR_SESSION_INFO, 0U, 0U, out); }
esp_err_t c5_spi_link_wifi_packet_monitor_session_start(uint64_t session, uint8_t channel,
    cerberus_wifi_packet_monitor_session_status_t *out)
{ return packet_monitor_session_exchange(CERBERUS_SPI_COMMAND_WIFI_PACKET_MONITOR_SESSION_START, session, channel, out); }
esp_err_t c5_spi_link_wifi_packet_monitor_session_status(uint64_t session,
    cerberus_wifi_packet_monitor_session_status_t *out)
{ return packet_monitor_session_exchange(CERBERUS_SPI_COMMAND_WIFI_PACKET_MONITOR_SESSION_STATUS, session, 0U, out); }
esp_err_t c5_spi_link_wifi_packet_monitor_session_stop(uint64_t session,
    cerberus_wifi_packet_monitor_session_status_t *out)
{ return packet_monitor_session_exchange(CERBERUS_SPI_COMMAND_WIFI_PACKET_MONITOR_SESSION_STOP, session, 0U, out); }

esp_err_t c5_spi_link_wifi_packet_monitor_session_sample(uint64_t session, uint16_t sample_sequence,
    cerberus_wifi_packet_monitor_session_sample_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!session) return ESP_ERR_INVALID_ARG;
    if (!link_mutex) return ESP_ERR_INVALID_STATE;
    int64_t begun = esp_timer_get_time();
    if (begun < 0) return ESP_ERR_INVALID_STATE;
    uint32_t deadline = (uint32_t)((uint64_t)begun / 1000U) + 20U;
    if (take_link_mutex(pdMS_TO_TICKS(20)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t error = packet_monitor_session_gate_locked(deadline, false);
    cerberus_wifi_packet_monitor_session_read_t query = {session, sample_sequence};
    if (error == ESP_OK) error = survey_session_frame_locked(
        CERBERUS_SPI_COMMAND_WIFI_PACKET_MONITOR_SESSION_SAMPLE,
        &query, sizeof(query), out, sizeof(*out), deadline);
    give_link_mutex();
    int64_t ended = esp_timer_get_time();
    if (error == ESP_OK && (ended < begun || (uint64_t)(ended - begun) >= 20000U)) error = ESP_ERR_TIMEOUT;
    if (error == ESP_OK && (out->session_id != session ||
        out->sample.sequence != sample_sequence || !out->sample.generation ||
        !cerberus_wifi_packet_monitor_channel_valid(out->sample.channel))) error = ESP_ERR_INVALID_RESPONSE;
    if (error != ESP_OK) memset(out, 0, sizeof(*out));
    return error;
}
