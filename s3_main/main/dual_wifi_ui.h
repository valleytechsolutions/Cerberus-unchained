#pragma once

/* Private ordinary Wi-Fi view. Include after common drawing/privacy helpers.
 * One UI task owns this PSRAM model. All taps use copied painted identities;
 * the coordinator's C5 rebind, not a displayed index, grants AP-tool access. */
#include "dual_wifi_scan.h"
#include "s3_wifi_scanner.h"

typedef enum {
    DUAL_WIFI_UI_NONE,
    DUAL_WIFI_UI_REDRAW,
    DUAL_WIFI_UI_BACK,
    DUAL_WIFI_UI_OPEN_AP_TOOLS,
    DUAL_WIFI_UI_UNSUPPORTED,
    DUAL_WIFI_UI_SAVE_ALL,
    DUAL_WIFI_UI_SAVE_SELECTED,
    DUAL_WIFI_UI_OPEN_PACKET_COUNT,
} dual_wifi_ui_action_t;

typedef struct {
    cerberus_dual_wifi_row_t rows[CERBERUS_DUAL_WIFI_CAPACITY];
    cerberus_dual_wifi_row_t painted[4], selected;
    cerberus_dual_wifi_status_t status;
    uint32_t generation, last_draw_ms;
    uint64_t save_mask; /* Historical file selection, never wifi_targets. */
    uint8_t indexes[CERBERUS_DUAL_WIFI_CAPACITY];
    uint8_t painted_indexes[4], selected_row;
    uint8_t count, filtered_count, page, painted_count, sort, band;
    bool cache_valid, paint_valid, selected_valid, detail, pending_start, pending_tools, dirty;
} dual_wifi_ui_store_t;

static dual_wifi_ui_store_t *dual_wifi_ui_store;
static bool dual_wifi_ui_opened;
static esp_err_t dual_wifi_ui_error;

static bool dual_wifi_ui_busy(const cerberus_dual_wifi_status_t *status)
{
    return status->cleanup_pending || status->rebinding ||
        status->state == CERBERUS_DUAL_WIFI_RUNNING ||
        status->state == CERBERUS_DUAL_WIFI_READING ||
        status->state == CERBERUS_DUAL_WIFI_STOPPING;
}

static bool dual_wifi_ui_inside(uint16_t x, uint16_t y, unsigned left, unsigned top,
    unsigned width, unsigned height)
{
    return x >= left && x < left + width && y >= top && y < top + height;
}

static const char *dual_wifi_ui_source(uint8_t source)
{
    return source == CERBERUS_DUAL_WIFI_SOURCE_S3 ? "S3" :
        source == CERBERUS_DUAL_WIFI_SOURCE_C5 ? "C5" : "UNKNOWN";
}

static const char *dual_wifi_ui_state(const cerberus_dual_wifi_status_t *status)
{
    if (status->rebinding) return "CHECKING SELECTED AP ON C5";
    if (status->cleanup_pending) return "RELEASING RADIOS";
    switch (status->state) {
    case CERBERUS_DUAL_WIFI_RUNNING: return "SCANNING";
    case CERBERUS_DUAL_WIFI_READING: return "READING RESULTS";
    case CERBERUS_DUAL_WIFI_STOPPING: return "STOPPING";
    case CERBERUS_DUAL_WIFI_COMPLETE: return "COMPLETE";
    case CERBERUS_DUAL_WIFI_ERROR: return "SCAN ERROR";
    default: return "READY";
    }
}

static void dual_wifi_ui_name(const cerberus_c5_wifi_result_t *ap, char *out, size_t capacity)
{
    cerberus_s3_wifi_scan_row_t row = {0};
    while (row.ssid_length < 32U && ap->ssid[row.ssid_length]) {
        row.ssid[row.ssid_length] = (uint8_t)ap->ssid[row.ssid_length];
        ++row.ssid_length;
    }
    (void)cerberus_s3_wifi_scanner_ssid_display(&row, out, capacity);
}

