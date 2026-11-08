"""Actual local Packet Count producer with SDK faults and exact-session calls."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/s3-wifi-packet-count-checks"
stubs = out / "stubs"
for name in ("esp_err.h", "esp_event.h", "esp_netif.h", "esp_heap_caps.h",
             "esp_timer.h", "esp_wifi.h", "freertos/FreeRTOS.h", "freertos/task.h"):
    path = stubs / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("/* SDK boundary supplied by production-source fixture. */\n")
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-global"),
           ZIG_LOCAL_CACHE_DIR=str(out / "zig-local"))
exe = out / "producer.exe"
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1",
    "-Wall", "-Wextra", "-Werror", "-I", str(stubs), "-I", str(root / "shared"),
    "-I", str(root / "s3_main/main"),
    str(root / "tools/host_checks/s3_wifi_packet_count_check.c"),
    str(root / "shared/wifi_packet_count.c"), str(root / "shared/wifi_capture_ring.c"),
    "-o", str(exe)], check=True, env=env)
subprocess.run([str(exe)], check=True, env=env)
