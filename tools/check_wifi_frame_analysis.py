"""Check delivered-frame summaries and bounded passive disconnect evidence."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/wifi-frame-analysis-checks"
out.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-global-cache"),
           ZIG_LOCAL_CACHE_DIR=str(out / "zig-local-cache"))
exe = out / "check.exe"
sources = ["shared/wifi_frame_analysis.c", "shared/wifi_capture_filter.c",
           "shared/wifi_capture_plan.c", "shared/wifi_packet_count.c",
           "tools/host_checks/wifi_frame_analysis_check.c"]
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1",
    "-Wall", "-Wextra", "-Werror", "-I", str(root / "shared"),
    *[str(root / source) for source in sources], "-o", str(exe)], check=True, env=env)
subprocess.run([str(exe)], check=True, timeout=30)
