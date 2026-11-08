# Rev120: exact shared S3 BLE host claims

Rev120 is a local shared-host lifecycle candidate following
[rev119's Wardriving binding](DUAL_RADIO_REV119.md). Rev113/C5 firmware 68 remains
the last verified installed baseline. Full Marauder parity and physical
acceptance remain unfinished.

## Shared host ownership

The host API now returns a nonreused 64-bit claim token bound to its owner and
host epoch. Release checks all three together under a short critical lock;
there is no owner-only release API. Reset retires the current lease atomically,
so stale cleanup cannot release a replacement with the same owner name.

Initialization and synchronization waits retain a separate pending reservation
until their caller exits, including after reset invalidates the candidate
lease. That reservation blocks a second initialization, another BLE claim and
the S3 Wi-Fi scanner. SDK calls, heap observations and waits occur outside the
critical lock. Failed admission clears its output and retires only its own
candidate. Token/epoch exhaustion and host-task exit fail closed.

`lease_valid()` establishes retained ownership. `host_synced()` separately
reports SDK readiness; losing synchronization alone cannot make a caller
forget a still-held claim. The owner getter is an admission/diagnostic label,
including a pending reservation, and is not evidence of caller ownership.

## Existing callers

The scanner retains one exact host lease and reserves its own acquisition
across allocation and host initialization. Generic scanning, passive Recon
and Wardriving, discovery, TRACK, Analyzer and MAC Monitor use that lease for
cleanup. Same-owner/same-epoch replacement invalidates pending or running
work. Intentional release preserves completed evidence; a rejected claim
cannot overwrite a retained cleanup handle. Public scanner state shapes and
existing result banks remain unchanged.

The central and advertiser retain their exact claim and bind callbacks to
nonreused 32-bit tickets instead of truncating a 64-bit token into the ESP's
pointer-sized callback argument. Central connection, service, characteristic,
read and sound operations keep separate tickets. Existing public generation
fields remain. Deferred release uses the captured lease rather than reading
a possibly newer global claim after unlocking. Ticket exhaustion fails closed.

Central's scanner handoff waits for the existing caller-owned drain. It no
longer sends a generic Stop to a scanner it does not own. Ordinary selection
and Find My routes already require release before the central request. Radio
Lab uses a worker-local lease, and Wardriving's first-use initializer releases
its exact temporary claim. These changes preserve existing payloads, rates,
targets and protocol operations.

Checking a lease and subsequently calling an SDK operation is not atomic with
reset. The new API proves claim/release identity, not complete RF-command
serialization. Broader queued-command/reset analysis, C5 ownership parity and
physical concurrent-reset behavior remain separate work.

## Validation and resources

Host tests exercise internal/external memory configurations and the disabled
branch. They cover same-owner replacement, token/epoch mismatch, reset during
initialization and synchronization, retained pending admission, exhaustion,
host-task exit and replacement during delayed diagnostic capture. They reject
SDK, heap and wait operations inside the host critical section.

The actual scanner fixture passes 374 additional host-token assertions in each
legacy and extended configuration, preserving existing passive, Wardriving,
TRACK, Analyzer and discovery regressions. The central fixture passes 10,890
checks, including 9,409 exact-lease and stale-callback checks, plus its disabled
branch. A reentrant successful-write completion test first reproduced stale
cleanup disconnecting a replacement; it passes after the callback captures its
lease before unlocking. Advertiser and actual-host Radio Lab tests exercise
replacement during startup, normal operation and Stop retries without altering
protocol behavior.

The complete Wardriving service fixture passes 7,234 checks, including 5,691
lifecycle checks. Its warm-up cases distinguish rejected admission from an
accepted lease retired or replaced before release. The Wi-Fi fixture compiles
the actual S3 Wi-Fi scanner and shared BLE host: reset during pending BLE
initialization blocks a second claim and Wi-Fi Start, then permits reuse after
the old claimant exits without initializing the BLE host again.

Independent source review found no remaining blocker within this claim/release
scope. All 124 registered host suites pass on unchanged final inputs. Radio
Lab (`check_radio.py`) and the advertiser lifecycle suite
(`check_s3_ble_attack.py`) are not part of that registry; both also pass with
separate retained receipts and logs bound to the same input map. The earlier
124-suite run before the write-completion correction is preserved separately
and is not the final checkpoint's validation.

All five native targets pass: main S3, onboard C5, temporary S3/C5 bridge,
expansion WROOM and expansion XIAO. Their full input map matches the retained
124-suite receipt and both supplemental receipts. This run used PlatformIO
Core 6.2.0, ESP-IDF 6.0.1 and Xtensa/RISC-V GCC 15.2.0+20251204; other documented
environments were not separately exercised. Host fault injection and native
measurements do not establish physical radio, touchscreen, SD durability or
sustained memory acceptance.

## Native resources and package

The native comparison uses the verified local rev119 candidate. Neither
rev119 nor rev120 has been installed on the device.

| Native measurement | Local rev119 | Local rev120 |
| --- | ---: | ---: |
| S3 fixed data and BSS | 173,884 bytes | 173,996 bytes |
| C5 fixed data and BSS | 83,160 bytes | 83,160 bytes |
| Wardriving PSRAM workspace | 22,464 bytes | 22,464 bytes |
| Wardriving status type | 384 bytes | 384 bytes |
| BLE memory diagnostic type | 200 bytes | 200 bytes |

The new exact host lease is 16 bytes. It was absent from rev119; absence is
not presented as a zero-byte runtime cost. The net S3 fixed-section increase
is 112 bytes, including the retained caller leases and callback tickets.
Existing scanner state types, result/history banks and optional workspace
sizes are unchanged. No new task or allocation was added. Fixed sections and
type sizes do not establish free heap, fragmentation or runtime headroom.

The selected host-claim entry frame remains 64 bytes, while host release
changes from 48 to 32. Passive scanner Start grows from 112 to 144 bytes,
common Start from 80 to 96, and scanner Stop from 32 to 48. The advertiser
worker entry grows from 288 to 304. The UI entry remains 1,072 bytes and the
Wardriving worker entry remains 160, with its existing 6,144-byte task stack.
Selected entry frames do not establish a complete call-chain bound; absent
or inlined helpers are not assigned zero cost. The separate native console
stack check passes against the final S3 ELF.

Final S3/C5/bridge application sizes are 1,952,928 / 1,678,960 / 255,296 bytes.
The WROOM and XIAO expansion applications remain 926,400 and 930,624 bytes.
The S3 image grows by 3,056 bytes relative to rev119. The native audit is
retained at `artifacts/device-rev120/native-memory-comparison.json`, SHA-256
`1c3f170d3712ea91e50f412820b24da32c9433a66eca6d4f1f359000f87a8ab8`.
The final host/native matrices are retained under
`artifacts/rev120-host-evidence` and `artifacts/rev120-native-evidence`.
The two supplemental lifecycle receipts/logs are under
`artifacts/device-rev120`.

The matched package is
`dist/cerberus-unchained-0.1.0-dev.rev120-20261101T181255Z`, with manifest
SHA-256 `cb6421fb08c8fac52a5775318f659cc2246553d558a5579b09d05de3336a13d9`.
All three packaged application hashes match the retained full native matrix.
The read-only verifier passes image hashes, chip headers, partition tables
and image bounds. The local milestone receipt binds committed source,
package, host/native matrices, both supplemental lifecycle receipts and the
native audit. No image was flashed or published; rev113 remains the last
verified installed baseline.

## Coverage and acceptance

The coverage ledger advances to `rev120-source` without promoting a mode:
95 upstream modes remain tracked, with 74 partial, nine needing review, four
hardware-limited and eight pending. Remaining work is recorded in
[the source audit](RECEIVE_REMAINING_GAPS.md).
