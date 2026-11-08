# Rev111: single C5 response dispatch per receive callback

The matched **rev111 / C5 firmware67 update is installed**, with all image
writes verified, paired application boot confirmed and SD mounted. Physical
response acquisition, touchscreen behavior and saved-report acceptance remain
pending. Boot verification does not promote any feature to full acceptance.

Rev111 follows [rev110](DUAL_RADIO_REV110.md), which implemented optional C5
response acquisition, source-separated live analysis and v6 SD evidence. This
checkpoint removes a redundant mode check from complete passive EXT callbacks
and corrects older coverage descriptions that no longer matched that code.

The C5 EXT callback dispatches the response-aware handler before response or
partial-report early returns. The legacy callback performs that dispatch in
its own wrapper after constructing legacy metadata. The common original
copier no longer repeats it. Each path checks once, preserving the newer-
original invalidation boundary and existing session/context checks.

This removes one unnecessary helper call and mutex take/give from each
complete passive EXT callback. RF intervals, windows, PHY choices, response
matching, task count and wire protocol are unchanged. C5 capability firmware
remains 67; application version is `0.1.0-dev.rev111`. No measured radio speedup
is claimed.

All 124 host suites and five native targets passed with unchanged, matching
inputs. The actual callback fixtures verify one response-mode predicate call
on each legacy/EXT route across passive Discovery and response Discovery/Family.
The ordinary owner-free complete EXT path takes three mutex round trips,
compared with four in the previous code. The new regression fails against the
previous actual callback as expected. Partial, malformed, null-data and stale
callback cases remain covered; this is not a source-text-only assertion.

Retained evidence is under `artifacts/rev111-host-evidence` and
`artifacts/rev111-native-evidence`. The native audit compares against installed
rev109; `artifacts/device-rev111/rev110-native-comparison.json` also compares
the immediately preceding local rev110 images. Native types, fixed S3/C5 data
and BSS, and the optional allocation sizes are identical to rev110. Selected
C5 entry frames remain 64 bytes for GAP, 320 for the common original copier,
448 for response processing and 240 for snapshot commands. The unnecessary
nested call is removed, but entry frames do not prove whole-chain or loaded
runtime stack headroom.

The matched package is
`dist/cerberus-unchained-0.1.0-dev.rev111-20261101T125257Z`. Its hashes, chip
headers, partitions and image bounds pass verification. The local milestone
receipt binds its application images to the native matrix and exact source
commit before installation.

Rev109/C5 firmware66 is the verified installed baseline at the start of this
checkpoint. Rev110 was a verified local package, not an installed rollback
baseline. Fresh backups must match rev109 before a new write; installation
and subsequent boot evidence are recorded separately from local packaging.

The wider port remains incomplete. Physical response delivery, loaded memory,
repeated UI transitions, saved-file acceptance, GPS and six-radio expansion
remain separate from builds and source review. No PR or release is published
as part of this checkpoint.

## Verified installation

The exact source commit is `df937a97bdc7e6da926a73440ef1f973b6ba92db`.
The package/source digest is
`d909b439aac47e5a54865175444b2317ab0c1b6d603d18739ed5fdf1b322eae3`;
the manifest digest is
`d8e51c178afa4bbc54ed373e1f71bbb6d99414b431960eeaf953ce769f1fcd8b`.
Six identity-guard tests and twelve installation-helper tests passed after
binding this exact source HEAD. Fresh pre-write diagnostics confirmed
rev109/firmware66 idle, mounted SD, USB MSC off and no previous reset fault.

Fresh first-two-MiB backups from both identified chips match the installed
rev109 bootloader, partition table and application bytes and passed device
verification. Their digests are:

- S3: `488f018b8c09c0cb3cd05c181f9249eefcb4c11260cc64d3d532d67061366d81`
- C5: `7b87f3f7a372d47c1c8667231837ee84d8defc23b38730fe692228bb05515538`

Every erase-rounded write fits the verified backup coverage. The bridge,
onboard C5 and final S3 stages each verified all three programmed image hashes.
Exact USB/ROM/stub identities were checked before writes. No recovery was used.
Higher flash regions and the prior stored crash record were retained.

Post-installation captures completed at 13:03:12.825 and 13:04:01.620 UTC on
2026-11-01, 48.795 seconds apart. Both report rev111/firmware67, protocol 2 online,
buffered display, mounted SD, all radio owners idle and `previous_reset_fault=no`.
S3 uptime advances from 34,641 to 83,438 ms and C5 uptime from 33,770 to 82,564 ms;
C5 boot count remains 1. SPI exchanges advance from 143 to 367. Four initial-READY timeouts and four
legacy fallbacks remain unchanged across these captures; mutex/fall/high
timeouts and driver errors remain zero. This does not establish RF performance.

| Sampled idle measurement | Before update, rev109 | Settled rev111 |
| --- | ---: | ---: |
| S3 internal free bytes | 134,140 | 133,372 |
| Largest S3 internal block | 90,112 | 90,112 |
| S3 PSRAM free bytes | 1,427,552 | 1,427,552 |
| C5 snapshot free bytes | 56,964 | 56,948 |
| S3 task count | 10 | 10 |
| Observed console stack low-water bytes | 1,292 | 1,324 |

These are sampled idle observations, not peak-load or worst-case stack claims.
The C5 response context and the optional SD viewer/worker were not exercised.
Resource demand under a live response scan still needs physical validation.

`artifacts/device-rev111/installation-receipt.json` binds the writes, backups,
preflight and both diagnostic captures. Its SHA-256 is
`70b792304ceac1082a84cafba137aa8a94209e24109f6372e7eaa1b2ff2ebae7`.
`idle-comparison.json` records the sampled stability checks. The local package
receipt deliberately keeps its earlier `installed:false` state; the separate
installation receipt establishes the later installed state.

Post-installation documentation and coverage wording are recorded separately
in `installation-editorial-verification.json`. The original build receipts
remain immutable, firmware source bytes are unchanged, and no mode or
workstream status is promoted by paired boot alone.

The next planned receive-side work is the
[dual-receiver Fox Hunt usability and cleanup milestone](BLE_DUAL_FOX_AUDIT.md).
That source audit identifies independent-source state/retry and retained cleanup
improvements; Rev111 does not implement them or change Fox Hunt's RF behavior.
