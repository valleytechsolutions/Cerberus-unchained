"""Exercise the production Wardrive PSRAM workspace allocation and teardown contract."""
from pathlib import Path
import importlib.util
import os
import re
import subprocess
import sys


root = Path(__file__).resolve().parents[1]
source = (root / "s3_main/main/wardrive_service.c").read_text(encoding="utf-8")
gps_header = (root / "s3_main/main/wardrive_gps_uart.h").read_text(encoding="utf-8")
budget_defines = "\n".join(re.findall(r"^#define (?:WARDRIVE_WORKER_STACK|CERBERUS_WARDRIVE_GPS_(?:RX_BUFFER|TASK_STACK)_BYTES) .*", source + "\n" + gps_header, re.M))
attempt_start = source.index("typedef struct {", source.index("static esp_err_t recent_error;"))
attempt_type = source[attempt_start:source.index("} wardrive_recent_attempt_t;", attempt_start) + len("} wardrive_recent_attempt_t;")]
workspace = source[source.index("/* These bounded buffers"):source.index(
    "static uint32_t open_file_rows;")]
helpers = source[source.index("static esp_err_t allocate_workspace(void)"):source.index(
    "static uint32_t now_ms(void)")]

stop = source[source.index("static void stop_scans_and_close_file("):source.index(
    "static void wardrive_worker(void *unused)")]
worker = source[source.index("static void wardrive_worker(void *unused)"):source.index(
    "static esp_err_t wardrive_start_with_idle_probes_paused(")]
start = source[source.index("static esp_err_t wardrive_start_with_idle_probes_paused("):source.index(
    "void cerberus_wardrive_request_stop(void)")]
assert start.index("cerberus_s3_ble_host_claim(CERBERUS_S3_BLE_OWNER_SCANNER, &warm_lease)") < start.index(
    "allocate_workspace();") < start.index("field_survey_snapshot(&expansion_stage);")
assert stop.index("stop_own_scans(") < stop.index(
    "if (wardrive_file_open) (void)save_open_file(keep);")
assert worker.index("stop_scans_and_close_file(") < worker.index(
    "stop_optional_gps(gps_enabled);") < worker.index(
    "release_workspace();") < worker.index("worker_active = false;")
assert "static cerberus_wardrive_dedupe_t dedupe;" not in source
assert "static field_survey_snapshot_t expansion_stage;" not in source

