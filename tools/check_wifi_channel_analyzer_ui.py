"""Compile the real Analyzer controller, menu/routes, model and font renderer."""
from pathlib import Path
import os
import re
import struct
import subprocess
import sys
import zlib

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/wifi-channel-analyzer-ui-checks"
out.mkdir(parents=True, exist_ok=True)
display = (root / "s3_main/main/display.c").read_text()
start = display.index("static const uint8_t *font5x7(")
(out / "display_font.h").write_text(display[start:display.index("\nesp_err_t ", start)])
frame = (root / "tools/host_checks/recon_render.c").read_text().split("/* Exercise the real private controls", 1)[0]
frame = frame.replace("static unsigned calls;", "static unsigned calls,fail_after;\nstatic char drawn[65536];\nstatic size_t drawn_size;")
frame = frame.replace("    bounds(x,y,w,h);", "    if(fail_after&&calls+1U==fail_after){++calls;return ESP_FAIL;}\n    bounds(x,y,w,h);")
frame = frame.replace("    if(!*text)return ESP_OK;", "    if(fail_after&&calls+1U==fail_after){++calls;return ESP_FAIL;}\n"
    "    drawn_size+=(size_t)snprintf(drawn+drawn_size,sizeof(drawn)-drawn_size,\"%s\\n\",text);assert(drawn_size<sizeof(drawn));\n    if(!*text)return ESP_OK;")
(out / "analyzer_framebuffer.h").write_text(frame)
ui = (root / "s3_main/main/ui.c").read_text()
modes = re.search(r"typedef enum \{\s*UI_WIFI_MODE_MENU,.*?\} ui_wifi_mode_t;", ui, re.S).group(0)
constants = "\n".join(re.search(rf"^#define {name} .+$", ui, re.M).group(0)
    for name in ["UI_WIFI_CONTROLS_Y", "UI_BACK_Y", "UI_BACK_HEIGHT"])
(out / "analyzer_modes.h").write_text(modes + "\n" + constants)

def block(needle, offset=0):
    start = ui.index(needle, offset)
    opening = ui.index("{", start)
    cursor, depth = opening + 1, 1
    while depth:
        depth += (ui[cursor] == "{") - (ui[cursor] == "}")
        cursor += 1
    return ui[start:cursor]

tap = ui.index("static void handle_wifi_tap(uint16_t x, uint16_t y)\n{")
route = "static void route_menu(uint16_t x,uint16_t y){switch(sentinel_hub_hit_test(SENTINEL_HUB_WIFI,x,y)){\n"
for item in (11, 0):
    start = ui.index(f"case {item}:", tap)
    route += ui[start:ui.index("break;", start) + 6] + "\n"
route += "default:break;}}\n"
route += "static void route_tap(uint16_t x,uint16_t y){\n" + block("if (wifi_mode == UI_WIFI_MODE_CHANNEL_ANALYZER) {", tap) + "\n}\n"
back_at = ui.index("static void handle_tap(uint16_t x, uint16_t y)\n{")
route += "static void route_back(uint16_t x,uint16_t y){\n" + block("if (current_page == UI_PAGE_WIFI && wifi_mode == UI_WIFI_MODE_CHANNEL_ANALYZER) {", back_at) + "\n}\n"
launcher_at = ui.index("static void return_to_launcher(void)\n{")
route += "static void route_launcher(void){\n" + block("if (current_page == UI_PAGE_WIFI && wifi_mode == UI_WIFI_MODE_CHANNEL_ANALYZER) {", launcher_at) + "\n}\n"
tick = ui.index("        bool channel_analyzer_visible =")
route += "static void route_tick(uint64_t now_ms,bool screen_asleep){\n" + ui[tick:ui.index("        ", ui.index("ESP_ERROR_CHECK(draw_detail_page());", tick) + len("ESP_ERROR_CHECK(draw_detail_page());"))] + "\n}\n"
draw = ui.index("    if (wifi_mode == UI_WIFI_MODE_CHANNEL_ANALYZER) return channel_analyzer_draw();")
route += "static esp_err_t route_draw(void){\n" + ui[draw:ui.index("\n", draw)] + "\nreturn ESP_OK;}\n"
footer = ui.index("    if (current_page == UI_PAGE_WIFI && wifi_mode == UI_WIFI_MODE_CHANNEL_ANALYZER)\n")
route += "static esp_err_t route_footer(void){\n" + ui[footer:ui.index(";", footer) + 1] + "\nreturn ESP_OK;}\n"
(out / "analyzer_routes.h").write_text(route)
assert '#include "wifi_channel_analyzer_ui.h"' in ui
assert "channel_analyzer_prepare_sleep() && wifi_fox_hunt_prepare_sleep()) {\n            awake_brightness" in ui
assert "if (learning_tool_active &&\n        !(current_page == UI_PAGE_WIFI && wifi_mode == UI_WIFI_MODE_CHANNEL_ANALYZER)" in ui
live = (root / "s3_main/main/sentinel_live_ui.h").read_text()
assert "|| wifi_packet_count_ui_busy() || channel_analyzer_ui_busy()" in live
assert "&& !wifi_packet_count_ui_busy() && !channel_analyzer_ui_busy()" in live
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-global"), ZIG_LOCAL_CACHE_DIR=str(out / "zig-local"))
exe = out / ("analyzer-ui.exe" if os.name == "nt" else "analyzer-ui")
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
    "-I", str(out), "-I", str(root / "tools/host_checks/stubs"), "-I", str(root / "s3_main/main"), "-I", str(root / "shared"),
    str(root / "tools/host_checks/wifi_channel_analyzer_ui_check.c"),
    str(root / "s3_main/main/wifi_channel_analyzer_scan.c"), str(root / "s3_main/main/sentinel_hub.c"),
    str(root / "s3_main/main/ui_widgets.c"), "-o", str(exe)], check=True, env=env)
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
