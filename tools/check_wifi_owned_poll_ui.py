"""Exercise actual owned Wi-Fi observer, capture and scheduler together.

Reuses the scan-store fixture's allocation/transport boundaries. Its old mock
observer/tick remain renamed fixture scaffolding; this suite compiles the real
observer and tick separately and calls those in every lifecycle scenario.
"""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/wifi-owned-poll-ui-checks"
out.mkdir(parents=True, exist_ok=True)
(out / "freertos").mkdir(exist_ok=True)
(out / "freertos/FreeRTOS.h").write_text('''#pragma once
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
''')
source = (root / "s3_main/main/sentinel_live_ui.h").read_text()
def between(first, after):
    start = source.index(first)
    return source[start:source.index(after, start)]

record_start = source.index("typedef struct {")
record_end = source.index("} live_scan_record_t;", record_start) + len("} live_scan_record_t;")
(out / "scan_display_record_type.h").write_text(source[record_start:record_end])
(out / "scan_display_page_tool_impl.h").write_text(
    between("static sentinel_scan_tool_t live_page_tool(", "static bool live_is_view("))
(out / "scan_display_capture_impl.h").write_text(
    between("static void live_status(", "static esp_err_t live_observe_owned_wifi("))
(out / "scan_display_tick_impl.h").write_text(
    between("static void live_scan_tick(", "static void live_reset_observations("))
(out / "wifi_owned_observe_impl.h").write_text(
    between("static esp_err_t live_observe_owned_wifi(", "static esp_err_t live_execute("))
exe = out / "wifi-owned-poll-ui-check.exe"
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-cache"))
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1",
    "-Wall", "-Wextra", "-Werror", "-I", str(out),
    "-I", str(root / "tools/host_checks/stubs"), "-I", str(root / "shared"),
    "-I", str(root / "s3_main/main"),
    str(root / "tools/host_checks/wifi_owned_poll_ui_check.c"),
    str(root / "s3_main/main/sentinel_scan_session.c"),
    str(root / "s3_main/main/wifi_readout_diagnostics.c"), "-o", str(exe)],
    check=True, env=env)
subprocess.run([str(exe)], check=True, env=env)
