"""Compile the production Locate adapter against deterministic SPI boundaries."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / 'artifacts/mesh-locate-link-checks'
out.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out/'zig-global'), ZIG_LOCAL_CACHE_DIR=str(out/'zig-local'))
exe = out / ('mesh-locate-link-check.exe' if os.name == 'nt' else 'mesh-locate-link-check')
subprocess.run([sys.executable, '-m', 'ziglang', 'cc', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
    '-I', str(root/'tools/host_checks/stubs'), '-I', str(root/'shared'), '-I', str(root/'s3_main/main'),
    str(root/'tools/host_checks/mesh_locate_link_check.c'), '-o', str(exe)], check=True, env=env)
subprocess.run([str(exe)], check=True, env=env)
