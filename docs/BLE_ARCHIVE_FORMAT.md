# Saved BLE evidence format

This guide describes the v6 format introduced by rev110. The verified installed baseline remains rev109. [The rev110 checkpoint](DUAL_RADIO_REV110.md) distinguishes code/build evidence from pending physical response and SD acceptance; historical checkpoints retain their own installation scope.

Version 6 writes a bounded evidence report of one to 64 retained BLE observations, at most 32 per receiver. The reader also accepts version 5 full-original family files, version 4 receive-envelope files, version 3 files with separate S3 response evidence, version 2 dual-receiver files and version 1 files, which contain at most 32 observations with no encoded receiver identity. The pure codec is in `s3_main/main/ble_archive.c` and `ble_archive.h`. Its receive-metadata type and validator come from the portable `shared/dual_ble_protocol.h`; full-family evidence uses `shared/ble_family_wire.h`. It performs no allocation, filesystem operation, radio operation, or ESP-IDF call.

| Version | Header | Row | Maximum count | Maximum file |
| --- | ---: | ---: | ---: | ---: |
| 1, read compatibility | 32 bytes | 128 bytes | 32 | 4,128 bytes |
| 2, read compatibility | 32 bytes | 144 bytes | 64, at most 32 per receiver | 9,248 bytes |
| 3, read compatibility | 32 bytes | 208 bytes | 64, at most 32 per receiver | 13,344 bytes |
| 4, read compatibility | 32 bytes | 216 bytes | 64, at most 32 per receiver | 13,856 bytes |
| 5, read compatibility | 32 bytes | 264 bytes | 64, at most 32 per receiver | 16,928 bytes |
| 6, current writer/reader | 32 bytes | 432 bytes | 64, at most 32 per receiver | 27,680 bytes |

Files retain the `.BLE` extension. Older readers may reject version 6 files; compatibility means that the new reader preserves support for old files, without silently rewriting them. Encoding a decoded version 1 model produces an explicit version 6 report with `LEGACY` receiver labels. V1/v2 decode with response state `UNKNOWN` and all response metadata zero; v3 retains its original response bytes and timestamps. All v1/v2/v3 records decode with receive metadata `UNKNOWN` and eight zero fields. Old C5 provenance or raw bytes cannot reconstruct an absent callback envelope. All v1–v4 records decode with all 48 full-family bytes zero (absent). Re-encoding old files as v6 does not reconstruct those missing hints; v4 receive metadata and v3 response evidence remain intact. Every v1-v5 file decodes with C5 response UNKNOWN and no response age bound.

This format preserves retained evidence for offline inspection. Loading it does not create live scan results, connection targets, or permission to use an active feature. It is distinct from Marauder's JSON AirTag log format and does not import that format.

## Evidence and time limits

