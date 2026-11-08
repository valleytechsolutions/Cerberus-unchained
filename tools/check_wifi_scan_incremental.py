"""Fault-inject actual single-attempt AP readers and incremental bank fences."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/wifi-scan-incremental-checks"
out.mkdir(parents=True, exist_ok=True)
source = (root / "s3_main/main/c5_spi_link.c").read_text()
start = source.index("static esp_err_t decode_status_v2_response(")
end = source.index("static esp_err_t exchange_v2_locked(", start)
(out / "scan_peer_impl.h").write_text(source[start:end])
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-cache"))
exe = out / ("wifi-scan-incremental-check.exe" if os.name == "nt" else "wifi-scan-incremental-check")
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
    "-I", str(root / "tools/host_checks/stubs"), "-I", str(out),
    "-I", str(root / "shared"), "-I", str(root / "s3_main/main"),
    str(root / "tools/host_checks/wifi_scan_incremental_check.c"),
    str(root / "s3_main/main/s3_wifi_relation.c"), "-o", str(exe)], check=True, env=env)
subprocess.run([str(exe)], check=True, env=env)