program = r'''
#undef NDEBUG
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include "wardrive_rssi_filter.h"
#define CERBERUS_WIFI_SCAN_MAX_RESULTS 32U
#define CERBERUS_BLE_SCAN_MAX_RESULTS 32U
#define CERBERUS_WIFI_PROBE_LIMIT 16U
#define CERBERUS_WIFI_RELATION_LIMIT 32U
#define MALLOC_CAP_SPIRAM 0x10U
#define MALLOC_CAP_8BIT 0x20U
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 1
#define ESP_ERR_NO_MEM 2
typedef int esp_err_t;
static cerberus_wardrive_rssi_filter_t *rssi_filter;
static struct { bool rssi_filter_enabled; } wardrive_status;
static unsigned wardrive_lock;
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
typedef struct { uint64_t session_id, sequence; } cerberus_wardrive_recent_ticket_t;
typedef struct { uint8_t data[64]; } cerberus_wardrive_dedupe_t;
typedef struct { uint8_t data[64]; } cerberus_wardrive_probe_dedupe_t;
typedef struct { uint8_t data[64]; } cerberus_wardrive_relation_dedupe_t;
typedef struct { uint8_t data[44]; } cerberus_wifi_relation_row_t;
typedef struct { uint8_t data[56]; } cerberus_c5_wifi_relation_bank_t;
typedef int cerberus_wardrive_relation_state_t;
typedef struct { uint8_t data[55]; } cerberus_wifi_probe_row_t;
typedef struct { uint8_t data[48]; } cerberus_c5_wifi_probe_bank_t;
typedef int cerberus_wardrive_probe_state_t;
typedef struct { uint8_t data[48]; } cerberus_c5_wifi_result_t;
typedef struct { uint8_t data[52]; } cerberus_c5_ble_result_t;
typedef struct { uint8_t data[28]; } cerberus_s3_ble_wardrive_evidence_t;
/* Opaque boundary type: this test verifies allocation/capability/lifetime,
 * not native structure sizing (the retained-ELF audit owns that evidence). */
typedef struct { uint8_t data[64]; } cerberus_s3_ble_passive_state_t;
typedef struct { uint8_t data[80]; } field_survey_snapshot_t;
typedef struct { uint8_t data[96]; } expansion_record_t;
static unsigned alloc_calls, free_calls, last_caps;
static size_t last_bytes;
static bool fail_next;
static void *heap_caps_calloc(size_t count, size_t bytes, unsigned caps)
{
    ++alloc_calls; last_caps = caps; last_bytes = count * bytes;
    if (fail_next) { fail_next = false; return NULL; }
    return calloc(count, bytes);
}
static void heap_caps_free(void *ptr)
{
    if (ptr) ++free_calls;
    free(ptr);
}
''' + budget_defines + "\n" + attempt_type + workspace + helpers + r'''
int main(void)
{
    assert(post_ble_reserve_bytes(false) == 43008U);
    assert(post_ble_reserve_bytes(true) == 48640U);
    assert(45980U >= post_ble_reserve_bytes(false)); /* measured rev51 Stop */
    assert(45980U < post_ble_reserve_bytes(true)); /* Larger UART invalidates the old GPS-on cold reserve. */
    fail_next = true;
    assert(allocate_workspace() == ESP_ERR_NO_MEM);
    assert(workspace == NULL && free_calls == 0U);
    assert(allocate_workspace() == ESP_OK);
    assert(workspace != NULL);
    assert(last_caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    assert(last_bytes == sizeof(*workspace));
    assert(allocate_workspace() == ESP_ERR_INVALID_STATE);
    assert(alloc_calls == 2U);
    release_workspace();
    assert(workspace == NULL && free_calls == 1U);
    assert(allocate_workspace() == ESP_OK);
    release_workspace();
    assert(workspace == NULL && free_calls == 2U);
    fail_next=true;begin_rssi_filter();
    assert(!rssi_filter&&!wardrive_status.rssi_filter_enabled);
    begin_rssi_filter();
    assert(rssi_filter&&wardrive_status.rssi_filter_enabled);
    assert(last_caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)&&last_bytes==sizeof(*rssi_filter));
    assert(sizeof(*rssi_filter)<=5120U);
    assert(rssi_filter->entries[0][0].type==0);
    rssi_filter->entries[0][0].type=1;
    release_workspace();assert(!rssi_filter&&free_calls==3U);
    begin_rssi_filter();assert(rssi_filter->entries[0][0].type==0);
    release_workspace();assert(!rssi_filter&&free_calls==4U);
    return 0;
}
'''
out = root / "artifacts/wardrive-checks"
out.mkdir(parents=True, exist_ok=True)
fixture = out / "wardrive_memory_check.c"
fixture.write_text(program, encoding="utf-8")
exe = out / "wardrive-memory-check.exe"
mingw_bin = Path.home() / ".platformio/packages/toolchain-gccmingw32/bin"
use_zig = importlib.util.find_spec("ziglang") is not None
compiler = ([sys.executable, "-m", "ziglang", "cc"] if use_zig
    else [str(mingw_bin / "gcc.exe")])
environment = os.environ.copy()
if not use_zig:
    environment["PATH"] = str(mingw_bin) + os.pathsep + environment.get("PATH", "")
subprocess.run(compiler + ["-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
    "-I", str(root / "s3_main/main"),
    str(fixture), "-o", str(exe)], check=True, env=environment)
subprocess.run([str(exe)], check=True, env=environment)
print("wardrive PSRAM workspace allocation and Stop teardown: pass")
