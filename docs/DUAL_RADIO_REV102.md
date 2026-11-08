# Rev102: preserve C5 receive evidence in SD reports

Rev102 is installed as a local development checkpoint. It extends the existing
BLE report without another save menu or receiver query. All 120 host suites and
five native targets pass on matching unchanged inputs; paired writes and boot
are verified. Physical feature acceptance remains pending, and the full
Marauder port is incomplete. Rev101 is the preceding installed checkpoint.

## Saved evidence and compatibility

The v4 codec appends the eight-byte C5 receive envelope to each 208-byte v3 row,
making a 216-byte row and a maximum 13,856-byte file. Known metadata is restricted
to C5 observations and validated against the same row's preview length, original
event type and flags. Source kind, properties, callback length, SID, primary and
secondary PHY, controller status and independent full-AD layout validation are
encoded explicitly. UNKNOWN remains all zero; no absent field is inferred.

V1/v2/v3 files remain readable. In particular, v3 S3 matched-response bytes,
correlation evidence and historical time remain preserved after the version
bump. Old reports have UNKNOWN receive metadata. CRC, reserved-byte, source,
duplicate, time, exact-length and whole-file validation precede publication.
Failed decoding cannot partially replace the existing document.

C5 capture copies the envelope from the frozen selected or committed row with
its original provenance. Later live-bank changes cannot lend new metadata to an
old selection. Saving waits for normal receiver cleanup and then uses the
existing immutable PSRAM worker copy. Its tasks, ownership and out-of-critical
section reader copies are unchanged. No new RF, SPI or clock query is added.
C5 protocol firmware remains 64, and the scan/transport implementation is
unchanged from rev101.

The existing offline raw page shows ADVERTISEMENT / RECEIVE INFO for C5 and
keeps ADVERTISEMENT / RESPONSE for S3/LEGACY. The C5 pane labels its data as
historical and keeps unavailable response bytes explicit. A shared renderer
keeps live and saved callback/preview, PHY/SID/status and layout labels aligned.
Full valid AD framing with a cut preview remains distinct from a malformed full
report. Missing bytes are never reconstructed. The receive pane has no field
pagination, and Back remains reachable. Opening a report cannot start reception
or turn saved addresses into action targets.

## Validation record

Focused checks pass: codec 6,616,148 assertions, S3 capture 2,523, C5 capture and
actual dual-radio integration, viewer 1,881, live-controller 924, worker/lifecycle
15,021, and actual storage 297,359 checks across 48 file cases. Counts describe
host fixture work, not RF trials. Independent v1/v2/v3 compatibility vectors,
v4 mixed-source vectors, all eight receive-field domains, malformed/cut raw
contexts, frozen selection after bank replacement, concurrent readers,
cancellation, allocation/file errors and failed-publication rollback are covered.

The 320x480 saved C5 extended/detail/UNKNOWN renders were visually inspected;
legacy, extended-legacy-PDU and invalid-layout renders are retained too. Shared
live renderer checks pass. Host layouts measure archive row/document/UI store
at 240/15,376/32,304 bytes and worker context at 30,000 bytes. This projects
2,088 additional bytes across the optional UI and worker contexts versus rev101;
it does not establish native peak memory use or performance.

All 120 host suites pass on unchanged inputs with Python 3.14.2 and Zig 0.14.1.
Their receipt and exactly 120 hash-verified logs are retained in
`artifacts/rev102-host-evidence`. The coverage generator also verifies the clean
pinned Marauder develop checkout; its 95-mode statuses remain 73 partial,
eight pending, ten needing review and four hardware limited.

All five native builds pass: S3, onboard C5, bridge, WROOM and XIAO. Their final
receipt matches the host receipt's unchanged inputs and is retained with logs,
ELFs, map and SDK configurations in `artifacts/rev102-native-evidence`.
Linked S3/C5 data/BSS are unchanged from the
authoritative rev101 images. S3 data/BSS remain 26,412/146,160 bytes; C5 remain
19,552/60,480. Native DWARF confirms row/document/UI/worker sizes of
240/15,376/32,304/30,000 bytes: the combined optional UI and worker requests grow
by 2,088 bytes. These requests exclude task/allocator overhead and concurrent
objects. The archive decoder frame grows 304 to 320 bytes; save/capture stay
240/288 and worker/start stay 48/48. Copy-result and release frames stay 32/32.
The shared receive renderer has a 128-byte frame, while its live caller shrinks
368 to 304 and the archive UI caller shrinks 224 to 208. Frame extraction is not
a whole-program or runtime stack bound. Measurement files are retained under
`artifacts/device-rev102`, with `s3-archive-native-stack-verified.json` identifying
the exact S3 ELF and debugger commands. `final-native-audit-binding.json` binds
that inspection to the completed five-target evidence. Manual native review
confirms the 15,376-byte result memcpy remains outside the critical section and
protected by its reader pin, with deferred free after the final exit.

Matched package `dist/cerberus-unchained-0.1.0-dev.rev102-20261101T080830Z`
passes hash, chip-header, partition and image-bound verification. Its firmware
source SHA-256 is `52eea6f73c785da4a343f621807dffa07172bf4793d2ea58b6d69368232f6d49`.
The packaged applications match the final full-matrix binaries. No image set
has been selected as a public release.

## Recoverable installation and idle checks

Implementation commit `57cb92b72d98cc97e3337da4fa4d16f74d08f8e1` is installed.
Fresh first-2-MiB S3 and C5 backups match the previously installed rev101 images
and verify against device flash. They cover every erase-rounded write region;
higher flash remains untouched. Exact chip/MAC guards and the backup validator
run before writes. Five identity tests and nine receipt/stage tests pass.
All nine image writes across bridge, C5 and final S3 verify, with exclusive
package/source/image-bound receipts. No PR or release was published.

Read-only diagnostics report S3 `0.1.0-dev.rev102`, C5 protocol firmware 64,
active display buffering, mounted SD, inactive USB storage and no previous-reset
fault. At the final idle check, S3 internal free memory is 134,396 bytes, its
largest block is 94,208, PSRAM free is 1,428,324 and task count is ten. These match
the preceding idle readings; optional archive allocations are not exercised.
C5 reports 58 KiB free and a 54 KiB minimum. Radio owners and the archive worker
are idle. Startup SPI retains four initial READY timeouts and four legacy
fallbacks, with zero mutex timeouts and driver errors.

The observed console low-water margin is 524 bytes, versus 652 in the preceding
checkpoint's recorded sequence. It remains 524 on the follow-up read. The exact
console source/checker, selected emitted frames and 2,896-byte known frame
subtotal are unchanged; this comparison does not establish the cause of the
different runtime reading. Main-task low water is 5,176 bytes. Both are observed
readings, not worst-case bounds; runtime stress and stack acceptance remain open.
The existing 26,240-byte crash record was preserved and predates this update.

Evidence is retained under `artifacts/device-rev102`, including
`installation-receipt.json`, backup/write receipts, preflash verification,
`boot-confirmed.txt`, `settled-boot.txt`, `final-idle.txt`, and
`console-frame-comparison.json`. The receipt binds the implementation commit,
source digest and exact matched package. These checks establish recoverable
installation and idle boot, not RF, touch or real-card report acceptance.

## Pending physical acceptance

Physical acceptance remains pending: save/reopen selected C5
and mixed reports, verify S3 matched responses and old reports, repeat Back and
a subsequent split Wi-Fi scan, and inspect controlled extended/legacy reception.
Real-card failure recovery, power loss and sustained use are not established by
host tests. GPS/expansion hardware and earlier active Wi-Fi errors remain open.
No PR or binary release has been published for this checkpoint.
