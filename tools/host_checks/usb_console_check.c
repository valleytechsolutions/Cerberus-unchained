#undef NDEBUG
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <limits.h>
#include <inttypes.h>
#include "usb_console.h"
#include "ble_archive_io.h"

static unsigned checks;
#define CHECK(value) do { ++checks; if (!(value)) { fprintf(stderr, "USB console %u: %s\n", __LINE__, #value); exit(1); } } while (0)
typedef void *TaskHandle_t;
typedef void *usb_phy_handle_t;
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef struct { unsigned controller, target; } usb_phy_config_t;
typedef struct { size_t tx_buffer_size, rx_buffer_size; } usb_serial_jtag_driver_config_t;
#define USB_PHY_CTRL_SERIAL_JTAG 1U
#define USB_PHY_TARGET_INT 2U
#define MALLOC_CAP_SPIRAM 0x400U
#define MALLOC_CAP_8BIT 4U
#define pdPASS 1
#define pdMS_TO_TICKS(value) ((TickType_t)(value))
#define ESP_LOGI(tag, ...) ((void)(tag))
static void *heap_caps_malloc(size_t size, unsigned capabilities);
static void heap_caps_free(void *pointer);
static esp_err_t usb_new_phy(const usb_phy_config_t *config, usb_phy_handle_t *handle);
static esp_err_t usb_del_phy(usb_phy_handle_t handle);
static bool usb_serial_jtag_is_driver_installed(void);
static esp_err_t usb_serial_jtag_driver_install(const usb_serial_jtag_driver_config_t *config);
static esp_err_t usb_serial_jtag_driver_uninstall(void);
static void usb_serial_jtag_vfs_use_driver(void);
static void usb_serial_jtag_vfs_use_nonblocking(void);
static esp_err_t usb_serial_jtag_wait_tx_done(TickType_t timeout);
static bool usb_serial_jtag_is_connected(void);
static int usb_serial_jtag_write_bytes(const void *data, size_t length, TickType_t timeout);
static int usb_serial_jtag_read_bytes(void *data, size_t length, TickType_t timeout);
static BaseType_t xTaskCreate(void (*function)(void *), const char *name, unsigned stack,
    void *argument, unsigned priority, TaskHandle_t *handle);
static TickType_t xTaskGetTickCount(void);
static void vTaskDelay(TickType_t delay);
static void vTaskDelete(TaskHandle_t task);
static void execute_command(char *line);
static int checked_vsnprintf(char *output, size_t capacity, const char *format, va_list args);
#define vsnprintf checked_vsnprintf
#include "usb_console_impl.h"
#undef vsnprintf

enum event { ALLOC, FREE, PHY_NEW, PHY_DELETE, DRIVER_INSTALL, DRIVER_UNINSTALL,
    VFS_DRIVER, VFS_NONBLOCKING, TASK_CREATE, TASK_DELETE, WRITE, READ, WAIT_TX };
static enum event events[4096];
static size_t event_count;
static unsigned allocations, frees, creates, deletes, write_calls, read_calls, format_calls, delays;
static bool allocation_failure, task_failure, connected, driver_installed, phy_exists;
static bool in_task, format_failure, stop_from_command, stop_from_delete, stop_from_format, immediate_task;
static esp_err_t phy_error, driver_error, uninstall_error, delete_error, tx_error;
static unsigned quiesce_at_delay, disconnect_at_write, stop_at_write;
static TickType_t ticks;
static uint8_t *allocation;
static size_t allocation_size;
static void (*pending_task)(void *);
static void *pending_argument;
static char written[32768], commands[16][64];
static size_t written_size, command_count;
static int write_plan[32];
static size_t write_plan_count, write_plan_index;
typedef struct { int result; uint8_t byte; int connection; bool stop; } read_step_t;
static read_step_t read_plan[512];
static size_t read_plan_count, read_plan_index;
static cerberus_ble_archive_io_status_t archive_status;
static unsigned archive_reads;
static void run_task(void);

