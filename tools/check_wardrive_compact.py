"""Compare production SD compaction against an independent CSV/RSSI oracle."""
from collections import defaultdict
import argparse
import csv
import hashlib
import io
import json
import os
from pathlib import Path
import random
import subprocess
import sys
import tempfile
import time

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/wardrive-compact-checks"
out.mkdir(parents=True, exist_ok=True)
arguments = argparse.ArgumentParser(description=__doc__)
arguments.add_argument("--baseline-source", type=Path, help="Optional earlier compactor source for measured comparison")
arguments.add_argument("--extended", action="store_true", help="Also test a 500,000-row recording")
args = arguments.parse_args()
exe = out / "wardrive-compact-check.exe"

def build(executable, baseline=None):
    defines = [] if baseline is None else ["-DCERBERUS_COMPACT_BASELINE=1",
        f'-DCERBERUS_COMPACT_IMPLEMENTATION="{baseline.resolve(strict=True).as_posix()}"']
    subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O2", *defines,
    "-Wall", "-Wextra", "-Werror", "-UNDEBUG",
    "-I", str(root / "tools/host_checks/stubs"), "-I", str(root / "s3_main/main"),
    "-I", str(root / "shared"),
    *[str(root / "s3_main/main" / name) for name in
      ("wardrive_log.c", "wardrive_core.c", "wardrive_rssi_filter.c")],
    str(root / "tools/host_checks/wardrive_compact_check.c"), "-o", str(executable)], check=True)

build(exe)
baseline_exe = out / "wardrive-compact-baseline.exe" if args.baseline_source else None
if baseline_exe:
    build(baseline_exe, args.baseline_source)

header = ("WigleWifi-1.6,appRelease=Cerberus,model=Cerberus,release=test,device=S3-C5,"
          "display=LCD,board=Cerberus,brand=Cerberus,star=Sol,body=3,subBody=0\n"
          "MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,"
          "AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type\n")

def row(device, rssi, sample, ble=False):
    address = f"02:01:02:{device >> 16 & 255:02X}:{device >> 8 & 255:02X}:{device & 255:02X}"
    stream = io.StringIO(newline="")
    csv.writer(stream, lineterminator="\n").writerow([
        address, 'Shared, "café"', "[BLE]" if ble else "[WPA2-PSK-CCMP][ESS]",
        f"2026-11-07 12:{sample // 60 % 60:02}:{sample % 60:02}",
        0 if ble else 6, "" if ble else 2437, rssi,
        f"40.{sample % 1000000:06}", "-70.000000", "", "", "", 76 if ble else "", "BLE" if ble else "WIFI"])
    return stream.getvalue()

def verify(directory, rows, invalid=False):
    source, a, b, final, report = [directory / name for name in ("source.tmp", "a", "b", "out.csv", "stats.json")]
    source.write_bytes((header + "".join(rows)).encode())
    digest = hashlib.sha256(source.read_bytes()).digest()
    started = time.perf_counter()
    subprocess.run([str(exe), *map(str, (source, a, b, final, report))], check=True)
    compact_seconds = round(time.perf_counter() - started, 3)
    stats = json.loads(report.read_text())
    assert hashlib.sha256(source.read_bytes()).digest() == digest, "recovery source was modified"
    if invalid:
        assert stats["error"] != 0
        return
    assert stats["error"] == 0, stats
    groups = defaultdict(list)
    for index, line in enumerate(rows):
        fields = next(csv.reader([line]))
        groups[(fields[0].lower(), fields[-1])].append((int(fields[6]), index, line))
    expected = []
    for key in sorted(groups, key=lambda value: (bytes.fromhex(value[0].replace(":", "")), 1 if value[1] == "WIFI" else 2)):
        values = groups[key]
        low = min(values, key=lambda value: (value[0], value[1]))
        high = min(values, key=lambda value: (-value[0], value[1]))
        expected.extend([low[2]] if low[1] == high[1] else [value[2] for value in sorted((low, high), key=lambda value: value[1])])
    actual = final.read_bytes().decode().splitlines(keepends=True)
    assert "".join(actual[:2]) == header
    assert actual[2:] == expected, "wrong extrema, ordering or changed row/location/time"
    assert stats["input_rows"] == len(rows) and stats["output_rows"] == len(expected)
    assert stats["devices"] == len(groups)
    assert stats["input_bytes"] == source.stat().st_size and stats["output_bytes"] == final.stat().st_size
    if len(rows) > 512:
        assert stats["yields"] > 1
    original = final.read_bytes()
    assert stats["final_refs"] == stats["output_rows"] <= stats["initial_refs"] <= stats["input_rows"]
    assert stats["workspace_bytes"] == 10352, "compaction must retain its existing workspace size"
    baseline_stats = None
    if baseline_exe:
        baseline_started = time.perf_counter()
        subprocess.run([str(baseline_exe), *map(str, (source, a, b, final, report))], check=True)
        baseline_stats = json.loads(report.read_text())
        baseline_stats["host_seconds"] = round(time.perf_counter() - baseline_started, 3)
        assert baseline_stats["error"] == 0 and final.read_bytes() == original, "changed CSV relative to the earlier compactor"
        assert hashlib.sha256(source.read_bytes()).digest() == digest
    variants = {}
    for mode in ("filter", "no-cache"):
        subprocess.run([str(exe), *map(str, (source, a, b, final, report)), mode], check=True)
        filtered = json.loads(report.read_text())
        assert filtered["error"] == 0 and final.read_bytes() == original, "filter changed final CSV"
        assert filtered["devices"] == stats["devices"] and filtered["output_rows"] == stats["output_rows"]
        assert filtered["received"] == len(rows)
        assert filtered["input_rows"] + filtered["filtered"] == len(rows)
        assert filtered["filter_bytes"] <= 5120
        if mode == "no-cache":
            assert filtered["filtered"] == 0 and filtered["input_bytes"] == stats["input_bytes"]
        assert hashlib.sha256(source.read_bytes()).digest() == digest
        variants[mode] = filtered
    return {**stats, "host_seconds": compact_seconds, "capture_filter": variants["filter"], "baseline": baseline_stats}

