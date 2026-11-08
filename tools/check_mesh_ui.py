"""Actual Mesh UI/controller/model, isolated framebuffer and route boundaries."""
from pathlib import Path
import os
import re
import struct
import subprocess
import sys
import zlib

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/mesh-ui-checks"
out.mkdir(parents=True, exist_ok=True)
(out / "sdmmc_cmd.h").write_text("#pragma once\ntypedef struct sdmmc_card sdmmc_card_t;\n")
display = (root / "s3_main/main/display.c").read_text()
start = display.index("static const uint8_t *font5x7(")
(out / "display_font.h").write_text(display[start:display.index("\nesp_err_t ", start)])
frame = (root / "tools/host_checks/recon_render.c").read_text().split("/* Exercise the real private controls", 1)[0]
frame = frame.replace("static unsigned calls;", "static unsigned calls,fail_after;\nstatic char drawn[65536];\nstatic size_t drawn_size;")
frame = frame.replace("    bounds(x,y,w,h);", "    if(fail_after&&calls+1U==fail_after){++calls;return ESP_FAIL;}\n    bounds(x,y,w,h);")
frame = frame.replace("    if(!*text)return ESP_OK;", "    if(fail_after&&calls+1U==fail_after){++calls;return ESP_FAIL;}\n"
    "    drawn_size+=(size_t)snprintf(drawn+drawn_size,sizeof(drawn)-drawn_size,\"%s\\n\",text);assert(drawn_size<sizeof(drawn));\n    if(!*text)return ESP_OK;")
