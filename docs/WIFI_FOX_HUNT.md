# Wi-Fi Fox Hunt

Rev162 adds a tracker for a selected Wi-Fi transmitter. It uses the C5 receiver
and the S3 for control, signal history and the display. Signal strength is a
received measurement; it is not distance, bearing or proof of device identity.

## Upstream reference

The reference is JustCallMeKoko and the ESP32 Marauder contributors' develop
commit `79cc6bcba1db6679baee1dab2b0c1ad5010e6363`. The upstream branch was
checked again on 2026-11-08 UTC and still matched that commit.

- `MenuFunctions.cpp:2195–2203` exposes AP, station, WiFi Pineapple and
  MultiSSID target lists. The existing Cerberus inventories already provide
  these four sources; they open the tracker directly from their
  selected-target detail pages.
- `MenuFunctions.cpp:2155–2161` binds a selected MAC and channel.
- `WiFiScan.cpp:2846–2847` dispatches the signal mode to its receive path.
- `WiFiScan.cpp:12438–12466` maintains the target reading and source time.
- `WiFiScan.cpp:12538–12628` refreshes the signal view and begins a channel
  search after 1,500 ms without a matching observation.

Source: [Marauder target and signal handling](https://github.com/justcallmekoko/ESP32Marauder/blob/79cc6bcba1db6679baee1dab2b0c1ad5010e6363/esp32_marauder/WiFiScan.cpp#L12438),
[Marauder source menus](https://github.com/justcallmekoko/ESP32Marauder/blob/79cc6bcba1db6679baee1dab2b0c1ad5010e6363/esp32_marauder/MenuFunctions.cpp#L2195).
The original MIT notice remains in `LICENSES/ESP32Marauder.txt`.

## Measurement rules

Cerberus matches the selected **transmitter address** before using a
frame's RSSI. A destination or BSSID match alone cannot establish that the
selected device sent the frame. Short control frames without a transmitter
address cannot refresh the target signal. This deliberately avoids assigning
another transmitter's signal strength to the selected target.

The selected name, address and starting channel are a frozen selection from
an earlier observation. Starting a hunt clears the live reading. A later
packet supplies the observed channel and source age; the listening channel
is a separate field. A missing target starts channel search, not a zero-RSSI
reading. Channel search uses only SDK-accepted channels and reports skipped
channels or failed tuning explicitly.

S3 polling may skip intermediate target observations. History therefore
describes sampled readings, with visible gaps, rather than one point per
received frame. A duplicate reading retains its age. Transport failures
clear the current-signal claim, and clock discontinuities trigger cleanup.

## Using the tracker

Open **FOX HUNT** from the detail page of an AP, an observed client, a
PineScan transmitter or a MultiSSID transmitter. The selected address,
channel and label are copied before the prior receiver is stopped. **START**
waits for that receiver to release and starts a fresh session. **STOP**
retains the result; **RESTART** appends a new segment with a gap. Back waits
for release and returns to the same source detail. Sleep stops reception;
waking does not automatically restart it.

AP, PineScan and MultiSSID searches stay in the selected transmitter's band.
Client searches cover both bands for roaming. The screen shows that scope.
After 1,500 ms without a target observation, C5 searches supported channels
with at least 250 ms of successful reception per dwell. A matching
transmitter stops the search on that channel. The last observation and its
age remain separate from the currently reported listening channel.

The S3 reads the latest signal at a 100 ms target, renews session status at
a 500 ms target and draws at most four times per second. Each tick performs
at most one bounded transport operation. Timing targets are scheduling
settings, not measured field latency. Up to 160 signal/gap entries live in
an optional PSRAM workspace that is freed after Back completes.

The C5 shares Monitor and Analyzer's worker, exclusive reservation and radio
restoration; no new task or second RF receiver is added. Capability 83 adds
commands 198–202 with exact session identity. Older peers are refused.
Lost Start replies are resolved by querying the same session, without
replaying the mutation. Delayed callbacks carry an admission token so an
old channel or session cannot update a newly armed hunt. Failed restoration
retains ownership for Stop retry before another scan can start.

This candidate also fixes the Channel Analyzer's cleanup status: an error
remains visible while release is retried, including when Back is waiting.

## Acceptance

All 179 registered host suites and five native targets passed on unchanged
inputs. The separate 21-test CI shard-verification suite also passed.
Physical tracking,
reacquisition, Stop/Back/restart, sleep/wake and subsequent Wi-Fi/BLE scans
remain unverified. No radio activity is started by this document or its
source audit. The coverage ledger must remain partial until those remaining
checks have evidence.

## Installed checkpoint

Rev162 is installed from source `85d805211e6f68984b57b25f1dc8dba91fc68024`,
tagged `checkpoint/rev162-wifi-fox-hunt`. Package:
`cerberus-unchained-0.1.0-dev.rev162-20261108T085645Z`.
Manifest SHA-256:
`b0e6ce21129cf7f34f0f33a4599fa399d67f1f0e0e156d1bb4135bf3f76c84df`.

The host fixtures exercise the production C5 callback/worker/dispatcher,
S3 wire adapter/model and all four UI entry paths. They cover transmitter
attribution, loss/search/reacquisition, source age, lost replies, exact mode
ownership, failed tuning/restoration, Stop/Back, sleep, retained results and
subsequent scans. Real-font previews were checked for normal, stale, warning
and stopped states. Older extracted-callback fixtures were updated to supply
the inactive Fox consumer before the complete suite was rerun successfully.
Two unregistered legacy PineScan/MultiSSID navigation harnesses still have
older API drift; their results are not counted as passing coverage. The
registered and new focused route fixtures use the current entry handlers.

The optional S3 UI workspace is 2,768 bytes under the host ABI. C5 adds no
radio task, dynamic allocation, packet queue or retained packet payload.
Application images are 2,412,208 bytes on S3 and 1,747,312 bytes on C5, within
their configured partitions. Allocation sizes and image sizes are separate
from runtime headroom.

Fresh S3/C5 recovery backups matched the installed rev161 baseline and were
verified against flash. All nine bridge/C5/S3 image writes verified. Both
processors' live application descriptors match the packaged version, project,
SDK, compile time and full ELF hash.

Two idle captures 72.9 seconds apart showed advancing uptimes, mounted SD
and stable memory: S3 had 134,244 internal free bytes, a 94,208-byte largest
internal block, 1,426,140 free PSRAM bytes and ten tasks. C5 had 53,012 free
bytes and one boot. The four initial READY timeouts/fallbacks did not increase;
mutex, READY-fall, READY-high and driver errors remained zero. The console's
observed stack low-water changed from 2,084 to 1,044 bytes after diagnostic
commands; this is not loaded-service stack acceptance. Saved home Wi-Fi,
WiGLE and WDGWars profile readiness remains set; no upload was requested.

This establishes software verification and paired idle installation. Physical
2.4/5 GHz tracking and reacquisition, four-source navigation, Stop/restart,
sleep/wake, later Wi-Fi/BLE scans and sustained memory/stack/throughput remain
pending. Full Marauder parity is still open.
