/* Private passive Wardriving viewer. The enclosing UI owns allocation and
 * coherent snapshot copies. These functions perform display work only; Stop
 * and Back are returned as actions for the existing Wardriving controller. */
#include "wardrive_recent.h"
#include "wardrive_service.h"
#include "wifi_scan_observation.h"

typedef enum { WARDRIVE_RECENT_UI_LIST, WARDRIVE_RECENT_UI_DETAIL } wardrive_recent_ui_view_t;
typedef enum {
    WARDRIVE_RECENT_UI_NONE, WARDRIVE_RECENT_UI_REDRAW,
    WARDRIVE_RECENT_UI_BACK, WARDRIVE_RECENT_UI_STOP,
} wardrive_recent_ui_action_t;

static inline const char *wardrive_recent_ui_outcome(cerberus_wardrive_recent_outcome_t outcome)
{
    switch (outcome) {
    case CERBERUS_WARDRIVE_RECENT_PENDING: return "PENDING";
    case CERBERUS_WARDRIVE_RECENT_DUPLICATE: return "DUPLICATE / NO APPEND";
    case CERBERUS_WARDRIVE_RECENT_APPENDED_UNLOCATED: return "APPENDED / UNLOCATED";
    case CERBERUS_WARDRIVE_RECENT_APPENDED_WIGLE: return "APPENDED / WIGLE";
    case CERBERUS_WARDRIVE_RECENT_ERROR: return "LOG ERROR";
    case CERBERUS_WARDRIVE_RECENT_NOT_LOGGED: return "NOT LOGGED";
    default: return "OUTCOME UNKNOWN";
    }
}

static inline void wardrive_recent_ui_name(const cerberus_wardrive_observation_t *row, char out[33])
{
    size_t n = row->label_length <= 32U ? row->label_length : 32U;
    for (size_t i = 0U; i < n; ++i)
        out[i] = row->label[i] >= 32U && row->label[i] <= 126U ? (char)row->label[i] : '.';
    out[n] = '\0';
}

static inline void wardrive_recent_ui_address(const cerberus_wardrive_observation_t *row, char out[18])
{
    if (row->type == CERBERUS_WARDRIVE_BLE) {
        /* Wardriving already stores display order; the shared BLE formatter
         * and its privacy hash take native NimBLE byte order. */
        uint8_t native[6];
        for (unsigned i = 0U; i < 6U; ++i) native[i] = row->address[5U - i];
        cerberus_privacy_format_ble_identifier(native, privacy_addresses_masked(), out, 18U);
    } else cerberus_privacy_format_wifi_identifier(row->address, privacy_addresses_masked(), out, 18U);
}

static inline const char *wardrive_recent_ui_address_kind(const cerberus_wardrive_observation_t *row)
{
    if (row->type != CERBERUS_WARDRIVE_BLE)
        return row->address[0] & 2U ? "BSSID / LOCALLY ADMINISTERED" : "BSSID / UNIVERSALLY ADMINISTERED";
    if (row->address_type == 0U) return "BLE PUBLIC ADDRESS";
    if (row->address_type == 2U) return "BLE PUBLIC IDENTITY ADDRESS";
    if (row->address_type == 3U) return "BLE RANDOM IDENTITY ADDRESS";
    if (row->address_type != 1U) return "BLE ADDRESS TYPE UNKNOWN";
    unsigned kind = row->address[0] >> 6U;
    return kind == 3U ? "BLE RANDOM STATIC ADDRESS" : kind == 1U ?
        "BLE RESOLVABLE PRIVATE ADDRESS" : kind == 0U ?
        "BLE NONRESOLVABLE PRIVATE ADDRESS" : "BLE RANDOM / RESERVED ADDRESS BITS";
}

static inline const char *wardrive_recent_ui_radio(uint8_t radio)
{
    static const char *const names[] = {"C5", "S3", "R1", "R2", "R3", "R4", "R5", "R6"};
    return radio < 8U ? names[radio] : "UNKNOWN";
}

static inline const char *wardrive_recent_ui_band(uint8_t channel)
{
    if (!cerberus_wifi_observation_channel_valid(channel)) return "BAND UNKNOWN";
    return channel <= 14U ? "2.4 GHz" : "5 GHz";
}

static inline const char *wardrive_recent_ui_state(cerberus_wardrive_state_t state)
{
    switch (state) {
    case CERBERUS_WARDRIVE_IDLE: return "IDLE";
    case CERBERUS_WARDRIVE_STARTING: return "STARTING";
    case CERBERUS_WARDRIVE_RUNNING: return "LIVE";
    case CERBERUS_WARDRIVE_STOPPING: return "STOPPING / SAVING";
    case CERBERUS_WARDRIVE_STOPPED: return "STOPPED";
    case CERBERUS_WARDRIVE_ERROR: return "ERROR";
    default: return "STATE UNKNOWN";
    }
}

