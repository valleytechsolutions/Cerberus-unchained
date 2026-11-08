"""Render and exercise the production SSID workspace, keyboard and Tools menu.

The native session boundary is counted and uses the real list model. Native
worker ownership and SD writes are tested separately by its session suite.
Artifacts and compiler caches are private, so this check may run in parallel.
"""
from pathlib import Path
import os
import struct
import subprocess
import sys
import zlib

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/ssid-list-ui-checks"
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
(out / "ssid_framebuffer.h").write_text(frame, encoding="utf-8")
(out / "esp_random.h").write_text("#pragma once\n#include <stddef.h>\nvoid esp_fill_random(void *, size_t);\n", encoding="utf-8")
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-global"), ZIG_LOCAL_CACHE_DIR=str(out / "zig-local"))
exe = out / ("ssid-list-ui-check.exe" if os.name == "nt" else "ssid-list-ui-check")
sources = ["tools/host_checks/ssid_list_ui_check.c", "s3_main/main/ssid_list.c",
    "s3_main/main/wifi_marauder.c", "s3_main/main/wifi_archive.c", "s3_main/main/keyboard.c",
    "s3_main/main/sentinel_tools.c", "s3_main/main/ui_widgets.c"]
posix_flags = [] if os.name == "nt" else ["-D_POSIX_C_SOURCE=200809L"]
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", *posix_flags, "-O1", "-Wall", "-Wextra", "-Werror",
    "-I", str(out), "-I", str(root / "tools/host_checks/stubs"), "-I", str(root / "s3_main/main"),
    "-I", str(root / "shared"), *[str(root / source) for source in sources], "-o", str(exe)], check=True, env=env)
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
