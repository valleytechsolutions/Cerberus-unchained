#pragma once

/* Private offline viewer. The enclosing UI task owns this PSRAM store and
 * serializes calls. No allocation, clock, filesystem, scanner or active BLE
 * API belongs here. Completed jobs publish value copies; failed jobs retain
 * the previous document. Historical source clocks are never made current. */
#include "ble_archive.h"
#include "ble_archive_list.h"
#include "cerberus_protocol.h"
#include "ble_receive_info_ui.h"
#include "ble_family_wire.h"
#include "ble_response.h"
#include "ble_family_hint_ui.h"

typedef enum {
    BLE_ARCHIVE_UI_JOB, BLE_ARCHIVE_UI_LIST, BLE_ARCHIVE_UI_DETAIL,
    BLE_ARCHIVE_UI_RAW, BLE_ARCHIVE_UI_ERROR,
} ble_archive_ui_view_t;
typedef enum {
    BLE_ARCHIVE_UI_NONE, BLE_ARCHIVE_UI_REDRAW,
    BLE_ARCHIVE_UI_BACK, BLE_ARCHIVE_UI_CANCEL, BLE_ARCHIVE_UI_EXPORT, BLE_ARCHIVE_UI_FIND_LIVE,
} ble_archive_ui_action_t;
typedef struct {
    cerberus_ble_archive_t document, incoming;
    cerberus_ble_archive_row_t painted[4], selected;
    uint64_t job_id, painted_saved_uptime_ms, selected_saved_uptime_ms;
    uint64_t painted_age_ms[4], selected_age_ms;
    uint64_t release_started_ms;
    bool painted_age_valid[4], selected_age_valid, raw_response, raw_hints, c5_response;
    bool job_pending, saving, has_document, paint_valid, selected_valid, release_pending;
    uint8_t page, raw_page, painted_count, hint_page, hint_pages;
    uint8_t order[CERBERUS_BLE_ARCHIVE_LIST_CAPACITY], filtered_count;
    cerberus_ble_archive_list_source_t source_filter;
    cerberus_ble_archive_list_sort_t sort;
    ble_archive_ui_view_t view;
    esp_err_t error;
    char file_name[256];
} ble_archive_ui_store_t;

/* Fail before an IO job was accepted, retaining the prior offline model.
 * Pending jobs use the token-bound fail/cancel path instead. */
static inline bool ble_archive_ui_error(ble_archive_ui_store_t *store, esp_err_t error)
{
    if (!store || store->job_pending || error == ESP_OK) return false;
    store->error = error;
    store->paint_valid = store->selected_valid = false;
    store->view = BLE_ARCHIVE_UI_ERROR;
    return true;
}

static inline bool ble_archive_ui_begin(ble_archive_ui_store_t *store,
    uint64_t job_id, bool saving)
{
    if (!store || !job_id || job_id <= store->job_id || store->job_pending) return false;
    store->job_id = job_id;
    store->saving = saving;
    store->job_pending = true;
    store->paint_valid = store->selected_valid = false;
    store->error = ESP_OK;
    store->view = BLE_ARCHIVE_UI_JOB;
    return true;
}

static inline bool ble_archive_ui_fail(ble_archive_ui_store_t *store,
    uint64_t job_id, esp_err_t error)
{
    if (!store || !store->job_pending || job_id != store->job_id || error == ESP_OK) return false;
    store->job_pending = false;
    store->paint_valid = store->selected_valid = false;
    store->error = error;
    store->view = BLE_ARCHIVE_UI_ERROR;
    return true;
}

static inline bool ble_archive_ui_complete(ble_archive_ui_store_t *store,
    uint64_t job_id, const cerberus_ble_archive_t *document, const char *file_name)
{
    if (!store || !store->job_pending || job_id != store->job_id) return false;
    if (!cerberus_ble_archive_validate(document) || !file_name) {
        (void)ble_archive_ui_fail(store, job_id, ESP_ERR_INVALID_RESPONSE);
        return false;
    }
    /* memmove permits the committed document itself as the SAVE result. */
    memmove(&store->document, document, sizeof(store->document));
    size_t i = 0U;
    for (; i + 1U < sizeof(store->file_name) && file_name[i]; ++i)
        store->file_name[i] = (uint8_t)file_name[i] >= 32U && (uint8_t)file_name[i] <= 126U ? file_name[i] : '.';
    store->file_name[i] = '\0';
    store->job_pending = false;
    store->has_document = true;
    store->paint_valid = store->selected_valid = false;
    store->page = store->raw_page = 0U;
    store->filtered_count = 0U;
    store->source_filter = CERBERUS_BLE_ARCHIVE_LIST_ALL;
    store->sort = CERBERUS_BLE_ARCHIVE_LIST_FILE;
    store->raw_response = false;
    store->raw_hints = false; store->c5_response = false; store->hint_page = store->hint_pages = 0U;
    store->error = ESP_OK;
    store->view = BLE_ARCHIVE_UI_LIST;
    return true;
}

static inline void ble_archive_ui_name(const cerberus_ble_archive_row_t *row, char out[33])
{
    size_t i = 0U;
    for (; i < 32U && row->name[i]; ++i)
        out[i] = (uint8_t)row->name[i] >= 32U && (uint8_t)row->name[i] <= 126U ? row->name[i] : '.';
    out[i] = '\0';
}

static inline const char *ble_archive_ui_receiver(uint8_t receiver)
{
    return receiver == CERBERUS_BLE_ARCHIVE_RECEIVER_S3 ? "S3" :
        receiver == CERBERUS_BLE_ARCHIVE_RECEIVER_C5 ? "C5" : "LEGACY";
}

static inline void ble_archive_ui_address(const cerberus_ble_archive_row_t *row, char out[18])
{
    cerberus_privacy_format_ble_identifier(row->address, privacy_addresses_masked(), out, 18U);
}

static inline const char *ble_archive_ui_address_kind(const cerberus_ble_archive_row_t *row)
{
    if (row->address_type == 0U) return "PUBLIC ADDRESS";
    if (row->address_type == 2U) return "PUBLIC IDENTITY ADDRESS";
    if (row->address_type == 3U) return "RANDOM IDENTITY ADDRESS";
    unsigned kind = row->address[5] >> 6U;
    return kind == 3U ? "RANDOM STATIC ADDRESS" : kind == 1U ?
        "RESOLVABLE PRIVATE ADDRESS" : kind == 0U ?
        "NONRESOLVABLE PRIVATE ADDRESS" : "RANDOM / RESERVED ADDRESS BITS";
}

