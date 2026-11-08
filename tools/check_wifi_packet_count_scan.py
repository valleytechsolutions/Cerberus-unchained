"""Actual dual Packet Count controller and shared counter/projection core."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/wifi-packet-count-scan-checks"
out.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-global"),
           ZIG_LOCAL_CACHE_DIR=str(out / "zig-local"))
exe = out / ("packet-count-scan.exe" if os.name == "nt" else "packet-count-scan")
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
    "-I", str(root / "tools/host_checks/stubs"), "-I", str(root / "s3_main/main"), "-I", str(root / "shared"),
    str(root / "tools/host_checks/wifi_packet_count_scan_check.c"),
    str(root / "s3_main/main/wifi_packet_count_scan.c"), str(root / "shared/wifi_packet_count.c"),
    "-o", str(exe)], check=True, env=env)
subprocess.run([str(exe)], check=True, env=env)
