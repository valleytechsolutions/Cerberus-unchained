"""Compile the production Wi-Fi Fox Hunt model and bounded S3 SPI adapter."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/wifi-fox-hunt-scan-checks"
out.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-cache"))
source = (root / "s3_main/main/wifi_packet_monitor_link.h").read_text()
start = source.index("static esp_err_t packet_monitor_session_gate_locked(")
(out / "fox_gate_impl.h").write_text(source[start:source.index(
    "static esp_err_t packet_monitor_session_exchange(", start)])

for name, extra in (
    ("scan", [root / "s3_main/main/wifi_fox_hunt_scan.c"]),
    ("link", []),
):
    exe = out / f"wifi-fox-hunt-{name}.exe"
    subprocess.run([
        sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1",
        "-Wall", "-Wextra", "-Werror", "-I", str(out),
        "-I", str(root / "tools/host_checks/stubs"), "-I", str(root / "s3_main/main"),
        "-I", str(root / "shared"), str(root / f"tools/host_checks/wifi_fox_hunt_{name}_check.c"),
        *(str(path) for path in extra), "-o", str(exe),
    ], check=True, env=env)
    subprocess.run([str(exe)], check=True, env=env)
