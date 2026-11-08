"""Exercise the real portable BLE archive codec without SDK, filesystem, or RF."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/ble-archive-checks"
out.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-cache"),
           ZIG_LOCAL_CACHE_DIR=str(out / "zig-local-cache"))
for fixture in ("ble_archive_check", "dual_ble_archive_capture_check", "ble_archive_capture_check"):
    exe = out / (fixture + (".exe" if os.name == "nt" else ""))
    subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1", "-UNDEBUG",
        "-Wall", "-Wextra", "-Werror", "-I", str(root / "s3_main/main"),
        "-I", str(root / "shared"), "-I", str(root / "tools/host_checks/stubs"),
        str(root / f"tools/host_checks/{fixture}.c"),
        str(root / "s3_main/main/ble_archive.c"), "-o", str(exe)], check=True, env=env)
    subprocess.run([str(exe)], check=True, env=env)
