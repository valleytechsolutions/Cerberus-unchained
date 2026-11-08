"""Exercise Wi-Fi Fox Hunt through the production C5 dispatcher and S3 model."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / 'artifacts/wifi-fox-hunt-integration-checks'
out.mkdir(parents=True, exist_ok=True)

# Existing SDK scheduling and SPI fixtures are reused in an isolated directory.
# The dispatcher, radio worker, adapter and controller come from production.
extractor = (root / 'tools/check_wifi_packet_monitor_session.py').read_text()
extractor = extractor.split('env = dict(', 1)[0]
extractor = extractor.replace('artifacts/packet-monitor-session-checks',
    'artifacts/wifi-fox-hunt-integration-checks')
exec(compile(extractor, str(root / 'tools/check_wifi_packet_monitor_session.py'), 'exec'),
    {'__file__': str(root / 'tools/check_wifi_packet_monitor_session.py')})
source = (root / 'c5_firmware/main/main.c').read_text()
start = min(source.index('    case CERBERUS_SPI_COMMAND_WIFI_PACKET_MONITOR_SESSION_INFO:'),
    source.index('    case CERBERUS_SPI_COMMAND_WIFI_CHANNEL_ANALYZER_INFO:'),
    source.index('    case CERBERUS_SPI_COMMAND_WIFI_FOX_HUNT_INFO:'))
end = source.index('    case CERBERUS_SPI_COMMAND_WIFI_PACKET_MONITOR_START:', start)
block = source[start:end]
for command in ('INFO', 'START', 'STATUS', 'STOP', 'SIGNAL'):
    assert 'CERBERUS_SPI_COMMAND_WIFI_FOX_HUNT_' + command in block
(out / 'monitor_dispatch.h').write_text(block)
fixture = (root / 'tools/host_checks/wifi_packet_monitor_bridge_check.c').read_text()
assert fixture.count('int main(void)') == 1
(out / 'monitor_bridge_fixture.h').write_text(fixture.replace(
    'int main(void)', 'int monitor_bridge_regression_suite(void)'))

env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / 'zig-cache'))
exe = out / ('wifi-fox-hunt-integration.exe' if os.name == 'nt' else 'wifi-fox-hunt-integration')
subprocess.run([sys.executable, '-m', 'ziglang', 'cc', '-std=c11', '-O1',
    '-Wall', '-Wextra', '-Werror', '-I', str(out),
    '-I', str(root / 'tools/host_checks/stubs'), '-I', str(root / 'tools/host_checks'),
    '-I', str(root / 'c5_firmware/main'), '-I', str(root / 's3_main/main'),
    '-I', str(root / 'shared'),
    str(root / 'tools/host_checks/wifi_fox_hunt_integration_check.c'),
    str(root / 's3_main/main/wifi_packet_monitor_scan.c'),
    str(root / 's3_main/main/wifi_fox_hunt_scan.c'),
    str(root / 'shared/wifi_packet_monitor_record.c'),
    str(root / 'shared/wifi_capture_ring.c'), str(root / 'shared/wifi_packet_count.c'),
    '-o', str(exe)], check=True, env=env)
subprocess.run([str(exe)], check=True, env=env)
