/* Included in c5_spi_link.c after survey_session_frame_locked. One bounded
 * passive exchange; uncertain delivery is resolved by the session controller.
 * No driver restart or automatic retry of a mutating command. */
static esp_err_t mesh_request_size(uint8_t command, const void *request,
    size_t length, uint64_t *session)
{
    size_t expected;
    *session = 0U;
    switch (command) {
    case CERBERUS_MESH_OBSERVATION_COMMAND_INFO: expected = 0U; break;
    case CERBERUS_MESH_OBSERVATION_COMMAND_START:
        expected = sizeof(cerberus_mesh_observation_start_t); break;
    case CERBERUS_MESH_OBSERVATION_COMMAND_STATUS:
    case CERBERUS_MESH_OBSERVATION_COMMAND_STOP:
    case CERBERUS_MESH_OBSERVATION_COMMAND_RELEASE:
        expected = sizeof(cerberus_mesh_observation_request_t); break;
    case CERBERUS_MESH_OBSERVATION_COMMAND_PEEK:
        expected = sizeof(cerberus_mesh_observation_peek_t); break;
    case CERBERUS_MESH_OBSERVATION_COMMAND_READ:
        expected = sizeof(cerberus_mesh_observation_read_t); break;
    case CERBERUS_MESH_OBSERVATION_COMMAND_ACK:
        expected = sizeof(cerberus_mesh_observation_ack_t); break;
    default: return ESP_ERR_INVALID_ARG;
    }
    if (length != expected || (expected && !request)) return ESP_ERR_INVALID_ARG;
    if (!expected) return request ? ESP_ERR_INVALID_ARG : ESP_OK;
    memcpy(session, request, sizeof(*session));
    if (!*session) return ESP_ERR_INVALID_ARG;
    if (command == CERBERUS_MESH_OBSERVATION_COMMAND_START) {
        cerberus_mesh_observation_start_t q;
        memcpy(&q, request, sizeof(q));
        if (!q.channel_mask || q.mode > CERBERUS_MESH_OBSERVATION_SWEEP ||
            q.dwell_ms < 100U || q.dwell_ms > 2000U ||
            (q.mode == CERBERUS_MESH_OBSERVATION_FIXED &&
                (q.channel_mask & (q.channel_mask - 1U)))) return ESP_ERR_INVALID_ARG;
    } else if (command == CERBERUS_MESH_OBSERVATION_COMMAND_PEEK) {
        cerberus_mesh_observation_peek_t q;
        memcpy(&q, request, sizeof(q));
        if (q.kind > CERBERUS_MESH_OBSERVATION_SUMMARY) return ESP_ERR_INVALID_ARG;
    } else if (command == CERBERUS_MESH_OBSERVATION_COMMAND_READ) {
        cerberus_mesh_observation_read_t q;
        memcpy(&q, request, sizeof(q));
        bool row = q.kind == CERBERUS_MESH_OBSERVATION_ROW;
        if (q.kind > CERBERUS_MESH_OBSERVATION_SUMMARY || !q.sequence ||
            (row ? q.sequence > CERBERUS_MESH_OBSERVATION_SEQUENCE_MAX :
                q.sequence <= CERBERUS_MESH_OBSERVATION_SUMMARY_BASE) ||
            q.part >= (row ? CERBERUS_MESH_OBSERVATION_RECORD_PARTS :
                CERBERUS_MESH_OBSERVATION_SUMMARY_PARTS)) return ESP_ERR_INVALID_ARG;
    } else if (command == CERBERUS_MESH_OBSERVATION_COMMAND_ACK) {
        cerberus_mesh_observation_ack_t q;
        memcpy(&q, request, sizeof(q));
        if (!q.sequence || q.sequence > CERBERUS_MESH_OBSERVATION_SEQUENCE_MAX)
            return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t c5_spi_link_mesh_exchange(uint8_t command, const void *request,
    size_t request_length, void *reply, size_t reply_length)
{
    if (!reply || reply_length != CERBERUS_SPI_FRAME_PAYLOAD_SIZE) return ESP_ERR_INVALID_ARG;
    /* Preserve request bytes even if a caller deliberately aliases its input
     * and output. Wire request length is strictly bounded before this copy. */
    uint64_t session;
    esp_err_t error = mesh_request_size(command, request, request_length, &session);
    uint8_t query[sizeof(cerberus_mesh_observation_read_t)] = {0};
    if (error == ESP_OK && request_length) memcpy(query, request, request_length);
    memset(reply, 0, reply_length);
    if (error != ESP_OK) return error;
    if (!link_mutex) return ESP_ERR_INVALID_STATE;
    int64_t began = esp_timer_get_time();
    if (began < 0) return ESP_ERR_INVALID_STATE;
    uint32_t deadline = (uint32_t)((uint64_t)began / 1000U) + 20U;
    if (take_link_mutex(pdMS_TO_TICKS(20)) != pdTRUE) return ESP_ERR_TIMEOUT;
    bool refresh = command == CERBERUS_MESH_OBSERVATION_COMMAND_INFO ||
        command == CERBERUS_MESH_OBSERVATION_COMMAND_START;
    if (refresh) {
        cerberus_c5_status_t device = {0};
        error = get_status_v2_passive_locked(++sequence, deadline, &device);
        peer_firmware_version = error == ESP_OK ? device.firmware_version : 0U;
    }
    if (error == ESP_OK && peer_firmware_version < CERBERUS_MESH_OBSERVATION_MIN_FIRMWARE)
        error = ESP_ERR_NOT_SUPPORTED;
    if (error == ESP_OK) error = survey_session_frame_locked((cerberus_spi_command_t)command,
        request_length ? query : NULL, request_length, reply, reply_length, deadline);
    give_link_mutex();
    int64_t ended = esp_timer_get_time();
    if (error == ESP_OK && (ended < began || (uint64_t)(ended - began) >= 20000U))
        error = ESP_ERR_TIMEOUT;
    if (error == ESP_OK && command == CERBERUS_MESH_OBSERVATION_COMMAND_READ) {
        cerberus_mesh_observation_read_t q;
        cerberus_mesh_observation_chunk_t r;
        memcpy(&q, query, sizeof(q)); memcpy(&r, reply, sizeof(r));
        if (r.session_id != session || r.part != q.part) error = ESP_ERR_INVALID_RESPONSE;
        else if (!r.sequence) {
            int32_t remote;
            memcpy(&remote, r.data, sizeof(remote));
            error = remote ? remote : ESP_ERR_INVALID_RESPONSE;
            for (size_t i = 4U; i < sizeof(r.data); ++i)
                if (r.data[i]) error = ESP_ERR_INVALID_RESPONSE;
        } else if (r.sequence != q.sequence) error = ESP_ERR_INVALID_RESPONSE;
        else {
            size_t size = q.kind == CERBERUS_MESH_OBSERVATION_ROW ?
                CERBERUS_MESH_OBSERVATION_RECORD_BYTES : CERBERUS_MESH_OBSERVATION_SUMMARY_BYTES;
            size_t remaining = size - q.part * CERBERUS_MESH_OBSERVATION_CHUNK_BYTES;
            for (size_t i = remaining; i < sizeof(r.data); ++i)
                if (r.data[i]) error = ESP_ERR_INVALID_RESPONSE;
        }
    } else if (error == ESP_OK) {
        cerberus_mesh_observation_control_t r;
        memcpy(&r, reply, sizeof(r));
        if ((command != CERBERUS_MESH_OBSERVATION_COMMAND_INFO && r.session_id != session) ||
            r.state > CERBERUS_MESH_OBSERVATION_ERROR || (r.flags & 0x80U) ||
            r.count > CERBERUS_MESH_OBSERVATION_CAPACITY ||
            (r.channel && (r.channel < 11U || r.channel > 26U))) error = ESP_ERR_INVALID_RESPONSE;
        /* Full state/ownership and command-specific canonical reply validation
         * belongs to the pure controller, which retains the accepted identity. */
    }
    if (error != ESP_OK) memset(reply, 0, reply_length);
    return error;
}
