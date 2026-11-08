# Rev137: streaming dual-radio Wardriving and configurable GNSS

This milestone replaces the integrated Wardriving scan-result polling path
with passive receive workers and bounded observation queues on both chips.
It also makes GNSS status available before recording and adds a single
Settings → GPS Receiver page. The public release and broad Marauder parity
remain separate work; this is a development candidate.

## Radio roles

| Mode | S3 | Onboard C5 |
| --- | --- | --- |
| Wi-Fi | Continuous 2.4 GHz channel sweeps | Continuous 5 GHz channel sweeps |
| Bluetooth | Continuous passive 1M BLE scanning | Passive 1M and coded-PHY BLE scanning |
| Wi-Fi + Bluetooth | A 2.4 GHz sweep followed by a 1.6-second BLE window | Continuous 5 GHz channel sweeps |

The S3 has one shared RF receiver. Combined mode therefore has explicit RF
windows; it cannot receive Wi-Fi and Bluetooth at the same instant. There is
no deliberate idle interval between sweeps. Channel changes, controller
transitions and task scheduling still take time. At least one scheduler tick
is yielded during a receive dwell so GPS, SPI and SD work cannot be starved
by a nominal 5 ms delay rounding to zero at the configured 100 Hz tick rate.

Wi-Fi reception captures beacons and observed probe responses without sending
probes or associating. S3 visits channels 1–11; C5 visits the configured 5 GHz
channel list with 120 ms dwell. Unsupported channels are skipped and counted;
the driver remains responsible for regulatory support. C5 explicitly selects
the 5 GHz band at entry and restores its previous band mode, channel and
receive filter before releasing ownership. BLE duplicate filtering
is disabled. Incomplete extended advertising fragments and malformed frames
are rejected rather than turned into invented observations.

The S3 queue holds 512 observations in PSRAM; C5 holds 128 in internal RAM.
RF callbacks only validate/decode and enqueue. The S3 writer alternates bounded
batches from both queues, writes CSV, and services GPS/expansion status while
RF reception continues independently. Queue overflow and high-water counters
are visible in diagnostics. Bounded buffers cannot guarantee lossless capture
under arbitrary traffic or a stalled SD card.

Every accepted sighting is a separate observation, including identical MAC,
SSID, RSSI and manufacturer fields. The Recent view also shows repeated
observations. Exact transport sequence ACKs prevent a lost SPI response from
appending a second copy of the *same* queued event; this is transport retry
handling, not RF identity deduplication. A failed SD append is not replayed
because partial writes are ambiguous. Such a session reports an SD error and
remaining queued records may be discarded during cleanup.

Stop first quiesces both receivers, drains accepted queues, closes and syncs
the files, then releases ownership. Failed cleanup retains its owner for retry.
A ten-second host-contact lease stops a receiver if its host disappears.
The onboard wire extension requires C5 firmware capability 75; install a
matched S3/C5 pair.
The temporary USB flashing bridge retains Bluetooth-disabled stubs and does
not link the receive engine's NimBLE implementation.

## GNSS settings and status

The default receiver configuration is AUTO baud, S3 RX GPIO 1, S3 TX assignment
GPIO 2 and PPS assignment GPIO 46. Receiver TX connects to S3 RX, and vice
versa. The LC29H-AA preset listens at 115200 baud; ATGM336H uses 9600 baud.
Manual mode selects a supported baud and board-free pin assignments. Presets
preserve the selected pins. Reserved board pins and conflicting assignments
are rejected. Configuration is stored in its own validated `gps_v1` NVS blob;
it is not added to the older settings-backup wire format.

The current adapter receives NMEA only. It does not transmit receiver commands
on GPIO 2, and GPIO 46 is reserved for future PPS use rather than clock
disciplining. The GPS settings screen identifies PPS as reserved.

AUTO starts at 115200, then tries 9600, 38400, 57600, 19200, 4800, 230400,
460800 and 921600 baud. Two checksum-valid sentences establish the baud;
ten seconds without valid NMEA restarts detection. UART input buffering is
4096 bytes and the reader processes bounded 256-byte chunks. The Wardriving
page and GPS settings can preview the receiver without starting a capture.
Preview and recording share one UART reader, so Start preserves an acquired
fix and leaving a preview does not stop an active recording's GPS.

The dashboard exposes receiver/baud detection, fix state and satellite count.
The existing GPS Data view provides detailed NMEA inspection. The read-only
`gps` console command reports health, baud, assignments and fix age without
printing coordinates. GGA altitude is retained only when its fresh timestamp
matches the RMC epoch, including fractional seconds; sentence order no longer
silently loses same-epoch altitude.

## Files and location integrity

Located observations use the existing WiGLE 1.6, 14-column CSV formatter:
`MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type`.
Timestamps are UTC and CSV fields are quoted/escaped. Repeated observations
are retained. Buffered writes sync after 256 rows or two seconds, and at Stop.
This reduces repeated SD synchronization without placing file I/O in RF
callbacks. Unexpected power loss can still lose buffered data.

A row needs a fresh, valid GPS position and UTC near its RF observation time.
RF age is transported from C5 and retained from S3; a delayed queue is not
silently located at the writer's later position. Observations without a
qualifying fix go to a separate unlocated CSV. No zero/default coordinates,
uptime dates, interpolated positions or HDOP-as-metres are invented. Binary
labels that cannot be represented faithfully in the upload format also remain
in the unlocated diagnostic output. Upload only the WiGLE file to a mapping
service. No automatic upload or service credentials are introduced.

The six-C5 expansion path remains integrated and uses the same writer when
enabled in a Wi-Fi-capable mode. Its physical backboard and ATGM336H acceptance
remain deferred; Bluetooth-only mode does not start Wi-Fi expansion modules.

