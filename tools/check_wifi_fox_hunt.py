"""Exercise Wi-Fi Fox Hunt tracking and search through the actual C5 worker."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/wifi-fox-hunt-checks"
out.mkdir(parents=True, exist_ok=True)
source = (root / "c5_firmware/main/wifi_scanner.c").read_text()
start = source.index("static void wifi_scan_task(void *arg)")
(out / "wifi_scan_worker_impl.h").write_text(source[start:source.index(
    "esp_err_t cerberus_wifi_watch_get_status(", start)])
# CTRL must reach the shared callback before the legacy management length gate.
callback = source[source.index("static void wifi_promiscuous_callback("):source.index(
    "static void wifi_scan_task(void *arg)")]
assert callback.index("packet_monitor_receive(packet, type)") < callback.index("if(type==WIFI_PKT_MGMT")
exe = out / "wifi-fox-hunt-check.exe"
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-cache"))
subprocess.run([
    sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1",
    "-Wall", "-Wextra", "-Werror", "-I", str(out),
    "-I", str(root / "c5_firmware/main"), "-I", str(root / "shared"),
    "-I", str(root / "tools/host_checks/stubs"),
    str(root / "tools/host_checks/wifi_fox_hunt_check.c"),
    str(root / "shared/wifi_packet_monitor_record.c"),
    str(root / "shared/wifi_capture_ring.c"),
    str(root / "shared/wifi_packet_count.c"), "-o", str(exe),
], check=True, env=env)
subprocess.run([str(exe)], check=True, env=env)
