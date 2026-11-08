"""Production Wi-Fi save/view controller, renderer, and extracted Files routes.

Private generated headers/caches; no shared fixture outputs. The counted file
worker uses the real codec/capture round trip; its actual IO/storage lifetime
and native coordinator capture have separate production-bound suites.
"""
from pathlib import Path
import os
import struct
import subprocess
import sys
import zlib

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/wifi-archive-ui-checks"
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
(out / "wifi_framebuffer.h").write_text(frame, encoding="utf-8")
scanner = (root / "s3_main/main/s3_wifi_scanner.c").read_text(encoding="utf-8")
start = scanner.index("size_t cerberus_s3_wifi_scanner_ssid_display(")
(out / "ssid_display_impl.h").write_text(scanner[start:], encoding="utf-8")
ui = (root / "s3_main/main/ui.c").read_text(encoding="utf-8")

def block(needle, source=ui):
    begin = source.index(needle)
    opening = source.index("{", begin)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[begin:end]

files = block("static void handle_files_tap(uint16_t x, uint16_t y)\n{")
begin = files.index("if (wifi_archive_is_file(file_entries[index].name))")
end = files.index("\n        }\n        return;", begin)
(out / "wifi_file_route.h").write_text("static void open_selected_file(size_t index) {\n" + files[begin:end] + "\n}\n", encoding="utf-8")
draw_line = "if (current_page == UI_PAGE_WIFI_ARCHIVE) return wifi_archive_draw();"
assert draw_line in block("static esp_err_t draw_detail_page(void)\n{")
tap_branch = block("if (current_page == UI_PAGE_WIFI_ARCHIVE) {")
(out / "wifi_page_routes.h").write_text("static esp_err_t page_draw(void) {\n" + draw_line +
    "\nreturn ESP_OK;\n}\nstatic void page_tap(uint16_t x,uint16_t y) {\n" + tap_branch + "\n}\n", encoding="utf-8")
assert ui.index('#include "wifi_archive_live_ui.h"') > ui.index('#include "dual_wifi_live_ui.h"')
assert "wifi_archive_tick(screen_asleep);" in ui
assert "case DUAL_WIFI_UI_SAVE_ALL: wifi_archive_save_live(false); return;" in (root / "s3_main/main/dual_wifi_live_ui.h").read_text(encoding="utf-8")
assert "case DUAL_WIFI_UI_SAVE_SELECTED: wifi_archive_save_live(true); return;" in (root / "s3_main/main/dual_wifi_live_ui.h").read_text(encoding="utf-8")
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-global"), ZIG_LOCAL_CACHE_DIR=str(out / "zig-local"))
exe = out / ("wifi-archive-ui-check.exe" if os.name == "nt" else "wifi-archive-ui-check")
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
    "-I", str(out), "-I", str(root / "tools/host_checks/stubs"), "-I", str(root / "s3_main/main"),
    "-I", str(root / "shared"), str(root / "tools/host_checks/wifi_archive_ui_check.c"),
    str(root / "s3_main/main/wifi_archive.c"), str(root / "s3_main/main/wifi_archive_capture.c"),
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
