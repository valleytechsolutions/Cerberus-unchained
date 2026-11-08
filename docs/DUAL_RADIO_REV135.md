# Rev135: C5 callback payload compatibility

Rev134 was the installed baseline for this source checkpoint. Its new diagnostic
identified the owner's zero-total report at the native receive-metadata gate:

| Live session | Channel | Callbacks | Invalid metadata | Valid frames | Selected matches |
| --- | ---: | ---: | ---: | ---: | ---: |
| First | 48 (5 GHz) | 1,926 | 1,926 | 0 | 0 |
| Second | 10 (2.4 GHz) | 5,267 | 5,267 | 0 | 0 |

These were different exact sessions after the owner changed the selection.
Both had zero malformed, unsupported, outside-window and invalid-clock counts,
and no reported source or controller error. The first sample's UI tick gap was
19 ms, with a 140 ms maximum. Thus the conditional slow-tick defect fixed in
rev134 is not established as the cause of these captures. Receive windows and
counter readout advanced; all callbacks failed before address matching.

## Callback contract and correction

The installed ESP-IDF 6.0.1 public callback type defines the data/management
payload extent through `rx_ctrl.sig_len`. Its C5-supported `simple_sniffer`
example rejects `WIFI_PKT_MISC` and nonzero receive status, then consumes
`sig_len - 4` bytes, excluding FCS. The native HE metadata also contains
`dump_len`, but equality with the callback extent is not a requirement of that
public example. Sources:

