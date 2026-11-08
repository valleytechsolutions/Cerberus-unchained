/* Included by storage.c. The session lease and storage mutex cover every
 * rollover; radio workers continue publishing into their bounded queues. */
static bool wardrive_journal_path(const wardrive_storage_stream_t *stream,
    uint16_t part, char *path, size_t capacity)
{
    int length = part ? snprintf(path, capacity, "%s.P%05u", stream->temp_path, (unsigned)part) :
        snprintf(path, capacity, "%s", stream->temp_path);
    return length >= 0 && (size_t)length < capacity;
}

static esp_err_t wardrive_journal_advance(wardrive_storage_stream_t *stream)
{
    if (!stream->parts || stream->parts == UINT16_MAX || !stream->header_length ||
        stream->header_length > 512U) return ESP_ERR_INVALID_SIZE;
    char path[272], header[512];
    if (!wardrive_journal_path(stream, stream->parts, path, sizeof(path))) return ESP_ERR_INVALID_SIZE;
    FILE *base = fopen(stream->temp_path, "rb");
    if (!base) { stream->write_failed = true; return ESP_FAIL; }
    bool failed = fread(header, 1, stream->header_length, base) != stream->header_length;
    if (fclose(base)) failed = true;
    if (failed) { stream->write_failed = true; return ESP_FAIL; }
    int flags = O_RDWR | O_CREAT | O_EXCL;
#ifdef O_BINARY
    flags |= O_BINARY;
#endif
    int fd = open(path, flags, 0600);
    if (fd < 0) { stream->write_failed = true; return ESP_FAIL; }
    ++stream->parts; /* Even an incomplete new part belongs to this recovery set. */
    FILE *next = fdopen(fd, "w+b");
    if (!next) { close(fd); stream->write_failed = true; return ESP_FAIL; }
    failed = fwrite(header, 1, stream->header_length, next) != stream->header_length ||
        fflush(next) || fsync(fileno(next));
    if (failed) {
        (void)fclose(next);
        stream->write_failed = true;
        return ESP_FAIL;
    }
    FILE *previous = stream->file;
    failed = fflush(previous) || fsync(fileno(previous));
    if (fclose(previous)) failed = true;
    stream->file = next;
    stream->bytes += stream->header_length;
    stream->part_bytes = stream->header_length;
    stream->pending_rows = 0;
    stream->last_sync_us = esp_timer_get_time();
    stream->write_failed = failed;
    return failed ? ESP_FAIL : ESP_OK;
}

/* Two source handles avoid repeatedly reopening the common first/last extrema
 * pair. Together with two indexes, output and optional GPX/legacy streams this
 * stays within the existing eight-file mount limit. */
typedef struct {
    const wardrive_storage_stream_t *stream;
    FILE *files[2];
    uint32_t bytes[2];
    uint16_t parts[2];
    unsigned next;
} wardrive_journal_reader_t;

static esp_err_t wardrive_journal_select(void *context, uint16_t part, FILE **file, uint32_t *bytes)
{
    wardrive_journal_reader_t *reader = context;
    if (part >= reader->stream->parts) return ESP_ERR_INVALID_ARG;
    for (unsigned i = 0; i < 2; ++i) {
        if (reader->files[i] && reader->parts[i] == part) {
            *file = reader->files[i]; *bytes = reader->bytes[i];
            return ESP_OK;
        }
    }
    unsigned slot = reader->next;
    reader->next ^= 1U;
    if (reader->files[slot]) {
        FILE *previous = reader->files[slot];
        reader->files[slot] = NULL;
        if (fclose(previous)) return ESP_FAIL;
    }
    char path[272];
    if (!wardrive_journal_path(reader->stream, part, path, sizeof(path))) return ESP_ERR_INVALID_SIZE;
    FILE *source = fopen(path, "rb");
    if (!source) return ESP_FAIL;
    reader->files[slot] = source;
    reader->parts[slot] = UINT16_MAX;
    if (fseek(source, 0, SEEK_END)) return ESP_FAIL;
    long length = ftell(source);
    if (length <= 0 || (uint64_t)length > CERBERUS_STORAGE_WARDRIVE_MAX_BYTES) return ESP_ERR_INVALID_SIZE;
    *bytes = reader->bytes[slot] = (uint32_t)length;
    reader->parts[slot] = part;
    *file = source;
    return ESP_OK;
}

static bool wardrive_journal_reader_close(wardrive_journal_reader_t *reader)
{
    bool success = true;
    for (unsigned i = 0; i < 2; ++i) if (reader->files[i]) {
        if (fclose(reader->files[i])) success = false;
        reader->files[i] = NULL;
    }
    return success;
}

static bool wardrive_journal_remove(const wardrive_storage_stream_t *stream)
{
    bool success = true;
    char path[272];
    for (uint32_t part = 1; part < stream->parts; ++part) {
        if (!wardrive_journal_path(stream, (uint16_t)part, path, sizeof(path)) || remove(path))
            success = false;
    }
    /* Keep the base marker if any child remains, so a later recording cannot
     * reuse the family after its completed CSV is deleted. */
    if (success && remove(stream->temp_path)) success = false;
    return success;
}
