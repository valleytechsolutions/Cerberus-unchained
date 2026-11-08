"""Exercise the working SSID model with the production text codec."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/ssid-list-checks"
out.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-global"),
           ZIG_LOCAL_CACHE_DIR=str(out / "zig-local"))
exe = out / ("ssid-list-check.exe" if os.name == "nt" else "ssid-list-check")
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1", "-UNDEBUG",
    "-Wall", "-Wextra", "-Werror", "-I", str(root / "s3_main/main"),
    str(root / "tools/host_checks/ssid_list_check.c"),
    str(root / "s3_main/main/ssid_list.c"),
    str(root / "s3_main/main/wifi_marauder.c"), str(root / "s3_main/main/wifi_archive.c"),
    "-o", str(exe)], check=True, env=env)
subprocess.run([str(exe)], check=True, env=env, timeout=30)
