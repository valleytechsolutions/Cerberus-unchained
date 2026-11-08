/* Included by c5_spi_link.c. Incremental read-only AP transfer for the UI.
 * The worker-facing blocking APIs intentionally retain their existing contract. */
enum { WIFI_ROW_METADATA = 1, WIFI_ROW_SSID, WIFI_ROW_DONE, WIFI_ROW_FAILED };

static esp_err_t wifi_row_fail(cerberus_c5_wifi_scan_reader_t *reader, esp_err_t error)
{
    memset(&reader->pending, 0, sizeof(reader->pending));
    memset(&reader->observation, 0, sizeof(reader->observation));
    reader->phase = WIFI_ROW_FAILED;
    reader->error = error;
    return error;
}

static esp_err_t wifi_row_guard(cerberus_c5_wifi_scan_reader_t *reader,
    uint32_t *remaining, uint64_t *sample)
{
    int64_t current = esp_timer_get_time();
    if (current < 0 || (reader->clock_sampled && (uint64_t)current < reader->last_local_us))
        return ESP_ERR_INVALID_STATE;
    reader->clock_sampled = true;
    reader->last_local_us = (uint64_t)current;
    uint32_t left = reader->deadline_ms - (uint32_t)((uint64_t)current / 1000U);
    if (!left || left > INT32_MAX) return ESP_ERR_TIMEOUT;
    if (remaining) *remaining = left;
    if (sample) *sample = (uint64_t)current;
    return ESP_OK;
}

static esp_err_t wifi_row_exchange(cerberus_c5_wifi_scan_reader_t *reader,
    cerberus_spi_command_t command, const void *input, size_t input_size,
    void *output, size_t output_size, uint64_t *request_started_us)
{
    memset(output, 0, output_size);
    uint32_t remaining = 0U;
    esp_err_t error = wifi_row_guard(reader, &remaining, NULL);
    if (error != ESP_OK) return error;
    if (!link_mutex) return ESP_ERR_INVALID_STATE;
    if (remaining > 20U) remaining = 20U;
    if (take_link_mutex(pdMS_TO_TICKS(remaining)) != pdTRUE) return ESP_ERR_TIMEOUT;
    error = wifi_row_guard(reader, NULL, request_started_us);
    if (error == ESP_OK && command == CERBERUS_SPI_COMMAND_WIFI_SCAN_RESULT)
        reader->firmware_version = peer_firmware_version;
    else if (error == ESP_OK && command == CERBERUS_SPI_COMMAND_WIFI_SCAN_SSID &&
        reader->firmware_version != peer_firmware_version) error = ESP_ERR_INVALID_STATE;
    if (error == ESP_OK) {
        cerberus_spi_frame_t request, response = {0};
        uint8_t discarded[CERBERUS_SPI_TRANSFER_SIZE] = {0};
        uint8_t token = ++sequence;
        if (!cerberus_protocol_prepare_frame(&request, command, token, input, input_size))
            error = ESP_ERR_INVALID_ARG;
        else {
            error = spi_request_response_passive(&request, discarded, &response,
                sizeof(request), reader->deadline_ms);
            if (error == ESP_OK && cerberus_protocol_frame_is_valid(&response) && response.command == command &&
                response.sequence == token && response.payload_length == output_size) {
                memcpy(output, response.payload, output_size);
            } else if (error == ESP_OK) error = ESP_ERR_INVALID_RESPONSE;
        }
    }
    give_link_mutex();
    if (error == ESP_OK) error = wifi_row_guard(reader, NULL, NULL);
    if (error != ESP_OK) memset(output, 0, output_size);
    return error;
}

esp_err_t c5_spi_link_wifi_scan_result_begin(cerberus_c5_wifi_scan_reader_t *reader,
    uint8_t generation, uint8_t index, uint32_t deadline_ms)
{
    if (!reader) return ESP_ERR_INVALID_ARG;
    memset(reader, 0, sizeof(*reader));
    if (index >= CERBERUS_WIFI_SCAN_MAX_RESULTS) return wifi_row_fail(reader, ESP_ERR_INVALID_ARG);
    reader->generation = generation;
    reader->index = index;
    reader->deadline_ms = deadline_ms;
    reader->phase = WIFI_ROW_METADATA;
    return ESP_OK;
}

void c5_spi_link_wifi_scan_result_cancel(cerberus_c5_wifi_scan_reader_t *reader)
{
    if (reader) (void)wifi_row_fail(reader, ESP_ERR_NOT_FINISHED);
}

