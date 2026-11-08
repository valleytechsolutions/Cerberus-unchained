/* The renderer suite supplies only rasterization/platform display calls. The
 * live header and coordinator below are the production implementations. */
#define main survey_renderer_main
#include "wifi_ap_survey_ui_check.c"
#undef main
#include "c5_spi_link.h"
#include "dual_wifi_scan.h"
#include "wifi_ap_survey_protocol.h"
#include "wifi_capture_plan.h"
#include "field_menu.h"
#include "esp_check.h"
#include "survey_route_types.h"

#define MALLOC_CAP_SPIRAM 4U
#define MALLOC_CAP_8BIT 2U
#define UI_BACKGROUND 0U
#define UI_TEXT 0xffffU
#define UI_BAD 0xf800U
#define TAG "survey-host"
#define ESP_ERROR_CHECK(call) CHECK((call) == ESP_OK)

static ui_page_t current_page;
static ui_wifi_mode_t wifi_mode;
static cerberus_field_menu_t field_menu;
static bool ui_capability_blocked, allocation_failure, owners_ready, masked, hold_cleanup;
static const char *ui_capability_blocked_reason;
static uint64_t clock_us;
static unsigned allocations, frees, control_calls[2][8], snapshot_reads[2], backend_calls;
static unsigned saves, c5_steps, reader_cancels, lose_stop;
static esp_err_t c5_info_error, save_error, read_error;
static cerberus_ap_survey_control_t remote[2];
static cerberus_ap_survey_snapshot_t c5_projection;
static uint32_t projection_step;
static bool partial_read;

static void *heap_caps_calloc(size_t count, size_t size, unsigned flags)
{ CHECK(flags == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)); if (allocation_failure) return NULL;
    void *p = calloc(count, size); CHECK(p); ++allocations; return p; }
static void *heap_caps_malloc(size_t size, unsigned flags) { return heap_caps_calloc(1, size, flags); }
static void heap_caps_free(void *p) { if (p) { ++frees; free(p); } }
int64_t esp_timer_get_time(void) { return (int64_t)clock_us; }
static const char *esp_err_to_name(esp_err_t error) { return error == ESP_ERR_NO_MEM ? "NO MEMORY" : "STORAGE ERROR"; }
static bool privacy_addresses_masked(void) { return masked; }
static bool dual_wifi_live_released(void) { return owners_ready; }
static bool dual_wifi_ui_busy(const cerberus_dual_wifi_status_t *status) { (void)status; return !owners_ready; }
void cerberus_dual_wifi_scan_get_status(cerberus_dual_wifi_status_t *out) { memset(out, 0, sizeof(*out)); }
static bool remote_id_takeover_ready(void) { return owners_ready; }
static bool remote_id_keeps_awake(void) { return false; }
static bool mac_monitor_keeps_awake(void) { return false; }
static bool capture_busy(void) { return false; }
static bool station_busy(void) { return false; }
static bool ap_busy(void) { return false; }
static bool multissid_busy(void) { return false; }
static bool pinescan_busy(void) { return false; }
static esp_err_t draw_current_page(void) { return ESP_OK; }
static esp_err_t draw_centered(uint16_t y, const char *text, uint16_t color, uint8_t scale)
{ return cerberus_display_draw_text((uint16_t)((320U - strlen(text) * 6U * scale) / 2U), y, text, color, 0, scale); }
static esp_err_t draw_button(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const char *label)
{ (void)w; (void)h; return cerberus_display_draw_text(x + 4, y + 16, label, 0xffff, 0, 1); }
esp_err_t cerberus_storage_write_wifi_report(const char *data, size_t length, char *name, size_t capacity)
{
    ++saves;
    CHECK(data && length == strlen(data) && strstr(data, "ssid_hex") && strstr(data, "AP,S3,"));
    if (!save_error) snprintf(name, capacity, "WIFI000.CSV");
    return save_error;
}

