# Rev130: passive Mesh Detection foundation

**Post-installation navigation finding:** the owner's November 2 check found
that Scan still displayed Radio Sense and entered the observation tool directly;
Back then revealed the five-tool menu. This did not satisfy the intended
menu-first workflow. Rev131 corrects both the visible entry and route, with
explicit return labels. The requested rev130 Start/Stop/Back/Wi-Fi acceptance
was not confirmed by this response and remains open.

This development slice adds an original, bounded IEEE 802.15.4 observation
pipeline for the onboard S3/C5 pair. It builds on the receive continuity fix in
[rev129](DUAL_RADIO_REV129.md). It needs no GPS or expansion board. The wider
[Mesh Detection plan](MESH_DETECTION_PLAN.md), including protocol discovery,
selected-address location and historical map reopening, remains unfinished.

## Processor roles and ownership

C5 owns passive IEEE 802.15.4 reception on channels 11–26, fixed-channel or
selected-mask sweeping, callback-buffer lifetime and radio cleanup. It reuses
the existing 4,096-byte worker. The 16-slot copied-frame FIFO and pinned summary
are optional internal-heap allocations, released after cleanup. There is no
second C5 radio task. A valid active STATUS renews a ten-second lease; lease
expiry stops reception and records a timeout. Stopped evidence has a separate
30-second retention period, checked before renewal.

S3 owns bounded header parsing, PAN/address/traffic aggregation, touch selection,
rendering and asynchronous SD reports. Its model/controller and UI storage use
optional PSRAM, with no internal-RAM fallback. C5 is reserved for IEEE 802.15.4
while this service is held; it cannot simultaneously scan Wi-Fi/BLE on its one
RF path. S3 complementary BLE/Wi-Fi observations are a later integration step.

The C5 capability revision is 72. Commands 146–153 use full 64-bit sessions and
distinct, nonwrapping row and summary identities. A row stays immutable until
its exact ACK. Repeated ACKs cannot retire the next row. An uncertain START is
resolved by STATUS, never by blindly issuing another START. Back/hidden-page
cleanup closes the exact owned session rather than stopping a foreign tool.

The S3 controller budgets four logical wire calls per tick and checks an
eight-millisecond elapsed budget before starting another call. INFO and START
cost two calls because they refresh capability. This is cooperative scheduling,
not a hard SDK latency guarantee. A 139-byte record needs 16 nine-byte READs,
plus PEEK and ACK; high-rate traffic can overflow the 16-slot queue. Queue drops,
invalid callbacks and S3 retention omissions are separate evidence.

## What the observations mean

The independent parser supports bounded clear IEEE 802.15.4 MAC headers with
typed short and extended addresses, explicit/compressed/unknown PAN provenance,
security/header-IE bounds and opaque encrypted payloads. Full EUI-64 values are
retained. See [parser scope and references](MESH_MAC_PARSER.md).

The stable S3 store retains up to 16 PAN contexts, 64 addresses, 64 directed
traffic edges and 64 complete raw witnesses. It does not evict an older identity
when a new one appears. It keeps omission counters and explicitly marks an
unavailable latest witness after the raw store fills. Aggregated counters can
continue after the first 64 raw witnesses.

- A PAN context is a channel/value bucket within one session, not proof that
  every address in it belongs to one network. Unknown PAN is distinct from PAN 0.
- An edge means a clear MAC header addressed traffic from one endpoint to
  another. It does not prove delivery, routing, adjacency or parent/child roles.
- RSSI/LQI belong to a received source. An address seen only as a destination
  has no measured signal. Broadcast/sentinel addresses and addressless ACKs do
  not become invented peer links.
- Timestamps are C5 callback uptime, not UTC, synchronized S3 time, or precise
  RF arrival times. Retry suppression applies only to transport delivery IDs;
  repeated over-the-air MAC sequence numbers remain observations.
