#include <stdint.h>
static void baseline_text(uint16_t y, const char *text);
#include "baseline_framebuffer.h"
#include "scan_display_store.h"
#include "sentinel_baseline.h"
#include "sentinel_scan_session.h"
#include "sentinel_scan_view.h"
#include "settings.h"
#include "ui_widgets.h"
#include "baseline_live_record.h"
#include "baseline_settings.h"
#include <stdarg.h>
#include <limits.h>

#define UI_BACKGROUND 0x0042
#define UI_PANEL 0x08A4
#define UI_MUTED 0x7CB5
#define UI_TEXT 0xE77F
#define UI_GOOD 0x471B
#define UI_BAD 0xF800
#define CERBERUS_LCD_WIDTH 320
#define UI_BACK_Y 420
#define UI_BACK_HEIGHT 44
#define TAG "baseline-ui-check"
#define ESP_ERROR_CHECK(call) assert((call) == ESP_OK)
static const cerberus_ui_widget_theme_t theme = {UI_BACKGROUND, UI_PANEL, UI_GOOD, UI_TEXT, UI_MUTED};
#define UI_WIDGET_THEME (&theme)
enum { UI_PAGE_WIFI, UI_PAGE_TOOLS, UI_PAGE_MAIN, UI_PAGE_BLE };
enum { UI_WIFI_MODE_BASELINE, UI_WIFI_MODE_LIST, UI_WIFI_MODE_MENU, UI_WIFI_MODE_OBSERVED_CLIENTS, UI_WIFI_MODE_DETAIL };
static bool wifi_selection_valid;
enum { UI_BLE_MODE_MENU, UI_BLE_MODE_DETAIL };
static unsigned current_page, wifi_mode, tool_parent_page, ble_mode;
static bool learning_tool_active;
static sentinel_baseline_t wifi_baseline;
static uint64_t wifi_baseline_publication, wifi_relation_publication;
static uint8_t wifi_relation_generation;
static uint8_t wifi_snapshot_band, wifi_baseline_band;
static int64_t wifi_baseline_published_us, live_wifi_published_us, clock_us;
static cerberus_scan_display_store_t store, *live_store;
static live_scan_record_t live_records[SENTINEL_SCAN_TOOL_COUNT];
static uint32_t live_capture_pending, live_last_poll;
static bool live_owned_wifi_observation;
static live_wifi_paint_map_t live_wifi_built, live_wifi_painted;
static uint64_t live_wifi_view_token;
static uint16_t live_page[SENTINEL_SCAN_TOOL_COUNT];
static struct { bool settings_available; } ui_config;
static char wifi_report_buffer[4096], wifi_report_file_name[64], wifi_export_message[64];
static uint16_t wifi_export_color;
static char written[4096], painted[8192];
static unsigned checks, writes, draws, lessons, resets;
static esp_err_t storage_error;
#define CHECK(value) do { ++checks; assert(value); } while (0)
static void baseline_text(uint16_t y, const char *text)
{
    (void)y;
    CHECK(strlen(painted) + strlen(text) + 2U < sizeof(painted));
    strcat(painted, text); strcat(painted, "\n");
}
static int64_t esp_timer_get_time(void) { return clock_us; }
static void live_wifi_capture_reset(void) { ++resets; }
static bool live_is_view(void) { return wifi_mode == UI_WIFI_MODE_LIST; }
static sentinel_scan_tool_t live_page_tool(void) { return wifi_mode != UI_WIFI_MODE_MENU ? SENTINEL_SCAN_WIFI : SENTINEL_SCAN_NONE; }
static const char *tools_group_title(void) { return "TOOLS"; }
static const char *live_scan_action_label(void) { CHECK(false); return "UNEXPECTED SCAN"; }
static void live_toggle_pause(void) { CHECK(false); }
static void open_learning_lesson(unsigned lesson) { CHECK(lesson == 5U); ++lessons; }
static esp_err_t draw_detail_page(void);
static const char *esp_err_to_name(esp_err_t error)
{
    return error == ESP_ERR_INVALID_RESPONSE ? "ESP_ERR_INVALID_RESPONSE" :
        error == ESP_ERR_NO_MEM ? "ESP_ERR_NO_MEM" : "ESP_ERR_INVALID_STATE";
}
static const struct { const char *version; } app_description = {"fixture-rev84"};
#define esp_app_get_description() (&app_description)
esp_err_t cerberus_storage_write_wifi_report(const char *data, size_t length, char *name, size_t capacity)
{
    CHECK(length && length < sizeof(written)); ++writes;
    memcpy(written, data, length); written[length] = '\0';
    if (storage_error == ESP_OK) snprintf(name, capacity, "baseline-test.csv");
    return storage_error;
}
#include "baseline_ui_impl.h"