static inline esp_err_t wardrive_recent_ui_footer(bool detail, bool stop_available)
{
    ESP_RETURN_ON_ERROR(draw_button(8, 426, 148, 44,
        detail ? "BACK TO LIST" : "BACK TO SUMMARY"), TAG, "recent back");
    return draw_button(164, 426, 148, 44, stop_available ? "STOP" : "INACTIVE");
}

typedef struct {
    cerberus_wardrive_recent_snapshot_t snapshot;
    cerberus_wardrive_recent_row_t painted[4], selected;
    uint64_t painted_session_id, painted_revision, selected_revision;
    uint8_t page, painted_count, painted_total;
    bool paint_valid, selected_valid;
    wardrive_recent_ui_view_t view;
} wardrive_recent_ui_store_t;

static inline esp_err_t wardrive_recent_ui_draw(wardrive_recent_ui_store_t *store,
    esp_err_t fallback_error, uint64_t now_ms, bool stop_available)
{
    if (store) { store->paint_valid = false; store->painted_count = 0U; }
    bool detail = store && store->view == WARDRIVE_RECENT_UI_DETAIL && store->selected_valid;
    ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(0, 0, 320, 480, UI_BACKGROUND), TAG, "recent clear");
    ESP_RETURN_ON_ERROR(draw_header(detail ? "NETWORK DETAILS" : "RECENT NETWORKS"), TAG, "recent header");
    char line[53];
    if (store && !detail && !store->snapshot.available && fallback_error == ESP_OK &&
        store->snapshot.view_error == ESP_OK) {
        /* No session has populated this optional cache yet, or an idle cache
         * was explicitly cleared. Neither condition is an allocation fault. */
        ESP_RETURN_ON_ERROR(draw_centered(164, "NO RECENT NETWORKS YET", UI_MUTED, 1), TAG, "recent no history");
        ESP_RETURN_ON_ERROR(draw_centered(188, "START WARDRIVING TO COLLECT RESULTS", UI_MUTED, 1), TAG, "recent collect hint");
        if (store->snapshot.error != ESP_OK) {
            snprintf(line, sizeof(line), "SESSION: %.40s", esp_err_to_name(store->snapshot.error));
            ESP_RETURN_ON_ERROR(draw_centered(228, line, UI_MUTED, 1), TAG, "recent prior session error");
        }
        return wardrive_recent_ui_footer(false, stop_available);
    }
    if (!store || (!detail && (!store->snapshot.available || store->snapshot.count > CERBERUS_WARDRIVE_RECENT_VISIBLE))) {
        ESP_RETURN_ON_ERROR(draw_centered(164, "RECENT NETWORKS UNAVAILABLE", UI_MUTED, 1), TAG, "recent unavailable");
        esp_err_t error = fallback_error != ESP_OK ? fallback_error : store ? store->snapshot.view_error : ESP_ERR_NO_MEM;
        if (error == ESP_OK) error = ESP_ERR_INVALID_STATE;
        ESP_RETURN_ON_ERROR(draw_centered(188, esp_err_to_name(error), UI_MUTED, 1), TAG, "recent allocation error");
        ESP_RETURN_ON_ERROR(draw_centered(228, "WARDRIVING CONTROLS REMAIN AVAILABLE", UI_MUTED, 1), TAG, "recent scope");
        return wardrive_recent_ui_footer(detail, stop_available);
    }
    snprintf(line, sizeof(line), "STATE: %s", wardrive_recent_ui_state(store->snapshot.state));
    ESP_RETURN_ON_ERROR(draw_centered(60, line, UI_TEXT, 1), TAG, "recent state");
    esp_err_t error = fallback_error != ESP_OK ? fallback_error :
        store->snapshot.view_error != ESP_OK ? store->snapshot.view_error : store->snapshot.error;
    if (error != ESP_OK) snprintf(line, sizeof(line), "%s: %.40s",
        fallback_error != ESP_OK || store->snapshot.view_error != ESP_OK ? "VIEW" : "SESSION", esp_err_to_name(error));
    else snprintf(line, sizeof(line), "APPEND ACCEPTED DOES NOT MEAN FILE SAVED");
    ESP_RETURN_ON_ERROR(draw_centered(78, line, UI_MUTED, 1), TAG, "recent error or storage scope");
    if (detail) {
        const cerberus_wardrive_recent_row_t *entry = &store->selected;
        const cerberus_wardrive_observation_t *row = &entry->observation;
        char name[33], address[18];
        wardrive_recent_ui_name(row, name); wardrive_recent_ui_address(row, address);
        snprintf(line, sizeof(line), "%s: %s", row->type == CERBERUS_WARDRIVE_BLE ? "BLE" : "WI-FI",
            name[0] ? name : "[NO NAME]");
        ESP_RETURN_ON_ERROR(draw_centered(108, line, UI_TEXT, 1), TAG, "recent full name");
        for (unsigned part = 0U; part < 2U; ++part) {
            size_t used = (size_t)snprintf(line, sizeof(line), "NAME HEX %02u: ", part * 16U);
            for (unsigned i = part * 16U; i < (part + 1U) * 16U && i < row->label_length && i < 32U; ++i)
                used += (size_t)snprintf(line + used, sizeof(line) - used, "%02X", row->label[i]);
            ESP_RETURN_ON_ERROR(draw_centered(126U + part * 16U, line, UI_MUTED, 1), TAG, "recent raw name");
        }
        ESP_RETURN_ON_ERROR(draw_centered(166, address, UI_TEXT, 2), TAG, "recent address");
        ESP_RETURN_ON_ERROR(draw_centered(188, wardrive_recent_ui_address_kind(row), UI_MUTED, 1), TAG, "recent address kind");
        snprintf(line, sizeof(line), "SOURCE %s / RSSI %d dBm", wardrive_recent_ui_radio(row->radio), row->rssi_dbm);
        ESP_RETURN_ON_ERROR(draw_centered(210, line, UI_TEXT, 1), TAG, "recent radio signal");
        if (row->type == CERBERUS_WARDRIVE_BLE)
            snprintf(line, sizeof(line), "BLE TYPE %u / MANUFACTURER 0x%04X", row->address_type, row->manufacturer_id);
        else snprintf(line, sizeof(line), "%s / CHANNEL %u / SECURITY %u",
            wardrive_recent_ui_band(row->channel), row->channel, row->security);
        ESP_RETURN_ON_ERROR(draw_centered(230, line, UI_MUTED, 1), TAG, "recent radio details");
        uint64_t age;
        if (cerberus_wardrive_recent_age(entry, now_ms, now_ms <= INT64_MAX, &age))
            snprintf(line, sizeof(line), "PROCESSED %llus AGO", (unsigned long long)(age / 1000U));
        else snprintf(line, sizeof(line), "PROCESSING AGE UNAVAILABLE");
        ESP_RETURN_ON_ERROR(draw_centered(256, line, UI_TEXT, 1), TAG, "recent processing age");
        ESP_RETURN_ON_ERROR(draw_centered(274, "PROCESSING TIME IS NOT RF TIME OR UTC", UI_MUTED, 1), TAG, "recent age scope");
        snprintf(line, sizeof(line), "LOG: %s", wardrive_recent_ui_outcome(entry->outcome));
        ESP_RETURN_ON_ERROR(draw_centered(300, line, UI_TEXT, 1), TAG, "recent outcome");
        snprintf(line, sizeof(line), "SESSION %llu", (unsigned long long)entry->session_id);
        ESP_RETURN_ON_ERROR(draw_centered(322, line, UI_MUTED, 1), TAG, "recent identity");
        snprintf(line, sizeof(line), "OBSERVATION %llu", (unsigned long long)entry->sequence);
        ESP_RETURN_ON_ERROR(draw_centered(340, line, UI_MUTED, 1), TAG, "recent observation identity");
        ESP_RETURN_ON_ERROR(draw_centered(366, "FROZEN ROW / LIVE SCANS DO NOT CHANGE IT", UI_MUTED, 1), TAG, "recent frozen row");
        ESP_RETURN_ON_ERROR(draw_centered(390, "NAME AND ADDRESS DO NOT PROVE A DEVICE", UI_MUTED, 1), TAG, "recent identity scope");
        return wardrive_recent_ui_footer(true, stop_available);
    }
    uint8_t total = store->snapshot.count;
    if (store->page * 4U >= total) store->page = 0U;
    snprintf(line, sizeof(line), "%u OBSERVATIONS / PAGE %u OF %u", total,
        store->page + 1U, total ? (total + 3U) / 4U : 1U);
    ESP_RETURN_ON_ERROR(draw_centered(104, line, UI_TEXT, 1), TAG, "recent count");
    ESP_RETURN_ON_ERROR(draw_centered(122, "LATEST RESULTS / 24 OBSERVATION HISTORY", UI_MUTED, 1), TAG, "recent history scope");
    for (uint8_t i = 0U; i < 4U && store->page * 4U + i < total; ++i) {
        cerberus_wardrive_recent_row_t *entry = &store->painted[i];
        *entry = store->snapshot.rows[store->page * 4U + i];
        const cerberus_wardrive_observation_t *row = &entry->observation;
        char name[33], address[18];
        wardrive_recent_ui_name(row, name); wardrive_recent_ui_address(row, address);
        uint16_t y = 150U + i * 54U;
        ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(8, y, 304, 50, UI_PANEL), TAG, "recent row");
        snprintf(line, sizeof(line), "%s: %s", row->type == CERBERUS_WARDRIVE_BLE ? "BLE" : "WI-FI",
            name[0] ? name : address);
        ESP_RETURN_ON_ERROR(cerberus_display_draw_text(14, y + 5U, line, UI_TEXT, UI_PANEL, 1), TAG, "recent row name");
        if (row->type == CERBERUS_WARDRIVE_BLE)
            snprintf(line, sizeof(line), "%s / BLE TYPE %u / %d dBm", wardrive_recent_ui_radio(row->radio), row->address_type, row->rssi_dbm);
        else snprintf(line, sizeof(line), "%s / %s CH%u / %d dBm", wardrive_recent_ui_radio(row->radio),
            wardrive_recent_ui_band(row->channel), row->channel, row->rssi_dbm);
        ESP_RETURN_ON_ERROR(cerberus_display_draw_text(14, y + 20U, line, UI_MUTED, UI_PANEL, 1), TAG, "recent row radio");
        uint64_t age;
        if (cerberus_wardrive_recent_age(entry, now_ms, now_ms <= INT64_MAX, &age)) {
            if (age / 1000U > 99999U)
                snprintf(line, sizeof(line), "PROCESSED >99999s / %s", wardrive_recent_ui_outcome(entry->outcome));
            else snprintf(line, sizeof(line), "PROCESSED %llus / %s", (unsigned long long)(age / 1000U), wardrive_recent_ui_outcome(entry->outcome));
        }
        else snprintf(line, sizeof(line), "AGE UNKNOWN / %s", wardrive_recent_ui_outcome(entry->outcome));
        ESP_RETURN_ON_ERROR(cerberus_display_draw_text(14, y + 35U, line, UI_MUTED, UI_PANEL, 1), TAG, "recent row age outcome");
        ++store->painted_count;
    }
    if (!total) ESP_RETURN_ON_ERROR(draw_centered(216, "NO PROCESSED AP OR BLE OBSERVATIONS YET", UI_MUTED, 1), TAG, "recent empty");
    ESP_RETURN_ON_ERROR(draw_button(8, 374, 148, 44, "PREV"), TAG, "recent previous");
    ESP_RETURN_ON_ERROR(draw_button(164, 374, 148, 44, "NEXT"), TAG, "recent next");
    ESP_RETURN_ON_ERROR(wardrive_recent_ui_footer(false, stop_available), TAG, "recent footer");
    store->painted_total = total;
    store->painted_session_id = store->snapshot.session_id;
    store->painted_revision = store->snapshot.revision;
    store->paint_valid = true;
    return ESP_OK;
}

