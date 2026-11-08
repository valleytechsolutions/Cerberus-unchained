"""Check actual archive storage and async ownership with injected host boundaries.

The real codec and production functions execute; SD/RTOS timing and durability
are not measured by these tests. No radios or device connections are used.
"""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/ble-archive-io-checks"
out.mkdir(parents=True, exist_ok=True)
storage = (root / "s3_main/main/storage.c").read_text(encoding="utf-8")
header = (root / "s3_main/main/storage.h").read_text(encoding="utf-8")


def function(text, signature):
    start = text.index(signature)
    opening = text.index("{", start)
    depth = 0
    for end in range(opening, len(text)):
        depth += (text[end] == "{") - (text[end] == "}")
        if not depth:
            return text[start:end + 1]
    raise ValueError(signature)


constants = []
for name in ("CERBERUS_STORAGE_PATH_MAX", "CERBERUS_STORAGE_NAME_MAX",
        "CERBERUS_STORAGE_BLE_ARCHIVE_PATH", "CERBERUS_STORAGE_BLE_ARCHIVE_MAX_BYTES",
        "CERBERUS_STORAGE_AIRTAG_JSON_MAX_BYTES", "CERBERUS_STORAGE_WIFI_ARCHIVE_PATH",
        "CERBERUS_STORAGE_WIFI_ARCHIVE_MAX_BYTES", "CERBERUS_STORAGE_AP_JSON_MAX_BYTES",
        "CERBERUS_STORAGE_SSID_TEXT_MAX_BYTES", "CERBERUS_STORAGE_PROBE_REPORT_PATH",
        "CERBERUS_STORAGE_PROBE_REPORT_MAX_BYTES", "CERBERUS_STORAGE_MESH_REPORT_PATH",
        "CERBERUS_STORAGE_MESH_REPORT_MAX_BYTES", "CERBERUS_STORAGE_MESH_ARCHIVE_PATH",
        "CERBERUS_STORAGE_MESH_ARCHIVE_MAX_BYTES"):
    constants.append(re.search(r"^#define " + name + r"\s+[^\n]+", header, re.M).group(0))
end = header.index("} cerberus_storage_owner_t;") + len("} cerberus_storage_owner_t;")
constants.append(header[header.rindex("typedef enum", 0, end):end])
end = header.index("} cerberus_storage_entry_t;") + len("} cerberus_storage_entry_t;")
constants.append(header[header.rindex("typedef struct", 0, end):end])
(out / "archive_storage_types.h").write_text("\n".join(constants), encoding="utf-8")
# Reuse the immutable, independently generated v1-v5 compatibility vectors.
# They are literal historical bytes, not rewritten by the current encoder.
codec_fixture = (root / "tools/host_checks/ble_archive_check.c").read_text(encoding="utf-8")
vectors = []
for name in ("hex", "v2_hex", "v3_hex", "v4_hex", "v5_golden"):
    literal = re.search(r"static const char " + name + r"\[\]\s*=\s*(?:\"[0-9a-f]+\"\s*)+;", codec_fixture)
    if literal is None:
        raise ValueError(f"Missing fixed archive compatibility vector: {name}")
    vectors.append(literal.group(0).replace(f"char {name}[]", f"char archive_io_{name}[]", 1))
(out / "archive_compat_vectors.h").write_text("\n".join(vectors), encoding="utf-8")
parts = [function(storage, signature) for signature in (
    "static bool relative_path_is_safe(const char *relative_path)\n{",
    "static bool file_name_is_safe(const char *file_name)\n{",
    "static esp_err_t build_entry_path(\n",
    "static esp_err_t require_firmware_owner_locked(void)\n{",
    "static esp_err_t ensure_storage_mutex(void)\n{",
    "static esp_err_t take_storage_mutex(void)\n{",
    "static void release_storage_mutex(void)\n{",
    "static int compare_entries(const void *left_ptr, const void *right_ptr)\n{",
    "esp_err_t cerberus_storage_list_directory(\n",
    "esp_err_t cerberus_storage_unmount(void)\n{",
    "esp_err_t cerberus_storage_reserve_for_usb(void)\n{",
    "static esp_err_t recon_storage_absent(",
    "static esp_err_t recon_storage_directory(",
    "static bool recon_storage_flush(",
)]
for name in ("SD_BLE_ARCHIVE_FILE_LIMIT", "SD_BLE_ARCHIVE_SEARCH_LIMIT"):
    parts.append(re.search(r"^#define " + name + r"\s+[^\n]+", storage, re.M).group(0))
