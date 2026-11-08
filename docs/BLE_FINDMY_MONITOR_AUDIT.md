# Proposed dual-receiver Find My Monitor

This is the design audit recorded after [rev113](DUAL_RADIO_REV113.md), before
the monitor changes were implemented. The findings below describe that source
revision. The [rev114 local candidate](DUAL_RADIO_REV114.md) now implements
the shared monitor path; its validation and remaining physical acceptance are
recorded separately. Rev113 remains installed. The wider Marauder port remains
incomplete.

Audited Cerberus source: `67daaf4e8de541eccd51d266057fac33453e6dcc`.
The pinned Marauder checkout at `../integration/marauder-develop-2026-10-29`
is commit `79cc6bcba1db6679baee1dab2b0c1ad5010e6363`; this audit makes no claim
about a newer upstream revision. Source line references below describe those
two revisions. Cerberus UI filenames are under `s3_main/main/`; upstream
filenames are under `esp32_marauder/`.

## Existing behavior and gaps

Upstream `MenuFunctions.cpp:5505` opens `BT_SCAN_AIRTAG_MON`.
`WiFiScan.cpp:1231–1322` recognizes Apple, FMNA and DULT hints, updates an
existing address's RSSI and last-seen time, and appends new observations.
The monitor loop at `12796–12829` periodically restarts scanning and displays
RSSI, address and age. `RunBluetoothScan()` enables active scanning at `7356`.
This recurring observation display is separate from the sound and live-action
handlers.

Cerberus already has dual-receiver family classifiers, separate original and
matched-response evidence, live snapshots, frozen selection and the v6 SD
codec. The dedicated monitor does not use that full path:

- The ordinary Find My list labels its entry `S3 MONITOR`
  (`sentinel_live_ui.h:602,1156`).
- `s3_findmy_monitor_ui.h:92,159` reads S3 rows only after each completed
  eight-second sweep, using separate generation getters, then starts another
  active sweep. `dual_ble_live_ui.h:89` enables the C5 companion for the ordinary
  list, C5 pane, Analyzer or Fox Hunt; entering Find My Monitor stops it.
- The monitor retains at most 32 typed-address rows, with 120-second hint expiry
  (`s3_findmy_monitor.h`). Original advertisement and matched-response hints
  have separate times. These are observations of addresses, not confirmed
  physical tracker identities.

Three integration weaknesses should be addressed with the new presentation:

1. Paint records only address and type (`s3_findmy_monitor_ui.h:20–28`), then
   tap searches the mutable cache again (`228–246`). A newer observation with
   the same typed address can replace the evidence between paint and tap.
   Selectable rows are also counted before the complete draw succeeds. Reuse
   immutable painted proofs and enable row selection only after a successful paint.
2. Monitor selection clears the full source time and epoch (`240`), unlike
   `s3_ble_discovery_live_ui.h:280–291`. Its selected S3 SD path therefore cannot
   preserve the newer exact source-clock and response provenance. That evidence
   must travel with the selected copy; a later scanner query cannot reconstruct it.
3. The dedicated monitor has its own generation and ownership loop. Its tick
   returns when the page is no longer visible, while `asleep` suppresses drawing
   without canceling acquisition. The auto-sleep exclusion at `ui.c:12348` does
   not include the monitor. Define and test sleep and navigation behavior so
   ownership cleanup continues until release.

## Recommended scope

Make the dedicated monitor another presentation of the existing discovery
owners and committed observation banks. Keep one `Find My Monitor` entry from
the Find My family list, lock the family to Find My while it is open, and retain
receiver choices `ALL`, `S3` and `C5` with simple Start and Stop controls.
Show each receiver's acquisition state, update or sweep count, and error
separately. A total observation count must not imply a count of unique devices.

- Extend discovery eligibility separately from ordinary-list painting in
  `s3_ble_discovery_live_ui.h:55–66` and `dual_ble_live_ui.h:89`. Use the existing
  S3 discovery start, copy, stop and release APIs with exact epochs
  (`s3_ble_discovery_impl.h:55,139`). They already support active scanning and
  incremental proof copies without waiting for sweep completion.
