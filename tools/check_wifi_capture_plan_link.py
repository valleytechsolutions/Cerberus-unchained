"""Exercise actual S3 plan helpers over full passive SPI frames."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / 'artifacts/wifi-capture-plan-link-checks'
out.mkdir(parents=True, exist_ok=True)
source = (root / 's3_main/main/wifi_packet_monitor_link.h').read_text()
start = source.index('static esp_err_t packet_monitor_session_gate_locked(')
(out / 'capture_plan_gate_impl.h').write_text(source[start:source.index('static esp_err_t packet_monitor_session_exchange(', start)])
exe = out / 'check.exe'
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / 'zig-cache'))
subprocess.run([sys.executable, '-m', 'ziglang', 'cc', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
    '-I', str(out), '-I', str(root / 'tools/host_checks/stubs'),
    '-I', str(root / 's3_main/main'), '-I', str(root / 'shared'),
    str(root / 'tools/host_checks/wifi_capture_plan_link_check.c'),
    str(root / 'shared/wifi_capture_plan.c'), str(root / 'shared/wifi_packet_count.c'),
    '-o', str(exe)], check=True, env=env)
subprocess.run([str(exe)], check=True, env=env)