static inline esp_err_t ble_archive_ui_clear(const char *title)
{
    ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(0, 0, 320, 480, UI_BACKGROUND), TAG, "archive clear");
    return draw_header(title);
}

static inline esp_err_t ble_archive_ui_back(const char *label)
{
    return draw_button(64, 426, 192, 44, label);
}

static inline esp_err_t ble_archive_ui_list(ble_archive_ui_store_t *store)
{
    store->paint_valid = false;
    store->painted_count = 0U;
    ESP_RETURN_ON_ERROR(ble_archive_ui_clear("SAVED BLE SNAPSHOT"), TAG, "archive list");
    ESP_RETURN_ON_ERROR(draw_centered(60, "HISTORICAL / AGE AT SAVE / NO LIVE RADIO", UI_MUTED, 1), TAG, "archive scope");
    char line[53];
    snprintf(line, sizeof(line), "FILE: %.46s", store->file_name);
    ESP_RETURN_ON_ERROR(draw_centered(80, line, UI_TEXT, 1), TAG, "archive file");
    if (strlen(store->file_name) > 46U) {
        snprintf(line, sizeof(line), "...%.47s", store->file_name + 46U);
        ESP_RETURN_ON_ERROR(draw_centered(96, line, UI_MUTED, 1), TAG, "archive file continuation");
    }
    store->filtered_count = cerberus_ble_archive_list_order(&store->document,
        store->source_filter, store->sort, store->order);
    if ((unsigned)store->page * 4U >= store->filtered_count) store->page = 0U;
    const char *source_label = store->source_filter == CERBERUS_BLE_ARCHIVE_LIST_S3 ? "SOURCE S3 >" :
        store->source_filter == CERBERUS_BLE_ARCHIVE_LIST_C5 ? "SOURCE C5 >" :
        store->source_filter == CERBERUS_BLE_ARCHIVE_LIST_LEGACY ? "SOURCE LEGACY >" : "SOURCE ALL >";
    const char *sort_label = store->sort == CERBERUS_BLE_ARCHIVE_LIST_NAME ? "NAME A-Z >" :
        store->sort == CERBERUS_BLE_ARCHIVE_LIST_SIGNAL ? "SIGNAL >" : "FILE ORDER >";
    ESP_RETURN_ON_ERROR(draw_button(8, 112, 148, 30, source_label), TAG, "archive source filter");
    ESP_RETURN_ON_ERROR(draw_button(164, 112, 148, 30, sort_label), TAG, "archive sort");
    unsigned sources[3] = {0};
    for (uint8_t i = 0U; i < store->document.count; ++i)
        if (store->document.rows[i].receiver <= CERBERUS_BLE_ARCHIVE_RECEIVER_C5)
            ++sources[store->document.rows[i].receiver];
    for (unsigned row = 0U; row < 4U && (unsigned)store->page * 4U + row < store->filtered_count; ++row) {
        unsigned index = store->order[(unsigned)store->page * 4U + row];
        store->painted[row] = store->document.rows[index];
        const cerberus_ble_archive_row_t *item = &store->painted[row];
        store->painted_age_valid[row] = item->receiver == CERBERUS_BLE_ARCHIVE_RECEIVER_C5 ?
            cerberus_ble_archive_age_upper_at_save(&store->document, index, &store->painted_age_ms[row]) :
            cerberus_ble_archive_age_at_save(&store->document, index, &store->painted_age_ms[row]);
        char name[33], address[18];
        ble_archive_ui_name(item, name); ble_archive_ui_address(item, address);
        uint16_t y = 150U + row * 54U;
        ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(8, y, 304, 50, UI_PANEL), TAG, "archive row");
        snprintf(line, sizeof(line), "%s / %s", ble_archive_ui_receiver(item->receiver), name[0] ? name : "[NO RETAINED NAME]");
        ESP_RETURN_ON_ERROR(cerberus_display_draw_text(14, y + 6U, line, UI_TEXT, UI_PANEL, 1), TAG, "archive name");
        if (item->rssi_valid) snprintf(line, sizeof(line), "%s T%u / %s %d dBm", address, item->address_type,
            item->rssi_kind == CERBERUS_BLE_ARCHIVE_RSSI_LATEST_ADV ? "LAST" : "PEAK", item->rssi);
        else snprintf(line, sizeof(line), "%s T%u / RSSI UNKNOWN", address, item->address_type);
        ESP_RETURN_ON_ERROR(cerberus_display_draw_text(14, y + 21U, line, UI_MUTED, UI_PANEL, 1), TAG, "archive address");
        if (store->painted_age_valid[row]) snprintf(line, sizeof(line), "ADV %u B / AGE AT SAVE %s%llums", item->advertisement_length,
            item->receiver == CERBERUS_BLE_ARCHIVE_RECEIVER_C5 ? "<=" : "",
            (unsigned long long)store->painted_age_ms[row]);
        else snprintf(line, sizeof(line), "ADV %u BYTES / AGE AT SAVE UNKNOWN", item->advertisement_length);
        ESP_RETURN_ON_ERROR(cerberus_display_draw_text(14, y + 36U, line, UI_MUTED, UI_PANEL, 1), TAG, "archive age");
        ++store->painted_count;
    }
    if (!store->filtered_count) {
        const char *empty = store->source_filter == CERBERUS_BLE_ARCHIVE_LIST_S3 ? "NO S3 OBSERVATIONS IN THIS FILE" :
            store->source_filter == CERBERUS_BLE_ARCHIVE_LIST_C5 ? "NO C5 OBSERVATIONS IN THIS FILE" :
            store->source_filter == CERBERUS_BLE_ARCHIVE_LIST_LEGACY ? "NO LEGACY OBSERVATIONS IN THIS FILE" :
            "NO OBSERVATIONS IN THIS FILTER";
        ESP_RETURN_ON_ERROR(draw_centered(222, empty, UI_TEXT, 1), TAG, "archive empty source");
        ESP_RETURN_ON_ERROR(draw_centered(248, "FILTERING DOES NOT CHANGE THE SAVED FILE", UI_MUTED, 1), TAG, "archive filter scope");
    }
    unsigned pages = store->filtered_count ? (store->filtered_count + 3U) / 4U : 1U;
    snprintf(line, sizeof(line), "%u/%u OBS / P%u/%u / S3 %u C5 %u L%u",
        store->filtered_count, store->document.count, store->page + 1U, pages,
        sources[CERBERUS_BLE_ARCHIVE_RECEIVER_S3], sources[CERBERUS_BLE_ARCHIVE_RECEIVER_C5],
        sources[CERBERUS_BLE_ARCHIVE_RECEIVER_LEGACY]);
    ESP_RETURN_ON_ERROR(draw_centered(364, line, UI_MUTED, 1), TAG, "archive source counts");
    ESP_RETURN_ON_ERROR(draw_button(8, 374, 148, 44, "PREV"), TAG, "archive prev");
    ESP_RETURN_ON_ERROR(draw_button(164, 374, 148, 44, "NEXT"), TAG, "archive next");
    ESP_RETURN_ON_ERROR(draw_button(8, 426, 96, 44, "BACK"), TAG, "archive back");
    ESP_RETURN_ON_ERROR(draw_button(112, 426, 200, 44, "EXPORT FINDMY JSON"), TAG, "archive export");
    store->painted_saved_uptime_ms = store->document.saved_uptime_ms;
    store->paint_valid = true;
    return ESP_OK;
}

