# Adaptive two-radio scanning

## Current installed checkpoint

The latest installed checkpoint is [rev158](CHANNEL_ACTIVITY.md). The
rev159 candidate adds [S3 Wi-Fi cleanup recovery](GPS_AND_RADIO_RECOVERY.md):
the existing scan worker retains its owner and retries unfinished teardown
steps after a driver error. Successful cleanup lets Bluetooth or another Wi-Fi
scan acquire the S3 again. Persistent errors remain visible and keep ownership
reserved. This does not add another receiver or change scan dwell times.
The older checkpoints below retain their original acceptance scope.

[Rev123](DUAL_RADIO_REV123.md) implements offline Find My JSON interchange
over the existing serialized archive worker. Complete saved originals and
matched responses from both receivers are considered independently; duplicate
MAC records remain separate and imports do not invent source/type/time or
ADV/RSP provenance. The bounded model allows 64 records, 8,192 aggregate payload
bytes and 49,152 JSON bytes. One optional JSON viewer holds the committed model;
only the selected worker allocation arm is allocated, preserving the ordinary
native `.BLE` job size. Focused codec, IO/storage and UI checks and independent
reviews pass. The first complete 132-suite host run passed on 1,096 unchanged
inputs; all five native builds and the linked resource audit pass. S3 fixed
data/BSS grows by 32 bytes and C5 is unchanged. The optional JSON viewer and
worker total 68,200 bytes before allocator overhead and other stores; keeping
the native viewer during export brings those allocations to 129,312 bytes.
These are allocation sizes, not runtime headroom. Matched package build and
verify-only checks pass for the three-target, nine-image local package.
The matched rev123/C5 firmware69 update is installed with verified image writes,
paired application boot, display buffering and mounted SD. Two accepted idle
captures more than 42 seconds apart confirm the settled boot state; they do not
establish physical RF, touchscreen, storage-workflow, GPS or sustained-load
acceptance. Offline import is not load-to-live-list or complete raw-history parity.
The rev114–122 source milestones below are included in rev123; their checkpoint
documents retain their original validation and installation history.

[Rev122](DUAL_RADIO_REV122.md) implements integrated GPS DATA/received-NMEA
observation through S3's existing UART owner while C5 remains available for
Wardriving Wi-Fi. Independent GGA freshness, optional bounded trace storage
and retained UART cleanup are part of this source milestone. Implementation,
focused checks and independent reviews are complete, including frozen paging with
ages at copy and entire raw-line masking. All 130 host suites, five native
builds, the linked resource audit and matched package verification pass.
Measured S3 fixed data/BSS grows by 128 bytes; the optional trace/display copies
total 2,624 bytes of PSRAM before allocator overhead. C5 fixed data/BSS is
unchanged. Entry-frame measurements remain separate from runtime stack margin.
No physical GPS or concurrent-radio performance acceptance is claimed.

[Rev121](DUAL_RADIO_REV121.md) implements C5 receive cleanup bound to private
scan epochs, retained SDK ownership and Channel Survey admission/retry. Focused
regressions, independent review, all 128 host suites, five native builds and
matched package verification pass. The current Survey workflow requires C5
firmware 69 for checked cleanup acknowledgement; the original firmware 46 wire
layout is unchanged. Survey's generation-byte/band checks remain limited
fencing, not a full job token. Physical acceptance remains separate.

[Rev120](DUAL_RADIO_REV120.md) binds shared S3 BLE host release to an exact
claim and preserves pending initialization reservations across reset. Existing
host clients are migrated together.

[Rev119](DUAL_RADIO_REV119.md) extends exact S3 passive ownership to Wardriving
acquisition, completed BLE evidence and cleanup. Its existing terminal source
error policy remains explicit.

[Rev118](DUAL_RADIO_REV118.md) develops exact S3 passive scan ownership for
Recon acquisition, evidence and cleanup. C5 Wi-Fi retains its independent
scheduler.

