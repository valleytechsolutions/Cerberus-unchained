# Rev138: streaming writer efficiency and verified CSV readback

The installed rev137 combined-mode run completed and its two files were read
back from SD. All 16,592 accepted observations are accounted for in the saved
files, with no reported queue drop or radio/SD service error. This establishes
the tested receive-to-file path, not RF completeness or comparative performance.
The LC29H delivered checksum-valid NMEA at 115200 baud but lost its navigation
fix partway through this stationary run. The unlocated file preserves those
sightings without inventing positions.

## Changes

Streaming previously called the legacy 160-slot duplicate-history lookup for
each Wi-Fi/BLE observation, then ignored its decision. Rev138 skips that work
entirely. Every received observation remains eligible for its own CSV row.
Legacy snapshot consumers keep their history behavior and failed appends still
cannot commit a history entry. This removes known CPU/PSRAM work; no measured
increase in RF detections is claimed.

The WiGLE metadata pre-header now contains the documented Earth fields
`star=Sol,body=3,subBody=0`, the actual firmware build version, and accurate
timing provenance for onboard streaming versus expansion snapshots. CSV row
data and ordering are unchanged. No GNSS accuracy is fabricated from HDOP.

`tools/validate_wardrive_csv.py` reads complete CSV files without changing or
uploading them. It checks UTF-8/CSV structure, the ordered 14-column header,
metadata, addresses, UTC dates, finite/bounded coordinates, channel/frequency,
RSSI, optional numeric measurements, manufacturer IDs and a final newline.
It supports the Wi-Fi/BLE subset emitted by Cerberus. Error reports contain
line numbers and codes, not observed addresses, names or positions. Repeated
rows are preserved and counted. Unknown accuracy/altitude produce explicit
warnings, not replacement measurements. A local pass is not a server receipt.

```sh
python tools/validate_wardrive_csv.py path/to/WIGLE001.CSV
```

## Live evidence

| Installed rev137 combined run | Result |
| --- | --- |
| Duration | 392.458 seconds |
| S3 2.4 GHz Wi-Fi observations | 3,290 |
| S3 BLE observations | 11,338 |
| C5 5 GHz Wi-Fi observations | 1,964 |
| Located observations | 2,291: 685 Wi-Fi, 1,606 BLE |
| Unlocated observations | 14,301 |
| Reported queue drops | S3 0, C5 0 |
| Pending observations after Stop | S3 0, C5 0 |
| RF owners after Stop | Both released |
| Saved files | Two, both read back |

Private local evidence is under `artifacts/device-rev137/sd-readback` and the
`combined-*` diagnostic snapshots. The pre-header-corrected local copy preserves
the original column header and observation bytes exactly. Its preflight result
is PASS_WITH_WARNINGS for 2,291 missing accuracy measurements; server acceptance
is NOT_TESTED. The original on-card file is preserved.

### Installed rev138 capture

The owner confirmed FIX + UTC and a normal Stop on rev138. The stopped session
diagnostic records 31.415 seconds in combined mode, with the following counts:

| Rev138 combined run | Observations |
| --- | ---: |
| S3 2.4 GHz Wi-Fi | 263 |
| S3 BLE | 1,054 |
| C5 5 GHz Wi-Fi | 121 |
| Total accepted and verified in SD files | 1,438 |
| Geotagged | 1,436 |
| Preserved without location | 2 |

Both receive queues drained with zero reported drops, both RF owners released,
the worker stopped, and all reported radio/GPS/SD service errors were zero.
The device reports two saved files. At the post-stop diagnostic capture, the
LC29H had a fresh fix and valid UTC, 20 satellites and zero bad NMEA checksums
at 115200 baud. This is a sampled fix status; the two fallback rows remain
unlocated rather than borrowing a position.

Private diagnostic proof is
`artifacts/device-rev138/combined-session-validation.json`, bound to the raw
capture. The SD card was then read directly in the PC's card reader. The two
new files were copied locally and their bytes hash-verified against the card:

| File | Rows | Contents |
| --- | ---: | --- |
| `WIGLE002.CSV` | 1,436 | 263 2.4 GHz Wi-Fi, 121 5 GHz Wi-Fi, 1,052 BLE |
| `UNLOC008.CSV` | 2 | Two S3 BLE observations preserved without coordinates |

Every accepted observation is accounted for, including the two fallback rows.
The WiGLE file contains the native rev138 metadata and all Earth fields without
a post-processing correction. The 14-column CSV passes local preflight with
no errors. Warnings are 1,436 absent accuracy measurements and two absent
altitudes. There are 1,368 repeated-identity rows in the located file; they were
preserved. The original card files were not changed, and nothing was uploaded.

