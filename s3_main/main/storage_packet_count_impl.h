/* Included in storage.c: all state and file operations share storage_mutex. */
static esp_err_t packet_count_storage_path(char path[128], unsigned source, bool final)
{
    if (!sd_packet_count || source > 1U) return ESP_ERR_INVALID_STATE;
    int n = snprintf(path, 128U, "%s/%s.%s", sd_packet_count->directory,
        source ? "C5" : "S3", final ? "PCAPNG" : "TMP");
    return n > 0 && n < 128 ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t packet_record_storage_begin(uint64_t job, char name[8], bool monitor)
{
    if (!job || !name) return ESP_ERR_INVALID_ARG;
    name[0] = '\0';
    esp_err_t error = take_storage_mutex();
    if (error != ESP_OK) return error;
    if (!ble_archive_storage_available()) error = ESP_ERR_INVALID_STATE;
    packet_count_storage_t *context = NULL;
    if (error == ESP_OK) {
        context = heap_caps_calloc(1U, sizeof(*context), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!context) error = ESP_ERR_NO_MEM;
    }
    if (error == ESP_OK) error = recon_storage_directory(CERBERUS_STORAGE_MOUNT_PATH "/CERBERUS");
    const char *base = monitor ? CERBERUS_STORAGE_MOUNT_PATH "/CERBERUS/MONITOR" :
        CERBERUS_STORAGE_MOUNT_PATH CERBERUS_STORAGE_PACKET_COUNT_PATH;
    if (error == ESP_OK) error = recon_storage_directory(base);
    if (error == ESP_OK) {
        bool created = false;
        for (unsigned attempt = 0; attempt < 1024U; ++attempt) {
            unsigned index = sd_packet_count_next_index;
            sd_packet_count_next_index = index == 99999U ? 1U : index + 1U;
            snprintf(context->directory, sizeof(context->directory),
                "%s/%c%05u", base, monitor ? 'M' : 'P', index);
            if (mkdir(context->directory, 0775) == 0) {
                snprintf(name, 8U, "%c%05u", monitor ? 'M' : 'P', index);
                created = true;
                break;
            }
            if (errno != EEXIST) { error = ESP_FAIL; break; }
        }
        if (error == ESP_OK && !created) error = ESP_ERR_NOT_FOUND;
    }
    if (error == ESP_OK) {
        context->job = job;
        context->monitor = monitor;
        sd_packet_count = context;
    } else if (context) heap_caps_free(context);
    release_storage_mutex();
    return error;
}

esp_err_t cerberus_storage_packet_count_begin(uint64_t job, char name[8])
{ return packet_record_storage_begin(job, name, false); }

esp_err_t cerberus_storage_packet_monitor_begin(uint64_t job, char name[8])
{ return packet_record_storage_begin(job, name, true); }

static esp_err_t packet_count_storage_write(packet_count_storage_stream_t *stream,
    const uint8_t *data, size_t length)
{
    if (stream->error != ESP_OK) return stream->error;
    if (!stream->file) return stream->error = ESP_ERR_INVALID_STATE;
    if (stream->bytes > CERBERUS_STORAGE_WIFI_CAPTURE_MAX_BYTES ||
        length > CERBERUS_STORAGE_WIFI_CAPTURE_MAX_BYTES - stream->bytes)
        return stream->error = ESP_ERR_INVALID_SIZE;
    size_t wrote = fwrite(data, 1U, length, stream->file);
    stream->bytes += wrote;
    if (wrote != length || ferror(stream->file)) return stream->error = ESP_FAIL;
    int64_t now = esp_timer_get_time();
    if (++stream->unsynced >= 32U || now < stream->last_sync_us ||
        now - stream->last_sync_us >= INT64_C(1000000)) {
        if (!recon_storage_flush(stream->file)) return stream->error = ESP_FAIL;
        stream->last_sync_us = now;
        stream->unsynced = 0U;
    }
    return ESP_OK;
}

static esp_err_t packet_monitor_index_path(char path[128], bool final)
{
    int n = snprintf(path, 128U, "%s/C5.%s", sd_packet_count->directory,
        final ? "CSV" : "IDX");
    return n > 0 && n < 128 ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t packet_monitor_index_open(void)
{
    static const char header[] = "pcap_packet,session_id,source_sequence,source_uptime_us,channel,rssi_dbm,frame_bytes\n";
    packet_count_storage_stream_t *index = &sd_packet_count->index;
    char path[128];
    esp_err_t error = packet_monitor_index_path(path, false);
    if (error == ESP_OK) {
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0664);
        if (fd < 0) error = ESP_FAIL;
        else {
            index->opened = true;
            index->file = fdopen(fd, "wb");
            if (!index->file) { (void)close(fd); error = ESP_FAIL; }
            else error = packet_count_storage_write(index, (const uint8_t *)header, sizeof(header) - 1U);
        }
    }
    index->error = error;
    return error;
}

esp_err_t cerberus_storage_packet_count_open(uint64_t job, unsigned source,
    const uint8_t *header, size_t length)
{
    if (!job || source > 1U || !header || length != 68U) return ESP_ERR_INVALID_ARG;
    esp_err_t error = take_storage_mutex();
    if (error != ESP_OK) return error;
    if (!sd_packet_count || sd_packet_count->job != job ||
        (sd_packet_count->monitor && source != 1U) ||
        require_firmware_owner_locked() != ESP_OK) error = ESP_ERR_INVALID_STATE;
    if (error == ESP_OK) {
        packet_count_storage_stream_t *stream = &sd_packet_count->source[source];
        if (stream->opened) error = stream->error;
        else {
            char path[128];
            error = packet_count_storage_path(path, source, false);
            if (error == ESP_OK) {
                int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0664);
                if (fd < 0) error = ESP_FAIL;
                else {
                    stream->opened = true;
                    stream->file = fdopen(fd, "wb");
                    if (!stream->file) { (void)close(fd); error = ESP_FAIL; }
                    else {
                        error = packet_count_storage_write(stream, header, length);
                        if (error == ESP_OK && sd_packet_count->monitor)
                            error = packet_monitor_index_open();
                    }
                }
            }
            if (error != ESP_OK) stream->error = error;
        }
    }
    release_storage_mutex();
    return error;
}

