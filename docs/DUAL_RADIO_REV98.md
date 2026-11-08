# Rev98: live BLE results and consistent Wi-Fi return navigation

This checkpoint improves passive discovery and AP navigation. It is not a
claim that all Marauder features are ported, optimized or hardware accepted.
The owner is unavailable for touchscreen tests during this checkpoint.

## Changes

The C5 can now freeze a coherent Bluetooth results snapshot while its existing
eight-second listening sweep continues. The S3 reads into its existing staging
bank and publishes only after every row and the final identity check pass.
This removes the requirement to wait for RF completion before beginning C5
result delivery. It does not promise a particular physical latency. S3 and C5
remain separate receivers with separate evidence; their report counts are not
added together as a unique-device or packet count.

The companion UI distinguishes live snapshot updates, completed RF sweeps and
reader time to the first new nonempty results. Stop discards partial results
and releases only its own radio session. Existing v1 peers retain the
completed-sweep path. The protocol, memory model and focused verification are
documented in [live BLE snapshots](DUAL_BLE_LIVE_SNAPSHOTS.md).

Rev97 physical captures exposed an AP navigation inconsistency: the global
Back route honored the dual scan origin, but the AP page's own Change control
unconditionally entered the legacy C5-only list. Scan Again on that list could
advance C5 scans without starting S3 or advancing the dual scan generation.
The rev98 route fix returns dual-origin selections to the retained dual list;
legacy selections retain their existing path. Returning does not itself start
a replacement scan. The added `wifi_dual_rebind_source` diagnostic identifies
the selected receiver without exposing AP identities.

## Verification and installation

All 120 host suites pass on unchanged inputs with Python 3.14.2 and Zig 0.14.1.
The receipt and individual logs are retained in
`artifacts/host-checks/rev98-full-run`. The first full run exposed a missing
dependency in the standalone target render fixture; that fixture now compiles
the full handler and asserts both origin routes. The complete suite was rerun
after the fix. A negative control restoring the old CHANGE assignment fails
the dual-origin routing assertion.

All five native targets pass on unchanged inputs matching the host receipt:
S3, onboard C5, temporary bridge, expansion WROOM and expansion XIAO. The
isolated toolchain uses PlatformIO 6.2.0, ESP-IDF 6.0.1 and GCC 15.2.0.
Receipts, logs, effective SDK configurations and both application ELFs are
retained under `artifacts/rev98-final-native-evidence`. The matched package is
installed as recorded below. Physical latency, callback contention and
touchscreen behavior remain pending.

The completed S3 native build has unchanged linked internal sections:
26,412 bytes of `.dram0.data` and 145,904 bytes of `.dram0.bss`. Its optional
PSRAM BLE coordinator grows from 7,328 to 7,504 native bytes (+176); the cached
BLE diagnostic is 104 bytes. The interrupt-placement gate passes. The known
console frame subset remains 2,896 of 4,096 bytes, leaving 1,200 bytes; this is
not a whole-SDK or interrupt-stack bound.

The onboard C5 native build has unchanged `.dram0.data` at 19,552 bytes and
`.dram0.bss` increasing from 56,496 to 59,808 bytes (+3,312 including alignment).
Its frozen snapshot symbol is 3,304 bytes and the negotiated-version byte is
separate. Reported static RAM is 79,360 of 327,680 bytes; reported application
flash is 1,659,848 of 2,031,616 bytes. These are linked/configured figures,
not the live heap minimum during reception. Comparison receipts and native
type sizes are under `artifacts/device-rev98`.

The C5 snapshot dispatch path has a measured 672-byte core frame chain; adding
the immediate `xQueueGenericSend` frame gives a 736-byte subset of its 4,096-byte
task allocation. The four callers' frames are unchanged from rev97. The native
mutex-take call uses zero ticks. These checks exclude deeper RTOS, ROM, timer,
logging, task-entry and interrupt/context-switch frames; they are not a complete
stack bound or measured runtime headroom. Evidence is in
`artifacts/device-rev98/c5-snapshot-native-stack.json` and `.txt`.

## Installed package and idle boot evidence

The installed paired package is
`cerberus-unchained-0.1.0-dev.rev98-20261101T055514Z`, from source commit
`eda8da95b9f9bc95dc7600492a126c26b839eb36`, with firmware source SHA-256
`b525eb39097ae3b558f87475a404d7db188f209fa5ed789bbfad43989c05d517`.
Package hashes, chip headers, partitions and image bounds pass. The three
onboard application hashes exactly match the full native-matrix images.

First-2-MiB backups of the prior rev97 S3 and C5 were read, checked against the
installed package and verified against each chip. They cover every update
write region; higher flash was preserved, including the older main crash
record. The older complete S3 backup is also retained. Each bridge, C5 and S3
write verified three image hashes after checking the exact ROM/stub identity.

Rev98 boots with C5 firmware 62 online, an active display buffer and SD mounted.
USB storage is inactive and the boot reports no previous reset fault. The
existing 26,240-byte main crash record predates rev97/rev98 and remains present.
No new RF or touchscreen test was performed while the owner was unavailable.

The settled idle read shows 134,652 bytes of free S3 internal RAM, a 94,208-byte
largest internal block, 1,428,324 bytes of free PSRAM and ten tasks. S3 console
stack low-water is 1,100 bytes. C5 reports 59 KiB free and a 54 KiB minimum at
boot; its largest block is not exposed. Startup SPI telemetry shows four
initial READY timeouts with four legacy fallbacks, zero mutex timeouts and
zero driver errors. These figures are idle/startup evidence, not sustained
scan or callback-load acceptance.

Backups, write logs, matching-input checks, boot captures and
`installation-receipt.json` are retained under `artifacts/device-rev98`.
The coverage ledger's `rev98-source-preparation` entry preserves the earlier
source checkpoint; this document records the later build and installation
evidence. Full upstream parity remains open.

## Next owner checks

- Run Bluetooth discovery, open the C5 companion, and confirm live updates
  appear while reception continues. Stop and Back should exit normally.
- Then complete a split Wi-Fi scan. Repeat without resetting to check memory
  recovery and release of both receivers.
- From a completed split scan, open a C5/5 GHz AP and use the AP page's own
  return/change control. Confirm that the dual list returns. Reopen the same
  result, then run Scan Again; diagnostics must distinguish proven reuse from
  fresh reacquisition and show the new split generation when requested.

No test above needs an active Wi-Fi or Bluetooth transmission feature. GPS,
six-radio expansion, extended BLE reception, sustained load and remaining
upstream feature parity have separate acceptance requirements. The
[coverage ledger](MARAUDER_COVERAGE_LEDGER.md) remains explicit about those gaps.
