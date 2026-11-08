# Mesh Detection: requested scope and implementation order

The owner requested Scan → Mesh Detection after the current reliability fixes,
using CYM as a workflow reference. This extends the ongoing Marauder port; it
does not replace that objective. GPS, the ATGM336H and the six-C5 expansion board
are deferred. The first implementation must work with the onboard S3 and C5.
This document is a plan and source inventory, not an implemented-feature claim.

The receive prerequisite now has a verified local
[rev129 candidate](DUAL_RADIO_REV129.md): 139 host suites and five native targets
pass, including the sustained 802.15.4 regression. Physical reception acceptance
remains open. Mesh Detection itself is not in the rev129 candidate. The
[rev130 implementation](DUAL_RADIO_REV130.md) now contains typed MAC
parsing, exact-session C5 transport and the S3 observation model/controller.
Interface and SD report integration pass the complete local host/native matrix
and package checks. The paired rev130 update passed sampled idle boot checks
before the rev131 update below; Mesh feature acceptance remains pending.
The [rev131 implementation](DUAL_RADIO_REV131.md) corrects menu-first
navigation and adds passive selected-source tracking with stopped CSV export.
All 150 host suites, five native targets and matched-package checks pass. The
paired rev131 update is installed and passes two sampled idle boot checks;
the owner has accepted menu-first navigation and Back. Real reception, Locate
and SD report acceptance remain pending. Protocol discovery, raw capture and historical map reopening remain
later work; this plan does not
imply that the complete CYM workflow is ported.

## Reference baseline

