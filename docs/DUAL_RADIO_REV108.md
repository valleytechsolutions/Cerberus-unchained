# Rev108: C5 full-report BLE family evidence and focused retention

All 124 host suites and five native targets pass on unchanged matching inputs.
This local candidate extends the concurrent S3/C5 passive family workflow.
Rev106/C5 firmware65 remains the last installed build. Rev108 has not been
flashed, published or accepted on the physical touchscreen.

## Intended behavior

Selecting Find My, iBeacon, Flipper, Flock, Meta/RayBan or HC-name discovery
requests the corresponding family from both receivers. The S3 retains its
existing independent admission, matched-response and historical evidence. The
C5 classifies each complete original callback before limiting the retained raw
preview to 31 bytes. This permits supported hints beyond the preview boundary
in complete delivered reports of up to 255 bytes.

The C5 family mode admits new typed addresses only when that same complete
original qualifies for the selected family. Unrelated reports cannot fill its
32-row inventory or consume its full-bank drop counter. A later complete
nonmatching or invalid original replaces the same address's earlier proof;
dequalified slots are preferred for replacement. Responses, partial extended
reports and stale callbacks do not refresh a retained original's evidence or
age. Negative or backward callback clocks likewise leave the retained original
unchanged. The bounded capacity still limits how many matching observations can be
retained.

## Compatibility and resource contract

The new focused mode requires C5 firmware66. Older firmware reports focused
scanning unavailable before starting that mode; the S3 family scan remains
usable. Ordinary ALL discovery, tracking and Analyzer retain their existing
paths and avoid the added full-family parsing and transfer.

The optional family snapshot appends a canonical 48-byte evidence record to
the existing 110-byte receive-metadata projection. The resulting 158-byte row
uses 16 bounded chunks with the same boot/job/epoch/frozen-handle binding.
Existing 102- and 110-byte reads remain unchanged. The S3 publishes only a
complete validated bank after the final identity fence. Invalid evidence is
not silently replaced by an older interpretation.

This costs five additional chunks per focused row compared with the metadata
read. Selected-family admission reduces unrelated inventory traffic, but a
wall-clock speed improvement is not claimed without measurements on hardware.

Evidence is kept alongside the existing live and frozen C5 banks; there is no
new radio task, retained full-payload bank or per-callback heap allocation.
Family changes drain the previous bound owner before beginning another. Stop
and Back retain the existing cleanup barriers and bounded transport work.

Cached diagnostic version3 reports the desired UI family, the C5 controller's
request family and the published bank's family separately. These values can
differ during cleanup and are not claims of current reception or fresh RF
evidence. Reading the console does not advance a scanner or perform SPI.

The HINTS view distinguishes decoded full-original evidence from the raw
preview. Typed addresses observed by both receivers remain separate source
observations. Advertised formats and address-prefix clues are unauthenticated
hints, not confirmed physical identities, cross-receiver packet correlation
or authorization for an active operation.

## Validation and remaining work

The complete 124-suite host run and five-target native matrix pass with no
input drift and matching source fingerprints. Targets are S3, onboard C5,
USB bridge, expansion WROOM and expansion XIAO. Existing unused Wi-Fi relation
helper warnings remain the same as rev107; no repair of those unrelated
helpers is claimed. The matched package is recorded below after verification.

The focused classifier/codec covers 1,834 checked cases, including every
family beyond the preview boundary, all 77 Meta identifier/context pairs,
valid no-hint reports, typed address clues, ambiguous iBeacon tuples and
malformed/noncanonical evidence. The actual C5 producer and S3 reader are
connected in the host integration fixture. It covers immutable 102/110/158-byte
projections, stale handles, complete-report dequalification, 32-slot focused
admission, SDK failures, callback-token exhaustion and transfer cancellation.