static esp_err_t draw_detail_page(void)
{
    ++draws;
    painted[0] = '\0';
    CHECK(cerberus_display_fill_rect(0, 0, 320, 480, UI_BACKGROUND) == ESP_OK);
    if (wifi_mode == UI_WIFI_MODE_BASELINE) {
        CHECK(draw_wifi_baseline() == ESP_OK);
        CHECK(actual_footer() == ESP_OK);
    }
    return ESP_OK;
}
static void prepare(uint8_t count)
{
    memset(&store, 0, sizeof(store)); memset(live_records, 0, sizeof(live_records));
    memset(&wifi_trusted, 0, sizeof(wifi_trusted));
    memset(&wifi_baseline, 0, sizeof(wifi_baseline));
    live_store = &store;
    wifi_relation_publication = UINT64_C(0x1234567800000042);
    wifi_relation_generation = 7U;
    wifi_snapshot_band = wifi_baseline_band = CERBERUS_WIFI_SCAN_BAND_ALL;
    live_wifi_published_us = INT64_C(4294967296000) + 1234567;
    clock_us = live_wifi_published_us + 1999999;
    live_records[SENTINEL_SCAN_WIFI] = (live_scan_record_t){.valid=true, .captured=true,
        .saved_generation=7U, .generation=7U, .count=count, .state=SENTINEL_SCAN_COMPLETE};
    settings_ready = ui_config.settings_available = true;
    current_page = UI_PAGE_WIFI; wifi_mode = UI_WIFI_MODE_BASELINE; tool_parent_page = UI_PAGE_MAIN;
    ble_mode = UI_BLE_MODE_MENU;
    learning_tool_active = false; storage_error = ESP_OK;
    wifi_export_message[0] = written[0] = painted[0] = '\0';
    writes = draws = lessons = resets = 0U;
    live_capture_pending = 0U; live_last_poll = 0U;
    live_owned_wifi_observation = false;
    memset(live_page, 0, sizeof(live_page));
    for (size_t i = 0; i < count; ++i) {
        snprintf(store.wifi[i].ssid, sizeof(store.wifi[i].ssid), "=PRIVATE-%u", (unsigned)i);
        store.wifi[i].bssid[0] = 2U; store.wifi[i].bssid[5] = (uint8_t)i;
        store.wifi[i].channel = i % 2U ? 36U : 1U;
        store.wifi[i].security = CERBERUS_WIFI_SECURITY_WPA2;
    }
}
static void trust_row(unsigned row)
{
    CHECK(wifi_trusted.count < CERBERUS_WIFI_TRUST_MAX);
    wifi_trusted_entry_t *entry = &wifi_trusted.entries[wifi_trusted.count++];
    memcpy(entry->ssid, store.wifi[row].ssid, sizeof(entry->ssid));
    memcpy(entry->bssid, store.wifi[row].bssid, 6U);
    entry->security = store.wifi[row].security;
}
static void draw_ok(void)
{
    cerberus_scan_display_store_t prior = store;
    wifi_trusted_record_t trusted = wifi_trusted;
    CHECK(draw_detail_page() == ESP_OK);
    CHECK(!memcmp(&prior, &store, sizeof(store)) && !memcmp(&trusted, &wifi_trusted, sizeof(trusted)));
    CHECK(wifi_baseline.error == ESP_OK && live_wifi_baseline_current());
    CHECK(strstr(painted, "AGE AT COMPARE") && strstr(painted, "WIFI MENU"));
    CHECK(!strstr(painted, "SCAN AGAIN") && !strstr(painted, "UNEXPECTED SCAN"));
    CHECK(!strstr(painted, "SAVE SD"));
}
static void save_preview(const char *prefix, const char *name)
{
    char path[1024]; snprintf(path, sizeof(path), "%s-%s.ppm", prefix, name);
    FILE *stream = fopen(path, "wb"); CHECK(stream != NULL);
    CHECK(fprintf(stream, "P6\n320 480\n255\n") > 0);
    for (unsigned i = 0; i < 320U * 480U; ++i) {
        uint16_t p = screen[i];
        uint8_t rgb[] = {(uint8_t)((p >> 11) * 255 / 31),
            (uint8_t)(((p >> 5) & 63) * 255 / 63), (uint8_t)((p & 31) * 255 / 31)};
        assert(fwrite(rgb, 1, 3, stream) == 3);
    }
    CHECK(fclose(stream) == 0);
}
static void guard_faults(void)
{
    for (unsigned fault = 0U; fault < 9U; ++fault) {
        prepare(2U); trust_row(0U); draw_ok();
        switch (fault) {
        case 0: ui_config.settings_available = false; break;
        case 1: live_records[SENTINEL_SCAN_WIFI].captured = false; break;
        case 2: live_store = NULL; break;
        case 3: wifi_relation_publication = 0U; break;
        case 4: ++wifi_relation_generation; break;
        case 5: ++live_records[SENTINEL_SCAN_WIFI].saved_generation; break;
        case 6: live_wifi_published_us = -1; break;
        case 7: live_records[SENTINEL_SCAN_WIFI].count = 33U; break;
        case 8:
            live_owned_wifi_observation = true;
            memset(&live_wifi_built, 0xA5, sizeof(live_wifi_built));
            memset(&live_wifi_painted, 0x5A, sizeof(live_wifi_painted));
            live_wifi_view_token = 37;
            live_reset_observations();
            CHECK(resets == 1U && !live_wifi_published_us && !live_owned_wifi_observation);
            const live_wifi_paint_map_t cleared = {0};
            CHECK(!memcmp(&live_wifi_built, &cleared, sizeof(cleared)));
            CHECK(!memcmp(&live_wifi_painted, &cleared, sizeof(cleared)));
            CHECK(live_wifi_view_token == 37); /* Reset must not reuse a view token. */
            break;
        }
        CHECK(!live_wifi_baseline_current());
        CHECK(export_wifi_baseline() == ESP_ERR_INVALID_STATE && !writes);
        CHECK(draw_detail_page() == ESP_OK);
        CHECK(wifi_baseline.error != ESP_OK && !wifi_baseline.compared && !wifi_baseline.saved);
        CHECK(!wifi_baseline_publication && !wifi_baseline_published_us);
        CHECK(export_wifi_baseline() == ESP_ERR_INVALID_STATE && !writes);
    }
    prepare(32U); memset(store.wifi[31].ssid, 'X', sizeof(store.wifi[31].ssid));
    CHECK(draw_detail_page() == ESP_OK && wifi_baseline.error == ESP_ERR_INVALID_RESPONSE);
    CHECK(!wifi_baseline.compared && !wifi_baseline_publication && !live_wifi_baseline_current());
}
static void publication_and_export(void)
{
    prepare(3U); trust_row(0U); store.wifi[1] = store.wifi[0];
    strcpy(store.wifi[1].ssid, "RENAMED-PRIVATE");
    draw_ok(); CHECK(wifi_baseline.exact == 1U && wifi_baseline.review == 1U && wifi_baseline.other == 1U);
    CHECK(export_wifi_baseline() == ESP_OK && writes == 1U);
    char expected[96];
    snprintf(expected, sizeof(expected), "snapshot_publication,%llu\r\n", (unsigned long long)wifi_relation_publication);
    CHECK(strstr(written, expected));
    snprintf(expected, sizeof(expected), "snapshot_uptime_us,%llu\r\n", (unsigned long long)live_wifi_published_us);
    CHECK(strstr(written, expected) && strstr(written, "related_trust_differences,1\r\n"));
    CHECK(strstr(written, "not RF receive time or UTC") && !strstr(written, "observed_at"));
    CHECK(!strstr(written, "PRIVATE") && !strstr(written, "02:00") && !strstr(written, "same_name_differences"));
    uint64_t prior_publication = wifi_relation_publication;
    ++wifi_relation_publication; /* Same gen8/count/time, a new unseen publication. */
    CHECK(!live_wifi_baseline_current());
    CHECK(export_wifi_baseline() == ESP_ERR_INVALID_STATE && writes == 1U);
    CHECK(wifi_baseline_publication == prior_publication);
    draw_ok(); CHECK(wifi_baseline_publication == wifi_relation_publication);
    CHECK(export_wifi_baseline() == ESP_OK && writes == 2U);
    ++live_wifi_published_us; /* The full-width publication timestamp is also bound. */
    CHECK(export_wifi_baseline() == ESP_ERR_INVALID_STATE && writes == 2U);
    draw_ok(); storage_error = ESP_ERR_NO_MEM;
    CHECK(export_wifi_baseline() == ESP_ERR_NO_MEM && writes == 3U);
    CHECK(strstr(wifi_export_message, "SAVE FAILED: ESP_ERR_NO_MEM") && wifi_export_color == UI_BAD);
}
static void held_snapshot_and_time(void)
{
    prepare(2U); trust_row(0U); draw_ok();
    sentinel_baseline_t retained = wifi_baseline;
    uint64_t publication = wifi_baseline_publication;
    int64_t published = wifi_baseline_published_us;
    for (unsigned state = SENTINEL_SCAN_IDLE; state <= SENTINEL_SCAN_ERROR; ++state) {
        live_records[SENTINEL_SCAN_WIFI].state = (uint8_t)state;
        live_records[SENTINEL_SCAN_WIFI].generation = 8U;
        live_records[SENTINEL_SCAN_WIFI].valid = false;
        live_records[SENTINEL_SCAN_WIFI].error = ESP_FAIL;
        memset(store.wifi_staging, 0xA5, sizeof(store.wifi_staging));
        draw_ok();
        CHECK(!memcmp(&wifi_baseline, &retained, sizeof(retained)));
        CHECK(wifi_baseline_publication == publication && wifi_baseline_published_us == published);
    }
    clock_us = published - 1; draw_ok(); CHECK(strstr(painted, "AGE AT COMPARE UNKNOWN"));
    clock_us = -1; draw_ok(); CHECK(strstr(painted, "AGE AT COMPARE UNKNOWN"));
    prepare(0U); live_store = NULL; live_wifi_published_us = clock_us = 0;
    draw_ok(); CHECK(strstr(painted, "0 ROWS / AGE AT COMPARE 0s"));
    CHECK(export_wifi_baseline() == ESP_OK && strstr(written, "snapshot_uptime_us,0\r\n"));
    clock_us = INT64_MAX; draw_ok(); CHECK(!strstr(painted, "UNKNOWN"));
    live_wifi_published_us = clock_us = INT64_MAX; draw_ok();
    CHECK(strstr(painted, "AGE AT COMPARE 0s"));
}
static void restricted_coverage(void)
{
    prepare(2U); trust_row(0U); trust_row(1U);
    const wifi_trusted_record_t original = wifi_trusted;
    const int64_t received = live_wifi_published_us;
    /* Present only the 5GHz bank; the older saved 2.4GHz trust entry survives. */
    store.wifi[0] = store.wifi[1];
    live_records[SENTINEL_SCAN_WIFI].count = 1;
    wifi_snapshot_band = CERBERUS_WIFI_SCAN_BAND_5G;
    clock_us = received + INT64_C(900000000);
    draw_ok();
    CHECK(wifi_baseline.saved == 2 && wifi_baseline.compared == 1 && wifi_baseline.exact == 1);
    CHECK(!memcmp(&wifi_trusted, &original, sizeof(original)));
    CHECK(strstr(painted, "TRUSTED BASELINE / 5 GHZ ONLY"));
    CHECK(strstr(painted, "Other bands were not compared."));
    CHECK(strstr(painted, "AGE AT COMPARE 900s"));
    CHECK(export_wifi_baseline() == ESP_OK && strstr(written, "coverage,5 GHZ ONLY\r\n"));
    CHECK(strstr(written, "Original S3 snapshot receipt time"));
    wifi_snapshot_band = CERBERUS_WIFI_SCAN_BAND_ALL;
    CHECK(!live_wifi_baseline_current());
    CHECK(export_wifi_baseline() == ESP_ERR_INVALID_STATE);
    draw_ok();
    CHECK(export_wifi_baseline() == ESP_OK && strstr(written, "coverage,2.4 + 5 GHZ\r\n"));
    CHECK(!memcmp(&wifi_trusted, &original, sizeof(original)));
}
static void controls(void)
{
    prepare(2U); draw_ok();
    unsigned before = draws;
    actual_body_tap(90U, 363U); actual_body_tap(90U, 406U); actual_body_tap(78U, 380U);
    CHECK(draws == before && !writes && !lessons);
    trust_row(0U); actual_body_tap(90U, 380U);
    CHECK(draws == before + 1U && wifi_baseline.exact == 1U);
    actual_body_tap(170U, 380U); CHECK(writes == 1U);
    ++wifi_relation_publication; actual_body_tap(170U, 380U);
    CHECK(writes == 1U && strstr(wifi_export_message, "COMPARISON CHANGED"));
    actual_body_tap(170U, 380U); CHECK(writes == 2U);
    actual_body_tap(250U, 380U); CHECK(lessons == 1U);
    actual_body_tap(20U, 380U); CHECK(wifi_mode == UI_WIFI_MODE_LIST);
    prepare(2U); draw_ok(); actual_footer_tap(200U, 419U); CHECK(wifi_mode == UI_WIFI_MODE_BASELINE);
    actual_footer_tap(200U, 430U); CHECK(wifi_mode == UI_WIFI_MODE_MENU);
    prepare(2U); draw_ok(); actual_footer_tap(20U, 430U); CHECK(wifi_mode == UI_WIFI_MODE_MENU);
    /* An unrelated retained BLE detail mode must not select its split SAVE SD
     * footer while the current page is the Wi-Fi baseline. */
    prepare(2U); ble_mode = UI_BLE_MODE_DETAIL; draw_ok();
    CHECK(current_page == UI_PAGE_WIFI && strstr(painted, "WIFI MENU"));
    actual_footer_tap(220U, 430U);
    CHECK(wifi_mode == UI_WIFI_MODE_MENU && ble_mode == UI_BLE_MODE_DETAIL);
    prepare(2U); ui_config.settings_available = settings_ready = false;
    CHECK(draw_detail_page() == ESP_OK && strstr(painted, "TRUST STORAGE UNAVAILABLE"));
    actual_body_tap(170U, 380U); CHECK(!writes);
    actual_footer_tap(200U, 430U); CHECK(wifi_mode == UI_WIFI_MODE_MENU);
}
int main(int argc, char **argv)
{
    CHECK(argc == 2);
    guard_faults(); publication_and_export(); held_snapshot_and_time(); restricted_coverage(); controls();
    prepare(32U); for (unsigned i = 0; i < 16U; ++i) trust_row(i);
    draw_ok(); save_preview(argv[1], "maximum-bank");
    prepare(3U); draw_ok(); CHECK(wifi_baseline.saved == 0U && wifi_baseline.other == 3U);
    save_preview(argv[1], "zero-trusted");
    prepare(0U); live_store = NULL; draw_ok(); save_preview(argv[1], "empty-completed");
    prepare(3U); ui_config.settings_available = settings_ready = false;
    CHECK(draw_detail_page() == ESP_OK && strstr(painted, "TRUST STORAGE UNAVAILABLE"));
    CHECK(!strstr(painted, "OTHER NETWORKS")); save_preview(argv[1], "settings-unavailable");
    prepare(3U); live_records[SENTINEL_SCAN_WIFI].captured = false;
    CHECK(draw_detail_page() == ESP_OK && strstr(painted, "START WITH A SCAN"));
    save_preview(argv[1], "no-snapshot");
    prepare(3U); clock_us = live_wifi_published_us - 1; draw_ok(); save_preview(argv[1], "clock-rollback");
    printf("Cached baseline UI: %u checks; exact publication, render/controls, privacy export and snapshot-time bounds pass\n", checks);
    return 0;
}
