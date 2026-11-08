# Passive EAPOL monitor

Rev167 extends the selected-AP monitor with a direct entry at **Scan -> Wi-Fi
-> TOOLS -> AUTH RX -> EAPOL MONITOR**. The same AUTH RX menu also opens
**SAE MONITOR**. Opening either monitor or its AP picker starts no receiver.
The picker shows the held inventory; **SCAN** explicitly requests a new sweep.
**DONE** and Back return to the owning monitor without starting capture.

For one AP, open **CAPTURE THIS AP** from its tools and cycle to **EAPOL
MONITOR**. This route starts with THIS AP selected and restores the original
AP and scan-return destination after Back. The menu route defaults to ALL.

| Scope | Receive plan |
| --- | --- |
| ALL | All 42 canonical candidate channels, all BSSIDs |
| 2.4 GHZ | Channels 1-14, all BSSIDs |
| 5 GHZ | The 28 canonical 5 GHz candidates, all BSSIDs |
| FIXED CHANNEL | One chosen canonical channel, all BSSIDs |
| SELECTED APS | Up to 32 exact BSSIDs and the union of their selected channel hints |
| THIS AP | The copied AP BSSID and channel; available from the AP route |

**START** freezes the scope before releasing earlier scan owners. Planned
EAPOL reception uses a **2,000 ms dwell after successful receiver arming**.
A one-channel plan keeps receiving without cycling the receiver off. Channel
coverage reports attempted and successfully applied channels; the driver and
country configuration decide whether a candidate can be tuned. Selected
channels are historical hints, not proof that an AP remains on that channel.
SAE retains its separate 250 ms dwell.

**SAVE PCAP OFF** is the default and needs no SD card. The summary reports
observed M1-M4 patterns, correlated exchanges, PMKID-bearing frames, other
EAPOL, malformed input, retries, bounded-table drops and radio coverage.
**PEERS** pages through AP-and-station pairs. **BEACONS** presents separately
observed beacon information. **STOP** retains the report; **RESTART** begins a
new report. Options edited after Stop affect only the next run.

Back, hidden navigation and sleep cancel pending Starts and stop the owned
session. Receiver drain and any open capture file must close before release.
A missed Stop is retried; a cleanup error does not become an idle state.
Waking the display does not restart reception.

## Evidence and limits

The parser accepts complete, CRC-verified 802.11 frames up to 4,096 bytes from
the frozen scope. Infrastructure BSSID selection follows frame direction:
management address 3, ToDS address 1, and FromDS address 2. It does not treat
a station or payload address as the BSSID. WDS/direct-link data cannot supply
infrastructure EAPOL evidence. Header lengths, fragmentation, protection,
LLC/EAPOL boundaries, descriptor layout and clear key-data element bounds
are checked before classification.

M1-M4 are observed field patterns. A correlated exchange requires an ordered
sequence for one **BSSID + station pair on one channel**, compatible
descriptor/MIC layouts, matching replay counters and publicly transmitted
nonces, within a 15-second C5 source-time window. A station seen at another AP
has a separate row. Frames from different channels cannot complete the same
in-progress exchange. RSN M3 requires the Secure bit; legacy WPA has its own
allowed pattern. Retries remain observations without counting the same
exchange again. Repeated SPI delivery does not refresh evidence or age.

This does not verify the MIC, prove successful authentication or possession
of a credential, or authenticate the physical AP/client. The bounded model
retains public nonces for correlation, not MIC bytes, PMKID values or full
payloads. Optional PCAP saving retains received frames through the existing
writer. PMKID counts indicate a bounded clear key-data element with that
selector. Encrypted key data is counted without inspecting its contents;
ambiguous descriptor-version-zero layouts cannot establish a correlated
exchange or PMKID claim. No solicitation, AP join or credential recovery is
performed by this monitor.

The independent **16-BSSID beacon context cache** retains exact SSID bytes
and length, observed channel, an advertised channel when present, capability
bits, RSSI and source provenance. Beacon parsing rejects malformed tails.
Names are display-sanitized and address masking follows the privacy setting.
These public fields are observations, not a derived security-suite or device
identity claim. A beacon never creates a station exchange, completes one,
or refreshes an EAPOL row's delivery age.

## Radio, storage and memory

The **C5 owns passive RF reception and the channel schedule**. The **S3 owns
frozen selection, verified transport, analysis, report navigation and optional
SD writes**. This uses one RF stream, the existing capture worker, lease and
two-slot frame ring. It adds no RF worker or queue. New scoped EAPOL requires
C5 capability **87**; older fixed raw EAPOL and SAE wire contracts remain
separate at 85 and 86 respectively. Mode/dwell mismatches fail before Start.

Beacon context is lower priority: a beacon is admitted only while the frame
ring is empty, leaving the second slot available to EAPOL. Occupied-slot
skips are included in the aggregate `dropped_other` counter. Pinned frames
are never evicted or overwritten. This applies to both planned and fixed
EAPOL-plus-beacon capture; it is bounded context, not complete beacon coverage.

The C5's per-arm token and signed callback time fence remain in force. Old,
unarmed, pre-arm, negative or unrepresentable callback provenance is rejected.
A new run reserves an exact generation; lost Start acknowledgement is resolved
by its owner. Stop cannot end another generation. Failed restarts preserve
the prior report, and the S3 observer ingests each verified sequence once.

