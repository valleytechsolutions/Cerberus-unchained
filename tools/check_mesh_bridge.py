"""Actual C5 ISR/worker/dispatch to S3 framed adapter/controller/model bridge."""
from pathlib import Path
import ast
import os
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/mesh-bridge-checks"
out.mkdir(parents=True, exist_ok=True)
# Reuse the worker platform declarations without executing its runner/compiler.
tree = ast.parse((root / "tools/check_radio_worker.py").read_text())
header = next(ast.literal_eval(node.value) for node in tree.body
              if isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id == "header"
                                                     for t in node.targets))
(out / "fake_worker_platform.h").write_text(header)
for name in ("esp_attr.h", "esp_ieee802154.h", "esp_log.h", "esp_heap_caps.h", "esp_random.h",
             "esp_timer.h", "freertos/FreeRTOS.h", "freertos/queue.h", "freertos/semphr.h", "freertos/task.h"):
    path = out / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('#include "fake_worker_platform.h"\n')
dispatch = (root / "c5_firmware/main/main.c").read_text()
state = dispatch[dispatch.index("static uint8_t current_radio_state(void)"):
                 dispatch.index("static void build_diagnostic_snapshot(")]
cases = dispatch[dispatch.index("    case CERBERUS_MESH_OBSERVATION_COMMAND_INFO:"):
                 dispatch.index("    case CERBERUS_SPI_COMMAND_IEEE802154_ENERGY_SCAN_START:")]
switches = re.findall(r'switch\s*\(\s*\(cerberus_spi_command_t\)request->command\s*\)\s*\{', dispatch)
assert len(switches) == 1, "Expected the actual typed production command switch"
(out / "mesh_bridge_dispatch.inc").write_text(state +
    "\nstatic void mesh_dispatch(const cerberus_spi_frame_t *request){" + switches[0] + "\n" +
    cases + "\ndefault:break;}}\n")
link = (root / "s3_main/main/c5_spi_link.c").read_text()
(out / "mesh_bridge_frame.inc").write_text(link[
    link.index("static esp_err_t survey_session_frame_locked("):
    link.index("static esp_err_t survey_session_gate_locked(")])
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-global"),
           ZIG_LOCAL_CACHE_DIR=str(out / "zig-local"))
exe = out / ("mesh-bridge-check.exe" if os.name == "nt" else "mesh-bridge-check")
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
    "-I", str(out), "-I", str(root / "tools/host_checks/stubs"), "-I", str(root / "shared"),
    "-I", str(root / "c5_firmware/main"), "-I", str(root / "s3_main/main"),
    str(root / "tools/host_checks/mesh_bridge_check.c"), str(root / "s3_main/main/mesh_scan.c"),
    str(root / "s3_main/main/mesh_observation.c"), str(root / "shared/mesh_mac.c"),
    "-o", str(exe)], check=True, env=env)
subprocess.run([str(exe)], check=True, env=env)
