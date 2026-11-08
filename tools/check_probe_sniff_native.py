"""Compile actual C5 Probe Sniff service/RX/dispatcher with SDK boundaries mocked."""
from pathlib import Path
import os
import subprocess
import sys
root=Path(__file__).resolve().parents[1]
out=root/"artifacts/probe-sniff-native-checks"
out.mkdir(parents=True,exist_ok=True)
def function(source,signature):
    start=source.index(signature);brace=source.index("{",start);depth=1;end=brace+1
    while depth:
        if source[end]=="{":depth+=1
        if source[end]=="}":depth-=1
        end+=1
    return source[start:end]+"\n"
source=(root/"c5_firmware/main/wifi_scanner.c").read_text()
(out/"probe_native_callback.h").write_text(function(source,"static void wifi_promiscuous_callback("))
(out/"probe_native_admission.h").write_text(function(source,"void cerberus_wifi_scanner_get_status("))
source=(root/"c5_firmware/main/main.c").read_text()
start=source.index("    /* BEGIN PROBE SNIFF DISPATCH */")
(out/"probe_native_dispatch.h").write_text(source[start:source.index("    /* END PROBE SNIFF DISPATCH */",start)])
exe=out/"probe-sniff-native.exe"
env=dict(os.environ,ZIG_GLOBAL_CACHE_DIR=str(out/"zig-cache"),ZIG_LOCAL_CACHE_DIR=str(out/"zig-local-cache"))
subprocess.run([sys.executable,"-m","ziglang","cc","-std=c11","-O1","-Wall","-Wextra","-Werror",
    "-I",str(out),"-I",str(root/"shared"),"-I",str(root/"tools/host_checks/stubs"),"-I",str(root/"c5_firmware/main"),
    str(root/"tools/host_checks/probe_sniff_native_check.c"),str(root/"shared/probe_sniff_core.c"),"-o",str(exe)],check=True,env=env)
subprocess.run([str(exe)],check=True,env=env)