static inline wardrive_recent_ui_action_t wardrive_recent_ui_tap(
    wardrive_recent_ui_store_t *store, uint16_t x, uint16_t y, bool stop_available)
{
    if (y >= 426U && y < 470U) {
        if (x >= 164U && x < 312U) return stop_available ? WARDRIVE_RECENT_UI_STOP : WARDRIVE_RECENT_UI_NONE;
        if (x >= 8U && x < 156U) {
            if (store && store->view == WARDRIVE_RECENT_UI_DETAIL) {
                store->view = WARDRIVE_RECENT_UI_LIST;
                store->selected_valid = store->paint_valid = false;
                return WARDRIVE_RECENT_UI_REDRAW;
            }
            return WARDRIVE_RECENT_UI_BACK;
        }
    }
    if (!store || store->view != WARDRIVE_RECENT_UI_LIST || !store->paint_valid) return WARDRIVE_RECENT_UI_NONE;
    if (y >= 374U && y < 418U) {
        if (x >= 8U && x < 156U && store->page > 0U) --store->page;
        else if (x >= 164U && x < 312U && (store->page + 1U) * 4U < store->painted_total) ++store->page;
        else return WARDRIVE_RECENT_UI_NONE;
        store->paint_valid = false;
        return WARDRIVE_RECENT_UI_REDRAW;
    }
    if (x >= 8U && x < 312U && y >= 150U && y < 362U && (y - 150U) % 54U < 50U) {
        uint8_t index = (uint8_t)((y - 150U) / 54U);
        if (index < store->painted_count) {
            store->selected = store->painted[index];
            store->selected_revision = store->painted_revision;
            store->selected_valid = true;
            store->paint_valid = false;
            store->view = WARDRIVE_RECENT_UI_DETAIL;
            return WARDRIVE_RECENT_UI_REDRAW;
        }
    }
    return WARDRIVE_RECENT_UI_NONE;
}