static void stopped(unsigned slot)
{
    cerberus_ap_survey_disarm(&tables[slot], remote[slot].session_id, 1, clock_us);
    remote[slot].state = CERBERUS_AP_SURVEY_COMPLETE;
    remote[slot].flags = CERBERUS_AP_SURVEY_BOUND | CERBERUS_AP_SURVEY_DATA_READY;
    remote[slot].channel = 0;
}

static esp_err_t exchange(unsigned slot, uint8_t command, const void *query, size_t bytes,
    void *reply, size_t reply_bytes)
{
    CHECK(command >= 208 && command <= 213 && reply_bytes == sizeof(cerberus_ap_survey_control_t));
    ++control_calls[slot][command - 208]; ++backend_calls;
    CHECK(bytes == cerberus_ap_survey_request_bytes(command));
    cerberus_ap_survey_control_t *out = reply;
    memset(out, 0, sizeof(*out));
    if (slot && command == CERBERUS_AP_SURVEY_COMMAND_INFO && c5_info_error) return c5_info_error;
    if (command == CERBERUS_AP_SURVEY_COMMAND_START) {
        const cerberus_ap_survey_start_t *request = query;
        CHECK(request->session_id == remote[slot].session_id + 1U);
        uint64_t mask = cerberus_wifi_capture_plan_band_mask(slot ? CERBERUS_WIFI_CAPTURE_PLAN_5G : CERBERUS_WIFI_CAPTURE_PLAN_2G);
        CHECK(request->channel_mask == mask && request->dwell_ms == 250);
        CHECK(cerberus_ap_survey_begin(&tables[slot], request->session_id, mask, mask));
        uint8_t channel = slot ? 36 : 6;
        CHECK(cerberus_ap_survey_note_channel(&tables[slot], channel, true));
        CHECK(cerberus_ap_survey_arm(&tables[slot], request->session_id, 1, channel, clock_us));
        survey_fixture_ap(&tables[slot], slot + 1U, clock_us);
        survey_fixture_client(&tables[slot], slot + 1U, 1, 1, clock_us);
        remote[slot] = (cerberus_ap_survey_control_t){.session_id = request->session_id,
            .state = CERBERUS_AP_SURVEY_RUNNING, .flags = 15, .channel = channel,
            .revision = tables[slot].live.revision, .ap_count = 1, .relation_count = 1};
    } else if (command != CERBERUS_AP_SURVEY_COMMAND_INFO) {
        CHECK(((const cerberus_ap_survey_request_t *)query)->session_id == remote[slot].session_id);
        if (command == CERBERUS_AP_SURVEY_COMMAND_STOP || command == CERBERUS_AP_SURVEY_COMMAND_RELEASE) {
            if (lose_stop) { --lose_stop; return ESP_ERR_TIMEOUT; }
            if (hold_cleanup) {
                remote[slot].state = CERBERUS_AP_SURVEY_STOPPING;
                remote[slot].flags &= (uint8_t)~CERBERUS_AP_SURVEY_RECEIVING;
            } else {
                stopped(slot);
                if (command == CERBERUS_AP_SURVEY_COMMAND_RELEASE)
                    remote[slot] = (cerberus_ap_survey_control_t){.session_id = remote[slot].session_id};
            }
        }
    }
    *out = remote[slot];
    CHECK(cerberus_ap_survey_control_valid(out, command == CERBERUS_AP_SURVEY_COMMAND_INFO, out->session_id));
    return ESP_OK;
}
esp_err_t cerberus_s3_wifi_ap_survey_exchange(uint8_t command, const void *request, size_t bytes, void *out, size_t out_bytes)
{ return exchange(0, command, request, bytes, out, out_bytes); }
esp_err_t c5_spi_link_wifi_ap_survey_exchange(uint8_t command, const void *request, size_t bytes, void *out, size_t out_bytes)
{ return exchange(1, command, request, bytes, out, out_bytes); }
esp_err_t cerberus_s3_wifi_ap_survey_snapshot(uint64_t session, cerberus_ap_survey_snapshot_t *out)
{ ++snapshot_reads[0]; ++backend_calls; CHECK(session == remote[0].session_id);
    return cerberus_ap_survey_snapshot(&tables[0], clock_us, out) ? ESP_OK : ESP_FAIL; }