## Evidence and reference boundaries

New host fixtures exercise the actual shared receiver on both target branches,
including C5 extended scanning, both 100 and 1000 Hz scheduling, repeated
reports, overflow, lost-host timeout, failed setup and failed cleanup. Writer
fixtures cover lost START/read responses, exact ACK progression, Stop draining
and failed SD writes. Strict frame/security decoding, 100,000 repeated FIFO
observations, repeated CSV rows, stale queue location routing, AUTO baud and
shared preview/session ownership are also covered.

All 160 registered host suites and five native targets pass on 1,248 unchanged
inputs at source commit `cf43ac01f736e81f1b741798064c6fc903851c76`.
The 21 CI shard/aggregation helper tests and 20 build/package failure-handling
tests also pass. Native READY, console-stack and C5 Locate/parser placement
guards pass. A successful build or synthetic test does not establish RF
coverage, GPS reception, upload acceptance or comparative performance.

The matched candidate is
`dist/cerberus-unchained-0.1.0-dev.rev137-20261104T210426Z`.
The S3 application is 2,122,544 bytes, C5 is 1,728,736 bytes and the temporary
bridge is 255,296 bytes. Manifest SHA-256:
`1dfbe5e3363a2db4b2414f64d8691b79f23712b01e73a48edc7336d760741d33`.
Retained evidence is `artifacts/rev137-validation/local-milestone.json`.

Native fixed data/BSS totals are 170,492 bytes on S3 and 83,584 on C5, increases
of 368 and 120 bytes respectively over rev135. The session queues allocate
27,648 bytes of S3 PSRAM and 6,912 bytes of C5 internal RAM. The shared writer
workspace is 22,464 bytes of PSRAM. Task stacks, UART buffering, SDK allocations
and allocator overhead are separate; these sizes are not peak-runtime bounds.

Rev137 was installed on the connected Cerberus on 2026-11-04. Fresh verified
rev135 recovery prefixes cover 2,191,360 S3 bytes and the full 2,097,152 C5 bytes.
All nine bridge/C5/S3 image writes verified their hashes against the exact chip
identities. The application reports rev137 and C5 capability 75, mounted SD,
buffered display and no previous-reset fault. Two idle snapshots 67.6 seconds
apart show advancing uptimes/exchanges, unchanged C5 boot count and no growth
in the listed SPI error counters. The startup's four initial-ready timeouts and
legacy fallbacks are retained in evidence; this is not a claim of zero lifetime
timeouts. Settled S3 internal free heap is 135,964 bytes, largest internal block
94,208, and free PSRAM 1,427,552. These are idle samples before receiver startup.
The installation receipt is `artifacts/device-rev137/installation-receipt.json`.

Live LC29H reception was confirmed at 115200 baud, with zero reported NMEA
checksum errors. The user observed FIX + UTC before recording. A subsequent
392.458-second combined-mode run saved 16,592 observations: 3,290 S3 Wi-Fi,
11,338 S3 BLE and 1,964 C5 Wi-Fi. Both reported queue-drop counters and all
reported radio/SD service errors remained zero. Stop drained both queues,
released both RF owners and published two CSV files.

Readback found 2,291 located rows (685 Wi-Fi and 1,606 BLE) and 14,301 unlocated
observations. These sum exactly to the final accepted-radio counters. All
located rows have 14 fields, valid bounded coordinates, valid UTC dates and
consistent channel/frequency fields under the new local preflight. The GPS
fix became invalid during the run; this is not acceptance of sustained GNSS
reception. Every located row lacks a measured accuracy in metres.

The readback also found that the old pre-header omitted `star/body/subBody`
and still described the previous result-receipt path. Rev138 corrects these
fields. The original files and a separate metadata-corrected local copy are
retained in ignored artifacts; the original observation bytes were not changed.
The corrected copy passes local structural checks with missing-accuracy warnings.
This is not proof of WiGLE ingestion. Raw captures are intentionally absent
from Git. See [rev138](DUAL_RADIO_REV138.md) for export checks and next work.

Wi-Fi-only/BLE-only loaded operation, repeated Stop/restart on this installed
version, sustained GPS reception and live service import remain unaccepted.
ATGM336H hardware, the expansion backboard, long-drive endurance and live
WiGLE/WDGWars imports have not been accepted by this installation receipt.
No public binary release or additional PR update is part of this checkpoint.

References reviewed for this implementation:

- [JustCallMeKoko ESP32DualBandWardriver](https://github.com/justcallmekoko/ESP32DualBandWardriver): channel scheduling and buffering reference. No source was copied into the new receive engine.
- [HaleHound-CYD](https://github.com/JesseCHale/HaleHound-CYD): public behavior documentation. The inspected repository tree did not contain its firmware implementation; no source-level efficiency comparison is claimed.
- [WiGLE Android uploader](https://github.com/wiglenet/wigle-wifi-wardriving): current CSV field order and UTC format.
- [WDGWars format documentation](https://www.wdgwars.pl/press): WiGLE 1.6 input compatibility. Live import is unverified.
- [Quectel LC29H specification](https://www.quectel.com/content/uploads/sites/3/2022/06/Quectel_LC29H_GNSS_Specification_V1.2.pdf): receiver baud reference.
- [Espressif S3 coexistence](https://docs.espressif.com/projects/esp-idf/en/v5.0.2/esp32s3/api-guides/coexist.html) and [C5 coexistence](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c5/api-guides/coexist.html): shared RF constraints.

Existing third-party notices and attribution remain in force. These references
do not constitute endorsement or a measured claim that Cerberus outperforms
another device.
