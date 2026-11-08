# Passive SAE monitor

Rev165 adds live SAE observations and planned channel scanning to the existing
capture service. Open **Scan -> Wi-Fi -> TOOLS -> SAE** without first selecting
an AP. The **SAE** button is in the Wi-Fi tools footer. For a selected AP, open
**CAPTURE THIS AP** and cycle the existing selector to **SAE MONITOR**. Both
routes open the same page; neither starts RF reception until **START**.

| Scope | Receive plan |
| --- | --- |
| ALL / 2.4 + 5 GHZ | All 42 candidate channels, all BSSIDs |
| 2.4 GHZ | Channels 1-14, all BSSIDs |
| 5 GHZ | The 28 canonical 5 GHz candidates, all BSSIDs |
| FIXED CHANNEL | One chosen canonical channel, all BSSIDs |
| SELECTED APS | Up to 32 exact BSSIDs and the union of their selected channel hints |
| THIS AP | The copied AP BSSID and channel; available from the AP route |

**CHOOSE APS** reuses the AP picker and returns to the monitor. The identities
and channel plan are copied at Start, so later scan-list changes cannot alter
a run. Selected channels are historical hints, not a guarantee that an AP
remains there. Options changed after Stop apply to the next run.

**SAVE PCAP OFF** is the default and needs no SD card. **SUMMARY** shows status
categories, receive/drop counts, channel coverage and storage counts when
enabled. **ROWS** displays two address pairs at a time: BSSID, transmitter,
receiver, status, conditional group, lengths, channel, RSSI and S3 delivery
age. Privacy masking applies to displayed addresses. **STOP** retains results;
**RESTART** begins a new report. Back, hidden navigation and sleep cancel
pending Starts and drain the owned session. Receiver/file cleanup must finish
before release or navigation. Stop remains pending through a failed attempt.

## Evidence and its limits

The parser accepts complete, CRC-verified frames up to 4,096 bytes and checks
unfragmented, unprotected authentication frames with algorithm 3 and
transaction 1. Full little-endian fields are checked, including their high
bytes. Unsupported versions, DS/order flags, fragments, invalid addresses and
wrong scope are excluded. The family filter now admits a complete **30-byte
authentication header**: error responses can have no group or other body.
A short commit body is retained as a header observation marked short.

Counts distinguish status 0, H2E (126), SAE-PK (127), token requests (76),
unsupported-group responses (77) and other statuses. These are observed
header claims, not valid commits or successful authentications. Transaction-2
Confirm frames are outside this commit-family filter; there is no
authentication-completion counter.

A group is read only for a status layout that supplies one and a body of at
least two bytes. Groups 19, 20 and 21 have known scalar/element length bounds;
shorter bodies are marked short. Unknown groups and extra data remain opaque.
The parser does not validate scalars, curve points, tokens, password identifiers
or extension sequences. Legacy tokens can precede scalar/element data while
H2E extensions follow it; body length cannot distinguish those layouts. No
token length is guessed or extension-fragment reassembly claimed.

The report retains only public fixed metadata and counters, never scalar,
element, token, password-identifier or full payload bytes. Optional PCAP
saving retains received frames. The monitor does not transmit SAE messages,
join an AP, use credentials or authenticate the physical source of a frame.

There are **16 stable rows**, keyed by `(BSSID, transmitter, receiver)`;
opposite directions remain separate. At capacity, new pairs increment an
explicit pair-limit count while existing rows keep updating. Rows are not
silently replaced or repeatedly appended. A newly received frame with the
802.11 Retry bit is a separate RF observation. Repeated SPI capture sequences
never count or refresh a row. Counters saturate with explicit display limits.

## Radio and channel ownership

The **C5 receives and filters RF traffic**. The **S3 verifies transfer,
interprets evidence, maintains the report and optionally writes SD records**.
The existing worker, five-second renewable lease and two-slot receive ring
are reused. This mode adds no local S3 RF worker or duplicate receive stream.

Both fixed and planned monitor entries require **C5 capability 86**. The
older generic fixed-capture API keeps its existing compatibility rules. Plans
use the canonical Packet Count channel mapping:

- Bits 0-13: channels 1-14.
- Bits 14-21: 36, 40, 44, 48, 52, 56, 60, 64.
- Bits 22-33: 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144.
- Bits 34-41: 149, 153, 157, 161, 165, 169, 173, 177.

These are **candidate channels**. Country configuration and successful SDK
tuning determine coverage. Successful channels receive a full 250 ms dwell;
delayed worker execution does not fabricate missed dwells. Rejected channels
remain distinguishable in attempted/applied masks. A pass with no successful
channel stops with an error. A one-channel plan receives continuously and
counts completed dwell intervals without repeatedly retuning that channel.

Retuning closes admission, disables reception and drains admitted callbacks
before changing band/channel. A new private admission token opens only after
successful arming. Old callbacks cannot acquire a new channel or capture
identity. Pinned records survive retunes with their original channel, source
time, length and CRC. BSSID selection matches address 3 in both directions.

