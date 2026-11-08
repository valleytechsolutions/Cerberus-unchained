/* Private, read-only Recon inventory pages. The enclosing UI task owns this
 * on-demand store and serializes model updates. Rendering never queries a
 * radio, starts recording, allocates memory or changes the inventory. */
#include "recon_inventory.h"
#include "sentinel_watch_view.h"

typedef struct {
    cerberus_recon_inventory_t model;
    cerberus_recon_inventory_entry_t painted[4], selected;
    size_t painted_total;
    esp_err_t radio_errors[2];
    uint8_t filter, page, painted_count;
    bool paint_valid, selected_valid;
    bool detail_from_dashboard;
    uint8_t dashboard_page;
    sentinel_watch_dashboard_t dashboard;
} recon_inventory_ui_store_t;
static recon_inventory_ui_store_t *recon_inventory_ui;
static esp_err_t recon_inventory_error;

static void recon_inventory_error_line(char *line, size_t capacity)
{
    unsigned slot = recon_inventory_ui && recon_inventory_ui->radio_errors[0] == ESP_OK ? 1U : 0U;
    const char *radio = recon_inventory_ui && recon_inventory_ui->radio_errors[slot] != ESP_OK ?
        (slot ? "BLE" : "WI-FI") : "LAST";
    esp_err_t error = recon_inventory_ui && recon_inventory_ui->radio_errors[slot] != ESP_OK ?
        recon_inventory_ui->radio_errors[slot] : recon_inventory_error;
    snprintf(line, capacity, "%s BATCH: %.35s", radio, esp_err_to_name(error));
}

static void recon_inventory_name(const cerberus_recon_record_t *record, char out[33])
{
    size_t n = record->name_length;
    if (n > 32U) n = 32U;
    for (size_t i = 0; i < n; ++i)
        out[i] = record->name[i] >= 32U && record->name[i] <= 126U ? (char)record->name[i] : '.';
    out[n] = 0;
}

static void recon_inventory_address(const cerberus_recon_record_t *record, char out[18])
{
    if (privacy_addresses_masked()) {
        if (record->radio == CERBERUS_RECON_RADIO_BLE)
            cerberus_privacy_format_ble_identifier(record->address, true, out, 18U);
        else cerberus_privacy_format_wifi_identifier(record->address, true, out, 18U);
        return;
    }
    const uint8_t *a = record->address;
    bool ble = record->radio == CERBERUS_RECON_RADIO_BLE;
    snprintf(out, 18U, "%02X:%02X:%02X:%02X:%02X:%02X",
        a[ble ? 5 : 0], a[ble ? 4 : 1], a[ble ? 3 : 2],
        a[ble ? 2 : 3], a[ble ? 1 : 4], a[ble ? 0 : 5]);
}

static const char *recon_inventory_age_basis(const cerberus_recon_inventory_entry_t *entry)
{
    if (entry->age_basis != CERBERUS_RECON_INVENTORY_AGE_SOURCE) return "SWEEP COPY / SOURCE TIME UNAVAILABLE";
    switch (entry->age_source_kind) {
    case CERBERUS_RECON_SOURCE_C5_AGE_MS: return "C5 SOURCE AGE UPPER BOUND";
    case CERBERUS_RECON_SOURCE_S3_ADV_MS32: return "S3 ORIGINAL ADV CALLBACK";
    case CERBERUS_RECON_SOURCE_S3_RESPONSE_MS32: return "S3 RESPONSE CALLBACK";
    case CERBERUS_RECON_SOURCE_S3_CALLBACK_US: return "S3 CALLBACK";
    default: return "SOURCE TIME UNAVAILABLE";
    }
}

