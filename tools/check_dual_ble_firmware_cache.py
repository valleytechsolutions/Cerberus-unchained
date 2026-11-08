"""Fault-inject production C5 BLE firmware-cache recovery and the shared deadline."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/dual-ble-firmware-cache-checks"
out.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-global-cache"),
           ZIG_LOCAL_CACHE_DIR=str(out / "zig-local-cache"))

exe = out / ("dual-ble-firmware-cache.exe" if os.name == "nt" else "dual-ble-firmware-cache")
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1", "-UNDEBUG",
    "-Wall", "-Wextra", "-Werror", "-I", str(root / "tools/host_checks/stubs"),
    "-I", str(root / "shared"), "-I", str(root / "s3_main/main"),
    str(root / "tools/host_checks/dual_ble_firmware_cache_check.c"),
    "-o", str(exe)], check=True, env=env)
subprocess.run([str(exe)], check=True, env=env)
