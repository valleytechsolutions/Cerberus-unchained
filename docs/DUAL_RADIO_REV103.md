# Rev103: reduce diagnostic stack pressure

Rev103 is installed and addresses the limited console-stack headroom observed
during rev102's read-only checks. All 121 host suites and five native targets
pass on matching unchanged inputs, with verified paired installation and boot.
The observed diagnostic-sequence margin improves from 524 to 1,292 bytes.
This does not complete the full Marauder port; receive, UI, saved-report,
GPS/expansion and earlier active-feature acceptance remain separately tracked.

## Console-owned PSRAM scratch

The console keeps its 4,096-byte task stack and 768-byte formatting capacity.
Its output buffer moves from an automatic array to a single allocation with
`MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT`. Allocation occurs before PHY/driver/task
setup; failure leaves the optional console unavailable without internal-RAM or
stack fallback. Repeated start while active does not allocate again. Only the
console task formats into the buffer; main/UI lifecycle callers are serialized.

Stop requests quiescence and waits for the last command to finish. A timeout
preserves the buffer and driver for retry. Once the task publishes quiescence,
the buffer can be freed or reused by a restart. PHY/driver/task startup failures
release it; cleanup retries cannot free it twice. The SDK USB writer copies
source bytes into its TX ring before returning, so the source does not need DMA
or internal-memory capabilities.

Full newlib formatting remains, including 64-bit values. The existing bounded
termination/truncation behavior is preserved. Archive status now prints the
scalar block, path and filename separately, preserving one snapshot and the
same key order. The former combined maximum could exceed 767 printable bytes;
the separately bounded strings fit without increasing buffer capacity.

The native verifier adds a maximum 128-byte `console_printf` frame, and the
packager requires that exact bound and evidence. Existing task-frame, command
isolation, formatter-call-chain and known-frame-budget gates remain. A small
selected frame is not a whole-program stack bound or runtime acceptance.

## Validation and limits

A new host suite extracts the production writer, formatter, task and lifecycle
functions with mocked peripheral/RTOS boundaries. Its focused run passes 4,248
assertions. It checks allocation/startup
failures, partial writes, formatting boundaries, disconnect and Stop behavior,
repeated starts/stops, timeout ownership and cleanup retry. Archive output uses
the real formatter with maximum path/name fields. All 121 registered host suites
pass on unchanged inputs with Python 3.14.2 and Zig 0.14.1. The receipt and 121
hash-verified logs are retained in `artifacts/rev103-host-evidence`. Nine focused
build/packaging tests also pass; the synthetic native parser rejects a return
to the old 848-byte formatter frame.

The successful native S3 stage measures `console_printf` at 80 bytes, down from
848. Its known console-chain subtotal drops from 2,896 to 2,128 bytes, leaving
1,968 bytes beyond that selected subtotal within the unchanged 4,096-byte stack.
The task/radio/archive-handler frames remain 112/800/880. The new pointer is four
bytes, while linked section padding leaves total S3 data/BSS unchanged at
26,412/146,160. The requested external scratch is 768 bytes plus allocator
overhead. All five native targets pass on the same unchanged inputs as the host
receipt: S3, onboard C5, bridge, WROOM and XIAO. The onboard C5 data/BSS remain
19,552/60,480 bytes. Full matrix logs, ELFs, SDK configuration and frame evidence
are retained in `artifacts/rev103-native-evidence`. This is compiled evidence;
physical expansion and runtime acceptance remain separate gates.

The rev102 baseline reported 524 bytes of remaining console stack in its
read-only sequence, with a 2,896-byte known compiled frame subtotal. Rev103's
comparable sequence reports 1,292 remaining bytes, matching the 768-byte
compiled reduction. Neither measurement is a worst-case bound; the prior
boot-to-boot variation was not explained. USB SD handover and restart under
load still require physical acceptance.

No scan, transmitter, radio owner, archive schema or C5 protocol change is part
of this checkpoint. C5 firmware protocol remains 64. Full upstream mode and
option parity remains incomplete in the coverage ledger.

## Matched package

Package `dist/cerberus-unchained-0.1.0-dev.rev103-20261101T083451Z` passes image
hash, chip-header, partition and bound verification. Firmware source SHA-256 is
`9054482a36aa78cfa80d9254230282889e2c4061d9cc35e8cacde513cc4cdebc`.
The packaged application hashes match the final full-matrix binaries. The
retained native console comparison binds the 80-byte formatter measurement to
that same S3 ELF, not a preliminary or different build. No public release or PR
is selected by these packaging checks.

## Paired installation and runtime observation

Implementation commit `1ab784db22783fa17c888b8ad41bff0b48a5bcb9` is installed.
Fresh first-2-MiB S3/C5 backups match the installed rev102 images and verify
against flash. They cover every erase-rounded update write; higher flash is
preserved. Exact board/ROM/stub identity guards remain in place. All nine
bridge/C5/S3 image writes verify. The five identity, nine receipt/stage and
three offline helper/proof tests pass. Evidence is under
`artifacts/device-rev103`, including the installation receipt and bound backups,
write receipts, native comparison, preflash proof and boot/settled/final reads.
`runtime-console-comparison.json` binds the before/after readings by file hash.

The boot reports S3 `0.1.0-dev.rev103`, C5 protocol firmware 64, active display
buffering, mounted SD and inactive USB storage. No previous-reset fault is
reported. Settled S3 internal free memory remains 134,396 bytes with a 94,208-byte
largest block and ten tasks. PSRAM free is 1,427,552 bytes, 772 below the rev102
baseline, consistent with the 768-byte scratch request plus allocation overhead.
C5 reports 58 KiB free and a 54 KiB minimum. The console low-water observation is
1,292 bytes and main-task low water is 5,432 bytes; these are exercised-sequence
readings, not full load bounds. Radio owners and the archive worker are idle.
The final idle check at about 77 seconds retains the same 1,292-byte console
margin and free-memory readings as the settled check.

Startup SPI records four initial READY timeouts and four legacy fallbacks,
unchanged from the preceding startup, with zero mutex timeouts or driver errors.
The existing 26,240-byte crash record is preserved and predates the update.
Real USB handover/restart, physical report save/reopen, active receive workflows
and sustained operation remain pending. No PR or release was published.

## Next receive milestone

The remaining presentation gap is one ordinary BLE discovery list with
ALL / S3 / C5 source filtering, reusing the existing bounded inventories and
typed selection/save/Fox routes. The same typed address heard by both receivers
must remain two source-labelled observations. S3 peak/exact-age and C5
latest/upper-bound evidence must remain distinct, with frozen displayed rows
and no extra scan/transport work while filtering. Existing C5 Fox handoff needs
physical acceptance; C5 scan-response collection needs a separate active-scan
and correlation design. These are future scope, not completed rev103 features.