static inline esp_err_t ble_archive_ui_detail(ble_archive_ui_store_t *store)
{
    const cerberus_ble_archive_row_t *row = &store->selected;
    char line[53], name[33], address[18];
    ble_archive_ui_name(row, name); ble_archive_ui_address(row, address);
    ESP_RETURN_ON_ERROR(ble_archive_ui_clear("BLE OBSERVATION"), TAG, "archive detail");
    snprintf(line, sizeof(line), "%s / %s", ble_archive_ui_receiver(row->receiver), name[0] ? name : "[NO RETAINED NAME]");
    ESP_RETURN_ON_ERROR(draw_centered(60, line, UI_TEXT, 1), TAG, "archive detail name");
    for (unsigned n = 0U; n < 2U; ++n) {
        size_t used = (size_t)snprintf(line, sizeof(line), "NAME %02u: ", n * 16U);
        size_t length = strlen(row->name);
        for (unsigned i = n * 16U; i < (n + 1U) * 16U && i < length; ++i)
            used += (size_t)snprintf(line + used, sizeof(line) - used, "%02X", (uint8_t)row->name[i]);
        ESP_RETURN_ON_ERROR(draw_centered(78U + n * 16U, line, UI_MUTED, 1), TAG, "archive name bytes");
    }
    ESP_RETURN_ON_ERROR(draw_centered(112, "NAME IS RETAINED PEER TEXT / MAY BE PARTIAL", UI_MUTED, 1), TAG, "archive name scope");
    ESP_RETURN_ON_ERROR(draw_centered(134, address, UI_TEXT, 1), TAG, "archive detail address");
    ESP_RETURN_ON_ERROR(draw_centered(150, ble_archive_ui_address_kind(row), UI_MUTED, 1), TAG, "archive address kind");
    if (row->rssi_valid) snprintf(line, sizeof(line), "RSSI %d dBm / %s", row->rssi,
        row->rssi_kind == CERBERUS_BLE_ARCHIVE_RSSI_LATEST_ADV ? "LATEST ORIGINAL ADV" : "ORIGINAL SWEEP PEAK");
    else snprintf(line, sizeof(line), "RSSI UNAVAILABLE");
    ESP_RETURN_ON_ERROR(draw_centered(172, line, UI_TEXT, 1), TAG, "archive detail rssi");
    if (row->manufacturer_id == UINT16_MAX) snprintf(line, sizeof(line), "MANUFACTURER UNKNOWN / PEER FLAGS %02X", row->peer_flags);
    else snprintf(line, sizeof(line), "MANUFACTURER %04X / PEER FLAGS %02X", row->manufacturer_id, row->peer_flags);
    ESP_RETURN_ON_ERROR(draw_centered(188, line, UI_MUTED, 1), TAG, "archive peer metadata");
    size_t used = (size_t)snprintf(line, sizeof(line), "UUID RAW: ");
    for (unsigned i = 0U; i < row->uuid_length; ++i)
        used += (size_t)snprintf(line + used, sizeof(line) - used, "%02X", row->uuid[i]);
    if (!row->uuid_length) snprintf(line + used, sizeof(line) - used, "NOT RETAINED");
    ESP_RETURN_ON_ERROR(draw_centered(206, line, UI_MUTED, 1), TAG, "archive peer uuid");
    if (store->selected_age_valid) snprintf(line, sizeof(line), "AGE AT SAVE: %s%llu ms",
        row->receiver == CERBERUS_BLE_ARCHIVE_RECEIVER_C5 ? "<= " : "", (unsigned long long)store->selected_age_ms);
    else snprintf(line, sizeof(line), "AGE AT SAVE: UNKNOWN");
    ESP_RETURN_ON_ERROR(draw_centered(230, line, UI_TEXT, 1), TAG, "archive historical age");
    snprintf(line, sizeof(line), "SAVED BOOT UPTIME: %llu ms", (unsigned long long)store->selected_saved_uptime_ms);
    ESP_RETURN_ON_ERROR(draw_centered(248, line, UI_MUTED, 1), TAG, "archive saved clock");
    if (row->receiver == CERBERUS_BLE_ARCHIVE_RECEIVER_C5)
        snprintf(line, sizeof(line), "C5 BOOT ID: %016llX", (unsigned long long)row->receiver_boot_id);
    else if (row->source_time_valid) snprintf(line, sizeof(line), "RAW ADV UPTIME: %llu ms", (unsigned long long)row->original_source_uptime_ms);
    else snprintf(line, sizeof(line), "RAW ADV UPTIME: UNKNOWN");
    ESP_RETURN_ON_ERROR(draw_centered(266, line, UI_MUTED, 1), TAG, "archive source clock");
    if (row->receiver == CERBERUS_BLE_ARCHIVE_RECEIVER_C5)
        snprintf(line, sizeof(line), "REMOTE AGE BOUND / NO LOCAL RX TIMESTAMP");
    else if (row->source_time_valid) snprintf(line, sizeof(line), "COPY CEILING: %llu ms", (unsigned long long)row->copy_ceiling_ms);
    else snprintf(line, sizeof(line), "COPY CEILING: UNKNOWN");
    ESP_RETURN_ON_ERROR(draw_centered(284, line, UI_MUTED, 1), TAG, "archive copy ceiling");
    if (row->scan_epoch_valid) snprintf(line, sizeof(line), "SAVED SCAN EPOCH: %llu", (unsigned long long)row->scan_epoch);
    else snprintf(line, sizeof(line), "SAVED SCAN EPOCH: UNKNOWN");
    ESP_RETURN_ON_ERROR(draw_centered(302, line, UI_MUTED, 1), TAG, "archive epoch");
    snprintf(line, sizeof(line), "GEN %u / INDEX %u / REPORTS %u", row->generation, row->index, row->reports_seen);
    ESP_RETURN_ON_ERROR(draw_centered(320, line, UI_MUTED, 1), TAG, "archive source counters");
    if (row->receiver == CERBERUS_BLE_ARCHIVE_RECEIVER_C5) {
        ESP_RETURN_ON_ERROR(draw_centered(340, row->receive.source_kind ?
            "C5 RECEIVE INFO RETAINED" : "C5 RECEIVE INFO UNAVAILABLE", UI_TEXT, 1), TAG, "archive receive info");
        if (row->c5_response.state == CERBERUS_DUAL_BLE_RESPONSE_PRESENT)
            snprintf(line, sizeof(line), "C5 MATCHED RESPONSE: %u OF %u BYTES", row->c5_response.raw_length,
                row->c5_response.receive.callback_length);
        else snprintf(line, sizeof(line), row->c5_response.state == CERBERUS_DUAL_BLE_RESPONSE_NONE ?
            "NO ADMITTED C5 RESPONSE" : "C5 RESPONSE BYTES UNAVAILABLE");
    } else if (row->response_state == CERBERUS_BLE_ARCHIVE_RESPONSE_PRESENT) {
        snprintf(line, sizeof(line), "MATCHED RESPONSE: %u OF %u BYTES", row->response_length,
            row->response_original_length);
        ESP_RETURN_ON_ERROR(draw_centered(340, line, UI_TEXT, 1), TAG, "archive response size");
        if (row->response_time_valid && store->selected_saved_uptime_ms >= row->response_source_uptime_ms)
            snprintf(line, sizeof(line), "RSP AGE AT SAVE: %llu ms",
                (unsigned long long)(store->selected_saved_uptime_ms - row->response_source_uptime_ms));
        else snprintf(line, sizeof(line), "RSP AGE AT SAVE: UNKNOWN");
    } else {
        ESP_RETURN_ON_ERROR(draw_centered(340, "NO LOCATION OR WALL CLOCK RECORDED", UI_MUTED, 1), TAG, "archive location scope");
        snprintf(line, sizeof(line), "%s", row->response_state == CERBERUS_BLE_ARCHIVE_RESPONSE_NONE ?
            "NO MATCHED RESPONSE RETAINED" : "RESPONSE BYTES / CORRELATION UNAVAILABLE");
    }
    ESP_RETURN_ON_ERROR(draw_centered(356, line, UI_MUTED, 1), TAG, "archive response evidence");
    ESP_RETURN_ON_ERROR(draw_button(8, 374, 148, 44, "RAW DATA"), TAG, "archive raw button");
    ESP_RETURN_ON_ERROR(draw_button(164, 374, 148, 44, "FIND LIVE"), TAG, "archive find live");
    return ble_archive_ui_back("BACK TO SNAPSHOT");
}

