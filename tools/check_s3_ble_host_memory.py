"""Exercise real host ownership/admission in internal, external and disabled builds."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/s3-ble-host-memory-checks"
stubs = out / "stubs"
for name in ("esp_err.h", "sdkconfig.h", "esp_heap_caps.h", "esp_timer.h", "esp_psram.h",
             "freertos/FreeRTOS.h", "freertos/task.h", "host/ble_hs.h", "host/util/util.h",
             "nimble/nimble_port.h", "nimble/nimble_port_freertos.h"):
    path = stubs / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("/* Harness supplies SDK types and functions. */\n")
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-global"),
           ZIG_LOCAL_CACHE_DIR=str(out / "zig-local"))
for external in (0, 1, None):
    name = "disabled" if external is None else str(external)
    exe = out / f"host-memory-{name}.exe"
    source = "s3_ble_host_disabled_check.c" if external is None else "s3_ble_host_memory_check.c"
    defines = [] if external is None else [f"-DCONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL={external}"]
    subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1",
        "-Wall", "-Wextra", "-Werror", *defines,
        "-I", str(stubs), "-I", str(root / "s3_main/main"),
        str(root / "tools/host_checks" / source), "-o", str(exe)], check=True, env=env)
    subprocess.run([str(exe)], check=True, env=env)

radio = (root / "sdkconfig.radio-lab.defaults").read_text()
global_defaults = (root / "sdkconfig.defaults").read_text()
for expected in ("CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y",
                 "CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL=n",
                 "CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_DEFAULT=n"):
    assert expected in radio
for expected in ("CONFIG_SPIRAM_USE_CAPS_ALLOC=y", "CONFIG_SPIRAM_USE_MALLOC=n",
                 "CONFIG_SPIRAM_IGNORE_NOTFOUND=y"):
    assert expected in global_defaults
print("S3 host allocation defaults retain caps-only global policy and optional-PSRAM boot.")
