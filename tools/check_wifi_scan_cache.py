"""Exercise actual no-I/O AP publication hydration for legacy detail views."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / 'artifacts/wifi-scan-cache-checks'
out.mkdir(parents=True, exist_ok=True)
source = (root / 's3_main/main/ui.c').read_text(encoding='utf-8')
end = source.index('} wifi_history_identity_t;') + len('} wifi_history_identity_t;')
start = source.rindex('typedef struct {', 0, end)
declarations_end = source.index('\n', source.index('static uint8_t wifi_snapshot_band =', end))
(out / 'cache_metadata.h').write_text(source[start:declarations_end] + '\n', encoding='utf-8')

def function(signature):
    start = source.index(signature)
    body = source.index('{', start)
    depth = 1
    end = body + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

parts = [function(s) for s in (
    'static void make_ssid_display_safe(',
    'static esp_err_t load_wifi_results(',
    'static const char *wifi_scan_coverage_text(',
    'static bool wifi_history_same_identity(',
    'static uint8_t wifi_channel_history_band_samples(',
    'static void record_wifi_channel_history(',
    'static void clear_wifi_channel_history(',
    'static void reset_wifi_channel_history_to_current(',
    'static uint16_t wifi_channel_history_total(',
    'static uint8_t wifi_channel_history_seen(',
    'static int8_t wifi_channel_history_best_rssi(',
    'static esp_err_t rebuild_wifi_channel_summary(',
    'static void wifi_commit_cached_results_at(',
    'static void wifi_commit_cached_results(',
    'static esp_err_t refresh_wifi_radio_stats(',
    'static esp_err_t refresh_wifi_scan_status(',
)]
# Exercise the actual telemetry invalidation trigger, not a parallel reset.
telemetry = function('static void refresh_c5_status(')
begin = telemetry.index('if (current_page == UI_PAGE_WIFI && wifi_link_changed) {')
opening = telemetry.index('{', begin)
depth, end = 1, opening + 1
while depth:
    depth += (telemetry[end] == '{') - (telemetry[end] == '}')
    end += 1
parts.append('static void telemetry_recovery(bool wifi_link_changed)\n{\n' +
             telemetry[begin:end] + '\n}\n')
(out / 'cache_impl.h').write_text('\n\n'.join(parts), encoding='utf-8')
env = os.environ.copy()
env['ZIG_GLOBAL_CACHE_DIR'] = str(out / 'zig-cache')
env['ZIG_LOCAL_CACHE_DIR'] = str(out / 'zig-local')
exe = out / 'wifi-scan-cache-check.exe'
subprocess.run([sys.executable, '-m', 'ziglang', 'cc', '-std=c11', '-O1',
    '-Wall', '-Wextra', '-Werror', '-I', str(out),
    '-I', str(root / 'tools/host_checks/stubs'), '-I', str(root / 'shared'),
    '-I', str(root / 's3_main/main'), str(root / 'tools/host_checks/wifi_scan_cache_check.c'),
    '-o', str(exe)], check=True, env=env)
subprocess.run([str(exe)], check=True, env=env)
