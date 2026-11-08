# Rev101: C5 receive metadata without more row transfers

Rev101 is installed as a local development checkpoint. All 120 host suites and
five native targets pass on the final source, including the unsaved-metadata
label. Matched image writes and application boot are verified. Physical
acceptance remains pending, and the full Marauder port is incomplete.

## Retained observation

C5 now keeps an eight-byte envelope beside the same original advertisement's
raw preview, latest RSSI and callback time. It records callback source, original
callback length, extended properties, SID, primary/secondary PHY, controller
data status and independently checked full AD-container layout. This separates
a valid long report with a cut preview from invalid full AD framing. Existing
raw flags remain unchanged; aggregate peer hints still do not become packet
fields, and these metadata fields grant no classification or action eligibility.

The source distinguishes a legacy callback from an extended callback, including
a legacy PDU delivered through the extended event. Receiver build capability
does not determine an individual row's origin. Legacy callbacks have no observed
controller status, SID or PHY, so those fields remain explicitly unavailable.
Invalid metadata becomes a canonical UNKNOWN envelope without changing existing
raw admission. Callback length is at most 255 bytes and is not a reconstructed
advertising train length. The raw preview remains at most 31 bytes.

Only already-admitted complete original reports receive known envelopes.
Incomplete/controller-truncated reports and scan responses cannot refresh the
metadata of a retained older original. No fragments are concatenated. The
independent full-container validator now also runs when NimBLE field parsing
fails, adding bounded work on that path; callback latency is not measured.

## Frozen transfer and selection

Firmware 64 adds optional command 130, `DUAL_BLE_SNAPSHOT_READ_METADATA`.
Metadata freezes beside rows and inventory counters at the same first BEGIN.
The virtual row is 110 bytes: the existing 102-byte row plus eight metadata
bytes. It still uses eleven ten-byte chunks. Existing READ retains its two-byte
final chunk and zero tail, and INFO/version-1 layouts remain unchanged.

The S3 validates metadata domains and their raw-row context before staging.
Unsupported reads discard the whole in-progress staging bank and restart the
old reader on the same frozen handle, retaining the original BEGIN time anchor.
Other malformed or stale responses cannot publish a partial bank. The final
STATUS fence, bounded retries, Stop priority and four-call/eight-ms local tick
policy remain. No additional full bank, task or RF job is introduced.

Painted and selected rows copy metadata with their original typed observation.
The C5 detail page adds **RAW ADV / RECEIVE INFO** tabs while retaining the
existing Back, Fox, Stop and Save controls. Drawing and switching tabs perform
no SPI or radio operations. The receive panel labels callback bytes versus
preview bytes, PHY/SID/properties, controller status and full AD-layout validity.
UNKNOWN metadata is shown as unavailable; it is not reconstructed from current
scan configuration or another row.

This checkpoint does not extend the v3 SD schema. Saved reports still preserve
the rev100 original/response and receiver provenance fields; new C5 metadata
is live/selected only; the receive-info view labels it **RX INFO NOT SAVED**.
Existing archives must not acquire these values from
current device state. See [the saved-report guide](BLE_SD_SNAPSHOTS.md).

## Validation gates

Focused work covers the actual extended callback through normalization, original
storage, immutable snapshot transfer and S3 decode using installed SDK field
widths. It includes legacy PDUs through extended events, callback lengths
0/31/32/255, invalid domains, partial/response exclusion, replacement during
reads, unchanged old final chunks, optional fallback and final-fence/Stop paths.
UI checks cover frozen metadata, unavailable fields, preview versus full framing,
tab hit areas and no transport work while inspecting evidence.

All 120 host suites pass with pinned Python 3.14.2 and Zig 0.14.1 on unchanged
inputs. The receipt and its 120 hash-bound logs are retained in
`artifacts/rev101-verified-host`; its receipt identifies those exact logs and
their hashes. Shared UI harnesses ran sequentially. Focused coordinator, actual
C5 owner, C5-to-S3 integration, adapter cache and UI suites pass. The Watch
timing suite reports 209 collector, 142,363 dispatch, 837 legacy BLE, 874 extended
BLE and 738 Wi-Fi checks. These assertion counts are fixture work, not RF trials.
Rendered raw, extended, legacy, legacy-PDU, malformed and unavailable panels
are retained under `artifacts/device-rev101/ui-verified`; extended/legacy/raw/unknown
layouts were visually inspected at 320x480.

The S3, onboard C5, bridge, WROOM and XIAO native builds pass. Compared with retained rev100 ELFs,
linked data/BSS grows by 32 bytes on S3 and 512 bytes on C5. Measurements are in
`artifacts/device-rev101/native-memory-comparison-final.json` and
`c5-native-memory-comparison.json`. Expansion builds do not establish expansion
hardware acceptance. These linked section measurements do not establish live
heap use or a performance improvement.

