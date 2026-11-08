"""Saved file actions, passive reacquisition, Fox return and actual UI rendering."""
from pathlib import Path
import os
import re
import struct
import subprocess
import sys
import zlib

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/saved-ble-lookup-ui-checks"
out.mkdir(parents=True, exist_ok=True)
display = (root / "s3_main/main/display.c").read_text()
start = display.index("static const uint8_t *font5x7(")
(out / "display_font.h").write_text(display[start:display.index("\nesp_err_t ", start)])
frame = (root / "tools/host_checks/recon_render.c").read_text().split("/* Exercise the real private controls", 1)[0]
frame = frame.replace("static unsigned calls;", "static unsigned calls, fail_after;\nstatic char drawn[65536];\nstatic size_t drawn_size;")
frame = frame.replace("    bounds(x,y,w,h);", "    if(fail_after&&calls+1U==fail_after){++calls;return ESP_FAIL;}\n    bounds(x,y,w,h);")
frame = frame.replace("    if(!*text)return ESP_OK;", "    if(fail_after&&calls+1U==fail_after){++calls;return ESP_FAIL;}\n"
    "    drawn_size+=(size_t)snprintf(drawn+drawn_size,sizeof(drawn)-drawn_size,\"%s\\n\",text);assert(drawn_size<sizeof(drawn));\n    if(!*text)return ESP_OK;")
(out / "lookup_framebuffer.h").write_text(frame)
native = (root / "tools/host_checks/saved_ble_lookup_check.c").read_text()
(out / "lookup_owner_fixture.h").write_text(native.replace("int main(void)", "static int lookup_component_main(void)"))
ui = (root / "s3_main/main/ui.c").read_text()
archive = (root / "s3_main/main/ble_archive_live_ui.h").read_text()
json = (root / "s3_main/main/airtag_json_live_ui.h").read_text()
fox = (root / "s3_main/main/dual_ble_live_ui.h").read_text()

def block(text, needle, offset=0):
    start = text.index(needle, offset)
    opening = text.index("{", start)
    cursor, depth = opening + 1, 1
    while depth:
        depth += (text[cursor] == "{") - (text[cursor] == "}")
        cursor += 1
    return text[start:cursor]

(out / "lookup_dispose.h").write_text(block(archive, "static void ble_archive_dispose(void)") + "\n" +
    block(json, "static void airtag_json_dispose(void)") + "\n" + block(json, "static void airtag_json_close(bool redraw)"))
routes = block(archive, "static esp_err_t ble_archive_draw(void)") + "\n"
routes += block(json, "static void airtag_json_tap(uint16_t x, uint16_t y)") + "\n"
routes += block(archive, "static void ble_archive_tap(uint16_t x, uint16_t y)") + "\n"
tick = block(json, "static bool airtag_json_tick(bool asleep)")
routes += tick[:tick.index("    cerberus_ble_archive_io_status_t status;")] + '    assert(!"Unexpected file worker");return true;\n}\n'
tick = block(archive, "static void ble_archive_tick(bool asleep)")
routes += tick[:tick.index("    if (ble_archive_view && ble_archive_view->release_pending)")] + "}\n"
(out / "lookup_file_routes.h").write_text(routes)
routes = block(fox, "static bool ble_fox_hunt_open_peer(") + "\n" + block(ui, "static void ble_fox_hunt_finish_exit(") + "\n"
offset = ui.index("static void ble_fox_hunt_tick(uint32_t now_ms, bool asleep)")
routes += "static void actual_fox_release_tick(uint32_t now_ms,bool asleep){\n" + block(ui, "if (ble_fox_hunt_exit_pending) {", offset) + "\n}\n"
(out / "lookup_fox_routes.h").write_text(routes)
assert '#include "saved_ble_lookup_ui.h"' in ui
assert "if (saved_ble_lookup_tick(asleep)) return;" in archive
assert "if (saved_ble_lookup_is_open()) return saved_ble_lookup_draw();" in archive
assert "if (action == AIRTAG_JSON_FIND_LIVE) { saved_ble_lookup_open(true); return; }" in json
assert "else if (action == BLE_ARCHIVE_UI_FIND_LIVE) saved_ble_lookup_open(false);" in archive
assert ui.index("if (saved_ble_lookup_fox_return(asleep)) return;") < ui.index("/* Hidden cleanup must not navigate away from a newer page. */", ui.index("static void ble_fox_hunt_finish_exit("))
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-global"), ZIG_LOCAL_CACHE_DIR=str(out / "zig-local"))
exe = out / "lookup-ui.exe"
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
    "-I", str(out), "-I", str(root / "tools/host_checks/stubs"), "-I", str(root / "tools/host_checks"),
    "-I", str(root / "s3_main/main"), "-I", str(root / "shared"),
    str(root / "tools/host_checks/saved_ble_lookup_ui_check.c"), str(root / "s3_main/main/saved_ble_lookup.c"),
    str(root / "s3_main/main/dual_ble_scan.c"), str(root / "s3_main/main/s3_ble_discovery_session.c"),
    str(root / "s3_main/main/ble_archive.c"), str(root / "s3_main/main/airtag_json.c"), "-o", str(exe)], check=True, env=env)
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
