"""Exercise Recon scheduling plus its actual scoped acquisition/Watch adapter."""
from pathlib import Path
import os
import re
import struct
import subprocess
import sys
import zlib

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/recon-parallel-checks"
out.mkdir(parents=True, exist_ok=True)
exe = out / "recon-parallel-check.exe"
subprocess.run([
    sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
    "-I", str(root / "s3_main/main"),
    str(root / "tools/host_checks/recon_parallel_check.c"),
    str(root / "s3_main/main/sentinel_scan_session.c"),
    "-o", str(exe),
], check=True)
subprocess.run([str(exe)], check=True)

# Reuse the independent recorder suite's framebuffer, native record types and
# exact capture fence. Its executable must pass before the composite uses it.
subprocess.run([sys.executable,str(root/"tools/check_recon_recorder_ui.py")],check=True)
live=(root/"s3_main/main/sentinel_live_ui.h").read_text()
def function(source,name):
    match=re.search(r"^static [^\n]*\b"+re.escape(name)+r"\([^;]*?\)\s*\{",source,re.M)
    assert match,name
    begin=match.start();end=source.index("{",begin)+1;depth=1
    while depth:
        depth+=(source[end]=="{")-(source[end]=="}");end+=1
    return source[begin:end]+"\n"
parts="".join(function(live,name) for name in
    ("live_supported","live_page_tool","live_status","live_store_acquire"))
parts+='#include "recon_ble_live_ui.h"\n'
parts+="".join(function(live,name) for name in
    ("live_capture","live_observe_owned_wifi","live_observe","live_execute"))
(out/"recon_live_acquire_impl.h").write_text(parts)
(out/"recon_live_tick_impl.h").write_text(function(live,"live_scan_tick"))
exe=out/"recon-live-check.exe"
environment=dict(os.environ,ZIG_GLOBAL_CACHE_DIR=str(out/"zig-cache"),ZIG_LOCAL_CACHE_DIR=str(out/"zig-local"))
# This separate translation unit includes sentinel_watch_ui.h through the
# recorder fixture; expose its POSIX strnlen before any system header.
subprocess.run([sys.executable,"-m","ziglang","cc","-std=c11","-D_POSIX_C_SOURCE=200809L","-O1","-Wall","-Wextra","-Werror",
    "-I",str(out),"-I",str(root/"artifacts/recon-recorder-ui-checks"),
    "-I",str(root/"tools/host_checks/stubs"),"-I",str(root/"s3_main/main"),"-I",str(root/"shared"),
    str(root/"tools/host_checks/recon_live_check.c"),
    str(root/"s3_main/main/sentinel_watch.c"),str(root/"s3_main/main/sentinel_watch_activity.c"),
    str(root/"s3_main/main/sentinel_watch_view.c"),str(root/"s3_main/main/sentinel_scan_session.c"),
    str(root/"s3_main/main/recon_journal.c"),str(root/"s3_main/main/recon_inventory.c"),
    str(root/"s3_main/main/s3_wifi_list.c"),"-o",str(exe)],check=True,env=environment)
subprocess.run([str(exe),str(out)],check=True,env=environment)
def png_chunk(kind,payload):
    return struct.pack(">I",len(payload))+kind+payload+struct.pack(">I",zlib.crc32(kind+payload))
for path in out.glob("recon-*.ppm"):
    data=path.read_bytes();header=b"P6\n320 480\n255\n"
    assert data.startswith(header) and len(data)==len(header)+320*480*3
    pixels=data[len(header):]
    path.with_suffix(".png").write_bytes(b"\x89PNG\r\n\x1a\n"+
        png_chunk(b"IHDR",struct.pack(">IIBBBBB",320,480,8,2,0,0,0))+
        png_chunk(b"IDAT",zlib.compress(b"".join(b"\0"+pixels[y*960:(y+1)*960] for y in range(480))))+
        png_chunk(b"IEND",b""))
