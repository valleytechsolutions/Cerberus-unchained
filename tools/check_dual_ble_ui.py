"""Render and exercise the actual read-only C5 companion UI and target binding."""
from pathlib import Path
import os
import subprocess
import sys
import struct
import zlib
root = Path(__file__).resolve().parents[1]
subprocess.run([sys.executable, str(root / "tools/check_s3_findmy_monitor_ui.py")], check=True)
out = root / "artifacts/dual-ble-ui-checks"
out.mkdir(parents=True, exist_ok=True)
(out / "esp_heap_caps.h").write_text("#pragma once\n")
ui = (root / "s3_main/main/ui.c").read_text()
scanner = (root / "s3_main/main/s3_ble_scanner.h").read_text()
end = scanner.index("} cerberus_s3_ble_track_state_t;") + len("} cerberus_s3_ble_track_state_t;")
begin = scanner.rfind("typedef struct", 0, end)
(out / "dual_ble_fox_state_type.h").write_text(scanner[begin:end])
begin = ui.index("static esp_err_t ble_fox_hunt_read_s3(cerberus_s3_ble_track_status_t *track,", ui.index("static void ble_fox_hunt_attach_s3(void)\n{"))
(out / "dual_ble_fox_read_impl.h").write_text(ui[begin:ui.index("static void ble_fox_hunt_observe_s3(", begin)])
begin = ui.index("static esp_err_t draw_ble_fox_hunt(void)")
(out / "dual_ble_fox_draw_impl.h").write_text(ui[begin:ui.index("static esp_err_t draw_ble_analyzer(void)", begin)])
live = (root / "s3_main/main/dual_ble_live_ui.h").read_text()
begin = live.index("static bool dual_ble_live_c5_retryable(void)")
(out / "dual_ble_retry_impl.h").write_text(live[begin:live.index("static bool dual_ble_live_discovery_released(void)", begin)])
exe = out / "check.exe"
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-cache"),
           ZIG_LOCAL_CACHE_DIR=str(out / "zig-local"))
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1", "-UNDEBUG",
    "-Wall", "-Wextra", "-Werror", "-I", str(out),
    "-I", str(root / "artifacts/findmy-monitor-ui-checks"),
    "-I", str(root / "tools/host_checks/stubs"), "-I", str(root / "s3_main/main"), "-I", str(root / "shared"),
    str(root / "tools/host_checks/dual_ble_ui_check.c"), str(root / "s3_main/main/dual_ble_scan.c"),
    str(root / "s3_main/main/privacy.c"), "-o", str(exe)], check=True, env=env)
subprocess.run([str(exe), str(out / "companion")], check=True)
def png_chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
for path in sorted(out.glob("companion-*.ppm")):
    data = path.read_bytes()
    header = b"P6\n320 480\n255\n"
    assert data.startswith(header) and len(data) == len(header) + 320 * 480 * 3
    pixels = data[len(header):]
    path.with_suffix(".png").write_bytes(b"\x89PNG\r\n\x1a\n" +
        png_chunk(b"IHDR", struct.pack(">IIBBBBB", 320, 480, 8, 2, 0, 0, 0)) +
        png_chunk(b"IDAT", zlib.compress(b"".join(b"\0" + pixels[y*960:(y+1)*960] for y in range(480)))) +
        png_chunk(b"IEND", b""))
print(out)
