/* Included in storage.c; every file operation and lease uses storage_mutex. */
#ifndef O_BINARY
#define O_BINARY 0
#endif
static unsigned sd_gps_next_index = 1U;

static bool gps_storage_absent(const char *path)
{
    struct stat item;
    if (stat(path, &item) == 0) { errno = EEXIST; return false; }
    return errno == ENOENT;
}

static esp_err_t gps_storage_write(gps_storage_t *s, const char *data, size_t length)
{
    if (s->error != ESP_OK) return s->error;
    if (!s->file) return s->error = ESP_ERR_INVALID_STATE;
    if (s->bytes > CERBERUS_STORAGE_WARDRIVE_MAX_BYTES ||
        length > CERBERUS_STORAGE_WARDRIVE_MAX_BYTES - s->bytes)
        return s->error = ESP_ERR_INVALID_SIZE;
    size_t wrote = fwrite(data, 1, length, s->file);
    s->bytes += wrote;
    if (wrote != length || ferror(s->file)) return s->error = ESP_FAIL;
    int64_t now = esp_timer_get_time();
    if (now < s->synced_us || now - s->synced_us >= INT64_C(2000000)) {
        if (!recon_storage_flush(s->file)) return s->error = ESP_FAIL;
        s->synced_us = now;
    }
    return ESP_OK;
}

esp_err_t cerberus_storage_gps_begin(uint64_t job, cerberus_gpx_mode_t mode)
{
    const char *header = cerberus_gpx_header(mode);
    if (!job || !header) return ESP_ERR_INVALID_ARG;
    esp_err_t error = take_storage_mutex();
    if (error != ESP_OK) return error;
    /* Separate paths and serialized writes allow an intentional GPX recording
     * beside the one Wardriving CSV. Other exclusive writers stay excluded. */
    if (require_firmware_owner_locked() != ESP_OK || sd_gps || sd_ward_log_lease ||
        sd_wifi_capture_file || sd_recon.active || sd_packet_count) error = ESP_ERR_INVALID_STATE;
    gps_storage_t *s = NULL;
    if (error == ESP_OK) {
        s = heap_caps_calloc(1, sizeof(*s), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s) error = ESP_ERR_NO_MEM;
    }
    if (error == ESP_OK) error = recon_storage_directory(CERBERUS_STORAGE_MOUNT_PATH "/CERBERUS");
    if (error == ESP_OK) error = recon_storage_directory(CERBERUS_STORAGE_MOUNT_PATH CERBERUS_STORAGE_GPS_PATH);
    if (error == ESP_OK) {
        bool claimed = false;
        for (unsigned attempt = 0; attempt < 1024U; ++attempt) {
            unsigned index = sd_gps_next_index;
            sd_gps_next_index = index == 99999U ? 1U : index + 1U;
            snprintf(s->name, sizeof(s->name), "GPS-%c%05u.GPX", mode == CERBERUS_GPX_TRACK ? 'T' : 'P', index);
            int n = snprintf(s->final, sizeof(s->final), "%s%s/%s", CERBERUS_STORAGE_MOUNT_PATH,
                CERBERUS_STORAGE_GPS_PATH, s->name);
            if (n < 4 || (size_t)n >= sizeof(s->final)) { error = ESP_ERR_INVALID_SIZE; break; }
            memcpy(s->temp, s->final, (size_t)n + 1U);
            memcpy(s->temp + n - 3, "TMP", 3);
            if (!gps_storage_absent(s->final)) {
                if (errno != EEXIST) { error = ESP_FAIL; break; }
                continue;
            }
            int fd = open(s->temp, O_WRONLY | O_CREAT | O_EXCL | O_BINARY, 0664);
            if (fd < 0) { if (errno == EEXIST) continue; error = ESP_FAIL; break; }
            s->file = fdopen(fd, "wb");
            if (!s->file) { (void)close(fd); error = ESP_FAIL; break; }
            claimed = true;
            break;
        }
        if (error == ESP_OK && !claimed) error = ESP_ERR_NOT_FOUND;
    }
    if (error == ESP_OK) {
        s->job = job; s->mode = mode; s->synced_us = esp_timer_get_time();
        error = gps_storage_write(s, header, strlen(header));
    }
    if (error == ESP_OK) sd_gps = s;
    else if (s) {
        if (s->file) (void)fclose(s->file);
        heap_caps_free(s); /* Keep any failed TMP; never publish incomplete XML. */
    }
    release_storage_mutex();
    return error;
}

esp_err_t cerberus_storage_gps_append(uint64_t job, const char *row, size_t length)
{
    if (!job || !row || !length || length >= CERBERUS_GPX_ROW_CAPACITY) return ESP_ERR_INVALID_ARG;
    esp_err_t error = take_storage_mutex();
    if (error != ESP_OK) return error;
    if (!sd_gps || sd_gps->job != job || require_firmware_owner_locked() != ESP_OK)
        error = ESP_ERR_INVALID_STATE;
    else error = gps_storage_write(sd_gps, row, length);
    release_storage_mutex();
    return error;
}

esp_err_t cerberus_storage_gps_finish(uint64_t job, bool keep,
    char name[CERBERUS_STORAGE_GPS_NAME_CAPACITY], uint64_t *bytes)
{
    if (name) name[0] = '\0';
    if (bytes) *bytes = 0;
    if (!job || !name || !bytes) return ESP_ERR_INVALID_ARG;
    esp_err_t error = take_storage_mutex();
    if (error != ESP_OK) return error;
    gps_storage_t *s = sd_gps;
    if (!s || s->job != job || require_firmware_owner_locked() != ESP_OK) {
        release_storage_mutex();
        return ESP_ERR_INVALID_STATE;
    }
    if (keep && s->error == ESP_OK) {
        const char *footer = cerberus_gpx_footer(s->mode);
        (void)gps_storage_write(s, footer, strlen(footer));
    }
    if (!recon_storage_flush(s->file)) s->error = ESP_FAIL;
    if (fclose(s->file) != 0) s->error = ESP_FAIL;
    s->file = NULL;
    *bytes = s->bytes;
    error = s->error;
    if (error == ESP_OK && keep) {
        if (!gps_storage_absent(s->final) || rename(s->temp, s->final) != 0) error = ESP_FAIL;
        else memcpy(name, s->name, sizeof(s->name));
    } else if (error == ESP_OK && remove(s->temp) != 0) error = ESP_FAIL;
    sd_gps = NULL;
    heap_caps_free(s);
    release_storage_mutex();
    return error;
}