- An entry retains the receiver, typed BLE address, a projected received name, explicitly described RSSI, peer flags, manufacturer and UUID metadata, and up to 31 original advertisement bytes. Address bytes use NimBLE order, with the least significant octet first. Identity is `(receiver, address type, address)`: the same typed address heard by S3 and C5 is two observations, not evidence of two physical devices. `LEGACY` means the source was unspecified in version 1; decoding does not invent an S3 receiver label.
- The name is a bounded C-string projection from the peer result. It is **not** a lossless copy of the original over-the-air name: earlier peer conversion may have discarded bytes following an embedded NUL, and the C5 companion has already sanitized its display projection. The codec preserves the bytes it receives, including S3 control/non-ASCII bytes; it cannot restore an earlier name or infer it from unrelated raw data.
- Advertisement bytes, event type, and flag bits are preserved even when the retained advertisement is malformed, truncated, or unfamiliar. A malformed AD element does not invalidate the archive container. Offline parsers must check AD lengths independently and label truncation or malformed contents.
- Versions 3 onward can retain up to 31 bytes of the latest admitted S3 scan response independently of its original advertisement. They record the delivered response length, retained length, event, truncation, source SID where valid, ordered original/response sequences, their observed low32 source times, and an independently valid exact64 response time. They do not serialize pointer-containing focus objects, response RSSI, or older response history. A response longer than 31 bytes keeps only that prefix; the prefix can end inside an AD element. This is not full extended-advertising support.
- For the S3 response block at offsets144..207, `UNKNOWN` means response availability cannot be proved for this saved original, including old files, C5/LEGACY rows and stale raw ADV retained after a newer invalid original. `NONE` means the frozen valid S3 original has no **admitted matching** response at capture; it does not prove that no response existed over the air. `PRESENT` means the separately saved response passed the same-source binding checks. Non-PRESENT response fields must be zero. C5/LEGACY rows must be UNKNOWN in this S3 block. C5 passive discovery does not collect scan responses; v6 uses a separate C5 response block for explicit response-request acquisition.
- The S3 mapper attaches response bytes only after matching the frozen base row's typed identity, original raw bytes/event/flags/report count, generation, full nonzero source epoch and exact original/copy clocks. The source helper also requires the raw advertisement sequence to match the current valid original sequence. A newer malformed/incomplete original, including one observed in the same millisecond, cannot attach later response hints to an older raw copy. Response capture does not update original bytes, original time, RSSI or active-action eligibility.
- V4 onward separately retain the C5 original callback's eight-byte receive envelope: callback source kind, properties, delivered length, SID, primary/secondary PHY, controller data status and independent full-AD validation. It is copied from the same frozen row as raw bytes/RSSI/age and validated against that row's retained raw length/event/flags. A known envelope is allowed only for C5. S3 and archive `LEGACY` receivers keep canonical UNKNOWN; a matching S3 response SID is not C5 receive evidence. Metadata source kind `LEGACY` means a known legacy callback received by C5 and is distinct from archive receiver `LEGACY`.
- Known legacy callback metadata has unavailable status/SID `0xff`, no observed PHY (`0/0`) and zero properties. Known extended metadata describes a complete delivered original report, with the actual source SID/PHY values or explicit unavailable SID `0xff`. Delivered length is bounded to 255, raw retention remains 31, and full-AD validity can remain VALID even when the retained prefix cuts an AD element or NimBLE's parser rejects another aspect. This is historical source metadata, not complete payload retention, fragment reassembly, authenticity or proof that the advertiser remains nearby.
- V5 optionally retains the C5's canonical 48-byte full-family proof from the same frozen original. It preserves the selected scan family, independent hint categories and details, including the full iBeacon tuple and Meta identifier/context pairs. Full delivered callbacks can be up to 255 bytes, while the saved raw preview remains at most 31 bytes. This evidence is not a retained copy of the omitted payload; it records the producer's bounded interpretation and cannot support arbitrary later re-parsing of the missing suffix.
- A zero family block means absent, including v1-v4 files and ordinary C5 discovery rows. V5 files can retain this proof. A nonzero version-1 block can describe a complete classified original (including no supported hints or loss of the selected family), or explicit unclassifiable evidence. A later complete malformed/unknown-envelope original can dequalify a retained typed row; that state is saved, not replaced with an earlier or short-preview positive hint. S3 and LEGACY receivers require the entire family block zero. Family hints never authorize actions or assert authenticated device identity.
- `S3` and `LEGACY` entries use original-report sweep-peak RSSI, which need not describe the particular retained ADV bytes. C5 entries use latest original-ADV RSSI. The name, manufacturer and UUID remain aggregate peer hints, not a claim that every value occurred in that raw packet. An unavailable RSSI is 127 with its validity bit clear, for either RSSI kind.
- `saved_uptime_ms` always describes the S3 save boot. For S3/LEGACY, valid exact source times satisfy `original <= ceiling <= saved`; zero is valid when explicitly marked valid. The copy ceiling is not a packet timestamp. Those receivers must have no C5 age bound or receiver boot ID.
- C5 rows require a nonzero committed C5 boot ID and valid nonzero source epoch. Exact S3 source/copy times are zero and invalid. An optional conservative **upper bound on age at save** is stored separately; it may exceed S3 saved uptime and is never subtracted to invent an exact packet timestamp. Unknown bounds have a clear validity bit and value 0. A valid bound of 0 or `UINT64_MAX` is distinguishable from unknown through that bit.
- `cerberus_ble_archive_age_at_save()` returns exact historical original-ADV age for S3/LEGACY. `cerberus_ble_archive_age_upper_at_save()` returns the C5 historical upper bound. `cerberus_ble_archive_response_age_at_save()` returns the independent exact S3 response age when available. All reject unavailable or invalid values without changing their output. Opening a file after reboot does not refresh any age; none accepts the loading boot's clock. There is no UTC timestamp or claim of present freshness.
- Scan epoch, generation, and original row index are historical source metadata. They are not an active scanner ownership token, an address identity, or an authorization binding.

