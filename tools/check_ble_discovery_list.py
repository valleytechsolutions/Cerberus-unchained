"""Exercise the production dual-receiver presentation helper without radios."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/ble-discovery-list-checks"
out.mkdir(parents=True, exist_ok=True)
executable = out / ("ble-discovery-list-check.exe" if os.name == "nt" else "ble-discovery-list-check")
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-cache"),
           ZIG_LOCAL_CACHE_DIR=str(out / "zig-local"))
subprocess.run([
    sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1", "-UNDEBUG",
    "-Wall", "-Wextra", "-Werror",
    "-I", str(root / "tools/host_checks/stubs"), "-I", str(root / "shared"),
    "-I", str(root / "s3_main/main"),
    str(root / "tools/host_checks/ble_discovery_list_check.c"),
    str(root / "s3_main/main/s3_ble_discovery_inventory.c"),
    "-o", str(executable),
], check=True, env=env)
subprocess.run([str(executable)], check=True, env=env)
