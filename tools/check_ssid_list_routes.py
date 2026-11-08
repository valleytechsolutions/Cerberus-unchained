"""Exercise the actual SSID page adapter, renderer, workspace and file routes.

Only display, keyboard and the asynchronous storage boundary are replaced. The
fixture uses its own generated headers/cache so it can run beside other checks.
"""
import os
from pathlib import Path
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/ssid-list-routes-checks"
stubs = out / "stubs"
(stubs / "freertos").mkdir(parents=True, exist_ok=True)
(stubs / "freertos/FreeRTOS.h").write_text("""#pragma once
#include <stdint.h>
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
""", encoding="utf-8")
(stubs / "freertos/semphr.h").write_text("""#pragma once
#include <stdint.h>
typedef void *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t mutex, uint32_t ticks);
int xSemaphoreGive(SemaphoreHandle_t mutex);
""", encoding="utf-8")
(stubs / "esp_heap_caps.h").write_text("""#pragma once
#include <stddef.h>
#define MALLOC_CAP_8BIT 2U
#define MALLOC_CAP_SPIRAM 4U
void *heap_caps_calloc(size_t count, size_t size, unsigned caps);
void heap_caps_free(void *pointer);
""", encoding="utf-8")
(stubs / "esp_random.h").write_text("""#pragma once
#include <stddef.h>
void esp_fill_random(void *out, size_t length);
""", encoding="utf-8")

ui = (root / "s3_main/main/ui.c").read_text(encoding="utf-8")
interchange = (root / "s3_main/main/wifi_interchange_live_ui.h").read_text(encoding="utf-8")

def end_block(text, opening):
    depth = 0
    for index in range(opening, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return index + 1
    raise ValueError("Unclosed production block")

def function(text, signature):
    start = text.index(signature + "\n{")
    return text[start:end_block(text, text.index("{", start))]

types = re.findall(r"typedef enum \{[^}]*\} \w+;", ui)
types = [item for item in types if any(item.endswith("} " + name + ";")
    for name in ("ui_page_t", "ui_file_mode_t", "ui_wifi_mode_t"))]
assert len(types) == 3
(out / "ssid_route_types.h").write_text("\n".join(types), encoding="utf-8")
suffix = function(interchange, "static bool wifi_interchange_suffix(const char *name, const char *suffix)")
can_open_start = interchange.index("static bool wifi_interchange_can_open(const char *name, bool ap_json)")
can_open = interchange[can_open_start:end_block(interchange, interchange.index("{", can_open_start))]
files = function(ui, "static void handle_files_tap(uint16_t x, uint16_t y)")
row_start = files.index("    if (y >= UI_FILE_LIST_Y &&")
row = files[row_start:end_block(files, files.index("{", row_start))]
(out / "ssid_file_routes.h").write_text(suffix + "\n" + can_open + "\n" +
    "static void actual_file_row_tap(uint16_t x, uint16_t y) { (void)x;\n" + row + "\n}\n" +
    function(ui, "static void return_to_launcher(void)"), encoding="utf-8")

# The compiled adapter must remain wired to the real page, touch and idle loop.
assert "case SENTINEL_TOOLS_SSID_LISTS: ssid_lists_open(); return;" in ui
assert "if (current_page == UI_PAGE_SSID_LIST) return ssid_lists_draw();" in ui
assert "ssid_lists_tap(x, y);" in ui and "ssid_lists_tick(screen_asleep);" in ui
assert 'ssid_lists_pick_file(file_path, file_entries[index].name)' in row

env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-global-cache"),
           ZIG_LOCAL_CACHE_DIR=str(out / "zig-local-cache"))
exe = out / ("ssid-list-routes-check.exe" if os.name == "nt" else "ssid-list-routes-check")
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1", "-UNDEBUG",
    "-Wall", "-Wextra", "-Werror", "-I", str(out), "-I", str(stubs),
    "-I", str(root / "tools/host_checks/stubs"), "-I", str(root / "s3_main/main"),
    "-I", str(root / "shared"), str(root / "tools/host_checks/ssid_list_routes_check.c"),
    str(root / "s3_main/main/ssid_list_session.c"), str(root / "s3_main/main/ssid_list.c"),
    str(root / "s3_main/main/wifi_marauder.c"), str(root / "s3_main/main/wifi_archive.c"),
    "-o", str(exe)], check=True, env=env)
subprocess.run([str(exe)], check=True, timeout=30)
