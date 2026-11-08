"""Exercise device metadata, fragmented transport, worker cancellation and UI.

SDK/RTOS boundaries are synthetic; no device, credentials or network is used.
The actual producer, parser, link reader, worker and view are compiled below.
"""
from pathlib import Path
import os
import struct
import subprocess
import sys
import zlib

root = Path(__file__).resolve().parents[1]
out = root / 'artifacts/device-info-checks'
out.mkdir(parents=True, exist_ok=True)
sdk = r'''
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef int esp_reset_reason_t;
typedef struct { int unused; } sdmmc_card_t;
typedef struct { unsigned model, revision, cores, features; } esp_chip_info_t;
typedef struct { uint8_t subtype; uint32_t address, size; char label[17]; } esp_partition_t;
typedef struct { char project_name[32],version[32],idf_ver[32],date[16],time[16]; uint8_t app_elf_sha256[32]; } esp_app_desc_t;
#define WIFI_IF_STA 0
#define WIFI_IF_AP 1
#define ESP_MAC_WIFI_STA 0
#define ESP_MAC_WIFI_SOFTAP 1
#define MALLOC_CAP_INTERNAL 1U
#define MALLOC_CAP_8BIT 2U
#define MALLOC_CAP_SPIRAM 4U
#define pdPASS 1
#define pdTRUE 1
#define pdMS_TO_TICKS(n) (n)
void esp_chip_info(esp_chip_info_t *);
esp_err_t esp_flash_get_size(void *, uint32_t *);
const esp_partition_t *esp_ota_get_running_partition(void);
const esp_app_desc_t *esp_app_get_description(void);
esp_err_t esp_efuse_mac_get_default(uint8_t *);
esp_err_t esp_wifi_get_mac(int,uint8_t *);
esp_err_t esp_read_mac(uint8_t *,int);
int64_t esp_timer_get_time(void);
void vTaskDelay(unsigned);
void vTaskDelete(void *);
int xTaskCreate(void (*)(void *),const char *,unsigned,void *,unsigned,void *);
size_t heap_caps_get_free_size(unsigned);
size_t heap_caps_get_total_size(unsigned);
void *heap_caps_calloc(size_t,size_t,unsigned);
void heap_caps_free(void *);
const char *esp_err_to_name(esp_err_t);
'''
(out / 'device_info_sdk.h').write_text(sdk)
for name in ('esp_app_desc.h', 'esp_chip_info.h', 'esp_flash.h', 'esp_mac.h', 'esp_ota_ops.h',
             'esp_wifi.h', 'esp_system.h', 'sdmmc_cmd.h', 'esp_heap_caps.h', 'esp_timer.h',
             'freertos/FreeRTOS.h', 'freertos/task.h'):
    path = out / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('#include "device_info_sdk.h"\n')
display = (root / 's3_main/main/display.c').read_text()
start = display.index('static const uint8_t *font5x7(')
(out / 'device_info_font.h').write_text(display[start:display.index('\nesp_err_t ', start)])
ui = (root / 's3_main/main/ui.c').read_text()
(out / 'device_info_widgets.h').write_text(ui[ui.index('static esp_err_t draw_header('):ui.index('static esp_err_t draw_small_button(')])
assert '#include "device_info_ui.h"' in ui
assert 'if (dashboard_mode == UI_DASHBOARD_MODE_DEVICE_INFO) return device_info_draw();' in ui
assert 'device_info_tick((uint64_t)now_ms, current_page == UI_PAGE_DASHBOARD &&' in ui
back = ui[ui.index('static void handle_tap(uint16_t x, uint16_t y)\n{'):]
assert 'dashboard_mode == UI_DASHBOARD_MODE_DEVICE_INFO) {\n                device_info_close();\n                dashboard_mode = UI_DASHBOARD_MODE_SUMMARY;' in back
tap = ui[ui.index('static void handle_dashboard_tap(uint16_t x, uint16_t y)\n{'):]
assert 'device_info_tap(x, y);' in tap and 'device_info_open();\n            dashboard_mode = UI_DASHBOARD_MODE_DEVICE_INFO;' in tap
main = (root / 'c5_firmware/main/main.c').read_text()
assert 'query.object_type == CERBERUS_FRAGMENT_OBJECT_DEVICE_IDENTITY' in main
assert 'cerberus_device_identity_collect(&identity, CERBERUS_C5_FIRMWARE_VERSION)' in main
console = (root / 's3_main/main/usb_console.c').read_text()
assert 'strcmp(start, "builds") == 0' in console
start = console.index('static void print_build_identity(')
end = console.index('static void print_memory(void)\n{', start)
(out / 'device_info_console.h').write_text(console[start:end])
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / 'zig-cache'))
exe = out / ('device-info-check.exe' if os.name == 'nt' else 'device-info-check')
subprocess.run([sys.executable, '-m', 'ziglang', 'cc', '-std=c11', '-O1', '-UNDEBUG', '-Wall', '-Wextra', '-Werror',
    '-I', str(out), '-I', str(root / 'tools/host_checks/stubs'), '-I', str(root / 's3_main/main'),
    '-I', str(root / 'shared'), '-I', str(root / 'c5_firmware/main'),
    str(root / 'tools/host_checks/device_info_check.c'), str(root / 'shared/device_identity.c'),
    str(root / 'shared/device_identity_esp.c'), str(root / 'c5_firmware/main/fragment_transfer.c'),
    str(root / 's3_main/main/device_info.c'), str(root / 's3_main/main/privacy.c'), str(root / 's3_main/main/ui_widgets.c'), '-o', str(exe)], check=True, env=env)
subprocess.run([str(exe), str(out)], check=True, env=env)

def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))

for path in out.glob('info-*.ppm'):
    prefix = b'P6\n320 480\n255\n'
    data = path.read_bytes()
    assert data.startswith(prefix) and len(data) == len(prefix) + 320 * 480 * 3
    pixels = data[len(prefix):]
    path.with_suffix('.png').write_bytes(b'\x89PNG\r\n\x1a\n' +
        chunk(b'IHDR', struct.pack('>IIBBBBB', 320, 480, 8, 2, 0, 0, 0)) +
        chunk(b'IDAT', zlib.compress(b''.join(b'\0' + pixels[y * 960:(y + 1) * 960] for y in range(480)))) + chunk(b'IEND', b''))
print('Device Info: SDK metadata, transfer faults, worker lifetime, console and rendered UI checks passed.')
