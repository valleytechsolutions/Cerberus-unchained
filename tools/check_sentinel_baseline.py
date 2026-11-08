"""Exercise cached baseline comparison with the actual production trust decision."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/sentinel-baseline-checks"
out.mkdir(parents=True, exist_ok=True)
source = (root / "s3_main/main/settings.c").read_text()


def function(signature):
    start = source.index(signature)
    body = source.index("{", start)
    depth = 1
    end = body + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def declaration(name):
    end = source.index("} " + name + ";") + len("} " + name + ";")
    start = source.rindex("typedef struct", 0, end)
    return source[start:end]


# Extract only the exact in-memory types and read decisions. NVS/platform
# initialization and writes are outside this fixture; no replacement classifier.
(out / "baseline_settings_impl.h").write_text("\n\n".join([
    declaration("wifi_trusted_entry_t"), declaration("wifi_trusted_record_t"),
    "static bool settings_ready;\nstatic wifi_trusted_record_t wifi_trusted;",
    "#define cerberus_settings_wifi_trusted_count production_trusted_count\n"
    "#define cerberus_settings_wifi_trust_state production_trust_state",
    function("size_t cerberus_settings_wifi_trusted_count(void)"),
    function("cerberus_wifi_trust_state_t cerberus_settings_wifi_trust_state("),
    "#undef cerberus_settings_wifi_trusted_count\n#undef cerberus_settings_wifi_trust_state",
]), encoding="utf-8")
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-cache"))
exe = out / ("sentinel-baseline-check.exe" if os.name == "nt" else "sentinel-baseline-check")
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1",
    "-Wall", "-Wextra", "-Werror", "-I", str(out),
    "-I", str(root / "tools/host_checks/stubs"), "-I", str(root / "shared"),
    "-I", str(root / "s3_main/main"),
    str(root / "tools/host_checks/sentinel_baseline_check.c"),
    str(root / "s3_main/main/sentinel_baseline.c"), "-o", str(exe)], check=True, env=env)
subprocess.run([str(exe)], check=True, env=env)
