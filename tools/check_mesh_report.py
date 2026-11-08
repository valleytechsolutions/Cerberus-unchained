"""Immutable released Mesh CSV, actual model/parser and boundary failures."""
from pathlib import Path
import os
import subprocess
import sys

root=Path(__file__).resolve().parents[1]
out=root/"artifacts/mesh-report-checks"
out.mkdir(parents=True,exist_ok=True)
env=dict(os.environ,ZIG_GLOBAL_CACHE_DIR=str(out/"zig-global"),ZIG_LOCAL_CACHE_DIR=str(out/"zig-local"))
exe=out/("mesh-report-check.exe" if os.name=="nt" else "mesh-report-check")
subprocess.run([sys.executable,"-m","ziglang","cc","-std=c11","-O1","-Wall","-Wextra","-Werror",
    "-I",str(root/"tools/host_checks/stubs"),"-I",str(root/"s3_main/main"),"-I",str(root/"shared"),
    str(root/"tools/host_checks/mesh_report_check.c"),str(root/"s3_main/main/mesh_report.c"),
    str(root/"s3_main/main/mesh_scan.c"),str(root/"s3_main/main/mesh_observation.c"),
    str(root/"shared/mesh_mac.c"),"-o",str(exe)],check=True,env=env)
subprocess.run([str(exe)],check=True,env=env)