esp_err_t c5_spi_link_wifi_scan_result_step(cerberus_c5_wifi_scan_reader_t *reader,
    cerberus_c5_wifi_result_t *result, cerberus_s3_wifi_observation_t *observation, bool *complete)
{
    if (result) memset(result, 0, sizeof(*result));
    if (observation) memset(observation, 0, sizeof(*observation));
    if (complete) *complete = false;
    if (!reader || !result || !observation || !complete) {
        if (reader) (void)wifi_row_fail(reader, ESP_ERR_INVALID_ARG);
        return ESP_ERR_INVALID_ARG;
    }
    if (reader->phase == WIFI_ROW_FAILED) return reader->error;
    esp_err_t error = ESP_OK;
    if (reader->phase == WIFI_ROW_METADATA) {
        cerberus_wifi_scan_result_request_t query = {reader->generation, reader->index};
        cerberus_wifi_scan_result_payload_t metadata = {0};
        uint64_t request_started_us = 0U;
        error = wifi_row_exchange(reader, CERBERUS_SPI_COMMAND_WIFI_SCAN_RESULT,
            &query, sizeof(query), &metadata, sizeof(metadata), &request_started_us);
        if (error == ESP_OK && (metadata.generation != reader->generation ||
            metadata.index != reader->index || metadata.ssid_length > CERBERUS_WIFI_SSID_MAX_LENGTH ||
            metadata.security > CERBERUS_WIFI_SECURITY_OTHER || !metadata.channel))
            error = ESP_ERR_INVALID_RESPONSE;
        if (error == ESP_OK && !cerberus_s3_wifi_observation_decode(&metadata,
            reader->firmware_version, request_started_us, &reader->observation))
            error = ESP_ERR_INVALID_RESPONSE;
        if (error == ESP_OK) {
            reader->pending.rssi = metadata.rssi;
            reader->pending.channel = metadata.channel;
            reader->pending.security = metadata.security;
            memcpy(reader->pending.bssid, metadata.bssid, sizeof(metadata.bssid));
            reader->ssid_length = metadata.ssid_length;
            reader->phase = metadata.ssid_length ? WIFI_ROW_SSID : WIFI_ROW_DONE;
        }
    } else if (reader->phase == WIFI_ROW_SSID) {
        if (reader->copied >= reader->ssid_length || reader->ssid_length > CERBERUS_WIFI_SSID_MAX_LENGTH)
            return wifi_row_fail(reader, ESP_ERR_INVALID_STATE);
        uint8_t chunk = reader->copied / CERBERUS_WIFI_SSID_CHUNK_SIZE;
        uint8_t expected = reader->ssid_length - reader->copied;
        if (expected > CERBERUS_WIFI_SSID_CHUNK_SIZE) expected = CERBERUS_WIFI_SSID_CHUNK_SIZE;
        cerberus_wifi_scan_ssid_request_t query = {reader->generation, reader->index, chunk};
        cerberus_wifi_scan_ssid_payload_t reply = {0};
        error = wifi_row_exchange(reader, CERBERUS_SPI_COMMAND_WIFI_SCAN_SSID,
            &query, sizeof(query), &reply, sizeof(reply), NULL);
        if (error == ESP_OK && (reply.generation != reader->generation || reply.index != reader->index ||
            reply.chunk != chunk || reply.byte_count != expected)) error = ESP_ERR_INVALID_RESPONSE;
        if (error == ESP_OK) {
            memcpy(reader->pending.ssid + reader->copied, reply.bytes, expected);
            reader->copied += expected;
            reader->pending.ssid[reader->copied] = '\0';
            if (reader->copied == reader->ssid_length) reader->phase = WIFI_ROW_DONE;
        }
    } else if (reader->phase != WIFI_ROW_DONE) error = ESP_ERR_INVALID_STATE;
    if (error != ESP_OK) return wifi_row_fail(reader, error);
    if (reader->phase == WIFI_ROW_DONE) {
        *result = reader->pending;
        *observation = reader->observation;
        *complete = true;
    }
    return ESP_OK;
}

esp_err_t c5_spi_link_get_wifi_scan_status_once(uint32_t deadline_ms,
    cerberus_wifi_scan_status_payload_t *status)
{
    if (!status) return ESP_ERR_INVALID_ARG;
    cerberus_c5_wifi_scan_reader_t reader = {.deadline_ms = deadline_ms};
    esp_err_t error = wifi_row_exchange(&reader, CERBERUS_SPI_COMMAND_WIFI_SCAN_STATUS,
        NULL, 0U, status, sizeof(*status), NULL);
    if (error == ESP_OK && (status->state > CERBERUS_WIFI_SCAN_ERROR ||
        status->stored_count > CERBERUS_WIFI_SCAN_MAX_RESULTS || status->stored_count > status->total_count ||
        status->hidden_count > status->total_count || status->reserved[0] || status->reserved[1]))
        error = ESP_ERR_INVALID_RESPONSE;
    if (error != ESP_OK) memset(status, 0, sizeof(*status));
    return error;
}
