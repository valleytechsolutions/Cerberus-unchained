"""Render Wardriving and exercise actual Start/Back/release-gated tick routes.

Service, storage-owner and BLE-host calls are counted boundaries. The real
Recon takeover predicate and ordinary discovery release model are compiled.
"""
from pathlib import Path
import os
import struct
import subprocess
import sys
import zlib

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/wardrive-ui-checks"
out.mkdir(parents=True, exist_ok=True)
ui = (root / "s3_main/main/ui.c").read_text(encoding="utf-8")

def between(source, first, after):
    begin = source.index(first)
    return source[begin:source.index(after, begin)]

parts = between(ui, "static const cerberus_ui_widget_theme_t ui_colors =", "static esp_err_t draw_detail_page(void);")
parts += between(ui, "static uint16_t text_width(", "static void refresh_capability_map(")
parts += between(ui, "static const char *wardrive_state_label(", "static const char *ble_attack_mode_label(")
parts += between(ui, "static esp_err_t draw_wardrive(", "static esp_err_t draw_detail_page(void)\n{")
parts += between(ui, "static void wardrive_handle_tap(", "static void wardrive_tick(")
parts += between(ui, "static bool recon_sessions_idle_for_takeover(void)\n{", "static void ble_sound_handle_tap(")
gps_ui = (root / "s3_main/main/gps_settings_ui.h").read_text(encoding="utf-8")
parts += gps_ui[gps_ui.index("static void gps_preview_poll("):]
parts += between(ui, "static void wardrive_tick(", "static void handle_tap(uint16_t x, uint16_t y)\n{")
tap = ui[ui.index("static void handle_tap(uint16_t x, uint16_t y)\n{"):]
back = tap.index("} else if (current_page == UI_PAGE_WARDRIVE) {")
back = tap.index("\n", back) + 1
back_end = tap.index("            } else if (current_page == UI_PAGE_DASHBOARD", back)
parts += "\nstatic void wardrive_back(void)\n{\n" + tap[back:back_end] + "\n}\n"
(out / "wardrive_ui_impl.h").write_text(parts)
display = (root / "s3_main/main/display.c").read_text()
begin = display.index("static const uint8_t *font5x7(")
(out / "display_font.h").write_text(display[begin:display.index("\nesp_err_t ", begin)])

env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-cache"))
exe = out / "wardrive-ui-check.exe"
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
    "-I", str(out), "-I", str(root / "tools/host_checks/stubs"),
    "-I", str(root / "s3_main/main"), "-I", str(root / "shared"),
    str(root / "tools/host_checks/wardrive_ui_check.c"),
    str(root / "s3_main/main/s3_ble_discovery_session.c"),
    str(root / "s3_main/main/ui_widgets.c"), "-o", str(exe)], check=True, env=env)
subprocess.run([str(exe), str(out / "wardrive")], check=True, env=env)

areas = out / "wardrive-areas-ui-check.exe"
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
    "-I", str(out), "-I", str(root / "tools/host_checks/stubs"),
    "-I", str(root / "s3_main/main"), "-I", str(root / "shared"),
    str(root / "tools/host_checks/wardrive_geofence_ui_check.c"),
    str(root / "s3_main/main/s3_ble_discovery_session.c"), str(root / "s3_main/main/ui_widgets.c"),
    str(root / "s3_main/main/keyboard.c"), str(root / "s3_main/main/wardrive_geofence.c"),
    "-lm", "-o", str(areas)], check=True, env=env)
subprocess.run([str(areas), str(out / "wardrive-areas")], check=True, env=env)
assert "if (wardrive_areas_is_open()) return wardrive_areas_draw();" in ui
assert "if (wardrive_areas_is_open()) {wardrive_areas_handle_tap(x,y);return;}" in ui

def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

for path in out.glob("wardrive-*.ppm"):
    header = b"P6\n320 480\n255\n"
    raw = path.read_bytes()
    assert raw.startswith(header) and len(raw) == len(header) + 320 * 480 * 3
    pixels = raw[len(header):]
    path.with_suffix(".png").write_bytes(b"\x89PNG\r\n\x1a\n" +
        chunk(b"IHDR", struct.pack(">IIBBBBB", 320, 480, 8, 2, 0, 0, 0)) +
        chunk(b"IDAT", zlib.compress(b"".join(b"\0" + pixels[y*960:(y+1)*960] for y in range(480)))) +
        chunk(b"IEND", b""))
print(out)