def verify_parts(directory, parts, different_header=False):
    rows = [line for part in parts for line in part]
    verify(directory, rows)
    expected = (directory / "out.csv").read_bytes()
    sources = [directory / f"part-{i}.tmp" for i in range(len(parts))]
    for i, (path, lines) in enumerate(zip(sources, parts)):
        preamble = header.replace("release=test", "release=oops") if different_header and i else header
        path.write_bytes((preamble + "".join(lines)).encode())
    hashes = [hashlib.sha256(path.read_bytes()).digest() for path in sources]
    a, b, final, report = [directory / name for name in ("a", "b", "out.csv", "stats.json")]
    subprocess.run([str(exe), *map(str, (sources[0], a, b, final, report)), "parts", *map(str, sources[1:])], check=True)
    stats = json.loads(report.read_text())
    assert hashes == [hashlib.sha256(path.read_bytes()).digest() for path in sources]
    if different_header or any(not part for part in parts):
        assert stats["error"] != 0
    else:
        assert stats["error"] == 0 and final.read_bytes() == expected, "cross-journal extrema/ties changed"
        assert stats["input_parts"] == len(parts) and stats["input_rows"] == len(rows)
        assert stats["workspace_bytes"] == 10352

rng = random.Random(147)
with tempfile.TemporaryDirectory(dir=out) as temporary:
    directory = Path(temporary)
    verify(directory, [row(1, -45, 1)])
    verify(directory, [row(1, rssi, i) for i, rssi in enumerate((-70, -50, -90, -50, -90, -40, -60))])
    repeats = verify(directory, [row(1, -50, i) for i in range(1025)])
    assert repeats["capture_filter"]["filtered"] == 1024
    verify(directory, [row(i, -50, i) for i in range(1025)])
    verify(directory, [row(1, -50, 1), row(1, -50, 2, True), row(2, -40, 3)])
    verify(directory, [row(1, rssi, i) for i, rssi in enumerate(range(-127, 1))])
    verify(directory, [row(1, rssi, i) for i, rssi in enumerate(range(0, -128, -1))])
    # Revisit evicted identities with equal extremes and changed names/channels.
    rows = [row(i, -60, i) for i in range(2048)]
    rows += [row(i, -60, i + 2048).replace('"Shared, ""café"""', 'Changed').replace(',6,2437,', ',11,2462,')
             for i in range(2048)]
    rows += [row(i, -90 if i % 2 else -30, i + 4096) for i in range(2048)]
    verify(directory, rows)
    resident_rng = random.Random(151)
    resident = verify(directory, [row(resident_rng.randrange(64), -resident_rng.randrange(20, 100), i)
                                  for i in range(100000)])
    assert resident["capture_filter"]["filtered"] > 90000
    assert resident["index_read_bytes"] + resident["index_write_bytes"] < 1_800_000
    for count in (511, 512, 513, 1024, 1025, 8193):
        verify(directory, [row(rng.randrange(400), -rng.randrange(20, 100), i, i % 3 == 0) for i in range(count)])
    # Bounded workspace with more identities than can fit in an in-memory map.
    benchmark = verify(directory, [row(rng.randrange(10000), -rng.randrange(20, 100), i, i % 4 == 0) for i in range(100000)])
    unique = verify(directory, [row(i, -50, i) for i in range(100000)])
    assert unique["input_rows"] == unique["output_rows"] == 100000
    assert unique["index_read_bytes"] + unique["index_write_bytes"] < 28_840_000
    extended = None
    if args.extended:
        extended = verify(directory, [row((i * 31337) % 128, -20 - ((i * 37) % 80), i) for i in range(500000)])
    # Equal offsets in different journals remain different capture positions;
    # ties retain the earlier journal, even across multiple merge passes.
    verify_parts(directory, [[row(1, -90, 0)], [row(1, -20, 1)]])
    verify_parts(directory, [[row(1, -50, 0)], [row(1, -50, 1)]])
    verify_parts(directory, [[row(rng.randrange(200), -rng.randrange(20, 100), i * 600 + j, j % 3 == 0)
                             for j in range(511 + i % 3)] for i in range(17)])
    verify_parts(directory, [[row(1, -50, 0)], [row(2, -30, 1)]], different_header=True)
    verify_parts(directory, [[row(1, -50, 0)], []])
    verify(directory, [row(1, -50, 1).rstrip("\n")], invalid=True)
    verify(directory, [row(1, -128, 1)], invalid=True)
    verify(directory, [], invalid=True)
(out / "summary.json").write_text(json.dumps({"many_identities": benchmark, "resident_identities": resident,
    "all_unique": unique, "extended": extended}, indent=2) + "\n")
print(f"Wardrive extrema: {21 if args.extended else 20} single-journal and 5 multi-journal cases passed; filtered/fallback CSVs match the independent oracle byte for byte; "
      f"100000 rows, {benchmark['devices']} identities, "
      f"{benchmark['output_rows']} retained; {benchmark['workspace_bytes']} bytes workspace. "
      "Host timing is not SD/hardware acceptance.")
