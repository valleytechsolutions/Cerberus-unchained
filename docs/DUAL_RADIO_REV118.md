# Rev118: exact S3 Recon ownership

Rev118 is a local receive-side Recon candidate following
[rev117's Fox Hunt binding](DUAL_RADIO_REV117.md). Rev113/C5 firmware 68 remains
installed. Full Marauder parity and physical acceptance remain unfinished.

## Accepted S3 acquisition and cleanup

Recon now receives an explicit passive scan attachment, identified by a
nonreused 64-bit capture token, full callback epoch and BLE host epoch. Status,
Stop, release and completed evidence reads validate that binding under the
existing scanner mutex. A public generation byte or generic SCANNER owner
label cannot establish Recon ownership.

The scheduler has a separate local-admission acknowledgement. A definite
rejected Start with no accepted or held attachment clears its pending owner
while preserving the existing retry/backoff policy. It cannot interpret a
later foreign RUNNING observation as its own success. C5's transport-delivery
ambiguity handling remains unchanged, and its Wi-Fi scheduler continues
independently of S3 failure or cleanup.

An error return is not, by itself, proof that acquisition held nothing. Failed
rearm retains the previous exact attachment; a failed post-claim cleanup
returns the held token. The UI preserves that handle and continues scoped
cleanup, including after scheduler retry exhaustion. A timeout or failed
status query does not abandon ownership. Proven absence of the token does
not authorize Stop against its replacement.

Completed Recon evidence stays attached until Watch consumes it or the user
leaves, pauses or sleeps. Discovery, Analyzer, Fox Hunt, split Wi-Fi and the
existing takeover check wait for that release. Native BLE admission checks also
reject an attached passive session. A retained completed bank cannot be
reserved by MAC Monitor or overwritten by a generic scanner Start.

## Coherent completed evidence

Each S3 row copy now retrieves peer identity, raw advertisement preview,
focus/response evidence, index, generation and original observation time
together under one lock. It reuses the existing scanner bank and UI staging
and evidence allocations. A final full binding check precedes publication;
failure retains the previous published bank and capture identity.

Watch reads raw bytes from that frozen UI evidence. Its extra protocol rows
and activity counters use the same full capture token, followed by a final
native check before classification, inventory publication, alerts or recorder
offers. Consumption deduplication uses the full S3 capture token. The original
source observation time remains distinct from UI publication/consumption time.
Raw previews remain bounded; this is not a new lossless packet archive.

The phase line distinguishes queued acquisition, SDK-confirmed reception,
source error and held cleanup. When pause is requested, both the phase and
badge report PAUSING while either scheduler or the S3 attachment is still
held; PAUSED requires release.
The Resume control retains the user's pause intent in either state. Demo mode likewise
does not claim radios stopped while the passive attachment remains. Rendering
adds no radio command or retry. It presents the last observed source state.

## Focused validation

The actual scanner suite adds 670 passive-binding assertions in each legacy
and extended BLE configuration, plus disabled-build canonical API checks.
It exercises rejected acquisition, queued Start/Stop, failed release, old-token
rearm failure, token exhaustion, generation-byte wrap, host resets, foreign
replacement and source-clock ambiguity. Existing Analyzer, TRACK, discovery,
MAC and Wardriving callback checks remain in the runner.

The display-store fixture compiles the production capture and final binding
checks. Sixteen additional same-generation mid-copy/final-check failures cover
full capture identity, callback/host epoch, start time, ownership, stopping,
error and state changes. Every case retains the old visible bank, evidence,
capture identity and publication time. Existing PSRAM allocation failures,
Wi-Fi transfer budgets, cancellation and cross-radio publication tests remain.

The actual Recon lifecycle fixture passes 814 checks alongside 1,194
recorder/Watch checks and 89 local-admission acknowledgement checks. It runs
production acquisition, scheduling, capture, Watch ingestion, history and
Back routes. Native scanner and C5 command transport boundaries are modeled;
clock, allocation/display, recorder and notification boundaries also use host
scaffolding.
Cases include C5 offline, either source failing independently, initial and
rearm rejection, accepted errors with retained cleanup, 18-second held Stop,
hidden/sleep behavior and both accurate old-attachment and empty-output
uncertainty responses. Existing C5 ambiguous-delivery behavior is tested
separately from definite local rejection.
The releasing-only incremental view update redraws the correct PAUSING/PAUSED
badge in either direction without repainting controls or making a radio call.

The live discovery/navigation fixture passes 1,908 checks, including retained
Recon attachments blocking discovery and Fox admission even when their legacy
scheduler is idle. Analyzer's 550 and Fox's 387 UI assertions remain. The split
Wi-Fi fixture holds a completed Recon attachment for 13 ticks: it starts no
scan until exact release, then starts exactly once. The Wi-Fi polling fixture
also preserves its 106 checks while exercising the real Recon adapter.
Rendered queued, rejected-start and held-pause previews were reviewed; they
are synthetic host fixtures, not photographs of the device.

All 124 host suites pass on the final unchanged inputs. The retained receipt
binds every test log by hash. All five native targets also pass: S3 application,
onboard C5, temporary S3/C5 bridge, expansion WROOM and expansion XIAO. The native
input map matches the retained host receipt and current source bytes.
This native run used PlatformIO Core 6.2.0, ESP-IDF 6.0.1 and the installed
Xtensa/RISC-V GCC 15.2.0+20251204 toolchains. It does not establish a separate
run with every environment described by inherited setup documentation.

The first native attempt caught misleading indentation in the new BLE-disabled
API stubs. Separating their independent statements fixed the bridge build
without changing behavior. The complete host and native matrices were rerun
after that correction; the earlier attempt is retained separately.

## Native resources and package

The native comparison uses the verified local rev117 candidate. Neither that
baseline nor rev118 has been installed on the device.

| Native measurement | Local rev117 | Local rev118 |
| --- | ---: | ---: |
| S3 fixed data and BSS | 173,660 bytes | 173,884 bytes |
| C5 fixed data and BSS | 83,160 bytes | 83,160 bytes |
| Exact S3 passive state | Absent | 56 bytes |
| Watch view | 32 bytes | 36 bytes |
| Watch cached rendering snapshot | 192 bytes | 192 bytes |
| One maximum 32-row frozen evidence allocation | 8,576 bytes | 8,576 bytes |
| Archive viewer plus worker type demand | 118,760 bytes | 118,760 bytes |

The fixed S3 increase is 224 bytes. Existing scanner snapshot, scan display
store, discovery, Analyzer and archive types do not grow. Evidence replacement
can retain the old and new allocations together until publication, giving
17,152 bytes for those two evidence allocations alone, excluding allocator
overhead and all other stores. Type sizes and fixed sections do not measure
runtime free heap, fragmentation or peak whole-device demand. The new passive
state has no rev117 counterpart; absence is not a zero-byte baseline.

The selected `live_capture` entry frame grows from 240 to 480 bytes and
`live_observe` from 80 to 112. Watch consumption remains 192, Watch drawing 272
and the main UI entry 1,072 bytes. The new passive Start and row-copy entries
are each 112 bytes; its state getter is 32. Inlined or absent helpers are not
assigned zero cost. These entries are not a complete call-chain bound or
evidence of runtime stack headroom. The separate native console stack check
passes against the final S3 ELF.

Final S3/C5/bridge application sizes are 1,948,800 / 1,678,960 / 255,296 bytes.
The WROOM and XIAO expansion applications are 926,400 and 930,624 bytes.
The audit is retained at `artifacts/device-rev118/native-memory-comparison.json`
with SHA-256
`42a3a224d27aa800a2936b63f336959cfa49acc79fb2c4f89922f93e9a1e401d`.
Host and native evidence is retained under `artifacts/rev118-host-evidence`
and `artifacts/rev118-native-evidence`.

The matched package is
`dist/cerberus-unchained-0.1.0-dev.rev118-20261101T172149Z`, with manifest
SHA-256 `bb212c28a19bd04bd6896dfa3240750c264f160cebdc792d26c44804191f647b`.
The read-only verifier passes image hashes, chip headers, partition tables and
image bounds. The local milestone receipt binds the committed source, matched
package, host/native receipts and native audit. No image was flashed and no
release or pull request was published. Rev113 remains installed.

## Scope and acceptance

The coverage ledger advances to `rev118-source` without promoting any mode:
95 upstream modes remain tracked, with 74 partial, nine needing review, four
hardware-limited and eight pending. Full Stop behavior across every workflow
is still unproven. This S3 change does not establish C5 transport ownership
parity or migrate Wardriving to the new lease API.

The remaining Wardriving ownership/source-failure policy, GPS and persistence
gaps are tracked separately in [the remaining-work audit](RECEIVE_REMAINING_GAPS.md).
Concurrent RF reception, touchscreen transitions, sustained heap/stack behavior,
SD recording, GPS and the six-radio backboard remain physical acceptance work.
This local candidate does not change the installed rev113 baseline.
