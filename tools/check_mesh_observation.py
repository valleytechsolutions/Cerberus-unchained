"""Production passive mesh parser/model regressions; no device or radio access."""
from hashlib import sha256
from pathlib import Path
import json
import os
import subprocess
import sys


def main():
    root = Path(__file__).resolve().parents[1]
    names = ("shared/mesh_mac.h", "shared/mesh_mac.c",
             "s3_main/main/mesh_observation.h", "s3_main/main/mesh_observation.c",
             "tools/host_checks/mesh_observation_check.c", "tools/check_mesh_observation.py")
    before = {name: sha256((root / name).read_bytes()).hexdigest() for name in names}
    out = root / "artifacts/mesh-observation-checks"
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-global"),
               ZIG_LOCAL_CACHE_DIR=str(out / "zig-local"))
    exe = out / ("mesh-observation-check.exe" if os.name == "nt" else "mesh-observation-check")
    command = [sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1",
               "-Wall", "-Wextra", "-Werror", "-I", str(root / "shared"),
               "-I", str(root / "s3_main/main"),
               str(root / "tools/host_checks/mesh_observation_check.c"),
               str(root / "s3_main/main/mesh_observation.c"),
               str(root / "shared/mesh_mac.c"), "-o", str(exe)]
    results, lines = [], []
    for args in (command, [str(exe)]):
        print("COMMAND:", json.dumps(args), flush=True)
        result = subprocess.run(args, cwd=root, env=env, capture_output=True, text=True)
        text = result.stdout + result.stderr
        print(text, end="", flush=True)
        lines.append("COMMAND: " + json.dumps(args) + "\n" + text)
        results.append({"command": args, "returncode": result.returncode})
        if result.returncode:
            break
    after = {name: sha256((root / name).read_bytes()).hexdigest() for name in names}
    log = out / "mesh-observation-check.log"
    log.write_text("\n".join(lines), encoding="utf-8", newline="\n")
    passed = len(results) == 2 and all(r["returncode"] == 0 for r in results) and before == after
    receipt = {"status": "PASS" if passed else "FAIL", "inputs_unchanged": before == after,
               "source_sha256_before": before, "source_sha256_after": after,
               "results": results, "log_sha256": sha256(log.read_bytes()).hexdigest(),
               "hardware_exercised": False}
    (out / "mesh-observation-check.json").write_text(json.dumps(receipt, indent=2) + "\n",
                                                    encoding="utf-8", newline="\n")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