## Header

All multibyte integers are unsigned little-endian. The format is independent of native C structure size, alignment, padding, and pointers.

| Offset | Bytes | Meaning |
| --- | ---: | --- |
| 0 | 8 | Magic bytes `43 42 4C 45 41 52 43 00` (`CBLEARC` followed by NUL) |
| 8 | 2 | Version 1 through 6; encoder writes 6 |
| 10 | 2 | Header size, exactly 32 |
| 12 | 2 | Entry size 128 for v1, 144 for v2, 208 for v3, 216 for v4, 264 for v5, 432 for v6 |
| 14 | 2 | Entry count 1..32 for v1, 1..64 for v2/v3/v4/v5/v6; each receiver is limited to 32 |
| 16 | 4 | Total file length, exactly `32 + entry_size * count` |
| 20 | 8 | Saved S3 uptime in milliseconds |
| 28 | 4 | CRC-32/ISO-HDLC |

The CRC uses reflected polynomial `0xEDB88320`, initial value `0xFFFFFFFF`, and final XOR `0xFFFFFFFF`. It covers the **entire header and all entries**, with bytes 28 through 31 treated as zero during calculation. This detects corruption; it does not authenticate the file or its claimed observations.

## Entry

Offsets below are relative to the beginning of each entry. Bytes 0..127 retain their existing fields, with the explicitly versioned additions noted below; v2 appends 16 bytes, v3 appends 64 response bytes, v4 appends eight receive-metadata bytes and v5 appends 48 full-family evidence bytes and v6 appends 168 C5 response/bound bytes. Earlier fields retain their offsets and meaning in later versions.