static esp_err_t recon_inventory_unavailable(const char *title)
{
    ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(0, 0, 320, 480, UI_BACKGROUND), TAG, "inventory clear");
    ESP_RETURN_ON_ERROR(draw_header(title), TAG, "inventory header");
    ESP_RETURN_ON_ERROR(draw_centered(170, "INVENTORY UNAVAILABLE", UI_MUTED, 1), TAG, "inventory unavailable");
    if (recon_inventory_error != ESP_OK)
        ESP_RETURN_ON_ERROR(draw_centered(194, esp_err_to_name(recon_inventory_error), UI_MUTED, 1), TAG, "inventory error");
    ESP_RETURN_ON_ERROR(draw_centered(240, "RECON SCANS AND SD RECORD ARE SEPARATE", UI_MUTED, 1), TAG, "inventory scope");
    return draw_button(64, 426, 192, 44, recon_inventory_ui && watch_page != WATCH_INVENTORY ?
        "BACK TO INVENTORY" : "BACK TO OPTIONS");
}

static esp_err_t recon_draw_inventory(uint64_t now_us)
{
    recon_inventory_ui_store_t *store = recon_inventory_ui;
    if (!store) return recon_inventory_unavailable("NEARBY INVENTORY");
    /* A failed or partial paint cannot leave a tappable row map. Each mapped
     * row is a complete value copy, so later expiry/replacement cannot retarget it. */
    store->paint_valid = false;
    store->painted_count = 0U;
    ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(0, 0, 320, 480, UI_BACKGROUND), TAG, "inventory clear");
    ESP_RETURN_ON_ERROR(draw_header("NEARBY INVENTORY"), TAG, "inventory header");
    const char *labels[] = {"ALL", "WI-FI", "BLE"};
    const uint8_t filters[] = {CERBERUS_RECON_INVENTORY_ALL, CERBERUS_RECON_RADIO_WIFI, CERBERUS_RECON_RADIO_BLE};
    char line[53];
    for (unsigned i = 0; i < 3U; ++i) {
        snprintf(line, sizeof(line), "%s%s", store->filter == filters[i] ? "> " : "", labels[i]);
        ESP_RETURN_ON_ERROR(draw_button(8U + i * 104U, 60, 96, 44, line), TAG, "inventory filter");
    }
    uint8_t indexes[CERBERUS_RECON_INVENTORY_CAPACITY];
    size_t total = cerberus_recon_inventory_indexes(&store->model, store->filter, indexes, sizeof(indexes));
    if ((size_t)store->page * 4U >= total) store->page = 0U;
    snprintf(line, sizeof(line), "%u RETAINED ADDRESSES / PAGE %u OF %u", (unsigned)total,
        (unsigned)store->page + 1U, (unsigned)(total ? (total + 3U) / 4U : 1U));
    ESP_RETURN_ON_ERROR(draw_centered(116, line, UI_TEXT, 1), TAG, "inventory count");
    if (recon_inventory_error != ESP_OK)
        recon_inventory_error_line(line, sizeof(line));
    else snprintf(line, sizeof(line), "NEWEST SWEEP COPY FIRST / NOT DEVICE COUNT");
    ESP_RETURN_ON_ERROR(draw_centered(132, line, UI_MUTED, 1), TAG, "inventory order or error");
    for (unsigned row = 0; row < 4U && (size_t)store->page * 4U + row < total; ++row) {
        cerberus_recon_inventory_entry_t *entry = &store->painted[row];
        *entry = store->model.entries[indexes[(size_t)store->page * 4U + row]];
        const cerberus_recon_record_t *record = &entry->record;
        char name[33], address[18];
        recon_inventory_name(record, name); recon_inventory_address(record, address);
        uint16_t y = 150U + row * 54U;
        ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(8, y, 304, 50, UI_PANEL), TAG, "inventory row");
        snprintf(line, sizeof(line), "%s: %s", record->radio == CERBERUS_RECON_RADIO_BLE ? "BLE" : "WI-FI",
            name[0] ? name : "[NO ADVERTISED NAME]");
        ESP_RETURN_ON_ERROR(cerberus_display_draw_text(14, y + 6U, line, UI_TEXT, UI_PANEL, 1), TAG, "inventory name");
        if (record->rssi_valid) snprintf(line, sizeof(line), "%s / %d dBm", address, record->rssi);
        else snprintf(line, sizeof(line), "%s / RSSI UNKNOWN", address);
        ESP_RETURN_ON_ERROR(cerberus_display_draw_text(14, y + 21U, line, UI_MUTED, UI_PANEL, 1), TAG, "inventory address");
        uint64_t age;
        if (cerberus_recon_inventory_entry_age(entry, now_us, &age))
            snprintf(line, sizeof(line), "%s AGE %llus / %s", entry->age_basis == CERBERUS_RECON_INVENTORY_AGE_SOURCE ?
                (entry->age_source_kind == CERBERUS_RECON_SOURCE_C5_AGE_MS ? "BOUND" : "SOURCE") : "COPY",
                (unsigned long long)(age / UINT64_C(1000000)), record->classified ? "HINT" : "UNCLASSIFIED");
        else snprintf(line, sizeof(line), "AGE UNAVAILABLE / %s", record->classified ? "HINT" : "UNCLASSIFIED");
        ESP_RETURN_ON_ERROR(cerberus_display_draw_text(14, y + 36U, line, UI_MUTED, UI_PANEL, 1), TAG, "inventory age");
        ++store->painted_count;
    }
    if (!total)
        ESP_RETURN_ON_ERROR(draw_centered(214, "NO RETAINED OBSERVATIONS IN THIS FILTER", UI_MUTED, 1), TAG, "inventory empty");
    ESP_RETURN_ON_ERROR(draw_button(8, 374, 96, 44, "PREV"), TAG, "inventory prev");
    ESP_RETURN_ON_ERROR(draw_button(112, 374, 96, 44, "NEXT"), TAG, "inventory next");
    ESP_RETURN_ON_ERROR(draw_button(216, 374, 96, 44, "ACTIVITY"), TAG, "inventory activity");
    ESP_RETURN_ON_ERROR(draw_button(64, 426, 192, 44, "BACK TO OPTIONS"), TAG, "inventory back");
    store->painted_total = total;
    store->paint_valid = true;
    return ESP_OK;
}

