"""Actual Count scalar publication, command dispatch and bounded console formatting.

SDK/RTOS boundaries are mocked; this is not USB or radio timing evidence.
"""
from pathlib import Path
import os
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/wifi-packet-count-diagnostics-checks"
(out / "freertos").mkdir(parents=True, exist_ok=True)
(out / "freertos/FreeRTOS.h").write_text("""#pragma once
#include <stdatomic.h>
typedef struct { atomic_flag flag; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED { ATOMIC_FLAG_INIT }
void count_diag_test_enter(portMUX_TYPE *lock);
void count_diag_test_exit(portMUX_TYPE *lock);
#define portENTER_CRITICAL(lock) count_diag_test_enter(lock)
#define portEXIT_CRITICAL(lock) count_diag_test_exit(lock)
""", encoding="utf-8")


def function(source, signature):
    start = source.index(signature + "\n{")
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


source = (root / "s3_main/main/usb_console.c").read_text(encoding="utf-8")
assert "static __attribute__((noinline)) void print_packet_count(void);" in source
dispatch = function(source, "static void execute_command(char *line)")
stubs = sorted(set(re.findall(r"\b(print_[a-z0-9_]+)\(\);", dispatch)) - {"print_help", "print_packet_count"})
parts = [f"static void {name}(void) {{ assert(!\"unexpected diagnostic command\"); }}" for name in stubs]
parts += [function(source, signature) for signature in (
    "static void print_packet_count(void)", "static void print_help(void)")]
parts.append(dispatch)
(out / "count_diagnostic_console.inc").write_text("\n".join(parts), encoding="utf-8")
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-global"),
           ZIG_LOCAL_CACHE_DIR=str(out / "zig-local"))
exe = out / ("wifi-packet-count-diagnostics.exe" if os.name == "nt" else "wifi-packet-count-diagnostics")
command = [sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
    "-I", str(out), "-I", str(root / "tools/host_checks/stubs"),
    "-I", str(root / "s3_main/main"), "-I", str(root / "shared"),
    str(root / "tools/host_checks/wifi_packet_count_diagnostics_check.c"),
    str(root / "s3_main/main/wifi_packet_count_scan.c"),
    str(root / "shared/wifi_packet_count.c"), "-o", str(exe)]
if os.name != "nt":
    command.append("-pthread")
subprocess.run(command, check=True, env=env)
subprocess.run([str(exe)], check=True, env=env)
