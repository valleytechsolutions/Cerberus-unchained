# Rev132: saved Mesh observation maps

The matched rev132 development package passes the complete local host/native
matrix, resource audit and package verification and is installed on both chips.
Paired application boot, SD mounting and sampled idle checks pass. The owner has
accepted rev131's menu-first Mesh entry and Back controls; actual Mesh reception,
Locate and the new SD save/reopen workflow still require physical acceptance.
No complete Mesh/CYM/Marauder parity claim is made. The
[implementation plan](MESH_ARCHIVE_PLAN.md) records its boundaries.

## User workflow

The existing five-tool Mesh menu stays intact. Inside the Mesh survey, SAVE MAP
or STOP & SAVE saves a new `.MSH` file, then OPEN MAP opens the historical view.
SAVED opens `/CERBERUS/MESH` in the existing Files browser when acquisition and
storage work are released. Files also recognizes copied `.MSH` documents.
Before the first save creates that directory, SAVED shows an empty-map message.
Only a missing Mesh directory is treated as empty; mounting and other directory
errors remain visible. Opening the browser does not create directories.

The historical view offers PAN contexts, addresses, directed observed traffic
and retained raw-frame details. Its selected rows are frozen copies, and the
existing address privacy setting also hides raw bytes. Back from a detail
returns to the map; Back from the map returns to Files. It exposes no Start or
Locate action. Historical source errors, partial channel coverage, clock faults
and saturated C5 counts remain visible independently of successful file loading.

EXPORT CSV creates the existing Mesh report schema under `/CERBERUS/LOGS`, then
offers OPEN CSV. Existing CSV and Locate report codecs remain supported. Failed
loading preserves the previous historical document; Back or sleep cancels and
releases only the view's exact storage job. A save already inside a filesystem
write reports its actual outcome, rather than implying rollback.

## Evidence and processor roles

C5 retains the existing passive receive, bounded FIFO and exact Stop/drain/
release path. No new C5 command, receiver task or capability revision is needed;
the onboard capability remains 73. S3 validates and copies only released frozen
evidence into a separate historical model, then uses the existing serialized SD
worker. Saved session identifiers describe the old capture and confer no live
radio authority.

The archive persists every retained PAN/address/edge aggregate and final source
summary, plus the first 64 raw witnesses. Counts and observations after those
first 64 records are preserved directly, with explicit omissions. Replaying the
raw subset cannot reconstruct them. The map represents observed MAC addressing,
not authenticated devices, actual delivery, network membership or routing.

Version 1 uses explicit little-endian fields, a 32-byte header, parser revision
1, exact count/length checks, canonical flags/reserved bytes and CRC32. Its
encoded length is 268..18,332 bytes. CRC detects corruption, not authenticity.
Failed decode leaves the destination unchanged and uses explicit caller-owned
scratch instead of a large stack object. Live CSV and historical CSV share a
borrowed evidence validator/renderer; the live wrapper still requires its
original frozen/released controller boundary.

Save/load/CSV operations have separate optional PSRAM allocation arms. Existing
BLE, Wi-Fi, interchange, Probe, Mesh CSV and Locate arms retain their own sizes.
The live Save conversion briefly overlaps an archive copy with the live UI and
save worker. OPEN MAP may overlap the old stopped UI, historical UI and load
worker until the next UI cleanup tick. Native allocation/frame measurements
must account for those transitions; host structure sizes alone are insufficient.

## Validation checkpoint

Focused codec checks cover empty/full/late-aggregate captures, typed identities,
known PAN zero versus unknown, unknown clocks, source errors, all truncations,
every-byte CRC corruption, semantic corruptions with a repaired CRC and alias/
atomicity boundaries. Independent Python bytes verify an empty golden document
and the complete maximum layout. The historical export matches the
pre-refactor CSV renderer byte-for-byte for 0, 80 and 1,000 records. Archive
checks also reject contradictory callback times while preserving valid zero
and first-valid timestamps arriving after the retained raw prefix.

Focused worker/storage checks exercise actual asynchronous publication, exact
typed jobs and result pins, release/reentry, cancellation, exclusive filenames,
short writes, flush/sync/close/rename failures, bounded EOF and input/output
alias rejection. The UI fixture uses the actual production controller/model,
codec and route excerpts with a synthetic framebuffer. It checks more than 64
observations, historical details/privacy, retained-map errors, CSV export,
Back/sleep cleanup, and the absence of live controls. Host-rendered previews
are not photographs or touchscreen acceptance.

