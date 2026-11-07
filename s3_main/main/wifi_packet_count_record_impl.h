#define PACKET_COUNT_RECORD_STACK 6144U
#define PACKET_COUNT_RECORD_RETRIES 3U
enum { COUNT_RECORD_PEEK, COUNT_RECORD_META, COUNT_RECORD_FRAME, COUNT_RECORD_ACK,
    COUNT_RECORD_EMPTY_VERIFY, COUNT_RECORD_STATS_PEEK, COUNT_RECORD_STATS_READ };
typedef struct {
    uint64_t session, next_poll_us;
    uint32_t pin, sequence, consumed, crc;
    uint16_t part, length;
    uint8_t phase, retries;
    bool disabled, file_open, storage_failed;
    uint8_t metadata[CERBERUS_PACKET_COUNT_RAW_META_BYTES];
} packet_count_record_stream_t;
typedef struct {
    wifi_packet_count_transport_t transport;
    uint64_t job;
    packet_count_record_stream_t source[2];
    /* Raw chunks enter block first. After CRC validation the serializer moves
     * them in place to make room for PCAPNG/radiotap headers. The current
     * source owns this scratch through ACK, so no second frame buffer is
     * needed and an ACK retry must never serialize or append it again. */
    uint8_t block[CERBERUS_WIFI_PCAP_RECORD_MAX_SIZE];
    unsigned current;
    bool monitor;
} packet_count_record_context_t;

static portMUX_TYPE packet_count_record_lock = portMUX_INITIALIZER_UNLOCKED;
static wifi_packet_count_record_status_t packet_count_record_public;
static uint64_t packet_count_record_epoch;

static uint32_t packet_count_record_increment(uint32_t value)
{ return value == UINT32_MAX ? value : value + 1U; }

static void packet_count_record_error(unsigned source, esp_err_t error, bool unknown)
{
    portENTER_CRITICAL(&packet_count_record_lock);
    wifi_packet_count_record_source_t *out = &packet_count_record_public.source[source];
    if (out->error == ESP_OK) out->error = error;
    out->loss_unknown |= unknown;
    out->transport_errors = packet_count_record_increment(out->transport_errors);
    portEXIT_CRITICAL(&packet_count_record_lock);
}

static void packet_count_record_drained_publish(unsigned source)
{
    portENTER_CRITICAL(&packet_count_record_lock);
    packet_count_record_public.source[source].drained = true;
    packet_count_record_public.source[source].bound = false;
    portEXIT_CRITICAL(&packet_count_record_lock);
}

static esp_err_t packet_count_record_exchange(packet_count_record_context_t *context,
    uint8_t command, const void *request, size_t size, void *reply, size_t reply_size)
{
    memset(reply, 0, reply_size);
    return context->transport.exchange(context->transport.context, context->current,
        command, request, size, reply, reply_size);
}

static esp_err_t packet_count_record_remote_error(const uint8_t *data, size_t length)
{
    int32_t error;
    memcpy(&error, data, sizeof(error));
    if (error == ESP_OK) return ESP_ERR_INVALID_RESPONSE;
    for (size_t i = sizeof(error); i < length; ++i)
        if (data[i]) return ESP_ERR_INVALID_RESPONSE;
    return error;
}

/* Exactly one bounded transport or storage operation at a time. The single
 * frame buffer belongs to current until ACK, then receivers alternate. */
