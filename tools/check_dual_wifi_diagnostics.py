"""Exercise production dual Wi-Fi snapshot, aggregate adapter and local console output."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/dual-wifi-diagnostics-checks"
(out / "freertos").mkdir(parents=True, exist_ok=True)
(out / "freertos/FreeRTOS.h").write_text("""#pragma once
#include <stdatomic.h>
typedef struct { atomic_flag flag; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED { ATOMIC_FLAG_INIT }
void dual_diag_test_enter(portMUX_TYPE *lock);
void dual_diag_test_exit(portMUX_TYPE *lock);
#define portENTER_CRITICAL(lock) dual_diag_test_enter(lock)
#define portEXIT_CRITICAL(lock) dual_diag_test_exit(lock)
""", encoding="utf-8")
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-global"),
           ZIG_LOCAL_CACHE_DIR=str(out / "zig-local"))
def function(source, signature):
    start = source.index(signature + "\n{")
    brace = source.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"

(out / "coordinator_publish.c.inc").write_text(function(
    (root / "s3_main/main/dual_wifi_scan.c").read_text(),
    "void cerberus_dual_wifi_scan_publish_diagnostics(int32_t ui_error)"))
(out / "print_wifi_dual.c.inc").write_text(function(
    (root / "s3_main/main/usb_console.c").read_text(), "static void print_wifi_dual(void)"))
exe = out / ("dual-wifi-diagnostics.exe" if os.name == "nt" else "dual-wifi-diagnostics")
command = [sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
    "-I", str(out), "-I", str(root / "tools/host_checks/stubs"),
    "-I", str(root / "s3_main/main"), "-I", str(root / "shared"),
    str(root / "tools/host_checks/dual_wifi_diagnostics_check.c"), "-o", str(exe)]
if os.name != "nt": command.append("-pthread")
subprocess.run(command, check=True, env=env)
subprocess.run([str(exe)], check=True, env=env)
