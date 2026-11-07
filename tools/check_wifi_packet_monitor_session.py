"""Exercise the monitor's real C5/S3 protocol and asynchronous page transitions."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/packet-monitor-session-checks"
out.mkdir(parents=True, exist_ok=True)

source = (root / "c5_firmware/main/wifi_scanner.c").read_text()
start = source.index("static void wifi_scan_task(void *arg)")
(out / "wifi_scan_worker_impl.h").write_text(source[start:source.index(
    "esp_err_t cerberus_wifi_watch_get_status(", start)])
source = (root / "c5_firmware/main/main.c").read_text()
start = source.index("    case CERBERUS_SPI_COMMAND_WIFI_PACKET_MONITOR_SESSION_INFO:")
(out / "monitor_dispatch.h").write_text(source[start:source.index(
    "    case CERBERUS_SPI_COMMAND_WIFI_PACKET_MONITOR_START:", start)])
source = (root / "s3_main/main/c5_spi_link.c").read_text()
start = source.index("static esp_err_t survey_session_frame_locked(")
(out / "monitor_frame.h").write_text(source[start:source.index(
    "static esp_err_t survey_session_gate_locked(", start)])
source = (root / "s3_main/main/ui.c").read_text()
start = source.index("typedef enum {\n    PACKET_MONITOR_NONE")
(out / "monitor_ui_state.h").write_text(source[start:source.index(
    "static cerberus_c5_wifi_result_t wifi_selected_result;", start)])
start = source.index("    if (wifi_mode == UI_WIFI_MODE_PACKET_MONITOR) {")
tap = source[start:source.index("\n    if (wifi_mode != UI_WIFI_MODE_LIST)", start)]
start = source.index("                packet_monitor_request_action(packet_monitor_from_target ?")
back = source[start:source.index("            } else if", start)]
start = source.index("        bool packet_monitor_visible =")
tick = source[start:source.index("        mesh_archive_tick(", start)]
(out / "monitor_ui_routes.h").write_text(
    "static void actual_tap(uint16_t x, uint16_t y) {\n" + tap + "}\n" +
    "static void actual_back(void) {\n" + back + "}\n" +
    "static void actual_tick(uint64_t now_ms) {\n" + tick + "}\n")

env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-cache"))
exe = out / "wifi-packet-monitor-bridge.exe"
subprocess.run([
    sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1",
    "-Wall", "-Wextra", "-Werror", "-I", str(out),
    "-I", str(root / "tools/host_checks/stubs"),
    "-I", str(root / "c5_firmware/main"), "-I", str(root / "s3_main/main"),
    "-I", str(root / "shared"),
    str(root / "tools/host_checks/wifi_packet_monitor_bridge_check.c"),
    str(root / "s3_main/main/wifi_packet_monitor_scan.c"), "-o", str(exe),
], check=True, env=env)
subprocess.run([str(exe)], check=True, env=env)