static inline const char *ble_archive_ui_ad_name(uint8_t type)
{
    switch (type) {
    case 0x01: return "FLAGS";
    case 0x02: case 0x03: return "UUID16 LIST";
    case 0x04: case 0x05: return "UUID32 LIST";
    case 0x06: case 0x07: return "UUID128 LIST";
    case 0x08: return "SHORT NAME";
    case 0x09: return "COMPLETE NAME";
    case 0x0A: return "TX POWER";
    case 0x16: return "SERVICE DATA16";
    case 0x20: return "SERVICE DATA32";
    case 0x21: return "SERVICE DATA128";
    case 0x19: return "APPEARANCE";
    case 0xFF: return "MANUFACTURER DATA";
    default: return "UNKNOWN TYPE";
    }
}

/* Structural TLV framing only; the type label does not assert valid field
 * semantics. Preserve malformed raw bytes, show complete preceding fields,
 * and never continue by guessing a length after an invalid boundary. */
static inline uint8_t ble_archive_ui_payload_fields(const uint8_t *data, uint8_t size,
    uint8_t offsets[15], uint8_t *bad_offset)
{
    uint8_t count = 0U, offset = 0U;
    *bad_offset = UINT8_MAX;
    while (offset < size) {
        uint8_t length = data[offset];
        if (!length) {
            for (uint8_t i = offset + 1U; i < size; ++i)
                if (data[i]) { *bad_offset = offset; break; }
            break;
        }
        if (length > size - offset - 1U || count >= 15U) {
            *bad_offset = offset; break;
        }
        offsets[count++] = offset;
        offset = (uint8_t)(offset + length + 1U);
    }
    return count;
}

static inline uint8_t ble_archive_ui_fields(const cerberus_ble_archive_row_t *row,
    uint8_t offsets[15], uint8_t *bad_offset)
{ return ble_archive_ui_payload_fields(row->advertisement, row->advertisement_length, offsets, bad_offset); }

static inline bool ble_archive_ui_c5_response_valid(const cerberus_ble_archive_row_t *row)
{
    if (row->receiver != CERBERUS_BLE_ARCHIVE_RECEIVER_C5 || !row->receiver_boot_id ||
        !row->scan_epoch_valid || !row->scan_epoch) return false;
    cerberus_dual_ble_family_evidence_t original;
    const cerberus_dual_ble_family_wire_t empty = {0};
    if (memcmp(&row->family, &empty, sizeof(empty)) &&
        !cerberus_dual_ble_family_wire_decode(&row->family, &row->receive,
            row->advertisement_length, row->advertisement_event_type, row->advertisement_flags,
            row->address, row->address_type, &original)) return false;
    return cerberus_dual_ble_response_source_valid(&row->c5_response, &row->receive,
        row->advertisement_length, row->advertisement_event_type, row->advertisement_flags,
        row->address, row->address_type, row->family.version ? row->family.family_code : 0U);
}

