"""Compile the actual GPS UART owner and parser with scripted SDK boundaries."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/wardrive-gps-uart-checks"
out.mkdir(parents=True, exist_ok=True)
stub = r'''
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 257
#define ESP_ERR_INVALID_ARG 258
#define ESP_ERR_INVALID_STATE 259
#define ESP_ERR_INVALID_RESPONSE 260
#define ESP_ERR_TIMEOUT 261
#define ESP_ERR_NOT_FINISHED 262
#define ESP_ERR_NOT_FOUND 263
#define ESP_ERR_NOT_SUPPORTED 264
typedef void *SemaphoreHandle_t;
#define portMAX_DELAY 0xffffffffU
#define pdMS_TO_TICKS(x) (x)
#define pdTRUE 1
#define pdPASS 1
#define UART_NUM_2 2
#define GPIO_NUM_1 1
#define UART_DATA_8_BITS 8
#define UART_PARITY_DISABLE 0
#define UART_STOP_BITS_1 1
#define UART_HW_FLOWCTRL_DISABLE 0
#define UART_SCLK_DEFAULT 0
#define UART_PIN_NO_CHANGE -1
#define MALLOC_CAP_SPIRAM 1U
#define MALLOC_CAP_8BIT 2U
typedef struct { int baud_rate,data_bits,parity,stop_bits,flow_ctrl,source_clk; } uart_config_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t, unsigned);
void xSemaphoreGive(SemaphoreHandle_t);
void vTaskDelay(unsigned);
void vTaskDelete(void *);
int xTaskCreate(void (*)(void *),const char *,unsigned,void *,unsigned,void *);
int64_t esp_timer_get_time(void);
esp_err_t uart_driver_install(int,int,int,int,void *,int);
esp_err_t uart_param_config(int,const uart_config_t *);
esp_err_t uart_set_baudrate(int,uint32_t);
esp_err_t uart_set_pin(int,int,int,int,int);
esp_err_t uart_flush_input(int);
esp_err_t uart_driver_delete(int);
int uart_read_bytes(int,void *,unsigned,unsigned);
void *heap_caps_calloc(size_t,size_t,unsigned);
void heap_caps_free(void *);
'''
(out / "gps_sdk_stub.h").write_text(stub, encoding="utf-8")
for name in ("esp_err.h", "esp_heap_caps.h", "esp_timer.h", "driver/gpio.h", "driver/uart.h",
             "freertos/FreeRTOS.h", "freertos/semphr.h", "freertos/task.h"):
    path = out / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('#include "gps_sdk_stub.h"\n', encoding="utf-8")
env = os.environ.copy()
env.setdefault("ZIG_GLOBAL_CACHE_DIR", str(out / "zig-global"))
env.setdefault("ZIG_LOCAL_CACHE_DIR", str(out / "zig-local"))
exe = out / "wardrive-gps-uart-check.exe"
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1", "-UNDEBUG",
    "-Wall", "-Wextra", "-Werror", "-I", str(out), "-I", str(root / "s3_main/main"),
    str(root / "tools/host_checks/wardrive_gps_uart_check.c"),
    str(root / "s3_main/main/wardrive_gps_core.c"), "-o", str(exe)], check=True, env=env)
subprocess.run([str(exe)], check=True, env=env)
