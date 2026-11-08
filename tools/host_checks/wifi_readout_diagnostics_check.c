#ifdef NDEBUG
#undef NDEBUG
#endif
#include "wifi_readout_diagnostics.h"
#include "freertos/FreeRTOS.h"
#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

static unsigned checks;
#define CHECK(condition) do { ++checks; assert(condition); } while (0)
static atomic_uint enters, exits;
static atomic_uintptr_t lock_address;
static _Thread_local bool held;
void readout_test_enter(portMUX_TYPE *lock)
{
    assert(lock && !held);
    uintptr_t expected = 0;
    if (!atomic_compare_exchange_strong(&lock_address, &expected, (uintptr_t)lock))
        assert(expected == (uintptr_t)lock);
    while (atomic_flag_test_and_set_explicit(&lock->flag, memory_order_acquire)) {}
    held = true;
    atomic_fetch_add(&enters, 1U);
}
void readout_test_exit(portMUX_TYPE *lock)
{
    assert(held && (uintptr_t)lock == atomic_load(&lock_address));
    atomic_fetch_add(&exits, 1U);
    held = false;
    atomic_flag_clear_explicit(&lock->flag, memory_order_release);
}

/* Compile the exact model here so saturation can be injected without billions
 * of iterations or a test-only production setter. No production code changes. */
static int forbidden_io(void) __attribute__((unused));
static int forbidden_io(void) { assert(!"Telemetry attempted forbidden I/O"); return -1; }
#define esp_timer_get_time(...) forbidden_io()
#define c5_spi_link_get_status(...) forbidden_io()
#define c5_spi_link_get_wifi_scan_status(...) forbidden_io()
#define cerberus_storage_get_status(...) forbidden_io()
#define heap_caps_calloc(...) forbidden_io()
#define xTaskCreate(...) forbidden_io()
#include "wifi_readout_diagnostics.c"
#undef esp_timer_get_time
#undef c5_spi_link_get_status
#undef c5_spi_link_get_wifi_scan_status
#undef cerberus_storage_get_status
#undef heap_caps_calloc
#undef xTaskCreate

static void reset(void)
{
    assert(!held);
    memset(&model, 0, sizeof(model));
    memset(&published, 0, sizeof(published));
}
static cerberus_wifi_readout_diagnostics_t read_value(void)
{
    cerberus_wifi_readout_diagnostics_t out;
    memset(&out, 0xA5, sizeof(out));
    cerberus_wifi_readout_diagnostics_get(&out);
    CHECK(!held);
    return out;
}
static void invalid_time(const cerberus_wifi_readout_diagnostics_t *value)
{
    CHECK(!value->timing_valid && !value->elapsed_us && !value->max_step_us && !value->max_tick_gap_us);
}

