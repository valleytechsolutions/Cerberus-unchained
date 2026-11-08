/* Included by c5_spi_link.c. Each operation is a single bounded attempt.
 * The caller retries a possible accepted START/STOP using the same epoch. */
static esp_err_t wifi_scan_band_exchange(cerberus_spi_command_t command,
    const cerberus_wifi_scan_band_request_t *query, cerberus_wifi_scan_band_response_t *reply)
{
    if (!reply) return ESP_ERR_INVALID_ARG;
    memset(reply, 0, sizeof(*reply));
    bool info = command == CERBERUS_SPI_COMMAND_WIFI_SCAN_BAND_INFO;
    if (!info && command != CERBERUS_SPI_COMMAND_WIFI_SCAN_BAND_START &&
        command != CERBERUS_SPI_COMMAND_WIFI_SCAN_BAND_STATUS &&
        command != CERBERUS_SPI_COMMAND_WIFI_SCAN_BAND_STOP) return ESP_ERR_INVALID_ARG;
    if ((!info && (!query || !query->session_id || !cerberus_wifi_scan_band_valid(query->band))) ||
        (info && query)) return ESP_ERR_INVALID_ARG;
    if (!link_mutex) return ESP_ERR_INVALID_STATE;
    int64_t now = esp_timer_get_time();
    if (now < 0) return ESP_ERR_INVALID_STATE;
    uint32_t deadline = (uint32_t)((uint64_t)now / 1000U) + 20U;
    if (take_link_mutex(pdMS_TO_TICKS(20)) != pdTRUE) return ESP_ERR_TIMEOUT;
    cerberus_c5_status_t device = {0};
    esp_err_t error = get_status_v2_passive_locked(++sequence, deadline, &device);
    peer_firmware_version = error == ESP_OK ? device.firmware_version : 0U;
    uint8_t band = query ? query->band : CERBERUS_WIFI_SCAN_BAND_ALL;
    uint32_t needed = band == CERBERUS_WIFI_SCAN_BAND_2G ? CERBERUS_C5_CAP_WIFI_2_4_GHZ :
        band == CERBERUS_WIFI_SCAN_BAND_5G ? CERBERUS_C5_CAP_WIFI_5_GHZ :
        CERBERUS_C5_CAP_WIFI_2_4_GHZ | CERBERUS_C5_CAP_WIFI_5_GHZ;
    if (error == ESP_OK && (device.firmware_version < CERBERUS_WIFI_SCAN_BAND_MIN_FIRMWARE ||
        (device.capabilities & needed) != needed)) error = ESP_ERR_NOT_SUPPORTED;
    if (error == ESP_OK) {
        cerberus_spi_frame_t request, response = {0};
        uint8_t discarded[CERBERUS_SPI_TRANSFER_SIZE] = {0};
        uint8_t token = ++sequence;
        if (!cerberus_protocol_prepare_frame(&request, command, token,
                query, query ? sizeof(*query) : 0U)) error = ESP_ERR_INVALID_ARG;
        else error = spi_request_response_passive(&request, discarded, &response,
            sizeof(request), deadline);
        if (error == ESP_OK && cerberus_protocol_frame_is_valid(&response) &&
            response.command == command && response.sequence == token &&
            response.payload_length == sizeof(*reply)) memcpy(reply, response.payload, sizeof(*reply));
        else if (error == ESP_OK) error = ESP_ERR_INVALID_RESPONSE;
    }
    give_link_mutex();
    int64_t finished = esp_timer_get_time();
    if (error == ESP_OK && finished < now) error = ESP_ERR_INVALID_STATE;
    if (error == ESP_OK && (uint64_t)(finished - now) >= 20000U) error = ESP_ERR_TIMEOUT;
    if (error == ESP_OK && (!reply->session_id || !cerberus_wifi_scan_band_valid(reply->band) ||
        reply->bound > 1U || reply->status.state > CERBERUS_WIFI_SCAN_ERROR ||
        reply->status.reserved[0] || reply->status.reserved[1] ||
        reply->status.stored_count > CERBERUS_WIFI_SCAN_MAX_RESULTS ||
        reply->status.stored_count > reply->status.total_count ||
        reply->status.hidden_count > reply->status.stored_count ||
        (!info && (reply->session_id != query->session_id || reply->band != query->band))))
        error = ESP_ERR_INVALID_RESPONSE;
    if (error != ESP_OK) memset(reply, 0, sizeof(*reply));
    return error;
}

esp_err_t c5_spi_link_wifi_scan_band_info(cerberus_wifi_scan_band_response_t *reply)
{
    return wifi_scan_band_exchange(CERBERUS_SPI_COMMAND_WIFI_SCAN_BAND_INFO, NULL, reply);
}

esp_err_t c5_spi_link_start_wifi_scan_band(uint8_t band, uint64_t session_id,
    cerberus_wifi_scan_status_payload_t *status)
{
    if (!status) return ESP_ERR_INVALID_ARG;
    memset(status, 0, sizeof(*status));
    const cerberus_wifi_scan_band_request_t query = {session_id, band};
    cerberus_wifi_scan_band_response_t reply;
    esp_err_t error = wifi_scan_band_exchange(CERBERUS_SPI_COMMAND_WIFI_SCAN_BAND_START, &query, &reply);
    if (error != ESP_OK) return error;
    *status = reply.status;
    if (!reply.bound || status->state == CERBERUS_WIFI_SCAN_ERROR)
        return status->error_code != ESP_OK ? status->error_code : ESP_ERR_INVALID_RESPONSE;
    if (status->error_code != ESP_OK) return ESP_ERR_INVALID_RESPONSE;
    return status->state == CERBERUS_WIFI_SCAN_IDLE ? ESP_ERR_INVALID_STATE : ESP_OK;
}

esp_err_t c5_spi_link_wifi_scan_band_status(uint8_t band, uint64_t session_id,
    cerberus_wifi_scan_status_payload_t *status)
{
    if (!status) return ESP_ERR_INVALID_ARG;
    memset(status, 0, sizeof(*status));
    const cerberus_wifi_scan_band_request_t query = {session_id, band};
    cerberus_wifi_scan_band_response_t reply;
    esp_err_t error = wifi_scan_band_exchange(CERBERUS_SPI_COMMAND_WIFI_SCAN_BAND_STATUS, &query, &reply);
    if (error != ESP_OK) return error;
    if (!reply.bound) return ESP_ERR_NOT_FOUND; /* No longer ours; never adopt this bank. */
    *status = reply.status;
    return ESP_OK;
}

esp_err_t c5_spi_link_stop_wifi_scan_band(uint8_t band, uint64_t session_id,
    cerberus_wifi_scan_status_payload_t *status)
{
    if (!status) return ESP_ERR_INVALID_ARG;
    memset(status, 0, sizeof(*status));
    const cerberus_wifi_scan_band_request_t query = {session_id, band};
    cerberus_wifi_scan_band_response_t reply;
    esp_err_t error = wifi_scan_band_exchange(CERBERUS_SPI_COMMAND_WIFI_SCAN_BAND_STOP, &query, &reply);
    if (error != ESP_OK) return error;
    if (!reply.bound) return ESP_ERR_NOT_FOUND;
    *status = reply.status;
    /* A running status still owns the radio even if the SDK Stop call failed.
     * Its control error is surfaced until the next bound status/Stop retry. */
    return ESP_OK;
}
