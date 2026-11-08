# Radio memory recovery candidate

Rev95 restored one measured BLE discovery -> Stop/Back -> S3 2.4 GHz plus C5
5 GHz scan, but left a narrow internal-memory reserve. Rev96 adds headroom.
The owner reports repeated split success on rev96; captured telemetry establishes
one completed warm scan so far. Directory browsing and a following scan remain
pending. A successful first scan is not full lifecycle acceptance.

## Observed problem

On rev94, the owner confirmed initial split Wi-Fi scans, Wi-Fi Stop/restart/Back,
and activity on both BLE receivers. After leaving BLE, the S3 host was still
initialized and synchronized but had no owner. Internal free RAM remained
46,588 bytes, below the Wi-Fi scanner's 65,536-byte admission requirement.
The screen correctly showed S3 unavailable and selected C5 all-band fallback.
This establishes a retained-memory limitation, not a reason to report the
split feature fully optimized. Full measurements and evidence limits are in
[the rev94 scan record](DUAL_RADIO_SCANNING.md).

## Candidate changes

The S3 selects the supported NimBLE `EXTERNAL` allocation mode with its existing
explicit PSRAM policy. Generic malloc remains caps-only. Controller/DMA memory,
normal task stacks, packet-pool counts and the resident host lifetime are
unchanged. Eligible host allocations move to PSRAM; this is not a claim that
every Bluetooth allocation is external.

The shared scanner/advertiser/central admission path checks that PSRAM is
initialized and registered before entering the external-only allocator. A
board without PSRAM can still boot; BLE admission returns `ESP_ERR_NO_MEM`.
This presence guard does not guarantee success under later PSRAM exhaustion.
The SDK's ordinary allocation-failure behavior is unchanged. No lower Wi-Fi
admission threshold, host teardown cycle, reduced scan pool or global malloc
policy change is introduced.

Fixed lifecycle samples record heap counters before/after initialization,
after synchronization, on release and on refusal. They describe process-wide
heap at those boundaries, not precise Bluetooth allocation totals. Largest
block queries remain in the explicit memory command because they walk the heap.

An unknown C5 firmware cache now triggers one passive status probe inside the
existing transport deadline. Confirmed older firmware remains unsupported;
a transient timeout is no longer treated as proof of an old peer. Existing
session/target ownership and stale-reply fences remain in place.

## Diagnostics

| Command | Scope |
| --- | --- |
| `wifi_dual` | UI-published assignment, counts, errors, cleanup, rebind and S3 ownership |
| `dual_ble` | UI-published companion state and independently bound S3/C5 Fox samples |
| `ble_memory` | Cached S3 BLE lifecycle heap samples |
| `memory` | Current heap counters and largest-block queries; existing C5 status request |

The three new commands read fixed local snapshots. They do not start RF,
query C5, renew leases, allocate memory or read UI-owned stores directly.
Publication sequence/time identify the snapshot; cached freshness applies at
that publication time and is not refreshed by repeatedly reading the console.

The BLE UI publisher uses a local scanner getter while Fox Hunt is bound. That
getter takes the scanner mutex and can reconcile a host reset; the publisher
is therefore not a hard real-time or strictly side-effect-free operation.
Similarly, the existing SPI software deadline cannot interrupt an in-flight
SDK polling transfer. No hard touch-latency bound is claimed.

## Rev95 validation

Focused host checks pass for the actual host in INTERNAL, EXTERNAL and
Bluetooth-disabled configurations, missing PSRAM and repeated claims,
ownership races, scan lifecycle, immutable target publication, cached snapshot
coherence, clock/sequence limits, console formatting, and firmware-cache
recovery. All 119 suites passed with no changed source/configuration inputs
in the full rev95 run; the final receipt and logs are retained under
`artifacts/host-checks/rev95-final-full-run`. Synthetic console checks
validate the stack-checker's parser contract, not native stack frames.

All five native targets pass with unchanged inputs. The generated S3 config
selects `CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL` and
`CONFIG_SPIRAM_USE_CAPS_ALLOC`. Native console frame analysis passes with
1,200 bytes beyond the measured frames within the 4,096-byte allocation;
this is not a whole-program stack bound. The isolated build environment uses
the same compiler/SDK versions and compiler binary hashes as the rev94 receipt.

The matched development package
`cerberus-unchained-0.1.0-dev.rev95-20261101T035627Z` passes source, image hash,
chip header, partition and image-bound verification. It contains S3, onboard
C5 and the temporary USB bridge. The two expansion targets are compile-tested
separately; no expansion hardware is attached or accepted.