- SDK metadata replaces the FCS positions. Retained MAC bytes omit both the SDK
  length byte and RSSI/LQI footer; they are not an RF frame with verified FCS.

This new observation service does not identify Zigbee or Thread protocols,
discover commissioned Matter fabrics, authenticate devices, join networks, or
extract authoritative routing tables. The separate legacy Zigbee Beacon tool
remains available. Matter commissioning BLE hints would be separate
evidence, not proof of Thread membership. LoRa reception needs additional radio
hardware.

## Interface and reports

The integrated entry is Scan → Mesh Detection, with PAN, Addresses and Traffic
views, fixed/sweep settings, frozen row details and explicit Stop/Back. A new
view defaults to sweeping channels 11–26 with a 250 ms dwell.
Existing Energy, Beacon, Activity and Compare tools remain reachable from the
same workflow. The focused UI/controller checks and software render previews
pass; touchscreen behavior still requires physical acceptance. STOP & SAVE requests
the final report while acquiring; SAVE REPORT saves an already frozen scan.
OPEN CSV hands a saved file to the existing paged text viewer. That viewer uses
the existing bounded file-read path, rather than the archive save worker.

SAVE REPORT uses the existing serialized archive worker. It copies a fully
stopped, drained, final-summary-fenced and released scan before returning to the
UI. Formatting and filesystem calls run in the worker. Reports are versioned
CSV files under `/CERBERUS/LOGS/M00001.CSV` and subsequent unused names, with
settings, source counters, channel dwell, S3 omissions, PANs, addresses, directed
observations and retained raw witnesses. Binary evidence is hex, with no
untrusted text interpreted as CSV formulas. Reports preserve full addresses;
screen privacy masking is not an export anonymizer.

The report buffer is capped at 98,304 bytes plus a terminal NUL in PSRAM. Existing
archive formats keep their exact-sized allocations. The SD writer reserves an
unused temporary name, checks write/flush/sync/close and publishes by rename.
Failure retains the created `.TMP` and its name, preserving older files.
Cancellation before storage skips the save; an in-progress synchronous save
reports its actual result. Successful rename is not proof of power-loss safety.
CSV export does not imply native map reopening or raw PCAP support.

## Validation status

Focused parser, model, link, controller, C5 worker, actual C5-to-S3 bridge,
report/worker and UI fixtures have passed on the host. The bridge exposed and
verified a cleanup fix: an SDK receive failure must clear the terminal channel
as well as close callback admission, allowing the S3 final-state fence to retain
prior evidence with the original radio error. Failed ACK operations return no
success sequence; successful ACKs still echo the exact retired row.

The complete 146-suite host registry and all five native targets pass at source
commit `2923db15114a5c5d86d387010261952de28ce055`, with the same 1,174 unchanged
inputs. The 21 shard-selection/aggregation tests, nine build-report tests and
standalone field-kit checks also pass. Host checks use Python 3.14.2 and Zig
0.14.1; native builds use PlatformIO 6.2.0 and IDF 6.0.1. The S3 READY/console
and C5 link gates pass. These local results are separate from hosted rev128 CI.

The matched development package is
`dist/cerberus-unchained-0.1.0-dev.rev130-20261102T200433Z`.
Its three application images match the retained native matrix, all 20 checksum
entries verify, and read-only web-package verification passes chip headers,
partitions and image bounds. The Marauder notice remains included. Manifest
SHA-256 is `3ef4cdd1e125706ab60d12cd67ef98de41e61761894551ecf3d687cd0d59c0d7`.
Bound evidence is retained in `artifacts/rev130-validation/local-milestone.json`
and `artifacts/rev130-validation/mesh-native-final/native-memory-comparison.json`.
The initial host run exposed an old console fixture missing the new report
format; that fixture was corrected before the complete passing run.

Native ELF measurements, compared with the retained rev129 candidate:

