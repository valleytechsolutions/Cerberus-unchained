# BLE SD snapshots and offline analysis

The verified installed checkpoint is [rev123](DUAL_RADIO_REV123.md), including
the v6 workflow and separate JSON interchange below. Paired application boot,
display buffering and SD mount are verified; physical save/reopen and
touchscreen acceptance are still pending. Earlier sections retain their own
revision-specific evidence and installation history.

## Rev123: separate JSON interchange

[Rev123](DUAL_RADIO_REV123.md) implements a separate offline viewer for Marauder
AirTag JSON lists, canonical copy saves and compatibility export of complete
qualifying S3/C5 packets. An explicit file action imports safe `.LOG` or `.JSON`
files. It preserves order, duplicate MAC records, empty payloads and opaque
bytes. Source, address type, time, RSSI and ADV/RSP identity remain unknown; the
imported model is never mapped into v6 or installed in live discovery. Address
privacy also hides the raw payload display.

Export from a validated native `.BLE` report considers each saved original and
matched response independently, in row/ADV/response order. Only complete,
source-proven raw packets with qualifying Find My evidence are exported;
decoded hints cannot reconstruct missing payload bytes. Omitted incomplete or
unproven packets and nonmatching packets have separate counts. More than 64
eligible packets fails without a partial export. The richer native document
remains available, and the confirmation explains metadata lost by JSON.

The imported document is bounded to 64 records and 8,192 total payload bytes;
encoded/read JSON is bounded to 49,152 bytes. Unknown extension values are
validated but not retained. Canonical saves retain the schema fields and bytes,
not original whitespace, case or extension members. Save Copy and export use
new `A00001.LOG`-style names under `/CERBERUS/BLE`, through exclusive temporary
files. They never overwrite the imported source or `/Airtags_0.log`.

There is one optional JSON viewer model and the existing serialized worker.
The selected native or JSON allocation arm is allocated exactly, with no
internal-RAM fallback. Export copies the native source before returning,
converts in the worker and reuses aligned scratch for encoded bytes. A retained
native viewer can coexist with the JSON viewer and worker. Linked native
measurements agree with the host layout: 57,648 bytes for an ordinary native
job (unchanged), 58,760 bytes for a JSON job and 8,836 bytes for the embedded
imported model. The optional JSON viewer is 9,440 bytes, giving 68,200 bytes
for JSON viewer plus worker, or 129,312 bytes while retaining the existing
61,112-byte native viewer for export Back. These sums exclude allocator/task
overhead and other stores; they are not running heap or fragmentation evidence.

Typed result copies are pinned outside the critical section. Canceling a load
discards its result; an in-flight Save Copy or export may still publish its
actual storage outcome. Failed saves retain their temporary file for diagnosis.
Focused codec, IO/storage and UI checks and independent reviews pass. The first
complete 132-suite host run passed on 1,096 unchanged inputs and is retained
under `artifacts/rev123-host-evidence`. All five native builds and the linked
resource audit pass; S3 fixed data/BSS grows by 32 bytes and C5 is unchanged.
The worker entry frame is 64 bytes (previously 48), with its existing 6,144-byte
stack allocation unchanged; selected frames do not establish full-chain stack
margin. The matched three-target, nine-image package and verify-only checks
pass. The matched rev123/C5 firmware69 package is installed with paired boot,
display buffering and SD mount verified. Real-card save/reopen, cancellation,
failure recovery and touchscreen acceptance remain unproven. The earlier
revision-specific sections below retain their own scope.

## Rev110 source checkpoint

V6 adds C5 matched-response metadata, preview, full-callback decoded family evidence and an independent upper age bound at save. V1-v5 remain readable with missing response evidence explicit. Saved C5 views select ADV or RSP independently of RAW, RX and HINTS. Opening a report does not use either radio or the current boot clock. Rev109 remains installed; [rev110](DUAL_RADIO_REV110.md) records the local candidate and validation. Earlier sections below keep their revision-specific scope.

Current source work in [rev109](DUAL_RADIO_REV109.md) extends saved reports to
v5 with the frozen C5 full-original family record and offline HINTS, while
retaining v1-v4 readers. The raw preview remains 31 bytes. The matched rev109
update is installed with paired boot and SD mount verified; physical save/reopen
and touchscreen acceptance remain pending. The sections below retain their
earlier revision-specific implementation and evidence.

