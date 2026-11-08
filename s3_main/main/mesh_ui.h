#pragma once

/* UI-task-owned, optional PSRAM only. The controller owns exact C5 cleanup;
 * rendering and selection never query a radio or resolve a live row index. */
#include "mesh_scan.h"
#include "mesh_locate.h"
#include "ble_archive_io.h"
#include "esp_heap_caps.h"

/* Defined by the historical viewer included immediately after this header. */
static esp_err_t mesh_archive_save_scan(const mesh_scan_t *scan, uint64_t *job);
static void mesh_archive_files(void);
static void mesh_archive_open_file(const char *path, const char *name);

#define MESH_UI_ROWS 4U
typedef struct {
    mesh_observation_selection_t value;
    mesh_observation_address_key_t source, destination;
} mesh_ui_painted_t;
typedef struct {
    mesh_scan_t scan;
    mesh_locate_t *locate;
    mesh_observation_address_key_t locate_target;
    uint64_t locate_origin;
    mesh_ui_painted_t painted[MESH_UI_ROWS], selected;
    uint64_t job, last_paint_ms;
    uint8_t mode, channel, group, page, count, painted_count, raw_page;
    bool detail, closing, return_tools, start_pending, save_pending, suppress_old;
    bool job_cancelled;
    bool locate_view, locate_pending, locate_return;
    bool locate_save_pending, job_locate;
    esp_err_t error;
    char message[48], saved_name[CERBERUS_STORAGE_NAME_MAX];
    char locate_message[48], locate_saved_name[CERBERUS_STORAGE_NAME_MAX];
} mesh_ui_store_t;
static mesh_ui_store_t *mesh_ui;
static esp_err_t mesh_ui_open_error;

static esp_err_t mesh_ui_exchange(void *context, uint8_t command, const void *request,
    size_t request_size, void *reply, size_t reply_size)
{ (void)context; return c5_spi_link_mesh_exchange(command, request, request_size, reply, reply_size); }
static int64_t mesh_ui_clock(void *context)
{ (void)context; return esp_timer_get_time(); }
static const mesh_scan_transport_t mesh_ui_transport = {
    .exchange = mesh_ui_exchange, .clock_us = mesh_ui_clock};
static esp_err_t mesh_ui_locate_exchange(void *context, uint8_t command, const void *request,
    size_t request_size, void *reply, size_t reply_size)
{ (void)context; return c5_spi_link_mesh_locate_exchange(command, request, request_size, reply, reply_size); }
static const mesh_scan_transport_t mesh_ui_locate_transport = {
    .exchange = mesh_ui_locate_exchange, .clock_us = mesh_ui_clock};
static esp_err_t mesh_ui_button(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
    const char *label, bool enabled)
{
    return cerberus_ui_widget_draw_button((cerberus_ui_rect_t){x, y, w, h},
        label, 1U, enabled, UI_WIDGET_THEME);
}

static bool mesh_ui_busy(void)
{ return mesh_ui && (!mesh_scan_released(&mesh_ui->scan) ||
    (mesh_ui->locate && !mesh_locate_released(mesh_ui->locate))); }