static inline esp_err_t ble_archive_ui_hints(ble_archive_ui_store_t *store)
{
    const cerberus_ble_archive_row_t *row = &store->selected;
    cerberus_dual_ble_family_evidence_t evidence = {0};
    const cerberus_dual_ble_family_wire_t absent = {0};
    bool response = store->c5_response;
    bool present = response ? row->c5_response.state == CERBERUS_DUAL_BLE_RESPONSE_PRESENT :
        memcmp(&row->family, &absent, sizeof(absent)) != 0;
    if (response) {
        if (!ble_archive_ui_c5_response_valid(row) ||
            !cerberus_dual_ble_response_family(&row->c5_response, row->address, row->address_type, &evidence))
            (void)cerberus_dual_ble_family_unavailable(&evidence, CERBERUS_DUAL_BLE_FAMILY_INVALID_SOURCE);
    } else if (present && (row->receiver != CERBERUS_BLE_ARCHIVE_RECEIVER_C5 ||
        !row->receiver_boot_id || !row->scan_epoch_valid || !row->scan_epoch ||
        !cerberus_dual_ble_family_wire_decode(&row->family, &row->receive,
            row->advertisement_length, row->advertisement_event_type,
            row->advertisement_flags, row->address, row->address_type, &evidence)))
        (void)cerberus_dual_ble_family_unavailable(&evidence, CERBERUS_DUAL_BLE_FAMILY_INVALID_SOURCE);
    /* An absent v1-v4/ordinary proof stays unknown. Never reconstruct a full
     * report's family facts from its short raw preview or aggregate fields. */
    char line[53];
    ESP_RETURN_ON_ERROR(draw_centered(120, "HISTORICAL / NO LIVE RADIO", UI_MUTED, 1), TAG, "archive hint scope");
    bool age_valid = response ? row->c5_response_age_upper_valid : store->selected_age_valid;
    uint64_t age = response ? row->c5_response_age_upper_at_save_ms : store->selected_age_ms;
    if (age_valid) snprintf(line, sizeof(line), "AGE AT SAVE: <= %llu ms", (unsigned long long)age);
    else snprintf(line, sizeof(line), "AGE AT SAVE: UNKNOWN");
    ESP_RETURN_ON_ERROR(draw_centered(140, line, UI_TEXT, 1), TAG, "archive hint age");
    snprintf(line, sizeof(line), "SAVED BOOT UPTIME: %llu ms", (unsigned long long)store->selected_saved_uptime_ms);
    ESP_RETURN_ON_ERROR(draw_centered(160, line, UI_MUTED, 1), TAG, "archive hint saved clock");
    snprintf(line, sizeof(line), "C5 BOOT ID: %016llX", (unsigned long long)row->receiver_boot_id);
    ESP_RETURN_ON_ERROR(draw_centered(180, line, UI_MUTED, 1), TAG, "archive hint source");
    ESP_RETURN_ON_ERROR(draw_centered(200, response ? "MATCHED RESPONSE / ORIGINAL KEPT SEPARATE" :
        "ORIGINAL ADV / RESPONSE KEPT SEPARATE", UI_MUTED, 1), TAG, "archive hint provenance");
    uint8_t pages = 0U;
    ESP_RETURN_ON_ERROR(ble_family_hint_ui_draw_source(&evidence, present && evidence.available,
        response ? row->c5_response.receive.callback_length : row->receive.callback_length,
        response ? row->c5_response.raw_length : row->advertisement_length, true, response, 218U,
        store->hint_page, &pages), TAG, "archive saved hints");
    ESP_RETURN_ON_ERROR(draw_centered(310, "DECODED HINTS / RAW PREVIEW IS SEPARATE", UI_MUTED, 1), TAG, "archive hint bytes");
    ESP_RETURN_ON_ERROR(draw_centered(332, "SAVED EVIDENCE / NOT CURRENT RADIO STATE", UI_MUTED, 1), TAG, "archive hint time");
    ESP_RETURN_ON_ERROR(draw_button(8, 374, 148, 44, "PREV HINTS"), TAG, "archive hints prev");
    ESP_RETURN_ON_ERROR(draw_button(164, 374, 148, 44, "NEXT HINTS"), TAG, "archive hints next");
    ESP_RETURN_ON_ERROR(ble_archive_ui_back("BACK TO DETAILS"), TAG, "archive hints back");
    store->hint_pages = pages;
    return ESP_OK;
}