Rev105's local viewer adds **SOURCE ALL / S3 / C5 / LEGACY** and
**FILE ORDER / NAME A-Z / SIGNAL** controls above the saved rows. The summary
shows filtered versus complete observation counts; empty filters retain the
report. Address types appear as T0–T3 and are expanded in Details. Returning
from Details preserves the filter, order and page. A successful new load
resets them, while a failed load retains the previous choices. Filtering and
sorting do not reread storage or query a current clock. Signal sorting compares
the recorded numbers while preserving their different PEAK/LAST meanings.
See [rev105](DUAL_RADIO_REV105.md) for validation. Rev103 remains installed;
rev104 and rev105 are local checkpoints pending device acceptance.

This guide describes installed rev102's v4 saved-report workflow. [The rev102 checkpoint](DUAL_RADIO_REV102.md) records matching host/native validation, verified installation/boot and remaining physical-acceptance limits. It extends [rev101's live C5 receive metadata](DUAL_RADIO_REV101.md) into saved reports. [The rev100 checkpoint](DUAL_RADIO_REV100.md) preserves the preceding v3 archive implementation and its revision-specific validation, while [rev99](DUAL_RADIO_REV99.md) records the earlier dual-receiver report scope. Earlier contribution evidence remains in [CONTRIBUTION_REV93.md](CONTRIBUTION_REV93.md); it does not establish physical acceptance of the new report.

Cerberus can save retained S3 and C5 Bluetooth observations in one SD report and reopen them for analysis after leaving the scan or rebooting. It supports the persistence outcome of saving received BLE evidence, including a selected Find My observation. This v6 workflow uses Cerberus's versioned `.BLE` format. The separate rev123 interchange work above addresses Marauder JSON files; neither offline format loads saved observations into live targeting, GATT or sound actions, so full upstream live-list parity remains open.

## Save an observation or retained list

Run a Bluetooth scan and select a row from the S3 list or the C5 companion pane. **SAVE SD** saves that page's frozen typed observation and original advertisement. An S3 selection also preserves the original full-width scan epoch and admitted matching response when the source proof is available. The C5 selection freezes its committed boot, epoch, generation, index and receive envelope when the row is painted/selected; a later bank or peer restart cannot lend newer provenance or callback metadata to the old row. A selection without the required base evidence reports an error instead of fetching a different current row; unavailable response proof and receive metadata remain explicitly UNKNOWN.

For the retained list, open **Bluetooth → SD SNAPSHOTS → SAVE BOTH RECEIVERS**. The menu also offers **SAVE SELECTED S3 ROW** and **BROWSE SD FILES**; save a selected C5 row from its own detail page. A report holds up to 64 source observations, at most 32 from each receiver. Either populated receiver may contribute when the other has no retained rows. Identical typed addresses heard by both remain two source-labelled observations, not two claimed physical devices.

Saving copies retained evidence at the request; the S3 inventory can include earlier sweeps, and the C5 contribution comes only from its fully published bank. The two sources are not claimed to be one simultaneous capture. Sort, source-pane and recent-filter presentation do not limit this save to the currently painted rows or make it a complete nearby-device capture. An entirely empty report or invalid contributing evidence reports an error.

In rev100, C5 protocol-v2 DISCOVERY replaces an old slot when a new qualifying typed address reaches its full 32-row bank. Rows and optional replacement/drop counters freeze together at the first snapshot BEGIN; firmware 63 exposes these counters to the live pane/diagnostics. They describe one snapshot's RF sweep, not all nearby devices, total RF loss or a cumulative session. The archive saves the committed rows and source provenance, not these bank counters. Exact-target tracking, v1 companion and other C5 receive workflows retain their prior admission behavior.

The UI then waits for normal S3/C5 discovery-owner cleanup before submitting the file worker. It does not fetch new observations while waiting. Back cancels this pending save; a cleanup timeout reports an error without starting the worker. Once submitted, the worker owns an immutable copy and writes a new file such as `/CERBERUS/BLE/B00001.BLE`. Existing files are never silently overwritten. After completion the UI opens the exact saved document. Back during an in-flight filesystem save requests cleanup, but that save may still finish and publish its file.

## Open saved evidence