static bool mesh_ui_acquire(void)
{
    if (mesh_ui) return true;
    mesh_ui = heap_caps_calloc(1U, sizeof(*mesh_ui), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    mesh_ui_open_error = mesh_ui ? ESP_OK : ESP_ERR_NO_MEM;
    if (mesh_ui) { mesh_scan_init(&mesh_ui->scan); mesh_ui->channel = 11U; mesh_ui->mode = CERBERUS_MESH_OBSERVATION_SWEEP; }
    return mesh_ui != NULL;
}
static void mesh_ui_open(void)
{
    ieee802154_mode = UI_IEEE802154_MODE_MESH;
    if (mesh_ui_acquire()) { mesh_ui->detail = false; mesh_ui->painted_count = 0U; }
}
static void mesh_ui_close(bool tools)
{
    if (!mesh_ui) { if (tools) ieee802154_mode = UI_IEEE802154_MODE_MENU; return; }
    mesh_ui_store_t *s = mesh_ui;
    s->closing = true; s->return_tools |= tools;
    s->start_pending = s->save_pending = false; s->painted_count = 0U;
    s->locate_pending = false;
    s->locate_save_pending = false;
    mesh_scan_close(&s->scan);
    if (s->locate) mesh_locate_close(s->locate);
    if (s->job && !s->job_cancelled) {
        cerberus_ble_archive_io_cancel(s->job); s->job_cancelled = true;
    }
}
static void mesh_ui_back(void)
{
    if (mesh_ui && mesh_ui->locate_view && !mesh_ui->closing) {
        mesh_ui->locate_pending = false; mesh_ui->locate_return = true;
        mesh_ui->locate_save_pending = false;
        if (mesh_ui->job && mesh_ui->job_locate && !mesh_ui->job_cancelled) {
            cerberus_ble_archive_io_cancel(mesh_ui->job); mesh_ui->job_cancelled = true;
        }
        if (mesh_ui->scan.faulted) mesh_scan_close(&mesh_ui->scan);
        if (mesh_ui->locate) {
            if (mesh_ui->locate->faulted) mesh_locate_close(mesh_ui->locate);
            else mesh_locate_stop(mesh_ui->locate);
        }
    } else if (mesh_ui && mesh_ui->detail && !mesh_ui->closing) {
        mesh_ui->detail = false; mesh_ui->painted_count = 0U;
    } else mesh_ui_close(true);
}
static bool mesh_ui_locate_allowed(const mesh_ui_store_t *s)
{
    return s && s->detail && !s->closing && !s->locate_view && !s->start_pending &&
        !s->save_pending && !s->job && !s->scan.faulted && s->selected.value.session &&
        s->selected.value.kind == MESH_OBSERVATION_SELECT_ADDRESS &&
        c5_status.firmware_version >= CERBERUS_MESH_LOCATE_MIN_FIRMWARE;
}
static void mesh_ui_locate_start(void)
{
    mesh_ui_store_t *s = mesh_ui;
    if (!mesh_ui_locate_allowed(s) || s->locate) return;
    /* Allocate before changing survey intent. Failure preserves both the
     * current survey acquisition and its frozen painted target. */
    mesh_locate_t *locate = heap_caps_calloc(1U, sizeof(*locate), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!locate) { s->error = ESP_ERR_NO_MEM; return; }
    mesh_locate_init(locate);
    s->locate = locate; s->locate_target = s->selected.value.record.address.key;
    s->locate_origin = s->selected.value.session;
    s->locate_view = s->locate_pending = true; s->locate_return = false;
    s->locate_save_pending = false; s->locate_message[0] = s->locate_saved_name[0] = '\0';
    s->error = ESP_OK; s->painted_count = 0U;
    if (!mesh_scan_released(&s->scan)) mesh_scan_stop(&s->scan);
}
static void mesh_ui_start_stop(void)
{
    mesh_ui_store_t *s = mesh_ui;
    if (!s || s->closing || s->job || s->save_pending || s->locate_view) return;
    if (s->start_pending) { s->start_pending = false; return; }
    if (!mesh_scan_released(&s->scan)) {
        if (s->scan.faulted) mesh_scan_close(&s->scan);
        else mesh_scan_stop(&s->scan);
        return;
    }
    if (c5_status.firmware_version < CERBERUS_MESH_OBSERVATION_MIN_FIRMWARE) {
        s->error = ESP_ERR_NOT_SUPPORTED; return;
    }
    s->start_pending = true; s->suppress_old = true;
    s->painted_count = s->page = 0U; s->detail = false;
    s->error = ESP_OK; s->message[0] = s->saved_name[0] = '\0';
}
static void mesh_ui_tick(uint64_t now_ms, bool visible)
{
    mesh_ui_store_t *s = mesh_ui;
    if (!s) return;
    if (!visible) {
        s->return_tools = false;
        if (!s->closing) mesh_ui_close(false);
    }
    /* Let previous exact owners finish through their normal coordinator first.
     * Only after this barrier may our request suppress unrelated C5 polling. */
    if (s->start_pending && !s->closing && dual_wifi_live_released()) {
        s->error = mesh_scan_begin(&s->scan, s->mode,
            s->mode == CERBERUS_MESH_OBSERVATION_SWEEP ? UINT16_MAX :
            (uint16_t)(UINT16_C(1) << (s->channel - 11U)), 250U);
        s->start_pending = false;
    }
    bool survey_work = !mesh_scan_released(&s->scan);
    if (survey_work) mesh_scan_tick(&s->scan, now_ms, &mesh_ui_transport);
    if (s->scan.accepted && s->scan.model.session &&
        (s->scan.model.session == s->scan.request.session_id || s->scan.frozen)) s->suppress_old = false;
    /* Only one wire owner is ticked per UI pass, including the handoff pass. */
    if (s->locate_pending && !s->closing && !s->locate_return && mesh_scan_released(&s->scan)) {
        s->locate_pending = false;
        s->error = s->scan.frozen && s->scan.final_valid && !s->scan.faulted ?
            mesh_locate_begin(s->locate, &s->locate_target, s->locate_origin) : ESP_ERR_INVALID_STATE;
    } else if (s->locate_pending && s->scan.faulted) {
        s->locate_pending = false; s->error = s->scan.fault_error;
    }
    if (s->locate && !survey_work && !mesh_locate_released(s->locate))
        mesh_locate_tick(s->locate, now_ms, &mesh_ui_locate_transport);
    if (s->save_pending && !s->closing) {
        if (s->scan.faulted) { s->save_pending = false; s->error = s->scan.fault_error; }
        else if (mesh_scan_released(&s->scan)) {
            s->save_pending = false;
            s->error = s->scan.frozen && s->scan.final_valid && !s->suppress_old ?
                mesh_archive_save_scan(&s->scan, &s->job) : ESP_ERR_INVALID_STATE;
            if (s->error == ESP_OK) {
                s->job_cancelled = s->job_locate = false;
                snprintf(s->message, sizeof(s->message), "SAVING FROZEN MAP");
            }
        }
    }
    if (s->locate_save_pending && !s->closing && s->locate) {
        if (s->locate->faulted) { s->locate_save_pending = false; s->error = s->locate->fault_error; }
        else if (mesh_locate_released(s->locate)) {
            s->locate_save_pending = false;
            s->error = s->locate->frozen && s->locate->final_valid ?
                cerberus_ble_archive_io_save_mesh_locate(s->locate, &s->job) : ESP_ERR_INVALID_STATE;
            if (s->error == ESP_OK) {
                s->job_cancelled = false; s->job_locate = true;
                snprintf(s->locate_message, sizeof(s->locate_message), "SAVING LOCATE EVIDENCE");
            }
        }
    }
    if (s->job) {
        cerberus_ble_archive_io_status_t status;
        cerberus_ble_archive_io_get_status(&status);
        if (status.job_id != s->job) {
            s->error = ESP_ERR_NOT_FOUND;
            cerberus_ble_archive_io_release(s->job); s->job = 0U;
        } else if (!status.worker_active) {
            s->error = status.error;
            char *name = s->job_locate ? s->locate_saved_name : s->saved_name;
            char *message = s->job_locate ? s->locate_message : s->message;
            bool publish = !s->closing && !(s->job_locate && s->locate_return);
            if (publish && status.state == CERBERUS_BLE_ARCHIVE_IO_COMPLETE && status.error == ESP_OK &&
                (s->job_locate || status.result_ready) &&
                status.operation == (s->job_locate ? CERBERUS_BLE_ARCHIVE_IO_SAVE_MESH_LOCATE : CERBERUS_BLE_ARCHIVE_IO_SAVE_MESH_ARCHIVE) &&
                status.format == (s->job_locate ? CERBERUS_BLE_ARCHIVE_IO_FORMAT_MESH_LOCATE_REPORT : CERBERUS_BLE_ARCHIVE_IO_FORMAT_MESH_ARCHIVE)) {
                snprintf(name, sizeof(s->saved_name), "%s", status.file_name);
                snprintf(message, sizeof(s->message), "SAVED %.31s", status.file_name);
            } else if (publish) {
                if (s->error == ESP_OK) s->error = ESP_ERR_INVALID_RESPONSE;
                snprintf(message, sizeof(s->message), "SAVE FAILED");
            }
            cerberus_ble_archive_io_release(s->job); s->job = 0U;
        }
    }
    if (s->locate_return && mesh_scan_released(&s->scan) &&
        mesh_locate_released(s->locate) && !s->job) {
        heap_caps_free(s->locate); s->locate = NULL;
        s->locate_return = s->locate_view = false; s->detail = true;
    }
    if (s->closing && mesh_scan_released(&s->scan) && mesh_locate_released(s->locate) && !s->job) {
        bool tools = s->return_tools && visible;
        heap_caps_free(s->locate);
        heap_caps_free(s); mesh_ui = NULL;
        if (tools) ieee802154_mode = UI_IEEE802154_MODE_MENU;
        if (visible) ESP_ERROR_CHECK(draw_detail_page());
        return;
    }
    if (visible && (now_ms < s->last_paint_ms || now_ms - s->last_paint_ms >= 500U)) {
        s->last_paint_ms = now_ms; ESP_ERROR_CHECK(draw_detail_page());
    }
}

static void mesh_ui_pan(const mesh_observation_pan_key_t *key, char *text, size_t size)
{
    if (!key->known) snprintf(text, size, "PAN ? / CH %u", key->channel);
    else if (privacy_addresses_masked()) snprintf(text, size, "PAN **** / CH %u", key->channel);
    else snprintf(text, size, "PAN %04X / CH %u", key->value, key->channel);
}
static void mesh_ui_address(const mesh_observation_address_key_t *key, char *text, size_t size)
{
    if (privacy_addresses_masked()) snprintf(text, size, "%s MASKED",
        key->mode == MESH_MAC_ADDRESS_EXT64 ? "EXT64" : "SHORT16");
    else if (key->mode == MESH_MAC_ADDRESS_EXT64)
        snprintf(text, size, "E64 %016llX", (unsigned long long)key->value);
    else snprintf(text, size, "S16 %04X", (unsigned)key->value);
}
static void mesh_ui_edge_context(const mesh_ui_painted_t *p, char *text, size_t size)
{
    char a[5], b[5];
    if (!p->source.pan.known) snprintf(a, sizeof(a), "?");
    else if (privacy_addresses_masked()) snprintf(a, sizeof(a), "****");
    else snprintf(a, sizeof(a), "%04X", p->source.pan.value);
    if (!p->destination.pan.known) snprintf(b, sizeof(b), "?");
    else if (privacy_addresses_masked()) snprintf(b, sizeof(b), "****");
    else snprintf(b, sizeof(b), "%04X", p->destination.pan.value);
    snprintf(text, size, "PAN %s -> %s / CH %u", a, b, p->source.pan.channel);
}
static const mesh_observation_seen_t *mesh_ui_seen(const mesh_ui_painted_t *p)
{
    return p->value.kind == MESH_OBSERVATION_SELECT_PAN ? &p->value.record.pan.seen :
        p->value.kind == MESH_OBSERVATION_SELECT_ADDRESS ? &p->value.record.address.seen :
        &p->value.record.edge.seen;
}
/* Pure copied evidence renderer, shared with the historical map viewer. */
static esp_err_t mesh_ui_detail_value(const mesh_ui_painted_t *p, uint8_t raw_page,
    esp_err_t error, bool historical, bool locate_enabled)
{
    const mesh_observation_seen_t *seen = mesh_ui_seen(p);
    char line[64], a[24], b[24];
    bool address_detail = p->value.kind == MESH_OBSERVATION_SELECT_ADDRESS;
    bool address_action = address_detail && !historical;
    if (address_action)
        ESP_RETURN_ON_ERROR(mesh_ui_button(8, 376, 148, 36, "LOCATE",
            locate_enabled), TAG, "mesh locate action");
    if (error != ESP_OK) {
        snprintf(line, sizeof(line), "ERROR: %.35s", esp_err_to_name(error));
        ESP_RETURN_ON_ERROR(draw_centered(340, line, UI_BAD, 1), TAG, "mesh detail error");
    }
    ESP_RETURN_ON_ERROR(draw_centered(54, historical ? "HISTORICAL SELECTED OBSERVATION" : "FROZEN SELECTED OBSERVATION", UI_MUTED, 1), TAG, "mesh detail scope");
    if (p->value.kind == MESH_OBSERVATION_SELECT_PAN) {
        mesh_ui_pan(&p->value.record.pan.key, line, sizeof(line));
        ESP_RETURN_ON_ERROR(draw_centered(77, line, UI_TEXT, 2), TAG, "mesh PAN");
        snprintf(line, sizeof(line), "%s", !p->value.record.pan.key.known ? "PAN NOT PRESENT IN HEADER" :
            p->value.record.pan.origin_bits == (1U << MESH_MAC_PAN_EXPLICIT) ? "EXPLICIT HEADER PAN" :
            p->value.record.pan.origin_bits == (1U << MESH_MAC_PAN_COMPRESSED_SAME) ? "COMPRESSED-SAME HEADER PAN" :
            "EXPLICIT + COMPRESSED-SAME HEADER PAN");
    } else if (p->value.kind == MESH_OBSERVATION_SELECT_ADDRESS) {
        mesh_ui_address(&p->value.record.address.key, line, sizeof(line));
        ESP_RETURN_ON_ERROR(draw_centered(77, line, UI_TEXT, 2), TAG, "mesh address");
        mesh_ui_pan(&p->value.record.address.key.pan, line, sizeof(line));
    } else {
        mesh_ui_address(&p->source, a, sizeof(a)); mesh_ui_address(&p->destination, b, sizeof(b));
        ESP_RETURN_ON_ERROR(draw_centered(76, a, UI_TEXT, 2), TAG, "mesh source");
        snprintf(line, sizeof(line), "TO %s", b);
    }
    ESP_RETURN_ON_ERROR(draw_centered(100, line, UI_TEXT, 1), TAG, "mesh context");
    if (p->value.kind == MESH_OBSERVATION_SELECT_EDGE) {
        mesh_ui_edge_context(p, line, sizeof(line));
        ESP_RETURN_ON_ERROR(draw_centered(116, line, UI_MUTED, 1), TAG, "mesh edge PANs");
    }
    snprintf(line, sizeof(line), "FRAMES %lu / LAST SEQ %lu",
        (unsigned long)seen->frames, (unsigned long)seen->last_sequence);
    ESP_RETURN_ON_ERROR(draw_centered(132, line, UI_GOOD, 1), TAG, "mesh frame count");
    if (seen->first_time_valid && seen->last_time_valid)
        snprintf(line, sizeof(line), "C5 UPTIME %llu..%llu MS", (unsigned long long)(seen->first_us / 1000U),
            (unsigned long long)(seen->last_us / 1000U));
    else snprintf(line, sizeof(line), "CALLBACK TIME UNKNOWN / NOT UTC");
    ESP_RETURN_ON_ERROR(draw_centered(148, line, UI_MUTED, 1), TAG, "mesh time");
    if (p->value.kind == MESH_OBSERVATION_SELECT_ADDRESS) {
        const mesh_observation_address_t *r = &p->value.record.address;
        snprintf(line, sizeof(line), "AS SOURCE %lu / AS DESTINATION %lu",
            (unsigned long)r->source_frames, (unsigned long)r->destination_frames);
        ESP_RETURN_ON_ERROR(draw_centered(164, line, UI_TEXT, 1), TAG, "mesh roles");
        if (r->source_flags & MESH_OBSERVATION_RSSI_VALID)
            snprintf(line, sizeof(line), "LAST SOURCE %d DBM / SOURCE SEQ %lu", r->source_rssi,
                (unsigned long)r->last_source_sequence);
        else snprintf(line, sizeof(line), "SOURCE SIGNAL NOT AVAILABLE");
    } else if (p->value.kind == MESH_OBSERVATION_SELECT_EDGE) {
        ESP_RETURN_ON_ERROR(draw_centered(164, "OBSERVED ADDRESSING / NOT A ROUTE", UI_MUTED, 1), TAG, "mesh edge scope");
        const mesh_observation_edge_t *r = &p->value.record.edge;
        if (r->last_signal_flags & MESH_OBSERVATION_RSSI_VALID)
            snprintf(line, sizeof(line), "LAST SOURCE %d DBM / TYPE %u", r->last_source_rssi, r->last_frame_type);
        else snprintf(line, sizeof(line), "SOURCE SIGNAL NOT AVAILABLE");
    } else snprintf(line, sizeof(line), "A CONTEXT MAY CONTAIN MULTIPLE ADDRESSES");
    ESP_RETURN_ON_ERROR(draw_centered(182, line, UI_MUTED, 1), TAG, "mesh signal");
    snprintf(line, sizeof(line), "SESSION %016llX", (unsigned long long)p->value.session);
    ESP_RETURN_ON_ERROR(draw_centered(198, line, UI_MUTED, 1), TAG, "mesh session");
    if (!p->value.witness_valid)
        return draw_centered(263, "LATEST RAW WITNESS NOT RETAINED", UI_BAD, 1);
    const mesh_observation_input_t *raw = &p->value.witness.input;
    snprintf(line, sizeof(line), "RAW MAC %u BYTES / OBSERVATION SEQ %lu", raw->length,
        (unsigned long)raw->sequence);
    ESP_RETURN_ON_ERROR(draw_centered(224, line, UI_TEXT, 1), TAG, "mesh raw scope");
    ESP_RETURN_ON_ERROR(draw_centered(242, "LATEST ADDRESSING / MAY BE DESTINATION", UI_MUTED, 1), TAG, "mesh raw role");
    if (privacy_addresses_masked())
        return draw_centered(280, "RAW BYTES HIDDEN / PRIVACY ENABLED", UI_MUTED, 1);
    for (unsigned row = 0U; row < 4U; ++row) {
        unsigned offset = raw_page * 64U + row * 16U;
        if (offset >= raw->length) break;
        size_t used = 0U;
        for (unsigned i = offset; i < raw->length && i < offset + 16U; ++i)
            used += (size_t)snprintf(line + used, sizeof(line) - used, "%02X%s", raw->mac[i],
                i + 1U < raw->length && i < offset + 15U ? " " : "");
        ESP_RETURN_ON_ERROR(draw_centered((uint16_t)(266U + row * 20U), line, UI_TEXT, 1), TAG, "mesh raw");
    }
    snprintf(line, sizeof(line), "RAW PAGE %u / %u", raw_page + 1U, (raw->length + 63U) / 64U);
    ESP_RETURN_ON_ERROR(draw_centered(353, line, UI_MUTED, 1), TAG, "mesh raw page");
    return raw->length > 64U ? draw_small_button(address_action ? 164U : 80U, 376,
        address_action ? 148U : 160U, 36, "NEXT RAW PAGE") : ESP_OK;
}

static esp_err_t mesh_ui_detail_draw(const mesh_ui_store_t *s)
{
    return mesh_ui_detail_value(&s->selected, s->raw_page, s->error, false,
        mesh_ui_locate_allowed(s));
}

static void mesh_ui_locate_toggle(void)
{
    mesh_ui_store_t *s = mesh_ui;
    if (!s || !s->locate || s->closing || s->locate_return || s->job || s->locate_save_pending) return;
    if (s->locate_pending) { s->locate_pending = false; return; }
    if (s->scan.faulted && !mesh_scan_released(&s->scan)) { mesh_scan_close(&s->scan); return; }
    if (!mesh_locate_released(s->locate)) {
        if (s->locate->faulted) mesh_locate_close(s->locate);
        else mesh_locate_stop(s->locate);
    } else if (mesh_scan_released(&s->scan) && s->scan.frozen && !s->scan.faulted) {
        s->error = mesh_locate_begin(s->locate, &s->locate_target, s->locate_origin);
        if (s->error == ESP_OK) s->locate_saved_name[0] = s->locate_message[0] = '\0';
    }
}
static void mesh_ui_locate_save(void)
{
    mesh_ui_store_t *s = mesh_ui;
    if (!s || !s->locate || s->closing || s->locate_pending || s->locate_return ||
        s->job || s->locate_save_pending || s->locate->faulted) return;
    if (s->locate_saved_name[0]) {
        char name[CERBERUS_STORAGE_NAME_MAX]; memcpy(name, s->locate_saved_name, sizeof(name));
        mesh_ui_close(false); current_page = UI_PAGE_FILES;
        snprintf(file_path, sizeof(file_path), "%s", CERBERUS_STORAGE_MESH_REPORT_PATH);
        open_text_file(name);
    } else if (s->locate->accepted || s->locate->frozen) {
        s->locate_save_pending = true; s->error = ESP_OK; mesh_locate_stop(s->locate);
    }
}
static esp_err_t mesh_ui_locate_draw(const mesh_ui_store_t *s)
{
    const mesh_locate_t *l = s->locate;
    if (!l) return draw_centered(180, "LOCATE VIEW UNAVAILABLE", UI_BAD, 1);
    bool held = !mesh_locate_released(l);
    const char *state = s->closing || s->locate_return ? "STOPPING / RETURN TO SURVEY" :
        s->scan.faulted || l->faulted ? "FAULT / CLOSE REQUIRED" :
        s->locate_pending ? "STOPPING SURVEY / PRESERVING EVIDENCE" :
        l->stop_requested && held ? "STOPPING SELECTED-ADDRESS LOCATE" :
        l->have_control && (l->remote.flags & CERBERUS_MESH_LOCATE_RX_ACTIVE) ? "C5 RECEIVING / SELECTED SOURCE" :
        held ? "STARTING SELECTED-ADDRESS LOCATE" : l->frozen ? "LOCATE STOPPED / RETAINED EVIDENCE" : "LOCATE IDLE / TAP START";
    ESP_RETURN_ON_ERROR(draw_centered(52, state, UI_MUTED, 1), TAG, "locate state");
    char line[64]; mesh_ui_address(&s->locate_target, line, sizeof(line));
    ESP_RETURN_ON_ERROR(draw_centered(76, line, UI_TEXT, 2), TAG, "locate target");
    mesh_ui_pan(&s->locate_target.pan, line, sizeof(line));
    ESP_RETURN_ON_ERROR(draw_centered(100, line, UI_TEXT, 1), TAG, "locate PAN");
    ESP_RETURN_ON_ERROR(draw_centered(116, s->locate_target.pan.known ?
        "HEADER MATCH / NOT AUTHENTICATED IDENTITY" : "UNKNOWN PAN / AMBIGUOUS ADDRESS CONTEXT",
        s->locate_target.pan.known ? UI_MUTED : UI_BAD, 1), TAG, "locate ambiguity");
    const cerberus_mesh_locate_projection_t *p = &l->snapshot;
    bool matched = l->has_snapshot && p->match_sequence;
    if (matched && (p->flags & CERBERUS_MESH_LOCATE_RSSI_VALID))
        snprintf(line, sizeof(line), "%d DBM", p->rssi);
    else snprintf(line, sizeof(line), "RSSI --");
    ESP_RETURN_ON_ERROR(draw_centered(141, line, UI_TEXT, 3), TAG, "locate RSSI");
    if (matched && (p->flags & CERBERUS_MESH_LOCATE_LQI_VALID))
        snprintf(line, sizeof(line), "SOURCE LQI %u / MATCH SEQ %lu", p->lqi, (unsigned long)p->match_sequence);
    else snprintf(line, sizeof(line), "%s", matched ? "SOURCE LQI UNKNOWN" : "NOT YET HEARD AS SOURCE");
    ESP_RETURN_ON_ERROR(draw_centered(171, line, UI_MUTED, 1), TAG, "locate LQI");
    uint64_t age = 0U;
    bool age_valid = mesh_locate_age_upper(l, esp_timer_get_time(), &age);
    bool stale = age_valid && p->copied_us >= p->last_match_us && p->copied_us - p->last_match_us > UINT64_C(3000000);
    if (age_valid) {
        uint64_t unit = age < UINT64_C(1000000000) ? 1000U : 1000000U;
        snprintf(line, sizeof(line), "SOURCE AGE <= %llu %s", (unsigned long long)(age / unit + (age % unit != 0U)),
            unit == 1000U ? "MS" : "S");
    }
    else snprintf(line, sizeof(line), "SOURCE AGE UNKNOWN / NOT DELIVERY AGE");
    ESP_RETURN_ON_ERROR(draw_centered(190, line, stale ? UI_BAD : UI_MUTED, 1), TAG, "locate source age");
    snprintf(line, sizeof(line), "MATCHES %lu / DELIVERED SAMPLES %lu",
        (unsigned long)(l->has_snapshot ? p->matches : 0U), (unsigned long)l->samples);
    ESP_RETURN_ON_ERROR(draw_centered(209, line, UI_GOOD, 1), TAG, "locate counts");
    ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(36, 235, 264, 66, UI_PANEL), TAG, "locate graph");
    ESP_RETURN_ON_ERROR(cerberus_display_draw_text(8, 235, "-20", UI_MUTED, UI_BACKGROUND, 1), TAG, "locate graph maximum");
    ESP_RETURN_ON_ERROR(cerberus_display_draw_text(8, 294, "-100", UI_MUTED, UI_BACKGROUND, 1), TAG, "locate graph minimum");
    for (uint8_t i = 0U; i < l->history_count; ++i) {
        mesh_locate_sample_t sample;
        if (!mesh_locate_history_get(l, i, &sample)) return ESP_ERR_INVALID_RESPONSE;
        uint16_t x = (uint16_t)(40U + (MESH_LOCATE_HISTORY - l->history_count + i) * 8U);
        if (sample.flags & CERBERUS_MESH_LOCATE_RSSI_VALID) {
            int value = sample.rssi < -100 ? -100 : sample.rssi > -20 ? -20 : sample.rssi;
            uint16_t height = (uint16_t)(1U + (unsigned)(value + 100) * 60U / 80U);
            ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(x, (uint16_t)(300U - height), 5, height, UI_GOOD), TAG, "locate sample");
        } else ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(x, 299, 5, 1, UI_BAD), TAG, "locate missing signal");
    }
    ESP_RETURN_ON_ERROR(draw_centered(314, "SOURCE RSSI / LAST 32 DELIVERED SAMPLES", UI_MUTED, 1), TAG, "locate graph scope");
    snprintf(line, sizeof(line), "COALESCED MATCHES %lu / NOT RF LOSS", (unsigned long)l->coalesced_matches);
    ESP_RETURN_ON_ERROR(draw_centered(331, line, UI_MUTED, 1), TAG, "locate coalescing");
    snprintf(line, sizeof(line), "SURVEY %016llX PRESERVED", (unsigned long long)s->locate_origin);
    ESP_RETURN_ON_ERROR(draw_centered(348, line, UI_MUTED, 1), TAG, "locate origin");
    esp_err_t error = s->error != ESP_OK ? s->error : l->error;
    if (error != ESP_OK) snprintf(line, sizeof(line), "ERROR: %.35s", esp_err_to_name(error));
    else if (s->job || s->locate_save_pending) snprintf(line, sizeof(line), "%s",
        s->job ? "SD WRITING / PLEASE WAIT" : "STOPPING FOR FINAL LOCATE REPORT");
    else if (s->locate_message[0]) snprintf(line, sizeof(line), "%s", s->locate_message);
    else if (stale) snprintf(line, sizeof(line), "LAST SOURCE SAMPLE IS STALE");
    else if (age_valid && age > UINT64_C(3000000)) snprintf(line, sizeof(line), "FRESHNESS UNCONFIRMED / AGE IS A BOUND");
    else snprintf(line, sizeof(line), "FIXED CHANNEL / NO DISTANCE OR BEARING");
    ESP_RETURN_ON_ERROR(draw_centered(365, line, error != ESP_OK ? UI_BAD : UI_MUTED, 1), TAG, "locate error");
    bool survey_fault = s->scan.faulted && !mesh_scan_released(&s->scan);
    bool can_start = mesh_scan_released(&s->scan) && s->scan.frozen && s->scan.final_valid && !s->scan.faulted;
    ESP_RETURN_ON_ERROR(mesh_ui_button(8, 382, 148, 36, survey_fault ? "CLOSE SURVEY" : l->faulted && held ? "CLOSE" :
        s->locate_pending || held ? "STOP" : "START LOCATE", !s->closing && !s->locate_return && !s->job &&
        !s->locate_save_pending && (survey_fault || s->locate_pending || held || can_start)), TAG, "locate start stop");
    return mesh_ui_button(164, 382, 148, 36, s->job || s->locate_save_pending ? "SAVING" :
        s->locate_saved_name[0] ? "OPEN CSV" : held ? "STOP & SAVE" : "SAVE LOCATE",
        !s->closing && !s->locate_return && !s->locate_pending && !s->job && !s->locate_save_pending &&
        !l->faulted && (l->accepted || l->frozen));
}

