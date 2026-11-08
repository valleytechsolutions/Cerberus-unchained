"""Run the production GPX worker against scripted GPS, SD and task boundaries."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/gps-recorder-checks"
out.mkdir(parents=True, exist_ok=True)
(out / "recorder_sdk.h").write_text(r'''
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef unsigned portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
void test_enter(void *);
void test_exit(void *);
#define portENTER_CRITICAL(p) test_enter(p)
#define portEXIT_CRITICAL(p) test_exit(p)
#define pdMS_TO_TICKS(n) (n)
#define pdPASS 1
#define MALLOC_CAP_INTERNAL 1U
#define MALLOC_CAP_8BIT 2U
#define MALLOC_CAP_SPIRAM 4U
int64_t esp_timer_get_time(void);
void vTaskDelay(unsigned);
void vTaskDelete(void *);
int xTaskCreate(void (*)(void *),const char *,unsigned,void *,unsigned,void *);
size_t heap_caps_get_free_size(unsigned);
size_t heap_caps_get_largest_free_block(unsigned);
void *heap_caps_calloc(size_t,size_t,unsigned);
void heap_caps_free(void *);
''', encoding="utf-8")
for name in ("freertos/FreeRTOS.h", "freertos/task.h", "esp_heap_caps.h", "esp_timer.h"):
    path = out / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('#include "recorder_sdk.h"\n', encoding="utf-8")
(out / "sdmmc_cmd.h").write_text("#pragma once\ntypedef struct { int unused; } sdmmc_card_t;\n")
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-cache"))
exe = out / "gps-recorder-check.exe"
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
    "-I", str(out), "-I", str(root / "tools/host_checks/stubs"), "-I", str(root / "s3_main/main"),
    "-I", str(root / "shared"), str(root / "tools/host_checks/gps_recorder_check.c"),
    str(root / "s3_main/main/gps_gpx.c"), "-lm", "-o", str(exe)], check=True, env=env)
subprocess.run([str(exe)], check=True, env=env)
