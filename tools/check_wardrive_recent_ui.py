"""Exercise the actual Wardriving Recent Networks viewer and framebuffer I/O.

No scanner, radio, SD, worker, clock or allocator is linked. Coherent snapshot
input is a caller boundary; service/model tests validate its production feed.
Rendered previews and assertions establish host UI behavior, not hardware.
"""
from pathlib import Path
import os
import struct
import subprocess
import sys
import zlib

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/wardrive-recent-ui-checks"
out.mkdir(parents=True, exist_ok=True)
display = (root / "s3_main/main/display.c").read_text(encoding="utf-8")
start = display.index("static const uint8_t *font5x7(")
end = display.index("\nesp_err_t ", start)
(out / "display_font.h").write_text(display[start:end], encoding="utf-8")
frame = (root / "tools/host_checks/recon_render.c").read_text(encoding="utf-8").split(
    "/* Exercise the real private controls", 1)[0]
frame = frame.replace("static unsigned calls;", "static unsigned calls, fail_after;\nstatic char drawn[65536];\nstatic size_t drawn_size;")
frame = frame.replace("    bounds(x,y,w,h);", "    if (fail_after && calls + 1U == fail_after) { ++calls; return ESP_FAIL; }\n    bounds(x,y,w,h);")
frame = frame.replace("    if(!*text)return ESP_OK;", "    if (fail_after && calls + 1U == fail_after) { ++calls; return ESP_FAIL; }\n"
    "    drawn_size += (size_t)snprintf(drawn + drawn_size, sizeof(drawn) - drawn_size, \"%s\\n\", text);\n"
    "    assert(drawn_size < sizeof(drawn));\n    if(!*text)return ESP_OK;")
(out / "wardrive_recent_framebuffer.h").write_text(frame, encoding="utf-8")
exe = out / ("wardrive-recent-ui-check.exe" if os.name == "nt" else "wardrive-recent-ui-check")
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-cache"),
           ZIG_LOCAL_CACHE_DIR=str(out / "zig-local-cache"))
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
    "-I", str(out), "-I", str(root / "tools/host_checks/stubs"), "-I", str(root / "s3_main/main"),
    "-I", str(root / "shared"), str(root / "tools/host_checks/wardrive_recent_ui_check.c"),
    str(root / "s3_main/main/wardrive_recent.c"), str(root / "s3_main/main/wardrive_core.c"),
    str(root / "s3_main/main/privacy.c"), "-o", str(exe)], check=True, env=env)
subprocess.run([str(exe), str(out)], check=True, env=env)

def chunk(kind, payload):
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))

for path in out.glob("*.ppm"):
    header = b"P6\n320 480\n255\n"
    raw = path.read_bytes()
    assert raw.startswith(header) and len(raw) == len(header) + 320 * 480 * 3
    pixels = raw[len(header):]
    rows = b"".join(b"\0" + pixels[y * 960:(y + 1) * 960] for y in range(480))
    path.with_suffix(".png").write_bytes(b"\x89PNG\r\n\x1a\n" +
        chunk(b"IHDR", struct.pack(">IIBBBBB", 320, 480, 8, 2, 0, 0, 0)) +
        chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))