static inline esp_err_t ble_archive_ui_raw(ble_archive_ui_store_t *store)
{
    const cerberus_ble_archive_row_t *row = &store->selected;
    const bool response = store->raw_response;
    const bool c5 = row->receiver == CERBERUS_BLE_ARCHIVE_RECEIVER_C5;
    store->hint_pages = 0U;
    ESP_RETURN_ON_ERROR(ble_archive_ui_clear(c5 && store->raw_hints ?
        (store->c5_response ? "SAVED C5 RSP HINTS" : "SAVED C5 HINTS") :
        response ? (c5 ? (store->c5_response ? "SAVED RSP RX INFO" : "SAVED RECEIVE INFO") : "SAVED RESPONSE") :
        c5 && store->c5_response ? "SAVED C5 RESPONSE" : "SAVED RAW ADV"), TAG, "archive raw");
    if (c5) {
        ESP_RETURN_ON_ERROR(draw_button(8, 54, 96, 28, !response && !store->raw_hints ?
            (store->c5_response ? "RAW RSP *" : "RAW ADV *") : (store->c5_response ? "RAW RSP" : "RAW ADV")), TAG, "archive ADV selector");
        ESP_RETURN_ON_ERROR(draw_button(112, 54, 96, 28, response ? "RX INFO *" : "RX INFO"), TAG, "archive receive selector");
        ESP_RETURN_ON_ERROR(draw_button(216, 54, 96, 28, store->raw_hints ? "HINTS *" : "HINTS"), TAG, "archive hint selector");
        ESP_RETURN_ON_ERROR(draw_button(8, 84, 148, 24, store->c5_response ? "ORIGINAL ADV" : "ORIGINAL ADV *"), TAG, "archive original source");
        ESP_RETURN_ON_ERROR(draw_button(164, 84, 148, 24, store->c5_response ? "MATCHED RSP *" : "MATCHED RSP"), TAG, "archive response source");
        if (store->c5_response && (!ble_archive_ui_c5_response_valid(row) ||
            row->c5_response.state != CERBERUS_DUAL_BLE_RESPONSE_PRESENT)) {
            const char *state = ble_archive_ui_c5_response_valid(row) && row->c5_response.state == CERBERUS_DUAL_BLE_RESPONSE_NONE ?
                "NO ADMITTED MATCHED RESPONSE" : "C5 RESPONSE EVIDENCE UNKNOWN";
            ESP_RETURN_ON_ERROR(draw_centered(144, state, UI_TEXT, 1), TAG, "archive C5 response missing");
            ESP_RETURN_ON_ERROR(draw_centered(176, "HISTORICAL / NO RESPONSE BYTES TO SHOW", UI_MUTED, 1), TAG, "archive C5 missing bytes");
            ESP_RETURN_ON_ERROR(draw_centered(204, "THIS DOES NOT PROVE NO RESPONSE WAS SENT", UI_MUTED, 1), TAG, "archive C5 response absence");
            return ble_archive_ui_back("BACK TO DETAILS");
        }
        if (store->raw_hints) return ble_archive_ui_hints(store);
    } else {
        ESP_RETURN_ON_ERROR(draw_button(8, 54, 148, 28, response ? "ADVERTISEMENT" : "ADVERTISEMENT *"), TAG, "archive ADV selector");
        ESP_RETURN_ON_ERROR(draw_button(164, 54, 148, 28, response ? "RESPONSE *" : "RESPONSE"), TAG, "archive alternate selector");
    }
    if (c5 && response) {
        ESP_RETURN_ON_ERROR(ble_receive_info_ui_draw(store->c5_response ? &row->c5_response.receive : &row->receive,
            store->c5_response ? row->c5_response.raw_length : row->advertisement_length, 116U), TAG, "archive receive metadata");
        ESP_RETURN_ON_ERROR(draw_centered(224, "HISTORICAL EVIDENCE / RECORDED AT SAVE", UI_MUTED, 1), TAG, "archive receive history");
        char detail[53];
        if (store->c5_response) {
            if (row->c5_response.flags & CERBERUS_DUAL_BLE_RESPONSE_RSSI_VALID)
                snprintf(detail, sizeof(detail), "SAVED RESPONSE RSSI %d dBm", row->c5_response.rssi);
            else snprintf(detail, sizeof(detail), "SAVED RESPONSE RSSI UNKNOWN");
            ESP_RETURN_ON_ERROR(draw_centered(244, detail, UI_TEXT, 1), TAG, "archive response RSSI");
            if (row->c5_response_age_upper_valid) snprintf(detail, sizeof(detail), "RSP AGE AT SAVE: <= %llu ms",
                (unsigned long long)row->c5_response_age_upper_at_save_ms);
            else snprintf(detail, sizeof(detail), "RSP AGE AT SAVE: UNKNOWN");
            ESP_RETURN_ON_ERROR(draw_centered(264, detail, UI_TEXT, 1), TAG, "archive response age");
            memcpy(detail, "RSP NAME: ", 10U);size_t n=10U;
            for (uint8_t i=0U;i<row->c5_response.name_length && i<32U;++i) {
                uint8_t c=row->c5_response.name[i];detail[n++]=c>=32U&&c<=126U?(char)c:'.';
            }
            detail[n]='\0';
            if (n==10U) snprintf(detail, sizeof(detail), "RSP NAME: NOT RETAINED");
            ESP_RETURN_ON_ERROR(draw_centered(282, detail, UI_MUTED, 1), TAG, "archive response name");
        } else {
            ESP_RETURN_ON_ERROR(draw_centered(244, "NOT CURRENT RADIO STATE", UI_MUTED, 1), TAG, "archive receive clock scope");
            ESP_RETURN_ON_ERROR(draw_centered(274, "RESPONSE HAS ITS OWN SOURCE TAB", UI_MUTED, 1), TAG, "archive receive response scope");
        }
        ESP_RETURN_ON_ERROR(draw_centered(314, "MISSING BYTES ARE NOT RECONSTRUCTED", UI_MUTED, 1), TAG, "archive receive preview scope");
        return ble_archive_ui_back("BACK TO DETAILS");
    }
    const bool c5_rsp = c5 && store->c5_response;
    const bool packet_response = c5_rsp || (!c5 && response);
    const bool available = c5 ? true : !response || row->response_state == CERBERUS_BLE_ARCHIVE_RESPONSE_PRESENT;
    const uint8_t *data = c5_rsp ? row->c5_response.raw : response ? row->response : row->advertisement;
    const uint8_t size = !available ? 0U : c5_rsp ? row->c5_response.raw_length : response ? row->response_length : row->advertisement_length;
    const uint8_t flags = c5_rsp ? row->c5_response.raw_flags : response ? row->response_flags : row->advertisement_flags;
    const uint16_t shift = c5 ? 26U : 0U;
    uint8_t offsets[15], bad;
    uint8_t count = ble_archive_ui_payload_fields(data, size, offsets, &bad);
    if ((unsigned)store->raw_page * 3U >= count) store->raw_page = 0U;
    char line[53];
    if (!available) {
        ESP_RETURN_ON_ERROR(draw_centered(144, row->response_state == CERBERUS_BLE_ARCHIVE_RESPONSE_NONE ?
            "NO MATCHED RESPONSE RETAINED" : "RESPONSE BYTES / CORRELATION UNAVAILABLE", UI_TEXT, 1), TAG, "archive absent response");
        ESP_RETURN_ON_ERROR(draw_centered(176, "NO RESPONSE BYTES IN THIS SAVED OBSERVATION", UI_MUTED, 1), TAG, "archive absent payload");
        ESP_RETURN_ON_ERROR(draw_centered(204, "THIS DOES NOT PROVE NO RESPONSE WAS SENT", UI_MUTED, 1), TAG, "archive absence scope");
        return ble_archive_ui_back("BACK TO DETAILS");
    }
    if (packet_response) snprintf(line, sizeof(line), "%u OF %u BYTES / EVENT %u / FLAGS %02X", size,
        c5_rsp ? row->c5_response.receive.callback_length : row->response_original_length,
        c5_rsp ? row->c5_response.event_type : row->response_event_type, flags);
    else snprintf(line, sizeof(line), "%u BYTES / EVENT %u / RAW FLAGS %02X", size,
        row->advertisement_event_type, flags);
    ESP_RETURN_ON_ERROR(draw_centered(90U + shift, line, UI_TEXT, 1), TAG, "archive raw metadata");
    if (bad != UINT8_MAX) snprintf(line, sizeof(line), "MALFORMED OR CUT AD AT BYTE %u", bad);
    else snprintf(line, sizeof(line), "AD BOUNDARIES VALID / FIELD VALUES UNVERIFIED");
    ESP_RETURN_ON_ERROR(draw_centered(108U + shift, line, UI_MUTED, 1), TAG, "archive raw structure");
    bool truncated = (flags & CERBERUS_BLE_ADVERTISEMENT_TRUNCATED) != 0U;
    bool malformed = (flags & CERBERUS_BLE_ADVERTISEMENT_MALFORMED) != 0U;
    const char *limits = truncated && malformed ? "SOURCE: TRUNCATED + MALFORMED" : truncated ?
        "SOURCE: TRUNCATED / MISSING BYTES UNKNOWN" : malformed ?
        "SOURCE MARKED MALFORMED / RAW RETAINED" : "RAW FLAGS RETAINED / MISSING BYTES NOT FILLED";
    ESP_RETURN_ON_ERROR(draw_centered(124U + shift, limits, UI_MUTED, 1), TAG, "archive raw limits");
    for (unsigned n = 0U; n < 2U; ++n) {
        size_t used = (size_t)snprintf(line, sizeof(line), "%02u: ", n * 16U);
        for (unsigned i = n * 16U; i < (n + 1U) * 16U && i < size; ++i)
            used += (size_t)snprintf(line + used, sizeof(line) - used, "%02X%s", data[i], i % 16U == 15U ? "" : " ");
        ESP_RETURN_ON_ERROR(draw_centered(144U + shift + n * 18U, line, UI_TEXT, 1), TAG, "archive hex bytes");
    }
    for (unsigned n = 0U; n < 3U && (unsigned)store->raw_page * 3U + n < count; ++n) {
        unsigned index = (unsigned)store->raw_page * 3U + n;
        uint8_t offset = offsets[index], length = data[offset];
        snprintf(line, sizeof(line), "AD %u / %02X / %s", index + 1U,
            data[offset + 1U], ble_archive_ui_ad_name(data[offset + 1U]));
        ESP_RETURN_ON_ERROR(draw_centered(184U + shift + n * (c5 ? 48U : 56U), line, UI_TEXT, 1), TAG, "archive field type");
        for (unsigned part = 0U; part < 2U; ++part) {
            size_t used = 0U;
            for (unsigned i = part * 15U; i < (part + 1U) * 15U && i + 1U < length; ++i)
                used += (size_t)snprintf(line + used, sizeof(line) - used, "%02X ", data[offset + 2U + i]);
            if (!used) snprintf(line, sizeof(line), "%s", part == 0U ? "[NO VALUE BYTES]" : "");
            ESP_RETURN_ON_ERROR(draw_centered(200U + shift + n * (c5 ? 48U : 56U) + part * 14U, line, UI_MUTED, 1), TAG, "archive field value");
        }
    }
    if (!count) ESP_RETURN_ON_ERROR(draw_centered(232, "NO COMPLETE AD FIELDS", UI_MUTED, 1), TAG, "archive no fields");
    snprintf(line, sizeof(line), "%u COMPLETE FIELDS / PAGE %u OF %u", count,
        store->raw_page + 1U, count ? (count + 2U) / 3U : 1U);
    ESP_RETURN_ON_ERROR(draw_centered(354, line, UI_MUTED, 1), TAG, "archive field page");
    ESP_RETURN_ON_ERROR(draw_button(8, 374, 148, 44, "PREV FIELDS"), TAG, "archive field prev");
    ESP_RETURN_ON_ERROR(draw_button(164, 374, 148, 44, "NEXT FIELDS"), TAG, "archive field next");
    return ble_archive_ui_back("BACK TO DETAILS");
}

