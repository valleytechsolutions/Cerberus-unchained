"""Exercise the production readout telemetry and its local-only console route."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / 'artifacts/wifi-readout-diagnostics-checks'
(out / 'freertos').mkdir(parents=True, exist_ok=True)
(out / 'freertos/FreeRTOS.h').write_text('''#pragma once
#include <stdatomic.h>
typedef struct { atomic_flag flag; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED { ATOMIC_FLAG_INIT }
void readout_test_enter(portMUX_TYPE *lock);
void readout_test_exit(portMUX_TYPE *lock);
#define portENTER_CRITICAL(lock) readout_test_enter(lock)
#define portEXIT_CRITICAL(lock) readout_test_exit(lock)
''', encoding='utf-8')
source = (root / 's3_main/main/usb_console.c').read_text(encoding='utf-8')
def between(first, after):
    start = source.index(first)
    return source[start:source.index(after, start)]

def function(signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n\n'

parts = between('static void print_help(void)', 'static void print_info(void)')
parts += function('static void print_wifi_readout(void)')
parts += between('static void execute_command(', 'static void usb_console_task(')
(out / 'wifi_readout_console_impl.h').write_text(parts, encoding='utf-8')
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / 'zig-cache'))
exe = out / ('wifi-readout-diagnostics-check.exe' if os.name == 'nt' else 'wifi-readout-diagnostics-check')
command = [sys.executable, '-m', 'ziglang', 'cc', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
    '-I', str(out), '-I', str(root / 's3_main/main'),
    str(root / 'tools/host_checks/wifi_readout_diagnostics_check.c'), '-o', str(exe)]
if os.name != 'nt': command.append('-pthread')
subprocess.run(command, check=True, env=env)
subprocess.run([str(exe)], check=True, env=env)
