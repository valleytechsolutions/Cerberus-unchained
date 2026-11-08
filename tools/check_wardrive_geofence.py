"""Exercise the production geofence policy without a receiver or private locations."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/wardrive-geofence-checks"
out.mkdir(parents=True, exist_ok=True)
environment = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-cache"))
executable = out / "wardrive-geofence-check.exe"
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
    "-I", str(root / "s3_main/main"), str(root / "s3_main/main/wardrive_geofence.c"),
    str(root / "tools/host_checks/wardrive_geofence_check.c"), "-lm", "-o", str(executable)],
    check=True, env=environment)
subprocess.run([str(executable)], check=True, env=environment)
