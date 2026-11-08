# Rev112: independent dual-receiver Bluetooth Fox Hunt

The **rev112 / C5 firmware67 update is installed**, with image writes, paired
boot and mounted SD verified. Rev111 was the preceding installed baseline.
This checkpoint addresses the source audit in
[BLE_DUAL_FOX_AUDIT.md](BLE_DUAL_FOX_AUDIT.md), using the existing receive-side
exact-address-and-type Fox Hunt. The wider Marauder port remains incomplete;
build/package checks are separate from physical acceptance.

After the shared discovery drain, the S3 tracker attaches and starts
independently of the C5 TRACK request. A failed S3 attachment or RF start no
longer suppresses the available C5. RETRY S3 and RETRY C5 target only the failed
receiver; the healthy source keeps its target and session. Reentry while Fox
already owns or drains work is rejected instead of overwriting its target and
losing permission to cancel the old scan.

The screen has independent S3 and C5 status, RSSI meters, ages and counts. S3
uses its local tracker age, while C5 uses a conservative upper bound. A large
C5 bound says RECENCY UNCONFIRMED, rather than proving that a sample is stale.
Unknown timing is explicit. A sample retained across an automatic C5 job
restart says LAST JOB / RETAINED READING; it is not promoted to current-job
evidence because the next job is listening. Pending target changes and cleanup
suppress the previous target's samples on both panels.

S3 reports include original and response callbacks and reset with its host.
C5 counts matching complete original callbacks for the reported job, including
typed RSSI observations whose AD container may be malformed; partial reports
and scan responses are excluded. Counts are never summed across receivers or
jobs. An unavailable S3 tracker is distinct from an attached tracker with a
measured zero count. These are callback counts, not physical device counts.

STOP BOTH RECEIVERS, the STOP AND BACK footer, and sleep use the same
cancellation path. It cancels a pending C5 request even before INFO acquires a
job and preserves the coordinator's full-width clock. Cleanup continues after
the 12-second warning until the owned radios release. Failed or uncertain
cancellation retains the barrier; a deadline does not establish release.
An S3 retry that times out keeps draining its failed sweep, but a late release
requires explicit retry rather than automatically restarting. Generation and
host-epoch changes revoke permission to stop a replacement S3 scanner. A
repeated startup timeout repaints after retry. The existing generic console
Fox error exposes shared startup/cleanup timeouts; it does not add fields or
change the receiver-specific diagnostic meanings.

The implementation reuses the existing tracker, cached C5 coordinator and
passive tracking mode. It requires no wire protocol change, new radio mode,
task or history bank. C5 capability firmware remains 67. Rendering must not
perform transport operations. Two nearby receivers do not establish distance,
bearing or triangulation.

All 124 host suites passed with unchanged inputs. The focused production
extraction checks cover independent attach/start failures and retry,
foreign-owner protection, target reentry, shared-barrier timeout, repeated
Back, sleep, pre-INFO cancellation, the full clock beyond the 32-bit rollover,
and cleanup held past its deadline followed by release. The native-live UI
fixture reports 1,251 checks. Presenter fixtures cover both/single/no-sample
views, typed-target mismatch, unavailable versus zero counts, C5 job rollover,
repeated cached reads, unknown/saturated ages and counts, privacy and display
failure propagation. The generated 320 by 480 previews were inspected for fit.
Independent source and fixture review found no remaining blocker.

All five native targets also passed with unchanged inputs matching the host
run: S3 application, onboard C5, temporary bridge, expansion WROOM and expansion
XIAO. Retained evidence is under `artifacts/rev112-host-evidence` and
`artifacts/rev112-native-evidence`. The earlier successful pass is retained
separately as `rev112-first-pass-*`; it predates the timeout repaint and generic
diagnostic follow-ups and is not the final package evidence.

The offline resource audit compares the final native images with the retained
installed-rev111 images and installation receipt:

| Native resource | Rev111 | Rev112 | Change |
| --- | ---: | ---: | ---: |
| S3 fixed data plus BSS | 173,596 B | 173,612 B | +16 B |
| C5 fixed data plus BSS | 83,160 B | 83,160 B | 0 B |
| Optional archive viewer plus worker types | 118,760 B | 118,760 B | 0 B |
| Those types plus retained dual discovery stores | 161,696 B | 161,696 B | 0 B |
| Native UI entry frame | 1,136 B | 1,136 B | 0 B |

The new temporary Fox cached-view type is 60 bytes and the linked Fox draw
entry frame is 304 bytes. There was no separately linked draw symbol in the
baseline, so its absence is not treated as zero cost. The Fox tick entry frame
remains 48 bytes. Main and NimBLE task allocations are unchanged. Selected
entry frames are not whole-call-chain bounds or runtime stack headroom;
optional type sums exclude allocator overhead and fragmentation. The audit
adds no radio operations and is retained in
`artifacts/device-rev112/native-memory-comparison.json`.

