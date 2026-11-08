"""Read-only local checks for Cerberus WiGLE 1.6 exports; never uploads or edits.

Reference: https://api.wigle.net/csvFormat-1_6.html
A local pass establishes structure/field consistency, not server acceptance,
GPS accuracy, RF completeness, or whether a service will award observations.
Output intentionally omits MACs, SSIDs, coordinates and credential values.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import re
from collections import Counter
from datetime import datetime, timedelta, timezone
from pathlib import Path

COLUMNS = (
    "MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,"
    "CurrentLongitude,AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type"
).split(",")
PREHEADER_KEYS = (
    "appRelease", "model", "release", "device", "display", "board", "brand",
    "star", "body", "subBody",
)
MAC = re.compile(r"(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}\Z")
STAMP = re.compile(r"\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\Z")
INTEGER = re.compile(r"-?\d+\Z")


def wifi_frequency(channel: int) -> int | None:
    if 1 <= channel <= 13:
        return 2407 + 5 * channel
    if channel == 14:
        return 2484
    if (36 <= channel <= 64 or 100 <= channel <= 144) and channel % 4 == 0:
        return 5000 + 5 * channel
    if 149 <= channel <= 165 and (channel - 149) % 4 == 0:
        return 5000 + 5 * channel
    return None


def validate(path: Path, *, now: datetime | None = None) -> dict:
    """Streaming validation, bounded error samples; never rewrites/deduplicates."""
    path = Path(path)
    now = now or datetime.now(timezone.utc)
    errors: Counter = Counter()
    warnings: Counter = Counter()
    types: Counter = Counter()
    samples: list[dict] = []
    total = 0
    digest = hashlib.sha256()
    last_byte = b""
    initial = path.stat()

    def error(code: str, line: int) -> None:
        errors[code] += 1
        if len(samples) < 16:
            samples.append({"line": line, "code": code})

    def integer(value: str) -> int:
        if not INTEGER.fullmatch(value):
            raise ValueError
        return int(value)

    def number(value: str) -> float:
        value = float(value)
        if not math.isfinite(value):
            raise ValueError
        return value

    def lines(source):
        nonlocal last_byte
        for line in source:
            digest.update(line)
            last_byte = line[-1:]
            yield line.decode("utf-8", errors="strict")

    with path.open("rb") as source:
        reader = csv.reader(lines(source), strict=True)
        try:
            metadata = next(reader, [])
            if not metadata or metadata[0] != "WigleWifi-1.6":
                error("not_wigle_1_6", 1)
            values = {}
            for field in metadata[1:]:
                key, separator, value = field.partition("=")
                if not separator or key in values:
                    error("invalid_preheader_field", 1)
                values[key] = value
            if any(key not in values for key in PREHEADER_KEYS):
                error("incomplete_device_or_planet_metadata", 1)
            elif [field.partition("=")[0] for field in metadata[1:11]] != list(PREHEADER_KEYS):
                error("preheader_field_order", 1)
            if any(key in values and values[key] != value
                   for key, value in (("star", "Sol"), ("body", "3"), ("subBody", "0"))):
                error("not_earth_surface_metadata", 1)
            if next(reader, []) != COLUMNS:
                error("column_header", reader.line_num)
            for row in reader:
                total += 1
                line = reader.line_num
                if len(row) != len(COLUMNS):
                    error("column_count", line)
                    continue
                mac, ssid, auth, stamp, channel, frequency, rssi, lat, lon, altitude, accuracy, rcois, mfgr, kind = row
                types[kind if kind in ("WIFI", "BLE") else "OTHER"] += 1
                if kind not in ("WIFI", "BLE"):
                    error("unsupported_cerberus_record_type", line)
                if not MAC.fullmatch(mac) or mac.lower() in ("00:00:00:00:00:00", "ff:ff:ff:ff:ff:ff"):
                    error("invalid_address", line)
                elif kind == "WIFI" and int(mac[:2], 16) & 1:
                    error("group_address_for_ap", line)
                if len(ssid.encode("utf-8")) > 32 or any(ord(c) < 32 or ord(c) == 127 for c in ssid):
                    error("invalid_cerberus_label", line)
                if not auth:
                    warnings["unknown_authentication"] += 1
                try:
                    if not STAMP.fullmatch(stamp):
                        raise ValueError
                    instant = datetime.strptime(stamp, "%Y-%m-%d %H:%M:%S").replace(tzinfo=timezone.utc)
                    if instant.year < 1980 or instant > now + timedelta(minutes=5):
                        raise ValueError
                except ValueError:
                    error("invalid_or_future_utc", line)
                try:
                    ch = integer(channel)
                    if kind == "WIFI":
                        expected = wifi_frequency(ch)
                        if expected is None or integer(frequency) != expected:
                            raise ValueError
                    elif kind == "BLE" and (ch != 0 or frequency != ""):
                        raise ValueError
                except ValueError:
                    error("channel_frequency", line)
                try:
                    if not -127 <= integer(rssi) <= 0:
                        raise ValueError
                except ValueError:
                    error("rssi", line)
                try:
                    latitude, longitude = number(lat), number(lon)
                    if not -90 <= latitude <= 90 or not -180 <= longitude <= 180:
                        raise ValueError
                    if latitude == longitude == 0:
                        warnings["zero_coordinate_pair_requires_review"] += 1
                except ValueError:
                    error("missing_or_invalid_location", line)
                for value, field in ((altitude, "altitude"), (accuracy, "accuracy")):
                    if not value:
                        warnings["missing_" + field + "_metres"] += 1
                        continue
                    try:
                        metric = number(value)
                        if field == "accuracy" and metric <= 0:
                            raise ValueError
                    except ValueError:
                        error("invalid_" + field + "_metres", line)
                if mfgr:
                    try:
                        if kind != "BLE" or not 0 <= integer(mfgr) < 65535:
                            raise ValueError
                    except ValueError:
                        error("manufacturer_id", line)
                if rcois and (kind != "WIFI" or not re.fullmatch(r"[0-9A-Fa-f]+(?: [0-9A-Fa-f]+)*", rcois)):
                    error("roaming_consortium", line)
        except (csv.Error, UnicodeError):
            error("invalid_csv_or_utf8", reader.line_num + 1)
        # Complete the hash even if parsing stopped on malformed input.
        for block in iter(lambda: source.read(65536), b""):
            digest.update(block)
            last_byte = block[-1:]
    final = path.stat()
    if (initial.st_size, initial.st_mtime_ns) != (final.st_size, final.st_mtime_ns):
        error("file_changed_during_validation", 0)
    if total == 0:
        error("no_observations", 0)
    if last_byte != b"\n":
        error("missing_final_newline_or_truncated_export", 0)
    status = "FAIL" if errors else "PASS_WITH_WARNINGS" if warnings else "PASS"
    return {"file": path.name, "status": status, "bytes": final.st_size,
            "sha256": digest.hexdigest(), "observations": total, "types": dict(types),
            "errors": dict(errors), "warnings": dict(warnings), "error_samples": samples,
            "server_acceptance": "NOT_TESTED"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", type=Path, nargs="+")
    args = parser.parse_args()
    failed = False
    for path in args.files:
        try:
            result = validate(path)
        except OSError:
            result = {"file": path.name, "status": "FAIL", "errors": {"file_read": 1},
                      "server_acceptance": "NOT_TESTED"}
        print(json.dumps(result, indent=2))
        failed |= result["status"] == "FAIL"
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
