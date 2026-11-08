"""Exercise the bounded, I/O-free Recon queue and independently parse its exports."""
from pathlib import Path
import csv
import json
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/recon-journal-checks"
out.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-cache"))
exe = out / ("recon-journal-check.exe" if os.name == "nt" else "recon-journal-check")
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1",
    "-Wall", "-Wextra", "-Werror", "-I", str(root / "s3_main/main"),
    str(root / "tools/host_checks/recon_journal_check.c"),
    str(root / "s3_main/main/recon_journal.c"), "-o", str(exe)], check=True, env=env)
subprocess.run([str(exe), str(out)], check=True, env=env)


def decode_name(value):
    assert value.startswith("bytes:")
    data = bytearray()
    text = value[6:]
    index = 0
    while index < len(text):
        if text[index] != "\\":
            data.append(ord(text[index]))
            index += 1
        elif text[index + 1] == "\\":
            data.append(ord("\\"))
            index += 2
        else:
            assert text[index + 1] == "x"
            data.append(int(text[index + 2:index + 4], 16))
            index += 4
    return bytes(data)


csv_bytes = (out / "OBS.CSV").read_bytes()
assert csv_bytes.isascii()
assert csv_bytes.count(b"\r\n") == 259  # header, 256 byte probes, two domain probes
with (out / "OBS.CSV").open(encoding="ascii", newline="") as stream:
    rows = list(csv.DictReader(stream))
assert len(rows) == 258 and all(len(row) == 24 and None not in row for row in rows)
for value, row in enumerate(rows[:256]):
    assert decode_name(row["received_name_escaped"]) == bytes([value]) * 32
    assert row["classified"] == "0" and row["rule"] == ""
    assert row["source_kind"] == "unavailable" and row["source_time"] == ""
    assert row["alert_state"] == "not_evaluated"
    assert row["address"] == "EF:CD:AB:34:12:02" and row["address_type"] == "1"
wifi, ble = rows[-2:]
assert wifi["source_kind"] == "c5_age_ms_at_s3_request"
assert wifi["source_time"] == "17" and wifi["source_anchor_s3_us"] == "80"
assert wifi["s3_consumed_us"] == "100" and wifi["rssi_scope"] == "driver_aggregate"
assert wifi["rssi_dbm"] == "-45" and wifi["channel"] == "6"
assert wifi["address"] == "02:12:34:AB:CD:EF"
assert decode_name(wifi["received_name_escaped"]) == b'=,"\\\x00\n\xff'
assert ble["source_kind"] == "s3_adv_uptime_ms32" and ble["source_time"] == "0"
assert ble["source_sequence"] == "1" and ble["source_anchor_s3_us"] == ""
assert ble["rssi_scope"] == "sweep_peak" and ble["ibeacon"] == "1"
assert ble["rule"] == "beacon.ibeacon" and ble["alert_state"] == "not_queued"
assert ble["rssi_dbm"] == "" and ble["channel"] == ""
manifest = json.loads((out / "SESSION.JSON").read_text(encoding="ascii"))
assert manifest["schema"] == "cerberus_recon_completed_sweep_v1"
assert manifest["session_name"] == "R00001" and manifest["session_id"] == 9
assert manifest["state"] == "complete" and manifest["gps"] is False
assert manifest["accepted"] == manifest["written"] == 258
assert manifest["queued"] == manifest["abandoned"] == 0
assert manifest["counter_lower_bounds"] is False
assert manifest["row_counter_version"] == 1
assert "journal rows including repeats" in manifest["row_counter_scope"]
assert "saturated differences are not exact" in manifest["row_counter_scope"]
for kind in ("wifi", "ble", "ibeacon"):
    observed = sum(row["radio"] == kind for row in rows) if kind != "ibeacon" else sum(
        row["radio"] == "ble" and row["ibeacon"] == "1" for row in rows)
    assert manifest[f"accepted_{kind}_rows"] == manifest[f"written_{kind}_rows"] == observed
# Repeated addresses are rows, not a claim of unique devices.
assert manifest["written_ble_rows"] == 257 and len({row["address"] for row in rows if row["radio"] == "ble"}) == 1
assert manifest["observations"] == "OBS.CSV"
assert "not RF time" in manifest["time_basis"]
assert "not every RF callback" in manifest["coverage"]
for filename, state, queued, abandoned in (("ACTIVE.JSON", "active", 3, 0), ("ERROR.JSON", "error", 0, 3)):
    partial = json.loads((out / filename).read_text(encoding="ascii"))
    assert partial["state"] == state and partial["queued"] == queued and partial["abandoned"] == abandoned
    assert partial["accepted"] == 6 and partial["written"] == 3
    for kind, accepted, written in (("wifi", 2, 1), ("ble", 4, 2), ("ibeacon", 2, 1)):
        assert partial[f"accepted_{kind}_rows"] == accepted and partial[f"written_{kind}_rows"] == written
    assert partial["counter_lower_bounds"] is False
reset = json.loads((out / "RESET.JSON").read_text(encoding="ascii"))
assert reset["session_id"] == 10 and reset["accepted"] == reset["written"] == reset["abandoned"] == 0
assert all(reset[f"{phase}_{kind}_rows"] == 0 for phase in ("accepted", "written") for kind in ("wifi", "ble", "ibeacon"))
print("Recon CSV/JSON independently parsed: byte round trip, typed repeated rows, partial-write accounting, restart and time/identity scope passed.")
