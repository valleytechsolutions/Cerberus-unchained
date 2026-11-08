"""Exercise the production geofence policy without a receiver or private locations."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/wardrive-geofence-checks"
out.mkdir(parents=True, exist_ok=True)
environment = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-cache"))
executable = out / "wardrive-geofence-check.exe"
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
    "-I", str(root / "s3_main/main"), str(root / "s3_main/main/wardrive_geofence.c"),
    str(root / "tools/host_checks/wardrive_geofence_check.c"), "-lm", "-o", str(executable)],
    check=True, env=environment)
subprocess.run([str(executable)], check=True, env=environment)

settings = (root / "s3_main/main/settings.c").read_text(encoding="utf-8")
begin = settings.index("static cerberus_geofence_config_t geofence_settings;")
end = settings.index("cerberus_gps_config_t cerberus_settings_gps_get", begin)
(out / "geofence_settings_impl.h").write_text(settings[begin:end])
service = (root / "s3_main/main/wardrive_service.c").read_text(encoding="utf-8")
begin = service.index("    bool geofences_enabled = false;", service.index("static esp_err_t wardrive_start_with_idle_probes_paused("))
end = service.index("    cerberus_wifi_scan_status_payload_t wifi =", begin)
(out / "geofence_admission_impl.h").write_text(
    "static esp_err_t admission(bool include_expansions)\n{\n" + service[begin:end] + "    return ESP_OK;\n}\n")
settings_exe = out / "geofence-settings.exe"
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
    "-I", str(out), "-I", str(root / "shared"), "-I", str(root / "s3_main/main"), str(root / "s3_main/main/wardrive_geofence.c"),
    str(root / "tools/host_checks/wardrive_geofence_settings_check.c"), "-lm", "-o", str(settings_exe)],
    check=True, env=environment)
subprocess.run([str(settings_exe)], check=True, env=environment)

stubs = out / "stubs"
stubs.mkdir(exist_ok=True)
for name in ("esp_err.h", "esp_random.h"):
    (stubs / name).write_text("/* SDK boundary supplied by fixture. */\n")
for tick_ms in (1, 10):
    worker = out / f"wardrive-geofence-worker-{tick_ms}.exe"
    subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
        f"-DFIXTURE_TICK_MS={tick_ms}", "-I", str(stubs), "-I", str(root / "shared"),
        "-I", str(root / "s3_main/main"), str(root / "tools/host_checks/wardrive_geofence_worker_check.c"),
        str(root / "s3_main/main/wardrive_core.c"), str(root / "s3_main/main/wardrive_geofence.c"),
        "-lm", "-o", str(worker)], check=True, env=environment)
    subprocess.run([str(worker)], check=True, env=environment)