static esp_err_t recon_draw_inventory_detail(uint64_t now_us)
{
    if (!recon_inventory_ui || !recon_inventory_ui->selected_valid)
        return recon_inventory_unavailable("OBSERVATION DETAILS");
    const cerberus_recon_inventory_entry_t *entry = &recon_inventory_ui->selected;
    const cerberus_recon_record_t *record = &entry->record;
    char line[53], name[33], address[18];
    recon_inventory_name(record, name); recon_inventory_address(record, address);
    ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(0, 0, 320, 480, UI_BACKGROUND), TAG, "inventory detail clear");
    ESP_RETURN_ON_ERROR(draw_header("OBSERVATION DETAILS"), TAG, "inventory detail header");
    snprintf(line, sizeof(line), "NAME: %s", name[0] ? name : "[NO ADVERTISED NAME]");
    ESP_RETURN_ON_ERROR(draw_centered(64, line, UI_TEXT, 1), TAG, "inventory detail name");
    for (unsigned row = 0; row < 2U; ++row) {
        size_t used = (size_t)snprintf(line, sizeof(line), "NAME HEX %02u: ", row * 16U);
        for (unsigned i = row * 16U; i < (row + 1U) * 16U && i < record->name_length && i < 32U; ++i)
            used += (size_t)snprintf(line + used, sizeof(line) - used, "%02X", record->name[i]);
        ESP_RETURN_ON_ERROR(draw_centered(80U + row * 16U, line, UI_MUTED, 1), TAG, "inventory name bytes");
    }
    snprintf(line, sizeof(line), "%s / %s", record->radio == CERBERUS_RECON_RADIO_BLE ? "BLE" : "WI-FI", address);
    ESP_RETURN_ON_ERROR(draw_centered(120, line, UI_TEXT, 1), TAG, "inventory detail address");
    const char *privacy;
    if (record->radio == CERBERUS_RECON_RADIO_WIFI)
        privacy = record->address[0] & 2U ? "BSSID: LOCALLY ADMINISTERED" : "BSSID: UNIVERSALLY ADMINISTERED";
    else if (record->address_type == 0U || record->address_type == 2U)
        privacy = record->address_type == 2U ? "BLE PUBLIC IDENTITY ADDRESS" : "BLE PUBLIC ADDRESS";
    else if (record->address_type == 3U) privacy = "BLE RANDOM IDENTITY ADDRESS";
    else {
        unsigned kind = record->address[5] >> 6U;
        privacy = kind == 3U ? "BLE RANDOM STATIC ADDRESS" : kind == 1U ?
            "BLE RESOLVABLE PRIVATE ADDRESS" : kind == 0U ? "BLE NONRESOLVABLE PRIVATE ADDRESS" : "BLE RANDOM / RESERVED ADDRESS BITS";
    }
    ESP_RETURN_ON_ERROR(draw_centered(138, privacy, UI_MUTED, 1), TAG, "inventory privacy");
    if (record->rssi_valid) snprintf(line, sizeof(line), "RSSI %d dBm / %s", record->rssi,
        record->radio == CERBERUS_RECON_RADIO_BLE ? "ORIGINAL SWEEP PEAK" : "C5 AP ROW");
    else snprintf(line, sizeof(line), "RSSI UNAVAILABLE");
    ESP_RETURN_ON_ERROR(draw_centered(160, line, UI_TEXT, 1), TAG, "inventory detail rssi");
    if (record->channel_valid) snprintf(line, sizeof(line), "CHANNEL %u / RSSI IS NOT DISTANCE", record->channel);
    else snprintf(line, sizeof(line), "CHANNEL UNAVAILABLE / RSSI IS NOT DISTANCE");
    ESP_RETURN_ON_ERROR(draw_centered(178, line, UI_MUTED, 1), TAG, "inventory detail channel");
    uint64_t age;
    if (cerberus_recon_inventory_entry_age(entry, now_us, &age))
        snprintf(line, sizeof(line), "%s: %llus", entry->age_basis == CERBERUS_RECON_INVENTORY_AGE_SOURCE &&
            entry->age_source_kind == CERBERUS_RECON_SOURCE_C5_AGE_MS ? "AGE UPPER BOUND" : "RETAINED AGE",
            (unsigned long long)(age / UINT64_C(1000000)));
    else snprintf(line, sizeof(line), "RETAINED AGE: UNAVAILABLE");
    ESP_RETURN_ON_ERROR(draw_centered(202, line, UI_TEXT, 1), TAG, "inventory detail age");
    ESP_RETURN_ON_ERROR(draw_centered(218, recon_inventory_age_basis(entry), UI_MUTED, 1), TAG, "inventory age basis");
    ESP_RETURN_ON_ERROR(draw_centered(234, "AGE BASIS IS INDEPENDENT OF LATEST RSSI", UI_MUTED, 1), TAG, "inventory independent source");
    snprintf(line, sizeof(line), "RAW SOURCE %u / TIME %llu", record->source_kind, (unsigned long long)record->source_time);
    ESP_RETURN_ON_ERROR(draw_centered(252, line, UI_MUTED, 1), TAG, "inventory raw source");
    snprintf(line, sizeof(line), "FLAGS %02X / SEQ %lu / LIMITS %02lX", record->source_flags,
        (unsigned long)record->source_sequence, (unsigned long)record->source_limits);
    ESP_RETURN_ON_ERROR(draw_centered(268, line, UI_MUTED, 1), TAG, "inventory source flags");
    if (record->classified) snprintf(line, sizeof(line), "%s / %s HINT",
        sentinel_watch_category_name((sentinel_watch_category_t)record->category),
        sentinel_watch_confidence_name((sentinel_watch_confidence_t)record->confidence));
    else snprintf(line, sizeof(line), "UNCLASSIFIED BROADCAST");
    ESP_RETURN_ON_ERROR(draw_centered(292, line, UI_GOOD, 1), TAG, "inventory classification");
    snprintf(line, sizeof(line), "RULE: %.31s", record->classified ? record->rule : "NONE");
    ESP_RETURN_ON_ERROR(draw_centered(310, line, UI_MUTED, 1), TAG, "inventory rule");
    ESP_RETURN_ON_ERROR(draw_centered(344, "ADDRESS AND HINTS DO NOT PROVE IDENTITY", UI_MUTED, 1), TAG, "inventory identity scope");
    ESP_RETURN_ON_ERROR(draw_centered(366, "FROZEN ROW / SCANS DO NOT CHANGE THIS COPY", UI_MUTED, 1), TAG, "inventory frozen");
    if (recon_inventory_error != ESP_OK)
        recon_inventory_error_line(line, sizeof(line));
    else snprintf(line, sizeof(line), "LIVE INVENTORY AND SD RECORD ARE SEPARATE");
    ESP_RETURN_ON_ERROR(draw_centered(390, line, UI_MUTED, 1), TAG, "inventory record scope or error");
    return draw_button(64, 426, 192, 44, recon_inventory_ui->detail_from_dashboard ? "BACK TO RECON" : "BACK TO INVENTORY");
}

