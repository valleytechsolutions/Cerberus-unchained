"""Exercise actual survey worker, SPI adapters, admission projection and UI cleanup."""
from pathlib import Path
import os
import re
import struct
import subprocess
import sys
import zlib

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/channel-survey-lifecycle-checks"
out.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-cache"),
           ZIG_LOCAL_CACHE_DIR=str(out / "zig-local-cache"))

def function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        if source[end] == "{": depth += 1
        if source[end] == "}": depth -= 1
        end += 1
    return source[start:end] + "\n"

wifi = (root / "c5_firmware/main/wifi_scanner.c").read_text()
ble = (root / "c5_firmware/main/ble_scanner.c").read_text()
native = function(wifi, "static bool another_radio_operation_is_active(")
native += function(wifi, "static bool wifi_connection_is_active_locked(")
native += function(wifi, "void cerberus_wifi_scanner_get_status(")
native += function(ble, "static bool another_radio_operation_is_active(").replace(
    "another_radio_operation_is_active", "ble_other_radio_active")
(out / "survey_admission.h").write_text(native)
dispatch = (root / "c5_firmware/main/main.c").read_text()
start = dispatch.index("    case CERBERUS_SPI_COMMAND_WIFI_CHANNEL_SURVEY_START:")
(out / "survey_dispatch.h").write_text(dispatch[start:dispatch.index(
    "    case CERBERUS_SPI_COMMAND_WIFI_PACKET_MONITOR_SESSION_INFO:", start)])
link = (root / "s3_main/main/c5_spi_link.c").read_text()
start = link.index("static esp_err_t wifi_channel_survey_status_command(")
(out / "survey_link.h").write_text(link[start:link.index(
    "static esp_err_t wifi_packet_monitor_status_command(", start)])
ui = (root / "s3_main/main/ui.c").read_text()
start = ui.index("static cerberus_wifi_channel_survey_status_payload_t wifi_channel_survey_status;")
globals_text = "\n".join(re.search(r"^#define " + name + r" .+$", ui, re.M).group(0)
    for name in ("UI_WIFI_VISIBLE_ROWS", "UI_WIFI_SURVEY_METRICS_MS", "UI_WIFI_SURVEY_RETRY_MS")) + "\n"
globals_text += ui[start:ui.index("typedef enum {\n    PACKET_MONITOR_NONE", start)]
start = ui.index("static bool wifi_channel_survey_matches_claim(")
helpers = ui[start:ui.index('#include "wifi_packet_monitor_ui.h"', start)]
(out / "survey_ui.h").write_text(globals_text + function(ui,
    "static uint8_t wifi_channel_survey_band(") + helpers)
(out / "survey_ui_draw.h").write_text(function(ui, "static esp_err_t draw_wifi_channel_survey_live("))
layout = "\n".join(re.search(r"^#define " + name + r" .+$", ui, re.M).group(0)
                   for name in ("UI_WIFI_LIST_Y", "UI_WIFI_ROW_HEIGHT", "UI_WIFI_CONTROLS_Y")) + "\n"
start = ui.index("static const cerberus_ui_widget_theme_t ui_colors =")
layout += ui[start:ui.index("static esp_err_t draw_detail_page(void);", start)]
for name in ("static uint16_t text_width(", "static esp_err_t draw_centered(",
             "static esp_err_t draw_header(", "static esp_err_t draw_small_button("):
    layout += function(ui, name)
(out / "survey_ui_layout.h").write_text(layout)
display = (root / "s3_main/main/display.c").read_text()
start = display.index("static const uint8_t *font5x7(")
(out / "display_font.h").write_text(display[start:display.index("\nesp_err_t ", start)])
start = ui.index("        bool channels_visible = current_page == UI_PAGE_WIFI &&")
(out / "survey_ui_tick.h").write_text(ui[start:ui.index("        if(station_visible", start)])
start = ui.index("                if (wifi_channel_survey_stop_ui()) wifi_mode = UI_WIFI_MODE_MENU;")
end = ui.index("            } else if", start)
(out / "survey_ui_back.h").write_text(ui[start:end])

exe = out / "channel-survey-lifecycle.exe"
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1",
    "-Wall", "-Wextra", "-Werror", "-I", str(out), "-I", str(root / "shared"),
    "-I", str(root / "tools/host_checks/stubs"), "-I", str(root / "c5_firmware/main"),
    "-I", str(root / "s3_main/main"),
    str(root / "tools/host_checks/channel_survey_lifecycle_check.c"),
    str(root / "s3_main/main/ui_widgets.c"),
    "-o", str(exe)], check=True, env=env)
subprocess.run([str(exe), str(out / "survey")], check=True, env=env)

def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

for path in out.glob("survey-*.ppm"):
    header = b"P6\n320 480\n255\n"
    raw = path.read_bytes()
    assert raw.startswith(header) and len(raw) == len(header) + 320 * 480 * 3
    pixels = raw[len(header):]
    path.with_suffix(".png").write_bytes(b"\x89PNG\r\n\x1a\n" +
        chunk(b"IHDR", struct.pack(">IIBBBBB", 320, 480, 8, 2, 0, 0, 0)) +
        chunk(b"IDAT", zlib.compress(b"".join(b"\0" + pixels[y*960:(y+1)*960] for y in range(480)))) +
        chunk(b"IEND", b""))

# Keep the native scoped-lease interleaving fixture under this registered suite.
# Its generated files and compiler caches are separate from the UI bridge.
subprocess.run([sys.executable, str(root / "tools/check_channel_survey_session.py")], check=True)