static void lifecycle(void)
{
    reset();
    cerberus_wifi_readout_diagnostics_t value = read_value(), zero = {0};
    CHECK(!memcmp(&value, &zero, sizeof(value)));
    unsigned before = atomic_load(&enters);
    cerberus_wifi_readout_diagnostics_get(NULL);
    CHECK(before == atomic_load(&enters));
    cerberus_wifi_readout_diagnostics_step(1, 1, 10, 20);
    value = read_value(); CHECK(value.state == CERBERUS_WIFI_READOUT_IDLE && !value.step_calls);
    cerberus_wifi_readout_diagnostics_begin(32, 1, 100);
    value = read_value();
    CHECK(value.attempts == 1 && value.state == CERBERUS_WIFI_READOUT_READING && value.rows_total == 32);
    CHECK(value.timing_valid && value.sampled_us == 100 && !value.elapsed_us);
    cerberus_wifi_readout_diagnostics_step(2, 4, 110, 115);
    value = read_value(); CHECK(value.phase == 2 && value.rows_copied == 4 && value.step_calls == 1);
    CHECK(value.elapsed_us == 15 && value.max_step_us == 5 && !value.max_tick_gap_us);
    cerberus_wifi_readout_diagnostics_finish(CERBERUS_WIFI_READOUT_COMPLETE,
        CERBERUS_WIFI_READOUT_CANCEL_NONE, 0, 7, 32, 140);
    /* Terminal marker occurred inside the second measured tick. */
    cerberus_wifi_readout_diagnostics_step(8, 32, 130, 145);
    value = read_value();
    CHECK(value.state == CERBERUS_WIFI_READOUT_COMPLETE && value.completed == 1 && value.step_calls == 2);
    CHECK(value.phase == 7 && value.rows_copied == 32 && value.sampled_us == 145);
    CHECK(value.elapsed_us == 40 && value.max_step_us == 15 && value.max_tick_gap_us == 20);
    CHECK(!value.cancelled && !value.failed && !value.cancel_reason);
    cerberus_wifi_readout_diagnostics_finish(CERBERUS_WIFI_READOUT_FAILED,
        CERBERUS_WIFI_READOUT_CANCEL_NONE, -1, 0, 0, 200);
    cerberus_wifi_readout_diagnostics_t retained = read_value();
    CHECK(!memcmp(&value, &retained, sizeof(value)));
    memset(&retained, 0, sizeof(retained));
    retained = read_value(); CHECK(!memcmp(&value, &retained, sizeof(value)));

    /* Begin precedes allocation, so even an empty/OOM attempt is reported. */
    cerberus_wifi_readout_diagnostics_begin(0, 0, 300);
    cerberus_wifi_readout_diagnostics_finish(CERBERUS_WIFI_READOUT_FAILED,
        CERBERUS_WIFI_READOUT_CANCEL_NONE, 0x101, 0, 0, 305);
    value = read_value(); CHECK(value.attempts == 2 && value.failed == 1 && value.completed == 1);
    CHECK(value.error == 0x101 && value.elapsed_us == 5 && !value.step_calls);
    for (unsigned reason = CERBERUS_WIFI_READOUT_CANCEL_STOP; reason <= CERBERUS_WIFI_READOUT_CANCEL_RESET; ++reason) {
        cerberus_wifi_readout_diagnostics_begin(8, 1, 400 + reason * 100);
        cerberus_wifi_readout_diagnostics_finish(CERBERUS_WIFI_READOUT_CANCELLED,
            (cerberus_wifi_readout_cancel_reason_t)reason, 0, 3, 2, 410 + reason * 100);
        value = read_value(); CHECK(value.cancel_reason == reason && value.cancelled == reason);
        CHECK(value.state == CERBERUS_WIFI_READOUT_CANCELLED && value.elapsed_us == 10 && value.rows_copied == 2);
    }
    uint32_t attempts = value.attempts, cancelled = value.cancelled;
    cerberus_wifi_readout_diagnostics_begin(2, 1, 1500);
    cerberus_wifi_readout_diagnostics_begin(3, 1, 1600);
    value = read_value(); CHECK(value.attempts == attempts + 2 && value.cancelled == cancelled + 1);
    CHECK(value.rows_total == 3 && !value.rows_copied && value.state == CERBERUS_WIFI_READOUT_READING);
    cerberus_wifi_readout_diagnostics_finish(CERBERUS_WIFI_READOUT_READING,
        CERBERUS_WIFI_READOUT_CANCEL_NONE, 0, 7, 3, 1700);
    value = read_value(); CHECK(value.state == CERBERUS_WIFI_READOUT_READING);
}

