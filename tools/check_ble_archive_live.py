"""Exercise production archive capture/controller with the real offline viewer.

Clock, allocator and async IO are counted boundaries. The actual private UI
adapter, codec and capture mapper run; no radio or filesystem call is linked.
"""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / 'artifacts/ble-archive-live-checks'
out.mkdir(parents=True, exist_ok=True)
display = (root / 's3_main/main/display.c').read_text(encoding='utf-8')
start = display.index('static const uint8_t *font5x7(')
(out / 'display_font.h').write_text(display[start:display.index('\nesp_err_t ', start)], encoding='utf-8')
frame = (root / 'tools/host_checks/recon_render.c').read_text(encoding='utf-8').split('/* Exercise the real private controls', 1)[0]
frame = frame.replace('static unsigned calls;', 'static unsigned calls, fail_after;\nstatic char drawn[65536];\nstatic size_t drawn_size;')
frame = frame.replace('    bounds(x,y,w,h);', '    if (fail_after && calls + 1U == fail_after) { ++calls; return ESP_FAIL; }\n    bounds(x,y,w,h);')
frame = frame.replace('    if(!*text)return ESP_OK;', '    drawn_size += (size_t)snprintf(drawn + drawn_size, sizeof(drawn) - drawn_size, "%s\\n", text);\n    assert(drawn_size < sizeof(drawn));\n    if(!*text)return ESP_OK;')
(out / 'archive_framebuffer.h').write_text(frame, encoding='utf-8')
prefix = (root / 'tools/host_checks/ble_archive_ui_check.c').read_text(encoding='utf-8').split('#include "ble_archive_ui.h"', 1)[0]
(out / 'archive_live_adapters.h').write_text(prefix, encoding='utf-8')
ui = (root / 's3_main/main/ui.c').read_text(encoding='utf-8')
def block(needle, source=ui):
    begin = source.index(needle)
    opening = source.index('{', begin)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[begin:end]

companion_ui = (root / 's3_main/main/dual_ble_ui.h').read_text(encoding='utf-8')
(out / 'archive_pending_cancel.h').write_text(
    block('static void dual_ble_ui_cancel_selection(void)', companion_ui), encoding='utf-8')
(out / 'archive_c5_footer.h').write_text(
    'static esp_err_t archive_root_footer_draw(void) {\n' +
    block('if (current_page == UI_PAGE_BLE && ble_mode == UI_BLE_MODE_C5) {') +
    '\nreturn ESP_OK;\n}\nstatic void archive_root_footer_tap(uint16_t x,uint16_t y) {\n' +
    block('if (!ui_capability_blocked && current_page == UI_PAGE_BLE && ble_mode == UI_BLE_MODE_C5 &&') +
    '\n}\nstatic esp_err_t archive_monitor_footer_draw(void) {\nconst char *back_label="BLE LIST";\n' +
    block('if (current_page == UI_PAGE_BLE && ble_mode == UI_BLE_MODE_FINDMY_MONITOR) {\n        ESP_RETURN_ON_ERROR(draw_button') +
    '\nreturn ESP_OK;\n}\nstatic void archive_monitor_footer_tap(uint16_t x,uint16_t y) {\n' +
    block('if (current_page == UI_PAGE_BLE && ble_mode == UI_BLE_MODE_FINDMY_MONITOR) {\n                if (x >= 164U') +
    '\n}\n', encoding='utf-8')
exe = out / ('ble-archive-live-check.exe' if os.name == 'nt' else 'ble-archive-live-check')
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / 'zig-global'), ZIG_LOCAL_CACHE_DIR=str(out / 'zig-local'))
subprocess.run([sys.executable, '-m', 'ziglang', 'cc', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
    '-I', str(out), '-I', str(root / 'tools/host_checks/stubs'), '-I', str(root / 's3_main/main'),
    '-I', str(root / 'shared'), str(root / 'tools/host_checks/ble_archive_live_check.c'),
    str(root / 's3_main/main/ble_archive.c'), str(root / 's3_main/main/dual_ble_scan.c'),
    '-o', str(exe)], check=True, env=env)
subprocess.run([str(exe)], check=True, env=env)

# Bind the exercised adapter to actual entry points, rather than assuming an
# isolated renderer makes the feature reachable. Live behavior is tested above.
for route in ('#include "ble_archive_live_ui.h"',
              'if (current_page == UI_PAGE_BLE_ARCHIVE) return ble_archive_draw();',
              'ble_archive_open_file(file_path, file_entries[index].name);',
              'if (item == 7) { ble_archive_open_menu(); return; }',
              'ble_archive_tick(screen_asleep);'):
    assert route in ui, route
assert ui.index('ble_archive_tap(x, y);') < ui.index('if (current_page != UI_PAGE_TOOLS) {')
assert 'ble_archive_save_selected();' in ui
print('Actual archive capture, job routing, Back, cleanup and production reachability passed.')
