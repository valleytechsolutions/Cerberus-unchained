"""Exercise actual cached-baseline guards, body/footer, report and local controls.

SPI APIs are not linked. Only the baseline-specific tap branch and the two
applicable footer dispatch branches are extracted; unrelated pages are outside
this fixture. Actual live scheduler/ownership is checked by scan_display_store.
"""
from pathlib import Path
import os
import struct
import subprocess
import sys
import zlib

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/wifi-baseline-ui-checks"
out.mkdir(parents=True, exist_ok=True)
ui = (root / "s3_main/main/ui.c").read_text()
live = (root / "s3_main/main/sentinel_live_ui.h").read_text()
settings = (root / "s3_main/main/settings.c").read_text()


def block(source, start):
    opening = source.index("{", start)
    depth = 0
    for end in range(opening, len(source)):
        depth += (source[end] == "{") - (source[end] == "}")
        if not depth:
            return source[start:end + 1]
    raise ValueError(source[start:start + 80])


def function(source, signature):
    return block(source, source.index(signature + "\n{"))


def declaration(source, name):
    end = source.index("} " + name + ";") + len("} " + name + ";")
    return source[source.rindex("typedef struct", 0, end):end]


(out / "baseline_live_record.h").write_text(declaration(live, "live_scan_record_t") + "\n" +
    declaration(live, "live_wifi_paint_map_t"))
(out / "baseline_settings.h").write_text("\n".join([
    declaration(settings, "wifi_trusted_entry_t"), declaration(settings, "wifi_trusted_record_t"),
    "static bool settings_ready; static wifi_trusted_record_t wifi_trusted;",
    function(settings, "size_t cerberus_settings_wifi_trusted_count(void)"),
    function(settings, "cerberus_wifi_trust_state_t cerberus_settings_wifi_trust_state(\n"
        "    const char *ssid,\n    const uint8_t bssid[6],\n    uint8_t security)"),
]))
parts = [function(live, signature) for signature in (
    "static bool live_wifi_baseline_current(void)",
    "static esp_err_t live_compare_wifi_baseline(void)",
    "static void live_reset_observations(void)")]
parts += [function(ui, signature) for signature in (
    "static const char *wifi_scan_coverage_text(uint8_t band)",
    "static uint16_t text_width(const char *text, uint8_t scale)",
    "static esp_err_t draw_centered(uint16_t y, const char *text, uint16_t color, uint8_t scale)",
    "static esp_err_t draw_small_button(uint16_t x, uint16_t y, uint16_t width, uint16_t height, const char *label)",
    "static esp_err_t draw_button(uint16_t x, uint16_t y, uint16_t width, uint16_t height, const char *label)",
    "static esp_err_t append_wifi_report(size_t *length, const char *format, ...)",
    "static esp_err_t refresh_wifi_baseline(void)",
    "static esp_err_t draw_baseline_metric(uint16_t x, uint16_t y, const char *label,\n"
        "    unsigned value, uint16_t color)",
    "static esp_err_t export_wifi_baseline(void)",
    "static esp_err_t draw_wifi_baseline(void)")]
parent = function(ui, "static esp_err_t draw_detail_page(void)")
start = parent.index("    if (learning_tool_active) {", parent.index("    if (live_is_view()"))
parts.append('static esp_err_t actual_footer(void)\n{ const char *back_label = "WIFI MENU";\n' + parent[start:])
tap = function(ui, "static void handle_wifi_tap(uint16_t x, uint16_t y)")
parts.append("static void actual_body_tap(uint16_t x, uint16_t y)\n{\n" +
    block(tap, tap.index("if (wifi_mode == UI_WIFI_MODE_BASELINE) {")) + "\n}")
tap = function(ui, "static void handle_tap(uint16_t x, uint16_t y)")
pause = block(tap, tap.index("if (!learning_tool_active && !live_is_view()"))
back = block(tap, tap.index("if (current_page == UI_PAGE_WIFI && wifi_mode != UI_WIFI_MODE_MENU)"))
parts.append("static void actual_footer_tap(uint16_t x, uint16_t y)\n{\n"
    "if (current_page != UI_PAGE_TOOLS && y >= UI_BACK_Y) {\n" + pause + " else " + back + "\n}\n}")
(out / "baseline_ui_impl.h").write_text("\n\n".join(parts))
display = (root / "s3_main/main/display.c").read_text()
start = display.index("static const uint8_t *font5x7(")
(out / "display_font.h").write_text(display[start:display.index("\nesp_err_t ", start)])
framebuffer = (root / "tools/host_checks/recon_render.c").read_text().split(
    "/* Exercise the real private controls", 1)[0]
framebuffer = framebuffer.replace("if(!*text)return ESP_OK;", "if(!*text)return ESP_OK; baseline_text(y,text);")
(out / "baseline_framebuffer.h").write_text(framebuffer)
exe = out / ("wifi-baseline-ui-check.exe" if os.name == "nt" else "wifi-baseline-ui-check")
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-cache"))
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1",
    "-Wall", "-Wextra", "-Werror", "-I", str(out), "-I", str(root / "tools/host_checks/stubs"),
    "-I", str(root / "shared"), "-I", str(root / "s3_main/main"),
    str(root / "tools/host_checks/wifi_baseline_ui_check.c"),
    str(root / "s3_main/main/sentinel_baseline.c"), str(root / "s3_main/main/ui_widgets.c"),
    "-o", str(exe)], check=True, env=env)
subprocess.run([str(exe), str(out / "baseline")], check=True, env=env)


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


for ppm in out.glob("baseline-*.ppm"):
    raw = ppm.read_bytes()
    header = b"P6\n320 480\n255\n"
    assert raw.startswith(header) and len(raw) == len(header) + 320 * 480 * 3
    pixels = raw[len(header):]
    ppm.with_suffix(".png").write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(
        ">IIBBBBB", 320, 480, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(b"".join(
        b"\0" + pixels[y * 960:(y + 1) * 960] for y in range(480)))) + chunk(b"IEND", b""))
print(out)
