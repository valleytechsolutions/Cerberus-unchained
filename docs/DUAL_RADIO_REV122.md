# Rev122: integrated GPS Data and received NMEA

Rev122 implements GPS Data and received-NMEA views in the existing main-firmware
Wardriving workflow, with independent metadata freshness and explicit UART
cleanup ownership. S3 application version is `0.1.0-dev.rev122`; C5 capability
revision remains 69. Rev113/C5 revision 68 is the last verified installed
baseline. Implementation, focused checks, independent source/UI review, all
130 host suites, five native builds, linked resource measurements and matched
package verification are complete. This is a verified local candidate, not an
installation or physical receiver acceptance.

## Source and division of work

The feature reference is justcallmekoko/ESP32Marauder develop
[`79cc6bcb`](https://github.com/justcallmekoko/ESP32Marauder/tree/79cc6bcba1db6679baee1dab2b0c1ad5010e6363).
Its `RunGPSInfo()` and `RunGPSNmea()` are in
[`WiFiScan.cpp`](https://github.com/justcallmekoko/ESP32Marauder/blob/79cc6bcba1db6679baee1dab2b0c1ad5010e6363/esp32_marauder/WiFiScan.cpp).
A read-only GitHub compare at 2026-11-01 19:05:21 UTC still reported develop
identical to that revision. The response is retained at
`artifacts/device-rev122/marauder-develop-check.json`.

Marauder's NMEA view combines retained receiver lines with generated GGA/RMC
sentences. Cerberus's new view is explicitly **received NMEA**; it does not
represent synthetic lines as received bytes. The parser, adapter, trace and
UI changes are original Cerberus code, using upstream behavior as a feature
reference. Existing source/license notices are preserved.

The S3 owns the existing UART2 RX reader, parser, UI and SD service. C5 keeps
acquiring Wi-Fi while integrated Wardriving uses S3 BLE and optional GPS. GPS
viewers observe that service and add no UART owner or receiver task. Sending
the same UART bytes through C5 would add a transport/ownership boundary; this
milestone keeps that existing peripheral on its owning processor. It makes
no new measured throughput or concurrent-radio performance claim.

## Implemented behavior

The committed rev121 parser reproduced three defects using synthetic input:
fresh RMC fixes kept old GGA satellite/HDOP values visible, no-fix GGA retained
the preceding metadata, and `strtof` accepted exponent/hexadecimal forms for
NMEA decimal fields. The baseline proof is retained under
`artifacts/device-rev122/prechange-gps-audit`; its assertions deliberately
demonstrate defects and are not candidate correctness checks.

GGA metadata gets its own age and per-field validity. Empty, malformed and
stale metadata cannot appear as a known zero. RMC and unrelated sentences do
not refresh GGA age. A no-fix GGA may still supply valid satellite count or
HDOP while withholding coordinates. HDOP remains a dimensionless value, never
metres of accuracy. Existing fresh-fix/UTC export gates remain separate.

The existing reader is the only line assembler. An optional observer receives
bounded original payload bytes, termination, receipt time and explicit parser
outcome. Raw-byte completeness is independent of validity: a fully retained
bad-checksum or control-containing line remains invalid input. Overflow and
discarded input are labelled; an unterminated partial line is not published.
Receipt time is UART read completion, not GNSS fix time or RF observation time.

The NMEA viewer attaches only to an already-running reader. Its optional
eight-line PSRAM ring and UI copy have no per-line allocations or internal-RAM
fallback. GPS OFF entry allocates neither. Exact observer tokens separate
viewer cleanup from the receiver, and receiver epochs distinguish sessions.
The trace reports parser overflow, framing/control/overflow discards and ring
eviction. These are counts of observed lines and overlapping discard categories,
not a measured total of bytes lost before the parser. Counters saturate; line
sequences and observer tokens are never reused within a boot. At sequence
exhaustion the retained ring remains
readable but stops claiming that it is capturing new rows. The parser continues.
A new UART session advances the receiver epoch and clears the attached ring,
so a single trace bank never combines two receiver sessions.

GPS DATA is reached from the existing Wardriving page and shares its single
GPS ON/OFF setting. Opening a view does not start GPS, radios or logging.
DATA/NMEA views default to hidden location, using a viewer-local visibility
control; the existing address-display setting is a different preference.
Hidden NMEA copies erase every payload, including malformed and unknown lines.
Two rows are shown per page; paging pauses an immutable UI copy and labels its
receipt ages as **ages at copy**. Hiding a paused copy immediately erases all
its raw bytes. Showing location again cannot reconstruct those bytes; resuming
LIVE obtains a new permitted copy. Negative or backward clocks do not create a
freshness claim.

Back to Summary and viewer cleanup on sleep/page leave release only the optional
observer resources. They do not themselves stop Wardriving; STOP WARDRIVE and
the existing outer-page Back behavior keep their separate service controls.
An unresolved observer End retains its exact token and retries at a 250 ms
UI cadence, including after a switch to DATA or a hidden page. Each observer
mutex attempt is bounded at 25 ms; begin can attempt the lock twice around its
allocation. These are configured wait/pacing bounds, not a measured screen
response time or a guarantee that cleanup finishes within either interval.

UART driver allocation, reader activity, startup, requested stop and error are
reported separately. Failed setup/reader creation/deletion does not erase an
installed-driver claim. A reader failure withholds current-valid fields. The
existing Wardriving worker retains STOPPING and retries unresolved GPS cleanup
before releasing its workspace or reporting a terminal session state. Radio
results and files drain before this GPS-only cleanup retry. Successful release
clears the current cleanup error; the adapter exposes the last UART failure
separately as historical evidence.

## Validation and resource evidence

Implementation and independent core, UART/service and production-UI reviews
are complete. The focused checks passed:

| Focused check | Evidence |
| --- | --- |
| GPS core | Existing regressions plus 482 new assertions |
| Actual UART owner and actual parser | 2,343 assertions |
| Actual whole Wardriving service | 7,527 assertions |
| GPS Data/NMEA production UI and routes | 7,539 assertions |
| Existing Wardriving UI | 2,350 assertions |
| GPS OFF helpers and workspace lifetime | Passed |

The UART fixture exercises the actual reader, parser and lifecycle with
scripted SDK boundaries; the service fixture executes the actual worker. UI
checks cover frozen paging, masking, error recovery and observer cleanup, with
synthetic render previews reviewed independently. These are host tests, not
physical UART, receiver or touchscreen acceptance.

The first full run passed 128 of 130 suites. Two older fixture boundaries needed
adaptation: `check_wardrive_recent_live.py` still expected the pre-GPS-view
repaint expression, and `check_wifi_relation_ui.py` treated the newly queried
GPS-view route as unrelated. That attempt and its logs are preserved at
`artifacts/rev122-host-review-evidence`; they are not the final passing receipt.
After focused fixture corrections, the complete matrix passed all 130 suites
with 1,088 fingerprinted inputs unchanged during the run. The authoritative
receipt and logs are retained at `artifacts/rev122-host-evidence`, with archive
verification recorded there.

All five native targets passed: S3 main, onboard C5, temporary S3-to-C5 bridge,
WROOM expansion and XIAO expansion. Their retained report is
`artifacts/rev122-native-evidence/build-report.json`. The linked resource audit
at `artifacts/device-rev122/native-memory-comparison.json` compares these images
with the verified local rev121 candidate, separately from installed rev113.

| Measured resource | Rev121 | Rev122 |
| --- | ---: | ---: |
| S3 fixed internal data + BSS | 173,996 B | 174,124 B (+128 B) |
| C5 fixed internal data + BSS | 83,184 B | 83,184 B |
| GPS parser core | 240 B | 264 B |
| GPS snapshot value | 104 B | 104 B |
| Optional trace ring | Not present | 1,304 B PSRAM |
| Optional UI trace copy | Not present | 1,320 B PSRAM |
| GPS UI state | Not present | 48 B |
| UART state value | Not present | 128 B |
| `gps_reader` selected entry frame | 288 B | 320 B |
| `cerberus_ui_run` selected entry frame | 1,072 B | 1,072 B |
| `wardrive_worker` selected entry frame | 160 B | 160 B |

The optional ring and UI copy total **2,624 bytes of PSRAM** while retained,
excluding allocator overhead and transient failed-attachment candidates.
GPS OFF allocates neither. The existing 1,024-byte UART RX ring and 3,584-byte
reader stack are unchanged. Fixed sections exclude heap allocations; entry
frames are not full call-chain bounds or runtime stack-headroom measurements.
These measurements establish neither worst-case heap availability nor physical
latency, input-loss rate or sustained receiver performance.

| Native application | Bytes |
| --- | ---: |
| S3 main | 1,964,240 |
| Onboard C5 | 1,679,920 |
| Temporary bridge | 255,296 |
| WROOM expansion | 926,400 |
| XIAO expansion | 930,624 |

The matched three-target local package contains nine binary images and is
`dist/cerberus-unchained-0.1.0-dev.rev122-20261101T193628Z`. Verification passed
image hashes, chip headers, partition layouts and image bounds without changing
files; the log is `artifacts/build/rev122-package-verification.log`.
The package manifest SHA-256 is
`2952f80cfc9dbca930715cc256e3a05af314dbe880e3ba59cd977966cb4cdfc6`.
The native audit SHA-256 is
`bcad487c33b2b1195b6db8e3d42ccdc008ce17a11a9c9a37f3f7901e0c0bc999`.
The final source-commit/package binding is recorded separately in
`artifacts/device-rev122/local-milestone.json`. The finalizer creates that
receipt only after checking the clean current commit against the retained
source and artifact hashes. No package installation or external publication
is included.

## Coverage limits

This work remains an integrated Wardriving observer, not standalone GPS
acquisition. Upstream generated-sentence/serial output, richer receiver fields,
PPS, POI, tracker/geofence controls and complete option parity remain open.
No GPS module or six-C5 backboard is connected for physical acceptance. Existing
WiGLE metadata continues to describe GPS at result receipt, not synchronized
RF geolocation. No live coordinates or captured network data are needed by the
synthetic regressions.

The ledger remains at 95 tracked modes and six workstreams: 74 partial, nine
needing review, four hardware-limited and eight pending. No mode is promoted
by this implementation or its focused checks. See
[remaining work](RECEIVE_REMAINING_GAPS.md) and
[GPS logging semantics](WARDRIVE_GPS_LOGGING.md).
