"""Fault-inject the production PSRAM scan snapshot allocation and publication."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/scan-display-store-checks"
out.mkdir(parents=True, exist_ok=True)
(out / "freertos").mkdir(exist_ok=True)
(out / "freertos/FreeRTOS.h").write_text('''#pragma once
#include <stdatomic.h>
typedef struct { atomic_flag flag; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED { ATOMIC_FLAG_INIT }
static inline void scan_store_enter(portMUX_TYPE *lock) {
    while (atomic_flag_test_and_set_explicit(&lock->flag, memory_order_acquire)) {}
}
static inline void scan_store_exit(portMUX_TYPE *lock) {
    atomic_flag_clear_explicit(&lock->flag, memory_order_release);
}
#define portENTER_CRITICAL(lock) scan_store_enter(lock)
#define portEXIT_CRITICAL(lock) scan_store_exit(lock)
''', encoding="utf-8")
source = (root / "s3_main/main/sentinel_live_ui.h").read_text(encoding="utf-8")
types_start = source.index("typedef struct {")
types_end = source.index("} live_scan_record_t;", types_start) + len("} live_scan_record_t;")
(out / "scan_display_record_type.h").write_text(source[types_start:types_end], encoding="utf-8")
start = source.index("static sentinel_scan_tool_t live_page_tool(void)")
end = source.index("static bool live_is_view(", start)
(out / "scan_display_page_tool_impl.h").write_text(source[start:end], encoding="utf-8")
start = source.index("static void live_status(")
end = source.index("static esp_err_t live_observe_owned_wifi(", start)
(out / "scan_display_capture_impl.h").write_text(source[start:end], encoding="utf-8")
start = source.index("static void live_scan_tick(")
end = source.index("static void live_reset_observations(", start)
(out / "scan_display_tick_impl.h").write_text(source[start:end], encoding="utf-8")
exe = out / "scan-display-store-check.exe"
env = os.environ.copy()
env["ZIG_GLOBAL_CACHE_DIR"] = str(out / "zig-cache")
subprocess.run([
    sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1",
    "-Wall", "-Wextra", "-Werror", "-I", str(out),
    "-I", str(root / "tools/host_checks/stubs"),
    "-I", str(root / "shared"), "-I", str(root / "s3_main/main"),
    str(root / "tools/host_checks/scan_display_store_check.c"),
    str(root / "s3_main/main/sentinel_scan_session.c"),
    str(root / "s3_main/main/wifi_readout_diagnostics.c"), "-o", str(exe),
], check=True, env=env)
subprocess.run([str(exe)], check=True, env=env)
