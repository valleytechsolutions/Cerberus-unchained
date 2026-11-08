# Rev106: independent BLE activity from both receivers

This checkpoint adds a receive-only C5 Analyzer path beside the existing S3
Analyzer. All 123 host suites and five native targets pass on the final source.
The paired rev106/C5 firmware65 update is installed with verified writes and
two read-only boot captures. It includes the previously local rev104 combined
discovery list and rev105 saved-report filtering. Physical feature acceptance
remains separate from this installation result.

## Upstream behavior and processor roles

The pinned Marauder develop source at
`79cc6bcba1db6679baee1dab2b0c1ad5010e6363` exposes Bluetooth Analyzer in
`MenuFunctions.cpp:5515`. Its callback at `WiFiScan.cpp:1584` increments an
activity counter and periodically updates a displayed name/RSSI. The graph
uses callback counts per configured banner interval. These facts were checked
against the local clean pinned source; they do not establish RF equivalence.

Cerberus's S3 Analyzer retains its 64 completed 200 ms callback buckets,
latest-report information and independent family evidence. C5 contributes
passive original advertising callback counts, including partial extended
reports, before inventory capacity. Scan responses are excluded on C5. These
are receiver-specific controller callbacks, not unique devices or all
over-the-air packets. The two totals must not be summed or deduplicated.

## Compact C5 measurement contract

Dedicated Analyzer mode uses the existing dual BLE owner, finite eight-second
RF sweeps and exact Stop/Release binding. It avoids inventory retention and
row downloads for activity display. Firmware 65 gates both the new mode's
START and its sample command; older peers cannot be silently started in an
unsupported mode.

One 22-byte sample carries full epoch, job ID, cumulative callback count,
C5-local elapsed milliseconds, validity/state flags and last callback RSSI.
The request supplies the existing full boot/job binding. Fields are sampled
atomically under the scanner mutex. Timing starts around the actual SDK scan
start and freezes at completion. Invalid or backward clocks cannot create a
valid rate. Saturating counters remain lower bounds, not exact interval counts.
RSSI has no peer identity or age in this frame and cannot establish freshness.

S3 derives C5 rates only from consecutive valid count/time differences in one
boot/job/epoch. Zero-duration, rewound, reset, saturated and interrupted sample
intervals cannot become invented zero-rate measurements. C5 windows retain
their own elapsed duration and are not inserted into S3's 200 ms buckets.
Polling uses the existing bounded transaction scheduler; drawing reads cached
state and performs no SPI work.

## Ownership and acceptance

Analyzer entry, retry, Stop, Back and sleep must respect both existing owners.
Unavailable C5 telemetry is distinct from a valid zero count; the existing S3
view remains useful independently. Cleanup must continue after timeout and
must not rearm a receiver while a previous owner is still releasing.

The default BOTH RECEIVERS page presents the S3 completed 200 ms buckets and
C5 source intervals separately. S3 DETAILS retains the existing family and
advertisement/response evidence. A transport error hides current C5 count/rate
while retained source intervals remain explicitly labelled. Stop, Back and
sleep continue cleanup after a release timeout; they cannot silently abandon
the old owner or restart it while exiting.

## Final local validation

- All 123 host suites pass, with retained per-suite logs and source hashes.
- All five native targets pass: S3, onboard C5, bridge, expansion WROOM and
  expansion XIAO. The first native run caught a missing UI forward declaration;
  that failure is retained, the declaration is fixed, and both complete host
  and native runs were repeated on the corrected source.
- Actual C5 producer/controller integration covers 75 callbacks across more
  than 32 identities, partial extended reports, response/anonymous exclusions,
  parser/inventory bypass, true zero intervals, final sweeps and new ownership.
  Additional checks cover stale bindings, failed starts, counter saturation,
  same-millisecond growth, clock faults, old firmware and release fences.
