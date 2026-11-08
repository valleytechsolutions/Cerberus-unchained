"""Fault-test the actual SD File Tools copy with real temporary file contents.

Only filesystem/syscall and FreeRTOS boundaries are replaced. Path validation,
mutex admission/release, firmware ownership and copy control flow are extracted
unchanged from storage.c. Host fsync is an injected boundary, not an SD durability
or power-loss test; real fopen/read/write/close/rename/remove preserve file data.
The actual Paste handler is also extracted; its copy branch uses that same
production function while status painting/list refresh are captured boundaries.
"""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/storage-file-copy-checks"
out.mkdir(parents=True, exist_ok=True)
source = (root / "s3_main/main/storage.c").read_text(encoding="utf-8")
header = (root / "s3_main/main/storage.h").read_text(encoding="utf-8")


def function(signature, text=source):
    start = text.index(signature)
    opening = text.index("{", start)
    depth = 0
    for end in range(opening, len(text)):
        depth += (text[end] == "{") - (text[end] == "}")
        if not depth:
            return text[start:end + 1]
    raise ValueError(signature)


constants = [re.search(r"^#define " + name + r"\s+[^\n]+", text, re.M).group(0)
    for text, name in ((source, "SD_HASH_BUFFER_SIZE"),
        (header, "CERBERUS_STORAGE_NAME_MAX"), (header, "CERBERUS_STORAGE_PATH_MAX"),
        (source, "SD_RECON_PATH_CAPACITY"), (header, "CERBERUS_STORAGE_RECON_NAME_CAPACITY"))]
end = header.index("} cerberus_storage_owner_t;") + len("} cerberus_storage_owner_t;")
constants.append(header[header.rindex("typedef enum", 0, end):end])
for name in ("sd_hash_buffer", "sd_operation_source_path",
             "sd_operation_destination_path", "sd_operation_temp_path"):
    constants.append(re.search(r"^static [^\n]*\b" + name + r"\[[^\n]+;", source, re.M).group(0))
end = source.index("static recon_storage_t sd_recon;") + len("static recon_storage_t sd_recon;")
constants.append(source[source.rindex("typedef struct", 0, end):end])
start = source.index("typedef struct {", source.index("/* Allocated only for optional selected-target PCAP"))
end = source.index("static unsigned sd_packet_count_next_index")
constants.append(source[start:end])
(out / "storage_copy_types.h").write_text("\n".join(constants) + "\n", encoding="utf-8")
parts = [function(signature) for signature in (
    "static bool relative_path_is_safe(const char *relative_path)\n{",
    "static bool file_name_is_safe(const char *file_name)\n{",
    "static esp_err_t build_entry_path(\n",
    "static esp_err_t require_firmware_owner_locked(void)\n{",
    "static esp_err_t ensure_storage_mutex(void)\n{",
    "static esp_err_t take_storage_mutex(void)\n{",
    "static void release_storage_mutex(void)\n{",
    "static bool recon_next_component(const char **cursor, const char **start, size_t *length)\n{",
    "static bool recon_path_protected(const char *path)\n{",
    "esp_err_t cerberus_storage_copy_file(\n",
)]
# The legacy real-file fixture has a dynamic temporary mount path. Its Recon
# lease is inactive; the dedicated journal suite exercises actual active paths.
implementation = "\n\n".join(parts).replace(
    "CERBERUS_STORAGE_MOUNT_PATH CERBERUS_STORAGE_RECON_PATH", "test_recon_root")
(out / "storage_copy_impl.h").write_text(implementation + "\n", encoding="utf-8")
ui = (root / "s3_main/main/ui.c").read_text(encoding="utf-8")
(out / "storage_copy_ui_impl.h").write_text(
    function("static void paste_file_clipboard(void)\n{", ui), encoding="utf-8")
exe = out / ("storage-file-copy-check.exe" if os.name == "nt" else "storage-file-copy-check")
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-cache"))
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-O1",
    "-Wall", "-Wextra", "-Werror", "-I", str(out),
    "-I", str(root / "tools/host_checks/stubs"),
    str(root / "tools/host_checks/storage_file_copy_check.c"), "-o", str(exe)],
    check=True, env=env)
with tempfile.TemporaryDirectory(prefix="files-", dir=out) as temporary:
    directory = Path(temporary).resolve()
    assert directory.is_relative_to(out.resolve())
    (directory / "src").mkdir()
    (directory / "dst").mkdir()
    subprocess.run([str(exe), str(directory)], check=True, env=env)