static void clocks_and_saturation(void)
{
    for (unsigned reason = 0; reason < 6; ++reason) {
        reset();
        cerberus_wifi_readout_diagnostics_begin(2, 1, reason == 0 ? -1 : 100);
        cerberus_wifi_readout_diagnostics_step(2, 1, 110, 120);
        if (reason == 1) cerberus_wifi_readout_diagnostics_step(3, 1, -1, 130);
        if (reason == 2) cerberus_wifi_readout_diagnostics_step(3, 1, 130, 129);
        if (reason == 3) cerberus_wifi_readout_diagnostics_step(3, 1, 119, 140);
        cerberus_wifi_readout_diagnostics_finish(CERBERUS_WIFI_READOUT_CANCELLED,
            CERBERUS_WIFI_READOUT_CANCEL_STOP, 0, 3, 1, reason == 4 ? 110 : reason == 5 ? -1 : 150);
        cerberus_wifi_readout_diagnostics_t value = read_value(); invalid_time(&value);
        CHECK(value.cancelled == 1 && value.rows_copied == 1 && value.state == CERBERUS_WIFI_READOUT_CANCELLED);
        cerberus_wifi_readout_diagnostics_begin(0, 1, 200);
        cerberus_wifi_readout_diagnostics_finish(CERBERUS_WIFI_READOUT_COMPLETE,
            CERBERUS_WIFI_READOUT_CANCEL_NONE, 0, 7, 0, 220);
        value = read_value(); CHECK(value.timing_valid && value.elapsed_us == 20 && !value.max_step_us);
    }
    reset();
    int64_t after_wrap = ((int64_t)UINT32_MAX + 1234) * 1000;
    cerberus_wifi_readout_diagnostics_begin(1, 1, after_wrap);
    cerberus_wifi_readout_diagnostics_step(3, 1, after_wrap + 10, after_wrap + 30);
    cerberus_wifi_readout_diagnostics_finish(CERBERUS_WIFI_READOUT_COMPLETE,
        CERBERUS_WIFI_READOUT_CANCEL_NONE, 0, 7, 1, after_wrap + 40);
    cerberus_wifi_readout_diagnostics_t value = read_value();
    CHECK(value.timing_valid && value.elapsed_us == 40 && value.max_step_us == 20);
    CHECK(value.sampled_us == (uint64_t)after_wrap + 40);
    reset();
    model.value.attempts = model.value.completed = model.value.failed = model.value.cancelled = UINT32_MAX;
    cerberus_wifi_readout_diagnostics_begin(1, 1, 100);
    model.value.step_calls = UINT32_MAX;
    cerberus_wifi_readout_diagnostics_step(2, 1, 110, 120);
    cerberus_wifi_readout_diagnostics_finish(CERBERUS_WIFI_READOUT_COMPLETE,
        CERBERUS_WIFI_READOUT_CANCEL_NONE, 0, 7, 1, 130);
    value = read_value(); CHECK(value.counters_saturated && value.attempts == UINT32_MAX && value.completed == UINT32_MAX);
    CHECK(value.step_calls == UINT32_MAX && value.failed == UINT32_MAX && value.cancelled == UINT32_MAX);
    cerberus_wifi_readout_diagnostics_begin(1, 1, 200);
    cerberus_wifi_readout_diagnostics_finish(CERBERUS_WIFI_READOUT_FAILED,
        CERBERUS_WIFI_READOUT_CANCEL_NONE, -1, 1, 0, 210);
    cerberus_wifi_readout_diagnostics_begin(1, 1, 300);
    cerberus_wifi_readout_diagnostics_finish(CERBERUS_WIFI_READOUT_CANCELLED,
        CERBERUS_WIFI_READOUT_CANCEL_STOP, 0, 1, 0, 310);
    value = read_value(); CHECK(value.counters_saturated && value.failed == UINT32_MAX && value.cancelled == UINT32_MAX);
}

#ifdef _WIN32
#define THREAD_RESULT DWORD WINAPI
#define THREAD_RETURN return 0
#else
#define THREAD_RESULT void *
#define THREAD_RETURN return NULL
#endif
static atomic_bool writer_done;
static THREAD_RESULT writer(void *unused)
{
    (void)unused;
    for (uint32_t n = 1; n <= 20000; ++n) {
        int64_t start = (int64_t)n * INT64_C(0x100000007);
        uint16_t rows = (uint16_t)(n % 33);
        cerberus_wifi_readout_diagnostics_begin(rows, (uint8_t)(n % 8), start);
        cerberus_wifi_readout_diagnostics_step((uint8_t)(n % 8), rows / 2, start + 10, start + 15);
        cerberus_wifi_readout_diagnostics_finish(CERBERUS_WIFI_READOUT_COMPLETE,
            CERBERUS_WIFI_READOUT_CANCEL_NONE, 0, (uint8_t)(n % 8), rows, start + 20);
    }
    atomic_store(&writer_done, true);
    THREAD_RETURN;
}
static void concurrent_copy(void)
{
    reset(); atomic_store(&writer_done, false);
#ifdef _WIN32
    HANDLE worker = CreateThread(NULL, 0, writer, NULL, 0, NULL); CHECK(worker != NULL);
#else
    pthread_t worker; CHECK(pthread_create(&worker, NULL, writer, NULL) == 0);
#endif
    do {
        cerberus_wifi_readout_diagnostics_t value = read_value();
        if (value.state == CERBERUS_WIFI_READOUT_IDLE) continue;
        uint32_t n = value.attempts;
        uint64_t start = (uint64_t)n * UINT64_C(0x100000007);
        CHECK(value.rows_total == n % 33 && value.phase == n % 8 && value.timing_valid);
        CHECK(value.failed == 0 && value.cancelled == 0 && value.error == 0 && !value.counters_saturated);
        CHECK(!value.max_tick_gap_us && value.step_calls <= 1);
        if (value.state == CERBERUS_WIFI_READOUT_COMPLETE) {
            CHECK(value.completed == n && value.rows_copied == value.rows_total);
            CHECK(value.sampled_us == start + 20 && value.elapsed_us == 20 && value.max_step_us == 5);
        } else {
            CHECK(value.state == CERBERUS_WIFI_READOUT_READING && value.completed == n - 1);
            if (!value.step_calls) CHECK(value.sampled_us == start && !value.elapsed_us && !value.rows_copied && !value.max_step_us);
            else CHECK(value.sampled_us == start + 15 && value.elapsed_us == 15 && value.max_step_us == 5 && value.rows_copied == value.rows_total / 2);
        }
    } while (!atomic_load(&writer_done));
#ifdef _WIN32
    CHECK(WaitForSingleObject(worker, INFINITE) == WAIT_OBJECT_0); CloseHandle(worker);
#else
    CHECK(pthread_join(worker, NULL) == 0);
#endif
    CHECK(atomic_load(&enters) == atomic_load(&exits));
}