| Offset | Bytes | Meaning |
| --- | ---: | --- |
| 0 | 6 | Address in native NimBLE byte order |
| 6 | 1 | Address type, 0 through 3 |
| 7 | 1 | Validity bits: bit 0 RSSI; bit 1 exact source times; bit 2 scan epoch; v2 bit 3 age upper bound; other bits zero. v1 bit 3 must be zero |
| 8 | 1 | Name projection byte count, 0 through 32 |
| 9 | 1 | Signed two's-complement RSSI, -127 through +20 when valid; 127 when unavailable |
| 10 | 1 | Peer flags, preserved without reinterpretation |
| 11 | 1 | UUID length, 0, 2, 4, or 16 |
| 12 | 2 | Manufacturer identifier |
| 14 | 1 | Retained original advertisement length, 0 through 31 |
| 15 | 1 | Original advertisement event type, preserved |
| 16 | 1 | Original advertisement flags, preserved |
| 17 | 1 | Source generation |
| 18 | 1 | Original source index, 0 through 31 |
| 19 | 1 | v1 reserved zero; v2 receiver: 0 LEGACY, 1 S3, 2 C5 |
| 20 | 2 | Delivered original advertising report count |
| 22 | 1 | v1 reserved zero; v2 RSSI kind: 0 sweep peak, 1 latest original ADV |
| 23 | 1 | Reserved, zero |
| 24 | 8 | Scan epoch; nonzero if valid, otherwise zero |
| 32 | 8 | Original source uptime in milliseconds; zero if unavailable |
| 40 | 8 | Copy ceiling uptime in milliseconds; zero if unavailable |
| 48 | 32 | Name projection; no NUL inside its declared length; remaining bytes zero |
| 80 | 16 | UUID bytes; remaining bytes zero |
| 96 | 31 | Retained original advertisement bytes; remaining bytes zero |
| 127 | 1 | Reserved, zero |
| 128 | 8 | v2 onward: conservative age upper bound at save, zero if unavailable |
| 136 | 8 | v2 onward: nonzero C5 receiver boot ID; zero for S3/LEGACY |
| 144 | 1 | v3 onward: response state 0 UNKNOWN, 1 NONE, 2 PRESENT |
| 145 | 1 | Retained response length, 1..31 when PRESENT |
| 146 | 1 | Delivered response length, 1..255 when PRESENT |
| 147 | 1 | Response event: 4 without SID; 0 or 3 with valid SID |
| 148 | 1 | Response raw flags: bit 0 TRUNCATED iff delivered length exceeds retained length; other bits zero |
| 149 | 1 | Response/original source SID, 0..15 when valid, otherwise zero |
| 150 | 1 | Response validity: bit 0 SID, bit 1 exact response time; other bits zero |
| 151 | 1 | Reserved, zero |
| 152 | 4 | Original source sequence, nonzero for PRESENT |
| 156 | 4 | Response source sequence, strictly greater than original sequence |
| 160 | 4 | Original observed low32 source time in milliseconds |
| 164 | 4 | Response observed low32 source time in milliseconds |
| 168 | 8 | Exact response S3 source uptime, zero if unavailable |
| 176 | 31 | Retained raw response bytes; remaining bytes zero |
| 207 | 1 | Reserved, zero |
| 208 | 1 | v4 onward: receive source kind 0 UNKNOWN, 1 LEGACY callback, 2 EXTENDED callback |
| 209 | 1 | Receive properties; zero for UNKNOWN/LEGACY callback |
| 210 | 1 | Delivered callback length 0..255; zero for UNKNOWN |
| 211 | 1 | Source SID 0..15 or `0xff` unavailable for known metadata; zero for UNKNOWN |
| 212 | 1 | Primary PHY: 1=1M or 3=coded for extended; zero for UNKNOWN/LEGACY callback |
| 213 | 1 | Secondary PHY: 0=none, 1=1M, 2=2M or 3=coded for extended; zero for UNKNOWN/LEGACY callback |
| 214 | 1 | Controller status: 0=COMPLETE for extended, `0xff` unavailable for legacy callback; zero for UNKNOWN |
| 215 | 1 | Full delivered AD validation: 0 UNKNOWN, 1 VALID, 2 INVALID |
| 216 | 1 | v5: family proof version, 0 absent or 1 canonical full-callback proof |
| 217 | 1 | Selected family code: 1 iBeacon, 2 Find My, 3 Flipper, 4 Flock, 5 Meta hint, 6 HC name |
| 218 | 1 | Evidence state: 1 COMPLETE, 3 RECEIVE_UNKNOWN, 5 MALFORMED/unclassifiable |
| 219 | 1 | Independent categories: bits 0..5 correspond to the six family codes above |
| 220 | 1 | HC complete-name code: 0 absent, 3 HC-03, 5 HC-05, 6 HC-06 |
| 221 | 1 | iBeacon tuple result: 0 NONE, 1 FOUND, 2 MALFORMED/ambiguous |
| 222 | 1 | Proof flags: bit 0 AVAILABLE, bit 1 public-address Flock OUI hint, bit 2 same-report Meta exclusion present |
| 223 | 1 | Find My hints: bit 0 Apple layout, bit 1 FMNA, bit 2 DULT |
| 224 | 1 | FMNA UUID list widths: bits 0/1/2 for exact 16/32/128-bit encodings |
| 225 | 1 | Flipper advertised variant hints: bits 0..3 for 3080..3083 |
| 226 | 1 | Flipper evidence flags: UUID16/32/128 bits 0..2, matching complete name bit 3, named service data bit 4 |
| 227 | 1 | Flock same-report facts: FS battery bit 0, Penguin bit 1, decimal-name bit 2, no nonempty local name bit 3, 09C8 company bit 4, 09C8 payload bit 5 |
| 228 | 1 | Flock accessory UUID contexts: exact UUID128 list bit 2 and service-data bit 5 |
| 229 | 1 | Flock 3100 contexts: list16/32/128 bits 0..2, service-data16/32/128 bits 3..5 |
| 230 | 1 | Flock 3200 contexts, same mapping as offset229 |
| 231 | 10 | Meta identifier/context bitset: bit = identifier index × 7 + context, low bit first; upper three bits zero |
| 241 | 16 | Full advertised iBeacon UUID; zero unless tuple FOUND |
| 257 | 2 | iBeacon major, little endian; zero unless FOUND |
| 259 | 2 | iBeacon minor, little endian; zero unless FOUND |
| 261 | 1 | Signed two's-complement advertised iBeacon measured power; zero unless FOUND |
| 262 | 2 | Family reserved bytes, zero |
| 264 | 1 | v6 C5 response version: 0 for canonical UNKNOWN, otherwise 1 |
| 265 | 1 | C5 response state: 0 UNKNOWN, 1 NONE, 2 PRESENT |
| 266 | 1 | Flags: bit0 frozen age valid, bit1 RSSI valid, bit2 complete name, bit3 truncated name |
| 267 | 1 | Retained response raw length, 1..31 for PRESENT |
| 268 | 1 | Response event for PRESENT: 4 legacy or legacy-PDU extended callback, 3 native extended |
| 269 | 1 | Projected response name length, 0..32 |
| 270 | 1 | Signed response RSSI for PRESENT; 127 when unavailable; zero for UNKNOWN/NONE |
| 271 | 1 | Raw TRUNCATED flag iff delivered length exceeds31 |
| 272 | 8 | Nonzero full original callback sequence for NONE/PRESENT |
| 280 | 8 | Full response sequence, greater than original for PRESENT |
| 288 | 4 | C5 response age at the frozen snapshot, UINT32_MAX when PRESENT age is unknown |
| 292 | 4 | Response-minus-original C5 interval in microseconds, at most2,000,000 |
| 296 | 8 | Independent response receive envelope, same field layout as208..215 |
| 304 | 31 | Response raw preview; unused tail zero |
| 335 | 32 | Bounded projected response name; unused tail zero |
| 367 | 48 | Independent full-response family proof, layout216..263; family code0 permitted for generic response discovery |
| 415 | 1 | Reserved zero |
| 416 | 1 | Response upper age bound at SAVE valid: 0 or1 |
| 417 | 7 | Reserved zero |
| 424 | 8 | Independent response upper age bound at SAVE; zero if invalid |


