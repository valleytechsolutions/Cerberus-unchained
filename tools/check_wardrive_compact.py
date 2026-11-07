"""Compare production SD compaction against an independent CSV/RSSI oracle."""
from collections import defaultdict
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
exe = out / "wardrive-compact-check.exe"
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O2",
    "-Wall", "-Wextra", "-Werror", "-UNDEBUG",
    "-I", str(root / "tools/host_checks/stubs"), "-I", str(root / "s3_main/main"),
    "-I", str(root / "shared"),
    *[str(root / "s3_main/main" / name) for name in
      ("wardrive_compact.c", "wardrive_log.c", "wardrive_core.c")],
    str(root / "tools/host_checks/wardrive_compact_check.c"), "-o", str(exe)], check=True)

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
    for values in groups.values():
        low = min(values, key=lambda value: (value[0], value[1]))
        high = min(values, key=lambda value: (-value[0], value[1]))
        expected.extend([low[2]] if low[1] == high[1] else [low[2], high[2]])
    actual = final.read_bytes().decode().splitlines(keepends=True)
    assert "".join(actual[:2]) == header
    assert sorted(actual[2:]) == sorted(expected), "wrong extrema or changed row/location/time"
    assert stats["input_rows"] == len(rows) and stats["output_rows"] == len(expected)
    assert stats["devices"] == len(groups)
    assert stats["input_bytes"] == source.stat().st_size and stats["output_bytes"] == final.stat().st_size
    if len(rows) > 512:
        assert stats["yields"] > 1
    return {**stats, "host_seconds": round(time.perf_counter() - started, 3)}

rng = random.Random(147)
with tempfile.TemporaryDirectory(dir=out) as temporary:
    directory = Path(temporary)
    verify(directory, [row(1, -45, 1)])
    verify(directory, [row(1, rssi, i) for i, rssi in enumerate((-70, -50, -90, -50, -90, -40, -60))])
    verify(directory, [row(1, -50, i) for i in range(1025)])
    verify(directory, [row(i, -50, i) for i in range(1025)])
    verify(directory, [row(1, -50, 1), row(1, -50, 2, True), row(2, -40, 3)])
    for count in (511, 512, 513, 1024, 1025, 8193):
        verify(directory, [row(rng.randrange(400), -rng.randrange(20, 100), i, i % 3 == 0) for i in range(count)])
    # Bounded workspace with more identities than can fit in an in-memory map.
    benchmark = verify(directory, [row(rng.randrange(10000), -rng.randrange(20, 100), i, i % 4 == 0) for i in range(100000)])
    verify(directory, [row(1, -50, 1).rstrip("\n")], invalid=True)
    verify(directory, [row(1, -128, 1)], invalid=True)
    verify(directory, [], invalid=True)
(out / "summary.json").write_text(json.dumps(benchmark, indent=2) + "\n")
print(f"Wardrive extrema: 15 real-file cases passed; 100000 rows, {benchmark['devices']} identities, "
      f"{benchmark['output_rows']} retained; {benchmark['workspace_bytes']} bytes workspace. "
      "Host timing is not SD/hardware acceptance.")