The matched package is
`dist/cerberus-unchained-0.1.0-dev.rev112-20261101T133750Z`.
Its hashes, chip headers, partitions and image bounds pass verification, and
its three application images match the retained final native matrix:

| Image | Application bytes | SHA-256 |
| --- | ---: | --- |
| S3 | 1,934,240 | `f8043012cf27230f7fbbc490a3572b70404adcc891b536f596839057f713933f` |
| C5 | 1,678,720 | `4fb73f0ba01782abe13e46f41df214f6bc0375c282caead70898e5cd3c03021a` |
| Bridge | 255,296 | `1a2f5b2aa7d097de535fb5bdddc46a8361013aaba42b84c242e2a872f29a48d1` |

The source fingerprint is
`790ebb410c77f78f0a97a5bcfb165a0e3b913d010271dd23f98c0b40da6b17fe`;
the manifest digest is
`ad62209702098a02b1fcc23bd95c3c61826dca67745aa3b539910d4b380cdb90`.
`artifacts/device-rev112/local-milestone.json` binds the clean source commit,
package, final host/native receipts and resource audit, with `installed:false`.

Physical movement, single-receiver reception, repeated transitions, error
recovery and loaded memory still require device acceptance. No RF speed,
coverage improvement or full-feature validation follows from these checks.
Installation was performed afterward, as recorded below. No PR, push or release
was performed for this revision.
The coverage ledger retains the same 95 mode-status assignments and six
workstream statuses; it is not a completion percentage.

## Verified installation

The exact installed source commit is
`b0d6b60460f1b24e3897b3d914f61e53b6c0f7f0`. Six identity-guard tests and sixteen
installation-helper tests passed against the final helper bytes. Fresh
pre-write diagnostics at 13:46:45.560 UTC on 2026-11-01 confirmed rev111/firmware67,
idle radio owners, mounted SD, USB storage off and no previous reset fault.
Baseline and candidate both use capability firmware67; the package and image
hashes distinguish the actual C5 versions.

Fresh first-two-MiB backups match the installed rev111 bootloader, partition
table and application and passed full device verification. They cover every
erase-rounded update region. Their SHA-256 values are:

- S3: `60254f98ac3d46c285214a6e2806662be52c2ce6b40805f9f4cefed20e8b6e22`
- C5: `7c18c49e49d0fa1d9b386a3d5c03f60616be6e4bfcdca492e76850c80aef1552`

All nine written images across the bridge, C5 and final S3 stages were verified.
USB and ROM/stub identities were checked before writes. Higher flash regions
and the prior stored crash record were retained. No recovery was used.

Post-installation captures at 13:55:45.769 and 13:56:56.129 UTC are 70.360059
seconds apart. Both report rev112/firmware67, protocol2 online, mounted SD and
idle owners, with `previous_reset_fault=no`. S3 uptime advances from 40,051 to
110,410 ms; C5 uptime advances from 39,178 to 109,539 ms with boot count 1 stable.
SPI exchanges advance from 166 to 481. The four initial-READY timeouts and four
legacy fallbacks remain unchanged; mutex/fall/high timeouts and driver errors
remain zero.

| Sampled idle measurement | Before update, rev111 | Settled rev112 |
| --- | ---: | ---: |
| S3 internal free bytes | 133,372 | 133,356 |
| Largest S3 internal block | 90,112 | 90,112 |
| S3 PSRAM free bytes | 1,427,552 | 1,427,552 |
| C5 snapshot free bytes | 56,948 | 56,948 |
| S3 task count | 10 | 10 |
| Observed console stack low-water bytes | 1,292 | 1,372 |

These are sampled idle observations, not loaded memory or worst-case stack
claims. No Fox Hunt receiver test, touchscreen transition, RF performance test
or real-card save/reopen acceptance was performed during installation.

`artifacts/device-rev112/installation-receipt.json` binds the backups, all
writes, preflight, captures and the independently recomputed idle comparison.
Its SHA-256 is
`528d559a691696fe9a8a7a996d62dab06aae1f7144ce9b8e34184f37c18c8007`.
The earlier local milestone retains `installed:false`; this later receipt
establishes the installed state. Post-installation coverage wording is recorded
separately in `installation-editorial-verification.json`, leaving original
host/native receipts immutable and all feature statuses unchanged.
The follow-up `installation-editorial-links-verification.json` records three
current-checkpoint link corrections and binds the first editorial receipt.

The [next receive-side transfer audit](BLE_RESPONSE_TRANSFER_AUDIT.md) proposes
reducing C5-to-S3 result reads for rows without a stored scan response, while
preserving the existing evidence and compatibility checks. It is a proposal,
not an implemented part of rev112 or a measured performance improvement.