static int dual_wifi_ui_compare(const cerberus_dual_wifi_row_t *a,
    const cerberus_dual_wifi_row_t *b, uint8_t sort)
{
    int result = 0;
    if (sort == 1U) result = memcmp(a->ap.ssid, b->ap.ssid, 32U);
    else if (sort == 2U && a->ap.channel != b->ap.channel)
        result = a->ap.channel < b->ap.channel ? -1 : 1;
    if (result) return result;
    bool av = a->ap.rssi < 0 && a->ap.rssi >= -127;
    bool bv = b->ap.rssi < 0 && b->ap.rssi >= -127;
    if (av != bv) return av ? -1 : 1;
    if (av && a->ap.rssi != b->ap.rssi) return a->ap.rssi > b->ap.rssi ? -1 : 1;
    result = memcmp(a->ap.bssid, b->ap.bssid, 6U);
    if (result) return result;
    return a->source < b->source ? -1 : a->source > b->source ? 1 : 0;
}

static void dual_wifi_ui_indexes(dual_wifi_ui_store_t *store)
{
    store->filtered_count = 0U;
    for (uint8_t i = 0; i < store->count; ++i) {
        uint8_t channel = store->rows[i].ap.channel;
        if ((store->band == 1U && channel > 14U) || (store->band == 2U && channel <= 14U)) continue;
        uint8_t insert = store->filtered_count;
        while (insert && dual_wifi_ui_compare(&store->rows[i],
            &store->rows[store->indexes[insert - 1U]], store->sort) < 0) {
            store->indexes[insert] = store->indexes[insert - 1U];
            --insert;
        }
        store->indexes[insert] = i;
        ++store->filtered_count;
    }
    if ((unsigned)store->page * 4U >= store->filtered_count) store->page = 0U;
}

static void dual_wifi_ui_snapshot(dual_wifi_ui_store_t *store)
{
    cerberus_dual_wifi_scan_get_status(&store->status);
    if (store->detail || (store->cache_valid && store->generation == store->status.generation &&
        store->count == store->status.count)) return;
    store->cache_valid = store->paint_valid = false;
    store->filtered_count = 0U;
    if (store->status.count > CERBERUS_DUAL_WIFI_CAPACITY) {
        dual_wifi_ui_error = ESP_ERR_INVALID_RESPONSE;
        return;
    }
    /* Only this UI consumer calls coordinator.step; no bank publication can
     * interleave this bounded value-copy loop. A failed copy publishes no UI. */
    for (uint8_t i = 0U; i < store->status.count; ++i) {
        esp_err_t error = cerberus_dual_wifi_scan_get_row(i, &store->rows[i]);
        if (error != ESP_OK) { dual_wifi_ui_error = error; return; }
        const cerberus_dual_wifi_row_t *row = &store->rows[i];
        if ((row->source != CERBERUS_DUAL_WIFI_SOURCE_S3 && row->source != CERBERUS_DUAL_WIFI_SOURCE_C5) ||
            !row->ap.channel || (row->source == CERBERUS_DUAL_WIFI_SOURCE_S3 && row->ap.channel > 14U) ||
            row->ap.ssid[32] != '\0') {
            dual_wifi_ui_error = ESP_ERR_INVALID_RESPONSE;
            return;
        }
    }
    if (store->generation != store->status.generation) store->save_mask = 0U;
    store->generation = store->status.generation;
    store->count = store->status.count;
    store->cache_valid = true;
    dual_wifi_ui_indexes(store);
}

static esp_err_t dual_wifi_ui_enter(bool start_scan)
{
    dual_wifi_ui_opened = true;
    if (!dual_wifi_ui_store)
        dual_wifi_ui_store = heap_caps_calloc(1U, sizeof(*dual_wifi_ui_store),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    dual_wifi_ui_error = dual_wifi_ui_store ? ESP_OK : ESP_ERR_NO_MEM;
    if (!dual_wifi_ui_store) return dual_wifi_ui_error;
    dual_wifi_ui_store->pending_start = start_scan;
    dual_wifi_ui_store->pending_tools = false;
    dual_wifi_ui_store->detail = dual_wifi_ui_store->selected_valid = false;
    dual_wifi_ui_store->paint_valid = false;
    dual_wifi_ui_store->dirty = true;
    dual_wifi_ui_store->last_draw_ms = 0U;
    return ESP_OK;
}

static void dual_wifi_ui_leave(void)
{
    cerberus_dual_wifi_scan_stop();
    heap_caps_free(dual_wifi_ui_store);
    dual_wifi_ui_store = NULL;
    dual_wifi_ui_opened = false;
    dual_wifi_ui_error = ESP_OK;
}

static void dual_wifi_ui_set_error(esp_err_t error)
{
    dual_wifi_ui_error = error;
    if (dual_wifi_ui_store) {
        dual_wifi_ui_store->pending_start = dual_wifi_ui_store->pending_tools = false;
        dual_wifi_ui_store->dirty = true;
        dual_wifi_ui_store->last_draw_ms = 0U;
    }
}

static esp_err_t dual_wifi_ui_copy_selected(cerberus_dual_wifi_row_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!dual_wifi_ui_opened || !dual_wifi_ui_store || !dual_wifi_ui_store->selected_valid)
        return ESP_ERR_INVALID_STATE;
    *out = dual_wifi_ui_store->selected;
    return ESP_OK;
}

