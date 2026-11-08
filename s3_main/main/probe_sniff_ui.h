#pragma once
#include "probe_sniff_scan.h"
#include "ble_archive_io.h"
#include "esp_heap_caps.h"

#define PROBE_UI_ROWS 4U
typedef struct {
    cerberus_probe_sniff_projection_t row;
    cerberus_probe_sniff_group_view_t group;
    uint64_t session;
    bool frozen;
} probe_sniff_painted_t;
typedef struct {
    cerberus_probe_sniff_scan_t scan;
    probe_sniff_painted_t painted[PROBE_UI_ROWS], selected;
    uint8_t order[CERBERUS_PROBE_SNIFF_CAPACITY];
    uint8_t count, painted_count, page, group, mode, channel_index;
    uint64_t shown_publication, job, last_paint_ms;
    bool detail, closing, return_menu, save_requested, suppress_old;
    esp_err_t error;
    char message[48], saved_name[CERBERUS_STORAGE_NAME_MAX];
} probe_sniff_ui_store_t;
static probe_sniff_ui_store_t *probe_sniff_ui;
static esp_err_t probe_sniff_open_error;

static esp_err_t probe_ui_control(void *context, uint8_t command,
    const cerberus_probe_sniff_start_t *request, cerberus_probe_sniff_control_t *reply)
{ (void)context; return c5_spi_link_probe_sniff_control(command, request, reply); }
static esp_err_t probe_ui_read(void *context, const cerberus_probe_sniff_read_t *request,
    cerberus_probe_sniff_chunk_t *reply)
{ (void)context; return c5_spi_link_probe_sniff_read(request, reply); }
static int64_t probe_ui_clock(void *context) { (void)context; return esp_timer_get_time(); }
static const cerberus_probe_sniff_transport_t probe_ui_transport = {
    .control=probe_ui_control,.read=probe_ui_read,.clock_us=probe_ui_clock};

