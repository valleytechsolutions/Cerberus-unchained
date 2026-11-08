#include <stdint.h>
static void detail_text(uint16_t y, const char *text);
#include "ssid_framebuffer.h"
#include "scan_display_store.h"
#include "s3_wifi_ssid_analysis.h"
#include "sentinel_scan_session.h"
#include "settings.h"
#include "recon_db.h"
#include "ui_widgets.h"
#include "ssid_live_record.h"

#define UI_BACKGROUND 0x0042
#define UI_PANEL 0x08A4
#define UI_MUTED 0x7CB5
#define UI_TEXT 0xE77F
#define UI_GOOD 0x471B
#define UI_BAD 0xF800
#define CERBERUS_LCD_WIDTH 320
#define TAG "ssid-detail-check"
static const cerberus_ui_widget_theme_t theme = {UI_BACKGROUND, UI_PANEL, UI_GOOD, UI_TEXT, UI_MUTED};
#define UI_WIDGET_THEME (&theme)
typedef cerberus_s3_wifi_ssid_analysis_t ui_wifi_ssid_analysis_t;
static ui_wifi_ssid_analysis_t wifi_ssid_analysis;
static uint8_t wifi_analysis_generation, wifi_selected_index, wifi_relation_generation;
static uint8_t wifi_snapshot_band;
static bool wifi_selection_valid;
static uint64_t wifi_relation_publication, wifi_relation_selected_publication;
static uint8_t wifi_relation_selected_bssid[6], wifi_relation_selected_channel;
static cerberus_c5_wifi_result_t wifi_selected_result;
static cerberus_scan_display_store_t store, *live_store;
static live_scan_record_t live_records[SENTINEL_SCAN_TOOL_COUNT];
static cerberus_wifi_scan_status_payload_t wifi_scan_status;
static cerberus_wifi_connection_status_payload_t wifi_connection_status;
static esp_err_t wifi_analysis_error, wifi_trust_error;
static struct { bool settings_available; } ui_config;
static struct { recon_db_result_t result; char name[64]; } vendor_cache;
static unsigned checks, vendor_calls, trust_calls;
static char title[64], summary[80], notice[80], coverage[80];
#define CHECK(value) do { ++checks; assert(value); } while (0)