static dual_wifi_ui_action_t dual_wifi_ui_tick(uint32_t now_ms, bool visible, bool may_start)
{
    cerberus_dual_wifi_scan_step();
    if (!visible || !dual_wifi_ui_opened || !dual_wifi_ui_store) return DUAL_WIFI_UI_NONE;
    dual_wifi_ui_store_t *store = dual_wifi_ui_store;
    cerberus_dual_wifi_status_t status;
    cerberus_dual_wifi_scan_get_status(&status);
    bool busy = dual_wifi_ui_busy(&status);
    if (may_start && !busy && store->pending_tools) {
        store->pending_tools = false;
        return store->selected_valid ? DUAL_WIFI_UI_OPEN_AP_TOOLS : DUAL_WIFI_UI_NONE;
    }
    if (may_start && !busy && store->pending_start) {
        store->pending_start = false;
        dual_wifi_ui_error = cerberus_dual_wifi_scan_start();
        if (dual_wifi_ui_error == ESP_ERR_NOT_SUPPORTED) return DUAL_WIFI_UI_UNSUPPORTED;
        return DUAL_WIFI_UI_REDRAW;
    }
    bool changed = store->dirty || status.generation != store->status.generation ||
        status.count != store->status.count || status.state != store->status.state ||
        status.error != store->status.error || status.fallback != store->status.fallback ||
        status.split != store->status.split || status.cleanup_pending != store->status.cleanup_pending ||
        status.rebinding != store->status.rebinding;
    /* A completed retained page needs no periodic full LCD repaint. Busy
     * work remains responsive, and a state/publication change redraws once. */
    if ((busy || changed) && (uint32_t)(now_ms - store->last_draw_ms) >= 200U) {
        store->last_draw_ms = now_ms;
        return DUAL_WIFI_UI_REDRAW;
    }
    return DUAL_WIFI_UI_NONE;
}

