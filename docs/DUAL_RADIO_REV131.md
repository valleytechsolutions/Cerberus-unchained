# Rev131: Mesh menu navigation and passive selected-source Locate

The matched rev131 development package passes the complete local host/native
matrix and package verification and is installed on both chips. Paired
application boot and sampled idle checks pass. The owner confirms menu-first
navigation and the Back controls work; real Mesh feature acceptance remains
pending. This original Cerberus implementation uses CYM as a
workflow reference; see the
[reference and scope plan](MESH_DETECTION_PLAN.md). It does not claim complete
CYM or Marauder parity.

## Menu-first workflow

The owner reported that rev130 still showed Radio Sense in Scan and skipped
the tool menu. Its Back button then exposed the menu, reversing the intended
hierarchy. Rev131 changes the actual Scan renderer and page entry, not only a
secondary menu string:

1. Home → Scan → Mesh Detection opens the five-tool menu without allocating a
   survey controller or starting a radio service.
2. Mesh Detection, Channel Energy, Zigbee Beacons, Frame Activity and Compare
   Snapshots remain individual tools in that menu.
3. Back from a tool returns to this menu; Back to Scan returns to Scan.

Locate is an action on a selected address inside the Mesh observation tool.
It does not introduce another Scan submenu or discard the preserved survey.

## Division of work

C5 receives IEEE 802.15.4 passively on the selected channel. Its new compact
service matches the complete source address, address type and PAN context using
the production MAC parser. Unknown PAN is distinct from a known PAN and is not
a wildcard. Destination-only observations acquire signal measurements only
after that address is heard as a source.

The callback coalesces the newest matching observation instead of sending all
unrelated traffic through the survey FIFO. It uses the existing worker and an
optional internal allocation; it adds no radio task. The C5 parser/callback
placement is checked against the final native ELF before a build can pass.
This check is not a full vendor call-graph, interrupt-stack or RF latency proof.

S3 validates each complete pinned source projection, retains a 32-sample signal
history in optional PSRAM, renders the graph and age bound, and saves stopped
reports through the existing serialized archive worker. Unchanged sequences
do not reread raw bytes or add duplicate history samples. Published samples,
coalesced match updates and omitted older history entries are separate counts.

The survey finishes its exact-session stop/drain/release before Locate starts.
Locate workspace is allocated before requesting that handoff, so allocation
failure does not destroy the ongoing survey. Stop retains the Locate result;
Back closes its session and returns to the preserved address detail. Hidden
page cleanup must finish before another C5 owner proceeds.

## Source integrity and storage

New commands 154–160 require C5 capability 73. START is attempted once per
session; an uncertain reply is resolved through STATUS. STATUS renews the
active ten-second lease. A stopped result has a separate 30-second retention
lease. Expiry is checked before renewal, and the C5 worker handles cleanup
without depending on another SPI call. PEEK freezes a compact projection and
READ retrieves it in bounded chunks; there is no FIFO ACK operation in Locate.

Full raw headers are checked against the selected source before publication.
An unchanged sequence cannot change its raw witness, callback time or signal.
Invalid clocks revoke the displayed age bound. The two chips' uptime clocks
remain separate; the S3 pre-PEEK anchor yields a conservative upper age bound,
not a synchronized timestamp or proof of continuous reception.

STOP & SAVE / SAVE LOCATE requires a stopped projection, matching final STATUS
and confirmed release. It copies an immutable snapshot before handing work to the shared SD
worker. `CERBERUS_MESH_LOCATE_CSV,1` is separate from the survey CSV schema and
uses `/CERBERUS/LOGS/Mxxxxx.CSV` names. It includes the selected typed source,
source counters/error, retained/coalesced history, age-at-anchor bound and final
raw witness. Only the final sample has a raw witness in this report. The codec
has a 16,384-byte cap plus NUL; old archive operations retain their own exact
allocation sizes. Cancel before storage skips writing; a save already in
progress reports its actual result.

This is passive source-signal observation. It does not join a network, transmit
to a device, establish authenticated identity or routing, identify a Matter
fabric, infer distance/bearing, or reopen a historical map. No GPS is required.

## Validation record

Production-bound host fixtures cover C5 receive/filtering,
S3 controller and SPI adapter, source identity/raw validation, lost responses,
clock failure, session cleanup, CSV and archive lifecycles, UI navigation and
the preserved survey/Locate handoff. The actual Scan entry, five-tool menu and
shared Back footer are rendered and checked by the host UI fixture.

At built source `6f7750ae58a3a9325f31a3384625c891923c039b`:

- All 150 registered host suites and five native targets pass with 1,194
  unchanged input hashes across the two matrices.
- The matched development package is
  `dist/cerberus-unchained-0.1.0-dev.rev131-20261102T212533Z`. All 22 checksum
  entries and the read-only web-export verification pass. No public binary
  release is selected or exported.
- The final native C5 IRAM check passes for twelve project functions and two
  DRAM lookup tables; its decoder has 151 passing self-test cases. This remains
  a bounded placement/control-flow check, not a complete vendor call graph or
  interrupt latency proof.
- Earlier failed runs are retained separately. They caught an outdated footer
  extractor, missing central-enum command entries and Windows/RISC-V decoder
  cases. The final matrix was rerun after those corrections.

Native linked fixed data/BSS totals are 169,420 bytes on S3 (16 fewer than
rev130) and 83,400 bytes on C5 (40 more). Those deltas exclude executable IRAM
and optional heap allocations and are not the total runtime RAM cost. C5's
448-byte Locate context and 2,624-byte survey context are mutually exclusive.
S3's optional Mesh UI, Locate controller and Locate report snapshot total
42,280 bytes when overlapping. A retained larger survey-report operation can
raise the conservative overlap to 143,784 bytes. These measurements establish
allocation sizes, not sufficient headroom under every loaded workflow.

## Installation and remaining acceptance

Fresh verified rev130 backups were captured before installing the matched
bridge, C5 and S3 images. Every programmed image hash verified. Both captured
application samples report rev131, C5 capability 73 online, mounted SD and
idle radio state. The samples are 200.52 seconds apart: both chip uptimes
advance, C5 boot count stays at one and SPI exchanges increase from 256 to
1,117. No additional listed SPI error or fallback counters accrue. Four
initial-ready timeouts/fallbacks are present at boot and remain unchanged;
mutex, falling/high-ready and driver error counters stay zero.

At both samples S3 internal free memory is 137,680 bytes with a 94,208-byte
largest block; PSRAM free is 1,427,552 bytes. C5 snapshot free memory is 53,636
bytes, 2,480 fewer than the rev130 baseline. The console's observed stack low
water changes from 2,016 to 1,280 bytes during these captures; it is an observed
minimum, not a worst-case stack guarantee.

Local evidence is retained in `artifacts/rev131-validation/` and
`artifacts/device-rev131/`. The installation receipt SHA-256 is
`d5828a331491e13f61a14b5ec079049c177763bc8a925f15f1bbe7209e17ef8e`.
The earlier immutable `local-milestone.json` correctly records the package
checkpoint before installation; the separate installation receipt records the
subsequent paired update.

The owner confirmed on 2026-11-02 that Home → Scan → Mesh Detection opens the
five-tool menu and that MESH MENU / BACK TO SCAN return to the expected screens.
Mesh Start/Stop followed by a normal Wi-Fi scan remains unverified. Real selected
source reception, Locate handoff/history, stopped SD save/reopen and sustained
loaded operation are also unverified. Sampled idle boot and software checks
do not establish those behaviors or complete Mesh/CYM/Marauder parity.
