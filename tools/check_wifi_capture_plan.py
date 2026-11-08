"""Check channel coverage and immutable AP-list normalization without hardware."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/wifi-capture-plan-checks"
out.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-global"),
           ZIG_LOCAL_CACHE_DIR=str(out / "zig-local"))
exe = out / "plan.exe"
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1",
    "-Wall", "-Wextra", "-Werror", "-I", str(root / "shared"),
    str(root / "tools/host_checks/wifi_capture_plan_check.c"),
    str(root / "shared/wifi_capture_plan.c"),
    str(root / "shared/wifi_packet_count.c"), "-o", str(exe)], check=True, env=env)
subprocess.run([str(exe)], check=True, timeout=30)