esp_err_t c5_spi_link_wifi_ap_survey_reader_begin(cerberus_c5_ap_survey_reader_t *reader,
    uint64_t session, cerberus_ap_survey_snapshot_t *out, uint32_t deadline)
{
    CHECK(session == remote[1].session_id);
    *reader = (cerberus_c5_ap_survey_reader_t){.session_id = session, .staging = out,
        .deadline_ms = deadline, .request_started_us = clock_us};
    CHECK(cerberus_ap_survey_snapshot(&tables[1], clock_us, &c5_projection));
    projection_step = 0; return ESP_OK;
}
esp_err_t c5_spi_link_wifi_ap_survey_reader_step(cerberus_c5_ap_survey_reader_t *reader, bool *complete)
{
    ++backend_calls; ++c5_steps; *complete = false;
    if (read_error) return read_error;
    if (partial_read || ++projection_step < 8U) return ESP_OK;
    ++snapshot_reads[1]; *reader->staging = c5_projection;
    reader->receipt_us = clock_us; *complete = true; return ESP_OK;
}
void c5_spi_link_wifi_ap_survey_reader_cancel(cerberus_c5_ap_survey_reader_t *reader)
{ ++reader_cancels; if (reader->staging) memset(reader->staging, 0, sizeof(*reader->staging)); memset(reader, 0, sizeof(*reader)); }

#include "wifi_ap_survey_live_ui.h"
#include "survey_parent_routes.h"

static void tick(unsigned count, bool asleep)
{
    while (count--) { clock_us += 20000; unsigned before = backend_calls; drawn[0] = 0;
        ap_survey_tick((uint32_t)(clock_us / 1000U), asleep); CHECK(backend_calls - before <= 1); }
}
static void open_page(void)
{
    CHECK(!ap_survey_live); memset(&field_menu, 0, sizeof(field_menu));
    current_page = UI_PAGE_FIELD_MENU; drawn[0] = 0;
    CHECK(cerberus_field_menu_tap(&field_menu, 12, 264) == CERBERUS_FIELD_AP_SURVEY);
    unsigned before = backend_calls;
    actual_field_route(cerberus_field_menu_tap(&field_menu, 100, 280));
    CHECK(current_page == UI_PAGE_WIFI && wifi_mode == UI_WIFI_MODE_AP_SURVEY);
    CHECK(backend_calls == before && !ap_survey_busy());
}
static void tap(unsigned x, unsigned y) { drawn[0] = 0; actual_tap(x, y); }
static void drain(void) { for (unsigned i = 0; i < 1200 && ap_survey_busy(); ++i) tick(1, false); CHECK(!ap_survey_busy()); }