Both fallback rows record fresh GPS state and an empty valid BLE name. The
precise formatter rejection was not retained, so the cause cannot be inferred
as lost GPS lock. The general GPS display accepts a fix up to five seconds old;
WiGLE export separately requires fix/UTC age and callback-to-snapshot delay
within one second. Per-reason fallback counters are a useful follow-up; this
checkpoint does not loosen those checks or reconstruct missing coordinates.

CSV observation UTC spans `2026-11-05 01:25:59` through `2026-11-05 01:26:30`.
FAT directory dates still show 1980, so they must not be used as the capture
date in a future log list. Private reconciled proof is
`artifacts/device-rev138/sd-readback/readback-validation.json`. This establishes
the short combined capture-to-file path, not server acceptance or endurance.

## Validation status

All 160 host suites pass, including ten new file-level preflight tests and
cross-checking actual C formatter output with an independent CSV reader. Routing
fixtures verify 1,000 repeated located observations, a BLE observation and stale
queue routing while asserting that streaming performs zero legacy lookups.
All five native targets pass. The host and native receipts bind to the same
1,250 unchanged inputs at source commit
`99383a6c3e3e77b90552e0d0b40fcb86d62273a8`. Native READY, console-stack and C5
Locate/parser gates pass. The compiled S3 image contains its actual rev138
metadata; both CSV header lines together occupy 379 bytes, below the existing
512-byte storage header limit.

The matched development package is
`dist/cerberus-unchained-0.1.0-dev.rev138-20261104T222302Z`.
Its 22 listed package files pass checksum verification. Manifest SHA-256:
`bc62df06636076615768aed9d1439bdec40061755efce85e66c30bd58ec7d6b8`.
S3 application size is 2,122,608 bytes (+64 versus rev137), C5 1,728,736 bytes,
and the recovery bridge 255,296 bytes. Fixed data/BSS and the measured receive
queue/workspace allocations are unchanged on both chips. Retained proof is
`artifacts/rev138-validation/local-milestone.json`.

The matched rev138 package is **installed on both chips**. Fresh recovery
backups match the preceding rev137 images: 2,191,360 bytes for S3 and 2,097,152
bytes for C5, covering all erase-rounded update writes. Six identity-guard tests
and 28 installation-helper tests pass. Exact USB and chip identities were checked
before access; all nine bridge/C5/S3 image writes passed hash verification.
Neither a blanket flash erase nor an NVS rewrite was performed.

Two post-install diagnostic captures 48.95 seconds apart confirm the rev138 S3
application, C5 capability 75 online, mounted SD, released radio owners and no
previous reset fault. S3 internal free memory stayed at 135,964 bytes, the largest
internal block at 94,208 bytes, and PSRAM free at 1,427,552 bytes. The C5 snapshot
reported 53,460 free bytes. SPI exchanges advanced from 50 to 286 without a new
listed transport error or a changed C5 boot count. The four initial READY timeout
and legacy-fallback counters were present at boot and did not increase; they are
not represented as zero.

Private installation evidence is retained in
`artifacts/device-rev138/installation-receipt.json` (SHA-256
`c4aff1d7a67994f054e992b3782c330fbbba835b724458edc20133dcee0ed423`).
The immutable pre-install milestone remains separate from that installation
receipt. Boot/idle checks do not establish fresh capture, GPS, touchscreen,
SD save/reopen or sustained-load behavior. The short combined-mode run and
physical SD readback above add capture/Stop and file-content evidence. No
service upload or PR update has been performed.

Further acceptance needs sustained GPS fix/UTC, repeated combined-mode runs,
separate-mode tests, endurance and a real service import result. Channel dwell
and S3 Wi-Fi/BLE allocation should be tuned with controlled coverage comparisons;
shorter sweeps alone do not establish better reception.

The user requested saved Wi-Fi and three upload destinations after the capture
and export work. That scope is recorded in [the upload design](WARDRIVE_UPLOADS.md).
It is not implemented or represented as an available device feature by rev138.

Sources: [WiGLE's CSV 1.6 specification](https://api.wigle.net/csvFormat-1_6.html),
[official Android export implementation](https://github.com/wiglenet/wigle-wifi-wardriving/blob/main/wiglewifiwardriving/src/main/java/net/wigle/wigleandroid/background/ObservationUploader.java),
and [Quectel LC29H protocol specification](https://www.quectel.com/content/uploads/2022/02/Quectel_LC29H_SeriesLC79HAL_GNSS_Protocol_Specification_V1.4.pdf).
Quectel documents GST error statistics in metres. Receiving and epoch-matching
those fields is future work; support/output settings depend on the receiver
variant and firmware. A configurable constant or HDOP-as-metres is not a substitute.