The analysis store holds at most **16 AP/station pairs** and **16 beacon
contexts**. Under the host ABI, the optional session allocation is **5,576
bytes** and the optional UI allocation is **1,216 bytes**. These are allocation
sizes, not measured device headroom. In-progress exchanges are protected from
replacement within their correlation window. Full tables report replacement
or dropped-new-row counts, and saturation remains explicit. Page reads copy
at most four rows under a mutex; the screen uses two.

C5 callback time stays source-relative. Displayed ages describe delivery to
the S3, with separate clocks for EAPOL and beacon rows. Unknown, rewound or
saturated ages never appear as zero. Historical scan RSSI is not a new sample.

Saved PCAPNG uses C5 interface identity and radiotap channel/RSSI. Without a
verified C5-to-UTC mapping, Simple Packet Blocks carry **no frame timestamps**.
GPS does not silently supply that mapping. Received, dropped and SD-written
counts measure different stages; recording is not claimed lossless.

## Upstream reference and remaining work

The 2026-11-08 `develop` comparison still resolved to
`79cc6bcba1db6679baee1dab2b0c1ad5010e6363`:

- [EAPOL/PMKID menu](https://github.com/justcallmekoko/ESP32Marauder/blob/79cc6bcba1db6679baee1dab2b0c1ad5010e6363/esp32_marauder/MenuFunctions.cpp#L5182) opens targetless monitoring.
- [`sniffpmkid` CLI](https://github.com/justcallmekoko/ESP32Marauder/blob/79cc6bcba1db6679baee1dab2b0c1ad5010e6363/esp32_marauder/CommandLine.cpp#L1045) exposes fixed-channel and selected-list choices.
- [EAPOL callback](https://github.com/justcallmekoko/ESP32Marauder/blob/79cc6bcba1db6679baee1dab2b0c1ad5010e6363/esp32_marauder/WiFiScan.cpp#L10458) gathers phase and beacon context; [passive channel scheduling](https://github.com/justcallmekoko/ESP32Marauder/blob/79cc6bcba1db6679baee1dab2b0c1ad5010e6363/esp32_marauder/WiFiScan.cpp#L13028) uses a two-second interval.

Upstream can enable transmission through the persistent ForcePMKID setting;
this passive monitor does not inherit that behavior. Upstream's per-AP phase
flags do not perform the AP/station/channel correlation described above.
Selected-channel union is a Cerberus optimization; upstream passive selected
capture filters BSSIDs while hopping its general channel set. Cerberus's 42
candidate channels also do not claim equivalence to all 51 upstream entries.

Rev167 source and focused fixtures cover the new scopes, independent evidence,
priority admission and menu lifecycle. The installed checkpoint validation is recorded below. Physical acceptance still needs controlled AP/client
traffic, comparison with an independent capture, both-band reception, PCAP
readback, repeated Stop/Back/next-scan transitions, sleep/wake, SD failures and
sustained loaded memory/stack measurements. Zero exchanges can be normal on
an idle network. Full upstream option/CLI parity remains open.

## Rev167 installed checkpoint

Rev167 is installed from `a1b83a6ca7c9dfd1771d84d3727494dcaff457fc` with C5 capability 87.
All **194 host suites** passed on **1,456 unchanged inputs**;
all five native targets, native placement/link checks and matched packaging
passed. The separate 21-test shard verifier passed. The first full host run
found one root-routing fixture compile failure after adding AUTH; its enum
boundary and idle-entry assertion were updated, then all 194 suites passed.

Package: `cerberus-unchained-0.1.0-dev.rev167-20261108T130124Z`.
Manifest SHA-256: `19e4f983541693664255d2fa552c7c29386a9c1381ba6bb29920846e9291b5c8`.
Application sizes are 2,475,088 S3 bytes, 1,751,488 C5 bytes and 255,280 bridge
bytes. Relative to rev166, the S3 app grows by 7,200 bytes and C5 by 336 bytes.

Fresh rev166 S3/C5 recovery backups matched the installed baseline and were
verified against flash. All nine bridge/C5/S3 image writes verified. Both live
application descriptors match the packaged version, project, SDK, compile
time and full ELF hash. The card mounted; saved home Wi-Fi, WiGLE and WDGWars
profile readiness remained set. Wardrift remained unconfigured. No connection
or upload was requested.

Two idle captures **72.5 seconds apart** showed advancing uptimes and SPI
exchanges, one C5 boot and no additional listed transport errors. Initial
READY timeouts/fallbacks stayed at 4/4; mutex, READY-fall, READY-high and driver
errors stayed zero. Both captures reported 133,820 S3 internal free bytes,
a 90,112-byte largest internal block, 1,426,140 free PSRAM bytes,
10 tasks and 52,740 C5 free bytes. The observed console stack
low-water changed from 2,092 to 924 bytes during diagnostics;
it is not a loaded-stack-margin measurement. These are sampled idle results,
not RF, touchscreen, recording, endurance or performance acceptance.

Focused checks include 10,531 parser checks, the production session fixture,
4,312 EAPOL UI checks and 17,097 SAE UI checks. The renderer previews were
inspected. No monitor was started on the connected device during installation.
The controlled-traffic and navigation acceptance checks above remain open.

## Historical rev164 validation

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

## Historical rev164 installed checkpoint

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
