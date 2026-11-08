#include "ssid_list_session.h"
#include "ble_archive_io.h"
#include "keyboard.h"
#include "esp_heap_caps.h"
#include "freertos/semphr.h"
#include "ssid_route_types.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks, allocations, frees, live, draws, starts, copies, cancels, releases;
static int mutex_token, locked;
static bool fail_alloc, fail_start, assigned_start_failure, fail_copy, fail_lock, held, release_requested;
static unsigned fail_nth_alloc, ignore_release, lost_status;
static uint64_t next_job = 100U, last_canceled, last_released;
static cerberus_ble_archive_io_status_t io;
static cerberus_wifi_marauder_ssid_t loaded, saved;
#define CHECK(expression) do { ++checks; assert(expression); } while (0)

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &mutex_token; }
int xSemaphoreTake(SemaphoreHandle_t mutex, uint32_t ticks)
{ CHECK(mutex == &mutex_token && !locked && ticks == 250U); if (fail_lock) return 0; locked = 1; return 1; }
int xSemaphoreGive(SemaphoreHandle_t mutex)
{ CHECK(mutex == &mutex_token && locked); locked = 0; return 1; }
void *heap_caps_calloc(size_t count, size_t size, unsigned caps)
{
    CHECK(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (fail_alloc || (fail_nth_alloc && --fail_nth_alloc == 0)) return NULL;
    void *p = calloc(count, size); CHECK(p != NULL); ++live; ++allocations;
    CHECK(live <= 3U); return p;
}
void heap_caps_free(void *pointer)
{ if (pointer) { CHECK(live != 0U); --live; ++frees; free(pointer); } }

static esp_err_t begin(cerberus_ble_archive_io_operation_t operation, uint64_t *job)
{
    CHECK(locked && job); *job = 0U; ++starts;
    if (held || io.worker_active) return ESP_ERR_INVALID_STATE;
    if (fail_start) return ESP_ERR_NO_MEM;
    io = (cerberus_ble_archive_io_status_t){.job_id = ++next_job, .operation = operation,
        .format = CERBERUS_BLE_ARCHIVE_IO_FORMAT_SSID_TEXT,
        .state = CERBERUS_BLE_ARCHIVE_IO_STARTING, .worker_active = true};
    held = true; release_requested = false; *job = io.job_id;
    return assigned_start_failure ? ESP_ERR_NO_MEM : ESP_OK;
}
static void finish(bool success)
{
    CHECK(io.worker_active); io.worker_active = false;
    io.state = success ? CERBERUS_BLE_ARCHIVE_IO_COMPLETE : CERBERUS_BLE_ARCHIVE_IO_ERROR;
    io.error = success ? ESP_OK : ESP_FAIL; io.result_ready = success && !release_requested;
    held = io.result_ready;
    if (io.operation == CERBERUS_BLE_ARCHIVE_IO_SAVE_SSID_TEXT) strcpy(io.file_name, "S00101.TXT");
}
esp_err_t cerberus_ble_archive_io_load_ssid_text(const char *path, const char *name, uint64_t *job)
{ CHECK(!strcmp(path, "/CERBERUS/WIFI") && !strcmp(name, "source.txt")); return begin(CERBERUS_BLE_ARCHIVE_IO_LOAD_SSID_TEXT, job); }
esp_err_t cerberus_ble_archive_io_save_ssid_text(const cerberus_wifi_marauder_ssid_t *document, uint64_t *job)
{ saved = *document; return begin(CERBERUS_BLE_ARCHIVE_IO_SAVE_SSID_TEXT, job); }
void cerberus_ble_archive_io_get_status(cerberus_ble_archive_io_status_t *out)
{ CHECK(locked); if (lost_status) { --lost_status; memset(out, 0, sizeof(*out)); } else *out = io; }
esp_err_t cerberus_ble_archive_io_copy_ssid_text(uint64_t job, cerberus_wifi_marauder_ssid_t *out)
{
    CHECK(locked && job == io.job_id && held && io.result_ready && !io.worker_active); ++copies;
    if (fail_copy) return ESP_ERR_TIMEOUT;
    *out = loaded; return ESP_OK;
}
void cerberus_ble_archive_io_cancel(uint64_t job)
{ CHECK(locked && job); ++cancels; last_canceled = job; if (job == io.job_id && io.worker_active) io.cancel_requested = true; }
void cerberus_ble_archive_io_release(uint64_t job)
{
    CHECK(locked && job); ++releases; last_released = job;
    if (ignore_release) { --ignore_release; return; }
    if (job != io.job_id) return;
    if (io.worker_active) { release_requested = true; io.cancel_requested = true; }
    else { held = false; io.result_ready = false; }
}

/* Rendering/keyboard are boundary doubles; actual page/controller code below
 * handles the taps, paint revisions, preview copies and navigation. */
static char drawn[32768], file_message[128];
static bool privacy;
enum { UI_BACKGROUND, UI_BAD, UI_MUTED, UI_TEXT, UI_PANEL, UI_GOOD };
#define TAG "SSID routes"
#define ESP_RETURN_ON_ERROR(call, tag, message) do { esp_err_t e_ = (call); if (e_ != ESP_OK) return e_; } while (0)
#define ESP_ERROR_CHECK(call) CHECK((call) == ESP_OK)
static const char *esp_err_to_name(esp_err_t error)
{ return error == ESP_ERR_NO_MEM ? "ESP_ERR_NO_MEM" : error == ESP_ERR_TIMEOUT ? "ESP_ERR_TIMEOUT" : "ESP_ERROR"; }
static bool privacy_addresses_masked(void) { return privacy; }
static void text(const char *value)
{ CHECK(value != NULL); size_t n = strlen(drawn); CHECK(n + strlen(value) + 2U < sizeof(drawn)); strcat(drawn, value); strcat(drawn, "\n"); }
static esp_err_t cerberus_display_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{ (void)color; CHECK(x + w <= 320U && y + h <= 480U); return ESP_OK; }
static esp_err_t cerberus_display_draw_text(uint16_t x, uint16_t y, const char *value, uint16_t color, uint16_t bg, uint8_t size)
{ (void)x; (void)y; (void)color; (void)bg; (void)size; text(value); return ESP_OK; }
static esp_err_t draw_header(const char *title) { text(title); return ESP_OK; }
static esp_err_t draw_centered(uint16_t y, const char *value, uint16_t color, uint8_t size)
{ (void)y; (void)color; (void)size; text(value); return ESP_OK; }
static esp_err_t draw_small_button(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const char *label)
{ CHECK(x + w <= 320U && y + h <= 480U); text(label); return ESP_OK; }
static esp_err_t draw_button(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const char *label)
{ return draw_small_button(x, y, w, h, label); }
esp_err_t cerberus_keyboard_begin(cerberus_keyboard_t *k, char *buffer, size_t capacity, const char *initial)
{ snprintf(buffer, capacity, "%s", initial); *k = (cerberus_keyboard_t){.buffer=buffer,.capacity=capacity,.length=strlen(buffer)}; return ESP_OK; }
esp_err_t cerberus_keyboard_draw(const cerberus_keyboard_t *k, const char *prompt)
{ (void)k; text(prompt); return ESP_OK; }
esp_err_t cerberus_keyboard_handle_tap(cerberus_keyboard_t *k, uint16_t x, uint16_t y, cerberus_keyboard_action_t *action)
{ (void)k; (void)x; (void)y; *action = CERBERUS_KEYBOARD_ACTION_NONE; return ESP_OK; }
void esp_fill_random(void *out, size_t length) { memset(out, 0x5a, length); }

static ui_page_t current_page = UI_PAGE_MAIN, tool_parent_page = UI_PAGE_MAIN;
static ui_file_mode_t file_mode;
static ui_wifi_mode_t wifi_mode = UI_WIFI_MODE_DETAIL;
static size_t file_page_offset, file_entry_count;
static bool file_listing_checked;
static char file_path[512], file_selected_path[512], file_action_message[64];
static struct file_entry { char name[256]; bool is_directory; } file_entries[4], file_selected_entry;
static esp_err_t storage_last_error = ESP_OK;
static bool dual_wifi_return_to_view, learning_tool_active, ui_capability_blocked, ble_attack_capture_valid;
static const char *ui_capability_blocked_reason;
static bool dual_view_ready;
static unsigned dual_reopens;
static cerberus_wifi_archive_row_t retained_dual_rows[2], displayed_dual_rows[2];
#define CERBERUS_STORAGE_WIFI_ARCHIVE_PATH "/CERBERUS/WIFI"
#define UI_FILE_LIST_Y 100U
#define UI_FILE_VISIBLE_ROWS 4U
#define UI_FILE_ROW_HEIGHT 50U
static esp_err_t draw_detail_page(void);
static esp_err_t draw_current_page(void);
static bool wifi_interchange_can_open(const char *name, bool ap_json);
static void dual_wifi_live_open(bool start_scan)
{
    /* This boundary reopens the view over an existing publication. A true
     * argument would schedule another RF scan and must fail this route test. */
    CHECK(!start_scan && current_page == UI_PAGE_WIFI && wifi_mode == UI_WIFI_MODE_DUAL);
    ++dual_reopens; dual_wifi_return_to_view = false; dual_view_ready = true;
    memcpy(displayed_dual_rows, retained_dual_rows, sizeof(displayed_dual_rows));
}
static void file_set_action_status(const char *message, bool success)
{ CHECK(!success); snprintf(file_message, sizeof(file_message), "%s", message); }
#include "ssid_list_live_ui.h"
static esp_err_t draw_detail_page(void)
{
    ++draws; drawn[0] = 0;
    if (current_page == UI_PAGE_WIFI && wifi_mode == UI_WIFI_MODE_DUAL) CHECK(dual_view_ready);
    return current_page == UI_PAGE_SSID_LIST ? ssid_lists_draw() : ESP_OK;
}
static esp_err_t draw_current_page(void) { return draw_detail_page(); }

/* The actual generic Files-to-launcher and file row branches are extracted
 * from ui.c. Any unrelated archive/radio route is a fixture failure. */
static bool sae_ui_is_open(void) { return false; }
static bool eapol_ui_is_open(void) { return false; }
static void sae_ui_close(bool back) { (void)back; CHECK(false); }
static void eapol_ui_close(bool back) { (void)back; CHECK(false); }
static void wifi_fox_hunt_back(void) { CHECK(false); }
enum { CHANNEL_ANALYZER_BACK };
static void channel_analyzer_request(unsigned action) { (void)action; CHECK(false); }
static void dual_wifi_ui_leave(void) { CHECK(false); }
static void navigate_file_directory(const char *name) { (void)name; CHECK(false); }
#define UNRELATED_FILE_TYPE(name) static bool name(const char *n) { (void)n; CHECK(false); return false; }
UNRELATED_FILE_TYPE(mesh_archive_is_file)
UNRELATED_FILE_TYPE(wifi_archive_is_file)
UNRELATED_FILE_TYPE(ble_archive_is_file)
UNRELATED_FILE_TYPE(cerberus_bmp_is_file_name)
#define UNRELATED_FILE_OPEN(name) static void name(const char *p, const char *n) { (void)p; (void)n; CHECK(false); }
UNRELATED_FILE_OPEN(mesh_archive_open_file)
UNRELATED_FILE_OPEN(wifi_archive_open_file)
UNRELATED_FILE_OPEN(ble_archive_open_file)
static void open_image_file(const char *name) { (void)name; CHECK(false); }
static void open_text_file(const char *name) { (void)name; CHECK(false); }
#include "ssid_file_routes.h"

static cerberus_ssid_list_session_status_t status(void)
{ cerberus_ssid_list_session_status_t s; CHECK(cerberus_ssid_list_session_poll(&s) == ESP_OK); return s; }
static cerberus_ssid_list_t snapshot(void)
{ cerberus_ssid_list_t s; CHECK(cerberus_ssid_list_session_copy(&s) == ESP_OK); CHECK(cerberus_ssid_list_valid(&s)); return s; }
static void unchanged(const cerberus_ssid_list_t *before)
{ cerberus_ssid_list_t after = snapshot(); CHECK(!memcmp(before, &after, sizeof(after))); }
static void document(cerberus_wifi_marauder_ssid_t *out, const char *value)
{ CHECK(cerberus_wifi_marauder_ssid_decode((const uint8_t *)value, strlen(value), out) == CERBERUS_WIFI_MARAUDER_OK); }
static void add(const char *value)
{
    cerberus_ssid_list_t before = snapshot();
    cerberus_ssid_list_edit_t edit = {.kind=CERBERUS_SSID_EDIT_ADD,.bytes=(const uint8_t *)value,.length=strlen(value)};
    CHECK(cerberus_ssid_list_session_edit(before.revision, &edit) == ESP_OK);
}
static void repaint(void) { CHECK(draw_detail_page() == ESP_OK); }
static void back(void) { ssid_lists_tap(40, 440); ssid_lists_tick(false); }
static void picker(void)
{
    CHECK(current_page == UI_PAGE_SSID_LIST); repaint(); ssid_lists_tap(150, 370);
    CHECK(current_page == UI_PAGE_FILES && tool_parent_page == UI_PAGE_SSID_LIST && ssid_lists_file_picker);
    CHECK(!strcmp(file_path, CERBERUS_STORAGE_WIFI_ARCHIVE_PATH) && !file_page_offset && !file_listing_checked);
}
static void pick(const char *name)
{
    memset(file_entries, 0, sizeof(file_entries)); strcpy(file_entries[0].name, name); file_entry_count = 1;
    actual_file_row_tap(40, UI_FILE_LIST_Y + 10U);
}
static void open_from(ui_page_t page, ui_page_t parent)
{ current_page = page; tool_parent_page = parent; ssid_lists_open(); CHECK(current_page == UI_PAGE_SSID_LIST); }

int main(void)
{
    /* Nothing is allocated or started for an unused page. */
    ssid_lists_tick(false); ssid_lists_tick(true);
    CHECK(!allocations && !starts && !status().allocated);
    fail_alloc = true; open_from(UI_PAGE_TOOLS, UI_PAGE_MAIN);
    CHECK(!ssid_lists_view && !live && strstr(drawn, "NO MEMORY"));
    back(); CHECK(current_page == UI_PAGE_TOOLS && tool_parent_page == UI_PAGE_MAIN);
    fail_alloc = false;

    /* View allocation can succeed while the optional workspace fails. */
    fail_nth_alloc = 2; open_from(UI_PAGE_TOOLS, UI_PAGE_MAIN);
    CHECK(ssid_lists_view && !status().allocated && strstr(drawn, "WORKSPACE UNAVAILABLE"));
    back(); CHECK(!ssid_lists_view && !live && current_page == UI_PAGE_TOOLS);
    open_from(UI_PAGE_TOOLS, UI_PAGE_MAIN);
    CHECK(live == 2U && status().allocated && !snapshot().dirty);
    add("kept"); repaint(); cerberus_ssid_list_t before = snapshot();

    /* File picker is an intentional child: its ticks do not close the list.
     * Generic Back returns to the workspace, then its own Back to Tools. */
    picker(); unsigned allocations_before = allocations;
    for (unsigned i = 0; i < 4; ++i) ssid_lists_tick(false);
    CHECK(ssid_lists_view && !ssid_lists_view->closing && allocations == allocations_before);
    return_to_launcher(); CHECK(current_page == UI_PAGE_SSID_LIST);
    ssid_lists_tick(false); CHECK(!ssid_lists_file_picker); unchanged(&before);
    back(); CHECK(current_page == UI_PAGE_TOOLS && !ssid_lists_view && live == 1U);
    open_from(UI_PAGE_TOOLS, UI_PAGE_MAIN); unchanged(&before);

    /* Wrong extension and pre-admission errors remain visible in the picker. */
    picker(); unsigned old_starts = starts;
    pick("wrong.json"); CHECK(current_page == UI_PAGE_FILES && starts == old_starts && strstr(file_message, "SSID"));
    fail_alloc = true; pick("source.txt"); fail_alloc = false;
    CHECK(current_page == UI_PAGE_FILES && starts == old_starts && strstr(file_message, "NO_MEM"));
    fail_start = true; pick("source.txt"); fail_start = false;
    CHECK(current_page == UI_PAGE_FILES && !status().busy && live == 2U); unchanged(&before);

    /* A failed start can still assign an exact cleanup token. Picker ticks
     * continue polling that token rather than abandoning its worker. */
    assigned_start_failure = true; pick("source.txt"); assigned_start_failure = false;
    CHECK(current_page == UI_PAGE_FILES && status().busy && live == 3U);
    uint64_t picker_job = status().job_id; finish(false); ignore_release = 1;
    ssid_lists_tick(false); CHECK(status().job_id == 0U && last_released == picker_job);
    CHECK(current_page == UI_PAGE_FILES && !status().preview_ready && live == 2U); unchanged(&before);

    document(&loaded, "one\none\n"); pick("source.txt");
    CHECK(current_page == UI_PAGE_SSID_LIST && status().busy && live == 3U);
    unchanged(&before); finish(true); ssid_lists_tick(false);
    CHECK(!status().busy && ssid_lists_view->view == SSID_UI_IMPORT && status().preview_ready);
    loaded.rows[0].bytes[0] = 'X'; CHECK(ssid_lists_view->preview.rows[0].bytes[0] == 'o');
    /* Explicit Append keeps duplicate names and the original source bytes. */
    repaint(); ssid_lists_tap(220, 370);
    CHECK(snapshot().document.count == 3U && snapshot().document.rows[1].bytes[0] == 'o');
    CHECK(snapshot().document.rows[2].bytes[0] == 'o' && loaded.rows[0].bytes[0] == 'X');
    before = snapshot(); back(); CHECK(current_page == UI_PAGE_TOOLS && live == 1U);

    /* Staged import copies source ownership before its original viewer leaves. */
    cerberus_wifi_marauder_ssid_t source; document(&source, "staged\n");
    current_page = UI_PAGE_WIFI_ARCHIVE; tool_parent_page = UI_PAGE_FILES;
    CHECK(ssid_lists_import_document(&source, UI_PAGE_WIFI_ARCHIVE) == ESP_OK);
    source.rows[0].bytes[0] = 'X'; repaint();
    CHECK(ssid_lists_view->preview.rows[0].bytes[0] == 's'); unchanged(&before);
    back(); CHECK(current_page == UI_PAGE_SSID_LIST && !status().preview_ready); unchanged(&before);
    back(); CHECK(current_page == UI_PAGE_WIFI_ARCHIVE && tool_parent_page == UI_PAGE_FILES && live == 1U);

    fail_alloc = true;
    CHECK(ssid_lists_import_document(&source, UI_PAGE_WIFI_ARCHIVE) == ESP_ERR_NO_MEM);
    CHECK(current_page == UI_PAGE_WIFI_ARCHIVE && !ssid_lists_view); unchanged(&before); fail_alloc = false;
    fail_nth_alloc = 2; /* View succeeds; staging buffer fails. */
    CHECK(ssid_lists_import_document(&source, UI_PAGE_WIFI_ARCHIVE) == ESP_ERR_NO_MEM);
    CHECK(current_page == UI_PAGE_WIFI_ARCHIVE && ssid_lists_view); unchanged(&before);
    ssid_lists_tick(false); CHECK(!ssid_lists_view && current_page == UI_PAGE_WIFI_ARCHIVE && live == 1U);

    /* A native archive can export text, then close its source view before
     * EDIT COPY. Back must rebuild the freed dual-results view, not leave a
     * blank Wi-Fi page or replace the retained publication with a new scan. */
    retained_dual_rows[0] = (cerberus_wifi_archive_row_t){.ssid="retained S3",.channel=6,
        .source=CERBERUS_WIFI_ARCHIVE_SOURCE_S3,.rssi=-41,.rssi_valid=true,.source_generation=7};
    retained_dual_rows[1] = (cerberus_wifi_archive_row_t){.ssid="retained C5",.channel=36,
        .source=CERBERUS_WIFI_ARCHIVE_SOURCE_C5,.rssi=-52,.rssi_valid=true,.source_generation=7};
    cerberus_wifi_archive_row_t original_rows[2]; memcpy(original_rows, retained_dual_rows, sizeof(original_rows));
    document(&source, "retained S3\nretained C5\n");
    current_page = UI_PAGE_WIFI_ARCHIVE; wifi_mode = UI_WIFI_MODE_DUAL; tool_parent_page = UI_PAGE_TOOLS;
    dual_view_ready = false; unsigned old_reopens = dual_reopens;
    CHECK(ssid_lists_import_document(&source, UI_PAGE_WIFI) == ESP_OK); repaint();
    back(); CHECK(current_page == UI_PAGE_SSID_LIST && dual_reopens == old_reopens);
    back(); CHECK(current_page == UI_PAGE_WIFI && wifi_mode == UI_WIFI_MODE_DUAL && dual_view_ready);
    CHECK(dual_reopens == old_reopens + 1U && !memcmp(original_rows, retained_dual_rows, sizeof(original_rows)));
    CHECK(!memcmp(original_rows, displayed_dual_rows, sizeof(original_rows))); unchanged(&before);

    /* A copied binary name has no borrowed AP or source record. */
    uint8_t name[4] = {'A',0,'\r',0xff}; current_page = UI_PAGE_WIFI; tool_parent_page = UI_PAGE_TOOLS;
    wifi_mode = UI_WIFI_MODE_DETAIL; old_reopens = dual_reopens;
    dual_wifi_return_to_view = true;
    CHECK(ssid_lists_import_name(name, sizeof(name), UI_PAGE_WIFI) == ESP_OK);
    memset(name, 0xee, sizeof(name)); repaint();
    CHECK(ssid_lists_view->preview.rows[0].bytes[0] == 'A' && ssid_lists_view->preview.rows[0].bytes[3] == 0xff);
    back(); picker(); return_to_launcher();
    CHECK(current_page == UI_PAGE_SSID_LIST && !dual_wifi_return_to_view);
    ssid_lists_tick(false); back();
    CHECK(current_page == UI_PAGE_WIFI && tool_parent_page == UI_PAGE_TOOLS && dual_wifi_return_to_view); unchanged(&before);
    CHECK(wifi_mode == UI_WIFI_MODE_DETAIL && dual_reopens == old_reopens);
    dual_wifi_return_to_view = false;

    /* Copy/parse failure never changes the list or produces an import. */
    open_from(UI_PAGE_TOOLS, UI_PAGE_MAIN); picker(); document(&loaded, "failure\n"); pick("source.txt");
    finish(true); fail_copy = true; ssid_lists_tick(false); fail_copy = false;
    CHECK(!status().busy && !status().preview_ready && strstr(drawn, "SD: ESP_ERR_TIMEOUT")); unchanged(&before);

    /* Back waits for exact release, including a lost status and lost release. */
    picker(); pick("source.txt"); uint64_t owned = status().job_id;
    back(); CHECK(current_page == UI_PAGE_SSID_LIST && ssid_lists_view->closing && status().busy);
    CHECK(last_canceled == owned && last_released == owned);
    lost_status = 1; ssid_lists_tick(false); CHECK(ssid_lists_view && current_page == UI_PAGE_SSID_LIST);
    finish(true); ignore_release = 4; ssid_lists_tick(false);
    /* The worker honored the earlier release, so its final no-result status
     * already proves release even if subsequent redundant releases are lost. */
    CHECK(!ssid_lists_view && current_page == UI_PAGE_TOOLS && live == 1U); ignore_release = 0;
    unchanged(&before);

    /* Terminal worker output still owns storage until exact release succeeds. */
    open_from(UI_PAGE_TOOLS, UI_PAGE_MAIN); picker(); pick("source.txt");
    owned = status().job_id; finish(true); ignore_release = 100;
    back(); CHECK(current_page == UI_PAGE_SSID_LIST && ssid_lists_view && status().busy && held);
    CHECK(last_canceled == owned && last_released == owned);
    unsigned old_draws = draws; ssid_lists_tick(false);
    CHECK(current_page == UI_PAGE_SSID_LIST && ssid_lists_view && held && draws == old_draws);
    ignore_release = 0; ssid_lists_tick(false);
    CHECK(!ssid_lists_view && current_page == UI_PAGE_TOOLS && !held && live == 1U); unchanged(&before);

    /* A save keeps its copied revision while edits and hidden cleanup proceed. */
    open_from(UI_PAGE_TOOLS, UI_PAGE_MAIN); repaint(); ssid_lists_tap(260, 370);
    CHECK(status().busy && !memcmp(&saved, &before.document, sizeof(saved)));
    owned = status().job_id; add("after-save"); before = snapshot();
    current_page = UI_PAGE_MAIN; ssid_lists_tick(false);
    CHECK(ssid_lists_view && ssid_lists_view->closing && last_canceled == owned && last_released == owned);
    finish(true); ssid_lists_tick(false);
    CHECK(current_page == UI_PAGE_MAIN && !ssid_lists_view && snapshot().dirty && live == 1U);
    unchanged(&before);

    /* Sleep while waiting keeps ownership until finish and never navigates.
     * Reopening the page recreates only its optional view, not its workspace. */
    open_from(UI_PAGE_TOOLS, UI_PAGE_MAIN); picker(); pick("source.txt"); owned = status().job_id;
    fail_lock = true; ssid_lists_tick(true); fail_lock = false;
    CHECK(ssid_lists_view && current_page == UI_PAGE_SSID_LIST);
    ssid_lists_tick(true); CHECK(status().busy && last_canceled == owned);
    finish(true); ssid_lists_tick(true);
    CHECK(current_page == UI_PAGE_SSID_LIST && !ssid_lists_view && live == 1U); unchanged(&before);
    repaint(); CHECK(ssid_lists_view && !ssid_lists_view->closing && !status().closing); unchanged(&before);
    back(); CHECK(current_page == UI_PAGE_TOOLS && live == 1U);

    /* A rejected load can retain its exact job while the picker is visible.
     * Wake before completion must finish the close started by sleep rather
     * than resume the normal picker exemption and retain its closing view. */
    open_from(UI_PAGE_TOOLS, UI_PAGE_MAIN); picker();
    assigned_start_failure = true; pick("source.txt"); assigned_start_failure = false;
    CHECK(current_page == UI_PAGE_FILES && ssid_lists_file_picker && status().busy);
    owned = status().job_id; ssid_lists_tick(true);
    CHECK(current_page == UI_PAGE_FILES && ssid_lists_view && ssid_lists_view->closing);
    CHECK(last_canceled == owned && last_released == owned && status().busy);
    old_draws = draws; ssid_lists_tick(false);
    CHECK(current_page == UI_PAGE_FILES && ssid_lists_view && ssid_lists_view->closing && status().busy);
    CHECK(draws == old_draws); finish(true); ssid_lists_tick(false);
    CHECK(current_page == UI_PAGE_FILES && !ssid_lists_view && !ssid_lists_file_picker && live == 1U);
    CHECK(draws == old_draws && !status().busy); unchanged(&before);
    return_to_launcher();
    CHECK(current_page == UI_PAGE_SSID_LIST && ssid_lists_view && !ssid_lists_view->closing);
    CHECK(!status().closing && !status().busy && ssid_lists_view->painted_valid); unchanged(&before);
    back(); CHECK(current_page == UI_PAGE_TOOLS && !ssid_lists_view && live == 1U);

    /* Leaving the picker for an unrelated page is not a permanent exemption. */
    open_from(UI_PAGE_TOOLS, UI_PAGE_MAIN); picker(); current_page = UI_PAGE_MAIN;
    ssid_lists_tick(false); CHECK(current_page == UI_PAGE_MAIN && !ssid_lists_view && !ssid_lists_file_picker && live == 1U);
    CHECK(!held && !locked && allocations == frees + 1U && !status().busy);
    printf("SSID routes: %u checks; actual adapter/UI/session/model and file navigation passed.\n", checks);
    return 0;
}