static bool probe_sniff_acquire(void)
{
    if (probe_sniff_ui) return true;
    probe_sniff_ui = heap_caps_calloc(1U, sizeof(*probe_sniff_ui), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    probe_sniff_open_error = probe_sniff_ui ? ESP_OK : ESP_ERR_NO_MEM;
    return probe_sniff_ui != NULL;
}
static void probe_sniff_open(void)
{
    wifi_mode = UI_WIFI_MODE_PROBE_SNIFF;
    if (probe_sniff_acquire()) {
        probe_sniff_ui->painted_count = 0U;
        probe_sniff_ui->detail = false;
    }
}
static void probe_sniff_close(bool menu)
{
    if (!probe_sniff_ui) { if (menu) wifi_mode = UI_WIFI_MODE_MENU; return; }
    probe_sniff_ui_store_t *s = probe_sniff_ui;
    s->closing = true; s->return_menu |= menu;
    s->save_requested = false; s->painted_count = 0U;
    cerberus_probe_sniff_scan_close(&s->scan);
    if (s->job) {
        cerberus_ble_archive_io_cancel(s->job); cerberus_ble_archive_io_release(s->job); s->job = 0U;
    }
}
static void probe_sniff_back(void)
{
    if (probe_sniff_ui && probe_sniff_ui->detail && !probe_sniff_ui->closing) {
        probe_sniff_ui->detail = false; probe_sniff_ui->painted_count = 0U;
    } else probe_sniff_close(true);
}

static void probe_sniff_start(void)
{
    if (!probe_sniff_acquire()) return;
    probe_sniff_ui_store_t *s = probe_sniff_ui;
    if (s->closing || s->job || s->save_requested) return;
    if (s->scan.requested || (s->scan.request.session_id &&
        (!s->scan.have_control || (s->scan.remote.flags & CERBERUS_PROBE_SNIFF_OWNED)))) {
        cerberus_probe_sniff_scan_stop(&s->scan); return;
    }
    if (c5_status.firmware_version < CERBERUS_PROBE_SNIFF_MIN_FIRMWARE) {
        s->error = ESP_ERR_NOT_SUPPORTED; return;
    }
    s->error = cerberus_probe_sniff_scan_begin(&s->scan, s->mode,
        s->mode == CERBERUS_PROBE_SNIFF_FIXED ? cerberus_probe_sniff_channel_at(s->channel_index) : 0U);
    if (s->error == ESP_OK) {
        s->suppress_old = true; s->painted_count = s->page = 0U;
        s->shown_publication = s->scan.publication;
        s->message[0] = '\0'; s->detail = false;
        s->saved_name[0] = '\0';
    }
}

static void probe_sniff_tick(uint64_t now_ms, bool visible)
{
    probe_sniff_ui_store_t *s = probe_sniff_ui;
    if (!s) return;
    if (!visible && !s->closing) probe_sniff_close(false);
    cerberus_probe_sniff_scan_tick(&s->scan, now_ms, &probe_ui_transport);
    if (s->save_requested && !s->closing) {
        bool retained = !s->suppress_old && !s->scan.requested &&
            !s->scan.restart_requested && !s->scan.request.session_id;
        if (s->scan.published.frozen && (retained ||
            s->scan.published.control.session_id == s->scan.request.session_id)) {
            s->error = cerberus_ble_archive_io_save_probe(&s->scan.published, &s->job);
            s->save_requested = false;
            if (s->error == ESP_OK) {
                snprintf(s->message, sizeof(s->message), "SAVING FROZEN EVIDENCE");
                cerberus_probe_sniff_scan_close(&s->scan);
            }
        } else if ((!s->scan.request.session_id && !s->scan.requested) ||
            (s->scan.have_control && !(s->scan.remote.flags & CERBERUS_PROBE_SNIFF_OWNED) &&
                !(s->scan.remote.flags & CERBERUS_PROBE_SNIFF_DATA_READY))) {
            s->save_requested = false;
            s->error = s->scan.error != ESP_OK ? s->scan.error : ESP_ERR_INVALID_STATE;
        }
    }
    if (s->job) {
        cerberus_ble_archive_io_status_t state;
        cerberus_ble_archive_io_get_status(&state);
        if (state.job_id != s->job) {
            s->error = ESP_ERR_NOT_FOUND; cerberus_ble_archive_io_release(s->job); s->job = 0U;
        } else if (!state.worker_active) {
            s->error = state.error;
            if (state.state == CERBERUS_BLE_ARCHIVE_IO_COMPLETE &&
                state.operation == CERBERUS_BLE_ARCHIVE_IO_SAVE_PROBE &&
                state.format == CERBERUS_BLE_ARCHIVE_IO_FORMAT_PROBE_REPORT) {
                snprintf(s->saved_name, sizeof(s->saved_name), "%s", state.file_name);
                snprintf(s->message, sizeof(s->message), "SAVED %.31s", state.file_name);
            } else {
                if (s->error == ESP_OK) s->error = ESP_ERR_INVALID_RESPONSE;
                snprintf(s->message, sizeof(s->message), "SAVE FAILED %.24s", state.file_name);
            }
            cerberus_ble_archive_io_release(s->job); s->job = 0U;
        }
    }
    if (s->closing && cerberus_probe_sniff_scan_released(&s->scan) && !s->job) {
        bool menu = s->return_menu && visible;
        heap_caps_free(s); probe_sniff_ui = NULL;
        if (menu) wifi_mode = UI_WIFI_MODE_MENU;
        if (visible) ESP_ERROR_CHECK(draw_detail_page());
        return;
    }
    if (s->scan.publication != s->shown_publication) {
        s->shown_publication = s->scan.publication; s->suppress_old = false;
        s->painted_count = 0U;
    }
    if (visible && (now_ms - s->last_paint_ms >= 250U)) {
        s->last_paint_ms = now_ms; ESP_ERROR_CHECK(draw_detail_page());
    }
}

static void probe_sniff_mac(const uint8_t mac[6], char text[18])
{
    if (privacy_addresses_masked()) snprintf(text, 18U, "**:**:**:**:**:**");
    else snprintf(text, 18U, "%02X:%02X:%02X:%02X:%02X:%02X",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
static bool probe_sniff_can_copy_name(const probe_sniff_ui_store_t *s)
{
    return s && s->detail && s->selected.frozen && s->scan.published.frozen &&
        s->selected.session == s->scan.published.control.session_id &&
        !s->closing && !s->job && !s->save_requested && !s->suppress_old &&
        !s->scan.requested && !s->scan.restart_requested &&
        (!s->scan.request.session_id || (s->scan.have_control &&
            !(s->scan.remote.flags & (CERBERUS_PROBE_SNIFF_OWNED | CERBERUS_PROBE_SNIFF_RECEIVING))));
}

static esp_err_t probe_sniff_detail_draw(const probe_sniff_ui_store_t *s)
{
    const probe_sniff_painted_t *p = &s->selected;
    const cerberus_probe_sniff_row_t *r = &p->row.row;
    char line[64], name[33], mac[18];
    probe_sniff_mac(r->transmitter, mac); cerberus_probe_sniff_name(r, name, sizeof(name));
    ESP_RETURN_ON_ERROR(draw_centered(57, p->frozen ? "FROZEN CAPTURE EVIDENCE" : "FROZEN LIVE SAMPLE", UI_MUTED, 1), TAG, "probe detail kind");
    ESP_RETURN_ON_ERROR(draw_centered(82, mac, UI_TEXT, 2), TAG, "probe detail address");
    snprintf(line, sizeof(line), "%s", name);
    ESP_RETURN_ON_ERROR(draw_centered(111, line, UI_GOOD, 1), TAG, "probe detail name");
    snprintf(line, sizeof(line), "SSID %u BYTES / EXACT HEX", r->ssid_length);
    ESP_RETURN_ON_ERROR(draw_centered(133, line, UI_MUTED, 1), TAG, "probe name length");
    for (uint8_t row = 0U; row < 2U; ++row) {
        size_t used = 0U;
        for (uint8_t i = (uint8_t)(row * 16U); i < r->ssid_length && i < (row + 1U) * 16U; ++i)
            used += (size_t)snprintf(line + used, sizeof(line) - used, "%02X%s", r->ssid[i],
                i + 1U < r->ssid_length && i % 16U != 15U ? " " : "");
        if (!used) snprintf(line, sizeof(line), "%s", row == 0U && !r->ssid_length ? "EMPTY / WILDCARD REQUEST" : "");
        ESP_RETURN_ON_ERROR(draw_centered((uint16_t)(153U + row * 16U), line, UI_TEXT, 1), TAG, "probe hex");
    }
    snprintf(line, sizeof(line), "LAST CH %u / %d DBM", r->last_channel, r->last_rssi);
    ESP_RETURN_ON_ERROR(draw_centered(199, line, UI_TEXT, 2), TAG, "probe last signal");
    snprintf(line, sizeof(line), "%s%lu CALLBACKS / %s%lu RETRY BIT",
        p->group.saturated ? ">=" : "", (unsigned long)p->group.frames,
        p->group.retry_frames == UINT32_MAX ? ">=" : "", (unsigned long)p->group.retry_frames);
    ESP_RETURN_ON_ERROR(draw_centered(227, line, UI_TEXT, 1), TAG, "probe counters");
    snprintf(line, sizeof(line), "%u RETAINED MAC + SSID PAIR%s", p->group.pairs, p->group.pairs == 1U ? "" : "S");
    ESP_RETURN_ON_ERROR(draw_centered(247, line, UI_MUTED, 1), TAG, "probe pairs");
    snprintf(line, sizeof(line), "FIRST %llu / LAST %llu MS",
        (unsigned long long)(p->group.first_callback_us / 1000U),
        (unsigned long long)(p->group.last_callback_us / 1000U));
    ESP_RETURN_ON_ERROR(draw_centered(273, line, UI_TEXT, 1), TAG, "probe times");
    ESP_RETURN_ON_ERROR(draw_centered(290, "C5 CALLBACK UPTIME / NOT UTC", UI_MUTED, 1), TAG, "probe time scope");
    snprintf(line, sizeof(line), "SESSION %016llX", (unsigned long long)p->session);
    ESP_RETURN_ON_ERROR(draw_centered(316, line, UI_MUTED, 1), TAG, "probe session");
    ESP_RETURN_ON_ERROR(draw_centered(347, s->error != ESP_OK && s->message[0] ? s->message :
        "REQUEST OBSERVATION / NOT ASSOCIATION", s->error != ESP_OK ? UI_BAD : UI_MUTED, 1), TAG, "probe scope");
    if (probe_sniff_can_copy_name(s)) return draw_button(48, 375, 224, 40, "COPY NAME TO LIST");
    return draw_centered(390, "STOP CAPTURE TO COPY A NAME", UI_MUTED, 1);
}

static esp_err_t probe_sniff_draw(void)
{
    probe_sniff_ui_store_t *s = probe_sniff_ui;
    if (s) s->painted_count = 0U;
    ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(0, 0, 320, 480, UI_BACKGROUND), TAG, "probe clear");
    ESP_RETURN_ON_ERROR(draw_header("PROBE SNIFF"), TAG, "probe header");
    ESP_RETURN_ON_ERROR(draw_button(8, 430, 304, 42, "BACK"), TAG, "probe back");
    if (!s) return draw_centered(185, probe_sniff_open_error == ESP_ERR_NO_MEM ?
        "NO MEMORY / RETURN AND RETRY" : "PROBE VIEW UNAVAILABLE", UI_BAD, 1);
    if (s->detail) return probe_sniff_detail_draw(s);
    bool held = s->scan.requested || (s->scan.request.session_id && (!s->scan.have_control ||
        (s->scan.remote.flags & CERBERUS_PROBE_SNIFF_OWNED)));
    const char *phase = s->closing || (s->scan.stop_requested && held) ? "RELEASING C5 RADIO" :
        s->scan.restart_requested ? "PREPARING NEXT CAPTURE" :
        s->scan.have_control && (s->scan.remote.flags & CERBERUS_PROBE_SNIFF_RECEIVING) ? "C5 PASSIVE / LISTENING" :
        s->scan.requested ? "STARTING C5 CAPTURE" : s->scan.published.frozen && !s->suppress_old ?
        (s->scan.published.control.error_code ? "STOPPED / SOURCE ERROR" : "STOPPED / FROZEN EVIDENCE") : "IDLE / TAP START";
    ESP_RETURN_ON_ERROR(draw_centered(53, phase, UI_MUTED, 1), TAG, "probe phase");
    char line[64];
    const char *modes[] = {"FIXED CHANNEL", "HOP 2.4 GHZ", "HOP 5 GHZ", "HOP BOTH"};
    ESP_RETURN_ON_ERROR(draw_small_button(8, 70, 196, 30, modes[s->mode]), TAG, "probe mode");
    snprintf(line, sizeof(line), s->mode == CERBERUS_PROBE_SNIFF_FIXED ? "CH %u +" : "250MS DWELL",
        cerberus_probe_sniff_channel_at(s->channel_index));
    ESP_RETURN_ON_ERROR(draw_small_button(212, 70, 100, 30, line), TAG, "probe channel");
    const char *tabs[] = {"REQUESTS", "SENDERS", "NETWORKS"};
    for (uint8_t i = 0U; i < 3U; ++i) {
        uint16_t x = (uint16_t)(8U + i * 104U);
        ESP_RETURN_ON_ERROR(draw_small_button(x, 108, 96, 30, tabs[i]), TAG, "probe tabs");
        if (i == s->group) ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(x+2U, 135, 92, 2, UI_GOOD), TAG, "probe tab underline");
    }
    const cerberus_probe_sniff_snapshot_t *bank = &s->scan.published;
    s->count = s->suppress_old ? 0U : cerberus_probe_sniff_order(bank,
        (cerberus_probe_sniff_group_t)s->group, s->order);
    if (s->page && s->page * PROBE_UI_ROWS >= s->count) s->page = 0U;
    uint8_t painted = 0U;
    for (uint8_t slot = 0U; slot < PROBE_UI_ROWS && s->page * PROBE_UI_ROWS + slot < s->count; ++slot) {
        uint8_t index = s->order[s->page * PROBE_UI_ROWS + slot];
        probe_sniff_painted_t *p = &s->painted[slot];
        if (!cerberus_probe_sniff_group_view(bank, (cerberus_probe_sniff_group_t)s->group, index, &p->group))
            return ESP_ERR_INVALID_RESPONSE;
        p->row = bank->rows[s->group == CERBERUS_PROBE_SNIFF_PAIRS ? index : p->group.latest];
        p->session = bank->control.session_id; p->frozen = bank->frozen;
        const cerberus_probe_sniff_row_t *r = &p->row.row;
        uint16_t y = (uint16_t)(145U + slot * 46U);
        ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(8, y, 304, 43, UI_PANEL), TAG, "probe row");
        char title[34], mac[18]; probe_sniff_mac(r->transmitter, mac);
        if (s->group == CERBERUS_PROBE_SNIFF_SENDERS) snprintf(title, sizeof(title), "%s", mac);
        else cerberus_probe_sniff_name(r, title, 24U);
        ESP_RETURN_ON_ERROR(cerberus_display_draw_text(14, y+3U, title, UI_TEXT, UI_PANEL, 2), TAG, "probe row title");
        if (s->group == CERBERUS_PROBE_SNIFF_PAIRS)
            snprintf(line, sizeof(line), "%s / CH %u / %d DBM", mac, r->last_channel, r->last_rssi);
        else snprintf(line, sizeof(line), "%u %s / LAST CH %u", p->group.pairs,
            s->group == CERBERUS_PROBE_SNIFF_SENDERS ? "NAMES" : "ADDRESSES", r->last_channel);
        ESP_RETURN_ON_ERROR(cerberus_display_draw_text(14, y+22U, line, UI_MUTED, UI_PANEL, 1), TAG, "probe row context");
        snprintf(line, sizeof(line), "%s%lu CALLBACKS / %s", p->group.saturated ? ">=" : "",
            (unsigned long)p->group.frames, p->frozen ? "FROZEN" : "SAMPLED");
        ESP_RETURN_ON_ERROR(cerberus_display_draw_text(14, y+33U, line, UI_GOOD, UI_PANEL, 1), TAG, "probe row counts");
        ++painted;
    }
    if (!s->count) ESP_RETURN_ON_ERROR(draw_centered(219,
        s->suppress_old || held ? "WAITING FOR A VALID READ CYCLE" : "NO RETAINED REQUESTS", UI_MUTED, 1), TAG, "probe empty");
    snprintf(line, sizeof(line), "%u/32 PAIRS / CAPACITY DROPS %s%lu", s->suppress_old ? 0U : bank->count,
        !s->suppress_old && bank->summary.capacity_dropped_frames == UINT32_MAX ? ">=" : "",
        (unsigned long)(s->suppress_old ? 0U : bank->summary.capacity_dropped_frames));
    ESP_RETURN_ON_ERROR(draw_centered(335, line, UI_MUTED, 1), TAG, "probe capacity");
    esp_err_t error = s->error != ESP_OK ? s->error : s->scan.error != ESP_OK ? s->scan.error :
        (!s->suppress_old ? bank->control.error_code : ESP_OK);
    if (c5_status.firmware_version < CERBERUS_PROBE_SNIFF_MIN_FIRMWARE)
        snprintf(line, sizeof(line), "C5 FIRMWARE %u REQUIRED", CERBERUS_PROBE_SNIFF_MIN_FIRMWARE);
    else if (error != ESP_OK) snprintf(line, sizeof(line), "ERROR: %.32s", esp_err_to_name(error));
    else if (s->save_requested) snprintf(line, sizeof(line), "STOPPING / READING FINAL EVIDENCE");
    else if (s->message[0]) snprintf(line, sizeof(line), "%s", s->message);
    else snprintf(line, sizeof(line), "SAVE RETAINS FULL MAC + SSID BYTES");
    ESP_RETURN_ON_ERROR(draw_centered(351, line, error != ESP_OK ? UI_BAD : UI_MUTED, 1), TAG, "probe message");
    ESP_RETURN_ON_ERROR(draw_small_button(8, 375, 68, 40, "PREV"), TAG, "probe previous");
    ESP_RETURN_ON_ERROR(draw_small_button(84, 375, 68, 40, "NEXT"), TAG, "probe next");
    ESP_RETURN_ON_ERROR(draw_small_button(160, 375, 68, 40,
        held ? (s->scan.stop_requested ? "STOPPING" : "STOP") : "START"), TAG, "probe start stop");
    ESP_RETURN_ON_ERROR(draw_small_button(236, 375, 76, 40,
        s->job || s->save_requested ? "SAVING" : s->saved_name[0] ? "OPEN FILE" : "SAVE"), TAG, "probe save");
    if (!s->closing && !s->save_requested && !s->job) s->painted_count = painted;
    return ESP_OK;
}

