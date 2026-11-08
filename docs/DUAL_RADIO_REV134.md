# Rev134: Packet Count progress and diagnostics

Rev133 was the installed baseline for this update. The owner reported that Packet Count showed
S3 NOT ASSIGNED and appeared inactive after Start. The single-channel plan
intentionally assigns reception to C5 and control, analysis and presentation
to S3. That role label was misleading, and reception has not been established
by the owner's report or the available rev133 console fields.

The owner subsequently confirmed that C5 shows RECEIVING and its RX seconds
increase while the selected AP's TOTAL, TX and RX stay zero. This establishes
progressing receiver-window publications rather than the reproduced setup
stall. It does not establish delivery of matching frames. The new diagnostics
separate callback totals, metadata rejections and selected-address matches so
that this report can be investigated without inferring reception from a timer.

## Reproduced scheduling defect

The controller checks status every second. If UI ticks are at least one second
apart and each successful exchange consumes the 8 ms tick budget, every tick
can perform only STATUS. This renews an accepted staging session without
uploading targets or starting reception. The same priority can prevent live
counter reads. A regression using 1.2 s tick spacing and 9 ms exchanges fails
against the prior implementation because START is never reached.

Rev134 gives each validated, exact-session STATUS one phase-progress credit.
While that proof is younger than half the native 10 s lease, one target upload,
Start or readout operation can precede another due status check. The credit is
consumed even if the operation fails. Stop, pending Release and missing control
proof retain priority. This does not add a second blocking operation after the
tick budget has expired, promise progress at arbitrary delays, or bypass
session and lease validation. No C5 protocol or native receiver change is
required; capability 74 remains current.

The host reproduction identifies a real failure path, not yet the confirmed
cause of the physical report. Rev133 diagnostics showed a C5 reservation but
could not distinguish staging, queued setup and actual reception.

## Screen and read-only diagnostics

For a C5-only plan, the S3 panel says ANALYSIS and CONTROL / READOUT. The C5
panel distinguishes checking, reservation, target upload, starting, queued
work and receiving. Readout reports copied parts; RX duration stays unknown
until a validated publication exists. A quiet channel with validated zero
counts is distinct from a session whose counters have not arrived.

The new USB console command `packet_count` reports copied scalar state: source
phase and ownership, channel/configuration, validated counters, recording
errors, UI tick gaps and last phase progress. Status heartbeats alone do not
advance the progress timestamp. Unknown or stale evidence is not reported as
known zero. The snapshot contains no target addresses or frame payload and
performs no radio calls or lease renewal. It is bounded and independently
copied for the console; the console handler remains a separate stack frame.

## Validation and installation

Source `30a955d04ecd052cba4b80e11c08ed32a9ad78cc` passes all **158 registered
host suites**, **five native targets** and their native gates on **1,234
unchanged inputs**. The slow-tick fixture fails against the old controller and
passes the correction, including single/dual source progress, lost replies,
expiry and Stop/release. Actual UI rendering and diagnostic formatter fixtures
also pass. Three older console fixtures needed their unrelated-handler boundary
updated for the new command; their initial failure logs are retained. The final
full run includes the expanded callback rejection counters.

The matched package is
`dist/cerberus-unchained-0.1.0-dev.rev134-20261103T005133Z`.
All 22 package checksum entries and nine images verify. Its manifest SHA256 is
`ff6d101cdd92b5f4e1481b16c37968eccfcec222210fd7e91fe8e59a0eef91d2`;
the source-content SHA256 is
`25c704969523202879b8bdc615cab52f4ed3fa482761d8afe2258f94c66f7c10`.
The retained local milestone is
`artifacts/rev134-validation/local-milestone.json`, SHA256
`b42f847cf2b797accaef1c0fcb73f1c1323b70c773dfac8f87bbe116218aeb11`.

Native fixed data/BSS grows by **464 bytes on S3** and **zero on C5**. The
diagnostic snapshot is 456 bytes with an 8-byte lock. UI/controller workspaces,
optional receive/recording allocations and configured task stacks are unchanged;
no new task is added. S3 application size is 2,116,976 bytes, C5 is 1,721,936,
and the bridge is 255,296. The console gate measures a 592-byte new handler
frame and a 2,128-byte selected console/formatter path within its 4,096-byte
allocation. These are linked measurements, not a full call-chain or loaded
runtime bound.

The owner stopped Packet Count and returned Home before installation. Fresh
inactive diagnostics preceded a deliberate non-flash reset and new cold-idle
proof. Fresh backups cover 2,183,168 S3 bytes and 2,097,152 C5 bytes and match
the installed rev133 images. All three bridge, three C5 and three final S3 image
writes verify. The previous package and original backup prefixes are retained.

Rev134 and C5 capability 74 boot successfully with mounted SD and no previous
reset fault. Two idle captures 62.47 seconds apart show advancing uptimes and
SPI exchanges (174 to 456), unchanged C5 boot count and no growth in the listed
SPI errors. Initial READY timeouts and legacy fallbacks remain four each;
mutex, READY-fall/high and driver errors remain zero. S3 idle free internal
memory is 136,720 bytes, largest block 94,208, free PSRAM 1,427,552 and tasks ten;
C5 sampled free memory is 53,572 bytes. The observed console stack low-water
value is 1,312 bytes, not a worst-case guarantee. The new diagnostic responds
with a known absent Count workspace before starting a test.

The installation receipt is `artifacts/device-rev134/installation-receipt.json`,
SHA256 `4edff522469c7433b66b8421baffc1f35a3bbb70dc7c3fcf4ded05476f41c6f1`.
Installation and sampled boot checks do not establish matching-frame reception.

Required physical checks are Packet Count setup and counter publication on an
active owner AP, Stop/Back followed by a normal Wi-Fi scan, and a repeat without
reset. Optional recording and multi-channel coverage need their own acceptance.
Mesh Save/Open, real Mesh/Locate reception, GPS and six-radio expansion remain
separate pending work. Full Marauder parity and serial packet capture remain
unfinished. A read-only upstream check retained the existing develop revision
`79cc6bcba1db6679baee1dab2b0c1ad5010e6363` on 2026-11-02.
