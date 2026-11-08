"""Exercise the production local Wi-Fi scanner/owners with deterministic SDK faults."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/s3-wifi-scanner-checks"
stubs = out / "stubs"
stubs.mkdir(parents=True, exist_ok=True)
for name in ("esp_err.h", "esp_event.h", "esp_netif.h", "esp_heap_caps.h",
             "esp_timer.h", "esp_wifi.h", "sdkconfig.h", "freertos/FreeRTOS.h",
             "freertos/task.h", "host/ble_hs.h", "host/util/util.h",
             "nimble/nimble_port.h", "nimble/nimble_port_freertos.h"):
    path = stubs / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("/* Types/functions supplied by the fault harness. */\n")
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-global"),
           ZIG_LOCAL_CACHE_DIR=str(out / "zig-local"))
exe = out / "s3-wifi-scanner-check.exe"
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1",
    "-Wall", "-Wextra", "-Werror", "-I", str(stubs),
    "-I", str(root / "s3_main/main"),
    "-I", str(root / "shared"),
    str(root / "tools/host_checks/s3_wifi_scanner_check.c"), "-o", str(exe)],
    check=True, env=env)
subprocess.run([str(exe)], check=True, env=env)
