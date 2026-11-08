"""Actual .MSH codec/model, transactional corruption tests and independent wire checks."""
from pathlib import Path
import hashlib
import os
import struct
import subprocess
import sys
import zlib

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/mesh-archive-checks"
out.mkdir(parents=True, exist_ok=True)

# Independent v1 golden document: zero observations from a completed sweep.
capture = struct.pack("<QQHB5x", 0, 0, 0, 0)
summary = struct.pack("<BBBBHHHHQQQ6I16Ii", 1, 15, 1, 0, 65535, 1, 100, 0,
                      500, 2000000, 2000100, *([0] * 6), 100, *([0] * 15), 0)
model = struct.pack("<QQIBBBBB3x", 0x1234567887654321, 0, 0, 0, 0, 0, 0, 0)
counters = struct.pack("<13IB3x", *([0] * 14))
body = capture + summary + model + counters
assert (len(capture), len(summary), len(model), len(counters)) == (24, 128, 28, 56)
golden = bytearray(struct.pack("<8sHHHHIIII", b"CBMSH\r\n\x1a", 1, 32, 1, 0,
                              32 + len(body), len(body), 0, 0) + body)
struct.pack_into("<I", golden, 24, zlib.crc32(golden))
(out / "golden-empty.msh").write_bytes(golden)

env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-global"),
           ZIG_LOCAL_CACHE_DIR=str(out / "zig-local"))
exe = out / ("mesh-archive-check.exe" if os.name == "nt" else "mesh-archive-check")
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
                "-I", str(root / "tools/host_checks/stubs"), "-I", str(root / "s3_main/main"),
                "-I", str(root / "shared"), str(root / "tools/host_checks/mesh_archive_check.c"),
                str(root / "s3_main/main/mesh_archive.c"), str(root / "s3_main/main/mesh_report.c"),
                str(root / "s3_main/main/mesh_scan.c"), str(root / "s3_main/main/mesh_observation.c"),
                str(root / "shared/mesh_mac.c"), "-o", str(exe)], check=True, env=env)
subprocess.run([str(exe), str(out), str(out / "golden-empty.msh")], check=True, env=env)
assert (out / "empty.msh").read_bytes() == golden

# Captured by compiling the pre-refactor mesh_report.c at fdc00ed763f5e1837b6e2381d372ec1ab378c059
# against these same actual-ingest fixtures. This catches a shared formatter
# regression that comparing only the new live/archive entry points would miss.
legacy_csv = {
    0: (2509, "b86c585ad5a7bc14f3ac649d0fab5e3c4bfd6e92c92c6b0cad454ad0b918f422"),
    80: (32998, "9560c1d0677baf0f3c3f094e0bc9ee3615a9f1646b6ec9e532af46837c00eb16"),
    1000: (33060, "f077e44c926e9ea145cf7d1eaac83e8e17e0e4e4f896890c5762bfcc9d081269"),
}
for records, (size, sha256) in legacy_csv.items():
    csv = (out / f"current-{records}.csv").read_bytes()
    assert len(csv) == size and hashlib.sha256(csv).hexdigest() == sha256
print("mesh archive before/refactor/reopened CSV byte compatibility PASS (0/80/1000 records)")

# Decode the emitted maximum layout without calling any C codec. Literal sizes
# intentionally catch accidental shared encoder/decoder field-order changes.
raw = (out / "maximum.msh").read_bytes()
assert len(raw) == 18332
header = struct.unpack_from("<8sHHHHIIII", raw)
assert header[:6] == (b"CBMSH\r\n\x1a", 1, 32, 1, 0, len(raw))
check = bytearray(raw)
check[24:28] = b"\x00" * 4
assert zlib.crc32(check) == header[7]
assert struct.unpack_from("<QQHB", raw, 32) == (1000, 1790, 65535, 1)
assert struct.unpack_from("<QQIBBBBB", raw, 184) == (0x1234567887654321, 1790, 80, 16, 64, 64, 64, 1)
assert struct.unpack_from("<4I", raw, 212) == (80, 80, 0, 0)
assert struct.unpack_from("<I", raw, 260)[0] == 16
assert struct.unpack_from("<HBB", raw, 268) == (0, 11, 0)  # Unknown, not known PAN0.
assert struct.unpack_from("<QHBBB", raw, 860) == (0xFEDCBA9800000001, 0, 11, 0, 3)
assert struct.unpack_from("<BBBB", raw, 5340 + 32) == (0, 1, 2, 1)
for index in range(64):
    offset = 8476 + index * 154
    session, callback, sequence, channel, flags, length, rssi, lqi = struct.unpack_from("<QQIBBBbB", raw, offset)
    assert (session, callback, sequence, channel, flags, length, rssi, lqi) == (
        0x1234567887654321, 1000 + 10 * index, index + 1, 11, 7, 125, -45, 200)
    assert raw[offset + 25:offset + 28] == bytes(3)
    assert raw[offset + 28:offset + 30] == b"\x41\xec"
    assert raw[offset + 153] == 0
known = (out / "known-zero.msh").read_bytes()
assert struct.unpack_from("<HBB", known, 268) == (0, 11, 1)
assert known[268 + 36] == 6
assert struct.unpack_from("<QHBBB", known, 305) == (0x1122, 0, 11, 1, 2)
print("mesh archive independent Python golden/maximum/known-PAN0 cross-checks PASS")
