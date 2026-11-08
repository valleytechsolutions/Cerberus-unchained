"""Exercise the production Fox Hunt page, route guards, model and font renderer."""
from pathlib import Path
import os
import re
import struct
import subprocess
import sys
import zlib

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/wifi-fox-hunt-ui-checks"
out.mkdir(parents=True, exist_ok=True)
display = (root / "s3_main/main/display.c").read_text()
start = display.index("static const uint8_t *font5x7(")
(out / "display_font.h").write_text(display[start:display.index("\nesp_err_t ", start)])
frame = (root / "tools/host_checks/recon_render.c").read_text().split("/* Exercise the real private controls", 1)[0]
frame = frame.replace("static unsigned calls;", "static unsigned calls,fail_after;\nstatic char drawn[65536];\nstatic size_t drawn_size;")
frame = frame.replace("    bounds(x,y,w,h);", "    if(fail_after&&calls+1U==fail_after){++calls;return ESP_FAIL;}\n    bounds(x,y,w,h);")
frame = frame.replace("    if(!*text)return ESP_OK;", "    if(fail_after&&calls+1U==fail_after){++calls;return ESP_FAIL;}\n"
    "    drawn_size+=(size_t)snprintf(drawn+drawn_size,sizeof(drawn)-drawn_size,\"%s\\n\",text);assert(drawn_size<sizeof(drawn));\n    if(!*text)return ESP_OK;")
(out / "fox_framebuffer.h").write_text(frame)
ui = (root / "s3_main/main/ui.c").read_text()
modes = re.search(r"typedef enum \{\s*UI_WIFI_MODE_MENU,.*?\} ui_wifi_mode_t;", ui, re.S).group(0)
constants = "\n".join(re.search(rf"^#define {name} .+$", ui, re.M).group(0)
    for name in ["UI_BACK_Y", "UI_BACK_HEIGHT"])
(out / "fox_modes.h").write_text(modes + "\n" + constants)

def block(text, needle, offset=0):
    start = text.index(needle, offset)
    opening = text.index("{", start)
    cursor, depth = opening + 1, 1
    while depth:
        depth += (text[cursor] == "{") - (text[cursor] == "}")
        cursor += 1
    return text[start:cursor]

routes = ""
for name, signature, needle, args in [
    ("route_tap", "static void handle_wifi_tap(uint16_t x, uint16_t y)\n{", "if (wifi_mode == UI_WIFI_MODE_FOX_HUNT) {", "uint16_t x,uint16_t y"),
    ("route_global_tap", "static void handle_tap(uint16_t x, uint16_t y)\n{", "if (current_page == UI_PAGE_WIFI && wifi_mode == UI_WIFI_MODE_FOX_HUNT) {", "uint16_t x,uint16_t y"),
    ("route_launcher", "static void return_to_launcher(void)\n{", "if (current_page == UI_PAGE_WIFI && wifi_mode == UI_WIFI_MODE_FOX_HUNT) {", "void"),
]:
    offset = ui.index(signature)
    routes += f"static void {name}({args}){{\n" + block(ui, needle, offset) + "\n}\n"
at = ui.index("        wifi_fox_hunt_tick((uint64_t)now_ms,")
routes += "static void route_tick(uint64_t now_ms,bool screen_asleep){\n" + ui[at:ui.index(";", at)+1] + "\n}\n"
at = ui.index("    if (current_page == UI_PAGE_WIFI && wifi_mode == UI_WIFI_MODE_FOX_HUNT) return wifi_fox_hunt_draw();")
routes += "static esp_err_t route_draw(void){\n" + ui[at:ui.index("\n", at)] + "\nreturn ESP_OK;}\n"
(out / "fox_routes.h").write_text(routes)
pine = (root / "s3_main/main/wifi_pinescan_ui.h").read_text()
(out / "fox_label.h").write_text(block(pine, "static size_t pinescan_escape("))
assert '#include "wifi_fox_hunt_ui.h"' in ui
assert "channel_analyzer_prepare_sleep() && wifi_fox_hunt_prepare_sleep()) {" in ui
# The dedicated touch branch must precede generic footer and learning guards.
tap = ui[ui.index("static void handle_tap(uint16_t x, uint16_t y)\n{"):]
assert tap.index("wifi_fox_hunt_tap(x, y)") < tap.index("if (learning_tool_active")
live = (root / "s3_main/main/sentinel_live_ui.h").read_text()
assert "|| channel_analyzer_ui_busy() || wifi_fox_hunt_ui_busy()" in live
assert "&& !channel_analyzer_ui_busy() && !wifi_fox_hunt_ui_busy()" in live

env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-global"), ZIG_LOCAL_CACHE_DIR=str(out / "zig-local"))
exe = out / ("fox-ui.exe" if os.name == "nt" else "fox-ui")
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
    "-I", str(out), "-I", str(root / "tools/host_checks/stubs"), "-I", str(root / "s3_main/main"), "-I", str(root / "shared"),
    str(root / "tools/host_checks/wifi_fox_hunt_ui_check.c"), str(root / "s3_main/main/wifi_fox_hunt_scan.c"),
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