All 151 registered host suites and five native targets pass on 1,202 unchanged
inputs at source `6a91b8be6dd291401de4d78de9d96771b81d7733`. The matched package's
22 checksums, nine images, chip headers, partition layouts and erase bounds
verify. The first-use correction is covered by actual directory-listing and
navigation fixtures: only `ENOENT` becomes a missing-directory result, logging
cannot change its classification, and mount/status/other-directory errors stay
visible. Empty, repeated and subsequently populated listings retain Back.

The native comparison against rev131 measures:

| Linked or optional allocation | Bytes | Change from rev131 |
| --- | ---: | --- |
| S3 fixed data + BSS | 169,436 | +16 |
| C5 fixed data + BSS | 83,400 | Unchanged |
| Live Mesh UI in PSRAM | 22,896 | Unchanged |
| Historical viewer in PSRAM | 22,776 | New, optional |
| Map save worker in PSRAM | 39,568 | New, optional |
| Map load worker in PSRAM | 60,032 | New, optional |
| Historical CSV worker in PSRAM | 119,544 | New, optional |

All previous worker allocation arms remain unchanged. Conservative optional
overlaps are 82,928 bytes during Save, 105,704 during direct Open, and 142,320
during historical CSV export. Including the old live UI before its cleanup
tick raises the last case to 165,216 bytes. These exclude allocator metadata,
filesystem/SDK allocations and unrelated stores. The archive worker stack is
still 6,144 bytes; measured codec entry frames are 80 bytes for encode, 64 for
decode and 64 for field transfer. Entry frames are not a full call-chain bound
or measured runtime headroom.

The offline audit's first attempt omitted a GCC `$constprop$` helper spelling.
Its evidence is preserved separately; the corrected final audit includes that
helper and the new filename-counter scalar. Firmware, ELF hashes and allocation
measurements did not change. No firmware rebuild was needed for that audit-only
correction.

## Package and installation record

- Package: `dist/cerberus-unchained-0.1.0-dev.rev132-20261102T222844Z`.
- Manifest SHA-256: `c17789e7565490e118b08a1aaf074252afcd3fe98bb8d373d42937fce532e805`.
- Source snapshot SHA-256: `7d7d91db3699319aba5002f6919d98aa44eef4a21d81f502d05f5de464af7530`.
- Local milestone: `artifacts/rev132-validation/local-milestone.json`, SHA-256
  `29d2f21b1aca365dd64dc3f5026550ef7bbedb28cb609d717aeaeeaacec07612`.
- Final native audit: `artifacts/rev132-validation/archive-native-final/native-memory-comparison.json`,
  SHA-256 `8ad8f0a2a282d8e31c5b2d6de9e29bd6a99d7f9b922db7318793670acf7f560c`.
- Installation: `artifacts/device-rev132/installation-receipt.json`, SHA-256
  `c2ef91f48c29b136b7097e591e3c796387745252bb7d0d334cebc8c2529516a9`.

The immutable local milestone describes the pre-install checkpoint; installation
has its own receipt. Fresh rev131 backups were read and verified through S3
`0x20b000` and C5 `0x200000`, covering every old/new/bridge erase extent. All nine
bridge/C5/final-S3 image writes verified. NVS/PHY regions were preserved; no
blanket erase was used. This is a local development installation, not a public
binary release or a new PR update.

Two rev132 idle captures 44.597 seconds apart show both uptimes advancing, C5
boot count remaining one, and SPI exchanges increasing from 210 to 416 without
growth in the listed error counters. Four initial READY timeouts and four
legacy fallbacks were already present and remained unchanged; this is not a
zero-error-startup claim. Both samples report mounted SD, 137,664 internal free
S3 bytes, a 94,208-byte largest internal block, 1,427,552 free PSRAM bytes and
53,636 snapshot-free C5 bytes. The observed console stack low-water value falls
from 2,080 to 1,408 bytes as diagnostics exercise more paths; these idle samples
do not measure loaded-operation stack or heap bounds.

The next physical check is Scan → Mesh Detection → Mesh Detection → START →
STOP & SAVE → OPEN MAP, then Back and a normal Wi-Fi scan. An empty map can be
valid without nearby traffic; it does not prove reception. Real SD save/reopen,
CSV export, repeated transitions, Locate signal behavior and loaded endurance
remain pending. No GPS is needed for these Mesh workflows.