static void detail_text(uint16_t y, const char *text)
{
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p)
        CHECK(*p >= 32U && *p <= 126U);
    if (y == 54U) snprintf(title, sizeof(title), "%s", text);
    if (y == 164U) snprintf(summary, sizeof(summary), "%s", text);
    if (y == 140U) snprintf(coverage, sizeof(coverage), "%s", text);
    if (y == 172U || y == 190U) snprintf(notice, sizeof(notice), "%s", text);
}
static bool privacy_addresses_masked(void) { return false; }
static void cerberus_privacy_format_wifi_identifier(const uint8_t *mac, bool masked,
    char *out, size_t capacity)
{
    CHECK(!masked);
    snprintf(out, capacity, "%02X:%02X:%02X:%02X:%02X:%02X",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
cerberus_wifi_trust_state_t cerberus_settings_wifi_trust_state(const char *ssid,
    const uint8_t bssid[6], uint8_t security)
{
    CHECK(ssid == wifi_selected_result.ssid && security == wifi_selected_result.security);
    CHECK(!memcmp(bssid, wifi_selected_result.bssid, 6U));
    ++trust_calls; return CERBERUS_WIFI_TRUST_NONE;
}
static void refresh_selected_vendor(bool ble, const uint8_t *mac, uint8_t type, int64_t now)
{
    CHECK(!ble && !type && now == 5000);
    CHECK(!memcmp(mac, wifi_selected_result.bssid, 6U));
    ++vendor_calls;
    vendor_cache.result = RECON_DB_FOUND;
    snprintf(vendor_cache.name, sizeof(vendor_cache.name), "FIXTURE VENDOR");
}
static int64_t esp_timer_get_time(void) { return 5000000; }
static const char *esp_err_to_name(esp_err_t error) { (void)error; return "ESP_ERR_INVALID_STATE"; }
#include "ssid_detail_impl.h"

static void select_row(uint8_t index, bool ordinary)
{
    wifi_selected_index = index;
    wifi_selected_result = store.wifi[index];
    wifi_selection_valid = true;
    wifi_relation_bind_selection(ordinary);
}
static void prepare(const char *name, uint8_t count)
{
    wifi_snapshot_band = CERBERUS_WIFI_SCAN_BAND_ALL;
    memset(&store, 0, sizeof(store));
    memset(live_records, 0, sizeof(live_records));
    live_store = &store;
    wifi_relation_publication = UINT64_C(0x1234567800000042);
    wifi_relation_generation = 7U;
    live_records[SENTINEL_SCAN_WIFI] = (live_scan_record_t){.valid = true, .captured = true,
        .saved_generation = 7U, .generation = 7U, .count = count, .state = SENTINEL_SCAN_COMPLETE};
    wifi_scan_status = (cerberus_wifi_scan_status_payload_t){.state = CERBERUS_WIFI_SCAN_COMPLETE,
        .generation = 7U, .stored_count = count};
    wifi_connection_status.state = CERBERUS_WIFI_CONNECTION_IDLE;
    wifi_trust_error = ESP_OK; ui_config.settings_available = true;
    for (uint8_t i = 0U; i < count; ++i) {
        snprintf(store.wifi[i].ssid, sizeof(store.wifi[i].ssid), "%s", name);
        store.wifi[i].bssid[0] = 2U; store.wifi[i].bssid[5] = i;
        store.wifi[i].channel = i % 2U ? 36U : 1U;
        store.wifi[i].security = i % 2U ? CERBERUS_WIFI_SECURITY_WPA3 : CERBERUS_WIFI_SECURITY_WPA2;
        store.wifi[i].rssi = (int8_t)(-40 - i);
    }
    select_row(0U, true);
}
static void draw_expected(esp_err_t expected)
{
    cerberus_scan_display_store_t before = store;
    cerberus_c5_wifi_result_t selected = wifi_selected_result;
    unsigned prior_vendor = vendor_calls, prior_trust = trust_calls;
    title[0] = summary[0] = notice[0] = '\0';
    memset(&wifi_ssid_analysis, 0xA5, sizeof(wifi_ssid_analysis));
    wifi_analysis_generation = 231U; wifi_analysis_error = ESP_OK;
    CHECK(cerberus_display_fill_rect(0, 0, 320, 480, UI_BACKGROUND) == ESP_OK);
    CHECK(draw_wifi_detail() == ESP_OK);
    CHECK(wifi_analysis_error == expected);
    CHECK(!memcmp(&before, &store, sizeof(store)));
    CHECK(!memcmp(&selected, &wifi_selected_result, sizeof(selected)));
    CHECK(vendor_calls == prior_vendor + 1U && trust_calls == prior_trust + 1U);
    if (expected != ESP_OK) {
        const ui_wifi_ssid_analysis_t zero = {0};
        CHECK(!memcmp(&wifi_ssid_analysis, &zero, sizeof(zero)) && !wifi_analysis_generation);
        CHECK(!summary[0] && notice[0]);
    } else CHECK(wifi_analysis_generation == live_records[SENTINEL_SCAN_WIFI].saved_generation);
}
static void save(const char *prefix, const char *name)
{
    char path[1024]; snprintf(path, sizeof(path), "%s-%s.ppm", prefix, name);
    FILE *stream = fopen(path, "wb"); CHECK(stream != NULL);
    CHECK(fprintf(stream, "P6\n320 480\n255\n") > 0);
    for (unsigned i = 0U; i < 320U * 480U; ++i) {
        uint16_t p = screen[i];
        uint8_t rgb[] = {(uint8_t)((p >> 11) * 255 / 31),
            (uint8_t)(((p >> 5) & 63) * 255 / 63), (uint8_t)((p & 31) * 255 / 31)};
        assert(fwrite(rgb, 1, 3, stream) == 3);
    }
    CHECK(fclose(stream) == 0);
}

static void guard_faults(void)
{
    for (unsigned fault = 0U; fault < 17U; ++fault) {
        prepare("network", 3U);
        switch (fault) {
        case 0: wifi_selection_valid = false; break;
        case 1: live_store = NULL; break;
        case 2: live_records[SENTINEL_SCAN_WIFI].captured = false; break;
        case 3: live_records[SENTINEL_SCAN_WIFI].count = 0U; break;
        case 4: live_records[SENTINEL_SCAN_WIFI].count = CERBERUS_WIFI_SCAN_MAX_RESULTS + 1U; break;
        case 5: wifi_selected_index = 3U; break;
        case 6: wifi_relation_publication = 0U; break;
        case 7: wifi_relation_bind_selection(false); break;
        case 8: ++wifi_relation_publication; break;
        case 9: wifi_relation_selected_channel = 11U; break;
        case 10: wifi_relation_selected_bssid[5] ^= 1U; break;
        case 11: ++live_records[SENTINEL_SCAN_WIFI].saved_generation; break;
        case 12: wifi_selected_result.ssid[0] = 'N'; break;
        case 13: memset(wifi_selected_result.ssid, 'X', sizeof(wifi_selected_result.ssid)); break;
        case 14: store.wifi[0].bssid[5] ^= 1U; break;
        case 15: store.wifi[0].channel = 11U; break;
        case 16: store.wifi[0].security = CERBERUS_WIFI_SECURITY_OPEN; break;
        }
        draw_expected(ESP_ERR_INVALID_STATE);
    }
    prepare("network", 3U);
    memset(store.wifi[2].ssid, 'X', sizeof(store.wifi[2].ssid));
    draw_expected(ESP_ERR_INVALID_RESPONSE);
    prepare("network", 3U);
    memset(live_records, 0, sizeof(live_records));
    draw_expected(ESP_ERR_INVALID_STATE); /* C5 reset invalidated the ordinary bank. */
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    guard_faults();
    prepare("network", CERBERUS_WIFI_SCAN_MAX_RESULTS);
    draw_expected(ESP_OK);
    CHECK(!strcmp(summary, "MATCHING ROWS: 32  2G:16  5G:16"));
    CHECK(wifi_ssid_analysis.peer_count == 4U);
    save(argv[1], "maximum-bank");
    prepare("five only", 3U);
    for (unsigned i = 0; i < 3; ++i) store.wifi[i].channel = 36;
    wifi_snapshot_band = CERBERUS_WIFI_SCAN_BAND_5G;
    select_row(0U, true); draw_expected(ESP_OK);
    CHECK(!strcmp(summary, "MATCHING ROWS: 3  2G:N/A  5G:3"));
    CHECK(!strcmp(coverage, "PASSIVE REVIEW / 5 GHZ ONLY"));
    save(argv[1], "five-only");
    for (unsigned i = 0; i < 3; ++i) store.wifi[i].channel = 6;
    wifi_snapshot_band = CERBERUS_WIFI_SCAN_BAND_2G;
    select_row(0U, true); draw_expected(ESP_OK);
    CHECK(!strcmp(summary, "MATCHING ROWS: 3  2G:3  5G:N/A"));
    CHECK(!strcmp(coverage, "PASSIVE REVIEW / 2.4 GHZ ONLY"));
    prepare("N?me", 3U);
    store.wifi[0].ssid[1] = store.wifi[2].ssid[1] = (char)0x81;
    select_row(0U, true); draw_expected(ESP_OK);
    CHECK(!strcmp(title, "N?me") && wifi_ssid_analysis.total_bssids == 2U);
    save(argv[1], "raw-name");
    select_row(1U, true); draw_expected(ESP_OK);
    CHECK(wifi_ssid_analysis.total_bssids == 1U); /* Same display, different received name. */
    wifi_relation_bind_selection(false); draw_expected(ESP_ERR_INVALID_STATE);
    save(argv[1], "unbound");
    prepare("", 3U); select_row(1U, true); draw_expected(ESP_OK);
    CHECK(!strcmp(title, "<HIDDEN SSID>") && wifi_ssid_analysis.total_bssids == 1U);
    CHECK(!strcmp(notice, "HIDDEN SSIDS CANNOT BE GROUPED"));
    save(argv[1], "hidden");
    prepare("12345678901234567890123456789012", 2U); draw_expected(ESP_OK);
    CHECK(strlen(title) == 32U && wifi_ssid_analysis.total_bssids == 2U);
    save(argv[1], "maximum-name");
    /* Remote status and a failed/canceled new transfer do not replace the held
     * committed snapshot. No generation/state query is needed for comparison. */
    prepare("held snapshot", 3U);
    wifi_scan_status.state = CERBERUS_WIFI_SCAN_RUNNING; wifi_scan_status.generation = 8U;
    live_records[SENTINEL_SCAN_WIFI].generation = 8U;
    live_records[SENTINEL_SCAN_WIFI].state = SENTINEL_SCAN_RUNNING;
    memset(store.wifi_staging, 0x5A, sizeof(store.wifi_staging));
    draw_expected(ESP_OK); CHECK(wifi_ssid_analysis.total_bssids == 3U);
    wifi_scan_status.state = CERBERUS_WIFI_SCAN_ERROR;
    live_records[SENTINEL_SCAN_WIFI].error = ESP_ERR_NOT_FINISHED;
    draw_expected(ESP_OK); CHECK(wifi_ssid_analysis.total_bssids == 3U);
    /* Same byte generation, same selected address, replaced local publication:
     * redraw must invalidate a previously successful analysis immediately. */
    ++wifi_relation_publication;
    wifi_scan_status.state = CERBERUS_WIFI_SCAN_COMPLETE; wifi_scan_status.generation = 7U;
    draw_expected(ESP_ERR_INVALID_STATE);
    save(argv[1], "replaced-publication");
    select_row(2U, true); draw_expected(ESP_OK);
    CHECK(wifi_ssid_analysis.total_bssids == 3U);
    printf("Actual Wi-Fi detail: %u checks; bound snapshot guard, raw-name rendering, held banks and no SPI linkage pass\n", checks);
    return 0;
}
