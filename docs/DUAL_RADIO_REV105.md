# Rev105: browse saved BLE evidence by receiver

Rev105 extends the existing offline `.BLE` report viewer with ALL/S3/C5/LEGACY
source filters and FILE ORDER, NAME A-Z and SIGNAL ordering. It follows
rev104's unified live discovery list. Rev103 remains the last installed
checkpoint; this revision is validated locally and remains unflashed.

## Historical browsing

The source and sort controls sit above the existing four-row cards. The
summary distinguishes shown observations from the complete report and retains
source totals. An empty filtered view keeps the document and offers the other
source choices. Source observations are not deduplicated into physical devices;
the receiver and BLE address type remain part of identity.

File order preserves the saved sequence. Name sorting uses bounded sanitized
text and stable ties. Signal sorting puts valid values first, preserving S3
sweep-peak versus C5 latest-original-ADV meanings; it does not calibrate the
radios or normalize their sampling windows. Unknown signal remains unknown.
There is no combined source-clock or current-freshness ordering.

Changing source, sort or page changes only the presentation index. It does not
modify the document, read the SD card again, fetch live observations, query a
clock or start any radio operation. Saved S3 exact ages and C5 upper bounds
remain ages at save. Old v1 files remain LEGACY, never relabelled as S3.
The v4 schema and v1/v2/v3 compatibility remain unchanged.

Tapping a card still selects the frozen row, source metadata, raw ADV/response,
saved uptime and age shown by the last complete successful paint. A failed
draw revokes row selection; local display controls and Back remain available
for recovery. Returning from details preserves source, order and page. A
successfully opened new report resets to ALL/FILE ORDER. A failed load retains
the previous document and its browsing choices. Back cancels waiting and exits
the viewer without modifying the saved file. Saved rows never become live
targets.

## Resource and validation scope

The existing optional PSRAM UI store gains a bounded 64-byte index and small
control fields. Final native ELFs measure **32,304 to 32,376 bytes (+72)** for
that store; its document, incoming, painted and selected evidence banks are
unchanged. The document remains 15,376 bytes and each row 240 bytes. There is
no new document bank, task, worker-buffer growth, archive schema change or C5
protocol change. C5 firmware protocol stays 64.

The retained rev104/rev105 comparison measures unchanged S3 internal static
sections: `.dram0.data` 26,412 bytes and `.dram0.bss` 146,160 bytes. The linked
archive draw entry frame grows from 208 to 240 bytes; the other comparable
selected frames are unchanged. The configured main-task stack remains 8,192
bytes. These selected frames are not a complete worst-case stack calculation;
optional PSRAM type sizes exclude allocator overhead and fragmentation.

All **123 host suites** and **five native targets** pass on matching frozen
inputs. Focused checks cover 170,281 pure-ordering cases, 8,293 actual archive
UI assertions and 934 archive live/controller assertions. Four reviewed
full-screen renderer previews cover mixed sources, long filenames/privacy,
an empty source filter and the final C5 page. Their layout proof does not
establish physical touchscreen behavior.

The complete receipts and logs are retained under
`artifacts/rev105-host-evidence` and `artifacts/rev105-native-evidence`.
`artifacts/device-rev105/native-ui-comparison.json` binds resource measurements
to the retained ELFs and SDK configurations; `visual-review.json` records
preview hashes and scope. Rev104 is the local comparison baseline, not
installed firmware. No serial command or flash was used for this milestone.

Physical SD save/open, filtering, selection, Stop/Back and return-to-scan
acceptance remain separate. Complete Marauder option parity, full
payload/history retention, AirTag JSON interoperability, GPS and expansion
acceptance remain open.

## Local matched package

Implementation commit `ceb9cb72ed722d75214186aec33fad66cba90ce2` is retained
locally. Package
`dist/cerberus-unchained-0.1.0-dev.rev105-20261101T093428Z` passes hash, chip
header, partition and image-bound verification. All three application hashes
and sizes match the retained five-target matrix. Its source map matches the
corresponding retained host/native inputs and the committed source. The
firmware source SHA-256 is
`14fa81dfc271568338b1bfedb126ce254f7a80dcdd4f0cb52dbc5074b35dc87d`.

`artifacts/device-rev105/local-milestone.json` binds the package, source commit,
123-suite host receipt, five-target native receipt, final ELF resource audit
and console evidence. The manifest, matched receipt and package log are also
retained with native evidence. No image was flashed, no device command was
sent, and no PR, push or public release was created for this milestone.
Rev103 remains the last installed checkpoint.
