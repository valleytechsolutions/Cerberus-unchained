/* Included once by c5_spi_link.c. One bounded call, no retry loop and no
 * bank transfer. An unknown firmware cache adds one passive v2 status probe
 * inside the same deadline. Scoped START retries keep their bound identity. */
esp_err_t c5_spi_link_dual_ble_exchange(uint8_t command, const void *input,
    size_t input_size, void *output, size_t output_size)
{
    bool allowed = command >= CERBERUS_SPI_COMMAND_DUAL_BLE_INFO &&
        command <= CERBERUS_SPI_COMMAND_DUAL_BLE_TRACK_STATUS;
    bool snapshot = command >= CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_BEGIN &&
        command <= CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_STATUS;
    bool inventory = command == CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_INVENTORY;
    bool metadata = command == CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_METADATA;
    bool analyzer = command == CERBERUS_SPI_COMMAND_DUAL_BLE_ANALYZER_SAMPLE;
    bool reacquire = false;
    bool family = command == CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_FAMILY;
    bool response_read = command == CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_RESPONSE;
    bool compact_response = command == CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_RESPONSE_COMPACT;
    allowed = allowed || snapshot || inventory || metadata || analyzer || family || response_read || compact_response;
    allowed = allowed || command == CERBERUS_SPI_COMMAND_BLE_SCAN_RESULT ||
        command == CERBERUS_SPI_COMMAND_BLE_SCAN_DATA;
    if (!allowed || !output || !output_size || output_size > CERBERUS_SPI_FRAME_PAYLOAD_SIZE ||
        input_size > CERBERUS_SPI_FRAME_PAYLOAD_SIZE || (!input && input_size)) return ESP_ERR_INVALID_ARG;
    memset(output, 0, output_size);
    if (command == CERBERUS_SPI_COMMAND_DUAL_BLE_START) {
        cerberus_dual_ble_start_t start_request;
        if (!input || input_size != sizeof(start_request)) return ESP_ERR_INVALID_ARG;
        memcpy(&start_request, input, sizeof(start_request));
        if (!cerberus_dual_ble_start_valid(&start_request)) return ESP_ERR_INVALID_ARG;
        analyzer = start_request.mode == CERBERUS_DUAL_BLE_ANALYZER;
        reacquire = start_request.mode == CERBERUS_DUAL_BLE_REACQUIRE;
        family = start_request.mode == CERBERUS_DUAL_BLE_FAMILY;
        response_read = start_request.mode == CERBERUS_DUAL_BLE_RESPONSE_DISCOVERY ||
            start_request.mode == CERBERUS_DUAL_BLE_RESPONSE_FAMILY;
    }
    if (!link_mutex) return ESP_ERR_INVALID_STATE;
    int64_t start = esp_timer_get_time();
    if (start < 0) return ESP_ERR_INVALID_STATE;
    uint32_t deadline = (uint32_t)((uint64_t)start / 1000U) + 20U;
    if (take_link_mutex(pdMS_TO_TICKS(5U)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t error = ESP_OK;
    if (peer_firmware_version == 0U) {
        /* A failed unrelated status poll clears the shared cache. Unknown is
         * not proof of an old firmware: revalidate without a legacy fallback. */
        cerberus_c5_status_t peer = {0};
        error = get_status_v2_passive_locked(++sequence, deadline, &peer);
        int64_t probed = esp_timer_get_time();
        if (error == ESP_OK && (probed < start || probed - start >= 20000LL))
            error = ESP_ERR_TIMEOUT;
        peer_firmware_version = error == ESP_OK ? peer.firmware_version : 0U;
    }
    if (error == ESP_OK && peer_firmware_version < (reacquire ? CERBERUS_DUAL_BLE_REACQUIRE_MIN_FIRMWARE :
        compact_response ? CERBERUS_DUAL_BLE_COMPACT_RESPONSE_MIN_FIRMWARE :
        response_read ? CERBERUS_DUAL_BLE_RESPONSE_MIN_FIRMWARE :
        family ? CERBERUS_DUAL_BLE_FAMILY_MIN_FIRMWARE :
        analyzer ? CERBERUS_DUAL_BLE_ANALYZER_MIN_FIRMWARE :
        metadata ? CERBERUS_DUAL_BLE_METADATA_MIN_FIRMWARE :
        inventory ? CERBERUS_DUAL_BLE_INVENTORY_MIN_FIRMWARE :
        snapshot ? CERBERUS_DUAL_BLE_SNAPSHOT_MIN_FIRMWARE : CERBERUS_DUAL_BLE_MIN_FIRMWARE))
        error = ESP_ERR_NOT_SUPPORTED;
    if (error == ESP_OK) {
        cerberus_spi_frame_t request, response = {0};
        uint8_t discard[CERBERUS_SPI_TRANSFER_SIZE] = {0};
        uint8_t token = ++sequence;
        if (!cerberus_protocol_prepare_frame(&request, (cerberus_spi_command_t)command,
            token, input, input_size)) error = ESP_ERR_INVALID_ARG;
        else {
            error = spi_request_response_passive(&request, discard, &response, sizeof(request), deadline);
            if (error == ESP_OK && cerberus_protocol_frame_is_valid(&response) &&
                response.command == command && response.sequence == token && response.payload_length == output_size)
                memcpy(output, response.payload, output_size);
            else if (error == ESP_OK) error = ESP_ERR_INVALID_RESPONSE;
        }
    }
    give_link_mutex();
    int64_t ended = esp_timer_get_time();
    if (error == ESP_OK && (ended < start || ended - start > 20000LL)) error = ESP_ERR_TIMEOUT;
    if (error != ESP_OK) memset(output, 0, output_size);
    return error;
}