static void packet_count_record_step(packet_count_record_context_t *context)
{
    unsigned source = context->current;
    packet_count_record_stream_t *stream = &context->source[source];
    wifi_packet_count_record_source_t desired;
    portENTER_CRITICAL(&packet_count_record_lock);
    desired = packet_count_record_public.source[source];
    portEXIT_CRITICAL(&packet_count_record_lock);
    if (!desired.bound) { context->current ^= 1U; return; }
    if (stream->session != desired.session) {
        bool file_open = stream->file_open, storage_failed = stream->storage_failed;
        memset(stream, 0, sizeof(*stream));
        stream->session = desired.session;
        stream->file_open = file_open;
        stream->storage_failed = storage_failed;
    }
    if (stream->disabled) {
        if (desired.draining) packet_count_record_drained_publish(source);
        context->current ^= 1U;
        return;
    }
    int64_t sampled_now = esp_timer_get_time();
    if (stream->phase == COUNT_RECORD_PEEK && !desired.draining && sampled_now >= 0 &&
        (uint64_t)sampled_now < stream->next_poll_us) {
        context->current ^= 1U;
        return;
    }
    if (!stream->file_open && !stream->storage_failed) {
        size_t length = cerberus_wifi_pcapng_file_header(source ? CERBERUS_WIFI_PCAP_RADIO_C5 :
            CERBERUS_WIFI_PCAP_RADIO_S3, context->block, sizeof(context->block));
        esp_err_t error = length ? cerberus_storage_packet_count_open(context->job, source,
            context->block, length) : ESP_ERR_INVALID_SIZE;
        stream->file_open = error == ESP_OK;
        stream->storage_failed = error != ESP_OK;
        portENTER_CRITICAL(&packet_count_record_lock);
        if (error != ESP_OK && packet_count_record_public.source[source].error == ESP_OK)
            packet_count_record_public.source[source].error = error;
        if (error == ESP_OK) packet_count_record_public.source[source].bytes = length;
        portEXIT_CRITICAL(&packet_count_record_lock);
        return;
    }
    esp_err_t error = ESP_OK;
    if (stream->phase == COUNT_RECORD_PEEK) {
        cerberus_packet_count_peek_t request = {stream->session, CERBERUS_PACKET_COUNT_READ_RAW_META, 0U};
        cerberus_packet_count_control_t reply;
        error = packet_count_record_exchange(context, CERBERUS_PACKET_COUNT_COMMAND_PEEK,
            &request, sizeof(request), &reply, sizeof(reply));
        if (error == ESP_OK && (reply.session_id != stream->session ||
            reply.state > CERBERUS_PACKET_COUNT_ERROR)) error = ESP_ERR_INVALID_RESPONSE;
        if (error == ESP_OK && !reply.sequence &&
            (reply.flags & (CERBERUS_PACKET_COUNT_BOUND | CERBERUS_PACKET_COUNT_RECORDING)) ==
                (CERBERUS_PACKET_COUNT_BOUND | CERBERUS_PACKET_COUNT_RECORDING) &&
            !(reply.flags & (CERBERUS_PACKET_COUNT_DATA_READY | CERBERUS_PACKET_COUNT_RX_ACTIVE)) &&
            reply.error_code == ESP_ERR_INVALID_STATE) {
            /* Binding precedes TARGET/START so the first received frames stay
             * owned even if START's reply is lost. Neither producer exposes a
             * raw queue before table initialization. This is setup wait, not
             * a transport failure. A canceled staging session has no data. */
            bool setup = (reply.flags & CERBERUS_PACKET_COUNT_HELD) &&
                (reply.state == CERBERUS_PACKET_COUNT_STAGING || reply.state == CERBERUS_PACKET_COUNT_QUEUED);
            bool never_started = !(reply.flags & CERBERUS_PACKET_COUNT_HELD) &&
                (reply.state == CERBERUS_PACKET_COUNT_COMPLETE || reply.state == CERBERUS_PACKET_COUNT_ERROR);
            if (setup || never_started) {
                stream->retries = 0U;
                /* PEEK sequence is a pin/error field, not the model revision.
                 * A terminal STATUS must prove revision0 before declaring a
                 * never-started FIFO empty; expired initialized data is loss. */
                if (never_started) stream->phase = COUNT_RECORD_EMPTY_VERIFY;
                else if (sampled_now >= 0) stream->next_poll_us = (uint64_t)sampled_now + 20000U;
                context->current ^= 1U;
                return;
            }
        }
        if (error == ESP_OK && !reply.sequence) {
            if (reply.error_code == ESP_ERR_NOT_FOUND && (reply.flags & CERBERUS_PACKET_COUNT_BOUND)) {
                stream->retries = 0U;
                if (desired.draining) {
                    if (context->monitor) stream->phase = COUNT_RECORD_STATS_PEEK;
                    else packet_count_record_drained_publish(source);
                }
                else if (sampled_now >= 0) stream->next_poll_us = (uint64_t)sampled_now + 20000U;
                context->current ^= 1U;
                return;
            }
            error = reply.error_code ? reply.error_code : ESP_ERR_INVALID_RESPONSE;
        }
        if (error == ESP_OK && (reply.sequence <= CERBERUS_PACKET_COUNT_PIN_BASE ||
            !(reply.flags & CERBERUS_PACKET_COUNT_BOUND))) error = ESP_ERR_INVALID_RESPONSE;
        if (error == ESP_OK) {
            stream->pin = reply.sequence;
            stream->part = 0U;
            stream->phase = COUNT_RECORD_META;
        }
    } else if (stream->phase == COUNT_RECORD_META) {
        cerberus_packet_count_read_t request = {stream->session, stream->pin, stream->part};
        cerberus_packet_count_chunk_t reply;
        error = packet_count_record_exchange(context, CERBERUS_PACKET_COUNT_COMMAND_READ,
            &request, sizeof(request), &reply, sizeof(reply));
        if (error == ESP_OK && (reply.session_id != stream->session || reply.part != stream->part))
            error = ESP_ERR_INVALID_RESPONSE;
        if (error == ESP_OK && !reply.pin) error = packet_count_record_remote_error(reply.data, sizeof(reply.data));
        if (error == ESP_OK && reply.pin != stream->pin) error = ESP_ERR_INVALID_RESPONSE;
        size_t offset = stream->part * CERBERUS_PACKET_COUNT_CHUNK_BYTES;
        size_t amount = sizeof(stream->metadata) - offset;
        if (amount > sizeof(reply.data)) amount = sizeof(reply.data);
        if (error == ESP_OK) for (size_t i = amount; i < sizeof(reply.data); ++i)
            if (reply.data[i]) error = ESP_ERR_INVALID_RESPONSE;
        if (error == ESP_OK) {
            memcpy(stream->metadata + offset, reply.data, amount);
            ++stream->part;
            if (offset + amount == sizeof(stream->metadata)) {
                const uint8_t *m = stream->metadata;
                stream->sequence = (uint32_t)cerberus_packet_count_get(m, 4U);
                stream->length = (uint16_t)cerberus_packet_count_get(m + 4U, 2U);
                stream->crc = (uint32_t)cerberus_packet_count_get(m + 16U, 4U);
                uint64_t time = cerberus_packet_count_get(m + 8U, 8U);
                if (!stream->sequence || stream->sequence > CERBERUS_PACKET_COUNT_SEQUENCE_MAX ||
                    (stream->sequence != stream->consumed && stream->sequence != stream->consumed + 1U) ||
                    stream->length < 24U ||
                    stream->length > CERBERUS_WIFI_PCAP_FRAME_MAX ||
                    cerberus_packet_count_get(m + 6U, 2U) != stream->length ||
                    !cerberus_wifi_packet_count_channel_bit(m[20]) ||
                    (m[22] & ~3U) || m[23] || m[24] || m[25] || m[26] || m[27] ||
                    ((m[22] & 2U) ? ((int8_t)m[21] == INT8_MIN || (int8_t)m[21] > 0) : m[21] != 0U) ||
                    ((m[22] & 1U) ? time > INT64_MAX : time != 0U) ||
                    (context->monitor && !(m[22] & 1U)))
                    error = ESP_ERR_INVALID_RESPONSE;
                if (error == ESP_OK) {
                    stream->phase = stream->sequence == stream->consumed ? COUNT_RECORD_ACK : COUNT_RECORD_FRAME;
                    stream->part = 0U;
                } else --stream->part; /* Retry the final metadata part, never past its bound. */
            }
        }
    } else if (stream->phase == COUNT_RECORD_FRAME) {
        cerberus_packet_count_read_t request = {stream->session, stream->sequence, stream->part};
        cerberus_packet_count_raw_chunk_t reply;
        error = packet_count_record_exchange(context, CERBERUS_PACKET_COUNT_COMMAND_RAW_READ,
            &request, sizeof(request), &reply, sizeof(reply));
        if (error == ESP_OK && (reply.session_id != stream->session || reply.part != stream->part))
            error = ESP_ERR_INVALID_RESPONSE;
        if (error == ESP_OK && !reply.pin) error = packet_count_record_remote_error(reply.data, sizeof(reply.data));
        if (error == ESP_OK && reply.pin != stream->sequence) error = ESP_ERR_INVALID_RESPONSE;
        size_t offset = stream->part * CERBERUS_PACKET_COUNT_RAW_CHUNK_BYTES;
        size_t amount = stream->length - offset;
        if (amount > sizeof(reply.data)) amount = sizeof(reply.data);
        if (error == ESP_OK) for (size_t i = amount; i < sizeof(reply.data); ++i)
            if (reply.data[i]) error = ESP_ERR_INVALID_RESPONSE;
        if (error == ESP_OK) {
            memcpy(context->block + offset, reply.data, amount);
            if (offset + amount < stream->length) ++stream->part;
            else {
                bool crc_ok = cerberus_protocol_crc32(context->block, stream->length) == stream->crc;
                cerberus_wifi_pcap_metadata_t metadata = {
                    .channel = stream->metadata[20], .rssi_dbm = (int8_t)stream->metadata[21],
                    .rssi_known = (stream->metadata[22] & 2U) != 0U,
                    .time_kind = CERBERUS_WIFI_PCAP_TIME_MONOTONIC,
                    .capture_time_us = cerberus_packet_count_get(stream->metadata + 8U, 8U)};
                size_t length = crc_ok ? cerberus_wifi_pcapng_record(&metadata, context->block,
                    stream->length, stream->length, context->block, sizeof(context->block)) : 0U;
                /* Mark before any append: an uncertain ACK/partial SD write
                 * cannot append this sequence a second time. */
                stream->consumed = stream->sequence;
                esp_err_t saved = ESP_ERR_INVALID_STATE;
                if (length && !stream->storage_failed) {
                    saved = context->monitor ? cerberus_storage_packet_monitor_append(
                        context->job, stream->session, stream->sequence, metadata.capture_time_us,
                        metadata.channel, metadata.rssi_dbm, metadata.rssi_known, stream->length,
                        context->block, length) : cerberus_storage_packet_count_append(
                            context->job, source, context->block, length);
                }
                if (length && saved != ESP_OK) stream->storage_failed = true;
                portENTER_CRITICAL(&packet_count_record_lock);
                wifi_packet_count_record_source_t *out = &packet_count_record_public.source[source];
                if (length && saved == ESP_OK) {
                    out->written = packet_count_record_increment(out->written);
                    out->bytes += length;
                } else {
                    out->discarded = packet_count_record_increment(out->discarded);
                    if (out->error == ESP_OK) out->error = length ? saved : ESP_ERR_INVALID_RESPONSE;
                }
                portEXIT_CRITICAL(&packet_count_record_lock);
                stream->phase = COUNT_RECORD_ACK;
            }
        }
    } else if (stream->phase == COUNT_RECORD_EMPTY_VERIFY) {
        cerberus_packet_count_request_t request = {stream->session};
        cerberus_packet_count_control_t reply;
        error = packet_count_record_exchange(context, CERBERUS_PACKET_COUNT_COMMAND_STATUS,
            &request, sizeof(request), &reply, sizeof(reply));
        if (error == ESP_OK && (reply.session_id != stream->session || reply.sequence ||
            (reply.flags & (CERBERUS_PACKET_COUNT_BOUND | CERBERUS_PACKET_COUNT_RECORDING)) !=
                (CERBERUS_PACKET_COUNT_BOUND | CERBERUS_PACKET_COUNT_RECORDING) ||
            (reply.flags & (CERBERUS_PACKET_COUNT_HELD | CERBERUS_PACKET_COUNT_RX_ACTIVE | CERBERUS_PACKET_COUNT_DATA_READY)) ||
            (reply.state != CERBERUS_PACKET_COUNT_COMPLETE && reply.state != CERBERUS_PACKET_COUNT_ERROR)))
            error = ESP_ERR_INVALID_RESPONSE;
        if (error == ESP_OK) {
            if (desired.draining) packet_count_record_drained_publish(source);
            else {
                stream->phase = COUNT_RECORD_PEEK;
                if (sampled_now >= 0) stream->next_poll_us = (uint64_t)sampled_now + 20000U;
            }
            context->current ^= 1U;
        }
    } else if (stream->phase == COUNT_RECORD_STATS_PEEK) {
        cerberus_packet_count_peek_t request = {stream->session, CERBERUS_PACKET_COUNT_READ_SUMMARY, 0U};
        cerberus_packet_count_control_t reply;
        error = packet_count_record_exchange(context, CERBERUS_PACKET_COUNT_COMMAND_PEEK,
            &request, sizeof(request), &reply, sizeof(reply));
        if (error == ESP_OK && (reply.session_id != stream->session ||
            !(reply.flags & CERBERUS_PACKET_COUNT_BOUND) ||
            (reply.flags & (CERBERUS_PACKET_COUNT_HELD | CERBERUS_PACKET_COUNT_RX_ACTIVE)) ||
            reply.sequence <= CERBERUS_PACKET_COUNT_PIN_BASE)) error = ESP_ERR_INVALID_RESPONSE;
        if (error == ESP_OK) {
            stream->pin = reply.sequence;
            stream->part = 0U;
            stream->phase = COUNT_RECORD_STATS_READ;
        }
    } else if (stream->phase == COUNT_RECORD_STATS_READ) {
        cerberus_packet_count_read_t request = {stream->session, stream->pin, stream->part};
        cerberus_packet_count_chunk_t reply;
        error = packet_count_record_exchange(context, CERBERUS_PACKET_COUNT_COMMAND_READ,
            &request, sizeof(request), &reply, sizeof(reply));
        if (error == ESP_OK && (reply.session_id != stream->session || reply.pin != stream->pin ||
            reply.part != stream->part)) error = ESP_ERR_INVALID_RESPONSE;
        if (error == ESP_OK) {
            memcpy(context->block + stream->part * 8U, reply.data, 8U);
            if (++stream->part == 8U) {
                const uint8_t *b = context->block;
                if (b[0] != 1U || (b[1] & ~6U) || b[2] || b[3] > 2U ||
                    cerberus_packet_count_get(b + 4U, 4U) != cerberus_packet_count_get(b + 8U, 4U))
                    error = ESP_ERR_INVALID_RESPONSE;
                for (unsigned i = 40U; i < 64U; ++i) if (b[i]) error = ESP_ERR_INVALID_RESPONSE;
                if (error == ESP_OK) {
                    uint64_t rejected = 0U;
                    for (unsigned i = 16U; i <= 32U; i += 4U)
                        rejected += cerberus_packet_count_get(b + i, 4U);
                    portENTER_CRITICAL(&packet_count_record_lock);
                    wifi_packet_count_record_source_t *out = &packet_count_record_public.source[source];
                    out->received = (uint32_t)cerberus_packet_count_get(b + 4U, 4U);
                    out->acknowledged = (uint32_t)cerberus_packet_count_get(b + 8U, 4U);
                    out->queue_full = (uint32_t)cerberus_packet_count_get(b + 12U, 4U);
                    out->rejected = rejected > UINT32_MAX ? UINT32_MAX : (uint32_t)rejected;
                    out->statistics_valid = true;
                    if (b[1] && out->error == ESP_OK) out->error = ESP_ERR_INVALID_STATE;
                    portEXIT_CRITICAL(&packet_count_record_lock);
                    packet_count_record_drained_publish(source);
                } else --stream->part;
            }
        }
    } else {
        cerberus_packet_count_ack_t request = {stream->session, stream->consumed};
        cerberus_packet_count_control_t reply;
        error = packet_count_record_exchange(context, CERBERUS_PACKET_COUNT_COMMAND_RAW_ACK,
            &request, sizeof(request), &reply, sizeof(reply));
        if (error == ESP_OK && (reply.session_id != stream->session ||
            reply.sequence != stream->consumed || !(reply.flags & CERBERUS_PACKET_COUNT_BOUND)))
            error = ESP_ERR_INVALID_RESPONSE;
        if (error == ESP_OK) {
            stream->phase = COUNT_RECORD_PEEK;
            context->current ^= 1U;
        }
    }
    if (error != ESP_OK) {
        packet_count_record_error(source, error, false);
        if (++stream->retries >= PACKET_COUNT_RECORD_RETRIES) {
            /* Fail this recording source closed. Counting continues. Once the
             * producer stops, its untransferred FIFO is explicitly abandoned. */
            stream->disabled = true;
            portENTER_CRITICAL(&packet_count_record_lock);
            packet_count_record_public.source[source].loss_unknown = true;
            portEXIT_CRITICAL(&packet_count_record_lock);
            if (desired.draining) packet_count_record_drained_publish(source);
            context->current ^= 1U;
        }
    } else stream->retries = 0U;
}

