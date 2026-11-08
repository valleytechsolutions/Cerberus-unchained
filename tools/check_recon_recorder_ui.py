"""Exercise the actual Recon UI, validated-bank feed and recorder controls.

The entire private watch/control headers and real session/history/view code are
compiled. Radio getters, recorder queue boundaries, clock and vendor lookup are
captured host adapters; this suite performs no RF or SD activity. Framebuffer
primitives retain the real font and check all drawing bounds.
"""
from pathlib import Path
import os
import struct
import subprocess
import sys
import zlib

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/recon-recorder-ui-checks"
out.mkdir(parents=True, exist_ok=True)
display = (root / "s3_main/main/display.c").read_text(encoding="utf-8")
start = display.index("static const uint8_t *font5x7(")
end = display.index("\nesp_err_t ", start)
(out / "display_font.h").write_text(display[start:end], encoding="utf-8")
classifier = (root / "s3_main/main/sentinel_watch_detect.c").read_text(encoding="utf-8")
labels_start = classifier.index("const char *sentinel_watch_category_name(")
(out / "recon_category_labels.h").write_text(classifier[labels_start:], encoding="utf-8")
framebuffer = (root / "tools/host_checks/recon_render.c").read_text(encoding="utf-8").split(
    "/* Exercise the real private controls", 1)[0]
framebuffer = framebuffer.replace("static unsigned calls;",
    "static unsigned calls, painted_paused, painted_pausing, painted_resume, fail_paint_at;\n"
    "static char painted_text[65536];\nstatic size_t painted_text_size;")
framebuffer = framebuffer.replace("    bounds(x,y,w,h);",
    "    if(fail_paint_at && calls+1==fail_paint_at){++calls;return ESP_FAIL;}\n    bounds(x,y,w,h);")
framebuffer = framebuffer.replace("    if(!*text)return ESP_OK;",
    '    if(fail_paint_at && calls+1==fail_paint_at){++calls;return ESP_FAIL;}\n'
    '    size_t n=strlen(text);assert(painted_text_size+n+2<sizeof(painted_text));\n'
    '    memcpy(painted_text+painted_text_size,text,n);painted_text_size+=n;painted_text[painted_text_size++]=\'\\n\';painted_text[painted_text_size]=0;\n'
    '    if(y==33&&!strcmp(text,"PAUSED"))++painted_paused;\n'
    '    if(y==33&&!strcmp(text,"PAUSING"))++painted_pausing;\n'
    '    if(!strcmp(text,"RESUME"))++painted_resume;\n'
    "    if(!*text)return ESP_OK;")
(out / "recon_framebuffer.h").write_text(framebuffer, encoding="utf-8")
live = (root / "s3_main/main/sentinel_live_ui.h").read_text(encoding="utf-8")
end = live.index("} live_scan_record_t;") + len("} live_scan_record_t;")
(out / "recon_live_types.h").write_text(live[live.rindex("typedef struct", 0, end):end], encoding="utf-8")
owner = (root / "s3_main/main/recon_ble_live_ui.h").read_text(encoding="utf-8")
owner_start = owner.index("static uint64_t live_ble_lease_epoch")
owner_end = owner.index("static void live_ble_forget_lease(")
(out / "recon_capture_owner_impl.h").write_text(owner[owner_start:owner_end], encoding="utf-8")
ui = (root / "s3_main/main/ui.c").read_text(encoding="utf-8")
marker = "if (sleep_err == ESP_OK) {"
assert ui.count(marker) == 1
start = ui.rindex("awake_brightness = cerberus_display_get_brightness();", 0, ui.index(marker))
opening = ui.index("{", ui.index(marker))
depth = 1
end = opening + 1
while depth:
    depth += (ui[end] == "{") - (ui[end] == "}")
    end += 1
assert ui[end:end + 7] == " else {"
end += 7
depth = 1
while depth:
    depth += (ui[end] == "{") - (ui[end] == "}")
    end += 1
(out / "recon_sleep_impl.h").write_text(
    "static void recon_test_sleep_attempt(void)\n{\n" + ui[start:end] + "\n}\n", encoding="utf-8")
exe = out / ("recon-recorder-ui-check.exe" if os.name == "nt" else "recon-recorder-ui-check")
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-cache"))
# Expose POSIX strnlen used by the production UI under strict C11 on Linux.
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-O1",
    "-Wall", "-Wextra", "-Werror", "-I", str(out),
    "-I", str(root / "tools/host_checks/stubs"),
    "-I", str(root / "s3_main/main"), "-I", str(root / "shared"),
    str(root / "tools/host_checks/recon_recorder_ui_check.c"),
    str(root / "s3_main/main/sentinel_watch.c"),
    str(root / "s3_main/main/sentinel_watch_activity.c"),
    str(root / "s3_main/main/sentinel_watch_view.c"),
    str(root / "s3_main/main/sentinel_scan_session.c"),
    str(root / "s3_main/main/recon_journal.c"),
    str(root / "s3_main/main/recon_inventory.c"),
    str(root / "s3_main/main/s3_wifi_list.c"), "-o", str(exe)], check=True, env=env)
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
