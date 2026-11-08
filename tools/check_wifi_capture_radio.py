"""Exercise production C5 capture radio lifecycle with deterministic ESP edges."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/wifi-capture-radio-checks"
out.mkdir(parents=True, exist_ok=True)
exe = out / "wifi-capture-radio-check.exe"
env = os.environ.copy()
env["ZIG_GLOBAL_CACHE_DIR"] = str(out / "zig-cache")
subprocess.run([
    sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1",
    "-Wall", "-Wextra", "-Werror",
    "-I", str(root / "tools/host_checks/stubs"),
    "-I", str(root / "c5_firmware/main"), "-I", str(root / "shared"),
    str(root / "tools/host_checks/wifi_capture_radio_check.c"),
    str(root / "shared/wifi_capture_ring.c"),
    str(root / "shared/wifi_capture_filter.c"),
    str(root / "shared/wifi_capture_plan.c"),
    str(root / "shared/wifi_packet_count.c"), "-o", str(exe),
], check=True, env=env)
subprocess.run([str(exe)], check=True, env=env)

# Keep the callback-entry provenance test on the production outer receiver. The
# Watch fixture provides inactive boundaries for unrelated receive consumers.
source = (root / "c5_firmware/main/wifi_scanner.c").read_text()
pieces = []
for first, after in (
    ("static void wifi_promiscuous_callback(", "static void reset_defense_capture("),
    ("esp_err_t cerberus_wifi_watch_get_timing(", "esp_err_t cerberus_wifi_scanner_init("),
):
    start = source.index(first)
    pieces.append(source[start:source.index(after, start)])
(out / "c5_wifi_watch_timing_impl.h").write_text("\n".join(pieces))
exe = out / "wifi-capture-callback-check.exe"
subprocess.run([
    sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1",
    "-Wall", "-Wextra", "-Werror", "-I", str(out),
    "-I", str(root / "tools/host_checks/stubs"),
    "-I", str(root / "c5_firmware/main"), "-I", str(root / "shared"),
    str(root / "tools/host_checks/wifi_capture_callback_check.c"),
    str(root / "c5_firmware/main/wifi_capture_service.c"),
    str(root / "shared/wifi_capture_ring.c"),
    str(root / "shared/wifi_capture_filter.c"),
    str(root / "shared/wifi_capture_plan.c"),
    str(root / "shared/wifi_packet_count.c"),
    str(root / "shared/cerberus_watch_capture.c"),
    str(root / "shared/sentinel_watch_protocol.c"), "-o", str(exe),
], check=True, env=env)
subprocess.run([str(exe)], check=True, env=env)
