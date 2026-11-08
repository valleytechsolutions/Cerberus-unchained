/* Private to c5_spi_link.c. Capability checks and the request share one 20 ms
 * budget. Mutations are never replayed after an ambiguous response. */
#include "wifi_channel_analyzer_validate.h"

static esp_err_t analyzer_frame_locked(cerberus_spi_command_t command,
    const void *payload, size_t size, void *out, size_t out_size,
    uint64_t read_session, uint32_t read_key, uint32_t deadline)
{
    cerberus_spi_frame_t request, response = {0};
    uint8_t discarded[CERBERUS_SPI_TRANSFER_SIZE] = {0};
    uint8_t token = ++sequence;
    if (!cerberus_protocol_prepare_frame(&request, command, token, payload, size))
        return ESP_ERR_INVALID_ARG;
    esp_err_t error = spi_request_response_passive(&request, discarded, &response,
        sizeof(request), deadline);
    if (error != ESP_OK) return error;
    if (!cerberus_protocol_frame_is_valid(&response) || response.command != command ||
        response.sequence != token) return ESP_ERR_INVALID_RESPONSE;
    if (read_session && response.payload_length == sizeof(cerberus_wifi_channel_analyzer_read_error_t)) {
        cerberus_wifi_channel_analyzer_read_error_t failure;
        memcpy(&failure, response.payload, sizeof(failure));
        if (failure.session_id != read_session || failure.key != read_key || failure.error_code == ESP_OK)
            return ESP_ERR_INVALID_RESPONSE;
        return failure.error_code;
    }
    if (response.payload_length != out_size) return ESP_ERR_INVALID_RESPONSE;
    memcpy(out, response.payload, out_size);
    return ESP_OK;
}

