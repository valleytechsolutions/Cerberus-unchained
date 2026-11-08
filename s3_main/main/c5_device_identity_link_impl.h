/* Included by c5_spi_link.c. The existing immutable fragment snapshot carries
 * metadata only; opening it never starts a receiver or changes a target list. */
enum { IDENTITY_PEER = 1, IDENTITY_OPEN, IDENTITY_READ, IDENTITY_DONE, IDENTITY_FAILED };

void c5_spi_link_device_identity_begin(cerberus_device_identity_reader_t *reader, uint32_t deadline_ms)
{
    if (!reader) return;
    memset(reader, 0, sizeof(*reader));
    reader->phase = IDENTITY_PEER;
    reader->deadline_ms = deadline_ms;
}

static esp_err_t identity_guard(cerberus_device_identity_reader_t *r, uint32_t *left)
{
    int64_t now = esp_timer_get_time();
    if (now < 0 || (uint64_t)now < r->last_us) return ESP_ERR_INVALID_STATE;
    r->last_us = (uint64_t)now;
    uint32_t remaining = r->deadline_ms - (uint32_t)((uint64_t)now / 1000U);
    if (!remaining || remaining > INT32_MAX) return ESP_ERR_TIMEOUT;
    if (left) *left = remaining;
    return ESP_OK;
}

static esp_err_t identity_exchange(cerberus_device_identity_reader_t *r,
    cerberus_spi_command_t command, const void *input, size_t input_size,
    void *output, size_t output_size)
{
    uint32_t left = 0;
    esp_err_t error = identity_guard(r, &left);
    if (error != ESP_OK) return error;
    if (!link_mutex) return ESP_ERR_INVALID_STATE;
    if (left > 20U) left = 20U;
    if (take_link_mutex(pdMS_TO_TICKS(left)) != pdTRUE) return ESP_ERR_TIMEOUT;
    error = identity_guard(r, NULL);
    if (error == ESP_OK && command == CERBERUS_SPI_COMMAND_STATUS)
        error = get_status_v2_passive_locked(++sequence, r->deadline_ms, output);
    else if (error == ESP_OK) {
        cerberus_spi_frame_t request, response = {0};
        uint8_t discarded[CERBERUS_SPI_TRANSFER_SIZE] = {0};
        uint8_t token = ++sequence;
        if (!cerberus_protocol_prepare_frame(&request, command, token, input, input_size))
            error = ESP_ERR_INVALID_ARG;
        else {
            error = spi_request_response_passive(&request, discarded, &response,
                sizeof(request), r->deadline_ms);
            if (error == ESP_OK && (!cerberus_protocol_frame_is_valid(&response) ||
                response.sequence != token || response.command != command || response.payload_length != output_size))
                error = ESP_ERR_INVALID_RESPONSE;
            if (error == ESP_OK) memcpy(output, response.payload, output_size);
        }
    }
    give_link_mutex();
    return error == ESP_OK ? identity_guard(r, NULL) : error;
}

esp_err_t c5_spi_link_device_identity_step(cerberus_device_identity_reader_t *r,
    cerberus_device_identity_t *output, bool *complete)
{
    if (output) memset(output, 0, sizeof(*output));
    if (complete) *complete = false;
    if (!r || !output || !complete) return ESP_ERR_INVALID_ARG;
    if (r->phase == IDENTITY_DONE) { *output = r->pending; *complete = true; return ESP_OK; }
    if (r->phase == IDENTITY_FAILED) return r->error;
    esp_err_t error = ESP_ERR_INVALID_STATE;
    if (r->phase == IDENTITY_PEER) {
        error = identity_exchange(r, CERBERUS_SPI_COMMAND_STATUS, NULL, 0, &r->peer, sizeof(r->peer));
        if (error == ESP_OK && (r->peer.protocol_version != CERBERUS_PROTOCOL_VERSION ||
            r->peer.firmware_version < CERBERUS_DEVICE_IDENTITY_MIN_FIRMWARE)) error = ESP_ERR_NOT_SUPPORTED;
        if (error == ESP_OK) r->phase = IDENTITY_OPEN;
    } else if (r->phase == IDENTITY_OPEN) {
        cerberus_fragment_open_request_t query = {.object_type = CERBERUS_FRAGMENT_OBJECT_DEVICE_IDENTITY};
        error = identity_exchange(r, CERBERUS_SPI_COMMAND_FRAGMENT_OPEN,
            &query, sizeof(query), &r->info, sizeof(r->info));
        if (error == ESP_OK && (r->info.object_type != query.object_type || !r->info.generation ||
            r->info.total_length != sizeof(r->pending) || r->info.fragment_size != CERBERUS_FRAGMENT_DATA_SIZE ||
            r->info.flags)) error = ESP_ERR_INVALID_RESPONSE;
        for (size_t i = 0; error == ESP_OK && i < sizeof(r->info.reserved); ++i)
            if (r->info.reserved[i]) error = ESP_ERR_INVALID_RESPONSE;
        if (error == ESP_OK) r->phase = IDENTITY_READ;
    } else if (r->phase == IDENTITY_READ && r->offset < sizeof(r->pending)) {
        cerberus_fragment_read_request_t query = {.object_type = r->info.object_type,
            .generation = r->info.generation, .offset = r->offset};
        cerberus_fragment_read_payload_t reply = {0};
        error = identity_exchange(r, CERBERUS_SPI_COMMAND_FRAGMENT_READ,
            &query, sizeof(query), &reply, sizeof(reply));
        size_t remaining = sizeof(r->pending) - r->offset;
        uint8_t length = remaining > CERBERUS_FRAGMENT_DATA_SIZE ? CERBERUS_FRAGMENT_DATA_SIZE : (uint8_t)remaining;
        uint8_t flags = remaining <= CERBERUS_FRAGMENT_DATA_SIZE ? CERBERUS_FRAGMENT_FLAG_LAST : 0;
        if (error == ESP_OK && (reply.object_type != query.object_type || reply.generation != query.generation ||
            reply.offset != query.offset || reply.data_length != length || reply.flags != flags)) error = ESP_ERR_INVALID_RESPONSE;
        if (error == ESP_OK) {
            memcpy((uint8_t *)&r->pending + r->offset, reply.data, length);
            r->offset += length;
            if (r->offset == sizeof(r->pending)) {
                if (cerberus_protocol_crc32(&r->pending, sizeof(r->pending)) != r->info.crc32)
                    error = ESP_ERR_INVALID_CRC;
                else if (!cerberus_device_identity_valid(&r->pending) ||
                         r->pending.capability_version != r->peer.firmware_version) error = ESP_ERR_INVALID_RESPONSE;
                else { r->phase = IDENTITY_DONE; *output = r->pending; *complete = true; }
            }
        }
    }
    if (error != ESP_OK) {
        memset(&r->pending, 0, sizeof(r->pending));
        memset(output, 0, sizeof(*output));
        r->phase = IDENTITY_FAILED;
        r->error = error;
    }
    return error;
}
