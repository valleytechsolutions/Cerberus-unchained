"""Check the pure production Recent Networks ring without SDK, SD or radios."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/wardrive-recent-checks"
out.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-global"),
    ZIG_LOCAL_CACHE_DIR=str(out / "zig-local"))
exe = out / ("wardrive-recent-check.exe" if os.name == "nt" else "wardrive-recent-check")
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1",
    "-Wall", "-Wextra", "-Werror", "-I", str(root / "s3_main/main"),
    "-I", str(root / "shared"),
    str(root / "s3_main/main/wardrive_recent.c"),
    str(root / "s3_main/main/wardrive_core.c"),
    str(root / "tools/host_checks/wardrive_recent_check.c"),
    "-o", str(exe)], check=True, env=env)
subprocess.run([str(exe)], check=True, env=env)