static void probe_sniff_tap(uint16_t x, uint16_t y)
{
    probe_sniff_ui_store_t *s = probe_sniff_ui;
    if (!s || s->closing) return;
    if (s->detail) {
        if (x >= 48U && x < 272U && y >= 375U && y < 415U && probe_sniff_can_copy_name(s)) {
            const cerberus_probe_sniff_row_t *row = &s->selected.row.row;
            s->error = ssid_lists_import_name(row->ssid, row->ssid_length, UI_PAGE_WIFI);
            if (s->error == ESP_OK) { probe_sniff_close(false); wifi_mode = UI_WIFI_MODE_MENU; }
            else snprintf(s->message, sizeof(s->message), "COPY NAME: %.31s", esp_err_to_name(s->error));
        }
        return;
    }
    bool held = s->scan.requested || (s->scan.request.session_id && (!s->scan.have_control ||
        (s->scan.remote.flags & CERBERUS_PROBE_SNIFF_OWNED)));
    if (y >= 70U && y < 100U && !held && !s->job && !s->save_requested) {
        if (x >= 8U && x < 204U) s->mode = (uint8_t)((s->mode + 1U) % 4U);
        else if (x >= 212U && x < 312U && s->mode == CERBERUS_PROBE_SNIFF_FIXED)
            s->channel_index = (uint8_t)((s->channel_index + 1U) % CERBERUS_PROBE_SNIFF_CHANNEL_COUNT);
    } else if (y >= 108U && y < 138U) {
        for (uint8_t i = 0U; i < 3U; ++i) if (x >= 8U+i*104U && x < 104U+i*104U) {
            s->group = i; s->page = 0U; s->painted_count = 0U;
        }
    } else if (x >= 8U && x < 312U && y >= 145U && y < 329U) {
        uint8_t row = (uint8_t)((y-145U)/46U);
        if (row < s->painted_count && (y-145U)%46U < 43U) {
            s->selected = s->painted[row]; s->detail = true; s->painted_count = 0U;
        }
    } else if (y >= 375U && y < 415U) {
        if (x >= 8U && x < 76U && s->page) { --s->page; s->painted_count = 0U; }
        else if (x >= 84U && x < 152U && (s->page+1U)*PROBE_UI_ROWS < s->count) { ++s->page; s->painted_count = 0U; }
        else if (x >= 160U && x < 228U) probe_sniff_start();
        else if (x >= 236U && x < 312U && !s->job && !s->save_requested) {
            if (s->saved_name[0]) {
                char name[CERBERUS_STORAGE_NAME_MAX]; memcpy(name, s->saved_name, sizeof(name));
                probe_sniff_close(false);
                current_page = UI_PAGE_FILES;
                snprintf(file_path, sizeof(file_path), "%s", CERBERUS_STORAGE_PROBE_REPORT_PATH);
                open_text_file(name);
            } else if (s->scan.requested || s->scan.request.session_id || s->scan.publication) {
                s->error = ESP_OK; s->save_requested = true; s->painted_count = 0U;
                cerberus_probe_sniff_scan_stop(&s->scan);
            }
        }
    }
}
