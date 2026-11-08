# Passive EAPOL monitor

Rev164 adds an EAPOL report to the existing selected-AP capture page. Open
**Scan -> Wi-Fi -> an AP -> CAPTURE THIS AP**, cycle the capture mode to
**EAPOL MONITOR**, then press **START**. The selected BSSID and channel are
frozen for the run. Opening the page starts no receiver.

**SAVE PCAP OFF** is the default. The report works without an SD card. Enable
**SAVE PCAP ON** before Start to save the received frames as well. The summary
shows observed M1-M4 patterns, correlated exchanges, PMKID-bearing frames,
other EAPOL traffic, malformed frames and retry/peer-limit counts. **PEERS**
opens a two-row paged view of station evidence, channel, last RSSI and S3
delivery age. **STOP** retains the report; **RESTART** begins a new report.

Back, leaving the page and sleep cancel pending Start requests and stop the
owned session. Navigation and memory release wait for the receiver, final
frame drain and any open capture file to close. An error during cleanup does
not turn into an idle state. Waking the screen does not restart reception.

Rev165 retries a pending Stop if the service mutex was temporarily busy. A
missed first request no longer leaves Back waiting indefinitely for release.
Host cases cover both the Stop button and leaving through Back; physical
navigation under load remains an acceptance check.

## What the report establishes

The parser accepts complete, CRC-verified 802.11 frames up to 4,096 bytes from
the selected infrastructure BSSID and channel. It checks direction, ordinary/QoS header
length, fragmentation, protection, LLC/EAPOL boundaries, descriptor layout
and clear key-data element bounds before classifying a Key frame. Non-Key
EAPOL, unsupported layouts and malformed input are accounted separately.

M1-M4 are **observed field patterns**. A correlated exchange requires an
ordered sequence for one station, compatible descriptor/MIC layouts,
matching replay counters and publicly transmitted nonces, within a
15-second C5 source-time window. RSN M3 requires the Secure bit; legacy WPA
has its own allowed pattern. Repeated radio receptions remain observations
but do not count the same exchange again. A repeated SPI frame sequence
does not refresh evidence or its displayed delivery age.

An observed exchange does not verify the MIC, prove successful authentication,
establish possession of a credential or authenticate the physical AP/client.
The bounded model retains public nonces for correlation; it does not retain
MIC bytes, PMKID values or full frame payloads. Optional PCAP saving retains
the received frames through the existing capture writer.

PMKID counts mean that a correctly bounded, clear EAPOL Key Data element
with the PMKID selector was observed. Encrypted key data is counted without
trying to inspect its contents. An ambiguous descriptor-version-zero
16/24-byte MIC layout cannot produce a correlated exchange or PMKID claim.
This does not scan association-frame RSN elements, decrypt traffic, recover
credentials, transmit solicitation frames or join the AP.

## Radio, storage and memory ownership

The **C5 owns RF reception** on the selected 2.4 or 5 GHz channel. The **S3
owns verified transport, parsing, correlation, the report and optional SD
writing**. There is one receiver stream, not two copies of the same RF work.
This mode does not add a local S3 RF worker or channel hopping.

Capability **85** is required by both the UI and programmatic session entry.
The C5 snapshots a private per-arm admission token and signed callback time
before other receive consumers run. Admission opens only after the SDK
successfully enables reception on the channel. The token advances on each
arm, including rearming within the same capture generation, and exhaustion
fails closed. Old-token, unarmed, pre-arm, negative and unrepresentable
timestamp callbacks cannot be relabeled as new evidence.
This uses the existing capture commands, worker, lease and two-slot frame
ring; it adds no queue or per-frame copy.

The S3 reuses `wifi_capture_session.c` and its once-per-generation/sequence
observer. A new run reserves an exact generation; a lost Start response is
resolved by the capture owner rather than replaying a new Start. Stop uses
that generation and cannot stop another capture. Failed restarts preserve
the previous report. Optional file writes and analysis counts stay separate.

At rev165, the report holds at most **16 stations** in a **3,696-byte optional
PSRAM store under the host ABI**. This is an allocation size, not measured device
headroom. The optional UI store adds **520 bytes**, including its frozen AP
identity and two copied peer rows. Rev165 adds 32 bytes to each of the
session and UI stores for shared channel-plan telemetry; the rev164 sizes
were 3,664 and 488 bytes, respectively. An in-progress exchange is protected from
replacement within its correlation window. Once the table is full, eligible old rows may be replaced;
otherwise the new peer is counted as dropped. These counts and counter
saturation remain explicit. Row admission sequences distinguish replacement
from an update, even when a station reappears at the same source timestamp.
Page reads copy at most four rows under the evidence mutex; the UI uses two.

C5 callback times are source-relative and are not translated into S3 or UTC
time. Displayed ages say when a row reached the S3, not when its RF signal
arrived. Unknown or saturated delivery age is not displayed as zero. Scan-list
RSSI is historical and never supplies a new monitor sample.

Saved files use the existing PCAPNG writer, with C5 interface identity and
radiotap channel/RSSI. Without a verified C5-to-UTC mapping, the writer emits
Simple Packet Blocks with **no frame timestamps**. The page states this when
PCAP saving is selected. GPS presence does not silently convert C5 callback
time into UTC. C5 received/dropped counts, SD-written counts and the final
filename describe different stages; none is a promise of lossless capture.

