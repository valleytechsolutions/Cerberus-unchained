"""Compile production GPX formatting and independently parse its saved XML."""
from pathlib import Path
from datetime import datetime, timezone
import math
import os
import subprocess
import sys
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/gps-gpx-checks"
out.mkdir(parents=True, exist_ok=True)
exe = out / "gps-gpx-check.exe"
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-cache"))
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
    "-I", str(root / "s3_main/main"), str(root / "s3_main/main/gps_gpx.c"),
    str(root / "tools/host_checks/gps_gpx_check.c"), "-lm", "-o", str(exe)], check=True, env=env)
subprocess.run([str(exe), str(out)], check=True, env=env)
ns = "{http://www.topografix.com/GPX/1/1}"
for name in ("track", "points"):
    document = ET.parse(out / f"{name}.gpx").getroot()
    assert document.tag == ns + "gpx" and document.attrib == {"version": "1.1", "creator": "Cerberus"}
    if name == "track":
        segments = document.findall(f"{ns}trk/{ns}trkseg")
        assert len(segments) == 2 and [len(s) for s in segments] == [2, 1]
        rows = [p for s in segments for p in s]
    else:
        rows = document.findall(ns + "wpt")
        assert [p.findtext(ns + "name") for p in rows] == ["Lake & <bridge> \"north\" 'side'", "café 📍", "POI 3"]
    assert len(rows) == 3
    for i, point in enumerate(rows):
        assert math.isclose(float(point.attrib["lat"]), 12.3456789 + i * .0001, abs_tol=1e-8)
        assert float(point.attrib["lon"]) == -98.7654321
        assert datetime.fromisoformat(point.findtext(ns + "time")) == datetime(2026, 11, 8, 1, 2, 3, 456000, tzinfo=timezone.utc)
        expected = (["ele"] if i == 0 else []) + ["time"] + (["name"] if name == "points" else []) + (["sat", "hdop"] if i == 0 else [])
        assert [c.tag.removeprefix(ns) for c in point] == expected
        if i == 0:
            assert point.findtext(ns + "ele") == "123.500" and point.findtext(ns + "sat") == "12"
            assert point.findtext(ns + "hdop") == "0.800"
print("Independent XML readback: namespace, GPX point order, coordinates, UTC, escaped UTF-8 names, optional fields and segment boundaries pass.")
