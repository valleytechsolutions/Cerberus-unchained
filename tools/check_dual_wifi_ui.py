"""Exercise dual-radio list selection and ownership-aware navigation without RF."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/dual-wifi-ui-checks"
out.mkdir(parents=True, exist_ok=True)
(out / "esp_err.h").write_text("/* Harness supplies esp_err_t and errors. */\n")
scanner = (root / "s3_main/main/s3_wifi_scanner.c").read_text(encoding="utf-8")
start = scanner.index("size_t cerberus_s3_wifi_scanner_ssid_display(")
(out / "ssid_display_impl.h").write_text(scanner[start:], encoding="utf-8")
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-global"),
           ZIG_LOCAL_CACHE_DIR=str(out / "zig-local"))
exe = out / "dual-wifi-ui-check.exe"
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1",
    "-Wall", "-Wextra", "-Werror", "-I", str(out),
    "-I", str(root / "s3_main/main"), "-I", str(root / "shared"),
    str(root / "tools/host_checks/dual_wifi_ui_check.c"), "-o", str(exe)],
    check=True, env=env)
subprocess.run([str(exe)], check=True, env=env)
