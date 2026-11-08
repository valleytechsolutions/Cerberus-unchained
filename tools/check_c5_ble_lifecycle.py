"""Exercise actual C5 receive controls with the installed SDK's queue contract.

Only the SDK/RTOS boundary is modeled. Production controls, callback finish,
reset/sync, admission and companion command handlers are compiled unchanged.
"""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/c5-ble-lifecycle-checks"
out.mkdir(parents=True, exist_ok=True)
source = (root / "c5_firmware/main/ble_scanner.c").read_text()

def function(signature, text=source):
    start = text.index(signature)
    brace = text.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        if text[end] == "{":
            depth += 1
        if text[end] == "}":
            depth -= 1
        end += 1
    return text[start:end] + "\n"

start = source.index("typedef struct {")
end = source.index("} cerberus_ble_record_t;", start) + len("} cerberus_ble_record_t;")
(out / "c5_ble_watch_record_type.h").write_text(source[start:end])
start = source.index("static uint64_t dual_boot_id,")
end = source.index("static void dual_expire_locked(void);", start) + len("static void dual_expire_locked(void);")
(out / "c5_ble_dual_state.h").write_text(source[start:end])
pieces = []
for first, after in (
    ("static bool analyzer_observe(", "static int gap_event("),
    ("static void set_control_error_for_epoch(", "static void set_connection_error("),
    ("static esp_err_t start_scan(", "esp_err_t cerberus_ble_watch_get_status("),
):
    start = source.index(first)
    pieces.append(source[start:source.index(after, start)])
start = source.index("static int gap_event(")
pieces.insert(1, source[start:source.index("    case BLE_GAP_EVENT_CONNECT:", start)] +
              "    default: break;\n    }\n    return 0;\n}\n")
for signature in ("static esp_err_t stop_scan(", "void cerberus_ble_scanner_get_status(",
                  "static void host_reset(", "static void host_sync(",
                  "esp_err_t cerberus_ble_connection_start("):
    pieces.append(function(signature))
(out / "actual_lifecycle.h").write_text("\n".join(pieces))
wifi = (root / "c5_firmware/main/wifi_scanner.c").read_text()
(out / "actual_wifi_admission.h").write_text(
    function("static bool another_radio_operation_is_active(", wifi).replace(
        "another_radio_operation_is_active", "wifi_another_radio_operation_is_active"))
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-cache"),
           ZIG_LOCAL_CACHE_DIR=str(out / "zig-local"))
for extended in (0, 1):
    exe = out / f"c5-ble-lifecycle-{extended}.exe"
    subprocess.run([
        sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1", "-UNDEBUG",
        "-Wall", "-Wextra", "-Werror", f"-DTEST_EXTENDED={extended}",
        "-I", str(out), "-I", str(root / "tools/host_checks/stubs"),
        "-I", str(root / "shared"), "-I", str(root / "c5_firmware/main"),
        str(root / "tools/host_checks/c5_ble_lifecycle_check.c"),
        str(root / "shared/cerberus_watch_capture.c"),
        str(root / "shared/sentinel_watch_protocol.c"), "-o", str(exe),
    ], check=True, env=env)
    subprocess.run([str(exe)], check=True)
