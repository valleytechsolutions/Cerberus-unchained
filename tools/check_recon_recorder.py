"""Exercise actual async recorder lifecycle with deterministic RTOS/storage boundaries."""
from pathlib import Path
import json
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / 'artifacts/recon-recorder-checks'
out.mkdir(parents=True, exist_ok=True)
source = (root / 's3_main/main/recon_recorder.c').read_text(encoding='utf-8')
(out / 'recorder_impl.h').write_text(re.sub(r'^#include[^\n]*\n', '', source, flags=re.M), encoding='utf-8')
console = (root / 's3_main/main/usb_console.c').read_text(encoding='utf-8')
start = console.index('static void print_recon_record(void)')
end = console.index('static void print_wifi_seen_error(', start)
(out / 'recorder_console_impl.h').write_text(console[start:end], encoding='utf-8')
assert '"recon_record"' in console and 'print_recon_record();' in console
exe = out / 'recon-recorder-check.exe'
subprocess.run([sys.executable, '-m', 'ziglang', 'cc', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
    '-I', str(out), '-I', str(root / 'tools/host_checks/stubs'),
    '-I', str(root / 's3_main/main'), '-I', str(root / 'shared'),
    str(root / 'tools/host_checks/recon_recorder_check.c'), str(root / 's3_main/main/recon_journal.c'),
    '-o', str(exe)], check=True)
subprocess.run([str(exe), str(out)], check=True)
sample = json.loads((out / 'complete-session.json').read_text(encoding='utf-8'))
assert sample['state'] == 'complete'
assert sample['accepted'] == sample['written'] == 4
assert sample['row_counter_version'] == 1
for kind, count in (('wifi', 1), ('ble', 3), ('ibeacon', 1)):
    assert sample[f'accepted_{kind}_rows'] == sample[f'written_{kind}_rows'] == count
print('Async recorder lifecycle and read-only diagnostic extraction passed; complete manifest parses as JSON.')
