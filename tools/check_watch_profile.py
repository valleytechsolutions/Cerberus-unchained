"""Exercise actual Remote ID profile decoding, canonical values and group ages."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/watch-profile-checks"
out.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-global-cache"),
           ZIG_LOCAL_CACHE_DIR=str(out / "zig-local-cache"))
exe = out / "check.exe"
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1",
    "-Wall", "-Wextra", "-Werror", "-I", str(root / "shared"),
    str(root / "tools/host_checks/watch_profile_check.c"),
    str(root / "shared/sentinel_watch_protocol.c"),
    str(root / "shared/cerberus_watch_capture.c"), "-o", str(exe)], check=True, env=env)
subprocess.run([str(exe)], check=True, timeout=30)
