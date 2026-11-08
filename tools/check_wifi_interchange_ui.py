"""Actual offline Wi-Fi interchange renderer/controller and explicit Files routes.

Private framebuffer/cache. Worker is a counted asynchronous boundary using real
codec conversions and wire round trips; actual SD/worker ownership has its own
suite. No clock, receiver, RF, target table or SDK operations are linked here.
"""
from pathlib import Path
import os
import struct
import subprocess
import sys
import zlib

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/wifi-interchange-ui-checks"
out.mkdir(parents=True, exist_ok=True)
display = (root / "s3_main/main/display.c").read_text(encoding="utf-8")
start = display.index("static const uint8_t *font5x7(")
(out / "display_font.h").write_text(display[start:display.index("\nesp_err_t ", start)], encoding="utf-8")
frame = (root / "tools/host_checks/recon_render.c").read_text(encoding="utf-8").split("/* Exercise the real private controls", 1)[0]
frame = frame.replace("static unsigned calls;", "static unsigned calls, fail_after;\nstatic char drawn[65536];\nstatic size_t drawn_size;")
frame = frame.replace("    bounds(x,y,w,h);", "    if (fail_after && calls + 1U == fail_after) { ++calls; return ESP_FAIL; }\n    bounds(x,y,w,h);")
frame = frame.replace("    if(!*text)return ESP_OK;", "    if (fail_after && calls + 1U == fail_after) { ++calls; return ESP_FAIL; }\n"
    "    drawn_size += (size_t)snprintf(drawn + drawn_size, sizeof(drawn) - drawn_size, \"%s\\n\", text);\n"
    "    assert(drawn_size < sizeof(drawn));\n    if(!*text)return ESP_OK;")
(out / "interchange_framebuffer.h").write_text(frame, encoding="utf-8")
ui = (root / "s3_main/main/ui.c").read_text(encoding="utf-8")
def block(needle, source=ui):
    begin = source.index(needle)
    opening = source.index("{", begin)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[begin:end]

(out / "interchange_files.h").write_text(block("static esp_err_t draw_file_actions(void)\n{") + "\n" +
    block("static void handle_file_actions_tap(uint16_t x, uint16_t y)\n{"), encoding="utf-8")
# The file-actions body is followed by the common parent Back control. Retain
# the actual parent draw statement and actual action-page Back branch too.
footer = 'return draw_button(64, UI_BACK_Y, 192, UI_BACK_HEIGHT, back_label);'
assert footer in block("static esp_err_t draw_detail_page(void)\n{")
actions = '(file_mode == UI_FILE_MODE_TOOLS || file_mode == UI_FILE_MODE_SELECT || file_mode == UI_FILE_MODE_ACTIONS)) {'
back = ui.index(actions, ui.index('static void handle_touch')) if 'static void handle_touch' in ui else ui.rindex(actions)
opening = ui.index('{', back)
ending = ui.index('}', opening)
body = ui[opening + 1:ending]
assert 'file_mode = UI_FILE_MODE_LIST;' in body and 'draw_detail_page()' in body
(out / "interchange_files_parent.h").write_text(
    '\n'.join(line for line in ui.splitlines() if line.startswith(('#define UI_BACK_Y ', '#define UI_BACK_HEIGHT '))) + '\n'
    'static esp_err_t files_page_draw(void) {\n'
    'ESP_RETURN_ON_ERROR(draw_file_actions(),TAG,"file actions");\n'
    'const char *back_label="FILES";\n' + footer + '\n}\n'
    'static void files_actions_back(void) {\n' + body + '\n}\n', encoding="utf-8")
native = (root / "s3_main/main/wifi_archive_live_ui.h").read_text(encoding="utf-8")
(out / "interchange_native_routes.h").write_text(
    "static esp_err_t native_draw(void) {\n" + "if (wifi_interchange_is_open()) return wifi_interchange_draw();\n"
    "return wifi_archive_ui_draw(wifi_archive_view,ESP_OK);\n}\n"
    "static void native_tap(uint16_t x,uint16_t y) {\n" + block("if (wifi_interchange_is_open()) {", native) + "\n"
    "wifi_archive_ui_action_t action=wifi_archive_ui_tap(wifi_archive_view,x,y);\n"
    "if(action==WIFI_ARCHIVE_EXPORT) wifi_interchange_open_export();\n}\n", encoding="utf-8")
assert "if (wifi_interchange_is_open()) return wifi_interchange_draw();" in native
assert "wifi_interchange_open_export(); return;" in native
assert "if (wifi_interchange_tick(asleep)) return;" in native
assert ui.index('#include "wifi_interchange_live_ui.h"') > ui.index('#include "wifi_archive_live_ui.h"')
ordinary = block("static void handle_files_tap(uint16_t x, uint16_t y)\n{")
assert "wifi_interchange_open_file" not in ordinary
assert "open_text_file(file_entries[index].name);" in ordinary
assert "wifi_archive_open_file(file_path, file_entries[index].name);" in ordinary

exe = out / ("wifi-interchange-ui-check.exe" if os.name == "nt" else "wifi-interchange-ui-check")
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-global"), ZIG_LOCAL_CACHE_DIR=str(out / "zig-local"))
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
    "-I", str(out), "-I", str(root / "tools/host_checks/stubs"), "-I", str(root / "s3_main/main"),
    "-I", str(root / "shared"), str(root / "tools/host_checks/wifi_interchange_ui_check.c"),
    str(root / "s3_main/main/wifi_marauder.c"), str(root / "s3_main/main/wifi_archive.c"),
    "-o", str(exe)], check=True, env=env)
subprocess.run([str(exe), str(out)], check=True, env=env)
def chunk(kind, payload):
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))
for path in out.glob("*.ppm"):
    raw = path.read_bytes()
    header = b"P6\n320 480\n255\n"
    assert raw.startswith(header) and len(raw) == len(header) + 320 * 480 * 3
    pixels = raw[len(header):]
    rows = b"".join(b"\0" + pixels[y * 960:(y + 1) * 960] for y in range(480))
    path.with_suffix(".png").write_bytes(b"\x89PNG\r\n\x1a\n" +
        chunk(b"IHDR", struct.pack(">IIBBBBB", 320, 480, 8, 2, 0, 0, 0)) +
        chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))
