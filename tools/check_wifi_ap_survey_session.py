"""Exercise the actual dual survey coordinator with bounded receiver services."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/wifi-ap-survey-session-checks"
out.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-global"),
           ZIG_LOCAL_CACHE_DIR=str(out / "zig-local"))
exe = out / "session.exe"
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1",
    "-Wall", "-Wextra", "-Werror", "-I", str(root / "shared"),
    "-I", str(root / "s3_main/main"),
    str(root / "tools/host_checks/wifi_ap_survey_session_check.c"),
    str(root / "s3_main/main/wifi_ap_survey_session.c"),
    str(root / "s3_main/main/wifi_ap_survey_model.c"),
    str(root / "shared/wifi_ap_survey.c"), str(root / "shared/wifi_capture_plan.c"),
    str(root / "shared/wifi_packet_count.c"), "-o", str(exe)], check=True, env=env)
subprocess.run([str(exe)], check=True, env=env)
