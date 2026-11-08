# Rev127: Channel Survey session ownership

This checkpoint passes the complete host and native build matrices, linked
resource inspection and matched local package verification. It is not installed.
The connected device now runs rev126 after verified paired installation and
the owner's targeted acceptance of the Recon controls, label and priority.
The full Marauder port remains unfinished.

## Reproduced failure

The previous Channel Survey UI predicted its next eight-bit generation from a
status read. If C5 restarted between that read and Start, it could accept a
survey with a different generation. S3 then rejected the reply and discarded
its claim, so Back sent no Stop and the C5 survey continued. Reopening the page
refused the still-running survey. The production UI, transport, dispatcher and
native worker reproduce this sequence in the retained host fixture; it is not
a report of an observed device reboot.

Separate API-level cases also demonstrate that a delayed unbound Stop can
cancel a replacement operation and that an eight-bit generation eventually
aliases. Those require an additional caller or repeated operations; they are
not presented as naturally reachable interleavings of the serialized UI.

## Intended contract

Capability 70 adds a separate, nonwrapping 64-bit survey session sequence. C5
validates the exact next session before starting, accepts an exact session/band
replay without rearming completed work, and validates the identity before any
scoped Stop, renewal or result read. The random boot seed reduces accidental
cross-boot reuse; it is not persistent uniqueness or authentication. Session
and read-pin exhaustion must fail closed rather than wrap.

S3 retains a candidate session before sending Start and retains cleanup while
delivery is ambiguous. Back, hidden-page and sleep cleanup must use that exact
identity. Legacy unbound commands cannot control a scoped held job. Trusted
native radio arbitration remains distinct from external command ownership.

The scoped worker has a renewable ten-second ownership lease so an S3 restart
does not leave an indefinitely abandoned survey. Status may renew a still-live
exact session; result reads do not renew it, and expiry or cancellation cannot
be reversed by a delayed request. Expiry is checked between SDK calls, not a
hard real-time guarantee. A failed SDK disable retains the reservation until a
checked retry establishes quiescence.

All existing metrics remain available. Full session identity plus a pinned
projection requires three bounded 22-byte replies for each existing status or
result object. One small scratch projection replaces no observation bank and
adds no task. Publication requires matching identity, pin and fragment fields;
stale, mixed or incomplete projections must not become displayed results.
Healthy visible polling targets a minimum 250 ms interval, with one scoped
control plus at most one three-part projection per eligible update. The
summary and six visible rows rotate over seven updates, nominally 1.75 seconds
without stalls. Failed transport/validation, admission retries and cleanup
retain a 700 ms retry interval. Stop is processed before optional metrics.
Each adapter call has one 20 ms passive transport budget and no internal
command retry; this is not a hard SDK execution or measured touchscreen-latency
bound. Faster display refresh does not shorten the native 100 ms channel dwell
or establish faster RF sampling. The linked measurements below separate fixed
section growth from symbol payload sizes and runtime behavior.

Changing page or band clears the old display projection before transport, so
a failed read cannot present earlier rows as a new page. Explicit restart of a
terminal job proceeds in one tap only after confirmed quiescence. Routine
asynchronous cleanup says RELEASING C5 RADIO and STOPPING; a real failure retains
its error and retry control. Repeated Stop does not rearm a job.

## Evidence and outstanding validation

The original reproduction is retained in the rev126 checkout under
`artifacts/channel-survey-control-audit-20261101T224546Z`; its 5,875 assertions
include assertions demonstrating the faulty outcome. The evidence manifest
SHA-256 is
`16c47f3bee66a67ae691402e7ff1c67ad79b2ec77b22c6a60c8bf41143c7f951`.
Passing that reproduction alone does not mean the bug is fixed.

At 2026-11-01 22:47:19 UTC the GitHub compare API reported Marauder `develop`
identical to the pinned `79cc6bcba1db6679baee1dab2b0c1ad5010e6363` revision
(zero commits ahead/behind and zero changed files). The response is retained
in `artifacts/upstream/rev127-marauder-develop-check.json`. This is a dated
source check, not a claim that every upstream feature is ported.

Focused validation passes 4,865 composite checks through the actual C5 worker,
dispatcher, S3 framing/validation and production UI, plus 18,827 native scoped
service assertions. All 136 host suites and all five native targets pass on
the same unchanged source/configuration inputs. Linked resource inspection
passes; physical survey acceptance remains pending.
No device, network transmission, publication or pull-request operation is part
of the offline reproduction or implementation.

The first focused fixture used four visible rows, although production displays
six. Its preliminary evidence is retained separately under
`artifacts/rev127-survey-four-row-preliminary`. The corrected runner extracts
the production row count and covers the fifth/sixth rows, final partial page,
complete seven-object rotation and polling/backoff boundaries. Its final rerun
passes, with four inspected six-row renderer previews. The final focused log is
`artifacts/build/rev127-channel-survey-six-row-final.log`, SHA-256
`296a0616debef0c35f59f7cee83f217fbff0f009a140bf69fc2b6d97a62c2157`. The first
native compilation also resolves the fresh worktree's dependencies and
overlaps these final source/fixture corrections. It is not the final
unchanged-input validation receipt. All five targets compiled successfully in
that first pass; only the expected UI, lifecycle runner and fixture changed.
The logs and report remain in `artifacts/rev127-first-native`.

## Final build and resource evidence

The final host run completed at 2026-11-02 00:00:48 UTC. The subsequent native
matrix passes S3, onboard C5, temporary bridge, WROOM expansion and XIAO
expansion, with no input drift. The retained host receipt SHA-256 is
`b1ee7f4e9f81de9030fe3c8ce678fb7d464d782b9fab0d4b64537b4bea62eb8d`;
the retained native report is
`a5a82b069bbf0828fcfe270ea43872f4e01b9ea89efcc20d02ca9d8e8ded7076`.

Against the retained rev126 ELFs, fixed S3 data/BSS increases by 16 bytes and
fixed C5 data/BSS by 88 bytes. Survey-owned linked scalar/scratch payloads grow
by 13 and 73 bytes respectively; section totals include alignment. Existing
observation banks, compared model sizes and configured task stacks are
unchanged. No new task or observation-bank allocation is introduced.
The resource report SHA-256 is
`caf0a7788ff967d6344de5319b26125b73c58fe518b33dc498f26831da6403cb`.
These are linked-image measurements, not runtime heap, worst-case stack,
loaded latency or physical RF acceptance.

The first resource-helper attempt expected an unused S3 request typedef in
debug information. That typedef has no linked debug entry because S3 transmits
the scalar directly. The corrected helper explicitly records that absence;
it does not call it a zero-size measurement. The failed helper output is
retained separately and the successful audit binds the same final ELFs.

The final receipts bind 1,122 unchanged inputs. Local source commit
`3396ab623df8a1ab3d8f4b4713030d9fa1338174` produced matched package
`cerberus-unchained-0.1.0-dev.rev127-20261102T000402Z`. Its manifest SHA-256 is
`05fe827347deda50151c333f29a5159d4cf50c7630884d34493aa370dfd7bb4f`.
The packaged S3, onboard C5 and temporary bridge applications match the retained
native matrix hashes. Package verification does not select a public release,
flash the device or establish physical survey acceptance; rev126 remains
installed. The next agreed major milestone is the passive Probe Sniff workflow,
followed by an update to existing Unchained PR #1 and a requested pause.
