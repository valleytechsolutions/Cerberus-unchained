# Rev128: passive Probe Sniff and accumulated contribution

This milestone is implemented. All 138 host suites and all five native targets
pass on the same 1,142 unchanged inputs, and the linked resource audit passes.
Matched-package verification also passes. Rev126
remains the latest verified installed pair; the owner accepted its Recon
Dashboard/Radar controls, ALERT wording and stable recognized-hint priority.
Rev127 has a verified local package and has not been installed separately.
The new Probe Sniff workflow still requires physical acceptance.

## Workflow and processor roles

Wi-Fi Tools gains a dedicated Probe Sniff entry. Entry opens an idle page;
explicit Start begins passive reception on a chosen channel or band-hopping
plan. Requests, Transmitters and Networks provide different views of the same
bounded observations. Details preserve the selected observation even as capture
continues. Stop releases reception; Save stops first and writes an explicit
evidence report to SD. Back and hidden-page cleanup retain the exact accepted
capture identity until release is confirmed.

C5 owns Wi-Fi reception, frame validation and its bounded native inventory.
S3 validates transferred objects, builds the views, renders details and saves
reports through the existing serialized archive worker. S3 does not duplicate
C5 reception in this mode. This division leaves presentation and storage work
off C5 and avoids another receiver task on S3.

## Evidence semantics and limits

- The native table retains the first 32 distinct transmitter-address plus
  exact SSID-byte pairs. Existing keys update in place. Capacity drops are
  reported; old rows are not silently evicted to make room.
- An empty SSID is a wildcard request. Embedded NUL and other binary bytes
  remain significant. Display text is an aid; explicit length and hexadecimal
  bytes preserve the observation in details and reports.
- Addresses are observed transmitter addresses, not confirmed physical-device
  identities. Probe requests do not prove association to a network. Callback
  counts include repeated and retry-marked frames; they are not packet-loss
  estimates or deduplicated RF sequence counts.
- Row channel/RSSI/time describe the latest admitted callback. Channel tuning
  masks report attempted and successful tuning, not guaranteed reception of
  every frame on those channels. Source times are C5 callback uptime, not UTC,
  S3 RF time, distance or bearing.
- Live rows are copied independently. The UI must identify them as a sampled
  view, not one simultaneous capture instant. A saved report uses a stopped,
  frozen table with session and revision checks before and after readout.

## Ownership, responsiveness and storage

The new protocol requires C5 capability 71. Scoped control carries a full
session identity; object reads carry that identity plus an immutable read pin.
Malformed, mixed or incomplete transfers cannot become displayed observations.
Running ownership uses a renewable lease. Stopped data retention may be renewed
by exact reads/status without restarting RF. Native cleanup retains its radio
reservation while the SDK has not confirmed release.

S3 advances the transfer incrementally, with Stop intent ahead of optional
reads. A full table download is not one synchronous UI call. Runtime loaded
latency and dense RF behavior require physical measurement; exchange budgets
and host fixtures alone do not establish them.

The saved CSV contains bounded source metadata and exact SSID hex bytes under
`/CERBERUS/LOGS`, readable through the existing Files viewer. A dedicated writer
uses exclusive temporary-file creation, checked destination absence and the
existing SD/USB ownership gate. Failed writes retain their temporary filename;
successful rename establishes publication, not a power-loss durability
guarantee. The report has an 8,192-byte bound. It preserves captured identifiers
and requested names even when the screen's address masking is enabled.

The implementation uses one optional native C5 context and an optional S3
PSRAM workspace. The linked resource audit measures 2,464 bytes for the C5
context, 6,368 bytes for the S3 UI workspace, and 11,502 bytes for the selected
Probe save-worker context, including its header and bounded formatter buffer.
These sizes exclude allocator overhead and runtime SDK allocations. It reuses
the existing scanner and archive workers; invoking the archive worker still
requires that worker's existing task allocation.

## Upstream reference and scope

The workflow reference is **JustCallMeKoko and the ESP32 Marauder contributors'**
`RunProbeScan`, the ordinary `WIFI_SCAN_PROBE` callback in `WiFiScan.cpp`, and
its `MenuFunctions.cpp` entry at pinned develop commit
`79cc6bcba1db6679baee1dab2b0c1ad5010e6363`.
At 2026-11-02 00:22:40 UTC the GitHub compare API reported `develop` identical
to that pin: zero commits ahead/behind and no changed files. The response is
retained in `artifacts/upstream/rev128-marauder-develop-check.json`.
The Cerberus session protocol, bounded inventory, display/controller and report
integration are independent implementations informed by that workflow. Existing
upstream notices remain intact; see [the attribution guide](MARAUDER_ATTRIBUTION.md).

The existing Wardriving clue-filtered probe table retains its separate
semantics. Raw-frame PCAP capture remains a separate existing workflow; this
CSV report is not a full-frame capture and does not establish complete Marauder
Probe Sniff parity. The coverage ledger must keep `WIFI_SCAN_PROBE` partial.

## Validation and requested stopping point

