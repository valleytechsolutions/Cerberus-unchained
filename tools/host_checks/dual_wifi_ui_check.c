#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int esp_err_t;
enum { ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE, ESP_ERR_NO_MEM,
    ESP_ERR_INVALID_RESPONSE, ESP_ERR_NOT_SUPPORTED, ESP_FAIL };
enum { MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2,
    UI_BACKGROUND = 0, UI_PANEL, UI_TEXT, UI_MUTED, UI_BAD };
#define ESP_RETURN_ON_ERROR(call,tag,message) do { esp_err_t test_error = (call); if (test_error) return test_error; } while (0)
#include "dual_wifi_scan.h"
#include "s3_wifi_scanner.h"
#include "ssid_display_impl.h"

static cerberus_dual_wifi_status_t mock_status;
static cerberus_dual_wifi_row_t mock_rows[64];
static unsigned allocations, start_calls, stop_calls, step_calls, draw_calls;
static bool fail_alloc;
static int fail_row = -1;
static unsigned fail_draw_call;
static esp_err_t start_error;
static char drawn[96][70];
static unsigned drawn_count;
static void *heap_caps_calloc(size_t count, size_t size, unsigned caps)
{
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (fail_alloc) return NULL;
    void *p = calloc(count, size); assert(p); ++allocations; return p;
}
static void heap_caps_free(void *p) { if (p) { assert(allocations); --allocations; free(p); } }
esp_err_t cerberus_dual_wifi_scan_start(void)
{
    ++start_calls;
    if (!start_error) { mock_status.state = CERBERUS_DUAL_WIFI_RUNNING; mock_status.cleanup_pending = true; }
    return start_error;
}
void cerberus_dual_wifi_scan_step(void) { ++step_calls; }
void cerberus_dual_wifi_scan_stop(void)
{
    ++stop_calls;
    if (mock_status.state == CERBERUS_DUAL_WIFI_RUNNING || mock_status.rebinding || mock_status.cleanup_pending) {
        mock_status.state = CERBERUS_DUAL_WIFI_STOPPING;
        mock_status.cleanup_pending = true;
    }
}
void cerberus_dual_wifi_scan_get_status(cerberus_dual_wifi_status_t *out) { *out = mock_status; }
esp_err_t cerberus_dual_wifi_scan_get_row(uint8_t index, cerberus_dual_wifi_row_t *out)
{
    memset(out, 0, sizeof(*out));
    if ((int)index == fail_row || index >= mock_status.count) return ESP_ERR_INVALID_STATE;
    *out = mock_rows[index];
    return ESP_OK;
}
static const char *esp_err_to_name(esp_err_t value)
{ return value == ESP_ERR_NO_MEM ? "ESP_ERR_NO_MEM" : value ? "TEST_ERROR" : "ESP_OK"; }
static bool privacy_addresses_masked(void) { return false; }
static void cerberus_privacy_format_wifi_identifier(const uint8_t *address, bool masked,
    char *out, size_t capacity)
{
    (void)masked;
    snprintf(out, capacity, "%02X:%02X:%02X:%02X:%02X:%02X", address[0], address[1],
        address[2], address[3], address[4], address[5]);
}
static const char *wifi_security_text(uint8_t security)
{ return security == CERBERUS_WIFI_SECURITY_OPEN ? "OPEN" : "WPA2"; }
static esp_err_t record_draw(const char *text)
{
    ++draw_calls;
    if (draw_calls == fail_draw_call) return ESP_FAIL;
    if (text && drawn_count < 96U) snprintf(drawn[drawn_count++], 70, "%s", text);
    return ESP_OK;
}
static esp_err_t cerberus_display_fill_rect(unsigned x, unsigned y, unsigned w, unsigned h, unsigned color)
{ (void)color; assert(x + w <= 320U && y + h <= 480U); return record_draw(NULL); }
static esp_err_t cerberus_display_draw_text(unsigned x, unsigned y, const char *text,
    unsigned color, unsigned background, unsigned scale)
{
    (void)color; (void)background;
    assert(x + strlen(text) * 6U * scale <= 320U && y + 8U * scale <= 480U);
    return record_draw(text);
}
static esp_err_t draw_header(const char *text) { return record_draw(text); }
static esp_err_t draw_centered(unsigned y, const char *text, unsigned color, unsigned scale)
{
    assert(strlen(text) * 6U * scale <= 320U);
    return cerberus_display_draw_text((320U - (unsigned)strlen(text) * 6U * scale) / 2U,
        y, text, color, 0, scale);
}
static esp_err_t draw_button(unsigned x, unsigned y, unsigned w, unsigned h, const char *text)
{ assert(x + w <= 320U && y + h <= 480U && strlen(text) * 12U <= w); return record_draw(text); }
static esp_err_t draw_small_button(unsigned x, unsigned y, unsigned w, unsigned h, const char *text)
{ assert(x + w <= 320U && y + h <= 480U && strlen(text) * 6U <= w); return record_draw(text); }

