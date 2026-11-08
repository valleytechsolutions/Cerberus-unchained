"""Actual Wardriving GPS observer renderer and UI routes; no UART/device access.

The UART trace and service APIs are counted value-only boundaries. Parser and
UART producer/lifetime coverage belongs to their separate production suites.
This runner uses its own generated includes/cache and can run in parallel.
"""
from pathlib import Path
import os
import struct
import subprocess
import sys
import zlib

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/wardrive-gps-ui-checks"
out.mkdir(parents=True, exist_ok=True)
ui = (root / "s3_main/main/ui.c").read_text(encoding="utf-8")

def between(source, first, after):
    start = source.index(first)
    return source[start:source.index(after, start)]

common = between(ui, "static const cerberus_ui_widget_theme_t ui_colors =", "static esp_err_t draw_detail_page(void);")
common += between(ui, "static uint16_t text_width(", "static void refresh_capability_map(")
common += between(ui, "static const char *wardrive_state_label(", "static const char *ble_attack_mode_label(")
(out / "wardrive_gps_common.h").write_text(common)
routes = between(ui, "static esp_err_t draw_wardrive(", "static esp_err_t draw_detail_page(void)\n{")
routes += between(ui, "static void wardrive_handle_tap(", "static void handle_tap(uint16_t x, uint16_t y)\n{")
tap = ui[ui.index("static void handle_tap(uint16_t x, uint16_t y)\n{"):]
begin = tap.index("    if (wardrive_gps_view_is_open()) {")
end = tap.index("    if (wardrive_recent_is_open())", begin)
routes += "\nstatic void actual_tap(uint16_t x,uint16_t y)\n{\n" + tap[begin:end] + "wardrive_handle_tap(x,y);\n}\n"
entry = ui[ui.index("static void open_tool_page(ui_page_t page)\n{"):]
begin = entry.index("    if (current_page == UI_PAGE_WARDRIVE) {")
end = entry.index("    if (current_page == UI_PAGE_IEEE802154)", begin)
routes += "\nstatic void actual_open_wardrive(void)\n{\n" + entry[begin:end] + "}\n"
begin = tap.index("} else if (current_page == UI_PAGE_WARDRIVE) {")
begin = tap.index("\n", begin) + 1
end = tap.index("            } else if (current_page == UI_PAGE_DASHBOARD", begin)
routes += "\nstatic void actual_summary_back(void)\n{\n" + tap[begin:end] + "}\n"
(out / "wardrive_gps_routes.h").write_text(routes)
assert "if (wardrive_gps_view_is_open()) return wardrive_gps_view_draw();" in ui
assert ui.index('#include "wardrive_recent_live_ui.h"') < ui.index('#include "wardrive_gps_live_ui.h"')
display = (root / "s3_main/main/display.c").read_text()
begin = display.index("static const uint8_t *font5x7(")
(out / "display_font.h").write_text(display[begin:display.index("\nesp_err_t ", begin)])
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-cache"))
exe = out / "wardrive-gps-ui-check.exe"
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
    "-I", str(out), "-I", str(root / "tools/host_checks/stubs"),
    "-I", str(root / "s3_main/main"), "-I", str(root / "shared"),
    str(root / "tools/host_checks/wardrive_gps_ui_check.c"),
    str(root / "s3_main/main/ui_widgets.c"), "-o", str(exe)], check=True, env=env)
subprocess.run([str(exe), str(out / "gps")], check=True, env=env)

def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

for path in out.glob("gps-*.ppm"):
    header = b"P6\n320 480\n255\n"
    raw = path.read_bytes()
    assert raw.startswith(header) and len(raw) == len(header) + 320 * 480 * 3
    pixels = raw[len(header):]
    path.with_suffix(".png").write_bytes(b"\x89PNG\r\n\x1a\n" +
        chunk(b"IHDR", struct.pack(">IIBBBBB", 320, 480, 8, 2, 0, 0, 0)) +
        chunk(b"IDAT", zlib.compress(b"".join(b"\0" + pixels[y*960:(y+1)*960] for y in range(480)))) +
        chunk(b"IEND", b""))
print(out)
