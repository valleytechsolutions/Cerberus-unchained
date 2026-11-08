"""Exercise production C5 planned capture dispatch, owner loop and RX service."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / 'artifacts/wifi-capture-plan-native-checks'
out.mkdir(parents=True, exist_ok=True)
source = (root / 'c5_firmware/main/main.c').read_text()
start = source.index('    case CERBERUS_SPI_COMMAND_WIFI_CAPTURE_PLAN:')
(out / 'wifi_capture_plan_dispatch.h').write_text(source[start:source.index('    /* BEGIN PACKET COUNT DISPATCH */', start)])
exe = out / 'check.exe'
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / 'zig-cache'))
subprocess.run([sys.executable, '-m', 'ziglang', 'cc', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
    '-I', str(out), '-I', str(root / 'tools/host_checks/stubs'),
    '-I', str(root / 'c5_firmware/main'), '-I', str(root / 'shared'),
    str(root / 'tools/host_checks/wifi_capture_plan_native_check.c'),
    str(root / 'shared/wifi_capture_ring.c'), str(root / 'shared/wifi_capture_filter.c'),
    str(root / 'shared/wifi_capture_plan.c'), str(root / 'shared/wifi_packet_count.c'),
    '-o', str(exe)], check=True, env=env)
subprocess.run([str(exe)], check=True, env=env)