void cerberus_ble_archive_io_get_status(cerberus_ble_archive_io_status_t *status)
{
    ++archive_reads; *status = archive_status;
    /* A subsequent live publication may change everything; all output from
     * this command must still describe the one original status copy. */
    memset(&archive_status, 0, sizeof(archive_status));
    strcpy(archive_status.relative_path, "changed-after-snapshot");
    strcpy(archive_status.file_name, "changed.BLE");
}
const char *cerberus_ble_archive_io_state_name(cerberus_ble_archive_io_state_t state)
{
    static const char *const names[] = {"IDLE", "STARTING", "SAVING", "LOADING", "COMPLETE", "ERROR", "CANCELED"};
    return (unsigned)state < sizeof(names) / sizeof(names[0]) ? names[state] : "UNKNOWN";
}

static void event(enum event value) { CHECK(event_count < sizeof(events) / sizeof(events[0])); events[event_count++] = value; }
static size_t position(enum event value)
{ for (size_t i = 0; i < event_count; ++i) if (events[i] == value) return i; return SIZE_MAX; }
static void canaries(void)
{ if (allocation) { CHECK(allocation[0] == 0xA5); CHECK(allocation[allocation_size + 1] == 0x5A); } }
static void *heap_caps_malloc(size_t size, unsigned capabilities)
{
    event(ALLOC); ++allocations;
    CHECK(!allocation && size == 768 && capabilities == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (allocation_failure) return NULL;
    allocation = malloc(size + 2); CHECK(allocation); allocation_size = size;
    memset(allocation, 0xCC, size + 2); allocation[0] = 0xA5; allocation[size + 1] = 0x5A;
    return allocation + 1;
}
static void heap_caps_free(void *pointer)
{
    if (!pointer) return;
    CHECK(atomic_load(&console_task_stopped));
    CHECK(allocation && pointer == allocation + 1); canaries(); event(FREE); ++frees;
    free(allocation); allocation = NULL; allocation_size = 0;
}
static esp_err_t usb_new_phy(const usb_phy_config_t *config, usb_phy_handle_t *handle)
{
    event(PHY_NEW); CHECK(allocation && !phy_exists && !*handle);
    CHECK(config->controller == USB_PHY_CTRL_SERIAL_JTAG && config->target == USB_PHY_TARGET_INT);
    if (phy_error != ESP_OK) return phy_error;
    *handle = (void *)(uintptr_t)0xC5; phy_exists = true; return ESP_OK;
}
static esp_err_t usb_del_phy(usb_phy_handle_t handle)
{
    event(PHY_DELETE); CHECK(handle == (void *)(uintptr_t)0xC5 && phy_exists);
    CHECK(atomic_load(&console_task_stopped));
    if (delete_error != ESP_OK) return delete_error;
    phy_exists = false; return ESP_OK;
}
static bool usb_serial_jtag_is_driver_installed(void) { return driver_installed; }
static esp_err_t usb_serial_jtag_driver_install(const usb_serial_jtag_driver_config_t *config)
{
    event(DRIVER_INSTALL); CHECK(allocation && phy_exists && !driver_installed);
    CHECK(config->tx_buffer_size == 1024 && config->rx_buffer_size == 256);
    if (driver_error != ESP_OK) return driver_error;
    driver_installed = true; return ESP_OK;
}
static esp_err_t usb_serial_jtag_driver_uninstall(void)
{
    event(DRIVER_UNINSTALL); CHECK(driver_installed && atomic_load(&console_task_stopped));
    CHECK(position(VFS_NONBLOCKING) < position(DRIVER_UNINSTALL));
    if (uninstall_error != ESP_OK) return uninstall_error;
    driver_installed = false; return ESP_OK;
}
static void usb_serial_jtag_vfs_use_driver(void) { event(VFS_DRIVER); CHECK(driver_installed && allocation); }
static void usb_serial_jtag_vfs_use_nonblocking(void) { event(VFS_NONBLOCKING); CHECK(atomic_load(&console_task_stopped)); }
static esp_err_t usb_serial_jtag_wait_tx_done(TickType_t timeout)
{ event(WAIT_TX); CHECK(timeout == 100 && driver_installed && atomic_load(&console_task_stopped)); return tx_error; }
static bool usb_serial_jtag_is_connected(void)
{ if (in_task) CHECK(!atomic_load(&console_task_stopped)); return connected; }
static int usb_serial_jtag_write_bytes(const void *data, size_t length, TickType_t timeout)
{
    CHECK(driver_installed && timeout == 100 && connected && length > 0);
    if (in_task) CHECK(!atomic_load(&console_task_stopped));
    event(WRITE); ++write_calls; canaries();
    int count = write_plan_index < write_plan_count ? write_plan[write_plan_index++] : INT_MAX;
    if (count == INT_MAX || (count > 0 && (size_t)count > length)) count = (int)length;
    if (count > 0) { CHECK(written_size + (size_t)count < sizeof(written)); memcpy(written + written_size, data, (size_t)count); written_size += (size_t)count; written[written_size] = 0; }
    if (disconnect_at_write == write_calls) connected = false;
    if (stop_at_write == write_calls) atomic_store(&console_stop_requested, true);
    return count;
}
static int usb_serial_jtag_read_bytes(void *data, size_t length, TickType_t timeout)
{
    CHECK(in_task && !atomic_load(&console_task_stopped) && driver_installed && length == 1 && timeout == 50);
    event(READ); ++read_calls;
    if (read_plan_index == read_plan_count) { atomic_store(&console_stop_requested, true); return 0; }
    read_step_t step = read_plan[read_plan_index++];
    if (step.connection >= 0) connected = step.connection != 0;
    if (step.stop) atomic_store(&console_stop_requested, true);
    if (step.result > 0) *(uint8_t *)data = step.byte;
    return step.result;
}
static BaseType_t xTaskCreate(void (*function)(void *), const char *name, unsigned stack,
    void *argument, unsigned priority, TaskHandle_t *handle)
{
    event(TASK_CREATE); ++creates;
    CHECK(allocation && driver_installed && phy_exists && !pending_task);
    CHECK(!strcmp(name, "usb_console") && stack == 4096 && priority == 4 && !argument);
    CHECK(!atomic_load(&console_task_stopped) && !atomic_load(&console_stop_requested));
    if (task_failure) return 0;
    *handle = (void *)(uintptr_t)0x103; pending_task = function; pending_argument = argument;
    if (immediate_task) run_task();
    return pdPASS;
}
static TickType_t xTaskGetTickCount(void) { return ticks; }
static void vTaskDelay(TickType_t delay)
{
    CHECK(delay == 10); ticks += delay; ++delays;
    if (quiesce_at_delay && delays == quiesce_at_delay) run_task();
}
static void vTaskDelete(TaskHandle_t task)
{
    CHECK(!task && in_task && atomic_load(&console_task_stopped)); event(TASK_DELETE); ++deletes;
    if (stop_from_delete) { CHECK(cerberus_usb_console_stop() == ESP_OK); CHECK(!console_output && !allocation); }
}
static int checked_vsnprintf(char *output, size_t capacity, const char *format, va_list args)
{
    CHECK(output == console_output && allocation && output == (char *)allocation + 1);
    CHECK(capacity == 768); ++format_calls; canaries();
    if (stop_from_format) {
        CHECK(in_task); stop_from_format = false; unsigned freed = frees;
        CHECK(cerberus_usb_console_stop() == ESP_ERR_TIMEOUT);
        CHECK(frees == freed && output == console_output && allocation && !atomic_load(&console_task_stopped));
    }
    if (format_failure) { memset(output, '!', capacity); return -1; }
    int result = vsnprintf(output, capacity, format, args); canaries(); return result;
}
static void execute_command(char *line)
{
    CHECK(in_task && !atomic_load(&console_task_stopped) && command_count < 16);
    CHECK(strlen(line) < sizeof(commands[0])); strcpy(commands[command_count++], line);
    if (stop_from_command) {
        unsigned before = frees;
        CHECK(cerberus_usb_console_stop() == ESP_ERR_TIMEOUT);
        CHECK(frees == before && allocation && driver_installed && console_phy && console_task_handle);
        CHECK(!atomic_load(&console_task_stopped));
    }
    console_printf("reply=%s\r\n", line);
}
static void run_task(void)
{
    CHECK(pending_task && !in_task); void (*function)(void *) = pending_task; void *argument = pending_argument;
    pending_task = NULL; pending_argument = NULL; in_task = true; unsigned before = deletes;
    function(argument); in_task = false; CHECK(deletes == before + 1 && atomic_load(&console_task_stopped));
}
static void output_clear(void)
{
    written_size = write_calls = format_calls = 0; written[0] = 0;
    write_plan_count = write_plan_index = 0; disconnect_at_write = stop_at_write = 0;
}
static void reset(void)
{
    CHECK(!in_task); uninstall_error = delete_error = ESP_OK; quiesce_at_delay = 0;
    stop_from_delete = stop_from_command = stop_from_format = immediate_task = false;
    if (pending_task) { atomic_store(&console_stop_requested, true); run_task(); }
    CHECK(cerberus_usb_console_stop() == ESP_OK);
    CHECK(!allocation && !console_output && !console_phy && !console_task_handle && !driver_installed && !phy_exists);
    allocations = frees = creates = deletes = read_calls = delays = 0; event_count = 0;
    allocation_failure = task_failure = format_failure = false;
    phy_error = driver_error = uninstall_error = delete_error = tx_error = ESP_OK;
    connected = true; ticks = 0; read_plan_count = read_plan_index = command_count = 0;
    output_clear();
}
static void start(void) { CHECK(cerberus_usb_console_start() == ESP_OK); CHECK(console_output && allocation && pending_task); }
static void input(const char *text)
{
    for (; *text; ++text) { CHECK(read_plan_count < sizeof(read_plan) / sizeof(read_plan[0])); read_plan[read_plan_count++] = (read_step_t){1, (uint8_t)*text, -1, false}; }
}

static void formatting(void)
{
    reset(); console_printf("not started %u", 1U); CHECK(!format_calls && !write_calls && !allocations);
    start(); console_printf("value=%u text=%s\r\n", 42U, "ok"); CHECK(!strcmp(written, "value=42 text=ok\r\n"));
    CHECK(format_calls == 1 && allocations == 1); output_clear();
    char long_text[1024]; memset(long_text, 'Q', sizeof(long_text)); long_text[sizeof(long_text)-1] = 0;
    console_printf("%s", long_text); CHECK(written_size == 767); CHECK(!memcmp(written, long_text, 767));
    CHECK(console_output[767] == 0 && format_calls == 1 && allocations == 1); canaries(); output_clear();
    long_text[767] = 0; console_printf("%s", long_text); CHECK(written_size == 767); output_clear();
    long_text[766] = 0; console_printf("%s", long_text); CHECK(written_size == 766); output_clear();
    console_printf("%s", ""); CHECK(format_calls == 1 && !write_calls); output_clear();
    format_failure = true; console_printf("%s", "bad"); CHECK(format_calls == 1 && !write_calls); canaries();
    format_failure = false; output_clear(); console_printf("%s", "recovered"); CHECK(!strcmp(written, "recovered"));
}
static void partial_output(void)
{
    reset(); start(); console_write(NULL); console_write(""); CHECK(!write_calls);
    write_plan[0] = 1; write_plan[1] = 2; write_plan[2] = 1; write_plan_count = 3;
    console_printf("%s", "abcdef"); CHECK(!strcmp(written, "abcdef") && write_calls == 4); output_clear();
    write_plan[0] = 2; write_plan[1] = 0; write_plan_count = 2;
    console_write("abcdef"); CHECK(!strcmp(written, "ab") && write_calls == 2); output_clear();
    write_plan[0] = -1; write_plan_count = 1; console_write("abcdef"); CHECK(!written_size && write_calls == 1); output_clear();
    write_plan[0] = 2; write_plan_count = 1; disconnect_at_write = 1;
    console_write("abcdef"); CHECK(!strcmp(written, "ab") && write_calls == 1); output_clear();
    console_write("disconnected"); CHECK(!write_calls); connected = true;
    write_plan[0] = 2; write_plan_count = 1; stop_at_write = 1;
    console_printf("%s", "abcdef"); CHECK(!strcmp(written, "ab") && write_calls == 1);
    console_write("stopped"); CHECK(write_calls == 1 && allocation); canaries();
}
static void startup_failures(void)
{
    reset(); allocation_failure = true; CHECK(cerberus_usb_console_start() == ESP_ERR_NO_MEM);
    CHECK(event_count == 1 && events[0] == ALLOC && !frees && !console_output && !console_phy && !console_task_handle);
    reset(); phy_error = ESP_FAIL; CHECK(cerberus_usb_console_start() == ESP_FAIL);
    CHECK(allocations == 1 && frees == 1 && position(ALLOC) < position(PHY_NEW) && position(PHY_NEW) < position(FREE));
    CHECK(position(DRIVER_INSTALL) == SIZE_MAX && position(TASK_CREATE) == SIZE_MAX && !console_output);
    reset(); driver_error = ESP_ERR_INVALID_STATE; CHECK(cerberus_usb_console_start() == ESP_ERR_INVALID_STATE);
    CHECK(frees == 1 && position(PHY_DELETE) != SIZE_MAX && !console_output && !console_phy && !phy_exists);
    CHECK(position(TASK_CREATE) == SIZE_MAX);
    reset(); task_failure = true; CHECK(cerberus_usb_console_start() == ESP_ERR_NO_MEM);
    CHECK(creates == 1 && frees == 1 && atomic_load(&console_task_stopped) && !console_output && !console_task_handle);
    CHECK(!driver_installed && !console_phy && !pending_task);
    CHECK(position(FREE) < position(VFS_NONBLOCKING) && position(VFS_NONBLOCKING) < position(DRIVER_UNINSTALL));
    reset(); driver_installed = true; start(); CHECK(position(DRIVER_INSTALL) == SIZE_MAX && creates == 1);
    atomic_store(&console_stop_requested, true); run_task();
    CHECK(cerberus_usb_console_stop() == ESP_OK && !driver_installed && frees == 1);
    reset(); driver_installed = true; task_failure = true;
    CHECK(cerberus_usb_console_start() == ESP_ERR_NO_MEM && !driver_installed && frees == 1);
    CHECK(position(DRIVER_INSTALL) == SIZE_MAX && position(DRIVER_UNINSTALL) != SIZE_MAX);
    reset(); driver_installed = true; phy_error = ESP_FAIL;
    CHECK(cerberus_usb_console_start() == ESP_FAIL && driver_installed && frees == 1 && !console_phy);
    CHECK(position(DRIVER_INSTALL) == SIZE_MAX && position(DRIVER_UNINSTALL) == SIZE_MAX);
}
static void lifecycle(void)
{
    reset(); start(); char *first = console_output; unsigned count = (unsigned)event_count;
    CHECK(cerberus_usb_console_start() == ESP_OK && console_output == first && event_count == count && creates == 1);
    quiesce_at_delay = 3; CHECK(cerberus_usb_console_stop() == ESP_OK);
    CHECK(delays == 3 && ticks == 30 && deletes == 1 && frees == 1 && !console_output && !console_phy && !console_task_handle);
    CHECK(position(TASK_DELETE) < position(FREE) && position(FREE) < position(VFS_NONBLOCKING));
    CHECK(position(WAIT_TX) < position(DRIVER_UNINSTALL) && position(DRIVER_UNINSTALL) < position(PHY_DELETE));
    unsigned freed = frees; CHECK(cerberus_usb_console_stop() == ESP_OK && frees == freed);
    for (unsigned i = 0; i < 20; ++i) { start(); atomic_store(&console_stop_requested, true); run_task(); CHECK(cerberus_usb_console_stop() == ESP_OK); }
    CHECK(allocations == 21 && frees == 21 && creates == 21 && deletes == 21);
}
static void timeout_and_late_exit(void)
{
    reset(); start(); char *saved = console_output; void *phy = console_phy, *handle = console_task_handle;
    ticks = UINT32_MAX - 30U;
    CHECK(cerberus_usb_console_stop() == ESP_ERR_TIMEOUT);
    CHECK(delays == 200 && ticks == (TickType_t)(UINT32_MAX - 30U + 2000U));
    CHECK(console_output == saved && console_phy == phy && console_task_handle == handle && driver_installed);
    CHECK(!frees && position(VFS_NONBLOCKING) == SIZE_MAX && position(DRIVER_UNINSTALL) == SIZE_MAX);
    CHECK(!atomic_load(&console_task_stopped) && atomic_load(&console_stop_requested));
    unsigned count = (unsigned)event_count; CHECK(cerberus_usb_console_start() == ESP_OK && event_count == count);
    run_task(); CHECK(!frees && allocation && driver_installed && atomic_load(&console_task_stopped));
    CHECK(cerberus_usb_console_stop() == ESP_OK && frees == 1 && !console_output && !console_phy);
    reset(); start(); quiesce_at_delay = 200;
    CHECK(cerberus_usb_console_stop() == ESP_OK && ticks == 2000 && frees == 1 && deletes == 1);
    reset(); start(); atomic_store(&console_stop_requested, true); stop_from_delete = true;
    run_task(); CHECK(frees == 1 && !console_output && !driver_installed && !console_task_handle);
    reset(); start(); saved = console_output;
    CHECK(cerberus_usb_console_stop() == ESP_ERR_TIMEOUT); run_task();
    CHECK(cerberus_usb_console_start() == ESP_OK && console_output == saved && allocations == 1 && creates == 2);
    quiesce_at_delay = delays + 1;
    CHECK(cerberus_usb_console_stop() == ESP_OK && frees == 1 && deletes == 2);
}
static void cleanup_failures_and_reuse(void)
{
    reset(); start(); run_task(); char *saved = console_output;
    CHECK(atomic_load(&console_task_stopped) && allocation && driver_installed && !frees);
    start(); CHECK(console_output == saved && allocations == 1 && creates == 2);
    atomic_store(&console_stop_requested, true); run_task(); uninstall_error = ESP_FAIL;
    CHECK(cerberus_usb_console_stop() == ESP_FAIL && frees == 1 && !console_output && driver_installed && console_phy);
    CHECK(position(PHY_DELETE) == SIZE_MAX); uninstall_error = ESP_OK;
    CHECK(cerberus_usb_console_stop() == ESP_OK && frees == 1 && !driver_installed && !console_phy);
    reset(); start(); run_task(); delete_error = ESP_FAIL;
    CHECK(cerberus_usb_console_stop() == ESP_FAIL && frees == 1 && !driver_installed && console_phy);
    delete_error = ESP_OK; CHECK(cerberus_usb_console_stop() == ESP_OK && frees == 1 && !console_phy);
    reset(); start(); run_task(); tx_error = ESP_ERR_TIMEOUT;
    CHECK(cerberus_usb_console_stop() == ESP_OK && frees == 1 && !driver_installed && !console_phy);
    reset(); immediate_task = true;
    CHECK(cerberus_usb_console_start() == ESP_OK && atomic_load(&console_task_stopped) && deletes == 1 && !pending_task);
    saved = console_output; CHECK(saved && !frees && driver_installed);
    immediate_task = false; start(); CHECK(console_output == saved && allocations == 1 && creates == 2);
}
static void task_commands_and_quiescence(void)
{
    reset(); start(); input("one\r\ntwox\b\n"); run_task();
    CHECK(command_count == 2 && !strcmp(commands[0], "one") && !strcmp(commands[1], "two"));
    CHECK(strstr(written, "READ-ONLY DIAGNOSTIC CONSOLE") && strstr(written, "reply=one\r\ncerberus> "));
    CHECK(strstr(written, "\b \b") && read_calls == read_plan_count + 1 && !frees && allocation);
    CHECK(events[event_count - 1] == TASK_DELETE);
    CHECK(cerberus_usb_console_stop() == ESP_OK && frees == 1);
    reset(); start(); for (unsigned i = 0; i < 64; ++i) input("x"); input("\nnext\n"); run_task();
    CHECK(command_count == 1 && !strcmp(commands[0], "next") && strstr(written, "ERR line too long"));
    reset(); start(); input("old"); read_plan[read_plan_count++] = (read_step_t){0, 0, 0, false};
    read_plan[read_plan_count++] = (read_step_t){-1, 0, 1, false}; input("new\n"); run_task();
    CHECK(command_count == 1 && !strcmp(commands[0], "new"));
    reset(); start(); stop_from_command = true; input("block\n"); run_task();
    CHECK(command_count == 1 && delays == 200 && !frees && allocation && driver_installed);
    CHECK(atomic_load(&console_task_stopped) && events[event_count - 1] == TASK_DELETE);
    CHECK(!strstr(written, "reply=block") && cerberus_usb_console_stop() == ESP_OK && frees == 1);
    reset(); start(); stop_from_format = true; input("format\n"); run_task();
    CHECK(command_count == 1 && format_calls == 1 && delays == 200 && !frees && allocation);
    CHECK(atomic_load(&console_task_stopped) && !strstr(written, "reply=format"));
    CHECK(cerberus_usb_console_stop() == ESP_OK && frees == 1);
}
static void archive_maximum_output(void)
{
    reset(); start(); archive_reads = 0;
    memset(&archive_status, 0, sizeof(archive_status));
    archive_status.state = CERBERUS_BLE_ARCHIVE_IO_ERROR;
    archive_status.operation = CERBERUS_BLE_ARCHIVE_IO_LOAD;
    archive_status.error = INT32_MIN; archive_status.job_id = UINT64_MAX;
    archive_status.worker_active = archive_status.cancel_requested = archive_status.result_ready = true;
    archive_status.bytes = UINT32_MAX; archive_status.count = UINT8_MAX;
    char path[512], file[256], expected[2048];
    for (size_t i = 0; i < sizeof(path) - 1; ++i) path[i] = (char)('A' + i % 26);
    for (size_t i = 0; i < sizeof(file) - 1; ++i) file[i] = (char)('a' + i % 26);
    path[sizeof(path) - 1] = file[sizeof(file) - 1] = 0;
    strcpy(archive_status.relative_path, path); strcpy(archive_status.file_name, file);
    int size = snprintf(expected, sizeof(expected),
        "ble_archive_state=ERROR\r\nble_archive_active=1\r\nble_archive_error=-2147483648\r\n"
        "ble_archive_job_id=18446744073709551615\r\nble_archive_operation=1\r\n"
        "ble_archive_cancel_requested=1\r\nble_archive_result_ready=1\r\n"
        "ble_archive_bytes=4294967295\r\nble_archive_rows=255\r\nble_archive_format=BLE\r\n"
        "ble_archive_path=%s\r\nble_archive_file=%s\r\n", path, file);
    CHECK(size > 768 && (size_t)size < sizeof(expected));
    /* Partial writes exercise that each subsequent formatted part follows the
     * completed previous part instead of replacing an in-flight buffer. */
    write_plan[0] = 3; write_plan[1] = 7; write_plan[2] = 13; write_plan_count = 3;
    print_ble_archive();
    CHECK(archive_reads == 1 && format_calls == 4 && allocations == 1 && !frees);
    CHECK(written_size == (size_t)size && !strcmp(written, expected)); canaries();
    const char *keys[] = {"state", "active", "error", "job_id", "operation", "cancel_requested",
        "result_ready", "bytes", "rows", "format", "path", "file"};
    const char *cursor = written;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        char key[64]; CHECK(snprintf(key, sizeof(key), "ble_archive_%s=", keys[i]) > 0);
        CHECK(!strncmp(cursor, key, strlen(key)));
        CHECK(!strstr(cursor + strlen(key), key));
        cursor = strstr(cursor, "\r\n"); CHECK(cursor); cursor += 2;
    }
    CHECK(!*cursor && !strstr(written, "changed-after-snapshot"));
}
static void archive_format_output(void)
{
    const char *names[] = {"BLE", "AIRTAG_JSON", "WIFI_NATIVE", "AP_JSON", "SSID_TEXT", "PROBE_REPORT", "MESH_REPORT", "MESH_LOCATE_REPORT", "MESH_ARCHIVE", "UNKNOWN"};
    for (unsigned format = 0; format < sizeof(names) / sizeof(names[0]); ++format) {
        reset(); start(); archive_reads = 0;
        memset(&archive_status, 0, sizeof(archive_status));
        archive_status.state = CERBERUS_BLE_ARCHIVE_IO_COMPLETE;
        archive_status.format = (cerberus_ble_archive_io_format_t)format;
        archive_status.wifi_result = CERBERUS_WIFI_MARAUDER_UNREPRESENTABLE;
        archive_status.wifi_export_stats = (cerberus_wifi_marauder_export_stats_t){
            .loss_flags = UINT32_MAX, .records = UINT16_MAX, .security_unmapped = UINT16_MAX,
            .rssi_omitted = UINT16_MAX, .names_absent = UINT16_MAX};
        if (format == CERBERUS_BLE_ARCHIVE_IO_FORMAT_AIRTAG_JSON)
            archive_status.export_stats = (cerberus_airtag_json_export_stats_t){
                .eligible = 65535, .filtered = 65534, .incomplete = 65533};
        strcpy(archive_status.file_name, "retained.LOG");
        write_plan[0] = 2; write_plan[1] = 5; write_plan_count = 2;
        print_ble_archive();
        char expected[64]; snprintf(expected, sizeof(expected), "ble_archive_format=%s\r\n", names[format]);
        CHECK(archive_reads == 1 && strstr(written, expected));
        CHECK(strstr(written, "ble_archive_file=retained.LOG\r\n") && !strstr(written, "changed-after-snapshot"));
        bool wifi = format == CERBERUS_BLE_ARCHIVE_IO_FORMAT_AP_JSON || format == CERBERUS_BLE_ARCHIVE_IO_FORMAT_SSID_TEXT;
        CHECK((strstr(written, "ble_archive_wifi_codec_result=") != NULL) == wifi);
        CHECK((strstr(written, "ble_archive_json_eligible=") != NULL) == (format == CERBERUS_BLE_ARCHIVE_IO_FORMAT_AIRTAG_JSON));
        if (wifi) {
            CHECK(strstr(written, "ble_archive_wifi_codec_result=3\r\n"));
            CHECK(strstr(written, "ble_archive_wifi_loss_flags=4294967295\r\n"));
            CHECK(strstr(written, "ble_archive_wifi_records=65535\r\n"));
            CHECK(strstr(written, "ble_archive_wifi_security_unmapped=65535\r\n"));
            CHECK(strstr(written, "ble_archive_wifi_rssi_omitted=65535\r\n"));
            CHECK(strstr(written, "ble_archive_wifi_names_absent=65535\r\n"));
        } else if (format == CERBERUS_BLE_ARCHIVE_IO_FORMAT_AIRTAG_JSON) {
            CHECK(strstr(written, "ble_archive_json_eligible=65535\r\n"));
            CHECK(strstr(written, "ble_archive_json_filtered=65534\r\n"));
            CHECK(strstr(written, "ble_archive_json_incomplete=65533\r\n"));
        }
        CHECK(format_calls == (wifi || format == CERBERUS_BLE_ARCHIVE_IO_FORMAT_AIRTAG_JSON ? 5U : 4U));
        canaries();
    }
}
int main(void)
{
    formatting(); partial_output(); startup_failures(); lifecycle(); timeout_and_late_exit();
    cleanup_failures_and_reuse(); task_commands_and_quiescence(); archive_maximum_output(); archive_format_output(); reset();
    printf("USB console PASS: %u assertions; actual formatter/task/start/stop, PSRAM lifetime and injected interleavings\n", checks);
    return 0;
}