## Upstream reference

The milestone's `develop` check on 2026-11-08 still resolved to
`79cc6bcba1db6679baee1dab2b0c1ad5010e6363`. Relevant reachable upstream paths are:

- [EAPOL/PMKID Scan menu](https://github.com/justcallmekoko/ESP32Marauder/blob/79cc6bcba1db6679baee1dab2b0c1ad5010e6363/esp32_marauder/MenuFunctions.cpp#L5182), which starts `WIFI_SCAN_EAPOL`.
- [The `sniffpmkid` CLI handler](https://github.com/justcallmekoko/ESP32Marauder/blob/79cc6bcba1db6679baee1dab2b0c1ad5010e6363/esp32_marauder/CommandLine.cpp#L1045), including channel and selected-list options.
- [`RunEapolScan()`](https://github.com/justcallmekoko/ESP32Marauder/blob/79cc6bcba1db6679baee1dab2b0c1ad5010e6363/esp32_marauder/WiFiScan.cpp#L5206), [`getCompleteEapol()` and the callback](https://github.com/justcallmekoko/ESP32Marauder/blob/79cc6bcba1db6679baee1dab2b0c1ad5010e6363/esp32_marauder/WiFiScan.cpp#L10458).

The pinned callback records M1-M4 flags per AP; its complete counter combines
those four flags without per-station exchange correlation. Its persistent
ForcePMKID setting can enable transmission even through the nominal EAPOL
scan entry. Cerberus's monitor remains receive-only. Fixed selected-AP analysis
does not establish parity with upstream channel hopping, multi-AP selection,
CLI options or active EAPOL workflows.

## Validation boundary

All 184 registered host suites passed on 1,422 unchanged inputs at source
`345ba46f1fc1220c99dabc8e9ae5d11220bac9d1`. All five native targets passed,
including their native placement/link checks; the matched package passed its
source and image checks. The separate 21-test CI shard verifier also passed.
The first full host run found two fixture compile failures in unrelated Mesh
and MAC callback boundaries. Those fixture boundaries were updated, and the
entire 184-suite matrix passed afterward; production behavior was unchanged
by that follow-up.

Focused production fixtures cover parser boundaries, source sequence/time,
per-station correlation and retries; exact session ownership, allocation
failure, retained errors and peer replacement; and the capture callback's
admission fence across delayed callbacks, same-channel restart and rearm.
The session fixture passes with the real parser. C5 service/radio and affected
receive-callback regression fixtures exercise the admission changes. The UI
fixture passes 3,285 checks with the production renderer and controls, covering
the existing selector, summary/peer views, Stop, Back, hidden/sleep cleanup,
old firmware, frozen AP identity and optional PCAP display. These checks do
not establish RF reception or touchscreen acceptance.

Physical acceptance still needs a controlled AP/client connection while the
monitor is passively listening, comparison with an independent capture,
optional PCAP readback, repeated Stop/Back/next-scan transitions, sleep/wake,
SD failure handling and sustained loaded memory/stack measurements. Zero
observed exchanges can be normal when no relevant connection occurs. Neither
full upstream parity nor authenticated-handshake verification is claimed.

## Installed checkpoint

Rev164 is installed from source `345ba46f1fc1220c99dabc8e9ae5d11220bac9d1`,
tagged `checkpoint/rev164-passive-eapol`, with C5 capability 85. Package:
`cerberus-unchained-0.1.0-dev.rev164-20261108T103133Z`.
Manifest SHA-256:
`34126ace1b6ccf82ba246dd03b4cc2944240ce765ec75867a78674d57d3da612`.

Fresh rev163 S3/C5 recovery backups were checked against the installed
baseline and verified against flash. All nine bridge/C5/S3 image writes
verified. Both live application descriptors match the packaged version,
project, SDK, compile time and full ELF hash. The card mounted, and saved
home Wi-Fi, WiGLE and WDGWars profile readiness remained set. No connection
or upload was requested.

Two idle captures 50.9 seconds apart showed advancing uptimes and SPI
exchanges, one C5 boot, and no additional listed transport errors. Four
initial READY timeouts/fallbacks remained unchanged; mutex, READY-fall,
READY-high and driver errors remained zero. S3 retained 134,092 internal
free bytes, a 90,112-byte largest internal block, 1,426,140 free PSRAM bytes
and ten tasks across those captures. C5 retained 52,996 free bytes.

Compared with the captured rev163 baseline, S3 internal free memory is
104 bytes lower and C5 free memory is 16 bytes lower. The largest S3 block,
PSRAM and task counts match. The console stack low-water changed from 2,092
to 924 bytes after diagnostics; it is not treated as constant or as loaded
service acceptance. The optional EAPOL stores were not allocated during these
idle checks. Restart allocates a replacement analysis store before retiring
the previous stopped report, so its transient allocation exceeds one store.

Application images occupy 2,432,592 of 7,340,032 S3 application bytes and
1,748,160 of 2,031,616 C5 application bytes. Image fit and idle measurements
do not establish sustained capture throughput or loaded memory headroom.
Physical RF, touch navigation, PCAP readback, sleep/wake and subsequent-scan
acceptance remain pending, as does full upstream feature parity.
