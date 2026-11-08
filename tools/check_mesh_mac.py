"""Exercise the production passive MAC-header parser with synthetic inputs."""
from datetime import datetime, timezone
from hashlib import sha256
from importlib.metadata import version
from pathlib import Path
import json
import os
import platform
import subprocess
import sys


def main():
    root = Path(__file__).resolve().parents[1]
    names = ("shared/mesh_mac.h", "shared/mesh_mac.c",
             "tools/host_checks/mesh_mac_check.c", "tools/check_mesh_mac.py",
             "docs/MESH_MAC_PARSER.md")
    before = {name: sha256((root / name).read_bytes()).hexdigest() for name in names}
    out = root / "artifacts/mesh-mac-checks"
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-global"),
               ZIG_LOCAL_CACHE_DIR=str(out / "zig-local"))
    exe = out / ("mesh-mac-check.exe" if os.name == "nt" else "mesh-mac-check")
    compile_command = [sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1",
                       "-Wall", "-Wextra", "-Werror", "-I", str(root / "shared"),
                       str(root / "tools/host_checks/mesh_mac_check.c"),
                       str(root / "shared/mesh_mac.c"), "-o", str(exe)]
    results = []
    text = []
    for command in (compile_command, [str(exe)]):
        print("COMMAND:", json.dumps(command), flush=True)
        result = subprocess.run(command, cwd=root, env=env, capture_output=True, text=True)
        output = result.stdout + result.stderr
        print(output, end="", flush=True)
        results.append({"command": command, "returncode": result.returncode})
        text.append("COMMAND " + json.dumps(command) + "\n" + output)
        if result.returncode:
            break
    after = {name: sha256((root / name).read_bytes()).hexdigest() for name in names}
    log = out / "mesh-mac-check.stdout.log"
    log.write_text("\n".join(text), encoding="utf-8", newline="\n")
    passed = len(results) == 2 and all(r["returncode"] == 0 for r in results) and before == after
    receipt = {
        "status": "PASS" if passed else "FAIL",
        "completed_utc": datetime.now(timezone.utc).isoformat(),
        "scope": "Production passive header parser, synthetic inputs; no RF or device acceptance",
        "python": sys.executable,
        "python_version": platform.python_version(),
        "ziglang_version": version("ziglang"),
        "source_sha256_before": before,
        "source_sha256_after": after,
        "inputs_unchanged": before == after,
        "results": results,
        "stdout_sha256": sha256(log.read_bytes()).hexdigest(),
    }
    (out / "mesh-mac-check.json").write_text(json.dumps(receipt, indent=2) + "\n",
                                            encoding="utf-8", newline="\n")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
