#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef int esp_err_t;
enum { ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_SIZE, ESP_ERR_NO_MEM,
       ESP_ERR_NOT_FOUND, ESP_ERR_INVALID_STATE, ESP_FAIL };
enum { MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2 };
#define ESP_ERROR_CHECK(x) assert((x) == ESP_OK)
#define ESP_LOGW(...) ((void)0)
#include "ui_directory_memory_types.h"
_Static_assert(sizeof(cerberus_storage_entry_t) == 272, "match measured S3 metadata layout");

static cerberus_storage_entry_t *file_entries, *log_entries, *script_entries;
static size_t file_entry_count, log_entry_count, script_entry_count;
static size_t file_page_offset, log_page_offset, script_page_offset;
static bool storage_checked, file_listing_checked, storage_safely_removed;
static bool log_checked, script_listing_checked;
static esp_err_t storage_last_error, log_error, script_listing_error, script_load_error;
static char file_path[512] = "/";
static cerberus_storage_status_t storage_status;
static cerberus_script_t loaded_script;
static cerberus_script_error_t script_parse_error;
static int script_mode, file_mode, log_mode, viewed_file_hash_state;
static esp_err_t viewed_file_hash_error;
static char viewed_file_name[256];
static uint64_t viewed_file_offset;
static size_t viewed_file_history_count;
static uint8_t viewed_file_sha256[32];

static bool mounted, psram_available;
static esp_err_t mount_error, unmount_error, status_error, list_error;
static unsigned allocation_attempts, allocation_successes, list_calls, mounts, unmounts, draws;
static unsigned script_loads, script_runs, viewer_refreshes;
static size_t allocated_bytes, source_count;
static bool malformed_count, overwrite_viewer_source;
static cerberus_storage_entry_t source_rows[32];
static char last_list_path[512], last_script_name[256];
static void *allocation_log[3];

static void *heap_caps_calloc(size_t count, size_t size, unsigned caps)
{
    ++allocation_attempts;
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    assert(size == sizeof(cerberus_storage_entry_t));
    assert(count == 16 || count == 32);
    if (!psram_available) return NULL;
    assert(allocation_successes < 3);
    void *p = calloc(count, size);
    assert(p != NULL);
    allocated_bytes += count * size;
    allocation_log[allocation_successes++] = p;
    return p;
}
static bool cerberus_storage_is_mounted(void) { return mounted; }
static esp_err_t cerberus_storage_mount(void)
{
    ++mounts;
    if (mount_error == ESP_OK) mounted = true;
    return mount_error;
}
static esp_err_t cerberus_storage_unmount(void)
{
    ++unmounts;
    if (unmount_error == ESP_OK) mounted = false;
    return unmount_error;
}
static esp_err_t cerberus_storage_get_status(cerberus_storage_status_t *out)
{
    assert(out == &storage_status);
    return status_error;
}
static esp_err_t cerberus_storage_list_directory(const char *path,
    cerberus_storage_entry_t *entries, size_t capacity, size_t *count)
{
    assert(entries != NULL && count != NULL && path != NULL);
    assert(*count == 0);
    ++list_calls;
    snprintf(last_list_path, sizeof(last_list_path), "%s", path);
    size_t n = source_count < capacity ? source_count : capacity;
    memcpy(entries, source_rows, n * sizeof(*entries));
    *count = malformed_count ? capacity + 1 : n;
    return list_error;
}
static esp_err_t draw_detail_page(void) { ++draws; return ESP_OK; }
static esp_err_t refresh_text_viewer(void)
{
    ++viewer_refreshes;
    if (overwrite_viewer_source) {
        if (file_entries) memset(file_entries, 0, sizeof(*file_entries));
        if (log_entries) memset(log_entries, 0, sizeof(*log_entries));
    }
    return ESP_OK;
}
static void start_script_run(void) { ++script_runs; }
static esp_err_t cerberus_script_load(const char *name, cerberus_script_t *script,
    cerberus_script_error_t *error)
{
    (void)error;
    ++script_loads;
    snprintf(last_script_name, sizeof(last_script_name), "%s", name);
    /* Exercise the real caller's fallback naming after a successful parse. */
    script->name[0] = '\0';
    return ESP_OK;
}