esp_err_t cerberus_storage_packet_count_append(uint64_t job, unsigned source,
    const uint8_t *record, size_t length)
{
    if (!job || source > 1U || !record || !length || length > 4144U) return ESP_ERR_INVALID_ARG;
    esp_err_t error = take_storage_mutex();
    if (error != ESP_OK) return error;
    if (!sd_packet_count || sd_packet_count->job != job || sd_packet_count->monitor ||
        require_firmware_owner_locked() != ESP_OK) error = ESP_ERR_INVALID_STATE;
    else error = packet_count_storage_write(&sd_packet_count->source[source], record, length);
    release_storage_mutex();
    return error;
}

esp_err_t cerberus_storage_packet_monitor_append(uint64_t job, uint64_t session,
    uint32_t sequence, uint64_t source_us, uint8_t channel, int8_t rssi,
    bool rssi_known, uint16_t frame_bytes, const uint8_t *record, size_t length)
{
    if (!job || !session || !sequence || source_us > INT64_MAX || !channel ||
        frame_bytes < 24U || frame_bytes > 4096U || !record || !length || length > 4144U ||
        (rssi_known && (rssi == INT8_MIN || rssi > 0))) return ESP_ERR_INVALID_ARG;
    esp_err_t error = take_storage_mutex();
    if (error != ESP_OK) return error;
    if (!sd_packet_count || sd_packet_count->job != job || !sd_packet_count->monitor ||
        require_firmware_owner_locked() != ESP_OK) error = ESP_ERR_INVALID_STATE;
    else {
        packet_count_storage_stream_t *stream = &sd_packet_count->source[1];
        char row[160], signal[8] = {0};
        if (rssi_known) snprintf(signal, sizeof(signal), "%d", (int)rssi);
        int n = snprintf(row, sizeof(row), "%lu,%llu,%lu,%llu,%u,%s,%u\n",
            (unsigned long)sd_packet_count->records + 1UL, (unsigned long long)session,
            (unsigned long)sequence, (unsigned long long)source_us,
            (unsigned)channel, signal, (unsigned)frame_bytes);
        if (sd_packet_count->records == UINT32_MAX || n <= 0 || (size_t)n >= sizeof(row))
            error = ESP_ERR_INVALID_SIZE;
        else error = packet_count_storage_write(stream, record, length);
        if (error == ESP_OK) error = packet_count_storage_write(&sd_packet_count->index,
            (const uint8_t *)row, (size_t)n);
        if (error == ESP_OK) ++sd_packet_count->records;
        else if (stream->error == ESP_OK) stream->error = error;
    }
    release_storage_mutex();
    return error;
}

