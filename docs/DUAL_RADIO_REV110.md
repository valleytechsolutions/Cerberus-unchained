# Rev110: C5 scan-response collection and saved evidence

Rev110 adds optional C5 scan-response requests to ordinary Bluetooth discovery
and the six family views. The S3 retains its independent observations and
coordinates the combined list, transport, display and SD storage. The verified
installed baseline is rev109/C5 firmware66. This checkpoint is a local candidate
until an explicit installation receipt says otherwise; no physical response,
touchscreen or SD acceptance is inferred from a build.

## Behavior

The C5 companion starts with PASSIVE selected. Its acquisition control can
select RESPONSE REQS for the next session. Changing acquisition while running
drains and releases the old session before starting the new one. Response mode
uses active 1M reception and keeps coded PHY reception passive. It does not
connect to advertisers or initiate GATT operations. Unsupported firmware and
allocation failures remain visible while the independent S3 path remains usable.

Each retained C5 row distinguishes original ADV from matched RSP. RAW, RX and
HINTS show the selected source; response RSSI, name and age do not replace
original report counts, original bytes or original timing. A response name is
an explicitly labelled fallback, and family/recent filters consider independent
source evidence. Selecting a row freezes the evidence used by Details and SAVE.

The six family views can admit a response-only match, including evidence beyond
the 31-byte preview. Eight bounded original candidates support promotion into
the existing 32-row inventory. Classification uses the complete delivered
callback, up to 255 bytes, while retention stores a 31-byte preview, a bounded
name and a 48-byte decoded family record. This does not preserve omitted raw
bytes for arbitrary future parsing or prove device identity.

## Matching and lifecycle

Response association requires the same typed address, active session/epoch,
callback source format, advertising-set SID and compatible receive properties.
The original must be complete, well-formed and scannable. Full-width ordered
callback sequences and C5 monotonic times limit the association interval to
2,000,000 microseconds. A newer original invalidates the previous response
binding before malformed, incomplete or invalid-time early returns. A later
response cannot attach to an older retained raw advertisement.

This is bounded host correlation. The SDK callback has no exact air-transaction
identifier, and broadcast evidence is unauthenticated. NONE means no matching
response was admitted for the retained eligible original; UNKNOWN means no
current binding is proved. Neither proves that no response existed over the air.

The C5 response context is allocated only for explicit response sessions, with
no per-callback allocation or additional task. It is released only after a
confirmed quiescent detach/error boundary. Failed cancellation retains ownership
and storage so an in-flight callback cannot access freed memory. Passive scans,
Analyzer and Watch retain original-only report semantics.

## Protocol and cost

Onboard firmware67 adds modes4/5 and command133. The new frozen projection is
310 bytes: the unchanged 158-byte family prefix plus a 152-byte response suffix.
Old 102/110/158-byte reads remain compatible. Firmware capability is checked
before the new RF operation. An unsupported explicit response request does not
silently fall back to passive acquisition.

The first-BEGIN S3 age anchor, frozen handle, retry idempotence and final status
fence remain mandatory. Offsets now support the larger row. Stop takes priority
at every transfer boundary; the existing four-call/eight-millisecond local
budget is unchanged. A full 32-row response bank needs 992 read transactions
versus 512 for family originals, plus BEGIN/inventory/fence. Only the opt-in
mode pays this transfer cost. No scan speedup or doubled coverage is claimed.

## SD reports

The v6 writer uses 432-byte rows, a 32-byte header and a maximum 27,680-byte file
for 64 observations, at most 32 per receiver. The v6 reader still accepts v1-v5 files.
The original v5 prefix stays unchanged. C5 response bytes and an independent
validity-tagged upper age bound at save follow it. Old files decode with C5
response UNKNOWN; reopening them does not invent missing proof.

The offline viewer separates ADV/RSP and RAW/RX/HINTS, including historical
response age and decoded evidence. Loading never queries a current clock,
starts a radio, or turns a saved row into an action target. Exact S3 response
timestamps and conservative C5 age bounds remain distinct. See
[the archive format](BLE_ARCHIVE_FORMAT.md) for the byte layout.

## Validation status

Focused host checks cover real C5 producer-to-S3 transfer, all six response-only
families, full255-byte reports, typed-address/SID collisions, newer malformed
originals, sequence/time boundaries, candidate eviction, allocation failures,
failed Stop, orphan cleanup, every transfer cancellation boundary and final
fence rejection. Archive checks cover an independent struct/zlib v6 response
golden vector, all five older readers, canonical mutations, transactional output,
64 mixed rows, immutable save input and actual file rollback/cancellation.

