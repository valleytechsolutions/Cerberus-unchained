# Rev119: exact S3 Wardriving ownership

Rev119 is a local receive-side Wardriving candidate following
[rev118's Recon binding](DUAL_RADIO_REV118.md). Rev113/C5 firmware 68 remains
the last verified installed baseline. Full Marauder parity and physical
acceptance remain unfinished.

## Acquisition, capture and cleanup

Wardriving uses the accepted 64-bit passive scan attachment for S3 acquisition,
status, rearm, completed evidence and cleanup. The native scanner validates
the full callback and BLE host epochs. A generation byte or generic scanner
owner label cannot establish Wardriving ownership.

The scoped whole-bank copy validates every row before copying the completed
BLE result and evidence arrays under one lock. It also returns the matching
passive state. It requires completed reception and released RF/queued work;
wrong tokens and failed checks clear the bounded outputs. Original source
identity, address type, time and separate advertisement/response evidence
remain together. Existing bounded buffers are reused.

The worker retains its attachment until copied rows are processed and cleanup
is confirmed. A later scan with the same generation byte cannot authorize
capture or Stop. An acquisition error with a retained token still requires
cleanup. A completed row cursor remains distinct from ownership so repeated
cleanup does not duplicate successfully appended observations.
An exact, error-free IDLE response after cancellation releases the unfinished
sweep without manufacturing a completed bank or error. A bank that completed
before Stop still drains once.

After the existing bounded C5 and expansion cleanup, an unresolved S3
attachment keeps the same worker and workspace in STOPPING. It retries scoped
Stop/release rather than treating elapsed time as proof of release. The
timeout is reported, but terminal state, file closure and workspace release
wait for authoritative cleanup. Worker cleanup never stops a foreign
replacement. BLE capture and SD append errors retain separate source labels;
a storage failure is not additionally labelled a BLE acquisition failure.

Back remains pending while the service is STARTING, RUNNING or STOPPING. Its
message now says `WAITING FOR RADIOS AND FILES`. A source-specific timeout can
be displayed while cleanup remains pending. Start waits for retained Recon
and discovery attachments, even when their legacy schedulers appear idle.

## Preserved policy and scope

The worker still ends the recording session on a terminal receiver or storage
error. This milestone does not silently turn such failures into a successful
single-source session. A degraded-source recording/retry policy remains
separate work. C5 acquisition/transport semantics and expansion cleanup are
not established as equivalent to the new S3 binding by this change.
Returned S3 status/copy timeouts also retain this terminal policy; they are not
newly retried as transient mutex contention during RUNNING. Cleanup itself
retains and retries the exact attachment. Source-specific retry semantics
remain part of the next policy audit.

First-use BLE host initialization retains its existing generic claim/release
before worker creation. A new epoch/owner check refuses admission after a
detected reset or replacement. It is not an atomic epoch-bound host operation;
the shared host's reset-versus-release race remains in the separate ownership
audit. Exact worker leases do not prove every host client is race-free.

GPS remains optional and OFF by default on entry. With GPS OFF, the worker
uses the existing unlocated CSV path. The integrated main-firmware Wardriving
page and optional R1–R6/GPS interfaces remain; no separate firmware is needed
to enter this workflow. This does not establish GPS or backboard hardware
acceptance.

## Validation

The actual scanner suite passes 626 additional whole-bank assertions in each
legacy and extended BLE configuration, plus the existing 670 passive ownership
checks and earlier scanner regressions. Disabled builds also exercise the new
API. Cases cover full 32-row banks, address types, independent advertisement
and response evidence, clock wrap, corrupt rows, transactional failure, queued
cancellation, failed release and host reset/replacement.

All 20 affected Wardriving runners pass. The full production-service fixture
passes 7,203 assertions, including 5,660 new lifecycle checks. It compiles the
actual worker, capture, recent-results and cleanup code with modeled native,
SD, clock and allocation boundaries. It covers rejected and accepted-error
Starts, exact-token replacement despite an equal generation byte, queued and
receiving cancellation, failed rearm, cleanup held for 18 seconds, repeated
sessions, SD errors on the first/second row, and completed counters counted
once. Thirteen resumed-capture proof mutations reject further appends while
preserving the existing frozen prefix. C5 ambiguity, expansion, GPS OFF and
storage checks remain in the run.

The actual UI route/render fixture passes 2,350 assertions, including rendering
bounds. It compiles the production Recon takeover predicate and discovery
release model, with counted service/storage/host boundaries. Retained Recon
and discovery attachments block Start. Back stays pending beyond the former
cleanup deadline and returns only on terminal service state. USB ownership
blocks Start; this fixture does not test a USB storage handoff. Normal cleanup
and held-timeout previews were visually inspected as synthetic renders.

All 124 host suites pass on unchanged final source/configuration inputs. The
retained receipt binds every test log by hash. All five native targets pass:
S3 application, onboard C5, temporary S3/C5 bridge, expansion WROOM and expansion
XIAO. The native input map matches the retained host receipt and current source
bytes. This run used PlatformIO Core 6.2.0, ESP-IDF 6.0.1 and the installed
Xtensa/RISC-V GCC 15.2.0+20251204 toolchains; other documented environments were
not separately exercised. Host fixtures and synthetic renders are not physical
radio, touchscreen, SD durability or long-run memory measurements.

## Native resources and package

The native comparison uses the verified local rev118 candidate. Neither that
baseline nor rev119 has been installed on the device.

| Native measurement | Local rev118 | Local rev119 |
| --- | ---: | ---: |
| S3 fixed data and BSS | 173,884 bytes | 173,884 bytes |
| C5 fixed data and BSS | 83,160 bytes | 83,160 bytes |
| Wardriving PSRAM workspace | 22,400 bytes | 22,464 bytes |
| Wardriving status type | 384 bytes | 384 bytes |
| Exact S3 passive state | 56 bytes | 56 bytes |

The existing workspace grows by 64 bytes. Its BLE rows, packet-evidence array
and append-attempt cache do not grow, and there is no new task or allocation.
Unresolved cleanup deliberately retains existing session resources longer.
These type and fixed-section measurements exclude allocator overhead, other
simultaneous buffers and fragmentation; they are not runtime free-heap values.

The selected `capture_ble` entry frame grows from 112 to 208 bytes, and
`wardrive_worker` from 128 to 160. The new scoped bank-copy entry is 192 bytes
and `stop_owned_ble` is 112. The UI entry remains 1,072 bytes. The configured
Wardriving worker allocation remains 6,144 bytes. Inlined/absent cleanup helpers
are not assigned zero cost, and selected entry frames do not establish a full
call-chain bound or runtime stack headroom. The separate native console stack
check passes against the final S3 ELF.

Final S3/C5/bridge application sizes are 1,949,872 / 1,678,960 / 255,296 bytes.
The WROOM and XIAO expansion applications are 926,400 and 930,624 bytes.
The audit is retained at `artifacts/device-rev119/native-memory-comparison.json`
with SHA-256
`9b960a5f1f88a475a5aba744b74ad32f7a7519dbb2575b02ee515ea7f29c3b0d`.
Host and native evidence is retained under `artifacts/rev119-host-evidence`
and `artifacts/rev119-native-evidence`.

The matched package is
`dist/cerberus-unchained-0.1.0-dev.rev119-20261101T174427Z`, with manifest
SHA-256 `5528860342aeb1b2523217e1e2be606fd2e117858676a73e61f16f5bac346a0d`.
All three packaged application hashes match the retained full native matrix.
The read-only verifier passes image hashes, chip headers, partition tables and
image bounds. The local milestone receipt binds committed source, package,
host/native receipts and native audit. No image was flashed or published.
Noninvasive USB inventory confirmed the matching carrier was present; running
firmware and radio state were not queried, so rev113 remains the last verified
installed baseline.

## Coverage and acceptance

The coverage ledger advances to `rev119-source` without promoting a mode:
95 upstream modes remain tracked, with 74 partial, nine needing review, four
hardware-limited and eight pending. Remaining receiver, GPS and persistence
work is recorded in [the remaining-work audit](RECEIVE_REMAINING_GAPS.md).
