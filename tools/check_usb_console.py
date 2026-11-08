"""Run actual console formatting/task/lifecycle with injected SDK boundaries.

No device or build tools are used. The SDK/RTOS mocks exercise deterministic
interleavings; this is not a measurement of USB timing or native stack use.
"""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/usb-console-checks"
out.mkdir(parents=True, exist_ok=True)
source = (root / "s3_main/main/usb_console.c").read_text(encoding="utf-8")


def function(signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for end in range(opening, len(source)):
        depth += (source[end] == "{") - (source[end] == "}")
        if depth == 0:
            return source[start:end + 1]
    raise ValueError(signature)


start = source.index("#define USB_CONSOLE_LINE_CAPACITY")
end = source.index("/* Keep command-local snapshots", start)
parts = [source[start:end]]
parts += [function(signature) for signature in (
    "static void console_write(const char *text)",
    "static void console_printf(const char *format, ...)",
    "static void print_banner(void)",
    "static void print_ble_archive(void)",
    "static void usb_console_task(void *argument)",
    "esp_err_t cerberus_usb_console_start(void)",
    "esp_err_t cerberus_usb_console_stop(void)",
)]
(out / "usb_console_impl.h").write_text("\n\n".join(parts), encoding="utf-8")
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-cache"),
           ZIG_LOCAL_CACHE_DIR=str(out / "zig-local"))
executable = out / ("usb-console-check.exe" if os.name == "nt" else "usb-console-check")
command = [sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1", "-UNDEBUG",
    "-Wall", "-Wextra", "-Werror", "-I", str(out),
    "-I", str(root / "tools/host_checks/stubs"), "-I", str(root / "shared"), "-I", str(root / "s3_main/main"),
    str(root / "tools/host_checks/usb_console_check.c"), "-o", str(executable)]
subprocess.run(command, check=True, env=env)
subprocess.run([str(executable)], check=True, env=env)