Open **Tools → SD Files**, enter `/CERBERUS/BLE`, and tap a `.BLE` file. Files copied or moved to another safe SD folder can also be opened there. A dedicated historical view provides a four-row paginated list, frozen selected details, raw bytes and paginated advertising-data fields. S3 and unspecified LEGACY observations retain **ADVERTISEMENT / RESPONSE** selectors. C5 observations offer **ADVERTISEMENT / RECEIVE INFO** instead; the receive pane has no field-pagination controls and labels its evidence as historical. Back returns to the original file browser location or Bluetooth page. The existing address privacy setting applies to the offline display.

The same browser opens new v4 reports and old v1/v2/v3 files. V1 files show **LEGACY** receiver identity because v1 did not encode the receiver. V1/v2 response availability remains **UNKNOWN**; v3 retains its saved S3 response evidence unchanged. Every pre-v4 file has **UNKNOWN receive metadata**, including C5 rows with known boot/epoch provenance. The reader cannot reconstruct absent bytes, source sequences or receive fields from current scans. The viewer never starts reception, connects to a peer or promotes a loaded address into the live selection. A failed or canceled load cannot partially replace a committed offline document. S3 rows show independent historical **ages at save** for original ADV and retained matching response when their exact source times are known; C5 rows show a conservative original-ADV **upper bound at save**, or unknown. Opening after reboot never compares these values with the new boot's clock. No wall-clock date, GPS location, current presence or physical-device identity is inferred.

## What is retained

V4 adds the same eight-byte C5 receive envelope already retained by rev101's
live/selected **RECEIVE INFO** panel: callback source, delivered callback
length, properties, SID, primary/secondary PHY, controller data status and
independent full AD-layout validation. Known metadata must match the saved
original's preview length, mapped event and truncation flags. Legacy callback
fields not reported by that API remain N/A. An extended callback carrying a
legacy PDU is labelled separately. Controller COMPLETE describes the delivered
callback's data status, not retention of its full payload. Full AD-layout
VALID can coexist with a cut or parser-marked malformed 31-byte preview; the
two observations are displayed separately. S3/LEGACY rows and older-peer C5
rows retain canonical UNKNOWN receive metadata. These fields neither create
scan-response evidence nor grant live-action eligibility.

Each row includes its receiver and typed address, bounded peer-name projection, RSSI semantics/validity, manufacturer and UUID hints, peer flags, original raw ADV bytes/flags, report count, generation/index and available source metadata. S3 rows retain sweep-peak RSSI and exact historical source/copy times. C5 rows retain latest-original-ADV RSSI, the published or selected full-width C5 boot/epoch provenance, and an optional conservative age bound. C5 exact S3 source/copy times remain absent; an age bound may exceed saved S3 uptime and cannot be converted into an exact S3 packet timestamp. Unknown timing stays explicit.

Both archived sources retain at most **31 raw original advertisement bytes** per row. V3 additionally retains up to **31 S3 matching-response bytes from a delivered response of up to 255 bytes**, with separate lengths, event/flags, SID where valid, ordered original/response sequences and independent source times. This is a bounded prefix, not full extended-report retention. C5 passive discovery does not collect scan responses and its archived response availability is UNKNOWN. The S3 native extended-advertisement build remains disabled; controlled C5 extended/coded reception remains unaccepted on hardware.

The S3 response must belong to the frozen valid original's typed address, generation, full epoch and raw-source sequence. A newer invalid original can leave an older ADV copy for inspection but cannot lend that copy its later response hints, even with the same observed millisecond. UNKNOWN means correlation/source proof is unavailable; NONE means no admitted matching response for the proven original at capture, not proof that no response was transmitted. Original bytes/time are never replaced by response bytes/time. Aggregate name/UUID/manufacturer hints can still come from another report; the archive does not claim they occurred in either saved packet. A projected name may have lost bytes after an embedded NUL, and C5 peer text is already sanitized. Full names, full extended payloads, earlier response history and classifier-pointer structures remain outside this format.

Malformed or truncated raw advertisement bytes remain inspectable. The viewer validates AD boundaries and labels malformed data; parsed field values remain unverified claims. Unknown flag bits are retained. CRC and schema validation detect corruption and incompatible structure, not authenticity. See [the format specification](BLE_ARCHIVE_FORMAT.md).