static esp_err_t analyzer_exchange(cerberus_spi_command_t command,
    const void *payload, size_t size, void *out, size_t out_size,
    bool refresh, uint64_t read_session, uint32_t read_key)
{
    memset(out, 0, out_size);
    if (!link_mutex) return ESP_ERR_INVALID_STATE;
    int64_t begun = esp_timer_get_time();
    if (begun < 0) return ESP_ERR_INVALID_STATE;
    uint32_t deadline = (uint32_t)((uint64_t)begun / 1000U) + 20U;
    if (take_link_mutex(pdMS_TO_TICKS(20)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t error = packet_monitor_session_gate_locked(deadline, refresh,
        CERBERUS_WIFI_CHANNEL_ANALYZER_MIN_FIRMWARE);
    if (error == ESP_OK) error = analyzer_frame_locked(command, payload, size,
        out, out_size, read_session, read_key, deadline);
    give_link_mutex();
    int64_t ended = esp_timer_get_time();
    if (error == ESP_OK && (ended < begun || (uint64_t)(ended - begun) >= 20000U)) error = ESP_ERR_TIMEOUT;
    if (error != ESP_OK) memset(out, 0, out_size);
    return error;
}

static esp_err_t analyzer_control(cerberus_spi_command_t command,
    uint64_t session, uint8_t channel, cerberus_wifi_channel_analyzer_status_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    bool info = command == CERBERUS_SPI_COMMAND_WIFI_CHANNEL_ANALYZER_INFO;
    bool start = command == CERBERUS_SPI_COMMAND_WIFI_CHANNEL_ANALYZER_START;
    if ((!info && !session) || (start && !cerberus_wifi_packet_monitor_channel_valid(channel)))
        return ESP_ERR_INVALID_ARG;
    cerberus_wifi_channel_analyzer_start_t request = {session, channel};
    esp_err_t error = analyzer_exchange(command, info ? NULL : &request,
        info ? 0U : start ? sizeof(request) : sizeof(session), out, sizeof(*out),
        info || start, 0U, 0U);
    if (error == ESP_OK && (!wifi_analyzer_status_valid(out, info, session) ||
        (start && (out->flags & CERBERUS_WIFI_CHANNEL_ANALYZER_BOUND) && out->channel != channel)))
        error = ESP_ERR_INVALID_RESPONSE;
    if (error != ESP_OK) memset(out, 0, sizeof(*out));
    return error;
}

esp_err_t c5_spi_link_wifi_channel_analyzer_info(cerberus_wifi_channel_analyzer_status_t *out)
{ return analyzer_control(CERBERUS_SPI_COMMAND_WIFI_CHANNEL_ANALYZER_INFO, 0U, 0U, out); }
esp_err_t c5_spi_link_wifi_channel_analyzer_start(uint64_t session, uint8_t channel,
    cerberus_wifi_channel_analyzer_status_t *out)
{ return analyzer_control(CERBERUS_SPI_COMMAND_WIFI_CHANNEL_ANALYZER_START, session, channel, out); }
esp_err_t c5_spi_link_wifi_channel_analyzer_status(uint64_t session, cerberus_wifi_channel_analyzer_status_t *out)
{ return analyzer_control(CERBERUS_SPI_COMMAND_WIFI_CHANNEL_ANALYZER_STATUS, session, 0U, out); }
esp_err_t c5_spi_link_wifi_channel_analyzer_stop(uint64_t session, cerberus_wifi_channel_analyzer_status_t *out)
{ return analyzer_control(CERBERUS_SPI_COMMAND_WIFI_CHANNEL_ANALYZER_STOP, session, 0U, out); }

esp_err_t c5_spi_link_wifi_channel_analyzer_sample(uint64_t session, uint32_t sample_sequence,
    cerberus_wifi_channel_analyzer_sample_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!session || !sample_sequence) return ESP_ERR_INVALID_ARG;
    cerberus_wifi_channel_analyzer_read_t request = {session, sample_sequence};
    esp_err_t error = analyzer_exchange(CERBERUS_SPI_COMMAND_WIFI_CHANNEL_ANALYZER_SAMPLE,
        &request, sizeof(request), out, sizeof(*out), false, session, sample_sequence);
    if (error == ESP_OK && !wifi_analyzer_sample_valid(out, session, sample_sequence)) error = ESP_ERR_INVALID_RESPONSE;
    if (error != ESP_OK) memset(out, 0, sizeof(*out));
    return error;
}

esp_err_t c5_spi_link_wifi_channel_analyzer_hint_peek(uint64_t session,
    cerberus_wifi_channel_analyzer_hint_status_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!session) return ESP_ERR_INVALID_ARG;
    esp_err_t error = analyzer_exchange(CERBERUS_SPI_COMMAND_WIFI_CHANNEL_ANALYZER_HINT_PEEK,
        &session, sizeof(session), out, sizeof(*out), false, 0U, 0U);
    if (error == ESP_OK && !wifi_analyzer_hint_status_valid(out, session)) error = ESP_ERR_INVALID_RESPONSE;
    if (error != ESP_OK) memset(out, 0, sizeof(*out));
    return error;
}

esp_err_t c5_spi_link_wifi_channel_analyzer_hint_read(uint64_t session, uint32_t projection, uint8_t part,
    cerberus_wifi_channel_analyzer_hint_part_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!session || !projection || part >= CERBERUS_WIFI_CHANNEL_ANALYZER_HINT_PARTS) return ESP_ERR_INVALID_ARG;
    cerberus_wifi_channel_analyzer_hint_request_t request = {session, projection, part};
    esp_err_t error = analyzer_exchange(CERBERUS_SPI_COMMAND_WIFI_CHANNEL_ANALYZER_HINT_READ,
        &request, sizeof(request), out, sizeof(*out), false, session, projection);
    if (error == ESP_OK && !wifi_analyzer_hint_part_valid(out, session, projection, part)) error = ESP_ERR_INVALID_RESPONSE;
    if (error != ESP_OK) memset(out, 0, sizeof(*out));
    return error;
}