static esp_err_t dual_wifi_ui_draw(void)
{
    dual_wifi_ui_store_t *store = dual_wifi_ui_store;
    cerberus_dual_wifi_status_t status;
    cerberus_dual_wifi_scan_get_status(&status);
    if (store) {
        store->paint_valid = false;
        store->painted_count = 0;
        dual_wifi_ui_snapshot(store);
    }
    bool detail = store && store->detail && store->selected_valid;
    bool busy = dual_wifi_ui_busy(&status);
    ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(0, 0, 320, 480, UI_BACKGROUND), TAG, "dual scan clear");
    ESP_RETURN_ON_ERROR(draw_header(detail ? "NETWORK DETAILS" : "WI-FI SCAN"), TAG, "dual scan header");
    if (!store) {
        ESP_RETURN_ON_ERROR(draw_centered(172, "SCAN VIEW UNAVAILABLE", UI_TEXT, 1), TAG, "dual scan no memory");
        ESP_RETURN_ON_ERROR(draw_centered(196, esp_err_to_name(dual_wifi_ui_error), UI_MUTED, 1), TAG, "dual scan view error");
    } else {
        char line[53];
        snprintf(line, sizeof(line), "%s", store->pending_tools ? "WAITING TO RELEASE RADIOS" :
            store->pending_start ? "WAITING FOR RADIOS" : dual_wifi_ui_state(&status));
        ESP_RETURN_ON_ERROR(draw_centered(54, line, UI_TEXT, 1), TAG, "dual scan state");
        if (detail) {
            const cerberus_dual_wifi_row_t *row = &store->selected;
            char name[49], address[18];
            dual_wifi_ui_name(&row->ap, name, sizeof(name));
            ESP_RETURN_ON_ERROR(draw_centered(78, name[0] ? name : "[HIDDEN SSID]", UI_TEXT, 1), TAG, "dual selected name");
            unsigned ssid_length = 0U;
            while (ssid_length < 32U && row->ap.ssid[ssid_length]) ++ssid_length;
            for (unsigned part = 0; part < 2U; ++part) {
                size_t used = (size_t)snprintf(line, sizeof(line), "SSID HEX %02u: ", part * 16U);
                for (unsigned i = part * 16U; i < (part + 1U) * 16U && i < ssid_length; ++i)
                    used += (size_t)snprintf(line + used, sizeof(line) - used, "%02X", (uint8_t)row->ap.ssid[i]);
                ESP_RETURN_ON_ERROR(draw_centered(98U + part * 16U, line, UI_MUTED, 1), TAG, "dual selected hex");
            }
            cerberus_privacy_format_wifi_identifier(row->ap.bssid, privacy_addresses_masked(), address, sizeof(address));
            ESP_RETURN_ON_ERROR(draw_centered(142, address, UI_TEXT, 2), TAG, "dual selected address");
            snprintf(line, sizeof(line), "%s / %s / CHANNEL %u", dual_wifi_ui_source(row->source),
                row->ap.channel <= 14U ? "2.4 GHz" : "5 GHz", row->ap.channel);
            ESP_RETURN_ON_ERROR(draw_centered(172, line, UI_TEXT, 1), TAG, "dual selected source");
            snprintf(line, sizeof(line), "%d dBm / %s", row->ap.rssi, wifi_security_text(row->ap.security));
            ESP_RETURN_ON_ERROR(draw_centered(194, line, UI_MUTED, 1), TAG, "dual selected signal");
            ESP_RETURN_ON_ERROR(draw_centered(226, "FROZEN SCAN RESULT / NOT A LIVE READING", UI_MUTED, 1), TAG, "dual selected snapshot");
            ESP_RETURN_ON_ERROR(draw_centered(246, "OPEN TOOLS RECHECKS THIS AP ON C5", UI_MUTED, 1), TAG, "dual selected handoff");
            esp_err_t error = dual_wifi_ui_error != ESP_OK ? dual_wifi_ui_error : status.error;
            if (error != ESP_OK) {
                snprintf(line, sizeof(line), "SCAN: %.40s", esp_err_to_name(error));
                ESP_RETURN_ON_ERROR(draw_centered(278, line, UI_BAD, 1), TAG, "dual selected error");
                if (status.rebind_cached)
                    ESP_RETURN_ON_ERROR(draw_centered(300, "C5 RESULT NOT VERIFIED / SCAN AGAIN",
                        UI_MUTED, 1), TAG, "dual selected recovery");
            }
            ESP_RETURN_ON_ERROR(draw_small_button(8, 326, 148, 44,
                status.rebinding ? "CHECKING AP" : "AP TOOLS"), TAG, "dual selected tools");
            ESP_RETURN_ON_ERROR(draw_small_button(164, 326, 148, 44, "PACKET COUNT"), TAG, "dual selected count");
            ESP_RETURN_ON_ERROR(draw_small_button(8, 374, 148, 44,
                store->save_mask & (UINT64_C(1) << store->selected_row) ? "UNMARK FOR SAVE" : "MARK FOR SAVE"), TAG, "dual saved mark");
            ESP_RETURN_ON_ERROR(draw_small_button(164, 374, 148, 44, "SAVE SELECTED"), TAG, "dual saved selected");
        } else {
            const char *mode = status.split ? "S3: 2.4 GHz + C5: 5 GHz" :
                "S3 + C5 / AUTOMATIC RADIO SELECTION";
            if (status.fallback) mode = status.s3_error == ESP_ERR_NO_MEM ?
                "C5: 2.4 + 5 GHz / S3 LOW MEMORY" : "C5: 2.4 + 5 GHz / S3 UNAVAILABLE";
            esp_err_t error = dual_wifi_ui_error != ESP_OK ? dual_wifi_ui_error : status.error;
            if (error != ESP_OK) snprintf(line, sizeof(line), "SCAN: %.40s", esp_err_to_name(error));
            else snprintf(line, sizeof(line), "%s", mode);
            ESP_RETURN_ON_ERROR(draw_centered(72, line, error ? UI_BAD : UI_MUTED, 1), TAG, "dual scan mode");
            static const char *const bands[] = {"BAND: ALL", "BAND: 2.4 GHz", "BAND: 5 GHz"};
            static const char *const sorts[] = {"SORT: SIGNAL", "SORT: NAME", "SORT: CHANNEL"};
            ESP_RETURN_ON_ERROR(draw_small_button(8, 90, 148, 32, bands[store->band]), TAG, "dual band");
            ESP_RETURN_ON_ERROR(draw_small_button(164, 90, 148, 32, sorts[store->sort]), TAG, "dual sort");
            unsigned marked = 0U;
            for (uint64_t bits = store->save_mask; bits; bits &= bits - 1U) ++marked;
            snprintf(line, sizeof(line), "%u RESULTS / PAGE %u/%u / SAVE %u%s", store->cache_valid ? store->filtered_count : 0U,
                store->page + 1U, store->filtered_count ? (store->filtered_count + 3U) / 4U : 1U,
                marked, busy && store->count ? " / RETAINED" : "");
            ESP_RETURN_ON_ERROR(draw_centered(132, line, UI_TEXT, 1), TAG, "dual scan count");
            if (store->cache_valid) {
                for (unsigned i = 0; i < 4U && (unsigned)store->page * 4U + i < store->filtered_count; ++i) {
                    cerberus_dual_wifi_row_t *row = &store->painted[i];
                    store->painted_indexes[i] = store->indexes[store->page * 4U + i];
                    *row = store->rows[store->painted_indexes[i]];
                    uint16_t y = (uint16_t)(150U + i * 54U);
                    char name[43];
                    dual_wifi_ui_name(&row->ap, name, sizeof(name));
                    ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(8, y, 304, 50, UI_PANEL), TAG, "dual scan row");
                    snprintf(line, sizeof(line), "%.36s", name[0] ? name : "[HIDDEN SSID]");
                    ESP_RETURN_ON_ERROR(cerberus_display_draw_text(14, y + 5U, line, UI_TEXT, UI_PANEL, 1), TAG, "dual scan name");
                    snprintf(line, sizeof(line), "%s / %s CH%u / %d dBm", dual_wifi_ui_source(row->source),
                        row->ap.channel <= 14U ? "2.4G" : "5G", row->ap.channel, row->ap.rssi);
                    ESP_RETURN_ON_ERROR(cerberus_display_draw_text(14, y + 22U, line, UI_MUTED, UI_PANEL, 1), TAG, "dual scan source");
                    snprintf(line, sizeof(line), "%s / TAP FOR DETAILS", wifi_security_text(row->ap.security));
                    ESP_RETURN_ON_ERROR(cerberus_display_draw_text(14, y + 36U, line, UI_MUTED, UI_PANEL, 1), TAG, "dual scan security");
                    ESP_RETURN_ON_ERROR(draw_small_button(276, y + 11U, 30, 28,
                        store->save_mask & (UINT64_C(1) << store->painted_indexes[i]) ? "X" : "+"), TAG, "dual save checkbox");
                    ++store->painted_count;
                }
            }
            if (!store->painted_count)
                ESP_RETURN_ON_ERROR(draw_centered(218, "NO NETWORKS IN THIS VIEW", UI_MUTED, 1), TAG, "dual scan empty");
            ESP_RETURN_ON_ERROR(draw_small_button(8, 374, 68, 44, "PREV"), TAG, "dual scan previous");
            ESP_RETURN_ON_ERROR(draw_small_button(84, 374, 68, 44, "NEXT"), TAG, "dual scan next");
            ESP_RETURN_ON_ERROR(draw_small_button(160, 374, 68, 44, "SAVE ALL"), TAG, "dual save all");
            ESP_RETURN_ON_ERROR(draw_small_button(236, 374, 76, 44, "SAVE SEL"), TAG, "dual save selection");
        }
    }
    ESP_RETURN_ON_ERROR(draw_button(8, 426, 148, 44, detail ? "BACK TO LIST" : "BACK"), TAG, "dual scan back");
    ESP_RETURN_ON_ERROR(draw_button(164, 426, 148, 44,
        busy || (store && store->pending_start) ? "STOP" : "SCAN AGAIN"), TAG, "dual scan start stop");
    if (store && !detail && store->cache_valid) store->paint_valid = true;
    if (store) store->dirty = false;
    return ESP_OK;
}

