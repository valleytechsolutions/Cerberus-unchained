"""Read real C5 copied profiles through the dispatcher and bounded S3 reader."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/remote-id-profile-transport-checks"
out.mkdir(parents=True, exist_ok=True)
source = (root / "c5_firmware/main/wifi_scanner.c").read_text()
start = source.index("static cerberus_watch_profile_projection_t watch_profile_projection;")
(out / "profile_producer.h").write_text(source[start:source.index("esp_err_t cerberus_wifi_scanner_init(void)", start)])
source = (root / "c5_firmware/main/main.c").read_text()
start = source.index("    case CERBERUS_SPI_COMMAND_WATCH_PROFILE_BEGIN:")
(out / "profile_dispatch.h").write_text(source[start:source.index("    case CERBERUS_SPI_COMMAND_REMOTE_ID_START:", start)])
source = (root / "s3_main/main/c5_watch_link.h").read_text()
pieces = []
for first, after in (("static bool watch_record_valid(", "static esp_err_t watch_timing_locked("),
                     ("static bool watch_peer_equal(", "static esp_err_t watch_motion_timing_locked(")):
    start = source.index(first)
    pieces.append(source[start:source.index(after, start)])
(out / "profile_validation.h").write_text("\n".join(pieces))
exe = out / "check.exe"
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-cache"))
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
    "-I", str(out), "-I", str(root / "tools/host_checks/stubs"),
    "-I", str(root / "s3_main/main"), "-I", str(root / "shared"),
    str(root / "tools/host_checks/remote_id_profile_transport_check.c"),
    str(root / "shared/cerberus_watch_capture.c"),
    "-o", str(exe)], check=True, env=env)
subprocess.run([str(exe)], check=True, env=env)
