/* Private to c5_spi_link.c. One bounded operation, including the capability
 * refresh on INFO/START. A missing mutation response is never replayed here. */
#include "wifi_fox_hunt_validate.h"

static esp_err_t fox_frame_locked(cerberus_spi_command_t command,
    const void *payload, size_t size, void *out, uint64_t signal_session, uint32_t deadline)
{
    cerberus_spi_frame_t request, response = {0};
    uint8_t discarded[CERBERUS_SPI_TRANSFER_SIZE] = {0};
    uint8_t token = ++sequence;
    if (!cerberus_protocol_prepare_frame(&request, command, token, payload, size)) return ESP_ERR_INVALID_ARG;
    esp_err_t error = spi_request_response_passive(&request, discarded, &response, sizeof(request), deadline);
    if (error != ESP_OK) return error;
    if (!cerberus_protocol_frame_is_valid(&response) || response.command != command || response.sequence != token)
        return ESP_ERR_INVALID_RESPONSE;
    if (signal_session && response.payload_length == sizeof(cerberus_wifi_fox_hunt_signal_error_t)) {
        cerberus_wifi_fox_hunt_signal_error_t failure;
        memcpy(&failure, response.payload, sizeof(failure));
        return failure.session_id == signal_session && failure.error_code != ESP_OK ?
            failure.error_code : ESP_ERR_INVALID_RESPONSE;
    }
    if (response.payload_length != sizeof(cerberus_wifi_fox_hunt_status_t)) return ESP_ERR_INVALID_RESPONSE;
    memcpy(out, response.payload, sizeof(cerberus_wifi_fox_hunt_status_t));
    return ESP_OK;
}

static esp_err_t fox_exchange(cerberus_spi_command_t command,
    const void *payload, size_t size, void *out, bool refresh, uint64_t signal_session)
{
    memset(out, 0, sizeof(cerberus_wifi_fox_hunt_status_t));
    if (!link_mutex) return ESP_ERR_INVALID_STATE;
    int64_t begun = esp_timer_get_time();
    if (begun < 0) return ESP_ERR_INVALID_STATE;
    uint32_t deadline = (uint32_t)((uint64_t)begun / 1000U) + 20U;
    if (take_link_mutex(pdMS_TO_TICKS(20)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t error = packet_monitor_session_gate_locked(deadline, refresh, CERBERUS_WIFI_FOX_HUNT_MIN_FIRMWARE);
    if (error == ESP_OK) error = fox_frame_locked(command, payload, size, out, signal_session, deadline);
    give_link_mutex();
    int64_t ended = esp_timer_get_time();
    if (error == ESP_OK && (ended < begun || (uint64_t)(ended - begun) >= 20000U)) error = ESP_ERR_TIMEOUT;
    if (error != ESP_OK) memset(out, 0, sizeof(cerberus_wifi_fox_hunt_status_t));
    return error;
}

static esp_err_t fox_control_exchange(cerberus_spi_command_t command, uint64_t session,
    const cerberus_wifi_fox_hunt_start_t *request, cerberus_wifi_fox_hunt_status_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    bool info = command == CERBERUS_SPI_COMMAND_WIFI_FOX_HUNT_INFO;
    bool start = command == CERBERUS_SPI_COMMAND_WIFI_FOX_HUNT_START;
    if ((!info && !session) || (start && !cerberus_wifi_fox_hunt_start_valid(request))) return ESP_ERR_INVALID_ARG;
    esp_err_t error = fox_exchange(command, info ? NULL : start ? (const void *)request : &session,
        info ? 0U : start ? sizeof(*request) : sizeof(session), out, info || start, 0U);
    if (error == ESP_OK && !wifi_fox_status_valid(out, info, session)) error = ESP_ERR_INVALID_RESPONSE;
    if (error == ESP_OK && start && (out->flags & CERBERUS_WIFI_FOX_HUNT_BOUND) &&
        ((out->current_channel && !wifi_fox_band_channel_valid(request->search_band, out->current_channel)) ||
         (out->last_target_channel && !wifi_fox_band_channel_valid(request->search_band, out->last_target_channel))))
        error = ESP_ERR_INVALID_RESPONSE;
    if (error != ESP_OK) memset(out, 0, sizeof(*out));
    return error;
}

esp_err_t c5_spi_link_wifi_fox_hunt_info(cerberus_wifi_fox_hunt_status_t *out)
{ return fox_control_exchange(CERBERUS_SPI_COMMAND_WIFI_FOX_HUNT_INFO, 0U, NULL, out); }
esp_err_t c5_spi_link_wifi_fox_hunt_start(const cerberus_wifi_fox_hunt_start_t *request,
    cerberus_wifi_fox_hunt_status_t *out)
{ return fox_control_exchange(CERBERUS_SPI_COMMAND_WIFI_FOX_HUNT_START, request ? request->session_id : 0U, request, out); }
esp_err_t c5_spi_link_wifi_fox_hunt_status(uint64_t session, cerberus_wifi_fox_hunt_status_t *out)
{ return fox_control_exchange(CERBERUS_SPI_COMMAND_WIFI_FOX_HUNT_STATUS, session, NULL, out); }
esp_err_t c5_spi_link_wifi_fox_hunt_stop(uint64_t session, cerberus_wifi_fox_hunt_status_t *out)
{ return fox_control_exchange(CERBERUS_SPI_COMMAND_WIFI_FOX_HUNT_STOP, session, NULL, out); }
esp_err_t c5_spi_link_wifi_fox_hunt_signal(uint64_t session, cerberus_wifi_fox_hunt_signal_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!session) return ESP_ERR_INVALID_ARG;
    esp_err_t error = fox_exchange(CERBERUS_SPI_COMMAND_WIFI_FOX_HUNT_SIGNAL, &session, sizeof(session), out, false, session);
    if (error == ESP_OK && !wifi_fox_signal_valid(out, session)) error = ESP_ERR_INVALID_RESPONSE;
    if (error != ESP_OK) memset(out, 0, sizeof(*out));
    return error;
}
