# Rev93 passive analysis, storage and Wardriving contribution

This contribution brings the completed local milestones from rev82 through rev93 into Cerberus Unchained, preserving its branding and build/deployment conventions. It improves passive Wi-Fi result handling, BLE evidence inspection, Recon recording and inventory, SD error handling, and the Wardriving tab. The earlier [rev81 contribution](CONTRIBUTION_REV81.md) remains the historical baseline.

The broader firmware mission is paused and unfinished. The current [coverage ledger](MARAUDER_COVERAGE_LEDGER.md) tracks 95 upstream mode symbols: **73 partial, 8 pending, 10 requiring review and 4 hardware limited**, plus six separate workstreams. Those symbols overlap with menu states and variants; the counts are not a completion percentage. Existing active radio engines are unchanged by this contribution, and earlier active Wi-Fi errors are not claimed repaired.

## What changes for the user

| Area | Result | Details |
| --- | --- | --- |
| Wi-Fi scan results | Incremental result transfer preserves the previous complete list on cancellation or failure, batches bounded work and exposes clearer readout errors. Exact publication proofs reject mixed banks. | [Batching](WIFI_READOUT_BATCHING.md), [recovery](WIFI_READOUT_RECOVERY.md), [owned polling](WIFI_OWNED_POLLING.md) |
| AP list and analysis | Signal/name/channel sorting and band filters compose with the existing recent-only view. Painted rows retain their original identities. Selected-SSID analysis and Trusted Baseline use the committed S3 cache. | [List controls](WIFI_LIST_CONTROLS.md), [Trusted Baseline](WIFI_TRUSTED_BASELINE.md) |
| BLE analysis | Analyzer adds independent Find My/Flipper facts and saturating family-event totals without merging unrelated ADV and response events. | [BLE receive scope](BLE_RECEIVE_LIST_PARITY.md) |
| SD File Tools | Copy reports read, write, synchronization and close failures instead of publishing a truncated file or showing false success. | [Copy reliability](SD_FILE_COPY_RELIABILITY.md) |
| Recon recording | Explicit recording retains generic completed Wi-Fi/BLE sweep rows, including repeats, with a session manifest and accepted/written totals. | [Recording](RECON_SESSION_RECORDING.md) |
| Recon inventory | Nearby Inventory retains 32 Wi-Fi and 32 typed BLE addresses, copied details and a 64-second additions/expirations view. Source ages and retention policy are labelled separately from RSSI. | [Inventory and activity](RECON_INVENTORY.md) |
| BLE saved evidence | Save a selected observation or retained list, then reopen `.BLE` files in an offline list/detail/raw-data viewer. Loaded observations never enter live targeting. | [Save/open workflow](BLE_SD_SNAPSHOTS.md), [format](BLE_ARCHIVE_FORMAT.md) |
| Wardriving | RECENT NETS ports the separate firmware's 24-event history and newest-ten-distinct view into the main tab, with frozen details and logging outcomes. GPS is optional for viewing unlocated observations. | [Recent Networks](WARDRIVE_RECENT_NETWORKS.md), [source contract](WARDRIVE_RECENT_SOURCE_CONTRACT.md) |

C5 continues dual-band Wi-Fi reception; S3 runs BLE, presentation, bounded analysis and storage orchestration. The new views consume existing observations instead of creating duplicate radio sessions. Both chips contribute through their appropriate roles. View allocations use bounded PSRAM stores; asynchronous storage workers own their copied inputs and clean up explicitly.

## Validation: two different source contexts

The **local Unified rev93 checkpoint** was validated before this contribution was adapted to Unchained. Its final matrix passed **107 host suites and five native targets** against stable inputs. The five targets were S3 main, onboard C5, the S3 C5-programming bridge and two optional expansion-C5 variants. Three S3 image writes were hash-verified. The C5 retained the earlier verified firmware-60 images; it was not reflashed for rev82–93.