#include "../../s3_main/main/dual_wifi_ui.h"

static void reset(void)
{
    dual_wifi_ui_leave();
    assert(!allocations);
    memset(&mock_status, 0, sizeof(mock_status));
    memset(mock_rows, 0, sizeof(mock_rows));
    start_calls = stop_calls = step_calls = draw_calls = drawn_count = fail_draw_call = 0;
    fail_alloc = false; fail_row = -1; start_error = ESP_OK;
}
static void publish(unsigned count, uint32_t generation)
{
    mock_status = (cerberus_dual_wifi_status_t){.state = CERBERUS_DUAL_WIFI_COMPLETE,
        .generation = generation, .count = (uint8_t)count, .split = true};
    for (unsigned i = 0; i < count && i < 64U; ++i) {
        mock_rows[i] = (cerberus_dual_wifi_row_t){.source = i & 1U ? CERBERUS_DUAL_WIFI_SOURCE_C5 : CERBERUS_DUAL_WIFI_SOURCE_S3,
            .source_index = (uint8_t)(i / 2U), .source_generation = generation,
            .ap = {.bssid = {2, 3, 4, 5, 6, (uint8_t)i}, .rssi = (int8_t)(-30 - (int)i),
                .channel = i & 1U ? 36 : 6, .security = CERBERUS_WIFI_SECURITY_WPA2}};
        snprintf(mock_rows[i].ap.ssid, sizeof(mock_rows[i].ap.ssid), "Example %02u", i);
    }
}
static bool has_text(const char *text)
{
    for (unsigned i = 0; i < drawn_count; ++i) if (!strcmp(drawn[i], text)) return true;
    return false;
}
static void draw_ok(void)
{
    drawn_count = draw_calls = 0;
    assert(dual_wifi_ui_draw() == ESP_OK);
}

static void admission_and_cleanup(void)
{
    reset();
    assert(dual_wifi_ui_enter(true) == ESP_OK && !start_calls);
    assert(dual_wifi_ui_tick(20, true, false) == DUAL_WIFI_UI_NONE && !start_calls);
    assert(dual_wifi_ui_tick(40, true, true) == DUAL_WIFI_UI_REDRAW && start_calls == 1);
    assert(dual_wifi_ui_tick(60, true, true) == DUAL_WIFI_UI_NONE && start_calls == 1);
    draw_ok();
    assert(has_text("STOP"));
    assert(dual_wifi_ui_tap(20, 440) == DUAL_WIFI_UI_BACK && stop_calls == 1);
    dual_wifi_ui_leave();
    assert(!allocations);
    unsigned before = step_calls;
    assert(dual_wifi_ui_tick(80, false, false) == DUAL_WIFI_UI_NONE && step_calls == before + 1U);
    reset();
    fail_alloc = true;
    assert(dual_wifi_ui_enter(true) == ESP_ERR_NO_MEM && !start_calls);
    draw_ok();
    assert(has_text("BACK") && has_text("SCAN VIEW UNAVAILABLE"));
    mock_status.cleanup_pending = true;
    assert(dual_wifi_ui_tap(180, 440) == DUAL_WIFI_UI_REDRAW && stop_calls == 1 && !start_calls);
    assert(dual_wifi_ui_tap(20, 440) == DUAL_WIFI_UI_BACK);
    reset();
    start_error = ESP_ERR_NOT_SUPPORTED;
    assert(dual_wifi_ui_enter(true) == ESP_OK);
    assert(dual_wifi_ui_tick(20, true, true) == DUAL_WIFI_UI_UNSUPPORTED && start_calls == 1);
    assert(dual_wifi_ui_tick(40, true, true) == DUAL_WIFI_UI_NONE && start_calls == 1);
    reset();
}

