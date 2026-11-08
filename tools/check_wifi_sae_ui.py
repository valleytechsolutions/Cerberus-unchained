"""Render and navigate the production AP capture/SAE page and route hooks."""
from pathlib import Path
import os
import re
import struct
import subprocess
import sys
import zlib

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/wifi-sae-ui-checks"
out.mkdir(parents=True, exist_ok=True)
display = (root / "s3_main/main/display.c").read_text()
start = display.index("static const uint8_t *font5x7(")
(out / "display_font.h").write_text(display[start:display.index("\nesp_err_t ", start)])
frame = (root / "tools/host_checks/recon_render.c").read_text().split("/* Exercise the real private controls", 1)[0]
frame = frame.replace("static unsigned calls;", "static unsigned calls,fail_after;\nstatic char drawn[65536];\nstatic size_t drawn_size;")
frame = frame.replace("    bounds(x,y,w,h);", "    if(fail_after&&calls+1U==fail_after){++calls;return ESP_FAIL;}\n    bounds(x,y,w,h);")
frame = frame.replace("    if(!*text)return ESP_OK;", "    if(fail_after&&calls+1U==fail_after){++calls;return ESP_FAIL;}\n"
    "    drawn_size+=(size_t)snprintf(drawn+drawn_size,sizeof(drawn)-drawn_size,\"%s\\n\",text);assert(drawn_size<sizeof(drawn));\n    if(!*text)return ESP_OK;")
(out / "sae_framebuffer.h").write_text(frame)
(out / "esp_heap_caps.h").write_text("#pragma once\n#include <stddef.h>\n#define MALLOC_CAP_SPIRAM 4U\n#define MALLOC_CAP_8BIT 2U\nvoid *heap_caps_calloc(size_t,size_t,unsigned);\nvoid heap_caps_free(void*);\n")
ui = (root / "s3_main/main/ui.c").read_text()
modes = re.search(r"typedef enum \{\s*UI_WIFI_MODE_MENU,.*?\} ui_wifi_mode_t;", ui, re.S).group(0)
(out / "sae_modes.h").write_text(modes)

def block(needle, offset=0, source=ui):
    start = source.index(needle, offset)
    cursor = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[cursor] == "{") - (source[cursor] == "}")
        cursor += 1
    return source[start:cursor]

routes = ""
at = ui.index("static void return_to_launcher(void)\n{")
routes += "static void route_launcher(void){\n" + block("if (current_page == UI_PAGE_WIFI && wifi_mode == UI_WIFI_MODE_CAPTURE && sae_ui_is_open())", at) + "\n}\n"
at = ui.index("        bool capture_visible =")
end = ui.index("        bool pinescan_visible =", at)
routes += "static void route_tick(uint32_t now_ms,bool screen_asleep){\n" + ui[at:end] + "}\n"
at = ui.index("static void handle_wifi_tap(uint16_t x, uint16_t y)\n{")
routes += "static void route_tap(uint16_t x,uint16_t y){\n" + block("if (wifi_mode == UI_WIFI_MODE_AUTH)", at) + "\n" + block("if (wifi_mode == UI_WIFI_MODE_CAPTURE) {", at) + "\n}\n"
at = ui.index("if (capture_back())")
routes += "static void route_back(void){\n" + ui[at:ui.index("            } else if", at)] + "}\n"
live = (root / "s3_main/main/sentinel_live_ui.h").read_text()
entry = ui[ui.index("        case 12: wifi_mode = UI_WIFI_MODE_AUTH;"):].split("\n",1)[0]
routes += "static void route_menu(uint16_t x,uint16_t y){switch(sentinel_hub_hit_test(SENTINEL_HUB_WIFI,x,y)){\n" + entry + "\ndefault:break;}}\n"
at = ui.index("} else if (current_page == UI_PAGE_WIFI && wifi_mode == UI_WIFI_MODE_LIST && wifi_attack_selecting)")
routes += "static void route_picker_back(void){\n" + block("if (sae_ui_selecting)",at) + "\n}\n"
# Compile the actual selection-purpose branch and label expression.
label = live[live.index("live_view.tools_label=(sae_ui_selecting || eapol_ui_selecting)"):].split("\n",1)[0]
done_at = live.index("if(sae_ui_selecting)sae_ui_selection_done();")
done = live[done_at:live.index("\n", live.index("else if(eapol_ui_selecting)eapol_ui_selection_done();", done_at))]
routes += "static void route_picker_done(void){" + done + "}\nstatic const char *route_picker_label(void){" + label + "return live_view.tools_label;}\n"
routes += "static void route_auth_back(void){\n" + block("if (current_page == UI_PAGE_WIFI && wifi_mode != UI_WIFI_MODE_MENU)", ui.index("if (capture_back())")) + "\n}\n"
routes += "static const char *route_auth_back_label(void){const char *back_label=\"TARGET\";\n" + block("if (current_page == UI_PAGE_WIFI && wifi_mode != UI_WIFI_MODE_MENU)") + "\nreturn back_label;}\n"
(out / "sae_routes.h").write_text(routes)
assert '#include "wifi_sae_ui.h"' in ui
assert "!ap_busy() && !capture_busy()" in ui
assert "if (wifi_mode == UI_WIFI_MODE_CAPTURE) return capture_draw();" in ui
target = (root / "s3_main/main/wifi_target_ui.h").read_text()
assert "CERBERUS_WIFI_TARGET_CAPTURE){capture_enter();wifi_mode=UI_WIFI_MODE_CAPTURE;}" in target
# Selection uses the production painted publication fence and target-copy
# operation. Only display I/O and the unrelated scan scheduler are doubled.
start = live.index("typedef struct {")
end = live.index("} live_wifi_paint_map_t;", start) + len("} live_wifi_paint_map_t;")
(out / "sae_picker_types.h").write_text(live[start:end])
picker = block("static bool live_wifi_paint_map_current(", source=live) + "\n"
picker += block("static esp_err_t live_paint(", source=live) + "\n"
picker += block("static bool wifi_target_toggle_selection(", source=target) + "\n"
picker += "static void route_picker_row(unsigned row){unsigned action=SENTINEL_VIEW_ROW0+row;unsigned tool=SENTINEL_SCAN_WIFI;uint16_t index=(uint16_t)row;\n"
picker += block("if (tool == SENTINEL_SCAN_WIFI && (!wifi_attack_selecting || (sae_ui_selecting || eapol_ui_selecting)))", source=live) + "\n"
picker += block("if(wifi_attack_selecting){", source=live) + "\n}\n"
(out / "sae_picker.h").write_text(picker)

(out / "sae_picker_begin.h").write_text(block("static void auth_monitor_picker_begin(void)", source=live))

env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-global"), ZIG_LOCAL_CACHE_DIR=str(out / "zig-local"))
exe = out / ("sae-ui.exe" if os.name == "nt" else "sae-ui")
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
    "-I", str(out), "-I", str(root / "tools/host_checks/stubs"), "-I", str(root / "s3_main/main"), "-I", str(root / "shared"),
    str(root / "tools/host_checks/wifi_sae_ui_check.c"), str(root / "s3_main/main/ui_widgets.c"), str(root / "s3_main/main/sentinel_hub.c"),
    str(root / "shared/wifi_capture_plan.c"), str(root / "shared/wifi_packet_count.c"), "-o", str(exe)], check=True, env=env)
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
