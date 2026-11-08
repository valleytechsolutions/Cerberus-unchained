"""Execute production optional recorder and C5 wide-frame adapter with faults."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / 'artifacts/wifi-packet-count-record-checks'
out.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out/'zig-global'), ZIG_LOCAL_CACHE_DIR=str(out/'zig-local'))
for name in ('wifi_packet_count_record_check', 'wifi_packet_count_link_check'):
    exe = out / (name + ('.exe' if os.name == 'nt' else ''))
    sources = [root/'tools/host_checks'/f'{name}.c', root/'shared/wifi_packet_count.c', root/'shared/wifi_pcap.c']
    subprocess.run([sys.executable, '-m', 'ziglang', 'cc', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
        '-I', str(root/'tools/host_checks/stubs'), '-I', str(root/'shared'), '-I', str(root/'s3_main/main'),
        *map(str, sources), '-o', str(exe)], check=True, env=env)
    subprocess.run([str(exe)], check=True, env=env)