static dual_wifi_ui_action_t dual_wifi_ui_tap(uint16_t x, uint16_t y)
{
    dual_wifi_ui_store_t *store = dual_wifi_ui_store;
    cerberus_dual_wifi_status_t status;
    cerberus_dual_wifi_scan_get_status(&status);
    bool busy = dual_wifi_ui_busy(&status);
    if (dual_wifi_ui_inside(x, y, 8, 426, 148, 44)) {
        if (store && store->detail) {
            if (status.rebinding || store->pending_tools) cerberus_dual_wifi_scan_stop();
            store->detail = store->selected_valid = store->paint_valid = store->pending_tools = false;
            return DUAL_WIFI_UI_REDRAW;
        }
        if (store) store->pending_start = store->pending_tools = false;
        cerberus_dual_wifi_scan_stop();
        return DUAL_WIFI_UI_BACK;
    }
    if (dual_wifi_ui_inside(x, y, 164, 426, 148, 44)) {
        if (busy || (store && store->pending_start)) {
            if (store) store->pending_start = store->pending_tools = false;
            cerberus_dual_wifi_scan_stop();
        } else if (!store) (void)dual_wifi_ui_enter(true);
        else {
            store->detail = store->selected_valid = store->pending_tools = false;
            store->pending_start = true;
            dual_wifi_ui_error = ESP_OK;
        }
        return DUAL_WIFI_UI_REDRAW;
    }
    if (!store) return DUAL_WIFI_UI_NONE;
    if (store->detail) {
        if (store->selected_valid && y >= 374U && y < 418U) {
            if (x >= 8U && x < 156U) {
                store->save_mask ^= UINT64_C(1) << store->selected_row;
                return DUAL_WIFI_UI_REDRAW;
            }
            if (x >= 164U && x < 312U && store->save_mask) return DUAL_WIFI_UI_SAVE_SELECTED;
        }
        if (store->selected_valid && !status.rebinding && !store->pending_tools &&
            dual_wifi_ui_inside(x, y, 164, 326, 148, 44)) return DUAL_WIFI_UI_OPEN_PACKET_COUNT;
        if (store->selected_valid && !status.rebinding && !store->pending_tools &&
            dual_wifi_ui_inside(x, y, 8, 326, 148, 44)) {
            store->pending_start = false;
            store->pending_tools = true;
            if (busy) cerberus_dual_wifi_scan_stop();
            return DUAL_WIFI_UI_REDRAW;
        }
        return DUAL_WIFI_UI_NONE;
    }
    if (dual_wifi_ui_inside(x, y, 8, 90, 148, 32)) {
        store->band = (uint8_t)((store->band + 1U) % 3U);
        store->page = 0;
        store->paint_valid = false;
        dual_wifi_ui_indexes(store);
        return DUAL_WIFI_UI_REDRAW;
    }
    if (dual_wifi_ui_inside(x, y, 164, 90, 148, 32)) {
        store->sort = (uint8_t)((store->sort + 1U) % 3U);
        store->page = 0;
        store->paint_valid = false;
        dual_wifi_ui_indexes(store);
        return DUAL_WIFI_UI_REDRAW;
    }
    if (store->cache_valid && dual_wifi_ui_inside(x, y, 160, 374, 68, 44))
        return DUAL_WIFI_UI_SAVE_ALL;
    if (store->cache_valid && store->save_mask && dual_wifi_ui_inside(x, y, 236, 374, 76, 44))
        return DUAL_WIFI_UI_SAVE_SELECTED;
    if (dual_wifi_ui_inside(x, y, 8, 374, 68, 44) && store->page) {
        --store->page;
        store->paint_valid = false;
        return DUAL_WIFI_UI_REDRAW;
    }
    if (dual_wifi_ui_inside(x, y, 84, 374, 68, 44) &&
        (unsigned)(store->page + 1U) * 4U < store->filtered_count) {
        ++store->page;
        store->paint_valid = false;
        return DUAL_WIFI_UI_REDRAW;
    }
    if (store->paint_valid && x >= 8U && x < 312U && y >= 150U && y < 362U &&
        (y - 150U) % 54U < 50U) {
        uint8_t i = (uint8_t)((y - 150U) / 54U);
        if (i < store->painted_count) {
            if (x >= 276U && x < 306U && (y - 150U) % 54U >= 11U && (y - 150U) % 54U < 39U) {
                store->save_mask ^= UINT64_C(1) << store->painted_indexes[i];
                store->paint_valid = false; return DUAL_WIFI_UI_REDRAW;
            }
            store->selected = store->painted[i];
            store->selected_row = store->painted_indexes[i];
            store->selected_valid = store->detail = true;
            store->paint_valid = false;
            return DUAL_WIFI_UI_REDRAW;
        }
    }
    return DUAL_WIFI_UI_NONE;
}