static char output[8192];
static size_t used;
static void console_write(const char *text)
{ assert(!held); size_t n = strlen(text); assert(used + n < sizeof(output)); memcpy(output + used, text, n + 1); used += n; }
static void console_printf(const char *format, ...)
{
    assert(!held); va_list args; va_start(args, format);
    int n = vsnprintf(output + used, sizeof(output) - used, format, args); va_end(args);
    assert(n >= 0 && (size_t)n < sizeof(output) - used); used += (size_t)n;
}
/* Any unexpected route would reach potential radio/storage commands. */
#define OTHER(name) static void name(void) { assert(!"Unexpected non-local console command"); }
OTHER(print_info) OTHER(print_health) OTHER(print_builds) OTHER(print_memory) OTHER(print_c5) OTHER(print_storage)
OTHER(print_resources) OTHER(print_capture) OTHER(print_usb) OTHER(print_network) OTHER(print_wifi_seen)
OTHER(print_remote_id) OTHER(print_stations) OTHER(print_reports) OTHER(print_radio) OTHER(print_gps) OTHER(print_upload_profile) OTHER(print_upload_status) OTHER(print_wardrive_logs) OTHER(print_battery)
OTHER(print_recon_record) OTHER(print_ble_archive)
OTHER(print_wifi_dual) OTHER(print_dual_ble) OTHER(print_ble_memory) OTHER(print_packet_count)
#include "wifi_readout_console_impl.h"
static void has(const char *text) { CHECK(strstr(output, text) != NULL); }
static void clear_output(void) { used = 0; output[0] = '\0'; }
static void console_cases(void)
{
    reset(); clear_output();
    char command[] = " \tWiFi_ReAdOuT\r\n"; execute_command(command);
    has("wifi_readout_scope=s3_ap_readout_excludes_rf_scan_display_touch\r\n");
    has("wifi_readout_state=0\r\n"); has("wifi_readout_attempts=0\r\n");
    has("wifi_readout_timing_valid=0\r\n");
    CHECK(strstr(output, "bssid") == NULL && strstr(output, "ssid") == NULL && strstr(output, "address") == NULL);
    cerberus_wifi_readout_diagnostics_begin(7, 1, INT64_C(5000000000000));
    cerberus_wifi_readout_diagnostics_step(3, 4, INT64_C(5000000000010), INT64_C(5000000000020));
    cerberus_wifi_readout_diagnostics_finish(CERBERUS_WIFI_READOUT_CANCELLED,
        CERBERUS_WIFI_READOUT_CANCEL_SLEEP, -123, 3, 4, INT64_C(5000000000030));
    clear_output(); unsigned before = atomic_load(&enters); print_wifi_readout();
    CHECK(atomic_load(&enters) == before + 1 && !held);
    has("wifi_readout_state=4\r\n"); has("wifi_readout_rows_total=7\r\n");
    has("wifi_readout_rows_copied=4\r\n"); has("wifi_readout_error=-123\r\n");
    has("wifi_readout_cancel_reason=3\r\n"); has("wifi_readout_sampled_us=5000000000030\r\n");
    has("wifi_readout_elapsed_us=30\r\n"); has("wifi_readout_max_step_us=10\r\n");
    clear_output(); char help[] = "help"; execute_command(help);
    has("wifi_readout Read local UI AP transfer metrics without SPI or scanning");
    const char *invalid[] = {"wifi_readout start", "wifi_readout reset", "wifi_readout 1", "wifi_readout;radio"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        char line[64]; strcpy(line, invalid[i]); clear_output(); before = atomic_load(&enters);
        execute_command(line); has("ERR unknown command"); CHECK(atomic_load(&enters) == before);
    }
}

int main(void)
{
    lifecycle(); clocks_and_saturation(); concurrent_copy(); console_cases();
    CHECK(!held && atomic_load(&enters) == atomic_load(&exits));
    printf("Wi-Fi UI readout telemetry: %u checks passed; exact timing scopes, terminal outcomes, saturation, clock faults, coherent readers and local-only console.\n", checks);
    return 0;
}