The encoder ignores unused legacy name/UUID/ADV tails and writes canonical zero padding. New response tails must already be zero, matching the admitted source's bounded copy. It does not display-sanitize either packet. The decoder rejects noncanonical padding, unknown receiver/RSSI kind or container validity bits, unsupported lengths/version, per-receiver overflow, duplicate `(receiver, address type, address)` keys, invalid receiver/time combinations, truncated files and trailing bytes. Original source index remains 0..31 for every receiver; it is not the mixed report's row number. Unknown **original advertisement** flags and event types remain valid evidence when the response state is UNKNOWN/NONE and the receive envelope is UNKNOWN; they are distinct from reserved **container** validity bits.

The S3 block's PRESENT state requires an S3 receiver, valid nonzero scan epoch, an original event in 0..3, and original flags containing only TRUNCATED (which requires a full 31-byte retained prefix). The modular32 response-minus-original interval must be at most 2,000 ms. Retained response length equals `min(delivered_length, 31)`. Exact original time, when available, must match the original low32 field, and the response interval must fit within `ceiling - original` even if exact response time is unknown. That consistency check does not populate an unknown timestamp. Exact response time, when available, additionally requires exact original time and `original <= response <= ceiling <= saved`, the same full64 interval as the modular32 interval, and matching low32 response time. Genuine time zero is valid. The codec can represent PRESENT with explicitly unknown exact64 time; the current S3 capture mapper emits PRESENT only when both exact clocks are proved. These consistency checks do not authenticate a file's claimed radio provenance.