The paired rev95 package was installed on the development board. Each of
the three images for the bridge, C5 and S3 passed flash hash verification.
Verified backups cover every updated region; the earlier full S3 backup is
also retained. Both chips returned online, the display initialized, SD mounted,
and `wifi_dual`, `dual_ble` and `ble_memory` returned normally. Local installation
evidence is retained under `artifacts/device-rev95`.

The settled pre-BLE baseline is 113,524 bytes free internal RAM (69,632-byte
largest block) and 1,428,324 bytes free PSRAM. BLE has not initialized at this
baseline; it does not demonstrate warm-memory recovery. The current boot
reports no preceding reset fault. An older stored crash record remains and
is not counted as a new crash.

The first explicit completed Wi-Fi scan after BLE Stop/Back produced 9 S3
results and 17 C5 results in 4,959 ms. C5 reports all 17 on 5 GHz; its own
sweep took 3,403 ms. Both radio owners and pending cleanup were clear, and
scan/UI/radio errors were zero. Earlier telemetry before the explicit START
still had generation zero, so the preceding display report alone was not
treated as scan evidence.

After both subsystems initialized, internal free RAM settled at 66,292 bytes,
with a 31,744-byte largest block and 30,248-byte observed minimum. This leaves
only 756 bytes above the 65,536-byte admission floor. The default network/event
infrastructure remains initialized after Wi-Fi deinit; the threshold pass does
not establish a useful reserve. SPI mutex timeouts reached four during this
short workflow; driver errors remained zero. No zero-contention claim is made.

## Rev96 headroom candidate

File, saved-log and script directory metadata use three internal BSS banks
totaling 21,760 bytes. They are ordinary CPU-accessed records, not DMA or ISR
buffers. Rev96 replaces them with three independently allocated PSRAM banks
whose successful allocations remain stable until reboot. The expected gross
BSS reduction is 21,748 bytes before link alignment. The native ELF confirms
21,760 bytes of bank symbols becoming 12 bytes of pointers. Total `.dram0.bss`
shrinks from 167,120 to 145,376 bytes (21,744 bytes after alignment), with
`.dram0.data` unchanged.

All five directory-list producers use the guarded wrapper. Allocation/storage
failure clears visible counts, retains existing allocated banks, and permits
later explicit retry. Selected names/entries keep their existing copy-before-
reload behavior. There is no internal-memory fallback: without available
PSRAM these list pages report a memory error while Back/Reload remain usable.
Background SD logging and USB storage services are unchanged.

All five rev96 native targets and all 120 host suites pass on unchanged inputs.
Native evidence is under `artifacts/integration`; the host receipt/logs are
retained under `artifacts/host-checks/rev96-full-run`; the ELF comparison is
`artifacts/rev96-directory-memory-native.json`.

The matched rev96 package
`cerberus-unchained-0.1.0-dev.rev96-20261101T042131Z` passed source/image/partition
checks and is installed. Both chip applications and the temporary bridge had
all image hashes verified; verified rev95 backups were retained first. Both
chips returned online, the display initialized and SD mounted. Evidence is
under `artifacts/device-rev96`.

The settled cold internal heap is 190,220 bytes total, 135,268 bytes free and
94,208 bytes in its largest free block. Free internal RAM therefore improved
by exactly 21,744 bytes over rev95. PSRAM is 1,428,324 bytes free before BLE or
directory banks initialize.

After BLE Stop/Back, the captured rev96 split scan returned 19 S3 APs and 11 C5
APs in 4,609 ms. The C5's own 5 GHz sweep took 3,402 ms. Split was true, fallback
false, scan/UI/radio errors zero and all radio owners/pending cleanup clear.
Internal free RAM was 88,036 bytes (22,500 above admission), with a 38,912-byte
largest block and 51,020-byte observed minimum. A later UI/idle read retained
the same 88,036-byte free value. This is short workflow evidence, not a sustained
load or comparative-speed result. SPI mutex timeouts reached one; driver errors
remained zero and the four startup READY timeouts were unchanged.

The owner answered that both requested repetitions finished with the split.
The diagnostic capture had one completed C5 Wi-Fi job and S3 job 1, so it is
not used as independent evidence of two completed transitions. The separate
SD-files/folder/Back, saved-log list, script list and following Wi-Fi scan check
is still pending. See `artifacts/device-rev96/repeated-warm-scans.txt`,
`sd-progress.txt` and the installation receipt for the measured scope.

Full Marauder parity, active Wi-Fi errors,
expansion/GPS hardware, calibrated Fox Hunt performance and sustained load
remain separate work.
