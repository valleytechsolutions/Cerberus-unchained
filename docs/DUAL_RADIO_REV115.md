# Rev115: independent ordinary BLE discovery and release barriers

Rev115 is a local candidate extending [rev114's shared monitoring](DUAL_RADIO_REV114.md)
to make ordinary Bluetooth discovery tolerate an unavailable receiver. Rev113/C5
firmware 68 remains installed. Full Marauder parity and physical acceptance are
unfinished. The coverage ledger remains the explicitly labelled `rev113-source`
snapshot; this milestone does not promote its parity statuses.

## Behavior

An ordinary discovery request now exists outside the optional S3 discovery
allocation. An S3 allocation failure or terminal session error therefore does
not stop a healthy C5 request. The main list displays the actual source failure
and keeps the C5 receiver view reachable, including when no S3 store exists.
An empty unavailable-source shell cannot expose old Watch rows as ordinary
discovery results. C5 retained results keep their source and evidence semantics.

The C5 receiver view offers separate RETRY S3 and RETRY C5 controls for failed
sources. Retrying one receiver leaves the healthy receiver's acquisition intact.
Family and response preferences do not silently retry a failed receiver; an
explicit new Start can retry both. Allocating the discovery store or redrawing
a page does not start acquisition. Returning to retained details or results
leaves discovery stopped.

Stop remains available when both receivers have failed but the overall request
is still active. Pressing Stop again while either owner is releasing remains a
Stop operation and cannot queue an implicit restart. Both ordinary row-selection
paths freeze the painted evidence before cleanup and wait for both owners before
opening details. Tools and dedicated-monitor navigation use the same release
barrier, even without an S3 store. A later explicit navigation choice cancels a
previous deferred destination.

Stop, outer Back, sleep and saving cancel pending work, including a C5 request
waiting for INFO. Saving first freezes available evidence and then cancels the
originating ordinary scan, whether capture succeeds or fails. Cleanup does not
cancel unrelated monitor, Fox Hunt or Analyzer work. The existing global owner
ticks perform release; controls and render paths add no SPI exchanges.

No new discovery bank, task, radio mode, wire command or archive format is
introduced. The existing C5 global exchange budget remains in force. C5 firmware
capability revision remains 68. This milestone changes ordinary discovery and
its navigation/save lifecycle; the Analyzer's independent intent audit remains
separate work.

## Host validation

All 124 host suites pass on unchanged source inputs. Their retained receipt
binds every log by hash. The production discovery/UI fixture passes 1,888
assertions, and the archive capture/controller fixture passes 4,335 checks.
The composite tests run the actual S3 session, C5 UI owner adapter, C5
coordinator and global UI ticks, with simulated radio boundaries and a bound
version-1 C5 peer. Existing controller tests retain snapshot-protocol coverage.

The added cases cover either source's allocation or terminal failure, source-only
retry without disturbing a healthy epoch/job, explicit new Start, failed-source
preference changes, pending INFO cancellation, held release beyond 12 seconds,
and repeated Stop. A repainted second row cannot replace a pending frozen proof.
No-store navigation and its later cancellation are exercised through the actual
tap handlers. Save tests cover successful and failed capture followed by Back
and a later owner tick without unintended acquisition.

The unavailable-store list refreshes cached C5 status on the existing 200 ms
cadence using one small UI clock outside PSRAM. Tests follow C5 startup,
receiving, stopping, retained results and failure without an extra transport
step or allocation attempt during repaint. Sleeping and hidden pages do not
repaint. The renderer preserves ERROR while presenting STOP where requested,
and changes only the button region when its action changes. Synthetic previews
were reviewed for both-failed and unavailable-S3 states.

The preliminary full run passed 123 of 124 suites. Its legacy completed-bank
fixture lacked declarations for the new ordinary discovery facade; its scope
and fail-fast boundaries were updated. The final full run passes all 124.

## Native, package and physical evidence

All five native targets pass on unchanged inputs: S3 application, onboard C5,
temporary S3/C5 USB bridge, expansion WROOM and expansion XIAO. Their input map
matches the retained host receipt exactly. The native audit compares the verified
local rev114 candidate with rev115; it does not treat rev114 as an installed build.

| Native measurement | Local rev114 | Local rev115 |
| --- | ---: | ---: |
| S3 fixed data and BSS | 173,564 bytes | 173,580 bytes |
| C5 fixed data and BSS | 83,160 bytes | 83,160 bytes |
| Shared discovery store | 21,448 bytes | 21,448 bytes |
| Dual-BLE coordinator | 21,496 bytes | 21,496 bytes |
| Optional C5 response context | 11,736 bytes | 11,736 bytes |
| Archive viewer plus worker type demand | 118,760 bytes | 118,760 bytes |
| Shared scan-view type | 152 bytes | 156 bytes |

The added ordinary intent, pending-navigation state and no-store repaint clock
remain outside optional PSRAM. The shared scan-view type grows by four bytes for
its independent Stop action. The final linked S3 fixed-memory increase is 16
bytes. These figures describe native types and fixed sections, not runtime heap
or peak allocation measurements. Existing discovery and archive buffers do not
grow.

The selected UI entry frame remains 1,072 bytes, `handle_ble_tap` remains 192
bytes and `handle_tap` remains 544 bytes. `live_make_view` grows from 256 to 304
bytes. Inlined or absent helpers are not treated as zero-cost, and these selected
entries do not bound complete call chains or establish runtime stack headroom.

Final S3/C5/bridge application sizes are 1,939,344 / 1,678,960 / 255,296 bytes.
The matched local package is
`dist/cerberus-unchained-0.1.0-dev.rev115-20261101T154644Z`.
Its manifest SHA-256 is
`56a0ee00862fe0853b4f6fd05e3dd03faa14d7f95a4c2baa9336a9609fdc6fbb`.
Image hashes, chip headers, partition tables and image bounds pass the read-only
package verifier. All three application hashes match the retained full matrix.

Evidence is retained under `artifacts/rev115-host-evidence`,
`artifacts/rev115-native-evidence` and `artifacts/device-rev115`.
The native audit SHA-256 is
`9014eb7cf73f1c977ce4e76548e91b91f420b6a12c8c9c4b99c97942fc87503e`.
The local milestone receipt binds the committed source, matched package,
host/native receipts and audit. No image was flashed and no release or pull
request was published. Rev113 remains the installed baseline.

Physical acceptance remains pending for receiver failures and retries, repeated
Stop/Back/sleep transitions, actual SD save/reopen, dense inventories and sustained
heap/stack behavior. Synthetic controller tests and compiled images cannot
establish RF reception, touchscreen behavior or SD durability. No GPS or expansion
backboard is required for this discovery change; their acceptance remains separate.
