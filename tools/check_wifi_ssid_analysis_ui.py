"""Exercise the actual selected-snapshot guard and native detail body renderer.

SPI APIs are deliberately not linked. Settings/vendor adapters remain separate
fixtures; this proves cache-only comparison, not zero SD work on detail entry.
"""
from pathlib import Path
import os
import struct
import subprocess
import sys
import zlib

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/wifi-ssid-detail-checks"
out.mkdir(parents=True, exist_ok=True)
ui = (root / "s3_main/main/ui.c").read_text()
live = (root / "s3_main/main/sentinel_live_ui.h").read_text()
relation = (root / "s3_main/main/wifi_relation_ui.h").read_text()

def function(source, signature):
    start = source.index(signature + "\n{")
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{": depth += 1
        elif source[index] == "}":
            depth -= 1
            if not depth: return source[start:index + 1]
    raise ValueError(signature)

start = live.index("typedef struct {")
end = live.index("} live_scan_record_t;", start) + len("} live_scan_record_t;")
(out / "ssid_live_record.h").write_text(live[start:end])
(out / "ssid_detail_impl.h").write_text("\n".join([
    function(relation, "static void wifi_relation_bind_selection(bool ordinary_publication)"),
    function(relation, "static bool wifi_relation_selection_bound(void)"),
    function(live, "static esp_err_t live_analyze_selected_wifi_ssid(void)"),
    function(ui, "static esp_err_t analyze_selected_wifi_ssid(void)"),
    function(ui, "static void make_ssid_display_safe(char *ssid)"),
    function(ui, "static const char *wifi_scan_coverage_text(uint8_t band)"),
    function(ui, "static uint8_t count_wifi_security_modes(uint16_t mask)"),
    function(ui, "static const char *wifi_security_text(uint8_t security)"),
    function(ui, "static uint16_t text_width(const char *text, uint8_t scale)"),
    function(ui, "static esp_err_t draw_centered(uint16_t y, const char *text, uint16_t color, uint8_t scale)"),
    function(ui, "static esp_err_t draw_small_button(uint16_t x, uint16_t y, uint16_t width, uint16_t height, const char *label)"),
    function(ui, "static esp_err_t draw_wifi_detail(void)"),
]))
display = (root / "s3_main/main/display.c").read_text()
start = display.index("static const uint8_t *font5x7(")
(out / "display_font.h").write_text(display[start:display.index("\nesp_err_t ", start)])
framebuffer = (root / "tools/host_checks/recon_render.c").read_text().split(
    "/* Exercise the real private controls", 1)[0]
framebuffer = framebuffer.replace("if(!*text)return ESP_OK;", "if(!*text)return ESP_OK; detail_text(y,text);")
(out / "ssid_framebuffer.h").write_text(framebuffer)
exe = out / "wifi-ssid-detail-check.exe"
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-cache"))
# Expose POSIX strnlen used by the production UI under strict C11 on Linux.
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-O1",
    "-Wall", "-Wextra", "-Werror", "-I", str(out), "-I", str(root / "tools/host_checks/stubs"),
    "-I", str(root / "shared"), "-I", str(root / "s3_main/main"),
    str(root / "tools/host_checks/wifi_ssid_detail_check.c"),
    str(root / "s3_main/main/s3_wifi_ssid_analysis.c"), str(root / "s3_main/main/ui_widgets.c"),
    "-o", str(exe)], check=True, env=env)
subprocess.run([str(exe), str(out / "detail")], check=True, env=env)

def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

for ppm in out.glob("detail-*.ppm"):
    raw = ppm.read_bytes()
    header = b"P6\n320 480\n255\n"
    assert raw.startswith(header) and len(raw) == len(header) + 320 * 480 * 3
    pixels = raw[len(header):]
    ppm.with_suffix(".png").write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(
        ">IIBBBBB", 320, 480, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(b"".join(
        b"\0" + pixels[y * 960:(y + 1) * 960] for y in range(480)))) + chunk(b"IEND", b""))
print(out)
