#!/usr/bin/env python3
"""Build the integration target matrix without uploading or publishing firmware."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
from datetime import datetime, timezone

ROOT = Path(__file__).resolve().parents[1]
TARGETS = {
    "s3": (ROOT, "s3-main"),
    "onboard": (ROOT / "c5_firmware", "c5-onboard"),
    "bridge": (ROOT, "s3-c5-bridge"),
    "expansion-wroom": (ROOT / "expansion/c5_scanner", "c5-expansion-wroom"),
    "expansion-xiao": (ROOT / "expansion/c5_scanner", "c5-expansion-xiao"),
}


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inputs() -> dict[str, str]:
    """Fingerprint firmware inputs, including effective generated SDK configs."""
    result = {}
    excluded = {".pio", ".git", "managed_components", "artifacts", "dist", "__pycache__", "node_modules", "web"}
    for path in ROOT.rglob("*"):
        relative = path.relative_to(ROOT)
        if any(part in excluded for part in relative.parts) or not path.is_file():
            continue
        if path.name.endswith(".old"):
            continue
        if path.suffix in {".c", ".h", ".cpp", ".py", ".S", ".ld", ".lf", ".ini", ".csv", ".json", ".yml", ".lock", ".defaults"} or path.name == "CMakeLists.txt" or path.name.startswith("sdkconfig"):
            result[relative.as_posix()] = digest(path)
    return result


def verify_console_stack(root: Path) -> dict:
    """Gate command isolation and a measured subset of the native call frames.

    This is not a whole-program stack bound: deeper SDK/libc paths, interrupt
    frames and stack high-water marks still require on-device validation.
    """
    from build_toolchain import resolve_build_tools

    build = root / ".pio/build/s3-main"
    tools = resolve_build_tools(build, "esp32s3")
    image = build / "firmware.elf"
    source_path = root / "s3_main/main/usb_console.c"
    source = source_path.read_text(encoding="utf-8")
    stack_match = re.search(r"^#define USB_CONSOLE_TASK_STACK_BYTES (\d+)U$", source, re.M)
    if stack_match is None or int(stack_match[1]) != 4096:
        raise RuntimeError("Console stack allocation changed; review its native frame budget")
    allocated = int(stack_match[1])
    dispatch = source.split("static void execute_command(char *line)", 1)[1].split(
        "static void usb_console_task(", 1)[0]
    handlers = set(re.findall(r"\b(print_\w+)\(\);", dispatch))
    isolated = set(re.findall(
        r"static __attribute__\(\(noinline\)\) void (print_\w+)\(void\);", source))
    if not handlers or handlers != isolated:
        raise RuntimeError("Every actual console command must have an isolated native boundary")
    symbols = subprocess.check_output([str(tools["objdump"]), "-t", str(image)], text=True)
    definitions = set(re.findall(r"^static void (print_\w+)\(", source, re.M))
    requested = definitions | {"usb_console_task", "execute_command", "console_printf",
                               "vsnprintf", "_vsnprintf_r", "_svfprintf_r"}
    selected = {}
    for address, size, name in re.findall(
            r"^([0-9a-f]+)\s+\w+\s+F\s+\.\S+\s+([0-9a-f]+)\s+(\S+)$", symbols, re.M):
        if name in requested:
            if name in selected:
                raise RuntimeError(f"Ambiguous console frame symbol: {name}")
            selected[name] = (int(address, 16), int(size, 16))
    required = handlers | {"usb_console_task", "console_printf", "vsnprintf",
                           "_vsnprintf_r", "_svfprintf_r"}
    if not required <= selected.keys():
        raise RuntimeError("Missing isolated console symbols: " + ", ".join(sorted(required - selected.keys())))

    # Use the installed ESP GDB adjacent to the selected PlatformIO toolchain,
    # never a PATH debugger or an attached target. Binutils marks some flash
    # spans as data; GDB decodes their actual linked Xtensa instructions.
    suffix = ".exe" if tools["compiler"].suffix == ".exe" else ""
    debugger_bin = tools["compiler"].parents[2] / "tool-xtensa-esp-elf-gdb" / "bin"
    # Espressif packages use either the S3-specific or unified Xtensa name.
    # Both must come from the debugger package beside this build's compiler;
    # never fall back to a PATH executable or connect to a hardware target.
    candidates = [debugger_bin / (name + suffix) for name in
                  ("xtensa-esp32s3-elf-gdb", "xtensa-esp-elf-gdb")]
    debugger = next((path.resolve(strict=True) for path in candidates if path.is_file()), None)
    if debugger is None:
        raise RuntimeError(f"Missing packaged Xtensa GDB in {debugger_bin}; resolve ESP-IDF dependencies first")
    command = [str(debugger), "-nx", "-nh", "-batch", "-ex", "set auto-load off",
               "-ex", "set pagination off", "-ex", f'file "{image.as_posix()}"']
    for name, (address, size) in sorted(selected.items()):
        command += ["-ex", f"echo CERBERUS_FRAME {name}\\n", "-ex",
                    f"disassemble {address:#x},{address + size:#x}"]
    disassembly = subprocess.check_output(command, text=True, stderr=subprocess.STDOUT, timeout=60)
    frames, calls = {}, {}
    for block in disassembly.split("CERBERUS_FRAME ")[1:]:
        name, body = block.split("\n", 1)
        instructions = re.findall(r"^\s*0x([0-9a-f]+)\s+<[^>]+>:\s+(\S+)\s*(.*)$", body, re.M)
        address, size = selected[name]
        if (not instructions or int(instructions[0][0], 16) != address or
                any(not address <= int(pc, 16) < address + size for pc, _, _ in instructions)):
            raise RuntimeError(f"Cannot bind decoded console instructions: {name}")
        first = instructions[0]
        entry = re.fullmatch(r"a1,\s*(0x[0-9a-f]+|[0-9]+)", first[2])
        if first[1] != "entry" or entry is None:
            raise RuntimeError(f"Unreviewed console frame prologue: {name}")
        frames[name] = int(entry[1], 0)
        if not frames[name] or frames[name] % 16:
            raise RuntimeError(f"Unreviewed console frame alignment: {name}")
        calls[name] = set()
        for index, (_, op, operands) in enumerate(instructions):
            if op.startswith("call") or op in {"j", "j.n"}:
                target = re.search(r"<(\w+)>", operands)
                if target and target[1] in selected:
                    calls[name].add(target[1])
                elif op == "callx8" and index:
                    _, previous_op, previous_operands = instructions[index - 1]
                    literal = re.fullmatch(r"(a\d+),\s*0x[0-9a-f]+\s+\(0x([0-9a-f]+) <(\w+)>\)",
                                           previous_operands)
                    if (previous_op == "l32r" and literal and literal[1] == operands.strip()
                            and literal[3] in selected
                            and int(literal[2], 16) == selected[literal[3]][0]):
                        calls[name].add(literal[3])
    if frames.keys() != selected.keys() or frames["usb_console_task"] > 192:
        raise RuntimeError("Console task permanent frame exceeds its reviewed 192-byte limit")
    if frames["console_printf"] > 128:
        raise RuntimeError("Console formatter frame exceeds its reviewed 128-byte limit")
    if ("vsnprintf" not in calls["console_printf"] or
            "_vsnprintf_r" not in calls["vsnprintf"] or
            "_svfprintf_r" not in calls["_vsnprintf_r"]):
        raise RuntimeError("Native formatting call boundaries changed; review the known frame chain")

    def reachable(name: str, pending: frozenset[str] = frozenset()) -> set[str]:
        if name in pending:
            raise RuntimeError("Recursive console frame graph needs explicit review")
        found = {name}
        for child in calls[name]:
            found |= reachable(child, pending | {name})
        return found

    if not handlers <= reachable("usb_console_task"):
        raise RuntimeError("Console dispatch no longer calls every isolated command handler")

    def longest(name: str, pending: frozenset[str] = frozenset()) -> tuple[int, list[str]]:
        if name in pending:
            raise RuntimeError("Recursive console frame graph needs explicit review")
        children = [longest(child, pending | {name}) for child in sorted(calls[name])]
        used, path = max(children, default=(0, []), key=lambda item: item[0])
        return frames[name] + used, [name, *path]

    known_bytes, known_path = longest("usb_console_task")
    if allocated - known_bytes < 1024:
        raise RuntimeError(f"Known console frames leave less than 1024 bytes: {known_bytes}/{allocated}")
    result = {"status": "passed", "image": image.as_posix(), "sha256": digest(image),
              "source_sha256": digest(source_path), "checker_sha256": digest(Path(__file__)),
              "debugger": {"path": debugger.as_posix(), "sha256": digest(debugger)},
              "allocated_stack_bytes": allocated, "task_frame_limit_bytes": 192,
              "formatter_frame_limit_bytes": 128,
              "frames_bytes": frames, "isolated_handlers": sorted(handlers),
              "known_calls": {name: sorted(children) for name, children in calls.items()},
              "known_frame_path": known_path, "known_frame_bytes": known_bytes,
              "remaining_after_known_frames_bytes": allocated - known_bytes,
              "minimum_remaining_after_known_frames_bytes": 1024,
              "hardware_validated": False,
              "scope": "Linked command boundaries and known console/formatting frames only; "
                       "not a full SDK/libc/interrupt stack bound or a measured runtime high-water mark."}
    output = root / "artifacts/integration"
    output.mkdir(parents=True, exist_ok=True)
    (output / "s3-console-stack.txt").write_text(disassembly, encoding="utf-8")
    (output / "s3-console-stack.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", choices=["all", *TARGETS], default="all")
    parser.add_argument("--list", action="store_true", help="List targets without building")
    args = parser.parse_args()
    if args.list:
        for name, (project, environment) in TARGETS.items():
            print(f"{name}: {project.relative_to(ROOT)} / {environment}")
        return 0
    output = ROOT / "artifacts/integration"
    output.mkdir(parents=True, exist_ok=True)
    before = inputs()
    report = {"format": 1, "started_utc": datetime.now(timezone.utc).isoformat(),
              "status": "building", "hardware_validated": False, "targets": {}}
    report_path = output / "build-report.json"

    def save() -> None:
        report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    save()
    for name in TARGETS if args.target == "all" else [args.target]:
        project, environment = TARGETS[name]
        log = output / f"{name}.log"
        command = [sys.executable, "-m", "platformio", "run", "-j", "4", "-d", str(project), "-e", environment]
        print(f"Building {name}; log: {log}", flush=True)
        with log.open("w", encoding="utf-8") as stream:
            process = subprocess.run(command, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT)
        entry = {"environment": environment, "exit_code": process.returncode,
                 "status": "passed" if process.returncode == 0 else "failed", "log": str(log.relative_to(ROOT))}
        if process.returncode == 0 and name == "s3":
            try:
                from check_c5_ready_iram import verify as verify_ready_iram
                entry["ready_iram"] = verify_ready_iram(ROOT)
            except (RuntimeError, ValueError, OSError, subprocess.SubprocessError) as error:
                entry["status"] = "failed"
                entry["ready_iram_error"] = str(error)
            try:
                entry["usb_console_stack"] = verify_console_stack(ROOT)
            except (RuntimeError, ValueError, OSError, IndexError, subprocess.SubprocessError) as error:
                entry["status"] = "failed"
                entry["usb_console_stack_error"] = str(error)
        if process.returncode == 0 and name == "onboard":
            try:
                from check_mesh_locate_iram import verify as verify_locate_iram
                entry["mesh_locate_iram"] = verify_locate_iram(ROOT)
            except (RuntimeError, ValueError, OSError, subprocess.SubprocessError) as error:
                entry["status"] = "failed"
                entry["mesh_locate_iram_error"] = str(error)
            try:
                from check_c5_raw_link import verify
                entry["raw_tx_link"] = verify(ROOT)
            except (RuntimeError, OSError, subprocess.SubprocessError) as error:
                entry["status"] = "failed"
                entry["raw_tx_link_error"] = str(error)
        # Never report stale output as successful after a failed build.
        if process.returncode == 0:
            entry["files"] = {}
            for filename in ("firmware.bin", "firmware.elf", "firmware.map", "partitions.bin", "flasher_args.json"):
                path = project / ".pio/build" / environment / filename
                if path.is_file():
                    entry["files"][filename] = {"bytes": path.stat().st_size, "sha256": digest(path),
                                                "path": path.relative_to(ROOT).as_posix()}
            missing = sorted({"firmware.bin", "firmware.elf", "partitions.bin"} - entry["files"].keys())
            if missing:
                entry["status"] = "failed"
                entry["missing_outputs"] = missing
        report["targets"][name] = entry
        save()
        print(f"{name}: {entry['status']}", flush=True)
    after = inputs()
    report["inputs_before"] = before
    report["inputs_after"] = after
    report["inputs_changed"] = before != after
    passed = all(item["status"] == "passed" for item in report["targets"].values())
    report["status"] = "passed" if passed and before == after else "needs-review" if passed else "failed"
    report["completed_utc"] = datetime.now(timezone.utc).isoformat()
    save()
    print(f"Report: {report_path}")
    if before != after:
        print("Build inputs changed (possibly dependency resolution); rerun for a stable receipt.")
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
