"""Fault-test the actual Recon journal lifecycle with real temporary file data.

Only the filesystem/RTOS boundaries are wrapped. Host sync results are injected;
this is not physical SD durability, power-loss or concurrent hardware evidence.
"""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/recon-journal-storage-checks"
out.mkdir(parents=True, exist_ok=True)
source = (root / "s3_main/main/storage.c").read_text(encoding="utf-8")
header = (root / "s3_main/main/storage.h").read_text(encoding="utf-8")


def function(signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for end in range(opening, len(source)):
        depth += (source[end] == "{") - (source[end] == "}")
        if not depth:
            return source[start:end + 1]
    raise ValueError(signature)


constants = []
for text, names in ((header, ("CERBERUS_STORAGE_NAME_MAX", "CERBERUS_STORAGE_PATH_MAX",
        "CERBERUS_STORAGE_RECON_PATH", "CERBERUS_STORAGE_RECON_ROW_MAX",
        "CERBERUS_STORAGE_RECON_MANIFEST_MAX", "CERBERUS_STORAGE_RECON_MAX_BYTES",
        "CERBERUS_STORAGE_RECON_NAME_CAPACITY", "CERBERUS_STORAGE_WARDRIVE_PATH",
        "CERBERUS_STORAGE_PACKET_COUNT_PATH", "CERBERUS_STORAGE_WIFI_CAPTURE_MAX_BYTES",
        "CERBERUS_STORAGE_GPS_NAME_CAPACITY")),
        (source, ("SD_HASH_BUFFER_SIZE", "SD_WIFI_CAPTURE_FILE_LIMIT", "SD_WARDRIVE_FILE_LIMIT"))):
    constants += [re.search(r"^#define " + n + r"\s+[^\n]+", text, re.M).group(0) for n in names]
end = header.index("} cerberus_storage_owner_t;") + len("} cerberus_storage_owner_t;")
constants.append(header[header.rindex("typedef enum", 0, end):end])
end = header.index("} cerberus_storage_packet_count_result_t;") + len("} cerberus_storage_packet_count_result_t;")
constants.append(header[header.rindex("typedef struct", 0, end):end])
for name in ("sd_hash_buffer", "sd_operation_source_path", "sd_operation_destination_path",
        "sd_operation_temp_path", "sd_wifi_capture_temp_path", "sd_wifi_capture_final_path",
        "sd_wifi_capture_name"):
    constants.append(re.search(r"^static [^\n]*\b" + name + r"\[[^\n]+;", source, re.M).group(0))
start = source.index("typedef struct {", source.index("/* Fixed Wardriving paths"))
end = source.index("static void release_sd_bus(void);", start)
constants.append(source[start:end])
(out / "recon_storage_types.h").write_text("\n".join(constants), encoding="utf-8")
signatures = (
    "static bool relative_path_is_safe(const char *relative_path)\n{",
    "static bool file_name_is_safe(const char *file_name)\n{",
    "static esp_err_t build_entry_path(\n",
    "static esp_err_t require_firmware_owner_locked(void)\n{",
    "static bool ble_archive_storage_available(void)\n{",
    "static esp_err_t ensure_storage_mutex(void)\n{",
    "static esp_err_t take_storage_mutex(void)\n{",
    "static void release_storage_mutex(void)\n{",
    "static esp_err_t ensure_directory_exists(const char *path)\n{",
    "static bool entry_is_protected_system_file(const char *relative_path, const char *name)\n{",
    "esp_err_t cerberus_storage_unmount(void)\n{",
    "esp_err_t cerberus_storage_reserve_for_usb(void)\n{",
    "esp_err_t cerberus_storage_wifi_capture_begin(",
    "static esp_err_t wardrive_stream_begin(",
    "esp_err_t cerberus_storage_wardrive_session_begin(void)\n{",
)
parts = [function(s) for s in signatures]
start = source.index("static esp_err_t recon_storage_path(")
end = source.index("esp_err_t cerberus_storage_create_directory(", start)
parts.append(source[start:end])
parts += [function(s) for s in (
    "esp_err_t cerberus_storage_create_directory(",
    "esp_err_t cerberus_storage_create_empty_file(",
    "esp_err_t cerberus_storage_copy_file(\n",
    "esp_err_t cerberus_storage_move_entry(\n",
    "esp_err_t cerberus_storage_delete_entry(",
)]
(out / "recon_storage_impl.h").write_text("\n\n".join(parts), encoding="utf-8")
exe = out / ("recon-journal-storage-check.exe" if os.name == "nt" else "recon-journal-storage-check")
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-cache"))
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-O1",
    "-Wall", "-Wextra", "-Werror", "-I", str(out),
    "-I", str(root / "tools/host_checks/stubs"), "-I", str(root / "s3_main/main"),
    str(root / "tools/host_checks/recon_journal_storage_check.c"), "-o", str(exe)],
    check=True, env=env)
with tempfile.TemporaryDirectory(prefix="files-", dir=out) as temporary:
    directory = Path(temporary).resolve()
    assert directory.is_relative_to(out.resolve())
    subprocess.run([str(exe)], cwd=directory, check=True, env=env)