The production live UI fixture passes 1,015 checks; the pure combined-list
helper passes 80,197 checks. Coordinator/UI checks cover delayed cleanup,
latest family selection, lost release replies, unsupported older peers,
generic ALL recovery and no rearm after Stop, Back or sleep. Six final
actual-renderer previews were inspected and retained. They are synthetic
body-region fixtures, not physical screenshots or touchscreen acceptance.
The seven relocated portable parser headers match their previous S3 contents
after newline normalization and retain compatibility include wrappers.

Measured native layouts compare the retained rev107 and rev108 ELFs:

| Measurement | Rev107 | Rev108 | Change |
| --- | ---: | ---: | ---: |
| Optional S3 companion store in PSRAM | 8,496 B | 11,616 B | +3,120 B |
| Optional S3 discovery store in PSRAM | 20,608 B | 20,800 B | +192 B |
| S3 internal data + BSS | 172,588 B | 172,828 B | +240 B |
| C5 data + BSS | 80,080 B | 83,152 B | +3,072 B |
| S3 reader tick entry frame | 192 B | 352 B | +160 B |
| S3 family classifier entry frame | 240 B | 304 B | +64 B |
| S3 C5-detail renderer entry frame | 384 B | 400 B | +16 B |
| C5 original-callback entry frame | 288 B | 320 B | +32 B |

The S3 UI main entry frame remains 1,136 bytes with an 8,192-byte task
allocation. The C5's new isolated classification helper has a 64-byte entry
frame, its shared parser 128 bytes, and its encoder 64 bytes. C5 gap-event and
snapshot-command entry frames remain 64 and 224 bytes. The C5 NimBLE host task
allocation remains 4,096 bytes. These are selected entry-frame measurements,
not complete nested-call stack bounds or runtime high-water measurements.
Inlined or absent standalone helpers are never assigned zero cost.

The added C5 storage is exactly two 1,536-byte evidence arrays beside the
existing live/frozen banks. S3 increases are existing stores and frozen
painted/selected copies. Native type sizes exclude allocation overhead,
fragmentation and other loaded features. No measured scan speedup or runtime
free-heap guarantee is claimed.

Evidence is retained in `artifacts/rev108-host-evidence`,
`artifacts/rev108-native-evidence` and `artifacts/device-rev108`. No serial,
flash or physical feature testing was performed in this revision.

C5 scan-response acquisition, S3 native extended reception, full-payload
recording and saved-file retention of the new decoded family record remain
outside this checkpoint. The current archive format retains its existing
original preview and receive metadata. Physical extended formats, dense RF
retention, repeated family transitions, Stop/Back, saves, latency and sustained
memory behavior remain unverified on this candidate. The Marauder coverage
ledger continues to track all modes without claiming complete parity.

## Matched local package

Implementation commit `c90976e939a7c6854c60afa267894d3df2c4b3b2` is retained
locally. Package
`dist/cerberus-unchained-0.1.0-dev.rev108-20261101T111735Z` passes hash, chip
header, partition and image-bound verification. All three application hashes
match the retained five-target native matrix, and packaged source hashes
match the final host and native receipts.

- Source SHA-256: `bc6d251ad15e1b58d7fbcad3460ebe286fae6d55dd7e30b0ae395be1d7861e76`.
- Manifest SHA-256: `93d604954d3d71057d4669e2cd6f56f2b826eebbcd5117bf858da001eabfb7c8`.
- S3 application: 1,914,208 bytes; C5: 1,672,176 bytes; bridge: 255,296 bytes.

`artifacts/device-rev108/local-milestone.json` binds the exact implementation
commit, package, reviewed ELFs and unchanged-input validation. Its `installed`
field is false. This revision was not flashed, pushed, submitted as a pull
request or selected for the public installer. Rev106 is the last verified
installed baseline; rev107 and rev108 are local candidates.

The next substantive storage gap is preserving the decoded full-original
family record in saved BLE reports, with compatible historical readers and
the same immutable source binding. Physical validation of focused reception,
family changes, Stop/Back and the following Wi-Fi scan remains pending.
