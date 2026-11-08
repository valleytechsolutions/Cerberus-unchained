"""Exercise production cached BLE aggregates and the actual local-only console handler."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/dual-ble-diagnostics-checks"
out.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-global-cache"),
           ZIG_LOCAL_CACHE_DIR=str(out / "zig-local-cache"))

stubs = out / "stubs/freertos"
stubs.mkdir(parents=True, exist_ok=True)
(stubs / "FreeRTOS.h").write_text("""#pragma once
typedef unsigned portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0U
void draft_enter_critical(unsigned *);
void draft_exit_critical(unsigned *);
#define portENTER_CRITICAL(p) draft_enter_critical(p)
#define portEXIT_CRITICAL(p) draft_exit_critical(p)
""")
def function(source, signature):
    start = source.index(signature + "\n{")
    brace = source.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"
source = (root / "s3_main/main/usb_console.c").read_text()
(out / "dual_ble_diagnostics_console.h").write_text(
    function(source, "static void print_dual_ble(void)") + function(source, "static void print_ble_memory(void)"))
analyzer = (root / "s3_main/main/ble_analyzer_live_ui.h").read_text()
(out / "dual_ble_diagnostics_analyzer.h").write_text(
    function(analyzer, "static bool ble_analyzer_s3_receiving(void)") +
    function(analyzer, "static esp_err_t ble_analyzer_s3_error(void)"))
ui = (root / "s3_main/main/ui.c").read_text()
(out / "dual_ble_diagnostics_fox.h").write_text(function(ui,
    "static esp_err_t ble_fox_hunt_read_s3(cerberus_s3_ble_track_status_t *track,\n    cerberus_s3_ble_track_state_t *state)"))
scanner = (root / "s3_main/main/s3_ble_scanner.h").read_text()
end = scanner.index("} cerberus_s3_ble_track_state_t;") + len("} cerberus_s3_ble_track_state_t;")
begin = scanner.rfind("typedef struct", 0, end)
(out / "dual_ble_diagnostics_track_type.h").write_text(scanner[begin:end])
exe = out / ("dual-ble-diagnostics.exe" if os.name == "nt" else "dual-ble-diagnostics")
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1", "-UNDEBUG",
    "-Wall", "-Wextra", "-Werror", "-I", str(out / "stubs"), "-I", str(out),
    "-I", str(root / "tools/host_checks/stubs"), "-I", str(root / "shared"),
    "-I", str(root / "s3_main/main"), str(root / "tools/host_checks/dual_ble_diagnostics_check.c"),
    str(root / "s3_main/main/dual_ble_scan.c"), str(root / "s3_main/main/s3_ble_discovery_session.c"),
    "-o", str(exe)], check=True, env=env)
subprocess.run([str(exe)], check=True, env=env)

subprocess.run([sys.executable, str(root / "tools/host_checks/dual_radio_console_contract.py")], check=True, env=env)
