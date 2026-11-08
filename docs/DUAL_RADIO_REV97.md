# Rev97: retained Wi-Fi handoff and C5 Bluetooth list

This checkpoint improves passive discovery and selected-result navigation.
It does not establish full Marauder parity or hardware acceptance of every
feature. The matched rev97 package is installed with the boot evidence below;
new touchscreen and RF acceptance remain pending.

## Behavior

- A selected C5-origin AP can reuse its original completed bank after an
  owned session check, an expected full relationship-proof check, a second
  owned status check, and an export check. Its source index, generation,
  receipt, BSSID, channel, security and SSID must match exactly and uniquely.
  A valid reuse issues no RF START/STOP and transfers no AP rows again.
- Starting any replacement scan or fresh rebind invalidates eligibility
  before staging or INFO, even when that attempt is canceled or fails.
  A runtime stale session/proof fails explicitly. A canceled validation does
  not stop another service's radio. Locally ineligible targets, including
  S3-origin results, retain fresh C5 reacquisition.
- Reuse preserves the original full-bank receipt, source observations and
  actual band coverage. Reopening the same proven C5 bank cannot fabricate
  a channel-history sample. Mixed 5 GHz/all-band history uses the appropriate
  per-band denominator. Saved trust entries retain their existing format;
  comparisons describe the current bank's coverage.
- A transient C5 link interruption cannot send an imported bank through
  the older generation-byte-only reader and relabel its coverage. Uncertain
  refreshes fail before transport or metadata mutation. Statistics → AP List
  returns to the retained dual list without scanning or poisoning a healthy
  cache; a new verified publication restores the ordinary refresh path.
- The C5 Bluetooth companion offers SIGNAL, NAME A-Z and SCAN ORDER, plus
  ALL AGES and WITHIN 30S. Recent filtering uses an upper bound on age;
  unknown or invalid timing, and bounds above 30 seconds, are excluded.
  A bound above 30 seconds shows `RECENCY?`, because it cannot prove the
  actual observation is stale. Visible rows retain copied
  typed-address identities, so a refreshed bank cannot redirect a tap.
  Sorting/filtering does not allocate memory or start/stop/query radios.
- `wifi_dual_rebind_cached` identifies cached validation in the local
  diagnostic snapshot. `wifi_dual_elapsed_ms` is coordinator operation time,
  not physical touch latency; during reuse it describes validation, not the
  original scan duration. The fixed snapshot contains no AP identities.
  Failed cached validation retains the exact error and displays
  `C5 RESULT NOT VERIFIED / SCAN AGAIN`.

## Verification

All 120 host suites passed on unchanged inputs using Python 3.14.2 and
Zig 0.14.1. The receipt and individual logs are retained under
`artifacts/host-checks/rev97-full-run`. All five native targets also pass on
unchanged inputs matching that host receipt: S3, onboard C5, temporary bridge,
expansion WROOM and expansion XIAO. The isolated toolchain uses PlatformIO
6.2.0, ESP-IDF 6.0.1 and GCC 15.2.0. Full native receipts/logs and the S3 ELF
are retained under `artifacts/rev97-final-native-evidence`. Rendered C5 list previews were
inspected for normal rows, name sorting, uncertain/unknown timing and an empty
recent filter. These checks do not establish RF performance.

The S3 native build passes its interrupt-placement and console-stack checks.
Compared with the retained rev96 ELF, `.dram0.bss` grows from 145,376 to
145,904 bytes (+528), while `.dram0.data` remains 26,412 bytes. The existing
PSRAM directory banks remain four-byte pointers in internal memory. Native
debug types show the coordinator's optional PSRAM store at 10,784 bytes
(rev96: 10,776), the unchanged Wi-Fi view at 5,648 bytes, and the diagnostic
snapshot at 88 bytes (rev96: 80). The measured subset of the console call
chain uses 2,896 of 4,096 bytes, leaving 1,200 bytes; this is not a whole-SDK
or interrupt-stack bound. Evidence is under `artifacts/device-rev97` and
`artifacts/integration/s3-console-stack.json`.

## Installed package

The paired development package is
`cerberus-unchained-0.1.0-dev.rev97-20261101T051146Z`, built from source commit
`f010fbaf4967b501cc385395225ce7e172b154b0` with firmware source SHA-256
`c8890c5bb7d672cfbbd1542de82d26c152bdbb71c703d45b491acb763a25f557`.
Package hashes, chip headers, partitions and image bounds pass verification.
All three onboard application hashes match the full native-matrix builds.

Verified first-2-MiB backups of the installed rev96 S3 and C5 were retained;
they cover every region this update writes. The older complete S3 backup is
also retained. The bridge, C5 and S3 each verified all three image writes, with
the expected ROM/stub chip identity checked before writing. Rev97 then booted
with C5 protocol firmware 61 online, the display buffer active and SD mounted.
USB storage was inactive and the boot reported no previous reset fault. The
existing 26,240-byte main crash record predates this update and was preserved.

Initial internal free RAM was 134,740 bytes. A settled display/console read
showed 134,652 bytes free, a 94,208-byte largest block, 1,428,324 bytes of free
PSRAM and ten tasks. The console stack low-water mark was 1,100 bytes. Startup
SPI telemetry recorded four initial READY timeouts with four legacy fallbacks,
zero mutex timeouts and zero driver errors; these are startup observations,
not a zero-contention or sustained-load claim. No scan had run at this baseline.
Backups, write logs, package/native checks and both boot captures are retained
under `artifacts/device-rev97`, with `installation-receipt.json` summarizing
their scope. No new PR or public binary release was created for this checkpoint.

## Physical acceptance still needed

The owner confirmed that the correct AP opened twice. The matching console
capture (`artifacts/device-rev97/accepted-handoff.txt`) shows three successful
C5 scans in retained generation 1, no failures, `rebind_cached=0`, and a fresh
all-band handoff (14 2.4 GHz plus 15 5 GHz rows) taking 7,079 ms. All owners
were released; internal free RAM was 125,856 bytes and SPI mutex/driver errors
were zero. This confirms navigation and fresh reacquisition, not cached reuse.
The owner subsequently confirmed that 5 GHz rows show C5 and 2.4 GHz rows
show S3, and again reported both C5 opens working. The second capture
(`artifacts/device-rev97/accepted-c5-reuse.txt`) still shows retained generation
1 and S3 job 1, with six successful C5 scans, no failures and no cached reuse.
The latest fresh handoff took 7,220 ms and retained 18 2.4 GHz plus 13 5 GHz
rows. This does not establish that an ordinary replacement split scan ran
between the two checks. The navigation and cache-selection route remains under
investigation; cached reuse is not hardware accepted. Opening an S3 row or
any other fresh rebind replaces the C5 bank and makes the original retained
list ineligible for reuse until an ordinary Scan Again completes.

- C5/5 GHz AP tools and Back: same selected AP, original observation age,
  no additional C5 scan, followed by a successful normal split scan.
- S3/2.4 GHz AP tools: fresh C5 reacquisition, correct target and normal Back.
- C5 Bluetooth sort/filter/page controls and selected-device identity;
  Stop/Back followed by split Wi-Fi scanning.
- Rev96's SD-files/folder/Back, saved-log list, script list and following
  scan check remains pending. The owner reported both warm BLE-to-Wi-Fi
  repetitions working on rev96; only one completed transition was captured
  independently in the console.

GPS and six-radio expansion, dual-receiver Fox Hunt, BLE archive contents,
extended BLE report acceptance and sustained load remain separate open work.
