"""Compile the actual streaming saved-log parser; exercise independent CSV fixtures."""
from pathlib import Path
import csv
import io
import json
import os
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "artifacts/wardrive-logs-checks"
OUT.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(OUT / "zig-global"),
           ZIG_LOCAL_CACHE_DIR=str(OUT / "zig-local"))
exe = OUT / ("wardrive-log-check.exe" if os.name == "nt" else "wardrive-log-check")
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
    "-I", str(ROOT / "s3_main/main"), "-I", str(ROOT / "shared"),
    str(ROOT / "tools/host_checks/wardrive_log_check.c"), str(ROOT / "s3_main/main/wardrive_log.c"),
    "-o", str(exe)], check=True, env=env)

META = "WigleWifi-1.6,appRelease=fixture,model=Cerberus,release=fixture,device=Cerberus,display=ST7796,board=ESP32-S3-C5,brand=Cerberus,star=Sol,body=3,subBody=0"
COLUMNS = "MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type"
WIFI = ["02:00:00:00:00:01", 'Lab, "quoted" café', "WPA2", "2026-11-04 12:00:00", "6", "2437", "-55", "12.5", "-45.5", "100", "", "", "", "WIFI"]
BLE = ["02:00:00:00:00:02", "", "[BLE]", "2026-11-04 12:00:01", "0", "", "-70", "12.5", "-45.5", "", "", "", "76", "BLE"]

def document(rows, metadata=META, newline="\n"):
    stream = io.StringIO(newline="")
    stream.write(metadata + newline + COLUMNS + newline)
    writer = csv.writer(stream, lineterminator=newline)
    writer.writerows(rows)
    return stream.getvalue().encode("utf-8")

counter = 0
def check(data, valid, error=None, blocks=(1, 7, 4096)):
    global counter
    path = OUT / "fixture.csv"
    path.write_bytes(data)
    results = []
    for size in blocks:
        r = subprocess.run([str(exe), str(path), str(size)], check=True, capture_output=True, text=True, env=env)
        parsed = json.loads(r.stdout)
        assert parsed["valid"] is valid, parsed
        if error is not None:
            assert parsed["error"] == error, parsed
        results.append(parsed)
        counter += 1
    assert all(r == results[0] for r in results), results
    return results[0]

rows = [WIFI] * 1000 + [BLE]
five = WIFI.copy(); five[4:6] = ["149", "5745"]
rows.append(five)
r = check(document(rows), True)
assert (r["rows"], r["wifi_24"], r["wifi_5"], r["ble"]) == (1002, 1000, 1, 1)
assert r["missing_accuracy"] == 1002 and r["missing_altitude"] == 1
assert (r["first_utc"], r["last_utc"]) == (WIFI[3], BLE[3])
assert not r["legacy"]
check(document([WIFI, BLE], newline="\r\n"), True)
assert check(document([WIFI], META.split(",star=")[0]), True)["legacy"]
check(document([WIFI], META + ",brand=duplicate"), False, 4)
check(document([WIFI], META.replace("body=3", "body=4")), False, 4)
check(document([WIFI], META.replace("device=Cerberus,", "")), False, 4)
check(document([]), False, 8)
check(document([WIFI])[:-1], False, 7)
check(document([WIFI]) + b"\n", False, 6)
check(document([WIFI]).replace(b"Channel,Frequency", b"Channel,Other"), False, 5)
check(document([WIFI]).replace(b"caf\xc3\xa9", b"bad\xc0\xaf"), False, 3)
check(document([WIFI]).replace(b"caf\xc3\xa9", b"bad\x00"), False, 3)
check(document([WIFI]).replace(b"caf\xc3\xa9", b"bad\xc3"), False, 3)
check(document([WIFI]).replace(b"Lab,", b"Lab\n"), False, 6)
for index, bad in ((0, "02:00:00:00:00:ZZ"), (3, "2025-02-29 12:00:00"), (3, "2026-00-01 00:00:00"),
                   (3, "2026-01-01 24:00:00"), (3, "1979-01-01 12:00:00"), (4, "15"), (5, "2438"),
                   (6, "-128"), (6, "1"), (7, "NaN"), (7, "90.1"), (8, "180.1"),
                   (7, "0x1p1"), (9, "inf"), (10, "-1"), (10, "0"), (12, "76"), (13, "UNKNOWN")):
    row = WIFI.copy(); row[index] = bad
    check(document([row]), False, 6)
for index, bad in ((4, "6"), (5, "2402"), (12, "65535"), (11, "ABCDEF")):
    row = BLE.copy(); row[index] = bad
    check(document([row]), False, 6)
row = WIFI.copy(); row[1] = "x" * 1100
check(document([row]), False, 2)
row = WIFI.copy(); row[3] = "2024-02-29 12:00:00"; row[10] = "3.5"
assert check(document([row]), True)["missing_accuracy"] == 0

UNLOC_COLUMNS = "uptime_ms,session_ms,record_type,radio,address,address_type,channel,rssi_dbm,security,ssid_hex,ble_name_hex,manufacturer_id,gps_state_at_log,latest_rmc_utc,flock_oui_clue,flock_adv_level,flock_adv_facts_hex,flock_adv_sequence,flock_adv_observed_ms,flock_rsp_level,flock_rsp_facts_hex,flock_rsp_sequence,flock_rsp_observed_ms,flock_sid,requested_ssid_hex,probe_name_bits,probe_oui_clue,probe_observed_ms,probe_count,bssid,relation_ds,relation_rx_us,uplink_count,downlink_count"
unloc = [""] * 34
unloc[:6] = ["9000", "1000", "ble", "S3", BLE[0], "1"]
data = ("# Cerberus unlocated v4; RX!=UTC\n" + UNLOC_COLUMNS + "\n" + ",".join(unloc) + "\n").encode()
r = check(data, True)
assert r["kind"] == 2 and r["rows"] == 1 and r["first_utc"] == ""
check(data.replace(b"v4;", b"v99;"), False, 4)
check(data[:-2] + b"\n", False, 6)
print(f"Wardriving log parser: {counter} chunk-boundary fixture runs passed; each executable run includes 10,000 malformed-input probes.")
