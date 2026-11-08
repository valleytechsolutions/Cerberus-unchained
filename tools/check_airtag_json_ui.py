"""Actual offline JSON renderer/controller and Files action routes.

The asynchronous worker is a counted boundary with real codec conversion and
round-trip bytes. Native worker/storage suites cover durable IO and cancellation.
No radio, live inventory, clock or SDK operation is linked into this executable.
"""
from pathlib import Path
import os
import struct
import subprocess
import sys
import zlib

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/airtag-json-ui-checks"
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
(out / "archive_framebuffer.h").write_text(frame, encoding="utf-8")
prefix = (root / "tools/host_checks/ble_archive_ui_check.c").read_text(encoding="utf-8").split('#include "ble_archive_ui.h"', 1)[0]
prefix = prefix.replace('return error==ESP_ERR_NO_MEM?',
    'return error==ESP_ERR_INVALID_SIZE?"ESP_ERR_INVALID_SIZE":error==ESP_ERR_NOT_FOUND?"ESP_ERR_NOT_FOUND":error==ESP_ERR_NO_MEM?')
(out / "airtag_adapters.h").write_text(prefix, encoding="utf-8")
ui = (root / "s3_main/main/ui.c").read_text(encoding="utf-8")
def block(needle, source=ui):
    begin = source.index(needle)
    opening = source.index("{", begin)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[begin:end]

(out / "airtag_file_routes.h").write_text(
    block("static esp_err_t draw_file_actions(void)\n{") + "\n" +
    block("static void handle_file_actions_tap(uint16_t x, uint16_t y)\n{"), encoding="utf-8")
adapter = (root / "s3_main/main/ble_archive_live_ui.h").read_text(encoding="utf-8")
(out / "airtag_archive_routes.h").write_text(
    "static esp_err_t archive_page_draw(void) {\n" +
    "if (airtag_json_is_open()) return airtag_json_draw();\nreturn ble_archive_ui_draw(ble_archive_view,ESP_OK);\n}\n"
    "static void archive_page_tap(uint16_t x,uint16_t y) {\n" +
    block("if (airtag_json_is_open()) {", adapter) + "\n" +
    "ble_archive_ui_action_t action=ble_archive_ui_tap(ble_archive_view,x,y);\n" +
    "if(action==BLE_ARCHIVE_UI_EXPORT) airtag_json_open_export();\n}\n", encoding="utf-8")
assert "if (airtag_json_is_open()) return airtag_json_draw();" in adapter
assert "else if (action == BLE_ARCHIVE_UI_EXPORT) airtag_json_open_export();" in adapter
assert "if (airtag_json_tick(asleep)) return;" in adapter
assert ui.index('#include "airtag_json_live_ui.h"') > ui.index('#include "ble_archive_live_ui.h"')
assert "ble_archive_tick(screen_asleep);" in ui
# The normal file-list route must still use text for LOG/JSON; only ACTIONS
# opts into structured import. This is intentionally independent of the suffix test.
ordinary = block("static void handle_files_tap(uint16_t x, uint16_t y)\n{")
assert "airtag_json_open_file" not in ordinary
assert "ble_archive_open_file(file_path, file_entries[index].name);" in ordinary
assert "open_text_file(file_entries[index].name);" in ordinary

exe = out / ("airtag-json-ui-check.exe" if os.name == "nt" else "airtag-json-ui-check")
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-global"), ZIG_LOCAL_CACHE_DIR=str(out / "zig-local"))
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
    "-I", str(out), "-I", str(root / "tools/host_checks/stubs"), "-I", str(root / "s3_main/main"),
    "-I", str(root / "shared"), str(root / "tools/host_checks/airtag_json_ui_check.c"),
    str(root / "s3_main/main/airtag_json.c"), str(root / "s3_main/main/ble_archive.c"),
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