static void running_and_retained(void)
{
    open_page(); tick(30, false); CHECK(!backend_calls);
    owners_ready = false; tap(220, 450); CHECK(ap_survey_busy()); tick(10, false);
    CHECK(!backend_calls && ap_survey_live->pending_start);
    tap(220, 450); CHECK(!ap_survey_busy());
    owners_ready = true; tap(220, 450); tick(150, false);
    CHECK(ap_survey_live->model.banks[0].published && ap_survey_live->model.banks[1].published);
    CHECK(control_calls[0][1] == 1 && control_calls[1][1] == 1);
    CHECK(ap_survey_live->view.ap_count == 2 && ap_survey_live->view.client_count == 2);
    tap(20, 180); CHECK(ap_survey_live->detail && ap_survey_live->model.selected.frozen.source == 1);
    cerberus_ap_survey_record_t selected = ap_survey_live->model.selected.frozen;
    tap(100, 410); CHECK(ap_survey_live->filtered && !ap_survey_live->detail && ap_survey_live->view.page.total == 1);
    tap(20, 180); CHECK(ap_survey_live->detail);
    tap(20, 450); CHECK(!ap_survey_live->detail && ap_survey_live->filtered);
    tap(20, 450); CHECK(ap_survey_live->detail && !ap_survey_live->filtered);
    CHECK(cerberus_ap_survey_record_equal(&selected, &ap_survey_live->model.selected.frozen));
    tap(20, 450); hold_cleanup = true; lose_stop = 1; tap(220, 450); tick(40, false);
    CHECK(ap_survey_busy() && !ap_survey_live->view.start && !ap_survey_live->view.save);
    hold_cleanup = false; drain(); tick(30, false);
    CHECK(ap_survey_live->view.start && ap_survey_live->view.save && !ap_survey_live->view.stop);
    CHECK(ap_survey_live->model.banks[0].snapshot.ap_count == 1);
    unsigned before = backend_calls; tap(260, 410); CHECK(saves == 1 && backend_calls == before);
    CHECK(strstr(ap_survey_live->message, "SAVED WIFI000.CSV"));
    save_error = ESP_FAIL; tap(260, 410); CHECK(strstr(ap_survey_live->message, "STORAGE ERROR")); save_error = 0;
    uint64_t run = ap_survey_live->session.run_id; tap(220, 450); tick(150, false);
    CHECK(ap_survey_live->session.run_id > run && ap_survey_live->model.banks[0].run_id > run);
    hold_cleanup = true; unsigned before_free = frees; actual_launcher(); tick(20, false);
    CHECK(ap_survey_live && frees == before_free && current_page == UI_PAGE_WIFI);
    tap(220, 450); CHECK(!ap_survey_live->pending_start);
    hold_cleanup = false; drain(); CHECK(!ap_survey_live && current_page == UI_PAGE_FIELD_MENU);
}

static void cancellation_and_faults(void)
{
    open_page(); tap(220, 450); partial_read = true; tick(40, false);
    CHECK(ap_survey_live->reader_active); unsigned canceled = reader_cancels;
    hold_cleanup = true; tick(15, true); CHECK(ap_survey_live && !ap_survey_live->pending_start);
    CHECK(reader_cancels > canceled); hold_cleanup = false; tick(100, true);
    CHECK(ap_survey_live && !ap_survey_busy()); partial_read = false;
    unsigned starts = control_calls[0][1] + control_calls[1][1]; tick(40, false);
    CHECK(control_calls[0][1] + control_calls[1][1] == starts);
    actual_launcher(); CHECK(!ap_survey_live);
    c5_info_error = ESP_ERR_NOT_SUPPORTED; open_page(); tap(220, 450); tick(100, false);
    CHECK(ap_survey_live->model.banks[0].published && !ap_survey_live->model.banks[1].published);
    CHECK(!strcmp(ap_survey_live->view.receivers[1].state, "UNSUPPORTED"));
    current_page = UI_PAGE_MAIN; hold_cleanup = true; tick(20, false);
    CHECK(ap_survey_live); hold_cleanup = false; drain(); CHECK(!ap_survey_live && current_page == UI_PAGE_MAIN);
    c5_info_error = 0; allocation_failure = true; open_page(); CHECK(!ap_survey_live);
    tap(20, 450); CHECK(current_page == UI_PAGE_FIELD_MENU); allocation_failure = false;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2); prefix = argv[1]; owners_ready = true; clock_us = 1000000;
    remote[0].session_id = 1; remote[1].session_id = 1;
    running_and_retained(); cancellation_and_faults();
    CHECK(allocations == frees);
    printf("AP/client survey production live UI: %u checks; store=%zu bytes; starts=%u/%u\n",
        checks, sizeof(ap_survey_live_store_t), control_calls[0][1], control_calls[1][1]);
    return 0;
}
