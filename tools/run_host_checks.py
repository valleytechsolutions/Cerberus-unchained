#!/usr/bin/env python3
"""Run the 169 registered host suites through integrated Wardriving uploads.

The checked-in synthetic fixtures need no board, radio, or private captures.
Generated fixtures, native executables, previews, logs, and the result receipt
remain in artifacts/. UI suites with shared fixture headers run serially.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
import hashlib
import importlib.metadata
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "artifacts" / "host-checks"

CHECKS = (
    'check_wardrive_logs.py',
    'check_wardrive_upload_config.py',
    'check_wardrive_upload_protocol.py',
    'check_wardrive_log_manager.py',
    'check_wardrive_upload_net.py',
    'check_usb_console.py',
    'check_ui_directory_memory.py',
    'check_s3_wifi_scanner.py',
    'check_s3_ble_host_memory.py',
    'check_radio.py',
    'check_radio_worker.py',
    'check_mesh_mac.py',
    'check_mesh_observation.py',
    'check_mesh_link.py',
    'check_mesh_scan.py',
    'check_mesh_bridge.py',
    'check_mesh_report.py',
    'check_mesh_archive.py',
    'check_wifi_packet_count.py',
    'check_wifi_packet_count_native.py',
    'check_s3_wifi_packet_count.py',
    'check_wifi_packet_count_scan.py',
    'check_wifi_packet_count_record.py',
    'check_wifi_packet_count_ui.py',
    'check_wifi_packet_count_diagnostics.py',
    'check_mesh_ui.py',
    'check_mesh_locate.py',
    'check_mesh_locate_link.py',
    'check_mesh_locate_report.py',
    'check_mesh_locate_bridge.py',
    'check_s3_ble_attack.py',
    'check_c5_ble_lifecycle.py',
    'check_channel_survey_lifecycle.py',
    'check_probe_sniff.py',
    'check_wifi_scan_band.py',
    'check_dual_wifi_scan.py',
    'check_dual_wifi_diagnostics.py',
    'check_dual_wifi_ui.py',
    'check_dual_wifi_live.py',
    'check_dual_wifi_routing.py',
    'check_dual_ble.py',
    'check_dual_ble_family.py',
    'check_dual_ble_diagnostics.py',
    'check_dual_ble_firmware_cache.py',
    'check_wardrive_recent.py',
    'check_wardrive_recent_service.py',
    'check_ble_archive.py',
    'check_airtag_json.py',
    'check_wifi_archive.py',
    'check_wifi_marauder.py',
    'check_ble_archive_list.py',
    'check_ble_archive_io.py',
    'check_recon_inventory.py',
    'check_recon_journal.py',
    'check_recon_journal_storage.py',
    'check_recon_recorder.py',
    'check_recon_parallel.py',
    'check_report_storage.py',
    'check_c5_ready_handshake.py',
    'check_storage_file_copy.py',
    'check_s3_wifi_list.py',
    'check_sentinel_baseline.py',
    'check_s3_wifi_ssid_analysis.py',
    'check_wifi_readout_diagnostics.py',
    'check_wifi_scan_cache.py',
    'check_wifi_scan_incremental.py',
    'check_wifi_relation_observation.py',
    'check_s3_wifi_relation.py',
    'check_wifi_relation_transport.py',
    'check_s3_ble_discovery_session.py',
    'check_s3_ble_discovery_inventory.py',
    'check_ble_discovery_list.py',
    'check_mac_observation.py',
    'check_mac_capture.py',
    'check_mac_monitor_transport.py',
    'check_mac_monitor_inventory.py',
    'check_mac_monitor_session.py',
    'check_csi_motion_model.py',
    'check_csi_capture.py',
    'check_csi_transport.py',
    'check_csi_session.py',
    'check_station_service.py',
    'check_wifi_capture_radio.py',
    'check_s3_ble_central.py',
    'check_s3_ble_connection_ui.py',
    'check_wifi_probe_observation.py',
    'check_wifi_probe_transport.py',
    'check_ble_vendor_cache.py',
    'check_wardrive_ble_capture.py',
    'check_wardrive_core.py',
    'check_wardrive_stream.py',
    'check_wardrive_radio.py',
    'check_wardrive_log_routing.py',
    'check_wardrive_memory.py',
    'check_wardrive_stop.py',
    'check_wardrive_storage.py',
    'check_wardrive_compact.py',
    'check_wardrive_wigle.py',
    'check_wardrive_gps_optional.py',
    'check_wardrive_gps_core.py',
    'check_wardrive_gps_uart.py',
    'check_gps_gpx.py',
    'check_gps_recorder.py',
    'check_wardrive_expansion.py',
    'check_wardrive_expansion_activation.py',
    'check_recon_store.py',
    'check_s3_ble_recent.py',
    'check_remote_id_inventory.py',
    'check_remote_id_receive.py',
    'check_remote_id_c5_receive.py',
    'check_watch_receive_timing.py',
    'check_watch_capture_timing.py',
    'check_watch_receive_transport.py',
    'check_watch_motion.py',
    'check_remote_id_channel_plan.py',
    'check_remote_id_transport.py',
    'check_remote_id_priority.py',
    'check_remote_id_session.py',
    'check_remote_id_ui.py',
    'check_remote_id_live_ui.py',
    'check_remote_id_diagnostics.py',
    'render_field_menu.py',
    'check_wifi_scan_observation.py',
    'check_s3_wifi_observation.py',
    'check_wifi_seen_console.py',
    'check_wifi_packet_monitor_worker.py',
    'check_wardrive_wifi_capture.py',
    'check_ble_flock_hints.py',
    'check_ibeacon.py',
    'check_ble_ad_hints.py',
    'check_ble_analyzer.py',
    'check_ble_flipper_hints.py',
    'check_ble_findmy_hints.py',
    'check_s3_ble_snapshot.py',
    'check_s3_ble_events.py',
    'check_s3_findmy_monitor.py',
    'check_s3_ble_tracker.py',
    'check_s3_ble_list.py',
    'check_s3_ble_live_routes.py',
    'check_scan_display_store.py',
    'check_s3_findmy_ui_route.py',
    'check_ble_discovery_console.py',
)

SERIAL_CHECKS = (
    'check_probe_sniff_ui.py',
    'check_dual_ble_ui.py',
    'check_wardrive_recent_ui.py',
    'check_wardrive_recent_live.py',
    'check_ble_archive_ui.py',
    'check_ble_archive_live.py',
    'check_airtag_json_ui.py',
    'check_wifi_archive_ui.py',
    'check_wifi_interchange_ui.py',
    'check_recon_inventory_ui.py',
    'check_recon_recorder_ui.py',
    'render_recon.py',
    'check_wifi_owned_poll_ui.py',
    'check_wifi_baseline_ui.py',
    'check_wifi_ssid_analysis_ui.py',
    'check_wifi_relation_ui.py',
    'check_s3_ble_discovery_live.py',
    'check_mac_monitor_ui.py',
    'check_mac_monitor_live_ui.py',
    'check_csi_ui.py',
    'check_csi_live_ui.py',
    'render_wifi_target.py',
    'check_wardrive_ui.py',
    'check_wardrive_upload_ui.py',
    'check_wardrive_gps_ui.py',
    'check_s3_ble_discovery_ui.py',
    'render_network_picker.py',
)


def registered_checks() -> tuple[str, ...]:
    names = (*CHECKS, *SERIAL_CHECKS)
    if len(names) != 169 or len(set(names)) != 169:
        raise ValueError("Expected 169 unique suites; do not silently skip a suite")
    return names


def registry_sha256() -> str:
    # Bind both the canonical order and the serial/parallel execution boundary.
    registered_checks()
    value = {"checks": CHECKS, "serial_checks": SERIAL_CHECKS}
    return hashlib.sha256(json.dumps(value, sort_keys=True,
        separators=(",", ":")).encode("utf-8")).hexdigest()


def selected_checks(shard_index: int | None = None,
                    shard_count: int | None = None) -> tuple[str, ...]:
    names = registered_checks()
    if shard_index is None and shard_count is None:
        return names
    if (type(shard_index) is not int or type(shard_count) is not int or
            not 1 <= shard_count <= len(names) or not 0 <= shard_index < shard_count):
        raise ValueError("Supply both --shard-index and --shard-count; index is zero-based and count is 1..169")
    return names[shard_index::shard_count]


def source_head() -> str | None:
    try:
        result = subprocess.run(["git", "rev-parse", "--verify", "HEAD"], cwd=ROOT,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=True)
    except (OSError, subprocess.CalledProcessError):
        return None
    value = result.stdout.strip()
    return value if len(value) == 40 and all(c in "0123456789abcdef" for c in value) else None


def source_clean() -> bool:
    try:
        result = subprocess.run(["git", "status", "--porcelain", "--untracked-files=normal"], cwd=ROOT,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=True)
    except (OSError, subprocess.CalledProcessError):
        return False
    return not result.stdout.strip()


def execution_context() -> dict:
    run_id, attempt = os.environ.get("GITHUB_RUN_ID"), os.environ.get("GITHUB_RUN_ATTEMPT")
    if run_id is None and attempt is None:
        return {"kind": "local"}
    if any(not isinstance(value, str) or not value.isascii() or not value.isdecimal() or
           value.startswith("0") for value in (run_id, attempt)):
        raise ValueError("GitHub execution requires both valid GITHUB_RUN_ID and GITHUB_RUN_ATTEMPT")
    return {"kind": "github_actions", "run_id": run_id, "run_attempt": attempt}


def run(name: str) -> dict:
    # Some older harnesses inherit Zig's shared default cache. Give each suite
    # independent cache roots while preserving each harness's own overrides.
    cache = OUTPUT / "caches" / name.removesuffix(".py")
    overrides = {"ZIG_GLOBAL_CACHE_DIR": str(cache / "global"),
                 "ZIG_LOCAL_CACHE_DIR": str(cache / "local")}
    for directory in overrides.values():
        Path(directory).mkdir(parents=True, exist_ok=True)
    started = time.perf_counter()
    result = subprocess.run(
        [sys.executable, str(ROOT / "tools" / name)], cwd=ROOT,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        env=dict(os.environ, **overrides),
    )
    path = OUTPUT / (name.removesuffix(".py") + ".log")
    path.write_bytes(result.stdout)
    print(f"{name}: {'pass' if result.returncode == 0 else 'FAIL'}", flush=True)
    if result.returncode:
        print(result.stdout[-3000:].decode("utf-8", errors="replace"), flush=True)
    return {
        "check": name, "exit_code": result.returncode,
        "duration_seconds": round(time.perf_counter() - started, 6),
        "log": path.relative_to(ROOT).as_posix(),
        "log_sha256": hashlib.sha256(result.stdout).hexdigest(),
        "cache_roots": {key: Path(value).relative_to(ROOT).as_posix() for key, value in overrides.items()},
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--list", action="store_true", help="List suites without running them")
    parser.add_argument("--jobs", type=int, default=3, help="Parallel compiler jobs (default: 3)")
    parser.add_argument("--shard-index", type=int, help="Zero-based shard index; omitted runs the full registry")
    parser.add_argument("--shard-count", type=int, help="Number of isolated shards; requires --shard-index")
    args = parser.parse_args()
    try:
        names = registered_checks()
        selected = selected_checks(args.shard_index, args.shard_count)
        execution = execution_context()
    except ValueError as error:
        parser.error(str(error))
    if args.list:
        for name in selected:
            print(name)
        return 0
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    missing = [name for name in names if not (ROOT / "tools" / name).is_file()]
    if missing:
        parser.error("Missing suites: " + ", ".join(missing))
    if importlib.util.find_spec("ziglang") is None:
        parser.error("Install the host compiler first: python -m pip install ziglang==0.14.1")
    version = importlib.metadata.version("ziglang")
    if version != "0.14.1":
        parser.error("Expected ziglang==0.14.1; install the pinned host compiler")
    from build_unified import inputs

    head = source_head()
    if args.shard_count is not None and head is None:
        parser.error("Sharded runs require an identifiable Git HEAD")
    clean = source_clean() if args.shard_count is not None else None
    if args.shard_count is not None and not clean:
        parser.error("Sharded runs require a clean Git checkout; use the default full run for local changes")
    OUTPUT.mkdir(parents=True, exist_ok=True)
    path = OUTPUT / "host-checks.json"
    # A cancelled run must not leave an earlier successful receipt behind.
    path.unlink(missing_ok=True)
    before = inputs()
    chosen = set(selected)
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        results = list(pool.map(run, (name for name in CHECKS if name in chosen)))
    # These harnesses create shared generated UI fixture headers.
    results.extend(run(name) for name in SERIAL_CHECKS if name in chosen)
    after = inputs()
    head_after = source_head()
    clean_after = source_clean() if args.shard_count is not None else None
    changed = sorted(k for k in before.keys() | after.keys() if before.get(k) != after.get(k))
    receipt = {
        "format": 1,
        "suite_set": "rev144",
        "recorded_utc": datetime.now(timezone.utc).isoformat(),
        "status": "passed" if not changed and head == head_after and
            (args.shard_count is None or clean_after) and
            tuple(r["check"] for r in results) == selected and
            all(r["exit_code"] == 0 for r in results) else "failed",
        "hardware_validated": False,
        "platform": sys.platform,
        "source_head": head,
        "source_head_after": head_after,
        "source_clean": clean,
        "source_clean_after": clean_after,
        "execution": execution,
        "registry_sha256": registry_sha256(),
        "registered_suite_count": len(names),
        "selection": {
            "mode": "full" if args.shard_count is None else "shard",
            "shard_index": args.shard_index,
            "shard_count": args.shard_count if args.shard_count is not None else 1,
            "selected_names": list(selected),
            "selected_suite_count": len(selected),
        },
        "python_version": sys.version.split()[0],
        "ziglang_version": importlib.metadata.version("ziglang"),
        "inputs_changed": changed,
        "inputs_after": after,
        "checks": results,
    }
    path.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print(f"{receipt['status']}: {len(results)} suites; receipt: {path}", flush=True)
    if changed:
        print("Source/configuration inputs changed during validation; rerun on stable inputs.")
    if head != head_after:
        print("Git HEAD changed during validation; rerun on one source revision.")
    if args.shard_count is not None and not clean_after:
        print("Git checkout became dirty during shard execution; rerun on clean source.")
    return 0 if receipt["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