- Request C5 focused family 2 through `dual_ble_ui_sync_family()`. Preserve the
  explicit `RESPONSE REQUESTS` preference and the passive default. Focused
  passive acquisition requires firmware 66; response acquisition requires 67;
  compact transfer is optional from 68. An unsupported C5 remains visibly
  unavailable while S3 scanning stays usable. Recon and Wardriving keep their
  current behavior.
- Reuse the committed C5 bank and S3 inventory, with at most 32 observations per
  receiver. Keep receiver, address and address type distinct, even when address
  bytes match. Admit only source-valid Find My advertisement or matched-response
  hints. Aggregate names and UUID fields cannot substitute for packet evidence;
  age-only repaint should not repeat parsing.
- Display advertisement and response facts separately. S3 exact ages use the
  retained epoch and full-width source clock. C5 ages remain conservative upper
  bounds or unknown. An upper bound above 30 seconds does not establish that an
  observation is stale; label its recency unconfirmed. Response `UNKNOWN` and
  `NONE` remain distinct, and `NONE` does not prove that no response occurred on air.
- Reuse four frozen painted rows and the existing selected-detail paths. C5
  keeps its own raw bytes, receive metadata, hints and copied boot/epoch proof.
  It never becomes S3 action evidence. Fox Hunt receives only the explicitly
  selected typed target. Beep, GATT controls and their eligibility remain outside
  this receive-monitoring change.
- Stop, Back, sleep, selection and saving clear acquisition intent, including
  pending C5 INFO work, before waiting for both exact owners. Continue cleanup
  beyond 12 seconds with a visible error; do not restart automatically or stop
  an unrelated owner. Preserve the monitor return destination through detail,
  Fox Hunt and SD pages. Retrying one failed receiver should preserve the other.
- Reuse `ble_archive_live_ui.h:75,153,231`: freeze inputs at one save clock,
  drain both owners, then submit the existing worker. The v6 schema already
  represents this evidence. Keep the existing `SAVE SD` and `SAVE BOTH` flows
  and describe their retained-observation scope accurately; it need not equal
  the currently visible filtered subset.

Retention differs between receivers: S3 keeps bounded history, while C5 exposes
its last committed bank. Preserve that distinction. A monitor-only 120-second
display filter can retain the previous S3 horizon without altering archived
evidence. Unknown C5 ages or large upper bounds need explicit retained or
age-unknown presentation. Symmetric C5 history would require a deliberate,
separately measured allocation rather than an unnoticed extra bank.

## Resource baseline and validation

The retained rev113 native measurements provide this baseline:

| Existing allocation | Native size |
| --- | ---: |
| S3 dual-BLE coordinator | 21,496 bytes, including two 10,240-byte row banks |
| S3 discovery store | 21,440 bytes, including 1,280 bytes for four painted C5 rows |
| C5 optional response context | 11,736 bytes |

Reuse those stores. Remove the obsolete monitor-only cache only after all its
callers migrate. Do not claim RAM savings before measuring the resulting native
images. This plan needs no new task, RF mode, C5 command, full-row stack array
or C5 history bank. The existing four-exchange/eight-millisecond C5 tick budget
continues to apply, including its in-flight transaction limitation.

Extend the existing focused checks:

- `tools/check_s3_findmy_monitor_ui.py` and its production UI fixture: mixed
  receivers, response-only hints, typed-address collisions, failed paint,
  replacement between paint and tap, independent errors and retry, pending
  start, held cleanup, Stop, Back, sleep, clock boundaries and source counts.
- Discovery UI, live and session fixtures: monitor eligibility without ordinary
  list overpainting, loss of full-width provenance or automatic restart.
- `tools/check_dual_ble_ui.py`: frozen C5 detail and Fox Hunt routes returning
  to the monitor.
- `tools/check_ble_archive_live.py`: capture from the monitor, delayed owner
  release and v6 reopen, retaining separate clocks and hints after live banks
  change. Existing parser, family, owner and archive-codec checks remain.

Host, render and native checks establish routing, evidence integrity and bounds.
Physical acceptance still requires controlled broadcasters heard by one or both
receivers, matched-response and extended-original cases, repeated transitions,
real SD save/reopen, dense-inventory latency and heap/stack measurements. Rotating
addresses remain distinct observations; neither ownership nor a person's
identity follows from a format hint.
