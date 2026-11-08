"""Compile actual root UI entry blocks so ordinary/special routes stay distinct."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/dual-wifi-routing-checks"
out.mkdir(parents=True, exist_ok=True)
source = (root / "s3_main/main/ui.c").read_text(encoding="utf-8")
target = (root / "s3_main/main/wifi_target_ui.h").read_text(encoding="utf-8")
target_view = (root / "s3_main/main/wifi_target_view.c").read_text(encoding="utf-8")


def block(needle: str, after: int = 0, text: str = source) -> str:
    start = text.index(needle, after)
    opening = text.index("{", start)
    depth = 1
    index = opening + 1
    while depth:
        depth += (text[index] == "{") - (text[index] == "}")
        index += 1
    return text[start:index]


open_at = source.index("static void open_tool_page(ui_page_t page)\n{")
tap_at = source.index("static void handle_wifi_tap(uint16_t x, uint16_t y)\n{")
guide_at = source.index("    if (current_page == UI_PAGE_GUIDE) {", open_at)
generated = "static void open_tool_page(unsigned page) { current_page=page;\n"
generated += block("if (current_page == UI_PAGE_WIFI) {", open_at) + "\n}\n"
generated += "static void root_wifi_tap(uint16_t x,uint16_t y) {\n"
generated += block("if (wifi_mode == UI_WIFI_MODE_DUAL)", tap_at) + "\n"
generated += block("if (wifi_mode == UI_WIFI_MODE_MENU) {", tap_at) + "\n"
generated += block("if (wifi_mode == UI_WIFI_MODE_STATS) {", tap_at) + "\n}\n"
generated += "static void root_guide(unsigned action) {\n"
generated += block("if (action == SENTINEL_GUIDE_OPEN_WIFI ||", guide_at) + "\n}\n"
generated += "static void root_touch(uint16_t x,uint16_t y) {\n"
generated += block("if (!ui_capability_blocked && current_page == UI_PAGE_WIFI && wifi_mode == UI_WIFI_MODE_DUAL)", guide_at)
generated += "\n}\n"
generated += block("cerberus_wifi_target_action_t cerberus_wifi_target_tap(", text=target_view) + "\n"
# Compile the real target widget hit test and CHANGE action body, as well as the
# parent's separate footer route. Other AP actions are outside this regression.
generated += "static void root_target_tap(uint16_t x,uint16_t y) {\n"
generated += "cerberus_wifi_target_view_t view = {0};\n"
generated += "cerberus_wifi_target_action_t action = cerberus_wifi_target_tap(&view,x,y);\n"
generated += block("if(action==CERBERUS_WIFI_TARGET_CHANGE)", text=target) + "\n}\n"
generated += "static void root_target_back(void) {\n"
generated += block("if (current_page == UI_PAGE_WIFI && wifi_mode == UI_WIFI_MODE_TARGET) {", guide_at)
generated += "\n}\n"
(out / "dual_wifi_root_routes.h").write_text(generated, encoding="utf-8")
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-global"),
           ZIG_LOCAL_CACHE_DIR=str(out / "zig-local"))
exe = out / ("dual-wifi-routing-check.exe" if os.name == "nt" else "dual-wifi-routing-check")
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
    "-I", str(out), "-I", str(root / "tools/host_checks/stubs"),
    "-I", str(root / "s3_main/main"),
    str(root / "tools/host_checks/dual_wifi_routing_check.c"), "-o", str(exe)],
    check=True, env=env)
subprocess.run([str(exe)], check=True, env=env)