| Resource | Native size or change |
| --- | ---: |
| S3 linked fixed data/BSS | 169,436 bytes; +32 bytes |
| C5 linked fixed data/BSS | 83,360 bytes; +40 bytes |
| Optional C5 Mesh context, including 16 copied-frame slots | 2,624 bytes |
| Optional S3 Mesh UI workspace, including model/controller | 22,560 bytes PSRAM |
| Selected Mesh report worker arm, including copied scan and formatter | 119,776 bytes PSRAM |
| Simultaneous Mesh UI plus save worker | 142,336 bytes PSRAM |

These sizes exclude allocator overhead and other services. Earlier archive
arms and legacy IEEE banks retain their prior sizes. The C5 worker remains
4,096 stack bytes and the archive worker 6,144; there is no new task. Selected
native entry frames were inspected, including the 880-byte Mesh UI tick,
320-byte controller tick, 400-byte report append and unchanged 800-byte libc
formatter frame. These are individual entry frames, not a proven full call
chain or runtime stack high-water measurement. Runtime headroom, latency,
throughput and sustained reception still need physical checks.

The S3 app is 2,044,816 bytes at offset 0x10000. Its write extends beyond the
first 2 MiB of flash. Historical local installers with a fixed 2 MiB backup
limit must not be reused unchanged: paired installation needs a fresh backup
covering every erase-rounded write region and the actual installed baseline.
The subsequent installation used the derived 0x204000-byte S3 backup and a
0x200000-byte C5 backup, freshly read and fully verified against the device and
the installed rev126 image segments. Both verified prefixes remain available
for explicit recovery. Normal writes preserved NVS/PHY and higher flash.

## Installed checkpoint and remaining acceptance

The paired rev130 update was installed on November 2, 2026. All nine image writes
(temporary bridge, C5 and final S3) passed their hash verification. The final
application reports rev130, C5 capability 72 online, buffered display and mounted
SD. Two idle samples 45.6 seconds apart show advancing S3/C5 uptime and SPI
exchanges, unchanged C5 boot count, no previous reset fault and no increase in
the monitored SPI error counters. Four initial READY timeouts/legacy fallbacks
were present at both boot samples, also observed in the cold rev126 baseline;
they are not represented as zero errors.

Both candidate idle samples show 137,664 bytes of free S3 internal heap,
94,208 bytes in the largest internal block and 1,427,552 bytes of free PSRAM.
C5 snapshot free heap is 56,116 bytes. The ten-task count is unchanged; the
lowest observed diagnostic-console stack margin is 1,312 bytes. These are
sampled idle measurements, not loaded Mesh heap/stack or RF performance results.

Installation evidence is retained separately in
`artifacts/device-rev130/installation-receipt.json` and `idle-comparison.json`.
The immutable build receipt's `installed: false` records its earlier packaging
state; the installation receipt records the later verified update and keeps
`full_feature_acceptance: false`. Fresh backups, identity checks, six identity
tests, 23 installer-helper tests and independent preparation review passed.
No firmware release was selected or published. The existing public PR still
describes the separately validated rev128 work.

Actual Mesh reception, touchscreen Stop/Back, SD reports and repeatable handoffs
to Wi-Fi and Bluetooth remain unaccepted. A user smoke check has been requested;
no successful result is assumed.

The next physical sequence is an idle entry, a default sweep lasting more than
ten seconds, Stop, and a second Start/Stop without reset. Repeat fixed channels
11 and 26; check Back
during acquisition and sleep/wake without automatic restart. Save and open a
CSV twice to confirm distinct filenames, then run ordinary Wi-Fi/BLE and the
preserved IEEE tools. Zero observations can exercise lifecycle and empty-report
handling, but cannot establish RF sensitivity. Known addressed traffic is
required to verify PAN/EUI attribution, source signal values, traffic edges,
raw witnesses and populated SD reports. No GPS is required.

No CYM application code was copied. Its pinned workflows informed the plan;
the existing Cerberus and third-party notices remain in place. This slice does
not close the larger Marauder parity ledger.