Known receive metadata requires a C5 receiver and the existing nonzero boot/epoch provenance. The shared validator requires original event 0..3, retained raw length `min(callback_length,31)`, raw flags limited to TRUNCATED/MALFORMED and TRUNCATED exactly when callback length exceeds 31. Known AD validation is VALID or INVALID. Legacy callbacks additionally require length at most 31, SID/status `0xff`, properties zero and primary/secondary PHY 0/0. Extended properties are limited to `0x1f`, exclude scan-response bit `0x08`, and use status COMPLETE, SID 0..15 or `0xff`, primary PHY 1 or 3 and secondary PHY 0..3. Extended reports carrying legacy PDUs require length at most 31, SID `0xff`, primary/secondary PHY 1/0 and these exact propertiesâ†’original-event pairs: `0x10â†’3`, `0x12â†’2`, `0x13â†’0`, `0x15â†’1`. Other native extended reports normalize to event 0 when connectable, otherwise event 3. UNKNOWN requires every receive field zero and does not tighten old original-ADV evidence validation. These envelope checks do not reparse a truncated prefix as a complete delivered payload.

Family proof version 0 requires all 48 bytes zero. Version 1 is C5-only, requires the row's existing valid nonzero boot/epoch provenance, and is checked by the same portable wire decoder as the live receiver against this row's typed address, original raw length/event/flags and receive metadata. AVAILABLE requires COMPLETE, known complete-original metadata and independently valid full AD layout. Raw preview MALFORMED/TRUNCATED can coexist with a valid full proof: those flags describe the bounded preview or SDK parser, not a reclassification of the complete delivered container. The Flock OUI flag must agree with the typed public/identity-public address. Unsupported states, flags, enum values, contradictory category/detail fields, invalid tuple padding and nonzero reserved bytes reject the whole file even with a correct CRC. Invalid nonzero proof never downgrades to absent evidence or a raw-preview fallback.

Unavailable proofs have zero category/detail/flag fields. RECEIVE_UNKNOWN corresponds to a canonical unknown receive envelope; MALFORMED denotes unclassifiable evidence with a known envelope. COMPLETE can be available with zero categories. The saved selected-family code describes the receiving session and does not require that a later retained original still qualify for that family. These are consistency checks, not cryptographic provenance or proof that the advertiser remains present.

The Meta bitset uses identifier order `FD5F, FEB7, FEB8, 01AB, 058E, 0D53, FD5A, FD69, 004C, 0006, FEF3`; the first six are allowed identifiers and the remaining five are exclusions. Context order is manufacturer16, UUID list16/32/128, service data16/32/128. A Meta hint requires at least one allowed pair and no exclusion pair from the same complete original. Tuples and other hints remain independent; ambiguous iBeacon fields do not invalidate a structurally valid original's independent Find My hint.

## Publication and validation

Decode validates the complete header, exact length, CRC, every entry, and all typed address duplicates before changing the destination document. Failure preserves the previous document in full. A successful decode clears unused model rows. Input must remain immutable during the operation, and input/output memory must not overlap. This protects the model from partial replacement; it does not by itself guarantee SD durability or make a filesystem rename power-loss atomic.

`tools/check_ble_archive.py` compiles the production codec and pure S3/C5 capture mappers. The historical rev102/v4 codec fixture passed 6,616,148 assertions: unchanged independent v1/v2/v3 vectors including v3 PRESENT response bytes/time, independent v4 UNKNOWN/S3-response/C5-envelope vectors, full 32-row v1 and 64-row v2/v3/v4 corruption and truncation, CRC-correct hostile fields, canonical re-encoding of UNKNOWN/PRESENT/known-envelope rows, all receiver/RSSI-kind and receive-metadata byte domains, source collisions, per-source limits, response time bounds, envelope/raw context, immutable input and failed-decode atomicity. That checkpoint retained the S3 capture fixture’s 2,523 assertions. The C5 fixture verifies frozen selected/publication receive metadata, raw/aggregate separation, historical age bounds and canonical UNKNOWN response fields. These are host checks, not physical SD or BLE reception acceptance.

