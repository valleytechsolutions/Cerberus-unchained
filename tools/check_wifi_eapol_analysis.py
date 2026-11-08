"""Exercise passive EAPOL frame boundaries and per-station exchange evidence."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / 'artifacts/wifi-eapol-analysis-checks'
out.mkdir(parents=True, exist_ok=True)
exe = out / 'check.exe'
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / 'zig-cache'))
subprocess.run([sys.executable, '-m', 'ziglang', 'cc', '-std=c11', '-O1', '-UNDEBUG',
    '-Wall', '-Wextra', '-Werror', '-I', str(root / 'shared'),
    str(root / 'shared/wifi_eapol_analysis.c'),
    str(root / 'tools/host_checks/wifi_eapol_analysis_check.c'), '-o', str(exe)], check=True, env=env)
subprocess.run([str(exe)], check=True, env=env)
