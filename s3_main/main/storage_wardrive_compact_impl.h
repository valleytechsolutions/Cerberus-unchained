/* Included by storage.c; the Wardriving lease and storage mutex are held.
 * The closed .TMP remains the recovery source until a complete CSV exists. */
static int64_t wardrive_compact_yield_us;
static void wardrive_compact_yield(void)
{
    int64_t now = esp_timer_get_time();
    /* At 100 Hz a tick per batch adds seconds to a large log. Share CPU
     * on elapsed time instead; SD calls can also yield while waiting. */
    if (now >= wardrive_compact_yield_us && now - wardrive_compact_yield_us < 20000) return;
    vTaskDelay(1);
    wardrive_compact_yield_us = esp_timer_get_time();
}

static esp_err_t wardrive_compact_publish(wardrive_storage_stream_t *stream,
    cerberus_wardrive_compact_stats_t *stats)
{
    char paths[3][256];
    const char *suffix[] = {".D0", ".D1", ".NEW"};
    for (unsigned i = 0; i < 3; ++i) {
        int n = snprintf(paths[i], sizeof(paths[i]), "%s%s", stream->temp_path, suffix[i]);
        if (n < 0 || (size_t)n >= sizeof(paths[i])) return ESP_ERR_INVALID_SIZE;
    }
    size_t capacity = cerberus_wardrive_compact_workspace_size();
    void *memory = heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!memory) return ESP_ERR_NO_MEM;
    FILE *files[3] = {0};
    wardrive_journal_reader_t reader = {.stream = stream};
    cerberus_wardrive_journals_t sources = {.context = &reader, .count = stream->parts,
        .select = wardrive_journal_select};
    bool created[3] = {false};
    esp_err_t error = ESP_OK;
    for (unsigned i = 0; i < 3 && error == ESP_OK; ++i) {
        int flags = O_RDWR | O_CREAT | O_EXCL;
#ifdef O_BINARY
        flags |= O_BINARY;
#endif
        int fd = open(paths[i], flags, 0600);
        if (fd < 0) { error = ESP_FAIL; break; }
        created[i] = true;
        files[i] = fdopen(fd, "w+b");
        if (!files[i]) { close(fd); error = ESP_FAIL; }
    }
    wardrive_compact_yield_us = esp_timer_get_time();
    if (error == ESP_OK) error = cerberus_wardrive_compact_journals(&sources, files[0], files[1],
        files[2], memory, capacity, wardrive_compact_yield, stats);
    if (error == ESP_OK && (fflush(files[2]) || fsync(fileno(files[2])))) error = ESP_FAIL;
    for (unsigned i = 0; i < 3; ++i)
        if (files[i] && fclose(files[i])) error = ESP_FAIL;
    if (!wardrive_journal_reader_close(&reader)) error = ESP_FAIL;
    heap_caps_free(memory);
    if (error == ESP_OK && rename(paths[2], stream->final_path)) error = ESP_FAIL;
    if (error == ESP_OK) {
        created[2] = false;
        /* A cleanup failure does not invalidate the already published CSV. */
        if (!wardrive_journal_remove(stream)) ESP_LOGW(TAG, "Wardriving recovery journals retained");
    }
    for (unsigned i = 0; i < 3; ++i) if (created[i]) (void)remove(paths[i]);
    return error;
}