[Rev117](DUAL_RADIO_REV117.md) adds exact S3 Fox Hunt scan ownership while
preserving independent C5 tracking. The coverage ledger has been reconciled
through these source milestones; [the remaining-work audit](RECEIVE_REMAINING_GAPS.md)
records unresolved receiver, GPS, storage and device-info scope. Installation
and physical feature acceptance remain separate.

[Rev116](DUAL_RADIO_REV116.md) separates Analyzer receiver intent and retries,
with exact S3 scan ownership for callback accounting and cleanup. Physical
acceptance is pending.

[Rev115](DUAL_RADIO_REV115.md) makes ordinary BLE discovery survive either
receiver's failure, including an unavailable S3 store, with independent retries
and shared release barriers for Stop, selection, navigation and saving.
Physical acceptance is pending.

[Rev114](DUAL_RADIO_REV114.md) integrates the dedicated Find My Monitor.
It reuses the shared discovery owners, retains separate S3/C5 evidence and
keeps Stop/Back/sleep and saved-result navigation on the existing release paths.
Physical acceptance is pending.

[Rev113](DUAL_RADIO_REV113.md) adds compact C5 response-result transfer with
full-format fallback for older peers. That matched update was installed with
verified writes, paired boot and mounted SD; physical feature and performance
acceptance remain pending. Rev112 below records its preceding installed baseline.

[Rev112](DUAL_RADIO_REV112.md) addresses independent dual-receiver Fox Hunt
startup, retry, signal/status display and retained cleanup. Its matched update
was installed with verified writes, paired boot and mounted SD; dual-receiver
physical acceptance remains pending. Rev111 below records earlier work; its
results do not establish rev112 or rev113 physical acceptance.

