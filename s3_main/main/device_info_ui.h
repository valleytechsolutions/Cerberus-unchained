/* UI-task-owned view. SD and C5 reads run only on explicit Open/Refresh. */
static cerberus_device_info_job_t *device_info_job;
static cerberus_device_info_t *device_info_snapshot;
static bool device_info_requested, device_info_discard, device_info_ready;
static uint8_t device_info_tab;
static esp_err_t device_info_error;
static uint64_t device_info_last_draw;

static void device_info_close(void)
{
    device_info_requested = device_info_ready = false;
    if (device_info_job) {
        device_info_discard = true;
        cerberus_device_info_cancel(device_info_job);
        if (cerberus_device_info_release(&device_info_job)) device_info_discard = false;
    }
    heap_caps_free(device_info_snapshot);
    device_info_snapshot = NULL;
}

static void device_info_open(void)
{
    device_info_close();
    device_info_tab = 0;
    device_info_snapshot = heap_caps_calloc(1, sizeof(*device_info_snapshot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    device_info_error = device_info_snapshot ? ESP_OK : ESP_ERR_NO_MEM;
    device_info_requested = device_info_snapshot != NULL;
    device_info_last_draw = 0;
}

static bool device_info_tick(uint64_t now_ms, bool visible)
{
    if (!visible) {
        if (device_info_job || device_info_snapshot) device_info_close();
        return false;
    }
    if (!device_info_snapshot && device_info_error == ESP_OK) device_info_open();
    bool changed = false;
    if (device_info_discard) {
        if (!cerberus_device_info_release(&device_info_job)) return false;
        device_info_discard = false;
        changed = true;
    }
    if (device_info_requested && !device_info_job) {
        device_info_requested = false;
        device_info_ready = false;
        device_info_error = cerberus_device_info_start(&device_info_job);
        changed = true;
    }
    if (device_info_job && cerberus_device_info_read(device_info_job, device_info_snapshot)) {
        device_info_ready = true;
        (void)cerberus_device_info_release(&device_info_job);
        changed = true;
    }
    if (changed || now_ms - device_info_last_draw >= 1000U) {
        device_info_last_draw = now_ms;
        return true;
    }
    return false;
}

static esp_err_t device_info_line(uint16_t y, const char *text, uint16_t color)
{
    return cerberus_display_draw_text(12, y, text, color, UI_BACKGROUND, 1);
}

static esp_err_t device_info_draw_identity(const cerberus_device_identity_t *d, esp_err_t error)
{
    char line[50], address[24];
    if (error != ESP_OK) {
        snprintf(line, sizeof(line), "METADATA: %s", esp_err_to_name(error));
        ESP_RETURN_ON_ERROR(device_info_line(145, line, UI_BAD), TAG, "device info error");
        return device_info_line(175, error == ESP_ERR_NOT_SUPPORTED ?
            "C5 BUILD METADATA NEEDS CAPABILITY 80+" : "REFRESH TO TRY THE READ AGAIN", UI_MUTED);
    }
    snprintf(line, sizeof(line), "MODEL %u  CORES %u  REV %u.%02u", d->chip_model, d->cores,
        d->chip_revision / 100U, d->chip_revision % 100U);
    ESP_RETURN_ON_ERROR(device_info_line(124, line, UI_TEXT), TAG, "device chip");
    snprintf(line, sizeof(line), "APP %.32s", d->version);
    ESP_RETURN_ON_ERROR(device_info_line(142, line, UI_GOOD), TAG, "device app");
    snprintf(line, sizeof(line), "PROJECT %.32s", d->project);
    ESP_RETURN_ON_ERROR(device_info_line(160, line, UI_MUTED), TAG, "device project");
    snprintf(line, sizeof(line), "SDK %.32s", d->idf);
    ESP_RETURN_ON_ERROR(device_info_line(178, line, UI_TEXT), TAG, "device sdk");
    snprintf(line, sizeof(line), "BUILD %.11s %.11s", d->date, d->time);
    ESP_RETURN_ON_ERROR(device_info_line(196, line, UI_MUTED), TAG, "device build");
    if (d->flags & CERBERUS_ID_PARTITION)
        snprintf(line, sizeof(line), "PART %.15s @%08lx %luK", d->partition_label,
            (unsigned long)d->partition_address, (unsigned long)(d->partition_bytes / 1024U));
    else snprintf(line, sizeof(line), "PARTITION UNAVAILABLE");
    ESP_RETURN_ON_ERROR(device_info_line(214, line, UI_TEXT), TAG, "device partition");
    if (d->flags & CERBERUS_ID_FLASH)
        snprintf(line, sizeof(line), "FLASH %lu MB  C5 API %lu",
            (unsigned long)(d->flash_bytes / (1024U * 1024U)), (unsigned long)d->capability_version);
    else snprintf(line, sizeof(line), "FLASH SIZE UNAVAILABLE");
    ESP_RETURN_ON_ERROR(device_info_line(232, line, UI_TEXT), TAG, "device flash");
    if (d->flags & CERBERUS_ID_STA_MAC) cerberus_privacy_format_wifi_identifier(d->sta_mac, privacy_addresses_masked(), address, sizeof(address));
    else snprintf(address, sizeof(address), "UNAVAILABLE");
    snprintf(line, sizeof(line), "STA %-17s %s", address, !(d->flags & CERBERUS_ID_STA_MAC) ? "" :
        (d->flags & CERBERUS_ID_STA_CURRENT) ? "CURRENT" : "DEFAULT");
    ESP_RETURN_ON_ERROR(device_info_line(250, line, UI_TEXT), TAG, "device station");
    if (d->flags & CERBERUS_ID_AP_MAC) cerberus_privacy_format_wifi_identifier(d->ap_mac, privacy_addresses_masked(), address, sizeof(address));
    else snprintf(address, sizeof(address), "UNAVAILABLE");
    snprintf(line, sizeof(line), "AP  %-17s %s", address, !(d->flags & CERBERUS_ID_AP_MAC) ? "" :
        (d->flags & CERBERUS_ID_AP_CURRENT) ? "CURRENT" : "DEFAULT");
    ESP_RETURN_ON_ERROR(device_info_line(268, line, UI_TEXT), TAG, "device ap");
    ESP_RETURN_ON_ERROR(device_info_line(286, "ELF BUILD SHA256", UI_MUTED), TAG, "device hash label");
    for (unsigned row = 0; row < 2U; ++row) {
        for (unsigned i = 0; i < 16U; ++i) snprintf(line + i * 2U, sizeof(line) - i * 2U, "%02x", d->elf_sha256[row * 16U + i]);
        ESP_RETURN_ON_ERROR(device_info_line((uint16_t)(304U + row * 12U), line, UI_MUTED), TAG, "device hash");
    }
    return device_info_line(340, device_info_tab ? "C5 REPORTS THIS API REVISION" :
        "S3 EXPECTS THIS C5 API REVISION", UI_MUTED);
}

static esp_err_t device_info_draw_system(const cerberus_device_info_t *s)
{
    char line[50];
    ESP_RETURN_ON_ERROR(device_info_line(124, "CERBERUS / S3 + C5", UI_GOOD), TAG, "device board");
    snprintf(line, sizeof(line), "S3 RESET %s  UP %llu S", cerberus_reset_reason_label(s->health.reset_reason),
        (unsigned long long)(s->health.uptime_ms / 1000U));
    ESP_RETURN_ON_ERROR(device_info_line(146, line, UI_TEXT), TAG, "device health");
    snprintf(line, sizeof(line), "S3 RAM %uK FREE  %uK LOW", (unsigned)(s->health.free_internal_bytes / 1024U),
        (unsigned)(s->health.minimum_internal_bytes / 1024U));
    ESP_RETURN_ON_ERROR(device_info_line(168, line, UI_TEXT), TAG, "device ram");
    snprintf(line, sizeof(line), "PSRAM %uK FREE / %uK", (unsigned)(s->psram_free / 1024U), (unsigned)(s->psram_total / 1024U));
    ESP_RETURN_ON_ERROR(device_info_line(190, line, UI_TEXT), TAG, "device psram");
    if (s->c5_error == ESP_OK) snprintf(line, sizeof(line), "C5 RAM %uK FREE  %uK LOW  P%u", s->peer.free_heap_kib,
        s->peer.minimum_free_heap_kib, s->peer.protocol_version);
    else snprintf(line, sizeof(line), "C5 READ: %s", esp_err_to_name(s->c5_error));
    ESP_RETURN_ON_ERROR(device_info_line(212, line, UI_TEXT), TAG, "device peer");
    if (s->storage_error == ESP_OK && s->storage.mounted)
        snprintf(line, sizeof(line), "SD %llu / %llu MB FREE", (unsigned long long)(s->storage.free_bytes / (1024U * 1024U)),
            (unsigned long long)(s->storage.total_bytes / (1024U * 1024U)));
    else if (s->storage_error == ESP_OK) snprintf(line, sizeof(line), "SD NOT MOUNTED");
    else snprintf(line, sizeof(line), "SD READ: %s", esp_err_to_name(s->storage_error));
    ESP_RETURN_ON_ERROR(device_info_line(234, line, UI_TEXT), TAG, "device sd");
    if (s->battery.valid) snprintf(line, sizeof(line), "BATTERY %u%%  %u MV / GAUGE", s->battery.percent, s->battery.voltage_mv);
    else snprintf(line, sizeof(line), "BATTERY GAUGE UNAVAILABLE");
    ESP_RETURN_ON_ERROR(device_info_line(256, line, UI_TEXT), TAG, "device battery");
    if (s->connection_error == ESP_OK && s->connection.state == CERBERUS_WIFI_CONNECTION_CONNECTED)
        snprintf(line, sizeof(line), "C5 IP %u.%u.%u.%u  CH %u", s->connection.ipv4[0], s->connection.ipv4[1],
            s->connection.ipv4[2], s->connection.ipv4[3], s->connection.channel);
    else snprintf(line, sizeof(line), "C5 WIFI %s", s->connection_error == ESP_OK ? "NOT CONNECTED" : "STATUS UNAVAILABLE");
    ESP_RETURN_ON_ERROR(device_info_line(278, line, UI_TEXT), TAG, "device network");
    if (s->c5_error == ESP_OK) snprintf(line, sizeof(line), "C5 RADIO FLAGS %02X", s->peer.radio_state);
    else snprintf(line, sizeof(line), "C5 RADIO STATUS UNAVAILABLE");
    ESP_RETURN_ON_ERROR(device_info_line(300, line, UI_MUTED), TAG, "device radio");
    return device_info_line(340, "VALUES ARE FROM THIS EXPLICIT READ", UI_MUTED);
}

static esp_err_t device_info_draw(void)
{
    char line[50];
    ESP_RETURN_ON_ERROR(draw_header("Device info"), TAG, "device header");
    ESP_RETURN_ON_ERROR(draw_button(12, 60, 92, 40, "S3"), TAG, "device s3 tab");
    ESP_RETURN_ON_ERROR(draw_button(114, 60, 92, 40, "C5"), TAG, "device c5 tab");
    ESP_RETURN_ON_ERROR(draw_button(216, 60, 92, 40, "SYSTEM"), TAG, "device system tab");
    ESP_RETURN_ON_ERROR(cerberus_display_fill_rect((uint16_t)(12U + device_info_tab * 102U), 101, 92, 3, UI_GOOD), TAG, "device selected tab");
    if (device_info_error != ESP_OK) {
        snprintf(line, sizeof(line), "READ: %s", esp_err_to_name(device_info_error));
        ESP_RETURN_ON_ERROR(device_info_line(140, line, UI_BAD), TAG, "device start error");
    } else if (!device_info_ready) {
        ESP_RETURN_ON_ERROR(device_info_line(140, device_info_discard ? "FINISHING PREVIOUS READ" : "READING DEVICE INFORMATION", UI_MUTED), TAG, "device pending");
    } else if (device_info_tab == 2U) {
        ESP_RETURN_ON_ERROR(device_info_draw_system(device_info_snapshot), TAG, "device system");
    } else {
        ESP_RETURN_ON_ERROR(device_info_draw_identity(device_info_tab ? &device_info_snapshot->c5 : &device_info_snapshot->s3,
            device_info_tab ? device_info_snapshot->c5_error : device_info_snapshot->s3_error), TAG, "device identity");
    }
    if (device_info_ready) {
        int64_t now_us = esp_timer_get_time();
        uint64_t now = now_us >= 0 ? (uint64_t)now_us / 1000U : 0;
        if (now_us >= 0 && device_info_snapshot->sampled_ms && now >= device_info_snapshot->sampled_ms)
            snprintf(line, sizeof(line), "READ AGE %llu S", (unsigned long long)((now - device_info_snapshot->sampled_ms) / 1000U));
        else snprintf(line, sizeof(line), "READ AGE UNAVAILABLE");
        ESP_RETURN_ON_ERROR(device_info_line(354, line, UI_MUTED), TAG, "device age");
    }
    return draw_button(20, 374, 280, 36,
        device_info_job || device_info_requested || device_info_discard ? "READING..." : "REFRESH");
}

static void device_info_tap(uint16_t x, uint16_t y)
{
    if (y >= 60U && y < 100U) {
        for (uint8_t tab = 0; tab < 3U; ++tab)
            if (x >= 12U + tab * 102U && x < 104U + tab * 102U) device_info_tab = tab;
    } else if (x >= 20U && x < 300U && y >= 374U && y < 410U &&
               !device_info_job && !device_info_requested && !device_info_discard) {
        if (!device_info_snapshot) device_info_open();
        else { device_info_requested = true; device_info_ready = false; }
    }
}