static inline esp_err_t ble_archive_ui_draw(ble_archive_ui_store_t *store, esp_err_t fallback_error)
{
    if (store && store->has_document && store->view == BLE_ARCHIVE_UI_LIST) return ble_archive_ui_list(store);
    if (store && store->selected_valid && store->view == BLE_ARCHIVE_UI_DETAIL) return ble_archive_ui_detail(store);
    if (store && store->selected_valid && store->view == BLE_ARCHIVE_UI_RAW) return ble_archive_ui_raw(store);
    ESP_RETURN_ON_ERROR(ble_archive_ui_clear("BLE SD ARCHIVE"), TAG, "archive status");
    if (store && store->release_pending) {
        ESP_RETURN_ON_ERROR(draw_centered(160, "RELEASING RECEIVERS", UI_TEXT, 2), TAG, "archive release");
        ESP_RETURN_ON_ERROR(draw_centered(204, "SNAPSHOT FROZEN / SD WRITE NOT STARTED", UI_MUTED, 1), TAG, "archive frozen");
        ESP_RETURN_ON_ERROR(draw_centered(244, "BACK CANCELS SAVE / RECEIVERS STAY STOPPED", UI_MUTED, 1), TAG, "archive release cancel");
    } else if (store && store->view == BLE_ARCHIVE_UI_JOB && store->job_pending) {
        ESP_RETURN_ON_ERROR(draw_centered(160, store->saving ? "SAVING SNAPSHOT" : "LOADING SNAPSHOT", UI_TEXT, 2), TAG, "archive working");
        ESP_RETURN_ON_ERROR(draw_centered(204, "READ-ONLY HISTORICAL OBSERVATIONS", UI_MUTED, 1), TAG, "archive working scope");
        ESP_RETURN_ON_ERROR(draw_centered(244, store->saving ? "BACK CANCELS WAIT / A SAVE MAY FINISH" :
            "BACK CANCELS THIS LOAD", UI_MUTED, 1), TAG, "archive cancel scope");
    } else {
        ESP_RETURN_ON_ERROR(draw_centered(160, "ARCHIVE UNAVAILABLE", UI_TEXT, 1), TAG, "archive unavailable");
        esp_err_t error = store && store->error != ESP_OK ? store->error : fallback_error;
        if (error == ESP_OK) error = ESP_ERR_INVALID_STATE;
        ESP_RETURN_ON_ERROR(draw_centered(190, esp_err_to_name(error), UI_MUTED, 1), TAG, "archive error");
        if (store && store->has_document) {
            ESP_RETURN_ON_ERROR(draw_centered(230, "PREVIOUS SNAPSHOT RETAINED", UI_MUTED, 1), TAG, "archive preserved");
            ESP_RETURN_ON_ERROR(draw_button(64, 374, 192, 44, "VIEW PREVIOUS"), TAG, "archive previous");
        }
    }
    return ble_archive_ui_back("BACK");
}

