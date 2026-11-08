"""Check passive SAE metadata interpretation against production scope/model code."""
import os
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/wifi-sae-analysis-checks"
out.mkdir(parents=True, exist_ok=True)
exe = out / "check.exe"
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-cache"))
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1",
    "-Wall", "-Wextra", "-Werror", "-I", str(root / "shared"),
    str(root / "tools/host_checks/wifi_sae_analysis_check.c"),
    str(root / "shared/wifi_sae_analysis.c"),
    str(root / "shared/wifi_capture_plan.c"),
    str(root / "shared/wifi_packet_count.c"), "-o", str(exe)], check=True, env=env)
subprocess.run([str(exe)], check=True, timeout=30)