[Rev111](DUAL_RADIO_REV111.md) was installed with paired boot and SD mount verified before rev112. It includes [rev110's](DUAL_RADIO_REV110.md) optional C5 BLE response collection and source-separated live/saved analysis, plus the callback cleanup. Passive remains the default, and S3 operates independently. Physical response and UI acceptance remain pending. The rev97 description below is historical; its physical results do not establish rev111 acceptance.

The rev97 source milestone extends the two-radio scanning introduced in rev94
after the rev93 contribution. It retains onboard C5 protocol firmware 61.
Rev97's cached C5 handoff and BLE list controls pass all 120 host suites and
five native targets. The matched package is installed and both applications
boot; the new touchscreen controls and handoff still need physical acceptance.
The [rev97 checkpoint](DUAL_RADIO_REV97.md) records the exact build, memory and
installation evidence. The historical rev94-rev96 results below keep their original scope.
No comparison of speed, coverage or full Marauder parity is claimed.

## Using the views

Open the ordinary Wi-Fi scan from the launcher, Wi-Fi menu or lesson. When
both radios are free, the view shows `S3: 2.4 GHz + C5: 5 GHz`. It retains up to
32 rows from each receiver (64 total), supports band filters and sorting, and
keeps a previous completed bank visible while a replacement runs. Stop and
Back remain available during scan and result transfer. A clean refusal by S3
before starting uses the existing C5 all-band scan with an explicit fallback
label; uncertain cleanup is an error, not permission to steal the radio.

Tap a row for a frozen result. `OPEN AP TOOLS` can now reuse a C5-origin row's
eligible completed C5 bank after checking its owned session, full-width bank
proof and exact AP identity. Successful cached handoff performs no new scan
or row retransfer and preserves the original receipt time and band coverage.
It validates retained evidence, not the AP's current over-the-air presence.
An S3-origin row or locally ineligible bank still uses the existing fresh C5
all-band scan. A failed runtime proof/owner check reports failure rather than
silently starting another scan. `BACK TO SCAN` returns to the retained
dual-radio list without starting a scan. The existing multi-AP selection path
remains separate from this single-AP handoff.

In ordinary BLE discovery, tap the receiver line below the filter/header to
open the C5 companion. S3 discovery continues while that pane is displayed.
The C5 pane shows its own 32-row bank, typed addresses, latest original-ADV
RSSI, timing and a bounded 31-byte advertisement preview. Rev97 adds `SIGNAL`,
`NAME A-Z` and `SCAN ORDER` controls, plus `ALL AGES` / `WITHIN 30S`.
The recent view admits only observations whose known conservative age upper
bound is at most 30,000 ms. Unknown, rewound or saturated timing is excluded.
Rows display an age bound, `RECENCY?` when that bound no longer proves recency,
or `AGE UNKNOWN`. Sort/filter operations perform no radio work. A tap retains
the exact painted typed observation even if a new bank arrives meanwhile.
An empty recent view reports `NO CONFIRMED RECENT C5 RESULTS`; absence from
that filter does not prove an old report or an absent device.
Its selected row
can enter `FOX THIS ADDRESS`; S3 rows retain their existing Fox Hunt route.
Fox Hunt displays separate S3 and C5 signals. Start/Stop on the companion
controls both receiver sessions. Counts remain per receiver rather than being
presented as a deduplicated count of physical devices.

## Implementation boundaries

The S3 Wi-Fi worker is temporary and uses an exclusive Wi-Fi owner plus BLE
admission guards. Its normalized result bank and SDK readout buffer use
PSRAM. The rev94 Wi-Fi view store measured 5,648 bytes of optional PSRAM. The separate
coordinator host model grows from 10,784 to 10,792 bytes (+8) for cached-handoff
state. These are distinct stores, not one combined allocation. The view avoids repeatedly
repainting an unchanged completed page. The optional BLE companion store
uses 7,336 bytes of PSRAM and adds no worker task. These sizes describe the
host-compiled model structures; live driver heap and stack headroom require
device measurements.

The BLE list addition allocates no heap and adds no task. It uses a 32-byte
stack index map, bounded display-name scratch space and six declared bytes
of static control state before linker alignment. It neither moves packet
records in place nor merges C5 data into S3 classification or SD archives.

C5 Wi-Fi leases use a C5-issued 64-bit epoch; BLE uses a boot identity,
C5-issued monotonic job ID and a full-width session epoch. Stale commands do
not acquire a newer session, and byte-sized legacy generations are not used
as the sole identity fence. Bounded SPI steps keep bulk result retrieval out
of a single synchronous UI call. Software deadlines do not guarantee an SDK
transaction can be interrupted at the exact deadline.

## Radio assignments

| Activity | ESP32-S3 | ESP32-C5 |
| --- | --- | --- |
| Wi-Fi-only scan, both receivers available | Passive 2.4 GHz scan | Passive 5 GHz scan |
| Wi-Fi scan while S3 is reserved | Existing S3 activity continues | Both Wi-Fi bands, with degraded-mode status |
| Mixed Wi-Fi/BLE Recon or Wardriving | BLE | Both Wi-Fi bands |
| BLE-only discovery | Existing BLE discovery | Complementary passive BLE observation |
| Selected-device BLE fox hunt | Local typed-address RSSI measurements | Independent typed-address RSSI measurements |

An assignment never authorizes taking a radio from a portal, connection,
capture, monitor or another owning session. A rejected or uncertain start
cannot be interpreted as an owned scan. Stop applies only to the session
which accepted that start. Failed cleanup retains ownership and an explicit
error until the underlying resource is known to be released.

The C5 cannot scan both Wi-Fi bands simultaneously by itself. Separating the
bands across two chips can reduce sweep time when both are free. Espressif's
[S3 coexistence guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/coexist.html)
and [C5 coexistence guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c5/api-guides/coexist.html)
describe the shared radio and time arbitration within each chip. Running every
protocol on both chips continuously is therefore not the default policy.

## Identity and evidence

Wi-Fi rows retain their receiver provenance. An S3 observation has no C5 scan
index or generation. A handoff resolves the exact chosen BSSID, channel,
security and SSID in a proven C5 snapshot. Cached reuse additionally binds
the original source generation, source index and receipt, and requires a
matching completed ordinary publication. Starting a new attempt invalidates
that eligibility before even the INFO exchange; failed validation invalidates
it too. Canceling cached validation stops no radio because it owns none.
The receiving AP workflow retains the original band, receipt and full-width
proof; a 5 GHz-only bank cannot satisfy an all-band inventory request. Never
reuse a position from a merged or sorted display as a C5 command index.

The S3 Wi-Fi SDK inventory supplies aggregate scan records. Its readout time
is a receipt time, not a beacon callback timestamp; it must not enable the
existing frame-recency flag. Its NUL-terminated SSID field also does not
reconstruct bytes after an embedded NUL in a raw beacon SSID element.

BLE identity comparisons include address type. Different receivers retain
independent RSSI and timing evidence. Cross-radio duplicate observations do
not establish unique physical-device counts, and two received reports do not
necessarily represent two transmitted packets. A C5 observation must not
borrow S3 raw-advertisement, scan-response or action-eligibility evidence.

The existing C5 scanner can request extended 1M and coded-PHY observation in
the current build configuration, but this path has not received physical
acceptance with controlled extended/coded advertisers. S3 native extended
advertisement reception remains disabled; its existing legacy scan path
retains names and scan-response evidence through its own discovery workflow.
This is a per-receiver limitation, not a claim that the whole device lacks
extended receive code. Actual coverage depends on broadcasters, the installed
SDK/controller, antennas and RF conditions. See the
[S3 native receive gap](BLE_EXTENDED_RECEIVE_GAP.md).

Two receivers on the same handheld are not a calibrated direction-finding
array. Fox hunting presents each receiver's signal and age independently;
it does not infer bearing, exact distance or triangulation from their RSSI.

## Acceptance required

- Compare repeated Wi-Fi-only scans against the prior C5-only baseline using
  the same dwell/country policy and AP environment. Record completion latency,
  per-band observations, retained/dropped inventory and internal/PSRAM minima.
- Verify Stop, Back, failed starts, C5 resets and repeated restart, including
  transitions to ordinary AP tools, mixed Recon and Wardriving.
- Verify that sorting and refreshing cannot change the selected identity;
  disappearing or changed APs must fail fresh reacquisition explicitly.
  Verify cached handoff rejects changed bank/session proof without implying
  it has reobserved the AP, and retains the original age and band coverage.
- Compare BLE observations on each receiver using controlled legacy and coded
  advertisers. Check duplicate handling, address types, report age and buffer
  truncation without assuming every address is a different device.
- Check fox hunting with a selected owned advertiser and movement, including
  a device heard by only one receiver, stale samples and Stop/Back recovery.

Faster discovery and improved coverage are goals to measure. Compilation and
host fixtures alone cannot establish either result on the Cerberus board.

## Rev94 recorded validation - 2026-11-01 UTC

The final Windows checkpoint passed 22 targeted runners against stable inputs.
This includes all eight new radio suites and the affected scan observation,
probe/relation evidence, packet-monitor worker, incremental SPI readout,
handshake, Watch timing, Recon parallelism, BLE tracking/discovery UI and
build-report/package-proof regressions. The complete configured 115-suite
matrix was not rerun for this checkpoint; it remains available through
`tools/run_host_checks.py` and the Windows/Linux workflow.

Host checks used Python 3.14.2 with Zig 0.14.1. Native builds used Python
3.12.14 and PlatformIO 6.2.0 with the repository's pinned ESP-IDF/toolchains.
All five native targets and the existing native gates passed on the stable
repeat build:

| Target | Application image bytes |
| --- | ---: |
| S3 main | 1,866,864 |
| Onboard C5 | 1,658,160 |
| S3 programming bridge | 255,248 |
| Expansion C5 WROOM | 926,400 |
| Expansion C5 XIAO | 930,624 |

The matched S3/C5/bridge development package passed the verify-only package
checks for hashes, chip headers, partitions and image bounds. Its three app
hashes match the stable native report. The final native and targeted-host
input fingerprints matched that rev94 checkout. Generated receipts are
`artifacts/integration/build-report.json`,
`artifacts/host-checks/rev94-radio-checks.json` and
`artifacts/build/matched-build.json`.

Initial validation exposed obsolete production-extraction fixtures for the
new scan internals; those fixtures were updated without dropping their
lifecycle assertions. The initial native run also generated SDK configs.
Earlier failed/changed-input reports were retained before the final passing
checks. Visual host previews verified the C5 list/details, and the Fox Hunt
render test confirms that pending target changes show neither receiver's
previous target sample under the new identity.

At that build checkpoint no device had been flashed and no hardware acceptance
was claimed. The subsequent installation below is a separate evidence record.
This development branch remains a local follow-up to the submitted rev93 contribution.

## Installation and initial device checks - 2026-11-01 UTC

The matched rev94 S3 application and C5 firmware 61 were installed on the
connected Cerberus board. All three images for each programmed target passed
the loader's hash verification. The running application identifies as Cerberus
Unchained rev94, the C5 reports firmware 61 online, and the SD card is mounted.
The previous S3 flash and all declared C5 partitions were backed up and their
digests checked before programming. Private backups and serial logs remain
local under `artifacts/device-rev94/`; they are not contribution assets.

The shared esptool installation disappeared between the S3 backup read and its
verification subprocess. No firmware had been overwritten at that point.
An isolated esptool 4.11.0 environment completed verification and the paired
installation. A subsequent local log-wording assertion was corrected after
the tool had already returned success with a matching digest. Both earlier
failure logs were preserved. This tool interruption was not a device failure.

The owner reported that two ordinary Wi-Fi scans finished with results and the
`S3: 2.4 GHz + C5: 5 GHz` assignment, then confirmed Wi-Fi Stop, restart and
Back, followed by activity in the C5 BLE companion alongside S3 discovery.
The subsequent console snapshot records three completed C5 Wi-Fi sweeps with
no recorded scan failures. The two sampled completed banks contained only
5 GHz records (nine and fourteen), with C5 durations of 3,402 and 3,403 ms.
These confirm the C5's band assignment and repeat completion; they are not
measured end-to-end UI latencies or a comparison against the previous firmware.

| S3 diagnostic | Cold boot | First post-Wi-Fi snapshot | Active dual BLE |
| --- | ---: | ---: | ---: |
| Free internal bytes | 113,832 | 104,804 | 46,588 |
| Minimum free internal bytes | 113,064 | 68,256 | 45,852 |
| Largest free internal block | 69,632 | 31,744 | 31,744 |
| Free PSRAM bytes | 1,428,324 | 1,410,108 | 1,382,068 |
| Task count | 10 | 12 | 14 |

The first scan initializes shared network/event infrastructure which survives
driver cleanup; the cold and warm baselines are therefore distinct. These
snapshots alone do not prove the absence of a leak. The exercised USB console
stack low-water mark was 512 bytes; further native and runtime stack checks
remain necessary when adding diagnostics. The valid crash record was already
present before installation, while the new application reported no previous
reset fault.

During the BLE sample, S3 discovery retained 26 rows with no scanner error;
the C5 independently reported its BLE-active radio state. The existing console
does not expose the new companion's detailed bank and sweep state, so its
visible activity is owner-reported. The SPI mutex-timeout counter increased
from zero to one during these interactive checks; no driver error was recorded.
The snapshot does not identify which request contended for that mutex.

The owner then stopped BLE, left Bluetooth, and reported `S3 unavailable` in
the subsequent Wi-Fi scan. Diagnostics confirmed that both S3 BLE discovery
ownership and the host owner were released, while the host remained initialized
and synchronized. Internal free memory remained 46,588 bytes: 18,948 below the
scanner's 65,536-byte admission floor. This is a confirmed retained-memory
limitation of rev94, so the intended split is not yet maintained after BLE use.
The C5 fallback remains available; lowering the admission floor or assuming all
BLE memory can move to PSRAM is not an established fix. This transition is the
priority for the next candidate.

Dual Fox Hunt, repaired post-Bluetooth split admission, mixed-mode transitions,
sustained operation, and comparative timing remain separate acceptance checks.
Earlier active Wi-Fi errors and full Marauder parity are not resolved by this
scan-only evidence.

## Full host matrix follow-up

The first full run completed all 115 suites on stable inputs: 114 passed and
the older relationship-navigation fixture failed to compile because it lacked
the new retained-dual-view return flag. The fixture was updated without any
firmware change. Its existing seven Back-state cases and seven explicit-only
scan starts remain, with an added client detail -> clients -> AP -> retained
dual-view case that rejects an implicit scan, extra relation read, or changed
selection on return.

The complete repeat run passed **115 of 115 suites** with no input changes,
using Python 3.14.2 and Zig 0.14.1. The first failed run and the final logs and
receipts are retained locally. These host results do not override the hardware
memory limitation recorded above. No additional native build was needed for
the fixture/documentation changes; the installed firmware inputs remain
identical to the already-built matched rev94 package.

## Rev95-rev96 warm-transition follow-up

Rev95 introduced cached local dual-radio diagnostics and changed eligible
NimBLE host allocations to the supported external-memory configuration.
Rev96 then moved three directory metadata banks to stable, lazy PSRAM storage,
reclaiming 21,744 linked internal bytes. Both revisions were built and
installed; their native, host and installation details remain in
[radio memory recovery](RADIO_MEMORY_RECOVERY.md).

On rev96, the owner confirmed two warm BLE Stop/Back-to-Wi-Fi cycles with the
split assignment. The independently captured console record contains one
completed job: 19 S3 APs and 11 C5 APs in 4,609 ms, with split enabled,
fallback disabled, no scan/UI/radio error, and all owners/cleanup released.
Free internal RAM settled at 88,036 bytes, 22,500 above admission. This is
one measured console job plus the owner's two-cycle confirmation, not two
independently measured jobs or a sustained-load result. The SD-files/folder/
Back, saved-log list, script list and following-scan acceptance remains pending.

## Rev97 source and focused-host checkpoint

Cached C5-origin AP handoff validates owned status, expected full-width proof
and owned status again, with a final export fence. Tests exercise cancellation
without foreign radio Stop, failed/replaced sessions and banks, preserved
receipt/band metadata, and invalidation when a new scan attempt begins. The
receiving cache rejects narrower coverage for an all-band request. An
S3-origin target retains fresh C5 all-band reacquisition.

The focused `check_dual_wifi_scan.py`, `check_dual_wifi_live.py` and
`check_wifi_scan_cache.py` fixtures cover the coordinator, actual UI handoff
and receiving cache. `check_dual_ble_ui.py` covers actual C5 list controls,
bounded ordering, uncertain timing, frozen typed selection and no radio work
on sorting/filtering. These focused checks pass with Python 3.14.2 and
Zig 0.14.1. The rev97 ledger generator and `--check --upstream` pass against
the clean pinned develop reference; its 95 statuses remain 73 partial,
8 pending, 10 needs-review and 4 hardware-limited. No status was promoted to
complete and these counts are not a completion percentage.

This checkpoint records source review, host fixtures and inspected UI previews.
Rev97 native-build, package, installation and physical acceptance remain
separate from this record. Dual Fox Hunt, controlled C5 extended/coded receive,
S3 native extended reception, SD-list acceptance, sustained load and full
Marauder parity remain open. See [the rev97 milestone](DUAL_RADIO_REV97.md).