The current channel is zero when reception is not admitted, including after
Stop; it is not a retained last-channel sample. Masks describe the current or
stopped partial sweep; completed-sweep counts saturate. Telemetry failures
remain explicit. Stop restores the prior band/channel configuration for a
hopping session. Failed disable or restore retains radio ownership.

## Transport, memory and saving

Protocol version 2 and existing capture START/STATUS/PEEK/READ/ACK/STOP layouts
remain in use. Capability 86 adds:

| Command | ID | Request / response |
| --- | --- | --- |
| WIFI_CAPTURE_PLAN | 203 | 16-byte generation/mask/dwell/count plan; 22-byte status. An all-zero request queries staging. |
| WIFI_CAPTURE_TARGET | 204 | 11-byte generation/index/BSSID; 22-byte status. |
| WIFI_CAPTURE_HOP_STATUS | 205 | 4-byte generation; 22-byte masks/sweeps, or an 8-byte exact-generation error. |

Planned START uses **HOP | PLAN**, channel zero, SAE mode and no separate
single-BSSID flag. Zero targets means all BSSIDs. Identical staging/target
retries are idempotent; conflicting writes fail. Staging owns no RF. New
capture generations exceed local reserved, remote capture and remote staged
generations; exhaustion fails closed. A lost Start reply is reconciled
through status. Stop and final drain remain bound to that exact generation.

The shared model is **1,848 bytes under the host ABI**. The complete optional
S3 analysis store is **2,352 bytes**, including retained status and delivery
anchors. The optional UI store is **1,000 bytes**, including frozen scope and
two copied rows. The capture worker separately owns a plan and frame buffer,
plus a PCAP block only when saving. These are host allocation sizes, not
measured device headroom. Restart allocates a replacement analysis store
before retiring the stopped report; failed starts preserve that report.

C5 callback time remains source-relative. Displayed **S3 delivery age** never
subtracts C5 time from the S3 clock or labels it UTC. Unknown or saturated age
is not zero. Scan-list RSSI never supplies a new observation. Page copies are
bounded to four rows under the session mutex; the UI requests two.

Saved PCAPNG files use C5 interface identity and radiotap channel/RSSI.
Without a verified C5-to-UTC mapping, Simple Packet Blocks have **no frame
timestamps**. The UI states this when saving; GPS presence does not change
that contract. C5 accepted/dropped, interpreted and SD-written counts remain
separate. File publication waits for close, and storage errors do not claim
that every observation was saved.

## Upstream reference and validation

The milestone's `develop` check on 2026-11-08 still resolved to
`79cc6bcba1db6679baee1dab2b0c1ad5010e6363`. Reachable passive upstream paths are
the [SAE Commit scan menu](https://github.com/justcallmekoko/ESP32Marauder/blob/79cc6bcba1db6679baee1dab2b0c1ad5010e6363/esp32_marauder/MenuFunctions.cpp#L5251),
[SAE sniff CLI handler](https://github.com/justcallmekoko/ESP32Marauder/blob/79cc6bcba1db6679baee1dab2b0c1ad5010e6363/esp32_marauder/CommandLine.cpp#L1023),
[`RunSAEScan`](https://github.com/justcallmekoko/ESP32Marauder/blob/79cc6bcba1db6679baee1dab2b0c1ad5010e6363/esp32_marauder/WiFiScan.cpp#L6995),
and [receive callback](https://github.com/justcallmekoko/ESP32Marauder/blob/79cc6bcba1db6679baee1dab2b0c1ad5010e6363/esp32_marauder/WiFiScan.cpp#L9356).
Upstream's dual-band array has 51 candidates, including channel 32 and
68-96 in steps of four. Cerberus uses its existing 42-channel map. This is an
explicit coverage difference, not a claim to support every upstream candidate.
The passive monitor does not implement the separate SAE Commit Flood action
or establish full upstream feature parity.

Fixed-field and status/body distinctions were checked against the installed
ESP-IDF 6.0.1 primary sources in
`components/wpa_supplicant/src/common/sae.c`, `ieee802_11_defs.h`, and
`src/ap/ieee802_11.c`. The model does not reuse a transmit serializer to infer
received-frame validity.

Focused production fixtures pass for SAE analysis, the S3 session and the
updated capture filter. They cover truncated/unsupported input, conditional
fields, opaque-payload non-retention, exact scope, sequence/time rejection,
row limits, saturation, old firmware, allocation failures, retained errors,
cleanup ownership and delivery ages. The UI fixture exercises the real
renderer and controls, both entry routes, copied AP plans, optional saving
and Stop/Back/hidden cleanup. Planned capture/link/radio fixtures separately
exercise staging, tuning and transfer behavior.

At this checkpoint, **the full host matrix, native builds, matched package
and installation have not yet been established for rev165**. Physical
acceptance is also pending: controlled WPA3 traffic, independent capture
comparison, selected/multi-AP scoping, both bands, PCAP readback,
Stop/Back/next-scan transitions, sleep, storage failures and sustained loaded
memory/stack measurements. Zero observations without a relevant connection
is inconclusive. Source and fixture results do not establish RF coverage or
touchscreen acceptance.