- [Versioned callback type](https://github.com/espressif/esp-idf/blob/v6.0.1/components/esp_wifi/include/local/esp_wifi_types_native.h#L109-L113).
- [Versioned sniffer implementation](https://github.com/espressif/esp-idf/blob/v6.0.1/examples/network/simple_sniffer/main/cmd_sniffer.c).

Count had additionally required `dump_len == sig_len - 4`; Probe Sniff had
required a nonzero dump length and the same equality. Rev135 removes those
extra conditions and uses the documented callback extent. Count admits only
management/data types before payload access. Probe Sniff retains management
type admission. Receive errors and short lengths remain rejected; strict
header/element parsing and raw-recording limits remain in force. Zero-payload
MISC callbacks must never reach a header read or raw copy.

The existing diagnostic establishes the rejection stage, not the individual
physical values of `rx_state` and `dump_len`. The source correction is grounded
in the same-version SDK contract. A fresh device test must establish whether
valid frames and selected matches now advance. No new wire projection, radio
command, task or per-callback allocation is introduced. Capability 74 remains
the compatible protocol version.

## Processor roles and display

A single-channel plan uses C5 reception. The S3 panel now describes its work
positively: PROCESSING, COUNTS + DISPLAY, RECEIVER: C5. The S3 cannot receive
5 GHz; a multi-channel plan can assign its supported 2.4 GHz channels separately
from C5 coverage. Both processors contribute without presenting duplicated
receptions as unique packets.

When a validated publication has metadata rejections and no valid frames, the
source card reports METADATA REJECTED. Unknown counters do not trigger the
warning. Fault, Stop, error and setup states keep their priority; readout
progress and recording-loss information remain visible.

## Validation boundary

All 158 host suites and five native targets passed on 1,234 unchanged inputs.
Focused native Count checks passed 10,889 assertions; Probe checks passed 657
model/controller, 6,593 native and 15,429 actual C5/S3 bridge assertions. Count
controller and UI checks passed 29,183 and 1,576,525 assertions respectively.
These exercise old-reject/new-accept callback cases, receive errors, short
lengths, inaccessible MISC payload and raw-recording extent. Host structure
sizes are not substituted for linked native measurements.

The exact linked audit found no fixed RAM growth from rev134: S3 remains at
170,124 bytes and C5 at 83,464 bytes. Count contexts remain 2,112 bytes on S3
and 2,152 bytes on C5. Optional recording allocations and task stacks are
unchanged. These measurements do not establish loaded runtime headroom.

The matched package is
`dist/cerberus-unchained-0.1.0-dev.rev135-20261103T013007Z`, built from
`d08bd149a83e93e29ec2aca835466d8b51d00ad4`. Application sizes are 2,117,152
bytes for S3, 1,721,872 for C5 and 255,296 for the temporary bridge. All 22
package checksum entries and the offline installation preflight passed.
The retained local milestone receipt SHA-256 is
`f04066bc31a972280b30bc82ec014064c75b1c3eed46c8aa34d1a8b1c07ab6c4`.

Rev135 is now installed. Fresh backups of the installed rev134 prefixes were
verified before writing the matched bridge/C5/S3 images; all nine programmed
image hashes verified. Two sampled idle boot checks, 79.24 seconds apart,
showed both chips online, mounted SD, stable memory and advancing SPI exchanges
without additional listed SPI errors or a changed C5 boot count. S3 internal
free memory was 136,720 bytes with a 94,208-byte largest block; PSRAM free was
1,427,552 bytes. The C5 free-memory snapshot was 53,572 bytes. The observed
console stack low-water value reached 1,280 bytes; this is not a worst-case
stack bound. Initial READY timeouts and legacy fallbacks remained at four
each, with no increase across the samples.

Installation evidence is retained in
`artifacts/device-rev135/installation-receipt.json`, SHA-256
`72baeb34b31610470d65131e49f06355c9dfce8ed6ac9404ebdcbc5041dbcf61`.
This receipt establishes paired installation and sampled boot state, not
physical feature acceptance.

## Live Packet Count result

After a fresh Wi-Fi scan, the owner confirmed that TOTAL or TX increases with
PCAP OFF. Two captures from the same accepted and published C5 session on
channel 11 (2.4 GHz) showed:

| Capture | Valid frames | Selected matches | Metadata rejected | Published revision |
| --- | ---: | ---: | ---: | ---: |
| First | 873 | 262 | 0 | 875 |
| Second | 2,748 | 854 | 0 | 2,750 |

Both had zero malformed, unsupported, outside-window and invalid-clock frames,
and no published, source, controller or UI error. This establishes that valid
frame parsing, selected-address matching and repeated counter readout now work
in this tested 2.4 GHz session. Captures are retained as
`artifacts/device-rev135/packet-count-check-1` and `packet-count-check-2`
(`.txt` plus hashed `.json` metadata). Counts are callback observations, not
unique packets or measured RF loss. An AP's beacons can increase TOTAL and TX
without increasing RX.

The owner then confirmed normal Stop. The retained `packet-count-stopped`
capture shows 4,306 valid frames and 1,303 selected matches, zero metadata
rejections and no reported error. Its final publication is valid and frozen,
the requested session is zero, the C5 controller is idle and no recording
binding remains. Back and the following Wi-Fi scan are still being checked.

The owner reported that Back and another scan worked. The follow-up capture
instead found a second running Count session on channel 11 with 2,184 valid
frames and 463 matches, zero metadata rejections and no reported error. The
retained Wi-Fi summary still described the first completed scan (one C5 scan,
21 APs, split S3/C5 coverage). This confirms repeat Count reception but does
not independently establish a new scan after release. A specific Scan Again
check, leaving the AP list open, has been requested.

The subsequent explicit Scan Again check passed. In
`artifacts/device-rev135/packet-count-fresh-rescan`, C5 successful scans and
the dual-scan generation both advanced from one to two, failed scans remained
zero, and 19 APs were retained (10 from S3 and nine from C5). The completed scan
used split coverage without fallback or source/UI errors, and Packet Count's
workspace was absent. This independently corroborates the owner's final
Back/rescan report; the earlier ambiguous capture is retained above.

The 5 GHz band, optional recording and Probe Sniff need their own acceptance.
Full Marauder parity, serial capture, Mesh Save/Open and
Locate reception, GPS and expansion-board validation remain unfinished.
