"""Exercise real survey receiver/service/dispatcher and bounded S3 transport."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/wifi-ap-survey-c5-checks"
out.mkdir(parents=True, exist_ok=True)
source = (root / "c5_firmware/main/wifi_scanner.c").read_text()
start = source.index("static void wifi_promiscuous_callback(")
brace = source.index("{", start)
depth, end = 1, brace + 1
while depth:
    depth += (source[end] == "{") - (source[end] == "}")
    end += 1
(out / "survey_outer_callback.h").write_text(source[start:end])
source = (root / "c5_firmware/main/main.c").read_text()
start = source.index("    /* BEGIN AP SURVEY DISPATCH */")
(out / "survey_dispatch.h").write_text(source[start:source.index("    /* END AP SURVEY DISPATCH */", start)])
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-cache"))
exe = out / "check.exe"
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1",
    "-Wall", "-Wextra", "-Werror", "-I", str(out),
    "-I", str(root / "shared"), "-I", str(root / "tools/host_checks/stubs"),
    "-I", str(root / "c5_firmware/main"), "-I", str(root / "s3_main/main"),
    str(root / "tools/host_checks/wifi_ap_survey_c5_check.c"),
    str(root / "shared/wifi_ap_survey.c"), str(root / "shared/wifi_capture_plan.c"),
    str(root / "shared/wifi_packet_count.c"), "-o", str(exe)], check=True, env=env)
subprocess.run([str(exe)], check=True, env=env)
