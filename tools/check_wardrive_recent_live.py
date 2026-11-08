"""Exercise the actual Recent Networks adapter and compact console printer.

The model, renderer and adapter are production code. Service getters, clock,
allocator and Stop are counted boundaries; no RF or SD operation is linked.
"""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / 'artifacts/wardrive-recent-live-checks'
out.mkdir(parents=True, exist_ok=True)
ui = (root / 's3_main/main/ui.c').read_text(encoding='utf-8')
# Compile the actual final repaint guard, including its complete condition and
# draw/update body. The full acquisition tick is exercised by wardrive_ui.
tick_start = ui.index('static void wardrive_tick(uint32_t now_ms, bool asleep)\n{')
tick = ui[tick_start:ui.index('static void handle_tap(uint16_t x, uint16_t y)\n{', tick_start)]
repaint = tick[tick.index('    if (!asleep && (wardrive_last_draw_ms'):tick.rindex('\n}')]
(out / 'recent_repaint_impl.h').write_text(
    'static void actual_wardrive_repaint(uint32_t now_ms,bool asleep)\n{\n' + repaint + '\n}\n', encoding='utf-8')
display = (root / 's3_main/main/display.c').read_text(encoding='utf-8')
start = display.index('static const uint8_t *font5x7(')
(out / 'display_font.h').write_text(display[start:display.index('\nesp_err_t ', start)], encoding='utf-8')
frame = (root / 'tools/host_checks/recon_render.c').read_text(encoding='utf-8').split('/* Exercise the real private controls', 1)[0]
frame = frame.replace('static unsigned calls;', 'static unsigned calls, fail_after;\nstatic char drawn[65536];\nstatic size_t drawn_size;')
frame = frame.replace('    bounds(x,y,w,h);', '    if (fail_after && calls + 1U == fail_after) { ++calls; return ESP_FAIL; }\n    bounds(x,y,w,h);')
frame = frame.replace('    if(!*text)return ESP_OK;', '    drawn_size += (size_t)snprintf(drawn + drawn_size, sizeof(drawn) - drawn_size, "%s\\n", text);\n    assert(drawn_size < sizeof(drawn));\n    if(!*text)return ESP_OK;')
(out / 'wardrive_recent_framebuffer.h').write_text(frame, encoding='utf-8')
prefix = (root / 'tools/host_checks/wardrive_recent_ui_check.c').read_text(encoding='utf-8').split('#include "wardrive_recent_ui.h"', 1)[0]
(out / 'recent_live_adapters.h').write_text(prefix, encoding='utf-8')
console = (root / 's3_main/main/usb_console.c').read_text(encoding='utf-8')
start = console.index('static void print_wardrive_recent(void)')
(out / 'recent_console_impl.h').write_text(console[start:console.index('static void print_radio(void)', start)], encoding='utf-8')
exe = out / ('wardrive-recent-live-check.exe' if os.name == 'nt' else 'wardrive-recent-live-check')
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / 'zig-global'), ZIG_LOCAL_CACHE_DIR=str(out / 'zig-local'))
subprocess.run([sys.executable, '-m', 'ziglang', 'cc', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
    '-I', str(out), '-I', str(root / 'tools/host_checks/stubs'), '-I', str(root / 's3_main/main'),
    '-I', str(root / 'shared'), str(root / 'tools/host_checks/wardrive_recent_live_check.c'),
    str(root / 's3_main/main/wardrive_recent.c'), str(root / 's3_main/main/wardrive_core.c'),
    str(root / 's3_main/main/privacy.c'), '-o', str(exe)], check=True, env=env)
subprocess.run([str(exe)], check=True, env=env)
for route in ('#include "wardrive_recent_live_ui.h"',
              'if (wardrive_recent_is_open()) return wardrive_recent_draw();',
              'wardrive_recent_release_if_left();',
              '(wardrive_recent_is_open() || wardrive_gps_view_is_open() || wardrive_logs_is_open() || wardrive_areas_is_open() ? 500U : 1000U)',
              'wardrive_recent_close();'):
    assert route in ui, route
tap = ui[ui.index('static void handle_tap(uint16_t x, uint16_t y)\n{'):]
assert tap.index('wardrive_recent_handle_tap(x, y);') < tap.index('if (current_page != UI_PAGE_TOOLS) {')
assert 'print_wardrive_recent();' in console[console.index('static void print_radio(void)\n{'):]
print('Actual recent adapter, bounded compact diagnostics and production entry points passed.')