static esp_err_t mesh_ui_rows_draw(const mesh_observation_t *m, uint8_t group,
    uint8_t page, uint8_t count, mesh_ui_painted_t rows[MESH_UI_ROWS], uint8_t *painted_out)
{
    char line[64];
    *painted_out = 0U;
    uint8_t painted = 0U;
    for (unsigned slot = 0U; slot < MESH_UI_ROWS && page * MESH_UI_ROWS + slot < count; ++slot) {
        uint8_t index = (uint8_t)(page * MESH_UI_ROWS + slot);
        mesh_ui_painted_t *p = &rows[slot];
        if (!mesh_observation_select(m, (mesh_observation_kind_t)group, index, &p->value)) return ESP_ERR_INVALID_RESPONSE;
        if (group == 2U) {
            const mesh_observation_edge_t *edge = &p->value.record.edge;
            if (edge->source >= m->address_count || edge->destination >= m->address_count) return ESP_ERR_INVALID_RESPONSE;
            p->source = m->addresses[edge->source].key; p->destination = m->addresses[edge->destination].key;
        }
        uint16_t y = (uint16_t)(145U + slot * 42U);
        ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(8, y, 304, 39, UI_PANEL), TAG, "mesh row");
        if (group == 0U) mesh_ui_pan(&p->value.record.pan.key, line, sizeof(line));
        else if (group == 1U) mesh_ui_address(&p->value.record.address.key, line, sizeof(line));
        else { char a[24], b[24]; mesh_ui_address(&p->source, a, sizeof(a)); mesh_ui_address(&p->destination, b, sizeof(b));
            snprintf(line, sizeof(line), "%s -> %s", a, b); }
        ESP_RETURN_ON_ERROR(cerberus_display_draw_text(14, y + 4U, line, UI_TEXT, UI_PANEL, 1), TAG, "mesh row identity");
        if (group == 1U) {
            mesh_ui_pan(&p->value.record.address.key.pan, line, sizeof(line));
            ESP_RETURN_ON_ERROR(cerberus_display_draw_text(14, y + 15U, line, UI_MUTED, UI_PANEL, 1), TAG, "mesh address PAN");
        } else if (group == 2U) {
            mesh_ui_edge_context(p, line, sizeof(line));
            ESP_RETURN_ON_ERROR(cerberus_display_draw_text(14, y + 15U, line, UI_MUTED, UI_PANEL, 1), TAG, "mesh edge limits");
        }
        snprintf(line, sizeof(line), "%lu FRAMES / %s RAW", (unsigned long)mesh_ui_seen(p)->frames,
            p->value.witness_valid ? "LATEST" : "NO LATEST");
        ESP_RETURN_ON_ERROR(cerberus_display_draw_text(14, y + 27U, line, UI_GOOD, UI_PANEL, 1), TAG, "mesh row facts");
        ++painted;
    }
    *painted_out = painted;
    return ESP_OK;
}