static void painted_identity_and_handoff(void)
{
    reset(); publish(64, 1);
    assert(dual_wifi_ui_enter(false) == ESP_OK && !start_calls);
    draw_ok();
    assert(dual_wifi_ui_store->painted_count == 4 && dual_wifi_ui_store->filtered_count == 64);
    assert(dual_wifi_ui_tap(100, 390) == DUAL_WIFI_UI_REDRAW);
    assert(dual_wifi_ui_store->page == 1);
    assert(dual_wifi_ui_tap(20, 160) == DUAL_WIFI_UI_NONE); /* New page not painted. */
    draw_ok();
    cerberus_dual_wifi_row_t original = dual_wifi_ui_store->painted[0], selected;
    publish(2, 2); /* A later bank cannot retarget the already-painted row. */
    assert(dual_wifi_ui_tap(20, 160) == DUAL_WIFI_UI_REDRAW);
    assert(dual_wifi_ui_copy_selected(&selected) == ESP_OK && !memcmp(&selected, &original, sizeof(selected)));
    draw_ok();
    assert(has_text("AP TOOLS") && has_text("PACKET COUNT"));
    assert(dual_wifi_ui_tap(180,340)==DUAL_WIFI_UI_OPEN_PACKET_COUNT);
    mock_status.state = CERBERUS_DUAL_WIFI_RUNNING;
    mock_status.cleanup_pending = true;
    assert(dual_wifi_ui_tap(20, 340) == DUAL_WIFI_UI_REDRAW && stop_calls == 1);
    assert(dual_wifi_ui_tick(20, true, true) == DUAL_WIFI_UI_NONE);
    mock_status.state = CERBERUS_DUAL_WIFI_COMPLETE;
    mock_status.cleanup_pending = false;
    assert(dual_wifi_ui_tick(40, true, false) == DUAL_WIFI_UI_NONE);
    assert(dual_wifi_ui_tick(60, true, true) == DUAL_WIFI_UI_OPEN_AP_TOOLS);
    assert(dual_wifi_ui_tick(80, true, true) == DUAL_WIFI_UI_NONE); /* Exactly once. */
    mock_status.rebinding = true;
    assert(dual_wifi_ui_tap(20, 340) == DUAL_WIFI_UI_NONE);
    assert(dual_wifi_ui_tap(20, 440) == DUAL_WIFI_UI_REDRAW && stop_calls == 2);
    memset(&selected, 255, sizeof(selected));
    assert(dual_wifi_ui_copy_selected(&selected) == ESP_ERR_INVALID_STATE && !selected.source);
    assert(dual_wifi_ui_tap(20, 440) == DUAL_WIFI_UI_BACK);
    dual_wifi_ui_leave();
    mock_status.rebinding = false; mock_status.cleanup_pending = false; mock_status.state = CERBERUS_DUAL_WIFI_COMPLETE;
    assert(dual_wifi_ui_enter(false) == ESP_OK);
    draw_ok();
    assert(dual_wifi_ui_store->count == 2 && !start_calls);
    reset();
}