esp_err_t cerberus_storage_packet_count_finish(uint64_t job,
    cerberus_storage_packet_count_result_t *result)
{
    if (!job || !result) return ESP_ERR_INVALID_ARG;
    memset(result, 0, sizeof(*result));
    esp_err_t error = ensure_storage_mutex();
    if (error != ESP_OK) return error;
    if (xSemaphoreTake(storage_mutex, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (!sd_packet_count || sd_packet_count->job != job) {
        release_storage_mutex();
        return ESP_ERR_INVALID_STATE;
    }
    for (unsigned source = 0; source < 3U; ++source) {
        packet_count_storage_stream_t *stream = source == 2U ? &sd_packet_count->index :
            &sd_packet_count->source[source];
        if (stream->file) {
            if (!recon_storage_flush(stream->file)) stream->error = ESP_FAIL;
            FILE *owned = stream->file;
            stream->file = NULL;
            if (fclose(owned) != 0) stream->error = ESP_FAIL;
        }
    }
    if (sd_packet_count->monitor && sd_packet_count->index.opened) {
        packet_count_storage_stream_t *index = &sd_packet_count->index;
        char temporary[128], final[128];
        if (index->error == ESP_OK && sd_packet_count->source[1].error == ESP_OK) {
            index->error = packet_monitor_index_path(temporary, false);
            if (index->error == ESP_OK) index->error = packet_monitor_index_path(final, true);
            if (index->error == ESP_OK) index->error = recon_storage_absent(final);
            if (index->error == ESP_OK && rename(temporary, final) != 0) index->error = ESP_FAIL;
        }
        if (sd_packet_count->source[1].error == ESP_OK)
            sd_packet_count->source[1].error = index->error;
    }
    /* Close both receiver files before publishing either result. */
    for (unsigned source = 0; source < 2U; ++source) {
        packet_count_storage_stream_t *stream = &sd_packet_count->source[source];
        if (stream->opened) {
            snprintf(result->file_name[source], sizeof(result->file_name[source]),
                "%s.TMP", source ? "C5" : "S3");
            char temporary[128], final[128];
            if (stream->error == ESP_OK) {
                stream->error = packet_count_storage_path(temporary, source, false);
                if (stream->error == ESP_OK) stream->error = packet_count_storage_path(final, source, true);
                if (stream->error == ESP_OK) stream->error = recon_storage_absent(final);
                if (stream->error == ESP_OK && rename(temporary, final) != 0) stream->error = ESP_FAIL;
                if (stream->error == ESP_OK) snprintf(result->file_name[source],
                    sizeof(result->file_name[source]), "%s.PCAPNG", source ? "C5" : "S3");
            }
        }
        result->bytes[source] = stream->bytes;
        result->error[source] = stream->error;
        if (error == ESP_OK) error = stream->error;
    }
    heap_caps_free(sd_packet_count);
    sd_packet_count = NULL;
    release_storage_mutex();
    return error;
}