static esp_err_t mesh_ui_draw(void)
{
    mesh_ui_store_t *s = mesh_ui;
    if (s) s->painted_count = 0U;
    ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(0, 0, 320, 480, UI_BACKGROUND), TAG, "mesh clear");
    ESP_RETURN_ON_ERROR(draw_header("MESH DETECTION"), TAG, "mesh header");
    ESP_RETURN_ON_ERROR(draw_button(8, 430, 304, 42, s && s->closing ? "CLOSING" : "BACK"), TAG, "mesh back");
    if (!s) return draw_centered(182, mesh_ui_open_error == ESP_ERR_NO_MEM ?
        "NO PSRAM / BACK TO TOOLS" : "MESH VIEW UNAVAILABLE", UI_BAD, 1);
    if (s->locate_view) return mesh_ui_locate_draw(s);
    if (s->detail) return mesh_ui_detail_draw(s);
    bool held = !mesh_scan_released(&s->scan);
    bool settings = !held && !s->start_pending && !s->job && !s->save_pending && !s->closing;
    const char *state = s->closing ? "CLOSING / RELEASING C5" : s->scan.faulted ? "FAULT / CLOSE REQUIRED" :
        s->scan.stop_requested && held ? "STOPPING / DRAINING EVIDENCE" : s->start_pending ? "WAITING FOR PRIOR OWNER RELEASE" :
        held && !s->scan.accepted ? "STARTING C5 / NO NEW EVIDENCE YET" :
        held && (s->scan.remote.flags & CERBERUS_MESH_OBSERVATION_RX_ACTIVE) ? "C5 PASSIVE / RECEIVING" :
        held ? "C5 ACQUISITION / WAITING" : s->scan.frozen && !s->suppress_old ? "STOPPED / FROZEN EVIDENCE" : "IDLE / TAP START";
    ESP_RETURN_ON_ERROR(draw_centered(52, state, UI_MUTED, 1), TAG, "mesh state");
    char line[64];
    ESP_RETURN_ON_ERROR(mesh_ui_button(8, 70, 124, 30,
        s->mode == CERBERUS_MESH_OBSERVATION_FIXED ? "FIXED CHANNEL" : "SWEEP 11-26", settings), TAG, "mesh mode");
    snprintf(line, sizeof(line), s->mode == CERBERUS_MESH_OBSERVATION_FIXED ? "CH %u +" : "250MS DWELL", s->channel);
    ESP_RETURN_ON_ERROR(mesh_ui_button(140, 70, 88, 30, line,
        settings && s->mode == CERBERUS_MESH_OBSERVATION_FIXED), TAG, "mesh channel");
    ESP_RETURN_ON_ERROR(mesh_ui_button(236, 70, 76, 30, "SAVED", settings), TAG, "mesh saved");
    const char *tabs[] = {"PAN CONTEXTS", "ADDRESSES", "TRAFFIC"};
    for (unsigned i = 0U; i < 3U; ++i) {
        uint16_t x = (uint16_t)(8U + i * 104U);
        ESP_RETURN_ON_ERROR(draw_small_button(x, 108, 96, 30, tabs[i]), TAG, "mesh tab");
        if (s->group == i) ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(x + 2U, 135, 92, 2, UI_GOOD), TAG, "mesh underline");
    }
    const mesh_observation_t *m = &s->scan.model;
    s->count = s->suppress_old ? 0U : s->group == 0U ? m->pan_count : s->group == 1U ? m->address_count : m->edge_count;
    if (s->page * MESH_UI_ROWS >= s->count) s->page = 0U;
    uint8_t painted = 0U;
    ESP_RETURN_ON_ERROR(mesh_ui_rows_draw(m, s->group, s->page, s->count,
        s->painted, &painted), TAG, "mesh rows");
    if (!s->count) ESP_RETURN_ON_ERROR(draw_centered(220,
        s->suppress_old ? "WAITING FOR THIS SESSION'S EVIDENCE" : "NO RETAINED OBSERVATIONS", UI_MUTED, 1), TAG, "mesh empty");
    snprintf(line, sizeof(line), "PAN %u/16 / ADDR %u/64 / TRAFFIC %u/64", s->suppress_old ? 0U : m->pan_count,
        s->suppress_old ? 0U : m->address_count, s->suppress_old ? 0U : m->edge_count);
    ESP_RETURN_ON_ERROR(draw_centered(320, line, UI_MUTED, 1), TAG, "mesh capacity");
    if (s->scan.frozen && !s->suppress_old)
        snprintf(line, sizeof(line), "C5 QUEUE DROP %lu / INVALID %lu", (unsigned long)s->scan.final_summary.queue_drops,
            (unsigned long)s->scan.final_summary.invalid_callbacks);
    else snprintf(line, sizeof(line), "C5 LOSS TOTALS AVAILABLE AFTER STOP");
    ESP_RETURN_ON_ERROR(draw_centered(334, line, UI_MUTED, 1), TAG, "mesh queue losses");
    snprintf(line, sizeof(line), "OMIT P%lu A%lu E%lu", (unsigned long)(s->suppress_old ? 0U : m->counters.pan_omissions),
        (unsigned long)(s->suppress_old ? 0U : m->counters.address_omissions), (unsigned long)(s->suppress_old ? 0U : m->counters.edge_omissions) );
    ESP_RETURN_ON_ERROR(draw_centered(348, line, UI_MUTED, 1), TAG, "mesh omissions");
    esp_err_t error = s->error != ESP_OK ? s->error : s->scan.error;
    if (c5_status.firmware_version < CERBERUS_MESH_OBSERVATION_MIN_FIRMWARE)
        snprintf(line, sizeof(line), "C5 FIRMWARE %u REQUIRED", CERBERUS_MESH_OBSERVATION_MIN_FIRMWARE);
    else if (error != ESP_OK) snprintf(line, sizeof(line), "ERROR: %.35s", esp_err_to_name(error));
    else if (s->job || s->save_pending) snprintf(line, sizeof(line), "%s", s->job ? "SD WRITING / PLEASE WAIT" : "STOPPING FOR FINAL MAP");
    else if (s->message[0]) snprintf(line, sizeof(line), "%s", s->message);
    else if (m->counters.saturated && !s->suppress_old) snprintf(line, sizeof(line), "COUNTERS SATURATED / VALUES ARE BOUNDS");
    else if (m->counters.witness_omissions && !s->suppress_old) snprintf(line, sizeof(line), "RAW OMIT %lu / PARSE %lu+%lu",
        (unsigned long)m->counters.witness_omissions, (unsigned long)m->counters.malformed, (unsigned long)m->counters.unsupported);
    else snprintf(line, sizeof(line), "PARSE: %lu MALFORMED / %lu UNSUPPORTED", (unsigned long)(s->suppress_old ? 0U : m->counters.malformed),
        (unsigned long)(s->suppress_old ? 0U : m->counters.unsupported));
    ESP_RETURN_ON_ERROR(draw_centered(362, line, error ? UI_BAD : UI_MUTED, 1), TAG, "mesh notice");
    snprintf(line, sizeof(line), "PAGE %u / %u", s->page + 1U,
        s->count ? (s->count + MESH_UI_ROWS - 1U) / MESH_UI_ROWS : 1U);
    ESP_RETURN_ON_ERROR(draw_centered(374, line, UI_MUTED, 1), TAG, "mesh page");
    ESP_RETURN_ON_ERROR(mesh_ui_button(8, 382, 56, 36, "PREV", s->page > 0U), TAG, "mesh prev");
    ESP_RETURN_ON_ERROR(mesh_ui_button(72, 382, 56, 36, "NEXT", (s->page + 1U) * MESH_UI_ROWS < s->count), TAG, "mesh next");
    ESP_RETURN_ON_ERROR(mesh_ui_button(136, 382, 72, 36, s->scan.faulted && held ? "CLOSE" :
        s->start_pending || held ? (s->scan.stop_requested ? "STOPPING" : "STOP") : "START",
        !s->closing && !s->job && !s->save_pending), TAG, "mesh start stop");
    bool save_enabled = !s->closing && !s->job && !s->save_pending && !s->start_pending &&
        !s->scan.faulted && !s->suppress_old && (s->scan.accepted || s->scan.frozen);
    ESP_RETURN_ON_ERROR(cerberus_ui_widget_draw_button((cerberus_ui_rect_t){216, 382, 96, 36},
        s->job || s->save_pending ? "SAVING" : s->saved_name[0] ? "OPEN MAP" : held ? "STOP & SAVE" : "SAVE MAP",
        1U, save_enabled, UI_WIDGET_THEME), TAG, "mesh save");
    if (!s->closing && !s->save_pending && !s->job) s->painted_count = painted;
    return ESP_OK;
}
static void mesh_ui_tap(uint16_t x, uint16_t y)
{
    mesh_ui_store_t *s = mesh_ui;
    if (!s || s->closing) return;
    if (s->locate_view) {
        if (x >= 8U && x < 156U && y >= 382U && y < 418U) mesh_ui_locate_toggle();
        else if (x >= 164U && x < 312U && y >= 382U && y < 418U) mesh_ui_locate_save();
        return;
    }
    if (s->detail) {
        bool address = s->selected.value.kind == MESH_OBSERVATION_SELECT_ADDRESS;
        if (address && x >= 8U && x < 156U && y >= 376U && y < 412U) mesh_ui_locate_start();
        else if (x >= (address ? 164U : 80U) && x < (address ? 312U : 240U) && y >= 376U && y < 412U && s->selected.value.witness_valid &&
            s->selected.value.witness.input.length > 64U && !privacy_addresses_masked()) s->raw_page = (uint8_t)((s->raw_page + 1U) %
                ((s->selected.value.witness.input.length + 63U) / 64U));
        return;
    }
    bool idle = mesh_scan_released(&s->scan) && !s->start_pending && !s->job && !s->save_pending;
    if (y >= 70U && y < 100U) {
        if (idle && x >= 236U && x < 312U) {
            mesh_ui_close(false); mesh_archive_files();
        }
        else if (idle && x >= 8U && x < 132U) s->mode ^= 1U;
        else if (idle && x >= 140U && x < 228U && s->mode == CERBERUS_MESH_OBSERVATION_FIXED)
            s->channel = s->channel == 26U ? 11U : (uint8_t)(s->channel + 1U);
    } else if (y >= 108U && y < 138U) {
        for (unsigned i = 0U; i < 3U; ++i) if (x >= 8U + i * 104U && x < 104U + i * 104U) {
            s->group = (uint8_t)i; s->page = s->painted_count = 0U;
        }
    } else if (x >= 8U && x < 312U && y >= 145U && y < 313U) {
        unsigned slot = (y - 145U) / 42U;
        if (slot < s->painted_count && (y - 145U) % 42U < 39U) {
            s->selected = s->painted[slot]; s->detail = true;
            s->raw_page = s->painted_count = 0U;
        }
    } else if (y >= 382U && y < 418U) {
        if (x >= 8U && x < 64U && s->page) { --s->page; s->painted_count = 0U; }
        else if (x >= 72U && x < 128U && (s->page + 1U) * MESH_UI_ROWS < s->count) { ++s->page; s->painted_count = 0U; }
        else if (x >= 136U && x < 208U) mesh_ui_start_stop();
        else if (x >= 216U && x < 312U && !s->job && !s->save_pending && !s->scan.faulted) {
            if (s->saved_name[0]) {
                char name[CERBERUS_STORAGE_NAME_MAX]; memcpy(name, s->saved_name, sizeof(name));
                mesh_ui_close(false);
                mesh_archive_open_file(CERBERUS_STORAGE_MESH_ARCHIVE_PATH, name);
            } else if (!s->start_pending && !s->suppress_old && (s->scan.accepted || s->scan.frozen)) {
                s->save_pending = true; s->error = ESP_OK; s->painted_count = 0U;
                mesh_scan_stop(&s->scan);
            }
        }
    }
}