The supplied [CYM SD asset repository](https://github.com/JimGat/CYM-SD-Assets)
links to the firmware at [JimGat/CYM](https://github.com/JimGat/CYM). The inspected
firmware revision is `59a86a86a7bfb35635ebd3a8cac9165bdc6ca238`, checked on
2026-11-02. Relevant references are:

- Zigbee Scout in `ESP32C5/main/main.c`: passive PAN/address observations,
  details, channel location and CSV/PCAP saving.
- [IOT/OT Air Survey](https://github.com/JimGat/CYM/blob/59a86a86a7bfb35635ebd3a8cac9165bdc6ca238/ESP32C5/docs/wiki-iot-ot-air-survey.md):
  radio profiles, typed Wi-Fi/BLE/ESP-NOW/802.15.4 observations, JSONL and optional
  PCAPNG. Its own Thread detection scope is partial.
- Matter commissioning BLE hints and Meshtastic BLE service recognition.
  Neither proves an operational Matter fabric or reception of LoRa mesh traffic.

No project-wide firmware license grant was found in the inspected CYM tree.
Use its workflows as a reference for an original Cerberus implementation;
component-specific licenses do not grant rights to unrelated application code.
Preserve the existing Cerberus and third-party notices. The supplied SD assets
retain their own separate provenance and license requirements.

Source behavior takes precedence over broad README claims. The inspected Scout
uses a 250 ms dwell and no-FCS PCAP link type 230. Its locator measures queued
channel traffic without consistently matching the selected PAN. Cerberus must
bind Locate to the selected network/address. CYM's default observation store is
1 MiB of PSRAM; this is unsuitable as an onboard C5 allocation.

## Current Cerberus foundation and prerequisites

`c5_firmware/main/ieee802154_scanner.c` already provides Energy, Beacon and
Activity sweeps on channels 11–26. S3 preserves these in Mesh Detection's Tools
menu, including assessment and diagnostic export. Commands 24–32 are existing wire interfaces.
IEEE 802.15.4 is enabled in the current C5 SDK configuration; OpenThread is not.

Receive prerequisites and design requirements:

1. The rev128 Linux fixture declaration failure and Windows CI timeout are
   resolved in the published CI follow-up: Ubuntu, four Windows shards and the
   coverage aggregate passed. Complete coverage is retained.
2. Rev129 corrects receive continuity: the prior Beacon/Activity worker started
   receive once per channel without enabling receive-when-idle. The actual IDF
   6.0.1 driver returns idle after a frame with its default setting. The actual
   worker fixture now covers multiple frames in one dwell, unrelated traffic
   before a valid beacon, passive ACK behavior, channel boundaries and cleanup.
3. Design explicit ownership, Stop and loss accounting for the new mode.
   Existing finite sweeps drain naturally on Back/sleep; they have no Stop API.
   Existing Beacon rows merge by network and cannot stand in for node records.
   ISR queue overflow and retained-bank capacity loss need explicit counters.

A failed SDK disable return is not proof that RF remained active: the installed
SDK disables RF/baseband before returning MAC teardown status. Retained logical
cleanup and reinitialization still need review; do not label an unproven radio
collision as a reproduced defect.

## Processor roles and radio scheduling

C5 owns bounded 802.15.4 reception, channel control, callback lifetime and the
radio reservation. S3 owns validation, protocol parsing, bounded network/node/
edge models, frozen selection, rendering and asynchronous SD persistence. Reuse
the onboard SPI/READY transport and the existing C5 worker where practical.

C5 has one RF path, including 5 GHz Wi-Fi. Its simultaneous BLE-scan and
Thread/Zigbee-scan combination is unsupported in the
[Espressif coexistence matrix](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c5/api-guides/coexist.html).
Dedicate C5 to 802.15.4 during this mode and evaluate complementary S3 BLE or
Wi-Fi observations separately. Do not promise simultaneous full coverage from
radio modes that share a physical receiver.

Matter is an application protocol over Thread, Wi-Fi or Ethernet, with BLE
commonly used for commissioning. S3 can contribute appropriate BLE/Wi-Fi
observations; detecting a commissioning advertisement does not establish
Thread membership or access to a Matter fabric. See
[Espressif's Matter architecture](https://docs.espressif.com/projects/esp-matter/en/latest/esp32c5/introduction.html)
and [OpenThread discovery/attachment](https://openthread.io/guides/thread-primer/network-discovery).

## Required user workflow

- **Survey:** explicit Start/Stop, fixed or selected-channel sweep, actual dwell
  and coverage, source state, malformed/unsupported counts and visible loss.
- **Networks:** PAN and extended PAN where observed, channel, protocol evidence,
  RSSI/LQI and age. PAN ID alone must not merge unrelated channels or sessions.
- **Devices:** typed 16-bit/64-bit addresses, network context, first/last sighting
  and counts. Preserve complete EUI-64 values and unknown fields.
- **Map:** directed edges labelled traffic observed, with frame class, count,
  source time and supporting evidence. Broadcast is not a confirmed peer link.
  Parent/child/router/coordinator roles need appropriate decoded evidence.
- **Inspect/Locate:** frozen raw and decoded details, channel lock and
  selected-network/address signal tracking. RSSI is not distance or bearing.
- **Save/Open/Export:** bounded historical inventory/map and optional raw capture,
  preserving session, coverage, loss, clock scope and parser version. Saved
  observations must not silently become live connection targets.
- **Complementary evidence:** source-labelled S3 observations. Cross-radio
  identities remain separate unless explicit evidence supports linking them.

The UI should consolidate the relevant Radio Sense tools into a clear Mesh
Detection workflow without duplicate menu entries. Existing functionality must
remain reachable throughout the transition. A disabled label is not a port.

## Evidence, memory and acceptance

Begin with an explicitly bounded optional C5 frame queue and S3 PSRAM stores,
then measure actual allocation and task-stack costs. Callback buffers are
borrowed in ISR context and must be copied before return to the SDK. The SDK's
FCS positions contain RSSI/LQI metadata rather than preserved RF FCS; exports
must label that accurately. Queue drops, unsupported frames and graph retention
loss must remain visible. SPI throughput is not proof of lossless capture.

Use exact session identities, replay-safe control, conditional Stop, immutable
read pins and retained cleanup. Expiry, reset, failed transfer, Back and sleep
must not leave untracked ownership or promote stale observations.

Authenticated joining, commissioned Matter access and authoritative routing
tables require a separate credential/stack design and interoperability tests.
They cannot be inferred from passive captures. LoRa traffic reception needs
additional radio hardware and is outside the onboard pair's receive capability.

Acceptance requires parser/bounds tests, typed-address/PAN collision cases,
encrypted payload remaining unknown, sustained receive and overflow handling,
callback/Stop overlap, lost replies/reboot, frozen selection, and faithful
save/reopen. Physical tests must separately demonstrate real Zigbee/Thread
traffic and repeatable handoffs to Wi-Fi/BLE. No GPS is required for this phase.

## First implementation contract

The first useful slice is Networks / Addresses / Observed Traffic, frozen
inspection and SD reports. Native map reopening follows the report slice; CSV
text viewing alone is not a reconstructed historical map. C5 copies bounded
raw observations; S3 parses and builds the model. Initial caps are 16 optional C5 queue slots, and S3
PSRAM stores for 16 PAN contexts, 64 addresses, 64 edges and 64 witnesses. These
are design bounds, not native allocation measurements. A destination is only an
addressed endpoint until heard as a source; RSSI/LQI belong to the received
transmitter. Broadcast and addressless ACKs must not create invented peer links.

The existing SPI payload is 22 bytes. The implemented session64/sequence32/part8
envelope leaves nine bytes per read. A 139-byte raw observation needs 16
reads plus Peek and ACK, before status/retry overhead. Keep Stop/lease work ahead
of optional readout and expose queue/model losses. Measure actual throughput
before selecting a compact projection or promising coverage. Preserve one
immutable frame until exact acknowledgement, and do not count retries twice.

Protocol references for the independent MAC parser are ESP-IDF v6.0.1 and its
OpenThread submodule `9b887f6bd1437170fc9bcb726f32185ccb3cb841`. In 2015 frames,
extended-to-extended addressing does not justify the legacy compressed-PAN
equality shortcut. Preserve explicit, compressed-same and unknown PAN origins;
bound auxiliary security, MIC and Header IE fields, and leave encrypted payload
opaque. Parsing a clear header does not establish authentication or Thread/
Matter classification. Future code must retain appropriate source attribution
if any licensed implementation is adapted rather than independently written.