All **124 host suites** and **five native targets** passed with unchanged,
matching source fingerprints. Native targets are the S3 application, onboard
C5, temporary USB bridge and both expansion C5 profiles. The retained receipts
and exact per-suite log hashes are under `artifacts/rev110-host-evidence` and
`artifacts/rev110-native-evidence`.

The codec fixture passed 16,448,835 assertions; actual filesystem checks passed
591,775 across 48 isolated cases; async lifecycle checks passed 19,194; live
capture/save/reopen passed 3,566; saved renderer checks passed 18,531. The real
C5 producer-to-S3 coordinator fixture and all six family fixtures passed.
Seven actual-renderer previews were inspected and retained with matching
hashes after the final suite. These are host renderings, not device screenshots.

Native builds used the installed PlatformIO 6.2.0 / ESP-IDF 6.0.1 toolchain.
The S3 READY-ISR and console-stack gates passed. The resource audit reads the
retained ELFs and ties the rev109 comparison to its verified installation
receipt. It does not connect to the device.

## Measured resource costs

| Native allocation or fixed section | Rev109 bytes | Rev110 bytes | Change |
| --- | ---: | ---: | ---: |
| S3 fixed data + BSS | 172,828 | 173,596 | +768 |
| C5 fixed data + BSS | 83,152 | 83,160 | +8 |
| Optional S3 dual-BLE store | 11,616 | 21,496 | +9,880 |
| Optional S3 discovery store | 20,800 | 21,440 | +640 |
| Optional SD viewer | 38,768 | 61,112 | +22,344 |
| Optional SD worker | 36,144 | 57,648 | +21,504 |
| Optional viewer + worker overlap | 74,912 | 118,760 | +43,848 |
| All four retainable S3 stores above | 107,328 | 161,696 | +54,368 |
| Optional C5 response context | absent | 11,736 | +11,736 when requested |

S3 stores use PSRAM and may coexist. These are allocation layouts, excluding
allocator overhead, fragmentation and other objects, not measured free heap.
The C5 context belongs to the C5 heap and must not be added to S3 memory demand.
Passive C5 scans do not allocate it; the fixed C5 increase includes the pointer
and alignment. The existing C5 frozen original store remains 5,104 bytes.

Selected linked entry frames include C5 `response_receive_event` at 448 bytes,
`gap_event` at 64, the family parser at 128 and AD-hint parser at 144. S3 capture
mapping is 592 (+176), archive row decoding 528 (+160), dual-BLE tick 448 (+96),
while the UI main frame remains 1,136 and archive worker entry 48. Both NimBLE
host tasks retain their configured 4,096-byte stacks; S3 main retains 8,192. Entry
frames are not full call-chain bounds or measured runtime headroom. Loaded
high-water measurements remain pending.

The detailed source-bound audit is retained at
`artifacts/device-rev110/native-memory-comparison.json`. Its passed status
means the measurements and evidence bindings succeeded, not a promise of
worst-case memory or RF performance.

## Local package

`dist/cerberus-unchained-0.1.0-dev.rev110-20261101T123646Z` contains the matched
S3, C5 and temporary bridge images. Hashes, chip headers, partitions and image
bounds pass verification. Its application binaries match the retained native
matrix. `artifacts/device-rev110/local-milestone.json` records the final exact
commit, package/source hashes and evidence bindings once finalized.

The package is **not installed**. Rev109 remains the verified installed
baseline, with its feature-acceptance limitations preserved in
[the rev109 checkpoint](DUAL_RADIO_REV109.md). No release or PR is published.

## Remaining acceptance

Physical checks remain for response delivery on legacy and extended formats,
response-only names/families, repeated Stop/Back/Wi-Fi transitions, live and
saved source selectors, SD save/reopen, dense RF inventory and loaded heap/stack
headroom. Coded PHY remains passive. S3 native extended reception, complete raw
payload retention, GPS, six-radio backboard acceptance, prior active Wi-Fi
errors and full Marauder parity remain outside this milestone.

A small follow-up optimization is identified: complete passive EXT callbacks
currently check response mode in both `gap_event()` and the common original
copier. Moving the latter dispatch into the legacy wrapper would remove one
redundant mutex take/give per complete passive EXT callback while preserving
the EXT early-invalidation boundary. This does not affect source correctness;
its timing benefit is unmeasured and the change is outside this frozen build.

No PR, release, publication or device flash is performed by these build tools.