Actual S3 rows grow from 112 to 120 bytes; the optional PSRAM coordinator grows
from 7,544 to 8,072 bytes, including its one 110-byte wire scratch. Four painted
rows plus the selected row add 40 named BSS bytes and the tab adds one byte;
alignment/layout changes make the whole-section net different. The C5 live
record grows from 108 to 116 bytes, its 32-row bank from 3,456 to 3,712, and its
frozen store from 3,312 to 3,568. The raw frozen bank remains 3,264 bytes.

Selected native frame inspection finds S3 coordinator tick unchanged at 176
bytes, including inlined metadata validation, and its companion renderer grows
304 to 368 bytes. A periodic rendering path subtotal grows 2,768 to 2,832 within
the configured 8,192-byte main stack. C5's snapshot handler remains 224 bytes
for both formats; its selected callback validation chains are 400 bytes for
extended reports and 432 for legacy reports, versus the prior 384-byte chain.
Its SPI snapshot subtotal remains 672 bytes, or 736 including immediate semaphore
give, within the configured 4,096-byte SPI stack. These are partial frame sums,
not exhaustive bounds or runtime high-water results. Reproduction details and
limits are retained in `artifacts/device-rev101/s3-metadata-native-audit-final.json`
and `c5-metadata-native-stack.json`.

Archive capture uses one local C5 row, with no new stack array. Its save/mapper
frames remain 240/288 bytes, and archive row/document/UI-store sizes remain
232/14,864/31,240 bytes. The final unsaved-info label leaves all inspected native
types and 22 selected frames unchanged from the first rev101 build.

The final native receipt, logs, ELFs and SDK configurations are retained in
`artifacts/rev101-verified-native-evidence`. Matched package
`dist/cerberus-unchained-0.1.0-dev.rev101-20261101T074125Z` passes hash,
chip-header, partition and image-bound verification. Its firmware source digest
is `756d707dd22a970efc0ce5f02585019337caea469e3d43313c0fa13f8777b0ee`.
The packaged applications match the final full-matrix images. Earlier rev101
artifacts and the initial package predate the unsaved-info label and are not the
final installation candidate.

## Local installation and boot

Implementation commit `7dd2a7a64a5faea88d6082e8cf25a8974490eb3d` is installed.
Fresh first-2-MiB S3/C5 backups match the previously installed rev100 images and
verified against device flash. They cover every written sector; higher flash
was preserved. The temporary bridge, C5 and final S3 each completed three image
writes with verified hashes and exact chip/MAC guards. Exclusive write receipts
bind package, manifest, source, image and guard-test evidence. No PR or release
was published.

Diagnostics report S3 `0.1.0-dev.rev101`, C5 protocol firmware 64, an active
display buffer, mounted SD, inactive USB storage and no previous-reset fault.
Settled S3 internal free memory is 134,396 bytes, its largest block is 94,208,
free PSRAM is 1,428,324 and task count remains ten. The read-only console command
sequence reports 652 remaining stack bytes, unchanged from the same preceding
sequence; this is not a runtime worst-case bound. C5 reports 58 KiB free and a
54 KiB minimum. Idle readings do not exercise optional receive/archive buffers.

Startup SPI reports four initial READY timeouts and four legacy fallbacks,
with zero mutex timeouts and driver errors. The preserved 26,240-byte crash
record predates this update; it is not a new rev101 fault. Inspected radio
services and the archive worker are idle after the read-only checks.

Evidence is retained under `artifacts/device-rev101`: the installation receipt,
preflash verification, per-target write receipts, backup receipts,
`boot-confirmed.txt`, `settled-boot.txt` and `final-idle.txt`. This establishes recoverable
installation and boot, not live RF, touchscreen, real-card persistence or
sustained-operation acceptance.

## Pending physical acceptance

- Inspect controlled legacy and extended advertisements in RECEIVE INFO and
  verify source, callback/preview lengths, SID/PHY and framing labels.
- Select a row, allow the live bank to change, and confirm its metadata remains
  the selected original while switching tabs.
- Exercise Stop/Back and a following split Wi-Fi scan without resetting.
- Rev98–100 SD save/reopen, dense discovery, Wi-Fi handoff and earlier Wi-Fi
  errors remain open, as do GPS and expansion-board hardware acceptance.

No GPS or external radio board is required for this receive workflow.

## Next persistence work

A read-only audit proposes an explicit v4 archive suffix containing these eight
receive fields. That would make rows 216 bytes and the maximum file 13,856 bytes,
while old v1/v2/v3 metadata remains UNKNOWN. The version bump must explicitly
retain v3 S3-response decoding; its current branch compares against CURRENT.
The frozen C5 row/provenance already provides the capture boundary, so saving
would not need another receive query or clock sample.

The existing offline raw page could use its second tab for C5 RECEIVE INFO,
while S3/LEGACY keep RESPONSE. Current native-layout arithmetic projects 2,088
additional bytes across optional archive UI/worker objects, not a measured peak
or a completed implementation. Codec compatibility vectors, failed-decode
atomicity, capture provenance, selected/live/save transitions, worker/storage
faults and native type/frame checks remain required. The audit is retained in
`artifacts/device-rev101/next-c5-archive-metadata.md`.