That local installation booted with mounted SD, online 40 MHz C5, ten S3 tasks and no reported reset fault. A bounded exercise completed 59 read-only console commands over three cycles plus final memory/health checks, observing at least 544 remaining console-stack bytes. It checked command completion and selected diagnostic fields, not every command's content: `help` printed its echo and prompt without its intended body in the captured rev92/rev93 samples, an open diagnostic limitation. These observations are neither a sustained-load test nor a universal stack/latency bound.

The first local rev93 host matrix had five compiler-cache failures and one fixture-routing failure; the failed results were preserved before cache isolation and fixture corrections. All nine production files were unchanged between that run and the final passing matrix. The first local console exercise missed its original eight-second Resources deadline despite a later complete response; the accepted rerun allowed fifteen seconds for Resources, eight for other commands and 119 seconds overall. This was not a failed flash. Separately, the first rev92 candidate exposed a real USB-console stack overflow after its automated checks; it was rejected, the previous image restored, and command-handler isolation plus native/runtime stack checks added before the corrected installation was accepted. Automated passes are not presented as substitutes for that hardware evidence.

The **Unchained contribution is a separate adaptation** with upstream branding/version and portable-tooling changes. It has not been flashed or physically accepted. Fresh local Windows results for these contribution sources are recorded separately in [VALIDATION.md](VALIDATION.md): 107 host suites, five native targets, matched packaging and the additional build/resource checks passed. Historical Unified results must not be relabelled as tests of these binaries. This submission does not publish a firmware release or update a public flasher.

## What remains unverified or incomplete

- Physical touch/selection, repeated transitions and extended operation of the newer list, Baseline and Analyzer controls.
- Real-card Recon recording contents, typed totals, Stop/save/restart, inventory activity, and SD copy fault recovery.
- BLE selected/list save, reboot/reopen, cancellation and repeated offline analysis on hardware. Archives retain at most 31 original ADV bytes, no raw scan response, and historical age at save; they do not implement Marauder JSON import.
- Wardriving Recent Nets while running/after Stop, copied details, logging outcomes and memory-failure paths on hardware. Earlier onboard no-GPS runs remain prior-version evidence.
- Physical GPS and six-C5 backboard operation, native extended BLE reception, controlled Remote ID/CSI accuracy, broader list persistence and complete Marauder workflow parity.
- Earlier reported active Wi-Fi memory/unresponsive-feature regressions, which are outside this passive contribution.

The software READY/batch allowances cannot interrupt an SDK call already in flight. Completed-sweep records are not a per-packet RF archive; address keys and advertised classifications are not authenticated physical identities. Detailed source and timing limits are retained in the feature guides.

## Source provenance and existing notices

The Marauder audit is pinned to develop [`79cc6bc`](https://github.com/justcallmekoko/ESP32Marauder/tree/79cc6bcba1db6679baee1dab2b0c1ad5010e6363), whose source tree matches the inventory revision [`511bb369`](https://github.com/justcallmekoko/ESP32Marauder/tree/511bb3694d0c4dc064888e762bde44fcc9b7f48a). See [baseline evidence](MARAUDER_DEVELOP_BASELINE.md) and the [MIT notice](../LICENSES/ESP32Marauder.txt). Reachable Save/Load Airtags and Recon behavior informed the independent passive workflows; no claim is made that their file formats or callback semantics are identical.

The separate Wardriving reference and inspected file hashes are documented in its [source contract](WARDRIVE_RECENT_SOURCE_CONTRACT.md). Existing [Watch third-party notices](WATCH_THIRD_PARTY_NOTICES.md), SDK/platform notices and optional SD resource provenance remain in place. The existing SD Recon database contains mixed-source material and embedded source/license records; this contribution does not relicense it or replace those notices. No new upstream root license is asserted.

Private receipts, machine-specific paths, captured network identifiers and raw console logs are not part of this public contribution. The public status input contains concise implementation scope and remaining work; previously published rev81 documentation remains available at its original scope.
