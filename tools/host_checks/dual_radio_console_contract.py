"""Synthetic contract check of the actual native console-stack gate.

Copies production console source into isolated artifacts and supplies fake tool
output. This verifies parser boundaries; it is not a native stack measurement.
"""
from pathlib import Path
import importlib.util
import re
import shutil
import sys
import tempfile
import types
from unittest.mock import patch

root = Path(__file__).resolve().parents[2]
out = root / "artifacts/dual-radio-console-contract"
out.mkdir(parents=True, exist_ok=True)
scratch = Path(tempfile.mkdtemp(prefix="contract-", dir=out))
spec = importlib.util.spec_from_file_location("draft_build_unified", root / "tools/build_unified.py")
module = importlib.util.module_from_spec(spec)
# Suppress Python bytecode writes beside frozen tools.
previous_bytecode = sys.dont_write_bytecode
sys.dont_write_bytecode = True
try:
    spec.loader.exec_module(module)
finally:
    sys.dont_write_bytecode = previous_bytecode

replica = scratch / "replica"
console_target = replica / "s3_main/main/usb_console.c"
console_target.parent.mkdir(parents=True)
shutil.copyfile(root / "s3_main/main/usb_console.c", console_target)
console_path = replica / "s3_main/main/usb_console.c"
source = console_path.read_text()
handlers = set(re.findall(r"\b(print_\w+)\(\);", source.split(
    "static void execute_command(char *line)", 1)[1].split("static void usb_console_task(", 1)[0]))
assert {"print_wifi_dual", "print_dual_ble", "print_ble_memory"} <= handlers
definitions = set(re.findall(r"^static void (print_\w+)\(", source, re.M))
assert handlers <= definitions
assert '#include "dual_ble_diagnostics_console.h"' not in source
for signature in ("static void print_wardrive_recent(void)", "static void print_radio(void)"):
    # No accidental new function in the extractor ending at the next known function.
    end = "static void print_radio(void)" if "wardrive" in signature else "static void print_pinescan(void)"
    span = source[source.index(signature):source.index(end, source.index(signature) + len(signature))]
    assert "print_dual_ble" not in span and "print_wifi_dual" not in span

image = replica / ".pio/build/s3-main/firmware.elf"
image.parent.mkdir(parents=True, exist_ok=True)
image.write_bytes(b"Synthetic parser fixture only; not firmware.\n")
compiler = scratch / "packages/toolchain/bin/xtensa-esp32s3-elf-gcc.exe"
debugger = scratch / "packages/tool-xtensa-esp-elf-gdb/bin/xtensa-esp32s3-elf-gdb.exe"
debugger.parent.mkdir(parents=True, exist_ok=True)
debugger.write_bytes(b"Synthetic debugger identity; never executed.\n")
toolchain = types.ModuleType("build_toolchain")
toolchain.resolve_build_tools = lambda *_: {"compiler": compiler, "objdump": scratch / "objdump.exe"}
names = definitions | {"usb_console_task", "execute_command", "console_printf",
                       "vsnprintf", "_vsnprintf_r", "_svfprintf_r"}
symbols = {name: (0x42000000 + index * 0x200, 0x100) for index, name in enumerate(sorted(names))}
symbol_text = "\n".join(f"{address:08x} l F .flash.text {size:08x} {name}"
                        for name, (address, size) in symbols.items())


def check_case(label, text, expected=None, *, drop_ble_call=False, ble_frame=160, formatter_frame=64):
    console_path.write_bytes(text.encode("utf-8"))

    def fake_output(command, **kwargs):
        if "-t" in command:
            return symbol_text
        assert command[0] == str(debugger.resolve())
        blocks = []
        selected = [part.split("CERBERUS_FRAME ")[1].split("\\n")[0]
                    for part in command if part.startswith("echo CERBERUS_FRAME ")]
        for name in selected:
            address, _ = symbols[name]
            children = []
            if name == "usb_console_task": children = ["execute_command"]
            elif name == "execute_command":
                children = sorted(handlers - ({"print_dual_ble"} if drop_ble_call else set()))
            elif name in handlers: children = ["console_printf"]
            elif name == "console_printf": children = ["vsnprintf"]
            elif name == "vsnprintf": children = ["_vsnprintf_r"]
            elif name == "_vsnprintf_r": children = ["_svfprintf_r"]
            frame = ble_frame if name == "print_dual_ble" else formatter_frame if name == "console_printf" else 64
            body = f"0x{address:x} <{name}+0>: entry a1, {frame}\n"
            for index, child in enumerate(children):
                body += f"0x{address + (index + 1)*3:x} <{name}+{(index+1)*3}>: call8 0x{symbols[child][0]:x} <{child}>\n"
            blocks.append(f"CERBERUS_FRAME {name}\n{body}")
        return "".join(blocks)

    with patch.dict(sys.modules, {"build_toolchain": toolchain}), \
         patch.object(module.subprocess, "check_output", side_effect=fake_output):
        try:
            result = module.verify_console_stack(replica)
        except RuntimeError as error:
            assert expected and expected in str(error), (label, error)
        else:
            assert expected is None, label
            assert {"print_dual_ble", "print_wifi_dual", "print_ble_memory"} <= result["frames_bytes"].keys()
            assert {"print_dual_ble", "print_wifi_dual", "print_ble_memory"} <= set(result["known_calls"]["execute_command"])
            assert not result["hardware_validated"]


check_case("both visible", source)
check_case("missing isolated declaration", source.replace(
    "static __attribute__((noinline)) void print_dual_ble(void);\n", ""),
    "Every actual console command")
# The original header/attribute-definition approach must not silently pass.
check_case("hidden definition", source.replace("static void print_dual_ble(void)\n",
    "static __attribute__((noinline)) void print_dual_ble(void)\n"), "Missing isolated console symbols")
check_case("unreachable new handler", source, "Console dispatch no longer calls", drop_ble_call=True)
check_case("oversized new handler", source, "Known console frames leave less", ble_frame=3008)
check_case("formatter stack array regression", source, "Console formatter frame exceeds", formatter_frame=848)
console_path.write_bytes(source.encode("utf-8"))
print("Actual native-stack parser accepts all three diagnostic handlers and rejects hidden, unisolated, unreachable and oversized handlers.")
print("Synthetic parser contract only; native linked frame sizes remain unmeasured. All writes stayed under:", scratch)
