"""Fault-inject the actual receive-only dual-radio Wi-Fi coordinator."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/dual-wifi-scan-checks"
out.mkdir(parents=True, exist_ok=True)
(out / "esp_heap_caps.h").write_text("""#pragma once
#include <stddef.h>
#define MALLOC_CAP_SPIRAM 4U
#define MALLOC_CAP_8BIT 2U
void *heap_caps_calloc(size_t count, size_t size, unsigned caps);
""", encoding="utf-8")
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-global"),
           ZIG_LOCAL_CACHE_DIR=str(out / "zig-local"))
exe = out / ("dual-wifi-scan-check.exe" if os.name == "nt" else "dual-wifi-scan-check")
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
    "-I", str(out), "-I", str(root / "tools/host_checks/stubs"),
    "-I", str(root / "shared"), "-I", str(root / "s3_main/main"),
    str(root / "tools/host_checks/dual_wifi_scan_check.c"),
    str(root / "s3_main/main/wifi_archive.c"), str(root / "s3_main/main/wifi_archive_capture.c"),
    "-o", str(exe)], check=True, env=env)
subprocess.run([str(exe)], check=True, env=env)