(out / "mesh_framebuffer.h").write_text(frame)
ui = (root / "s3_main/main/ui.c").read_text()
assert '{"Mesh Detection", UI_PAGE_IEEE802154}' in ui
assert 'if (current_page == UI_PAGE_IEEE802154 && ieee802154_mode == UI_IEEE802154_MODE_MESH) return mesh_ui_draw();' in ui
assert '#include "mesh_ui.h"' in ui
begin = ui.index('} else if (current_page == UI_PAGE_IEEE802154 &&\n                ieee802154_mode == UI_IEEE802154_MODE_MESH) {')
back = ui[begin:].split('{', 1)[1].split('} else if', 1)[0]
footer = ui[begin:ui.index('            } else if (current_page == UI_PAGE_SCRIPTS', begin)]
footer = footer.replace('} else if', 'if', 1) + '} else { return_to_launcher(); }'
begin = ui.index('static void return_to_launcher(void)\n{')
launcher = ui[begin:ui.index('\nstatic void open_learning_lesson', begin)]
begin = ui.index('else if (action == CERBERUS_FIELD_RADIO)')
field = ui[begin:ui.index(';', begin) + 1].replace('else if', 'if', 1)
begin = ui.index('        tool_parent_page = UI_PAGE_FIELD_MENU;', ui.index('if (current_page == UI_PAGE_FIELD_MENU)', ui.index('static void open_tool_page')))
field_parent = ui[begin:ui.index('        if (action == CERBERUS_FIELD_RECON)', begin)]
begin = ui.index('    } else if (current_page == UI_PAGE_IEEE802154) {', ui.index('const char *back_label'))
footer_label = ui[begin:ui.index('\n    if (current_page == UI_PAGE_WIFI', begin)].replace('} else if', 'if', 1)
begin = ui.index('    if (live_is_view() && current_page != UI_PAGE_IEEE802154)')
footer_override = ui[begin:ui.index('    if (learning_tool_active)', begin)]
footer_draw = 'return draw_button(64, UI_BACK_Y, 192, UI_BACK_HEIGHT, back_label);'
assert footer_draw in ui
footer_defines = '\n'.join(re.findall(r'^#define UI_BACK_(?:Y|HEIGHT) .+$', ui, re.M))
begin = ui.index('        mesh_ui_tick((uint64_t)now_ms,')
tick = 'mesh_archive_tick(screen_asleep);\n' + ui[begin:ui.index(';', begin) + 1]
begin = ui.index('    if (current_page == UI_PAGE_IEEE802154) {', ui.index('static void open_tool_page(ui_page_t page)\n{'))
entry = ui[begin:].split('{', 1)[1].split('}', 1)[0]
begin = ui.index('    if (ieee802154_mode == UI_IEEE802154_MODE_MENU) {', ui.index('static void handle_ieee802154_tap('))
menu = ui[begin:ui.index('\n    if (y < 364U', begin)]
live = (root / "s3_main/main/sentinel_live_ui.h").read_text()
begin = live.index('static bool live_is_view(void)')
live_view = live[begin:live.index('\nstatic void live_status', begin)]
assert 'const bool defer_c5 = live_wifi_capture_defers_c5() || mesh_ui_busy() || wifi_packet_count_ui_busy() || channel_analyzer_ui_busy() || wifi_fox_hunt_ui_busy();' in live
assert 'if (c5_online && !live_wifi_capture_defers_c5() && !mesh_ui_busy() && !wifi_packet_count_ui_busy() && !channel_analyzer_ui_busy() && !wifi_fox_hunt_ui_busy())' in live
begin = ui.index('            if (mesh_archive_is_file(file_entries[index].name)) {')
file_route = ui[begin:ui.index('            } else if', begin)] + '}'
file_route = file_route.replace('file_entries[index].name', 'name')
assert 'if (current_page == UI_PAGE_MESH_ARCHIVE) return mesh_archive_draw();' in ui
assert 'mesh_archive_tap(x, y);' in ui
assert '#include "mesh_archive_ui.h"' in ui
begin = ui.index('static esp_err_t draw_mesh_archive_empty_list(void)\n{')
empty_maps = ui[begin:ui.index('\nstatic esp_err_t draw_files(void)', begin)]
assert 'ESP_RETURN_ON_ERROR(draw_mesh_archive_empty_list(), TAG, "empty maps failed");' in ui
(out / "mesh_routes.h").write_text(empty_maps + '\nstatic void route_back(void){\n' + back + '}\n'
    + launcher + '\nstatic void route_footer(void){\n' + footer + '}\n'
    + live_view + footer_defines + '\nstatic esp_err_t route_draw_footer(void){const char *back_label="BACK";\n' + footer_label + footer_override + footer_draw + '}\n'
    'static void route_entry(void){\n' + entry + '}\n'
    'static void route_field(cerberus_field_action_t action){\n' + field_parent + field + '}\n'
    'static void route_file(const char *name){\n' + file_route + '}\n'
    'static void route_tick(uint64_t now_ms,bool screen_asleep){\n' + tick + '}\n'
    'static void route_tools(uint16_t x,uint16_t y){\n' + menu + '}\n')
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-global"), ZIG_LOCAL_CACHE_DIR=str(out / "zig-local"))
exe = out / ("mesh-ui.exe" if os.name == "nt" else "mesh-ui")
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-O1", "-Wall", "-Wextra", "-Werror",
    "-I", str(out), "-I", str(root / "tools/host_checks/stubs"), "-I", str(root / "s3_main/main"), "-I", str(root / "shared"),
    str(root / "tools/host_checks/mesh_ui_check.c"), str(root / "s3_main/main/mesh_scan.c"),
    str(root / "s3_main/main/mesh_locate.c"),
    str(root / "s3_main/main/mesh_archive.c"), str(root / "s3_main/main/mesh_report.c"),
    str(root / "s3_main/main/mesh_observation.c"), str(root / "shared/mesh_mac.c"),
    str(root / "s3_main/main/sentinel_hub.c"), str(root / "s3_main/main/field_menu.c"),
    str(root / "s3_main/main/ui_widgets.c"),
    "-o", str(exe)], check=True, env=env)
subprocess.run([str(exe), str(out)], check=True, env=env)

def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

for path in out.glob("*.ppm"):
    data = path.read_bytes()
    header = b"P6\n320 480\n255\n"
    assert data.startswith(header) and len(data) == len(header) + 320 * 480 * 3
    pixels = data[len(header):]
    path.with_suffix(".png").write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 320, 480, 8, 2, 0, 0, 0)) +
        chunk(b"IDAT", zlib.compress(b"".join(b"\0" + pixels[y*960:(y+1)*960] for y in range(480)))) + chunk(b"IEND", b""))
