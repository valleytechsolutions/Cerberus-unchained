"""Run the actual survey page, coordinator and immutable model through parent routes."""
from pathlib import Path
import os
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/wifi-ap-survey-live-ui-checks"
out.mkdir(parents=True, exist_ok=True)
ui = (root / "s3_main/main/ui.c").read_text()
display = (root / "s3_main/main/display.c").read_text()
start = display.index("static const uint8_t *font5x7(")
(out / "display_font.h").write_text(display[start:display.index("\nesp_err_t ", start)])


def block(text, start):
    opening = text.index("{", start)
    depth = 0
    for end in range(opening, len(text)):
        depth += (text[end] == "{") - (text[end] == "}")
        if not depth:
            return text[start:end + 1]
    raise ValueError("Unclosed production block")


types = re.findall(r"typedef enum \{[^}]*\} ui_(?:page|wifi_mode)_t;", ui)
assert len(types) == 2
(out / "survey_route_types.h").write_text("\n".join(types))
handler = block(ui, ui.index("static void handle_tap(uint16_t x, uint16_t y)\n{"))
field = block(handler, handler.index("if (current_page == UI_PAGE_FIELD_MENU)"))
action = block(field, field.index("if (action == CERBERUS_FIELD_AP_SURVEY)"))
assert field.index(action) < field.index("if (action == CERBERUS_FIELD_BLE) open_tool_page")
tap = block(handler, handler.index("if (current_page == UI_PAGE_WIFI && wifi_mode == UI_WIFI_MODE_AP_SURVEY)"))
launcher = block(ui, ui.index("static void return_to_launcher(void)\n{"))
back = block(launcher, launcher.index("if (current_page == UI_PAGE_WIFI && wifi_mode == UI_WIFI_MODE_AP_SURVEY)"))
(out / "survey_parent_routes.h").write_text(
    "static void actual_field_route(cerberus_field_action_t action) {\n" + action + "\n}\n" +
    "static void actual_tap(uint16_t x,uint16_t y) {\n" + tap + "\n}\n" +
    "static void actual_launcher(void) {\n" + back + "\n}\n")
assert "ap_survey_tick((uint32_t)now_ms, screen_asleep);" in ui
assert "!ap_survey_busy() &&" in ui
assert 'if (current_page == UI_PAGE_WIFI && wifi_mode == UI_WIFI_MODE_AP_SURVEY) return ap_survey_draw(true);' in ui
assert '"R1-R6", 1, true, theme), "hub", "survey"' in (root / "s3_main/main/sentinel_hub.c").read_text()
exe = out / ("wifi-ap-survey-live-ui-check.exe" if os.name == "nt" else "wifi-ap-survey-live-ui-check")
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-cache"))
subprocess.run([
    sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
    "-I", str(out), "-I", str(root / "tools/host_checks/stubs"),
    "-I", str(root / "s3_main/main"), "-I", str(root / "shared"),
    str(root / "tools/host_checks/wifi_ap_survey_live_ui_check.c"),
    *[str(root / "s3_main/main" / name) for name in (
        "wifi_ap_survey_model.c", "wifi_ap_survey_session.c", "field_menu.c", "ui_widgets.c", "privacy.c")],
    *[str(root / "shared" / name) for name in (
        "wifi_ap_survey.c", "wifi_capture_plan.c", "wifi_packet_count.c")],
    "-o", str(exe),
], check=True, env=env)
subprocess.run([str(exe), str(out / "survey-live")], check=True)