- Four actual-renderer fixtures were inspected: independent receivers,
  saturation, unsupported C5 and release timeout. These are synthetic UI
  previews, not photographs or physical touchscreen acceptance. The final host
  run reproduced the reviewed images byte for byte.

Native measurements compare final rev106 ELFs with retained, unflashed rev105:

| Measurement | Rev105 | Rev106 | Change |
| --- | ---: | ---: | ---: |
| S3 companion state in the existing PSRAM allocation | 8,072 B | 8,496 B | +424 B |
| S3 Analyzer state | 288 B | 288 B | 0 |
| S3 live-discovery store | 20,560 B | 20,560 B | 0 |
| S3 internal data + BSS | 172,572 B | 172,588 B | +16 B |
| C5 data + BSS | 80,032 B | 80,080 B | +48 B |
| S3 dual-controller entry frame | 176 B | 192 B | +16 B |

The new dual-view drawing entry frame is 864 bytes. The UI main entry frame
remains 1,136 bytes and its task allocation remains 8,192 bytes. These are
selected native frames, not a bound on an entire nested call chain. Inlined
helpers are not assigned zero cost. Type sizes exclude allocator overhead and
fragmentation; neither static section sizes nor host checks establish runtime
headroom, sustained stability or RF performance.

Evidence is retained under `artifacts/rev106-host-evidence`,
`artifacts/rev106-native-evidence` and `artifacts/device-rev106`.

## Installed package and boot evidence

- Implementation commit: `ed3fed60977ae414b79d95304af41238373bbc68`.
- Matched package: `dist/cerberus-unchained-0.1.0-dev.rev106-20261101T101002Z`.
- Source SHA-256: `1cf454655fed484bf7d63027dcb7c4338296da58d40f841a771aa64fada3dc10`.
- Manifest SHA-256: `f4a6e4e63e76190913f10250619794f9555488ab1d8165225120151d04398fdd`.

Package validation checked all image hashes, chip headers, partitions and
bounds. All three packaged application hashes match the retained native
matrix. Six synthetic identity-guard checks and nine offline installation
checks passed before device access. The exact S3/C5 identities were checked
before writes. Fresh first-2-MiB backups from each chip matched the actual
installed rev103 images and passed device verification; they cover every
erase-rounded region changed by the update. Higher flash was not erased or
programmed.

The bridge, C5 and S3 each verified all three programmed images. The final S3
application reports rev106 and the C5 reports protocol2/firmware65 online.
Both post-update read-only captures verify idle owners, SD mounted, USB storage
inactive, a buffered display and `previous_reset_fault=no`. The retained
26,240-byte historical crash record was not cleared. Four initial READY
timeouts/four legacy fallbacks were present at startup and remained unchanged
in the settled capture; mutex, fall/high READY and driver error counters were
zero. This is not a claim that the transport never times out.

The settled idle reading has 134,380 S3 internal bytes free, a 94,208-byte
largest internal block, 1,427,552 PSRAM bytes free and ten tasks. Observed main
and console stack low-water values are 5,432 and 1,324 bytes respectively.
C5 reports 58 KiB free and a 54 KiB minimum. These are startup/diagnostic
observations: the optional Analyzer/discovery/archive work has not been
exercised on hardware in this checkpoint, so idle readings do not measure its
loaded memory or stack headroom.

`artifacts/device-rev106/installation-receipt.json` binds the preflight, fresh
backups, nine verified image writes and two boot captures. The earlier
`local-milestone.json` remains an immutable pre-install package receipt;
installation is recorded separately. Recovery did not need to run. No pull
request, push, public release or web installer selection was performed.

## Remaining physical acceptance

The user was unavailable for touchscreen checks. The next physical checks are
both Analyzer displays receiving, Stop and Back releasing both owners, a
subsequent normal Wi-Fi scan, and the new combined discovery/archive views.
Real-card save/reopen, source-rate calibration, extended/coded reception and
sustained memory/stack headroom remain open. Full Marauder parity, GPS and
expansion hardware acceptance are not complete.