## Resource and failure behavior

The v4 binary uses a 32-byte header and 216-byte rows, bounded to **64 rows and 13,856 encoded bytes**. The reader also accepts v3's 208-byte rows/13,344-byte maximum, v2's 144-byte rows/9,248-byte maximum and v1's 128-byte rows/32-row/4,128-byte maximum. The UI uses on-demand PSRAM for committed/incoming documents and copied selections. The worker owns a separate immutable PSRAM document and codec buffer, with no internal-RAM fallback, and requests the existing 6 KiB stack only for a job.

At rev102, v4 host and native layouts agreed at **15,376 bytes per document**, **32,304 bytes for the UI store**, and **30,000 bytes for the worker context**. Documents already embedded in those stores must not be counted again. The document and encoded buffer each grew by 512 bytes relative to v3; combined optional UI/worker allocation requests grew by 2,088 bytes. Rev105 adds a presentation index and control fields to the UI store; its separate measurements are recorded in the rev105 checkpoint. These sizes exclude allocator/task overhead, other live objects and fragmentation; they are not runtime heap/stack acceptance. See the rev102 checkpoint for its original measured frame changes and observed boot limits.

Copying a completed document pins the worker context, copies outside the interrupt-disabled critical section, and releases that reader pin afterward. Concurrent Release rejects additional readers and defers disposal and a replacement job until the already accepted copies finish. No worker borrows UI document memory, so leaving the screen cannot free its in-flight data.

Filesystem operations occur in the worker and hold the storage mutex through cleanup. Unmounted or USB-owned storage and conflicting capture, Wardriving or Recon recording are rejected. These operations have bounded byte counts, not a fixed filesystem latency. The UI does not wait on SD I/O.

Save creates an exclusive `.TMP` file, checks the complete write, flush, synchronization and close, and publishes `.BLE` only after those succeed. Errors retain partial files for inspection. Names are bounded to B00001–B99999, with at most 1,024 candidate names examined per attempt. There is no claim of power-loss atomicity or storage-medium durability beyond the reported successful calls.

Load checks safe paths, file type and size, complete reads, EOF, close, version, exact encoded length, reserved fields, CRC, per-source row limits, receiver+typed-address duplicates and receiver-specific timing/provenance rules before publication. Back requests cancellation and deferred worker cleanup.

The read-only `ble_archive` console command reports the worker's state, job ID, ownership/result flags, error, complete byte count, row count and file path. It cannot initiate a save, load or radio operation.

## Validation scope

Focused host checks exercise the codec, capture mappers, offline viewer, UI adapter, worker and storage routines. The codec retains old-version compatibility, v3 response evidence, v4 receive envelopes, full 64-row validation, source collisions and failed-decode atomicity. S3 capture checks cover frozen raw identity, missing epoch/clock, equal-millisecond invalidation and independent response timing; C5 mapping preserves the copied receive envelope/provenance and unknown/backward/overflow age bounds. Worker checks inject concurrent Release and nested copies during the actual out-of-lock memcpy, plus allocation/task failure, cancellation, stale jobs and file faults.

The rev102 focused runs pass **1,881 offline UI checks**, **924 capture/controller checks**, **297,359 actual-storage checks across 48 isolated file cases**, and **15,021 worker/lifecycle checks**. They include a full 13,856-byte mixed S3-response/C5-receive save/load, the final encoded receive byte, poisoned uncommitted staging, replacement of live banks after selection, repeated Save during owner release, canceled or failed work retaining frozen evidence, and failed publication preserving an existing file. These host boundaries do not measure real-card durability, RF performance or touchscreen behavior. The rev102 checkpoint owns the complete validation and rendered-review record.

Physical acceptance remains pending: save selected S3/C5 observations and both retained banks, inspect source-specific RSSI/time labels and matching S3 response, verify C5 receive information after save/reopen, reopen old v1/v2/v3 files with unavailable receive metadata, reboot and reopen v4, and repeat save/load and return-to-scan transitions. Controlled extended/coded reception, real-card failure recovery, power interruption and prolonged operation are not established by host fault injection. Physical source-filter/sort acceptance, broader history/session recording, full extended/raw response capture, family parity, AP/SSID persistence and earlier Wi-Fi feature errors remain separate work. GPS and expansion radios are not required for this workflow.
