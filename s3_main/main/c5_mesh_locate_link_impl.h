/* Included after survey_session_frame_locked. One bounded passive exchange;
 * the owner resolves uncertain commands. Never replay a mutating request. */
static esp_err_t mesh_locate_request_size(uint8_t command, const void *request,
    size_t length, uint64_t *session)
{
    size_t expected;
    *session = 0U;
    switch (command) {
    case CERBERUS_MESH_LOCATE_COMMAND_INFO: expected = 0U; break;
    case CERBERUS_MESH_LOCATE_COMMAND_START:
        expected = sizeof(cerberus_mesh_locate_start_t); break;
    case CERBERUS_MESH_LOCATE_COMMAND_STATUS:
    case CERBERUS_MESH_LOCATE_COMMAND_STOP:
    case CERBERUS_MESH_LOCATE_COMMAND_PEEK:
    case CERBERUS_MESH_LOCATE_COMMAND_RELEASE:
        expected = sizeof(cerberus_mesh_locate_request_t); break;
    case CERBERUS_MESH_LOCATE_COMMAND_READ:
        expected = sizeof(cerberus_mesh_locate_read_t); break;
    default: return ESP_ERR_INVALID_ARG;
    }
    if (length != expected || (expected && !request)) return ESP_ERR_INVALID_ARG;
    if (!expected) return request ? ESP_ERR_INVALID_ARG : ESP_OK;
    memcpy(session, request, sizeof(*session));
    if (!*session) return ESP_ERR_INVALID_ARG;
    if (command == CERBERUS_MESH_LOCATE_COMMAND_START) {
        cerberus_mesh_locate_start_t q;
        memcpy(&q, request, sizeof(q));
        if (q.version != CERBERUS_MESH_LOCATE_VERSION || q.channel < 11U || q.channel > 26U ||
            (q.address_mode != 2U && q.address_mode != 3U) || q.pan_known > 1U ||
            (!q.pan_known && q.pan) ||
            (q.address_mode == 2U && q.address >= UINT64_C(0xfffe))) return ESP_ERR_INVALID_ARG;
    } else if (command == CERBERUS_MESH_LOCATE_COMMAND_READ) {
        cerberus_mesh_locate_read_t q;
        memcpy(&q, request, sizeof(q));
        if (q.pin <= CERBERUS_MESH_LOCATE_PIN_BASE || q.part >= CERBERUS_MESH_LOCATE_PARTS)
            return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t c5_spi_link_mesh_locate_exchange(uint8_t command, const void *request,
    size_t request_length, void *reply, size_t reply_length)
{
    if (!reply || reply_length != CERBERUS_SPI_FRAME_PAYLOAD_SIZE) return ESP_ERR_INVALID_ARG;
    uint64_t session;
    esp_err_t error = mesh_locate_request_size(command, request, request_length, &session);
    uint8_t query[sizeof(cerberus_mesh_locate_start_t)] = {0};
    /* START fills all22 wire bytes. Copy before clearing a possibly aliased reply. */
    if (error == ESP_OK && request_length) memcpy(query, request, request_length);
    memset(reply, 0, reply_length);
    if (error != ESP_OK) return error;
    if (!link_mutex) return ESP_ERR_INVALID_STATE;
    int64_t began = esp_timer_get_time();
    if (began < 0) return ESP_ERR_INVALID_STATE;
    uint32_t deadline = (uint32_t)((uint64_t)began / 1000U) + 20U;
    if (take_link_mutex(pdMS_TO_TICKS(20)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (command == CERBERUS_MESH_LOCATE_COMMAND_INFO || command == CERBERUS_MESH_LOCATE_COMMAND_START) {
        cerberus_c5_status_t device = {0};
        error = get_status_v2_passive_locked(++sequence, deadline, &device);
        peer_firmware_version = error == ESP_OK ? device.firmware_version : 0U;
    }
    if (error == ESP_OK && peer_firmware_version < CERBERUS_MESH_LOCATE_MIN_FIRMWARE)
        error = ESP_ERR_NOT_SUPPORTED;
    if (error == ESP_OK) error = survey_session_frame_locked((cerberus_spi_command_t)command,
        request_length ? query : NULL, request_length, reply, reply_length, deadline);
    give_link_mutex();
    int64_t ended = esp_timer_get_time();
    if (error == ESP_OK && (ended < began || (uint64_t)(ended - began) >= 20000U))
        error = ESP_ERR_TIMEOUT;
    if (error == ESP_OK && command == CERBERUS_MESH_LOCATE_COMMAND_READ) {
        cerberus_mesh_locate_read_t q;
        cerberus_mesh_locate_chunk_t r;
        memcpy(&q, query, sizeof(q)); memcpy(&r, reply, sizeof(r));
        if (r.session_id != session || r.part != q.part) error = ESP_ERR_INVALID_RESPONSE;
        else if (!r.pin) {
            int32_t remote;
            memcpy(&remote, r.data, sizeof(remote));
            error = remote ? remote : ESP_ERR_INVALID_RESPONSE;
            for (size_t i = 4U; i < sizeof(r.data); ++i)
                if (r.data[i]) error = ESP_ERR_INVALID_RESPONSE;
        } else if (r.pin != q.pin) error = ESP_ERR_INVALID_RESPONSE;
        else {
            size_t remaining = CERBERUS_MESH_LOCATE_PROJECTION_BYTES -
                q.part * CERBERUS_MESH_LOCATE_CHUNK_BYTES;
            for (size_t i = remaining; i < sizeof(r.data); ++i)
                if (r.data[i]) error = ESP_ERR_INVALID_RESPONSE;
            /* The controller additionally checks the exact variable-length
             * pin boundary from PEEK, including its final-part padding. */
        }
    } else if (error == ESP_OK) {
        cerberus_mesh_locate_control_t r;
        memcpy(&r, reply, sizeof(r));
        if ((command != CERBERUS_MESH_LOCATE_COMMAND_INFO && r.session_id != session) ||
            r.state > CERBERUS_MESH_LOCATE_ERROR || (r.flags & 0x80U) || r.reserved ||
            (r.channel && (r.channel < 11U || r.channel > 26U)) ||
            (r.projection_bytes && (r.projection_bytes < CERBERUS_MESH_LOCATE_HEADER_BYTES ||
                r.projection_bytes > CERBERUS_MESH_LOCATE_PROJECTION_BYTES)))
            error = ESP_ERR_INVALID_RESPONSE;
        /* Canonical state/ownership/pin/version semantics are checked by the
         * pure controller, which retains the exact accepted owner identity. */
    }
    if (error != ESP_OK) memset(reply, 0, reply_length);
    return error;
}
