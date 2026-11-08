"""Actual Packet Count UI/controller, frozen input routes and real-font previews."""
from pathlib import Path
import os
import re
import struct
import subprocess
import sys
import zlib

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/wifi-packet-count-ui-checks"
out.mkdir(parents=True, exist_ok=True)
(out / "freertos").mkdir(exist_ok=True)
(out / "freertos/FreeRTOS.h").write_text("""#pragma once
#include <stdatomic.h>
typedef struct { atomic_flag flag; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED { ATOMIC_FLAG_INIT }
#define portENTER_CRITICAL(m) do { while(atomic_flag_test_and_set_explicit(&(m)->flag,memory_order_acquire)) {} } while(0)
#define portEXIT_CRITICAL(m) atomic_flag_clear_explicit(&(m)->flag,memory_order_release)
""")
display = (root / "s3_main/main/display.c").read_text()
start = display.index("static const uint8_t *font5x7(")
(out / "display_font.h").write_text(display[start:display.index("\nesp_err_t ", start)])
frame = (root / "tools/host_checks/recon_render.c").read_text().split("/* Exercise the real private controls", 1)[0]
frame = frame.replace("static unsigned calls;", "static unsigned calls,fail_after;\nstatic char drawn[65536];\nstatic size_t drawn_size;")
frame = frame.replace("    bounds(x,y,w,h);", "    if(fail_after&&calls+1U==fail_after){++calls;return ESP_FAIL;}\n    bounds(x,y,w,h);")
frame = frame.replace("    if(!*text)return ESP_OK;", "    if(fail_after&&calls+1U==fail_after){++calls;return ESP_FAIL;}\n"
    "    drawn_size+=(size_t)snprintf(drawn+drawn_size,sizeof(drawn)-drawn_size,\"%s\\n\",text);assert(drawn_size<sizeof(drawn));\n    if(!*text)return ESP_OK;")
(out / "count_framebuffer.h").write_text(frame)
ui = (root / "s3_main/main/ui.c").read_text()
modes = re.search(r"typedef enum \{\s*UI_WIFI_MODE_MENU,.*?\} ui_wifi_mode_t;", ui, re.S).group(0)
live = (root / "s3_main/main/dual_wifi_live_ui.h").read_text()
begin = live.index("    case DUAL_WIFI_UI_OPEN_PACKET_COUNT: {")
ap = live[begin:live.index("    case DUAL_WIFI_UI_OPEN_AP_TOOLS:", begin)]
relation = (root / "s3_main/main/wifi_relation_ui.h").read_text()
begin = relation.index("    if (s->detail && s->selected.valid", relation.index("static void wifi_relation_tap"))
station = relation[begin:relation.index("    } else if (!s->detail", begin)] + "    }"
(out / "count_modes.h").write_text(modes)
(out / "count_routes.h").write_text("static void route_ap(void){switch(1){\n" + ap.replace("DUAL_WIFI_UI_OPEN_PACKET_COUNT", "1") + "default:break;}}\n"
    + "static void route_station(uint16_t x,uint16_t y){wifi_relation_ui_store_t *s=&relation_store;\n" + station + "}\n")
assert '#include "wifi_packet_count_ui.h"' in ui
assert 'UI_WIFI_MODE_PACKET_COUNT) return wifi_packet_count_draw();' in ui
assert 'wifi_packet_count_tick((uint64_t)now_ms, current_page == UI_PAGE_WIFI &&' in ui
assert 'wifi_packet_count_tap(x, y); ESP_ERROR_CHECK(draw_detail_page()); return;' in ui
live_poll = (root / "s3_main/main/sentinel_live_ui.h").read_text()
assert '|| mesh_ui_busy() || wifi_packet_count_ui_busy()' in live_poll
assert '&& !mesh_ui_busy() && !wifi_packet_count_ui_busy()' in live_poll
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-global"), ZIG_LOCAL_CACHE_DIR=str(out / "zig-local"))
exe = out / ("count-ui.exe" if os.name == "nt" else "count-ui")
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-O1", "-Wall", "-Wextra", "-Werror",
    "-I", str(out), "-I", str(root / "tools/host_checks/stubs"), "-I", str(root / "s3_main/main"), "-I", str(root / "shared"),
    str(root / "tools/host_checks/wifi_packet_count_ui_check.c"), str(root / "s3_main/main/wifi_packet_count_scan.c"),
    str(root / "s3_main/main/wifi_packet_count_diagnostics.c"),
    str(root / "shared/wifi_packet_count.c"), str(root / "s3_main/main/ui_widgets.c"), "-o", str(exe)], check=True, env=env)
subprocess.run([str(exe), str(out)], check=True, env=env)

def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

for path in out.glob("*.ppm"):
    data = path.read_bytes()
    header = b"P6\n320 480\n255\n"
    assert data.startswith(header) and len(data) == len(header) + 320 * 480 * 3
    pixels = data[len(header):]
    path.with_suffix(".png").write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 320, 480, 8, 2, 0, 0, 0)) +
        chunk(b"IDAT", zlib.compress(b"".join(b"\0" + pixels[y*960:(y+1)*960] for y in range(480)))) + chunk(b"IEND", b""))