parts.append(re.search(r"^static unsigned sd_ble_archive_next_index[^\n]+", storage, re.M).group(0))
parts.append(re.search(r"^static unsigned sd_airtag_json_next_index[^\n]+", storage, re.M).group(0))
parts.append(re.search(r"^static unsigned sd_wifi_archive_next_index[^\n]+", storage, re.M).group(0))
for name in ("sd_ap_json_next_index", "sd_ssid_text_next_index", "sd_probe_report_next_index", "sd_mesh_report_next_index", "sd_mesh_archive_next_index"):
    parts.append(re.search(r"^static unsigned " + name + r"[^\n]+", storage, re.M).group(0))
parts += [function(storage, signature) for signature in (
    "static bool ble_archive_storage_available(",
    "esp_err_t cerberus_storage_probe_report_save(",
    "esp_err_t cerberus_storage_mesh_report_save(",
    "esp_err_t cerberus_storage_ble_archive_save(",
    "esp_err_t cerberus_storage_ble_archive_load(",
    "esp_err_t cerberus_storage_airtag_json_save(",
    "esp_err_t cerberus_storage_airtag_json_load(",
    "esp_err_t cerberus_storage_wifi_archive_save(",
    "esp_err_t cerberus_storage_wifi_archive_load(",
    "static bool mesh_archive_storage_overlap(",
    "esp_err_t cerberus_storage_mesh_archive_save(",
    "esp_err_t cerberus_storage_mesh_archive_load(",
    "static esp_err_t wifi_interchange_storage_save(",
    "static esp_err_t wifi_interchange_storage_load(",
    "esp_err_t cerberus_storage_ap_json_save(",
    "esp_err_t cerberus_storage_ap_json_load(",
    "esp_err_t cerberus_storage_ssid_text_save(",
    "esp_err_t cerberus_storage_ssid_text_load(",
)]
(out / "archive_storage_impl.h").write_text("\n\n".join(parts), encoding="utf-8")
service = (root / "s3_main/main/ble_archive_io.c").read_text(encoding="utf-8")
(out / "archive_io_impl.h").write_text(re.sub(r"^#include[^\n]*\n", "", service, flags=re.M), encoding="utf-8")
console = (root / "s3_main/main/usb_console.c").read_text(encoding="utf-8")
(out / "archive_console_impl.h").write_text(function(console, "static void print_ble_archive(void)"), encoding="utf-8")
assert 'strcmp(start, "ble_archive") == 0' in console and 'print_ble_archive();' in console
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-cache"),
           ZIG_LOCAL_CACHE_DIR=str(out / "zig-local"))
for fixture in ("ble_archive_storage_check", "ble_archive_io_check"):
    executable = out / (fixture + ".exe")
    command = [sys.executable, "-m", "ziglang", "cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-O1", "-Wall", "-Wextra", "-Werror",
        "-I", str(out), "-I", str(root / "tools/host_checks/stubs"),
        "-I", str(root / "shared"),
        "-I", str(root / "s3_main/main"), str(root / "tools/host_checks" / (fixture + ".c"))]
    command.append(str(root / "s3_main/main/ble_archive.c"))
    command.append(str(root / "s3_main/main/airtag_json.c"))
    command.append(str(root / "s3_main/main/wifi_archive.c"))
    command.append(str(root / "s3_main/main/wifi_marauder.c"))
    for source in ("s3_main/main/mesh_archive.c", "s3_main/main/mesh_report.c", "s3_main/main/mesh_scan.c",
                       "s3_main/main/mesh_locate.c", "s3_main/main/mesh_locate_report.c",
                       "s3_main/main/mesh_observation.c", "shared/mesh_mac.c"):
        command.append(str(root / source))
    if fixture == "ble_archive_io_check":
        command.append(str(root / "s3_main/main/probe_sniff_model.c"))
        command.append(str(root / "shared/probe_sniff_core.c"))
    subprocess.run([*command, "-o", str(executable)], check=True, env=env)
    with tempfile.TemporaryDirectory(prefix="files-", dir=out) as temporary:
        subprocess.run([str(executable)], cwd=temporary, check=True, env=env)
