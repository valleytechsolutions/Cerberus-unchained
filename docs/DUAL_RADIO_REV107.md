# Rev107: source-specific BLE family discovery

All 124 host suites and five native targets pass on unchanged final inputs.
Rev106/C5 firmware65 remains installed. This local checkpoint extends the existing concurrent BLE receivers into
family-filtered lists and C5 original-advertisement details without another
radio task, SPI command or archive version.

The GitHub `develop` ref was rechecked on 2026-11-01 and still resolves to
[`79cc6bcba1db6679baee1dab2b0c1ad5010e6363`](https://github.com/justcallmekoko/ESP32Marauder/commit/79cc6bcba1db6679baee1dab2b0c1ad5010e6363).
The local pinned source exposes Find My, Flipper, iBeacon, HC-name, Flock and
Meta/RayBan family workflows. This source check does not prove Cerberus parity.

## Evidence contract

S3 family admission, source history and independently matched scan-response
evidence retain their existing meaning. C5 hints must come from its frozen
original payload and receive envelope, never accumulated name, UUID or vendor
fields from an earlier advertisement. Typed addresses seen by both receivers
remain separate source observations.

C5 qualification requires known valid receive metadata, a complete retained
original of at most 31 bytes, no truncation/malformed flags, and complete AD
stream validation. Unsupported or incomplete evidence is unclassified rather
than proof that a family is absent. Hints are unauthenticated advertised clues;
they do not identify a physical device or authorize an active operation.

## Remaining scope

The C5 generic 32-row bank does not prioritize the selected family, collect
scan responses or retain complete payloads longer than 31 bytes. Full-callback
classification and focused retention remain substantive follow-up work. S3
native extended reception also remains disabled. No current-build touchscreen,
RF, save/reopen, sustained-memory or full-feature acceptance is claimed.

## Presentation and ownership

The ordinary family selector now uses source-labelled references into the
existing S3 and C5 banks. Source selection remains ALL/S3/C5. For the existing
recent-capable Find My and iBeacon families, the same control also cycles
RECENT ALL/S3/C5. S3 keeps its exact family evidence timing; C5 recent admission
requires a proven original-age upper bound within 30 seconds. Unknown age is
not treated as current. Changing family preserves the selected receiver and
excludes an old S3 inventory from the wrong family.

The list caches C5 classification when its committed publication changes.
Age-only repaint recomputes visibility without reparsing raw advertisements.
Sorting remains source-aware and stable; a saturated publication counter
cannot establish an unchanged bank. The four successfully painted selections
retain complete source provenance, so a newer bank cannot retarget a tap.

C5 detail retains RAW ADV and RX INFO and adds HINTS. It distinguishes valid
reports without a supported hint from incomplete, malformed or unknown source
evidence. Pages include full iBeacon UUID/major/minor/advertised power, Find My
formats, Flipper codes, Flock clues, Meta ID/context pairs and exact HC names.
Every retained Meta ID/context pair is reachable through page navigation.
Public-address-prefix-only Flock matches are labelled address clues rather
than payload matches. Meta suppression here is same-original-only; it does not
claim the S3's historical suppression behavior.

Existing passive typed-address Fox Hunt and frozen Save routes are preserved.
The family-list Find My monitor remains explicitly S3-scoped. C5 classification
does not create S3 response evidence, a connection credential or sound/action
eligibility. The archive format and RF scheduling are unchanged.

## Validation

The focused classifier passes 1,630 checked cases, with a 48-byte host evidence
structure. The pure list helper passes 80,197 checks, and the production live
UI fixture passes 978 checks. Existing discovery/lifecycle/Analyzer, receiver
selection, age, archive and Wi-Fi regressions also pass in the complete
124-suite run. All five native targets pass: S3, onboard C5, bridge, expansion
WROOM and expansion XIAO. Host/native source hashes match with no input drift.

Actual-renderer tests cover all family pages, full iBeacon values, every Meta
ID/context pair, address-only Flock clues, frozen selection after replacement,
unknown/truncated/malformed evidence, failed paint, source filtering and
separate S3/C5 recency. The shared empty-state renderer now accurately says
live BLE results update as available; Wi-Fi's existing explanation remains.
Six final generated previews were visually checked and retained. These are
synthetic body-region fixtures, not physical screenshots or touch acceptance.

Native measurements compare retained rev106 and rev107 ELFs:

| Measurement | Rev106 | Rev107 | Change |
| --- | ---: | ---: | ---: |
| Optional S3 live-discovery store in PSRAM | 20,560 B | 20,608 B | +48 B |
| S3 companion state | 8,496 B | 8,496 B | 0 |
| S3 Analyzer state | 288 B | 288 B | 0 |
| S3 internal data + BSS | 172,588 B | 172,588 B | 0 |
| C5 data + BSS | 80,080 B | 80,080 B | 0 |
| C5 detail renderer entry frame on S3 | 304 B | 384 B | +80 B |
| Live-view builder entry frame on S3 | 304 B | 336 B | +32 B |

The new classification entry frame is 240 bytes. The UI main entry frame
remains 1,136 bytes and its task allocation remains 8,192 bytes. Helpers that
are inlined have no separate entry frame and are not assigned zero cost.
These selected frames do not establish a whole nested call-chain bound.
Native type sizes also exclude allocator overhead, fragmentation and other
loaded features. No new radio task, allocation site or observation bank is
introduced; runtime headroom and scan latency remain unmeasured on hardware.

Final host logs, native receipts/ELFs and the memory/visual review are retained
under `artifacts/rev107-host-evidence`, `artifacts/rev107-native-evidence` and
`artifacts/device-rev107`. This revision has not accessed or flashed the device.
The existing rev106 installation receipt remains the last installed evidence.

## Matched local package

Implementation commit `ff5f3379073f3c6331beee06f114dfe4378abc87` is retained
locally. Package
`dist/cerberus-unchained-0.1.0-dev.rev107-20261101T104658Z` passes hash, chip
header, partition and image-bound verification. All three packaged application
hashes match the retained five-target native matrix, and the packaged source
subset matches both final host/native input receipts.

- Source SHA-256: `98233048155ed5b8af4770051e7c6043bc49b88c8142d745614620acff1c74c0`.
- Manifest SHA-256: `388af56473e6a0e470d89b93ebe9e6c6603a7fd7881ec2d2a0f15c1ba3e66d3f`.
- S3 application: 1,910,736 bytes; C5: 1,664,464 bytes; bridge: 255,296 bytes.

`artifacts/device-rev107/local-milestone.json` binds the exact implementation
commit, package, reviewed native ELF, final logs and unchanged source hashes.
Its `installed` field is false. Rev107 was not flashed, pushed, submitted as a
pull request or selected for the public web installer. No new physical feature
acceptance is claimed.

The next substantive C5 work is full-callback classification and selected-family
retention, so longer valid originals and rare matching observations are not
limited by preview-only interpretation or the generic bank's admission policy.
That work must retain the exact boot/job/epoch/frozen-snapshot ownership and
separate source timing already used here.