static void packet_count_record_worker(void *argument)
{
    packet_count_record_context_t *context = argument;
    char directory[8] = {0};
    esp_err_t error = context->monitor ? cerberus_storage_packet_monitor_begin(context->job, directory) :
        cerberus_storage_packet_count_begin(context->job, directory);
    portENTER_CRITICAL(&packet_count_record_lock);
    packet_count_record_public.error = error;
    memcpy(packet_count_record_public.directory, directory, sizeof(directory));
    packet_count_record_public.ready = error == ESP_OK;
    portEXIT_CRITICAL(&packet_count_record_lock);
    if (error == ESP_OK) {
        for (;;) {
            portENTER_CRITICAL(&packet_count_record_lock);
            bool done = packet_count_record_public.finishing &&
                !packet_count_record_public.source[0].bound && !packet_count_record_public.source[1].bound;
            portEXIT_CRITICAL(&packet_count_record_lock);
            if (done) break;
            int64_t began = esp_timer_get_time();
            for (unsigned step = 0; step < 8U; ++step) {
                packet_count_record_step(context);
                int64_t now = esp_timer_get_time();
                if (now < began || now - began >= 4000) break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        cerberus_storage_packet_count_result_t result;
        error = cerberus_storage_packet_count_finish(context->job, &result);
        portENTER_CRITICAL(&packet_count_record_lock);
        packet_count_record_public.error = error;
        for (unsigned i = 0; i < 2U; ++i) {
            wifi_packet_count_record_source_t *out = &packet_count_record_public.source[i];
            out->bytes = result.bytes[i];
            if (out->error == ESP_OK) out->error = result.error[i];
            memcpy(out->file_name, result.file_name[i], sizeof(out->file_name));
        }
        portEXIT_CRITICAL(&packet_count_record_lock);
    }
    heap_caps_free(context);
    portENTER_CRITICAL(&packet_count_record_lock);
    packet_count_record_public.ready = false;
    packet_count_record_public.busy = false;
    portEXIT_CRITICAL(&packet_count_record_lock);
    vTaskDelete(NULL);
}

static esp_err_t packet_record_start(const wifi_packet_count_transport_t *transport,
    uint64_t *job, bool monitor)
{
    if (!job || !transport || !transport->exchange) return ESP_ERR_INVALID_ARG;
    *job = 0U;
    portENTER_CRITICAL(&packet_count_record_lock);
    if (packet_count_record_public.job || packet_count_record_epoch == UINT64_MAX) {
        portEXIT_CRITICAL(&packet_count_record_lock);
        return ESP_ERR_INVALID_STATE;
    }
    uint64_t reserved = ++packet_count_record_epoch;
    memset(&packet_count_record_public, 0, sizeof(packet_count_record_public));
    packet_count_record_public.job = reserved;
    packet_count_record_public.busy = true;
    portEXIT_CRITICAL(&packet_count_record_lock);
    packet_count_record_context_t *context = heap_caps_calloc(1U, sizeof(*context), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (context) {
        context->job = reserved;
        context->monitor = monitor;
        context->transport = *transport;
        if (xTaskCreate(packet_count_record_worker, "count_sd", PACKET_COUNT_RECORD_STACK,
            context, 4U, NULL) == pdPASS) { *job = reserved; return ESP_OK; }
        heap_caps_free(context);
    }
    portENTER_CRITICAL(&packet_count_record_lock);
    memset(&packet_count_record_public, 0, sizeof(packet_count_record_public));
    portEXIT_CRITICAL(&packet_count_record_lock);
    return ESP_ERR_NO_MEM;
}

esp_err_t wifi_packet_count_record_start(const wifi_packet_count_transport_t *transport, uint64_t *job)
{ return packet_record_start(transport, job, false); }

esp_err_t wifi_packet_count_record_start_monitor(const wifi_packet_count_transport_t *transport, uint64_t *job)
{ return packet_record_start(transport, job, true); }

bool wifi_packet_count_record_status(uint64_t job, wifi_packet_count_record_status_t *out)
{
    if (!out) return false;
    portENTER_CRITICAL(&packet_count_record_lock);
    bool matches = job && packet_count_record_public.job == job;
    if (matches) *out = packet_count_record_public;
    else memset(out, 0, sizeof(*out));
    portEXIT_CRITICAL(&packet_count_record_lock);
    return matches;
}

esp_err_t wifi_packet_count_record_bind(uint64_t job, unsigned source, uint64_t session)
{
    if (!job || source > 1U || !session) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&packet_count_record_lock);
    wifi_packet_count_record_source_t *out = &packet_count_record_public.source[source];
    esp_err_t error = ESP_ERR_INVALID_STATE;
    if (packet_count_record_public.job == job && packet_count_record_public.ready &&
        packet_count_record_public.busy && !packet_count_record_public.finishing &&
        (out->session == session || (!out->bound && session > out->session))) {
        /* Exact replay does not resurrect a drained session. */
        if (out->session != session) {
            out->session = session;
            out->bound = true;
            out->draining = out->drained = false;
        }
        error = out->drained ? ESP_ERR_INVALID_STATE : ESP_OK;
    }
    portEXIT_CRITICAL(&packet_count_record_lock);
    return error;
}

bool wifi_packet_count_record_drained(uint64_t job, unsigned source, uint64_t session)
{
    if (!job || source > 1U || !session) return false;
    portENTER_CRITICAL(&packet_count_record_lock);
    wifi_packet_count_record_source_t *out = &packet_count_record_public.source[source];
    bool done = false;
    if (packet_count_record_public.job == job && out->session == session) {
        out->draining = true;
        done = out->drained;
    }
    portEXIT_CRITICAL(&packet_count_record_lock);
    return done;
}

esp_err_t wifi_packet_count_record_finish(uint64_t job)
{
    portENTER_CRITICAL(&packet_count_record_lock);
    esp_err_t error = ESP_ERR_INVALID_STATE;
    if (job && packet_count_record_public.job == job) {
        packet_count_record_public.finishing = true;
        error = ESP_OK;
    }
    portEXIT_CRITICAL(&packet_count_record_lock);
    return error;
}

esp_err_t wifi_packet_count_record_release(uint64_t job)
{
    portENTER_CRITICAL(&packet_count_record_lock);
    esp_err_t error = ESP_ERR_INVALID_STATE;
    if (job && packet_count_record_public.job == job && !packet_count_record_public.busy) {
        memset(&packet_count_record_public, 0, sizeof(packet_count_record_public));
        error = ESP_OK;
    }
    portEXIT_CRITICAL(&packet_count_record_lock);
    return error;
}
