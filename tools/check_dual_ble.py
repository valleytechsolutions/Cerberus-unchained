"""Exercise the production bounded C5 companion reader and owner protocol."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/dual-ble-checks"
out.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-cache"),
           ZIG_LOCAL_CACHE_DIR=str(out / "zig-local"))
source = (root / "c5_firmware/main/ble_scanner.c").read_text()
record_end = source.index("} cerberus_ble_record_t;") + len("} cerberus_ble_record_t;")
record_begin = source.rindex("typedef struct {", 0, record_end)
(out / "c5_ble_record_type.h").write_text(source[record_begin:record_end])
state_begin = source.index("static uint64_t dual_boot_id,")
state_end = source.index("static void dual_expire_locked(void);", state_begin)
(out / "c5_ble_dual_state.h").write_text(source[state_begin:state_end])
legacy_begin = source.index("esp_err_t cerberus_ble_scanner_get_result(")
legacy_end = source.index('#include "dual_ble_service_impl.h"', legacy_begin)
(out / "c5_ble_legacy_getters.h").write_text(source[legacy_begin:legacy_end])
begin = source.index("static void copy_advertisement_fields(")
end = source.index("static void copy_raw_advertisement(", begin)
(out / "c5_ble_copy_fields_impl.h").write_text(source[begin:end])
begin = source.index("static void copy_raw_advertisement(")
end = source.index("static void finish_scan_for_epoch(", begin)
(out / "c5_ble_callback_impl.h").write_text(source[begin:end])
# Compile the actual receive cases, including the installed NimBLE EXT_DISC
# normalization. Central/GATT cases are unrelated to this passive fixture.
begin = source.index("static int gap_event(")
gap = source[begin:source.index("    case BLE_GAP_EVENT_CONNECT:", begin)]
(out / "c5_ble_gap_receive_impl.h").write_text(gap + "    default: break;\n    }\n    return 0;\n}\n")
for fixture, extra in (("dual_ble_check", [root / "s3_main/main/dual_ble_scan.c"]),
                       ("dual_ble_owner_check", []),
                       ("dual_ble_reacquire_check", [root / "s3_main/main/dual_ble_scan.c"]),
                       ("ble_analyzer_counter_check", []),
                       ("ble_response_check", []),
                       ("dual_ble_integration_check", [root / "s3_main/main/dual_ble_scan.c"])):
    exe = out / f"{fixture}.exe"
    subprocess.run([
        sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1", "-UNDEBUG",
        "-Wall", "-Wextra", "-Werror", "-I", str(out),
        "-I", str(root / "tools/host_checks/stubs"), "-I", str(root / "shared"),
        "-I", str(root / "s3_main/main"), "-I", str(root / "c5_firmware/main"),
        str(root / f"tools/host_checks/{fixture}.c"), *map(str, extra), "-o", str(exe),
    ], check=True, env=env)
    subprocess.run([str(exe)], check=True)