#include "ui_directory_memory_impl.h"

static void row(size_t n, const char *name, bool directory)
{
    memset(&source_rows[n], 0, sizeof(source_rows[n]));
    snprintf(source_rows[n].name, sizeof(source_rows[n].name), "%s", name);
    source_rows[n].size_bytes = n + 100;
    source_rows[n].is_directory = directory;
    if (source_count <= n) source_count = n + 1;
}

int main(void)
{
    size_t count = 42;
    assert(ui_list_directory("/", NULL, 32, &count) == ESP_ERR_INVALID_ARG && count == 0);
    assert(ui_list_directory("/", &file_entries, 0, &count) == ESP_ERR_INVALID_ARG);
    assert(ui_list_directory("/", &file_entries, 33, &count) == ESP_ERR_INVALID_ARG);
    assert(ui_list_directory("/", &file_entries, 32, NULL) == ESP_ERR_INVALID_ARG);
    assert(allocation_attempts == 0 && list_calls == 0);

    /* Missing PSRAM: every producer fails before touching a bank; repeated
     * attempts remain recoverable. Direct child/parent navigation is included. */
    row(0, "CHILD", true);
    for (unsigned i = 0; i < 3; ++i) {
        assert(refresh_file_listing(false) == ESP_ERR_NO_MEM);
        assert(refresh_log_listing(false) == ESP_ERR_NO_MEM);
        assert(refresh_script_listing() == ESP_ERR_NO_MEM);
        navigate_file_directory("CHILD");
        assert(storage_last_error == ESP_ERR_NO_MEM && file_entry_count == 0);
        navigate_file_parent();
        assert(strcmp(file_path, "/") == 0 && storage_last_error == ESP_ERR_NO_MEM);
        assert(file_entries == NULL && log_entries == NULL && script_entries == NULL);
        assert(file_entry_count == 0 && log_entry_count == 0 && script_entry_count == 0);
        handle_scripts_tap(20, UI_SCRIPT_LIST_Y + 1);
        assert(script_loads == 0);
    }
    assert(list_calls == 0 && allocation_successes == 0);

    /* A failed service prerequisite must not allocate or present stale rows. */
    unsigned attempts = allocation_attempts;
    mount_error = ESP_FAIL;
    mounted = false;
    file_entry_count = log_entry_count = script_entry_count = 7;
    assert(refresh_file_listing(false) == ESP_FAIL);
    assert(refresh_log_listing(false) == ESP_FAIL);
    assert(refresh_script_listing() == ESP_FAIL);
    assert(allocation_attempts == attempts);
    assert(file_entry_count == 0 && log_entry_count == 0 && script_entry_count == 0);
    mount_error = ESP_OK;

    /* A retry can allocate, and repeated listings retain exactly one bank. */
    psram_available = true;
    file_page_offset = 99;
    assert(refresh_file_listing(false) == ESP_OK && file_entry_count == 1);
    assert(file_page_offset == 0 && allocation_successes == 1);
    cerberus_storage_entry_t *retained_file = file_entries;
    attempts = allocation_attempts;
    for (unsigned i = 0; i < 5; ++i) assert(refresh_file_listing(false) == ESP_OK);
    assert(file_entries == retained_file && allocation_attempts == attempts);

    /* Child name aliases the old bank. Real navigation must copy it before
     * the synchronous listing overwrites that bank. Parent needs no new bank. */
    row(0, "replacement.txt", false);
    navigate_file_directory(file_entries[0].name);
    assert(strcmp(last_list_path, "/CHILD") == 0 && strcmp(file_path, "/CHILD") == 0);
    assert(strcmp(file_entries[0].name, "replacement.txt") == 0);
    navigate_file_parent();
    assert(strcmp(last_list_path, "/") == 0 && file_entries == retained_file);

    /* Allocation pressure on a second bank does not destroy the first. */
    psram_available = false;
    assert(refresh_log_listing(false) == ESP_ERR_NO_MEM && log_entries == NULL);
    assert(file_entries == retained_file && strcmp(file_entries[0].name, "replacement.txt") == 0);
    assert(refresh_file_listing(false) == ESP_OK); /* Existing bank never reallocates. */
    psram_available = true;
    source_count = 0;
    row(0, "random.txt", false); row(1, "SYSTEM.CSV", false);
    row(2, "WIFI0001.CSV", false); row(3, "SYSTEM.1.CSV", true);
    log_page_offset = 99;
    assert(refresh_log_listing(false) == ESP_OK && log_entry_count == 2);
    assert(log_page_offset == 0 && strcmp(log_entries[0].name, "SYSTEM.CSV") == 0);
    assert(strcmp(log_entries[1].name, "WIFI0001.CSV") == 0);
    assert(log_entries != file_entries && allocation_successes == 2);

    source_count = 0;
    row(0, "ignore.txt", false); row(1, "FIRST.CBS", false);
    row(2, "DIR.CBS", true); row(3, "SECOND.CBS", false);
    script_page_offset = 99;
    assert(refresh_script_listing() == ESP_OK && script_entry_count == 2);
    assert(script_page_offset == 0 && strcmp(script_entries[0].name, "FIRST.CBS") == 0);
    assert(strcmp(script_entries[1].name, "SECOND.CBS") == 0);
    assert(allocation_successes == 3 && allocated_bytes == 21760);
    assert(script_entries != file_entries && script_entries != log_entries);
    handle_scripts_tap(20, UI_SCRIPT_LIST_Y + UI_SCRIPT_ROW_HEIGHT + 1);
    assert(script_loads == 1 && strcmp(last_script_name, "SECOND.CBS") == 0);
    assert(strcmp(loaded_script.name, "SECOND.CBS") == 0 && script_mode == UI_SCRIPT_MODE_DETAIL);
    handle_scripts_tap(100, 350);
    assert(script_runs == 1);

    /* Real open paths copy an entry's name before downstream reads. */
    snprintf(file_entries[0].name, sizeof(file_entries[0].name), "selected.txt");
    overwrite_viewer_source = true;
    open_text_file(file_entries[0].name);
    assert(strcmp(viewed_file_name, "selected.txt") == 0 && file_mode == UI_FILE_MODE_TEXT);
    snprintf(log_entries[0].name, sizeof(log_entries[0].name), "SYSTEM.CSV");
    open_log_report(log_entries[0].name);
    assert(strcmp(viewed_file_name, "SYSTEM.CSV") == 0 && log_mode == UI_LOG_MODE_TEXT);
    snprintf(file_entries[0].name, sizeof(file_entries[0].name), "selected.bmp");
    open_image_file(file_entries[0].name);
    memset(file_entries, 0, sizeof(*file_entries));
    assert(strcmp(viewed_file_name, "selected.bmp") == 0 && file_mode == UI_FILE_MODE_IMAGE);
    assert(viewer_refreshes == 2);

    /* Partial/error results are never selectable; retained allocations survive
     * storage failure, malformed backend count, and unmount/remount cycles. */
    attempts = allocation_attempts;
    list_error = ESP_FAIL;
    assert(refresh_file_listing(false) == ESP_FAIL && file_entry_count == 0);
    assert(refresh_log_listing(false) == ESP_FAIL && log_entry_count == 0);
    assert(refresh_script_listing() == ESP_FAIL && script_entry_count == 0);
    script_mode = UI_SCRIPT_MODE_LIST;
    handle_scripts_tap(20, UI_SCRIPT_LIST_Y + 1);
    assert(script_loads == 1);
    list_error = ESP_ERR_NOT_FOUND;
    assert(refresh_log_listing(false) == ESP_OK && log_entry_count == 0);
    list_error = ESP_OK;
    malformed_count = true;
    assert(refresh_file_listing(false) == ESP_ERR_INVALID_SIZE && file_entry_count == 0);
    malformed_count = false;
    unmount_error = ESP_FAIL;
    mounted = true;
    assert(refresh_file_listing(true) == ESP_FAIL && file_entry_count == 0);
    assert(refresh_log_listing(true) == ESP_FAIL && log_entry_count == 0);
    unmount_error = ESP_OK;
    assert(refresh_file_listing(true) == ESP_OK);
    assert(refresh_log_listing(true) == ESP_OK);
    assert(refresh_script_listing() == ESP_OK);
    assert(allocation_attempts == attempts && allocated_bytes == 21760);
    assert(file_entries == retained_file && mounts > 0 && unmounts > 0 && draws > 0);

    /* First-use Saved Maps can be absent after a successful mount/status.
     * The storage boundary classifies only ENOENT as NOT_FOUND; neither other
     * paths nor errors from mount/status are empty-directory evidence. */
    snprintf(file_path, sizeof(file_path), "%s", CERBERUS_STORAGE_MESH_ARCHIVE_PATH);
    list_error = ESP_ERR_NOT_FOUND;
    file_page_offset = 99;
    assert(refresh_file_listing(false) == ESP_OK && storage_last_error == ESP_OK);
    assert(file_entry_count == 0 && file_page_offset == 0);
    assert(file_entries == retained_file && allocation_attempts == attempts);
    for (unsigned i = 0; i < 3; ++i) {
        assert(refresh_file_listing(false) == ESP_OK && file_entry_count == 0);
    }
    navigate_file_parent();
    assert(strcmp(file_path, "/CERBERUS") == 0 &&
        storage_last_error == ESP_ERR_NOT_FOUND && file_entry_count == 0);
    list_error = ESP_OK;
    source_count = 0;
    navigate_file_parent();
    assert(strcmp(file_path, "/") == 0 && storage_last_error == ESP_OK);
    snprintf(file_path, sizeof(file_path), "%s", CERBERUS_STORAGE_MESH_ARCHIVE_PATH);
    const esp_err_t errors[] = {ESP_FAIL, ESP_ERR_NO_MEM, ESP_ERR_INVALID_STATE};
    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        list_error = errors[i];
        assert(refresh_file_listing(false) == errors[i] && file_entry_count == 0);
    }
    list_error = ESP_ERR_NOT_FOUND;
    unsigned calls_before = list_calls;
    mount_error = ESP_ERR_NOT_FOUND;
    assert(refresh_file_listing(false) == ESP_ERR_NOT_FOUND && list_calls == calls_before);
    mount_error = ESP_OK;
    status_error = ESP_ERR_NOT_FOUND;
    assert(refresh_file_listing(false) == ESP_ERR_NOT_FOUND && list_calls == calls_before);
    status_error = ESP_OK;
    snprintf(file_path, sizeof(file_path), "/CERBERUS/MESH/CHILD");
    assert(refresh_file_listing(false) == ESP_ERR_NOT_FOUND && file_entry_count == 0);
    snprintf(file_path, sizeof(file_path), "/CERBERUS/OTHER");
    assert(refresh_file_listing(false) == ESP_ERR_NOT_FOUND && file_entry_count == 0);
    snprintf(file_path, sizeof(file_path), "%s", CERBERUS_STORAGE_MESH_ARCHIVE_PATH);
    list_error = ESP_OK;
    source_count = 0;
    assert(refresh_file_listing(false) == ESP_OK && file_entry_count == 0);
    row(0, "M00001.MSH", false);
    assert(refresh_file_listing(false) == ESP_OK && file_entry_count == 1);
    assert(strcmp(file_entries[0].name, "M00001.MSH") == 0);

    /* Harness teardown only: production intentionally never frees these banks. */
    for (unsigned i = 0; i < allocation_successes; ++i) free(allocation_log[i]);
    puts("PASS: missing PSRAM, OOM retry, five producer paths, retained banks, alias-safe navigation,");
    puts("      filtering, copied selections, partial/error counts, remount and 21,760-byte bound.");
    puts("      First-use Saved Maps: absent/empty/populated, repeat, exact path, SD and listing errors.");
    return 0;
}
