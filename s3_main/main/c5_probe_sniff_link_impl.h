/* Included after the bounded scoped-survey frame helper in c5_spi_link.c.
 * No mutation retries; callers resolve uncertain delivery by exact session. */
static esp_err_t probe_sniff_gate_locked(uint32_t deadline, bool refresh)
{
    if (refresh) {
        cerberus_c5_status_t device = {0};
        esp_err_t error = get_status_v2_passive_locked(++sequence, deadline, &device);
        peer_firmware_version = error == ESP_OK ? device.firmware_version : 0U;
        if (error != ESP_OK) return error;
    }
    return peer_firmware_version >= CERBERUS_PROBE_SNIFF_MIN_FIRMWARE ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

esp_err_t c5_spi_link_probe_sniff_control(uint8_t command,
    const cerberus_probe_sniff_start_t *request, cerberus_probe_sniff_control_t *reply)
{
    if (!reply) return ESP_ERR_INVALID_ARG;
    memset(reply, 0, sizeof(*reply));
    bool info = command == CERBERUS_PROBE_SNIFF_COMMAND_INFO;
    bool start = command == CERBERUS_PROBE_SNIFF_COMMAND_START;
    bool release = command == CERBERUS_PROBE_SNIFF_COMMAND_RELEASE;
    if ((!info && (!request || !request->session_id)) ||
        (command != CERBERUS_PROBE_SNIFF_COMMAND_INFO && !start && !release &&
            command != CERBERUS_PROBE_SNIFF_COMMAND_STATUS && command != CERBERUS_PROBE_SNIFF_COMMAND_STOP) ||
        (start && (request->mode > CERBERUS_PROBE_SNIFF_HOP_BOTH ||
            (request->mode == CERBERUS_PROBE_SNIFF_FIXED ?
                !cerberus_probe_sniff_channel_valid(request->channel) : request->channel != 0U))))
        return ESP_ERR_INVALID_ARG;
    if (!link_mutex) return ESP_ERR_INVALID_STATE;
    int64_t began = esp_timer_get_time();
    if (began < 0) return ESP_ERR_INVALID_STATE;
    uint32_t deadline = (uint32_t)((uint64_t)began / 1000U) + 20U;
    if (take_link_mutex(pdMS_TO_TICKS(20)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t error = probe_sniff_gate_locked(deadline, info || start);
    if (error == ESP_OK) error = survey_session_frame_locked((cerberus_spi_command_t)command,
        info ? NULL : request, info ? 0U : start ? sizeof(*request) : sizeof(request->session_id),
        reply, sizeof(*reply), deadline);
    give_link_mutex();
    int64_t ended = esp_timer_get_time();
    if (error == ESP_OK && (ended < began || (uint64_t)(ended - began) >= 20000U)) error = ESP_ERR_TIMEOUT;
    if (error == ESP_OK && !cerberus_probe_sniff_control_valid(reply, info, release,
        info ? 0U : request->session_id)) error = ESP_ERR_INVALID_RESPONSE;
    if (error != ESP_OK) memset(reply, 0, sizeof(*reply));
    return error;
}

esp_err_t c5_spi_link_probe_sniff_read(const cerberus_probe_sniff_read_t *request,
    cerberus_probe_sniff_chunk_t *reply)
{
    if (!reply) return ESP_ERR_INVALID_ARG;
    memset(reply, 0, sizeof(*reply));
    if (!request || !request->session_id || request->kind > CERBERUS_PROBE_SNIFF_READ_ROW ||
        (request->kind == CERBERUS_PROBE_SNIFF_READ_SUMMARY ? request->index != 0U :
            request->index >= CERBERUS_PROBE_SNIFF_CAPACITY) ||
        request->part >= (request->kind == CERBERUS_PROBE_SNIFF_READ_ROW ?
            CERBERUS_PROBE_SNIFF_ROW_PARTS : CERBERUS_PROBE_SNIFF_SUMMARY_PARTS) ||
        (!!request->part != !!request->pin)) return ESP_ERR_INVALID_ARG;
    if (!link_mutex) return ESP_ERR_INVALID_STATE;
    int64_t began = esp_timer_get_time();
    if (began < 0) return ESP_ERR_INVALID_STATE;
    uint32_t deadline = (uint32_t)((uint64_t)began / 1000U) + 20U;
    if (take_link_mutex(pdMS_TO_TICKS(20)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t error = probe_sniff_gate_locked(deadline, false);
    if (error == ESP_OK) error = survey_session_frame_locked(
        (cerberus_spi_command_t)CERBERUS_PROBE_SNIFF_COMMAND_READ,
        request, sizeof(*request), reply, sizeof(*reply), deadline);
    give_link_mutex();
    int64_t ended = esp_timer_get_time();
    if (error == ESP_OK && (ended < began || (uint64_t)(ended - began) >= 20000U)) error = ESP_ERR_TIMEOUT;
    if (error == ESP_OK && (reply->session_id != request->session_id || reply->part != request->part))
        error = ESP_ERR_INVALID_RESPONSE;
    if (error == ESP_OK && !reply->pin) {
        int32_t remote_error;
        memcpy(&remote_error, reply->data, sizeof(remote_error));
        error = remote_error ? remote_error : ESP_ERR_INVALID_RESPONSE;
        for (uint8_t i = 4U; i < CERBERUS_PROBE_SNIFF_CHUNK_BYTES; ++i)
            if (reply->data[i]) error = ESP_ERR_INVALID_RESPONSE;
    } else if (error == ESP_OK) {
        if (request->part && reply->pin != request->pin) error = ESP_ERR_INVALID_RESPONSE;
        size_t bytes = request->kind == CERBERUS_PROBE_SNIFF_READ_ROW ?
            CERBERUS_PROBE_SNIFF_PROJECTION_BYTES : CERBERUS_PROBE_SNIFF_SUMMARY_BYTES;
        size_t remaining = bytes - request->part * CERBERUS_PROBE_SNIFF_CHUNK_BYTES;
        for (size_t i = remaining; i < CERBERUS_PROBE_SNIFF_CHUNK_BYTES; ++i)
            if (reply->data[i]) error = ESP_ERR_INVALID_RESPONSE;
    }
    if (error != ESP_OK) memset(reply, 0, sizeof(*reply));
    return error;
}
