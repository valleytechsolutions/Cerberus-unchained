# Selected AP and station Packet Count

Development work after installed rev132. This is an implementation contract,
not a completed-feature or physical-acceptance claim. The full Marauder port
remains unfinished. Rev132's Mesh save/reopen test remains pending separately.

## Reference and required behavior

The GitHub commit API again resolved Marauder `develop` to
`79cc6bcba1db6679baee1dab2b0c1ad5010e6363` on 2026-11-02. The clean local reference
matches. `WIFI_SCAN_PACKET_RATE` is Packet Count: menu at
`MenuFunctions.cpp:5179`, CLI at `CommandLine.cpp:842`, setup at
`WiFiScan.cpp:5062`, receive handler at `10344`, display at `11422`, channel
controls at `11456`, and main-loop hopping at `12896`. `Buffer.cpp:61,143`
controls optional saved packets; the CLI also supports serial capture.

Upstream counts management/data frames against selected Addr1/Addr2 addresses.
It updates only the first AP match before considering stations, and counters
stop at 65,530. These are cumulative observations, not measured packets per
second. General hopping, manual channel changes, Stop, optional matched-packet
recording and prior AP/station selection are real requirements. A channel graph
or counter CSV does not substitute for the selected-target or PCAP workflows.

Cerberus will count each selected target independently, once per callback, and
retain a separate once-per-callback union total. An AP must not consume its
station's observations first. Counters are bounded and expose saturation.
No retry/sequence de-duplication or unique-packet claim is implied.

## Data and identity contract

- At most 16 immutable targets: AP/STA role, complete unicast MAC and initial
  channel hint. The role plus address is the target key; the channel hint and
  discovery's SSID/BSSID/source describe selection provenance, not association
  or authenticated identity. Re-selecting the same key must not duplicate it.
- Immediate transmitter (TA/Addr2) and receiver (RA/Addr1) matches define the
  primary observation count, regardless of which selected target was listed
  first. Transmitted/addressed totals may overlap. BSSID-context observations
  are a separate value and cannot silently become direct participation.
- Validate management/data version, SDK class, DS fields, delivered length and
  complete QoS/HT/four-address header before reading roles. Payload stays
  opaque. Unsupported frame forms are explicit; control-frame decoding is not
  required by the upstream management/data filter.
- RSSI describes the transmitter only. Addressed-to traffic must not update a
  selected receiver's signal. Last participation and last transmitter signal
  therefore have separate timestamps. Source clocks remain separate.
- Keep retry, protected and fragmented observations as subsets. Saturation,
  invalid clocks, rejected callbacks and unknown signal remain visible. Do not
  add an apparent packets-per-second value by dividing by UI poll time.

## Receiver division and lifecycle

Provide fixed channel, selected-channel sweep and all-supported-channel sweep.
The actual channel plan and measured software receive windows must be visible;
they do not prove continuous RF coverage or quantify unseen SDK/RF loss.
Manual retuning must preserve prior totals through explicit segments and show
the interruption; it cannot reset counts or reinterpret old exposure silently.

Partition channels without overlap. Only C5 can receive 5 GHz. S3 can receive
2.4 GHz while C5 handles 5 GHz, and multiple selected 2.4 GHz channels can be
distributed when that improves coverage. A single channel needs one receiver;
S3 presentation/analysis/storage is useful work without a redundant receiver.
Admit the optional S3 receiver first. Only a definite clean rejection permits
C5-only fallback; a retained or ambiguous owner cannot be bypassed. No automatic
mid-run reassignment may double-count observations.

Use a new passive S3 Wi-Fi owner and exact local session, with all existing BLE
admission race checks extended. Do not reuse the TX-corroboration receiver or the
BLE MAC Monitor. C5 reuses its existing Wi-Fi worker with an optional context.
All radio reservations, including staging and failed cleanup, must be visible
to Wi-Fi/BLE/802.15.4 admission and status.

C5 capability 74 uses new commands from 161: exact-session INFO, BEGIN, indexed
TARGET, START, STATUS, STOP, RELEASE and pinned reads. BEGIN reserves an immutable
target count, channel mask, dwell and recording option; conflicting uploads
fail, and START requires a complete target set. Staging expires without RF.
Lost Start replies resolve through exact STATUS, never a second unbound Start.
Stop, Back, sleep and hidden-page cleanup retain both owners until release.
Final reporting requires stopped receivers and validated final projections.

## Presentation and recording

Enter through the frozen AP detail's Packet Count action, or an observed-client
detail's Count This Station action. Keep a separate passive target basket;
archive save checkboxes and action-oriented `wifi_targets` are not this basket.
Four visible rows, a single Start/Stop surface and ordinary Back routes should
cover selection and live totals without another Scan submenu. Opening the page
must not start reception. S3 and C5 activity, errors and source coverage remain
separate, even when their target totals are presented together.

Count-only mode retains no raw payload queue. Optional matched-frame recording
must preserve both source identity and honest truncation/drop counts. Existing
PCAPNG code uses one radio per file and omits untrusted absolute timestamps;
dual recording must preserve those semantics, preferably with separate named
S3/C5 files under one exact recording lease. Queue pressure or SD failure must
not invalidate independently collected counters or silently stop counting.
Counter CSV export is additional, not a replacement for packet recording.
Serial capture is part of the upstream parity inventory and must remain
explicitly pending until an actual supported transport is implemented.

## Acceptance

Require production-bound parser/model, C5 service/transport, S3 owner/receiver,
dual controller, UI and storage checks. Include overlapping AP/STA targets,
all supported header forms and truncations, source-only RSSI, saturation,
clock faults, failed retunes, enable/callback/Stop races, uncertain commands,
stale pins/segments, clean versus held fallback, and recording drops independent
of counters. Check actual AP/client navigation, privacy and 320x480 rendering.
Measure compiled allocation/stack changes and build the full target matrix.
Physical receive/count accuracy, dual-radio transitions, SD recording, repeated
Stop/Back/rescan and loaded endurance remain separate acceptance requirements.