static inline ble_archive_ui_action_t ble_archive_ui_tap(ble_archive_ui_store_t *store,
    uint16_t x, uint16_t y)
{
    if (store && store->view == BLE_ARCHIVE_UI_LIST && store->has_document &&
        !store->release_pending && !store->job_pending && y >= 426U && y < 470U) {
        if (x >= 8U && x < 104U) return BLE_ARCHIVE_UI_BACK;
        if (x >= 112U && x < 312U) return BLE_ARCHIVE_UI_EXPORT;
        return BLE_ARCHIVE_UI_NONE;
    }
    if (x >= 64U && x < 256U && y >= 426U && y < 470U) {
        if (!store) return BLE_ARCHIVE_UI_BACK;
        store->paint_valid = false;
        if (store->release_pending || store->job_pending) {
            store->release_pending = store->job_pending = false; return BLE_ARCHIVE_UI_CANCEL;
        }
        if (store->view == BLE_ARCHIVE_UI_RAW) { store->view = BLE_ARCHIVE_UI_DETAIL; return BLE_ARCHIVE_UI_REDRAW; }
        if (store->view == BLE_ARCHIVE_UI_DETAIL) { store->view = BLE_ARCHIVE_UI_LIST; return BLE_ARCHIVE_UI_REDRAW; }
        return BLE_ARCHIVE_UI_BACK;
    }
    if (!store || store->release_pending || store->job_pending || !store->has_document) return BLE_ARCHIVE_UI_NONE;
    if (store->view == BLE_ARCHIVE_UI_ERROR && x >= 64U && x < 256U && y >= 374U && y < 418U) {
        store->view = BLE_ARCHIVE_UI_LIST; store->paint_valid = false; return BLE_ARCHIVE_UI_REDRAW;
    }
    if (store->view == BLE_ARCHIVE_UI_LIST) {
        if (y >= 112U && y < 142U && ((x >= 8U && x < 156U) || (x >= 164U && x < 312U))) {
            if (x < 156U) store->source_filter = (cerberus_ble_archive_list_source_t)
                (((unsigned)store->source_filter + 1U) % CERBERUS_BLE_ARCHIVE_LIST_SOURCE_COUNT);
            else store->sort = (cerberus_ble_archive_list_sort_t)
                (((unsigned)store->sort + 1U) % CERBERUS_BLE_ARCHIVE_LIST_SORT_COUNT);
            store->page = 0U;
            store->paint_valid = false; store->painted_count = 0U;
            return BLE_ARCHIVE_UI_REDRAW;
        }
        if (y >= 374U && y < 418U && ((x >= 8U && x < 156U) || (x >= 164U && x < 312U))) {
            if (x < 156U) { if (store->page) --store->page; }
            else if (((unsigned)store->page + 1U) * 4U < store->filtered_count) ++store->page;
            store->paint_valid = false; store->painted_count = 0U; return BLE_ARCHIVE_UI_REDRAW;
        }
        if (store->paint_valid && x >= 8U && x < 312U && y >= 150U && y < 362U) {
            unsigned row = (y - 150U) / 54U;
            if ((y - 150U) % 54U >= 50U || row >= store->painted_count) return BLE_ARCHIVE_UI_NONE;
            store->selected = store->painted[row];
            store->selected_saved_uptime_ms = store->painted_saved_uptime_ms;
            store->selected_age_ms = store->painted_age_ms[row];
            store->selected_age_valid = store->painted_age_valid[row];
            store->selected_valid = true; store->paint_valid = false;
            store->raw_page = 0U; store->raw_response = store->raw_hints = false; store->c5_response = false;
            store->hint_page = store->hint_pages = 0U; store->view = BLE_ARCHIVE_UI_DETAIL;
            return BLE_ARCHIVE_UI_REDRAW;
        }
    } else if (store->view == BLE_ARCHIVE_UI_DETAIL && store->selected_valid &&
        x >= 164U && x < 312U && y >= 374U && y < 418U) {
        return BLE_ARCHIVE_UI_FIND_LIVE;
    } else if (store->view == BLE_ARCHIVE_UI_DETAIL && store->selected_valid &&
        x >= 8U && x < 156U && y >= 374U && y < 418U) {
        store->view = BLE_ARCHIVE_UI_RAW; store->raw_page = 0U;
        store->raw_response = store->raw_hints = false; store->c5_response = false; store->hint_page = store->hint_pages = 0U;
        return BLE_ARCHIVE_UI_REDRAW;
    } else if (store->view == BLE_ARCHIVE_UI_RAW && store->selected_valid &&
        store->selected.receiver == CERBERUS_BLE_ARCHIVE_RECEIVER_C5 && y >= 84U && y < 108U) {
        if (x >= 8U && x < 156U) store->c5_response = false;
        else if (x >= 164U && x < 312U) store->c5_response = true;
        else return BLE_ARCHIVE_UI_NONE;
        store->raw_page = store->hint_page = store->hint_pages = 0U;
        return BLE_ARCHIVE_UI_REDRAW;
    } else if (store->view == BLE_ARCHIVE_UI_RAW && store->selected_valid &&
        y >= 54U && y < 82U) {
        if (store->selected.receiver == CERBERUS_BLE_ARCHIVE_RECEIVER_C5) {
            if (x >= 8U && x < 104U) store->raw_response = store->raw_hints = false;
            else if (x >= 112U && x < 208U) { store->raw_response = true; store->raw_hints = false; }
            else if (x >= 216U && x < 312U) { store->raw_response = false; store->raw_hints = true; }
            else return BLE_ARCHIVE_UI_NONE;
        } else if ((x >= 8U && x < 156U) || (x >= 164U && x < 312U)) {
            store->raw_response = x >= 164U; store->raw_hints = false;
        } else return BLE_ARCHIVE_UI_NONE;
        store->raw_page = store->hint_page = store->hint_pages = 0U;
        return BLE_ARCHIVE_UI_REDRAW;
    } else if (store->view == BLE_ARCHIVE_UI_RAW && store->selected_valid &&
        y >= 374U && y < 418U && ((x >= 8U && x < 156U) || (x >= 164U && x < 312U))) {
        if (store->raw_hints && store->selected.receiver == CERBERUS_BLE_ARCHIVE_RECEIVER_C5) {
            if (!store->hint_pages) return BLE_ARCHIVE_UI_NONE;
            if (x < 156U) { if (store->hint_page) --store->hint_page; }
            else if (store->hint_page + 1U < store->hint_pages) ++store->hint_page;
            store->hint_pages = 0U;
            return BLE_ARCHIVE_UI_REDRAW;
        }
        if (store->raw_response && (store->selected.receiver == CERBERUS_BLE_ARCHIVE_RECEIVER_C5 ||
            store->selected.response_state != CERBERUS_BLE_ARCHIVE_RESPONSE_PRESENT))
            return BLE_ARCHIVE_UI_NONE;
        uint8_t offsets[15], bad;
        if (store->selected.receiver == CERBERUS_BLE_ARCHIVE_RECEIVER_C5 && store->c5_response &&
            (!ble_archive_ui_c5_response_valid(&store->selected) ||
             store->selected.c5_response.state != CERBERUS_DUAL_BLE_RESPONSE_PRESENT)) return BLE_ARCHIVE_UI_NONE;
        unsigned count = store->selected.receiver == CERBERUS_BLE_ARCHIVE_RECEIVER_C5 && store->c5_response ?
            ble_archive_ui_payload_fields(store->selected.c5_response.raw, store->selected.c5_response.raw_length, offsets, &bad) :
            store->raw_response ? ble_archive_ui_payload_fields(store->selected.response,
                store->selected.response_state == CERBERUS_BLE_ARCHIVE_RESPONSE_PRESENT ?
                    store->selected.response_length : 0U, offsets, &bad) :
            ble_archive_ui_fields(&store->selected, offsets, &bad);
        if (x < 156U) { if (store->raw_page) --store->raw_page; }
        else if (((unsigned)store->raw_page + 1U) * 3U < count) ++store->raw_page;
        return BLE_ARCHIVE_UI_REDRAW;
    }
    return BLE_ARCHIVE_UI_NONE;
}
