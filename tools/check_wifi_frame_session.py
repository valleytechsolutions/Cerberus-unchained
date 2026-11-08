"""Exercise the native frame session with the production summary model."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/wifi-frame-session-checks"
stubs = out / "stubs"
(stubs / "freertos").mkdir(parents=True, exist_ok=True)
(stubs / "freertos/FreeRTOS.h").write_text("""#pragma once
#include <stdint.h>
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
#define portMAX_DELAY UINT32_MAX
""", encoding="utf-8")
(stubs / "freertos/semphr.h").write_text("""#pragma once
#include <stdint.h>
typedef void *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t mutex, uint32_t ticks);
int xSemaphoreGive(SemaphoreHandle_t mutex);
""", encoding="utf-8")
(stubs / "esp_heap_caps.h").write_text("""#pragma once
#include <stddef.h>
#define MALLOC_CAP_8BIT 2U
#define MALLOC_CAP_SPIRAM 4U
void *heap_caps_calloc(size_t count, size_t size, unsigned caps);
void heap_caps_free(void *pointer);
""", encoding="utf-8")
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-global-cache"),
           ZIG_LOCAL_CACHE_DIR=str(out / "zig-local-cache"))
exe = out / "check.exe"
sources = ["s3_main/main/wifi_frame_session.c", "shared/wifi_frame_analysis.c",
           "shared/wifi_capture_filter.c", "shared/wifi_capture_plan.c",
           "shared/wifi_packet_count.c", "tools/host_checks/wifi_frame_session_check.c"]
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1",
    "-Wall", "-Wextra", "-Werror", "-I", str(stubs),
    "-I", str(root / "tools/host_checks/stubs"), "-I", str(root / "s3_main/main"),
    "-I", str(root / "shared"), *[str(root / source) for source in sources],
    "-o", str(exe)], check=True, env=env)
subprocess.run([str(exe)], check=True, timeout=30)
