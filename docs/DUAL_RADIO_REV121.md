# Rev121: C5 receive cleanup and Channel Survey handoffs

Rev121 implements the receive-side lifecycle corrections found after the local
[rev120 shared-host checkpoint](DUAL_RADIO_REV120.md). It pairs S3 application
`0.1.0-dev.rev121` with C5 capability revision 69. Rev113/C5 revision 68 remains
the last verified installed baseline. Full Marauder parity remains unfinished.

## Failure cases and implementation scope

The rev120 source audit reproduced stale queued Stop/timeout work publishing
ERROR into an accepted replacement scan before that scan started. A separate
timer-arm failure canceled SDK discovery successfully but retained ownership
in passive companion modes, preventing Release. Legacy cancellation failures
could instead hide still-active discovery behind public ERROR, allowing Wi-Fi
admission. Channel Survey's pending/running and retained cleanup reservation
was also missing from BLE admission; the UI did not retry cleanup after ERROR.

The correction binds queued/dispatching C5 control work to private session
identity and separates pending or accepted SDK ownership from public scan
results. A timer event is a wake requiring the current accepted sweep and its
full monotonic deadline. Confirmed cancellation clears accepted SDK ownership
uniformly across receive modes, while queued or dispatching controls retain
their reservation until they drain. Uncertain cancellation retains both
cleanup responsibility and exclusion of competing radio operations. Companion
Release, orphan expiry and response context lifetime use the same quiescence
predicate. Callback tokens are never reused within a boot; exhaustion rejects
another receive scan.

Channel Survey remains visible to competing BLE admission while pending,
running or retaining failed cleanup. Checked preemption is the background
handoff path; a failed disable is not treated as release. The S3 UI retains
cleanup intent and retries Stop after errors, including navigation and sleep.
Its STOP, RETRY STOP and STOPPING labels reflect the retained cleanup state.
A repeated tap during cleanup retries Stop; restarting requires a later
explicit request after release.
Its existing generation-byte/band and unbound Stop protocol provide limited
fencing only; full reboot/wrap/interleaved-command ABA protection is outside
this wire-preserving change.

The current paired Survey workflow requires C5 firmware 69. Firmware 46 remains
the minimum for the original wire command layout, but it does not promise the
checked cleanup acknowledgement required by this UI. Unsupported peers show
`C5 FIRMWARE 69 REQUIRED`; fresh workflow entry performs no Survey transport.
The native Stop acknowledgement becomes IDLE only after the survey worker and
retained SDK reservation are quiescent.

Existing radio parameters, receive modes, result banks, tasks and wire layouts
are preserved. This milestone does not add or optimize disruptive payloads,
rates or targets. It addresses the receive workflows' resource handoffs.

## Validation

Implementation, focused production-bound regressions and independent source
review are complete. The C5 receive lifecycle fixture passes 2,076 assertions
in each legacy and extended callback configuration, and the Channel Survey
fixture passes 6,568 checks across the actual worker, admission, SPI transport
and UI cleanup paths. These include queued/dispatching controls through reset
and completion, stale timeout wakes, cancellation failures, response-context
retention, uncertain Survey Start replies, observed replacement, held cleanup,
Back/sleep and firmware-gated entry. Existing Watch and dual-BLE focused
regressions also pass.

All 128 registered host suites pass on 1,083 unchanged final inputs. The five
native builds also pass and use exactly the same input map: main S3, onboard
C5, temporary S3/C5 bridge, expansion WROOM and expansion XIAO.
The matrix now includes the previously supplemental Radio Lab and advertiser
lifecycle suites, as well as the new C5 receive and Channel Survey checks.
The initial passed 128-suite run is retained separately as review evidence;
the authoritative run follows the final coverage-text correction. Host tests
used Python 3.14.2 and Zig 0.14.1. Native builds used PlatformIO Core 6.2.0,
ESP-IDF 6.0.1 and Xtensa/RISC-V GCC 15.2.0+20251204. Other documented
environments were not separately exercised.

The initial audit assertions reproduced faulty behavior; they are not a
correctness gate for this candidate. SDK/RTOS fault fixtures and native builds
do not establish physical scheduling, touchscreen operation, radio performance,
SD durability or sustained memory headroom.

## Native resources and local package

The native comparison uses the independently verified local rev120 candidate.
Neither rev120 nor rev121 has been installed.

| Native measurement | Local rev120 | Local rev121 |
| --- | ---: | ---: |
| S3 fixed data and BSS | 173,996 bytes | 173,996 bytes |
| C5 fixed data and BSS | 83,160 bytes | 83,184 bytes |
| Survey status type, either chip | 22 bytes | 22 bytes |
| Survey result type, either chip | 22 bytes | 22 bytes |
| Shared S3 BLE host lease | 16 bytes | 16 bytes |
| Optional C5 response context | 11,736 bytes | 11,736 bytes |

The new S3 Survey scalar symbols occupy nine bytes despite unchanged total
fixed sections. C5 adds 21 bytes of named lifecycle state and removes the old
one-byte exhaustion flag; the measured net fixed-section increase is 24 bytes.
Symbol sizes and section totals are different measurements. Existing result
banks, optional stores and workspace sizes remain unchanged; no new task or
allocation was added. These measurements do not establish free heap,
fragmentation or sustained runtime headroom.

Selected C5 entry frames grow from 64 to 96 bytes for Start, 16 to 32 for
Stop, and 16 to 48 for timeout. The new cancellation/control-completion helpers
have 32/16-byte entry frames. S3 Survey Start grows from 48 to 64 bytes and
refresh from 32 to 64; Stop remains 64, while new cleanup/begin helpers have
32-byte frames. The C5 Survey worker remains 96 bytes and S3 UI entry remains
1,072. These are selected linked entry frames, not full call-chain bounds or
runtime stack headroom. Inlined helpers are not assigned zero cost. The
separate native console-stack check passes against the final S3 ELF.

Final S3/C5/bridge application sizes are 1,953,536 / 1,679,920 / 255,296 bytes.
The S3/C5 applications grow by 608/960 bytes relative to rev120; expansion
WROOM/XIAO applications remain 926,400/930,624 bytes. The native audit is
retained at `artifacts/device-rev121/native-memory-comparison.json`, SHA-256
`b74ae18b6bf180b64c06cd48a4006169f364f4e209e846a3ab59bfacc9e2e045`.
An independent review rehashed both revisions' native evidence and all 128
final host logs, and found no audit blocker. Final matrices are retained under
`artifacts/rev121-host-evidence` and `artifacts/rev121-native-evidence`.

The matched package is
`dist/cerberus-unchained-0.1.0-dev.rev121-20261101T185837Z`, with manifest
SHA-256 `ce1d7e18cec5ed7bc2711b46323b109bccbf7980a28d5b6c5591d09c26dcb62d`.
Its three application hashes match the retained five-target matrix. The
read-only verifier passes image hashes, chip headers, partition tables and
image bounds. The local milestone receipt binds committed source, the package,
host/native matrices and native audit. No image was flashed or published;
rev113/C5 revision 68 remains the last verified installed baseline.

## Coverage and acceptance

The coverage ledger advances to `rev121-source` without promoting a mode.
It continues to track 95 upstream modes and six workstreams: 74 partial modes,
nine needing review, four hardware-limited and eight pending. Existing source
notes were corrected to reflect rev119 Wardriving binding rather than retaining
its older generation-byte gap. Remaining work is tracked in
[the receive audit](RECEIVE_REMAINING_GAPS.md).
