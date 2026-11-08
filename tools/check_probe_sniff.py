"""Bounded passive Probe model/controller and actual native service checks."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/probe-sniff-checks"
out.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-global"), ZIG_LOCAL_CACHE_DIR=str(out / "zig-local"))
exe = out / "probe-sniff-check.exe"
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
    "-I", str(root / "tools/host_checks/stubs"), "-I", str(root / "s3_main/main"), "-I", str(root / "shared"),
    str(root / "tools/host_checks/probe_sniff_check.c"), str(root / "s3_main/main/probe_sniff_model.c"),
    str(root / "s3_main/main/probe_sniff_scan.c"), str(root / "shared/probe_sniff_core.c"),
    "-o", str(exe)], check=True, env=env)
subprocess.run([str(exe)], check=True, env=env)
subprocess.run([sys.executable, str(root / "tools/check_probe_sniff_native.py")], check=True)
def function(source, signature):
    start=source.index(signature);brace=source.index("{",start);depth=1;end=brace+1
    while depth:
        if source[end]=="{":depth+=1
        if source[end]=="}":depth-=1
        end+=1
    return source[start:end]+"\n"
source=(root/"c5_firmware/main/wifi_scanner.c").read_text()
(out/"probe_native_callback.h").write_text(function(source,"static void wifi_promiscuous_callback("))
(out/"probe_native_admission.h").write_text(function(source,"void cerberus_wifi_scanner_get_status("))
source=(root/"c5_firmware/main/main.c").read_text();start=source.index("    /* BEGIN PROBE SNIFF DISPATCH */")
(out/"probe_native_dispatch.h").write_text(source[start:source.index("    /* END PROBE SNIFF DISPATCH */",start)])
(out/"probe_bridge_frame.h").write_text(function((root/"s3_main/main/c5_spi_link.c").read_text(),"static esp_err_t survey_session_frame_locked("))
exe=out/"probe-sniff-bridge.exe"
subprocess.run([sys.executable,"-m","ziglang","cc","-std=c11","-O1","-Wall","-Wextra","-Werror",
    "-I",str(out),"-I",str(root/"tools/host_checks/stubs"),"-I",str(root/"s3_main/main"),
    "-I",str(root/"c5_firmware/main"),"-I",str(root/"shared"),
    str(root/"tools/host_checks/probe_sniff_bridge_check.c"),str(root/"s3_main/main/probe_sniff_model.c"),
    str(root/"s3_main/main/probe_sniff_scan.c"),str(root/"shared/probe_sniff_core.c"),"-o",str(exe)],check=True,env=env)
subprocess.run([str(exe)],check=True,env=env)