static void filters_and_fail_closed_paint(void)
{
    reset(); publish(64, 1);
    assert(dual_wifi_ui_enter(false) == ESP_OK);
    draw_ok();
    assert(dual_wifi_ui_tap(20, 100) == DUAL_WIFI_UI_REDRAW);
    draw_ok();
    assert(dual_wifi_ui_store->filtered_count == 32);
    for (unsigned i = 0; i < 4U; ++i) assert(dual_wifi_ui_store->painted[i].ap.channel <= 14U);
    assert(dual_wifi_ui_tap(20, 100) == DUAL_WIFI_UI_REDRAW);
    draw_ok();
    for (unsigned i = 0; i < 4U; ++i) assert(dual_wifi_ui_store->painted[i].ap.channel > 14U);
    assert(dual_wifi_ui_tap(180, 100) == DUAL_WIFI_UI_REDRAW);
    draw_ok();
    assert(dual_wifi_ui_store->sort == 1 && dual_wifi_ui_store->page == 0);
    fail_draw_call = 2; draw_calls = 0;
    assert(dual_wifi_ui_draw() == ESP_FAIL && !dual_wifi_ui_store->paint_valid);
    assert(dual_wifi_ui_tap(20, 160) == DUAL_WIFI_UI_NONE);
    fail_draw_call = 0;
    publish(3, 2); fail_row = 1;
    draw_ok();
    assert(!dual_wifi_ui_store->cache_valid && !dual_wifi_ui_store->paint_valid);
    fail_row = -1;
    draw_ok();
    assert(dual_wifi_ui_store->cache_valid);
    dual_wifi_ui_set_error(ESP_FAIL);
    draw_ok();
    assert(has_text("SCAN: TEST_ERROR"));
    publish(65, 3);
    draw_ok();
    assert(!dual_wifi_ui_store->cache_valid);
    reset();
}

static void idle_redraw_budget(void)
{
    reset(); publish(4, 1);
    assert(dual_wifi_ui_enter(false) == ESP_OK); draw_ok();
    unsigned initial_draws = draw_calls;
    for (uint32_t now = 200; now <= 2000; now += 200)
        assert(dual_wifi_ui_tick(now, true, true) == DUAL_WIFI_UI_NONE);
    assert(draw_calls == initial_draws);
    publish(4, 2);
    assert(dual_wifi_ui_tick(2200, true, true) == DUAL_WIFI_UI_REDRAW);
    draw_ok();
    assert(dual_wifi_ui_tick(2400, true, true) == DUAL_WIFI_UI_NONE);
    dual_wifi_ui_set_error(ESP_FAIL);
    assert(dual_wifi_ui_tick(2600, true, true) == DUAL_WIFI_UI_REDRAW);
    draw_ok();
    assert(dual_wifi_ui_tick(2800, true, true) == DUAL_WIFI_UI_NONE);
    reset();
}

static void cached_validation_recovery_hint(void)
{
    reset(); publish(2, 3);
    assert(dual_wifi_ui_enter(false) == ESP_OK); draw_ok();
    assert(dual_wifi_ui_tap(20, 220) == DUAL_WIFI_UI_REDRAW);
    mock_status.rebind_cached = true;
    mock_status.error = ESP_ERR_INVALID_STATE;
    draw_ok();
    assert(has_text("SCAN: TEST_ERROR"));
    assert(has_text("C5 RESULT NOT VERIFIED / SCAN AGAIN"));
    assert(has_text("SCAN AGAIN") && !start_calls && !stop_calls);
    mock_status.rebind_cached = false;
    draw_ok();
    assert(has_text("SCAN: TEST_ERROR") && !has_text("C5 RESULT NOT VERIFIED / SCAN AGAIN"));
    mock_status.rebind_cached = true; mock_status.error = ESP_OK;
    draw_ok();
    assert(!has_text("C5 RESULT NOT VERIFIED / SCAN AGAIN") && !start_calls);
    reset();
}

int main(void)
{
    admission_and_cleanup();
    painted_identity_and_handoff();
    filters_and_fail_closed_paint();
    idle_redraw_budget();
    cached_validation_recovery_hint();
    printf("Dual Wi-Fi UI: geometry, 64-row filters/pages, frozen identities, deferred admission, single handoff, hidden cleanup and OOM Back/Stop passed. PSRAM store=%zu bytes\n", sizeof(dual_wifi_ui_store_t));
    return 0;
}
