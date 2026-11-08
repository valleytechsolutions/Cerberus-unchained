# Rev116: independent BLE Analyzer receivers and exact S3 ownership

Rev116 is a local receive-side Analyzer candidate following
[rev115's ordinary discovery changes](DUAL_RADIO_REV115.md). Rev113/C5 firmware
68 remains installed. Full Marauder parity and physical acceptance remain
unfinished. The coverage ledger is still the explicitly labelled `rev113-source`
snapshot; this milestone does not promote its parity statuses.

## Receiver lifecycle

Analyzer requests both receivers after the previous workflow releases its
owners. Subsequent S3 allocation, start or runtime failures no longer cancel a
healthy C5 Analyzer. RETRY S3 and RETRY C5 operate on the failed source without
restarting the healthy source's job or history. A failed initial transition
offers RETRY START. Displaying a page does not authorize a retry.

The S3 scanner now exposes Analyzer-specific passive start, state, stop and
release operations. They bind to the accepted scanner callback epoch and BLE
host epoch under the existing scanner mutex. They do not infer ownership from
the generic SCANNER label or the public eight-bit generation. Generic scan
starts and stops cannot take over an attached Analyzer, and Analyzer callbacks
must match its accepted sweep before contributing to its history. Release
refuses to discard a still-owned sweep or queued cleanup.

Stop, Back, sleep and hidden-page cleanup clear intent, including C5 work
waiting for INFO. A 12-second cleanup warning does not abandon ownership or
allow either receiver to restart. Cleanup cannot stop a replacement workflow
merely because it also uses the scanner. An explicit new Analyzer entry first
releases any retained prior Analyzer attachment.

The BOTH RECEIVERS view retains separate S3 completed 200 ms callback buckets
and C5 intervals measured on C5's clock. Their counts are neither summed nor
treated as unique devices. A source with no valid view shows unavailable data,
not a fabricated zero measurement. Queued S3 startup, actual receiving and
between-sweep states are distinct. S3 DETAILS keeps the existing family and
source evidence when available. Diagnostics use Analyzer's own state rather
than a generic scanner's cached status.

No new radio mode, C5 wire command, task or discovery bank is introduced.
C5 capability revision remains 68. The scanner's additional binding and UI
intent are measured in the final native audit; physical performance is not
inferred from their type sizes.

## Host validation

All 124 host suites pass on unchanged inputs. The retained receipt binds every
log by hash. The actual scanner tests add 84 ownership assertions in each of the
legacy and extended receive configurations, with separate canonical-output
checks for BLE-disabled builds. Existing callback, parser, finite-sweep,
generation-wrap, discovery, Wardriving and MAC-monitor checks also pass.

Scanner cases cover queued Start cancellation, SDK startup failure, repeated
passive sweeps, stale callbacks, cancellation held beyond 12 seconds, failed
host-release retry, token exhaustion and host resets. A replacement scan with
the same public generation cannot contribute callbacks or be stopped by the old
Analyzer binding. A foreign advertiser remains untouched as well.

The UI fixture passes 550 Analyzer assertions, including 485 lifecycle checks.
It extracts the actual menu entry, lifecycle, controls, Back route and renderer.
Composite tests run the production C5 UI adapter and coordinator with a framed
peer simulation. The S3 boundary models the exact attachment contract; the
scanner suite separately exercises its actual mutex and SDK-event code.

These cases exercise either source failing while the healthy source keeps its
job/history, repeated retry failures, held re-entry, shared-barrier timeouts,
Stop/Back/sleep before INFO, hidden-page cleanup, exact owned-sweep restart and
C5 rollover. Tests use a local clock above `UINT32_MAX`, preserve source clock
denominators and distinguish gaps, saturation and unknown data from zero.
Ordinary discovery's 1,888 live checks also pass.

The diagnostics fixture compiles the actual publisher and source-state helpers.
It verifies foreign-scanner exclusion, source-error retention, cleanup/barrier
precedence and independent C5 failure. The snapshot remains 120 bytes and the
maximum tested console chunk is 415 bytes. Synthetic screen previews were
reviewed for independent source errors, unavailable data, queued startup,
rollover and cleanup timeout. These are rendered fixtures, not device photos.

## Native, package and physical evidence

All five native targets pass on unchanged inputs: S3 application, onboard C5,
temporary S3/C5 USB bridge, expansion WROOM and expansion XIAO. Their input map
exactly matches the retained host receipt. The native comparison uses the
verified local rev115 candidate as its baseline; it does not claim that rev115
was installed.

| Native measurement | Local rev115 | Local rev116 |
| --- | ---: | ---: |
| S3 fixed data and BSS | 173,580 bytes | 173,628 bytes |
| C5 fixed data and BSS | 83,160 bytes | 83,160 bytes |
| S3 Analyzer history type | 288 bytes | 288 bytes |
| C5 Analyzer history type | 424 bytes | 424 bytes |
| Shared discovery store | 21,448 bytes | 21,448 bytes |
| Dual-BLE coordinator | 21,496 bytes | 21,496 bytes |
| Exact S3 Analyzer state | Absent | 28 bytes |
| Archive viewer plus worker type demand | 118,760 bytes | 118,760 bytes |

The combined linked S3 fixed-memory increase is 48 bytes. Existing optional
history, discovery and archive allocations do not grow. These figures describe
native types and linked sections, not measured runtime heap or allocator
overhead. The new exact state has no rev115 counterpart; absence is not treated
as a zero-byte implementation.

The selected Analyzer tick entry frame decreases from 352 to 80 bytes. The UI
entry frame remains 1,072 bytes, the dual Analyzer renderer remains 864 bytes,
`handle_ble_tap` remains 192 bytes and `handle_tap` remains 544 bytes. Inlined or
absent helpers are not assigned zero cost. These selected entries are not a
complete call-chain bound and do not establish runtime stack headroom.

Final S3/C5/bridge application sizes are 1,941,888 / 1,678,960 / 255,296 bytes.
The matched package is
`dist/cerberus-unchained-0.1.0-dev.rev116-20261101T161203Z`.
Its manifest SHA-256 is
`0ed30d5d1192cd3732dd29c6fd6954f810fde7c8dd6ead444d2c57123d329b15`.
All three application hashes match the retained full matrix. Image hashes,
chip headers, partition tables and image bounds pass the read-only verifier.

Evidence is retained under `artifacts/rev116-host-evidence`,
`artifacts/rev116-native-evidence` and `artifacts/device-rev116`.
The native audit SHA-256 is
`a664733fb8ea414d35a12b19f22fc85b8847630a3f26d5ea122cac88064c8c34`.
The local milestone receipt binds the committed source, matched package,
host/native receipts and audit. No image was flashed and no release or pull
request was published. Rev113 remains the installed baseline.

Physical concurrent reception, retry/Stop/Back, dense environments and sustained
heap/stack behavior remain unverified. The upstream Analyzer's configured graph
interval and receive options are not claimed to be identical to Cerberus's
separate source clocks. GPS and the six-radio expansion backboard are separate
acceptance work.