The historical v4 host model was 15,376 bytes per 64-row document and 240 bytes per row, an increase of 512 bytes per document over v3; these are native host C type sizes, not wire sizes or new ESP32 measurements. The UI's retained/incoming documents and the worker's immutable copy remain bounded allocations. That v4 storage/IO contract used the 13,856-byte file limit and required preserving ownership, cancellation, close/rename and immutable-copy guarantees separately from codec validation. Native frame sizes and peak runtime memory require separate validation.


The rev109 v5 focused runner passes 8,323,571 codec assertions plus the C5 frozen-proof capture checks and unchanged 2,523 S3 capture assertions. Independent Python `struct`/`zlib` literals cover v5 absent-family, S3-response, C5-envelope and full255 HC-name proof records; unchanged v1–v4 literals remain reader tests. Coverage includes 64 mixed rows, absent old-file proof on re-save, complete/nohint/unavailable proof, all six families beyond the preview, repaired-CRC malformed fields, canonical byte re-encoding and last-row preflight failure without document mutation. This is host evidence, not physical SD acceptance.

The v5 host model measures 18,448 bytes per document and 288 bytes per row: +3,072 bytes per document over v4. Wire limit is 16,928 bytes. The retained/incoming documents and immutable worker copy remain bounded; native frame sizes, allocation totals, runtime headroom and SD durability require their separate validation receipts.


## C5 matched-response evidence in v6

S3 and LEGACY rows require the entire C5 response extension to be zero. C5
UNKNOWN is canonical all-zero. NONE has only version1, state1 and a nonzero
original sequence; it requires an eligible retained original. It means no
matching response was admitted, not that none existed over the air.

PRESENT requires an eligible complete, well-formed, scannable original and a
complete valid response envelope with the same callback source context, SID,
PHY and compatible properties. Original and response sequences are full64 and
ordered; their source interval is bounded to two seconds. The producer also
binds them to a live source epoch/session and invalidates matching before any
newer original's malformed/incomplete early return. These checks are bounded
host correlation, not proof of an exact air transaction or authenticated identity.

The 48-byte response family proof describes the full delivered callback up to
255 bytes, independently of the31-byte raw preview and original family proof.
Generic response discovery uses family code0 in this response-only domain;
selected family requests use codes1..6. Original family proof still requires
codes1..6. Invalid nonzero response evidence is rejected, never replaced with
a short-prefix interpretation. The response projection preserves its name
length and bytes; display sanitization does not modify stored evidence.

The PRESENT response age at the frozen C5 snapshot has an explicit validity flag;
UINT32_MAX means unknown, while age0 is valid. UNKNOWN/NONE keep age zero.
Save adds elapsed S3 time from
the first-BEGIN request anchor with checked64-bit arithmetic. Backward clocks
or overflow leave the SAVE bound unknown. If original and response bounds are
both valid, the original bound must be no smaller and their difference must lie between the
floor and ceiling of the source microsecond interval divided by1000. No exact
S3 packet timestamp is invented. `cerberus_ble_archive_c5_response_age_upper_at_save()`
returns this historical bound only, without a loading clock or radio query.

Original report counts, source clocks, bytes and RSSI are never refreshed by
response capture. These source domains also stay separate in the offline
ADV/RSP, RAW/RX/HINTS views. The file CRC detects corruption; it cannot prove
that a forged file's evidence was ever observed.


Rev110 focused checks additionally cover an independent v6 PRESENT response vector, all five historical versions, all six full255 response families, every new extension byte with repaired CRC, last-row atomic rejection, actual filesystem fault preservation, immutable async input and offline source-selected rendering. Final host/native/package evidence is recorded in [rev110](DUAL_RADIO_REV110.md); no physical SD acceptance is implied.