static esp_err_t recon_draw_inventory_activity(uint64_t now_us)
{
    (void)now_us;
    if (!recon_inventory_ui) return recon_inventory_unavailable("INVENTORY ACTIVITY");
    const cerberus_recon_inventory_t *model = &recon_inventory_ui->model;
    cerberus_recon_inventory_bucket_t buckets[CERBERUS_RECON_INVENTORY_BUCKETS];
    size_t count = cerberus_recon_inventory_buckets(model, buckets, CERBERUS_RECON_INVENTORY_BUCKETS);
    ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(0, 0, 320, 480, UI_BACKGROUND), TAG, "inventory activity clear");
    ESP_RETURN_ON_ERROR(draw_header("INVENTORY ACTIVITY"), TAG, "inventory activity header");
    ESP_RETURN_ON_ERROR(draw_centered(64, "ALL RADIOS / LAST 64 ONE-SECOND BUCKETS", UI_MUTED, 1), TAG, "inventory activity scope");
    ESP_RETURN_ON_ERROR(draw_centered(84, "ADDED (GREEN) / EXPIRED (GRAY)", UI_TEXT, 1), TAG, "inventory activity legend");
    uint32_t maximum = 1U;
    for (size_t i = 0; i < count; ++i) {
        if (buckets[i].added > maximum) maximum = buckets[i].added;
        if (buckets[i].expired > maximum) maximum = buckets[i].expired;
    }
    char line[53];
    snprintf(line, sizeof(line), "%u SAMPLES / SCALE %lu%s", (unsigned)count, (unsigned long)maximum,
        maximum == UINT32_MAX ? "+" : "");
    ESP_RETURN_ON_ERROR(draw_centered(108, line, UI_MUTED, 1), TAG, "inventory activity scale");
    ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(32, 134, 256, 126, UI_PANEL), TAG, "inventory graph background");
    for (size_t i = 0; i < count; ++i) {
        uint16_t x = 32U + (CERBERUS_RECON_INVENTORY_BUCKETS - count + i) * 4U;
        uint16_t a = (uint16_t)((uint64_t)buckets[i].added * 120U / maximum);
        uint16_t e = (uint16_t)((uint64_t)buckets[i].expired * 120U / maximum);
        if (a) ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(x, 258U - a, 2, a, UI_GOOD), TAG, "inventory added bar");
        if (e) ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(x + 2U, 258U - e, 2, e, UI_MUTED), TAG, "inventory expired bar");
    }
    if (!count) ESP_RETURN_ON_ERROR(draw_centered(184, "WAITING FOR A COMPLETED SWEEP", UI_MUTED, 1), TAG, "inventory graph empty");
    snprintf(line, sizeof(line), "SESSION ADDED %lu%s / EXPIRED %lu%s", (unsigned long)model->counters.added,
        model->counters.added == UINT32_MAX ? "+" : "", (unsigned long)model->counters.expired,
        model->counters.expired == UINT32_MAX ? "+" : "");
    ESP_RETURN_ON_ERROR(draw_centered(282, line, UI_TEXT, 1), TAG, "inventory activity totals");
    snprintf(line, sizeof(line), "CAPACITY REPLACED %lu%s / INVALID %lu%s", (unsigned long)model->counters.replaced,
        model->counters.replaced == UINT32_MAX ? "+" : "", (unsigned long)model->counters.invalid,
        model->counters.invalid == UINT32_MAX ? "+" : "");
    ESP_RETURN_ON_ERROR(draw_centered(304, line, UI_MUTED, 1), TAG, "inventory replacements");
    ESP_RETURN_ON_ERROR(draw_centered(338, "EXPIRED: OLD EVIDENCE AT COMPLETED SWEEP", UI_MUTED, 1), TAG, "inventory expiry scope");
    ESP_RETURN_ON_ERROR(draw_centered(360, "NOT PHYSICAL DEVICES ARRIVING OR LEAVING", UI_MUTED, 1), TAG, "inventory movement scope");
    if (recon_inventory_error != ESP_OK)
        recon_inventory_error_line(line, sizeof(line));
    else snprintf(line, sizeof(line), "PAUSED OR FAILED SWEEPS DO NOT EXPIRE ROWS");
    ESP_RETURN_ON_ERROR(draw_centered(386, line, UI_MUTED, 1), TAG, "inventory pause scope or error");
    return draw_button(64, 426, 192, 44, "BACK TO INVENTORY");
}

