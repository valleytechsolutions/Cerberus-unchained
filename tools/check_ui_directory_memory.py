"""Fault-check production UI directory-bank allocation and selection lifetimes.

The actual UI functions, constants, enums, and storage/script types are extracted
unchanged. Allocation, storage, drawing, and script parsing are mocked boundaries;
the filename filter, metadata filtering, navigation, and copied names are real.
Generated headers and Zig caches are private to this runner, so it can run in
parallel with the other host checks. This is not an SD or PSRAM hardware test.
"""
from pathlib import Path
import os
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/ui-directory-memory-checks"
out.mkdir(parents=True, exist_ok=True)
ui = (root / "s3_main/main/ui.c").read_text(encoding="utf-8")
storage = (root / "s3_main/main/storage.h").read_text(encoding="utf-8")
script_header = (root / "s3_main/main/script_engine.h").read_text(encoding="utf-8")
script_source = (root / "s3_main/main/script_engine.c").read_text(encoding="utf-8")


def function(name, text=ui):
    match = re.search(r"^(?:static )?[^\n]*\b" + re.escape(name) +
                      r"\([^;{]*\)\s*\n\{", text, re.M)
    assert match, name
    opening = match.end() - 1
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[match.start():end]


def typedef(name, text):
    match = re.search(r"typedef (?:struct|enum)\s*\{[^{}]*\}\s*" +
                      re.escape(name) + r";", text)
    assert match, name
    return match.group(0)


def define(name, text):
    match = re.search(r"^#define " + re.escape(name) + r"\s+[^\n]+", text, re.M)
    assert match, name
    return match.group(0)


types = [define(name, ui) for name in (
    "UI_FILE_ENTRY_CAPACITY", "UI_FILE_VISIBLE_ROWS", "UI_SCRIPT_ENTRY_CAPACITY",
    "UI_SCRIPT_VISIBLE_ROWS", "UI_SCRIPT_LIST_Y", "UI_SCRIPT_ROW_HEIGHT", "UI_LOG_PATH")]
types += [define(name, storage) for name in (
    "CERBERUS_STORAGE_NAME_MAX", "CERBERUS_STORAGE_SCRIPTS_PATH",
    "CERBERUS_STORAGE_MESH_ARCHIVE_PATH")]
types += [define(name, script_header) for name in (
    "CERBERUS_SCRIPT_EXTENSION", "CERBERUS_SCRIPT_MAX_ACTIONS", "CERBERUS_SCRIPT_TEXT_MAX")]
types += [typedef(name, storage) for name in (
    "cerberus_storage_entry_t", "cerberus_storage_status_t")]
types += [typedef(name, script_header) for name in (
    "cerberus_script_action_type_t", "cerberus_script_action_t",
    "cerberus_script_t", "cerberus_script_error_t")]
types += [typedef(name, ui) for name in (
    "ui_file_mode_t", "ui_file_hash_state_t", "ui_log_mode_t", "ui_script_mode_t")]
(out / "ui_directory_memory_types.h").write_text("\n\n".join(types) + "\n", encoding="utf-8")

parts = [function("cerberus_script_file_name_is_valid", script_source)]
parts += [function(name) for name in (
    "ui_list_directory", "refresh_file_listing", "log_report_name_is_generated",
    "log_file_name_is_system", "log_file_name_is_visible", "refresh_log_listing",
    "refresh_script_listing", "navigate_file_directory", "navigate_file_parent",
    "open_text_file", "open_image_file", "open_log_report", "handle_scripts_tap")]
(out / "ui_directory_memory_impl.h").write_text("\n\n".join(parts) + "\n", encoding="utf-8")

# All five producers must keep the original capacities and use the guarded
# pointer-to-bank wrapper. Other references are reads/copies, never realloc/free.
for bank in ("file", "log", "script"):
    assert f"static cerberus_storage_entry_t *{bank}_entries;" in ui
    assert not re.search(r"sizeof\s*\(\s*" + bank + r"_entries\s*\)", ui)
    assert not re.search(r"(?:free|realloc)\s*\([^;\n]*\b" + bank + r"_entries\b", ui)
assert ui.count("cerberus_storage_list_directory(") == 1
assert ui.count("ui_list_directory(") == 6  # Definition and five callers.
for name, bank, capacity in (
    ("refresh_file_listing", "file", "UI_FILE_ENTRY_CAPACITY"),
    ("navigate_file_directory", "file", "UI_FILE_ENTRY_CAPACITY"),
    ("navigate_file_parent", "file", "UI_FILE_ENTRY_CAPACITY"),
    ("refresh_log_listing", "log", "UI_FILE_ENTRY_CAPACITY"),
    ("refresh_script_listing", "script", "UI_SCRIPT_ENTRY_CAPACITY")):
    assert re.search(r"ui_list_directory\(\s*[^,]+,\s*&" + bank +
                     r"_entries,\s*" + capacity + r"\s*,", function(name))
assert "file_selected_entry = file_entries[index];" in function("handle_files_tap")
helper = function("ui_list_directory")
assert "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" in helper
assert "MALLOC_CAP_INTERNAL" not in helper and "xTaskCreate" not in helper
assert "heap_caps_free" not in helper and "realloc" not in helper
for name, error in (("draw_files", "storage_last_error"),
                    ("draw_log_list", "log_error"),
                    ("draw_scripts_list", "script_listing_error")):
    assert error + " == ESP_ERR_NO_MEM" in function(name)

env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-global"),
           ZIG_LOCAL_CACHE_DIR=str(out / "zig-local"))
exe = out / ("ui-directory-memory-check.exe" if os.name == "nt" else "ui-directory-memory-check")
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-O1",
    "-Wall", "-Wextra", "-Werror", "-I", str(out),
    str(root / "tools/host_checks/ui_directory_memory_check.c"), "-o", str(exe)],
    check=True, env=env)
subprocess.run([str(exe)], check=True, env=env)