Focused validation passes 657 model/controller checks, 6,023 actual C5
RX/dispatcher/worker assertions and 15,429 checks through the actual C5-to-S3
wire adapter/controller bridge. The actual UI fixture passes 2,774 checks;
all seven 320x480 renderer previews have been inspected. Fifteen affected
receiver fixtures and the registered dual-Wi-Fi routing, Recon, relationship
and baseline UI checks pass. Independent source review has no remaining
material blocker.

The first native pass rejected two compressed guard-return statements under
GCC's misleading-indentation warning. Formatting-only corrections separate
the guard returns from subsequent statements. The publication audit also
corrected candidate packaging to include the retained `LICENSES` directory
before checksum generation. The earlier host receipt and failed native pass
are retained separately; final validation must bind these corrected inputs.

The actual archive filesystem suite passes 254 isolated cases and the worker
suite passes 78,938 checks, covering unchanged older formats as well as the new
Probe arm. Console formatting checks pass. New cases cover immutable input,
binary names, every count from zero through 32, typed completion, cancellation,
late release, failed temporary files and retrying frozen evidence after radio
release. The maximal report fixture is 7,253 bytes against its 8,192-byte limit;
this is a tested fixture size, not a claim about arbitrary invalid inputs.

The final full host run completed at 2026-11-02 00:49:03 UTC; all 138 suites
passed. The five-target native matrix completed at 00:50:52 UTC with S3,
onboard C5, bridge, WROOM expansion and XIAO expansion passing. Both receipts
bind the same 1,142 unchanged inputs. The host receipt SHA-256 is
`60f4a32b7cc379159310c700de2370dec851d1c042c22c40f2aaf98fd4b24cee`;
the native report SHA-256 is
`ef10f0dd0b7164339568fb0b685c10b857cee7f720883247dd1650214d7740b0`.

Against the retained rev127 ELFs, linked fixed data/BSS is unchanged on S3
and grows by 48 bytes on C5. Compared existing observation banks, model sizes
and configured task stacks remain unchanged. The resource report SHA-256 is
`7e8785e9d500c9e86b7537709ebf96ba7b9aa216741a3172d31e9dc8477725ac`.
An initial off-target GDB inspection reached its default value-size limit
while examining the archive union. The corrected audit raises that inspection
limit to 1 MiB, measures the selected allocation arm rather than the whole
union, and binds the same final ELFs. The first helper output is retained
separately. No firmware input changed for that correction.
The final staged diff also removes trailing blank lines from four new files;
the earlier passing receipts are retained separately and the final full
matrices above bind the normalized source. Application hashes did not change.

Nine build/package failure-handling tests and the portable ledger checks pass.
SD-resource validation passes 40,187 OUI lookups, Recon policy checks and
100,000 malformed expansion frames. Seven UI previews were inspected at
320x480. These local Windows, linked-image and modeled-boundary checks do not
establish Linux CI, physical receive, save/reopen, repeated-use, runtime
heap/stack margins or loaded responsiveness acceptance.

The owner requested this major milestone, an update to existing
[Unchained PR #1](https://github.com/463N7/Cerberus-unchained/pull/1), then a pause
of the unfinished overall port. The existing PR branch's later Linux fixture
and Windows timeout fixes are preserved in the development history. No public
binary release, automatic upstream merge or further feature milestone is part
of that stopping point.

## Final package and resumption checkpoint

Source commit `c3d7d3c88487e70f1c99f04b8fb1126aeccd4be6` produced the verified
local package `cerberus-unchained-0.1.0-dev.rev128-20261102T005202Z`.
Its manifest SHA-256 is
`ea2cef8786dd9e5f002b027f3fa1a8e0735793ad85899cbbb48fe3fd64337ad6`.
The packaged S3, onboard C5 and bridge applications match the retained
five-target matrix; all 20 package checksum entries verify, including the
exact retained Marauder license notice. The native console evidence matches
the packaged S3 ELF. The exporter passes `--verify-only` for image hashes,
chip headers, partitions and bounds, without exporting or selecting a release.
The final local receipt is retained in
`artifacts/rev128-validation/local-milestone.json`.

This package has not been installed. Rev126 remains the last verified device
pair; its accepted Recon controls do not establish Probe Sniff acceptance.
The existing PR carries the accumulated rev82–128 contribution. Hosted CI
results belong to the actual updated PR head and remain separate from the
local Windows validation above. The overall mission pauses at this checkpoint.

On resumption, first review the updated PR checks and finish any failed CI
gate. Then install the matched candidate through the normal radio-release
gate and test Probe Sniff Start/Stop, all three views, selection stability,
Save/Open File and repeated Back-to-scan transitions. Measure loaded response
time and heap/stack margins on the board. Verify the scoped Channel Survey
handoff on hardware as well. Continue from
[remaining gaps](RECEIVE_REMAINING_GAPS.md) and the
[coverage ledger](MARAUDER_COVERAGE_LEDGER.md); reported active-feature failures,
full Marauder parity, GPS and six-C5 expansion acceptance are still open.