static bool recon_inventory_handle_tap(uint16_t x, uint16_t y, uint64_t now_us)
{
    (void)now_us;
    recon_inventory_ui_store_t *store = recon_inventory_ui;
    if (x >= 64U && x < 256U && y >= 426U && y < 470U) {
        watch_page = store && store->detail_from_dashboard && watch_page == WATCH_INVENTORY_DETAIL ? WATCH_LIVE :
            watch_page == WATCH_INVENTORY || !store ? WATCH_OPTIONS : WATCH_INVENTORY;
        if(store)store->detail_from_dashboard=false;
        if (store) store->paint_valid = false;
        return true;
    }
    if (!store || watch_page != WATCH_INVENTORY) return false;
    if (x >= 8U && x < 312U && (x - 8U) % 104U < 96U && y >= 60U && y < 104U) {
        const uint8_t filters[] = {CERBERUS_RECON_INVENTORY_ALL, CERBERUS_RECON_RADIO_WIFI, CERBERUS_RECON_RADIO_BLE};
        store->filter = filters[(x - 8U) / 104U]; store->page = 0U; store->paint_valid = false;
        return true;
    }
    if (x >= 8U && x < 312U && y >= 150U && y < 362U && (y - 150U) % 54U < 50U) {
        unsigned row = (y - 150U) / 54U;
        if (!store->paint_valid || row >= store->painted_count) return false;
        store->selected = store->painted[row]; store->selected_valid = true;store->detail_from_dashboard=false;
        store->paint_valid = false; watch_page = WATCH_INVENTORY_DETAIL;
        return true;
    }
    if (x >= 8U && x < 312U && (x - 8U) % 104U < 96U && y >= 374U && y < 418U) {
        unsigned action = (x - 8U) / 104U;
        if (action == 2U) watch_page = WATCH_INVENTORY_ACTIVITY;
        else if (!store->paint_valid) return false;
        else if (!action && store->page) --store->page;
        else if (action == 1U && ((size_t)store->page + 1U) * 4U < store->painted_total) ++store->page;
        else return false;
        store->paint_valid = false; return true;
    }
    return false;
}
