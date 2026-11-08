"""Exercise production completed-sweep Recon inventory without SDK or radio stubs."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/recon-inventory-checks"
out.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-cache"))
exe = out / ("recon-inventory-check.exe" if os.name == "nt" else "recon-inventory-check")
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1",
    "-Wall", "-Wextra", "-Werror", "-I", str(root / "s3_main/main"),
    str(root / "tools/host_checks/recon_inventory_check.c"),
    str(root / "s3_main/main/recon_inventory.c"),
    str(root / "s3_main/main/recon_journal.c"), "-o", str(exe)], check=True, env=env)
subprocess.run([str(exe)], check=True, env=env)
