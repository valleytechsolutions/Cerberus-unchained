# Cerberus Unchained

Experimental Cerberus firmware with integrated
wardriving, C5 Wi-Fi capture and analysis, S3 BLE discovery and tools, Remote ID
receive workflows, AP/client navigation, and optional expansion radios.

This is a separate project from
[Cerberus Sentinel](https://github.com/463N7/Cerberus-Sentinel). Sentinel keeps its
own scope and releases. Unchained starts from the complete rev81 contribution,
with its original Git history and contributor attribution preserved.

## Credit to JustCallMeKoko and the ESP32 Marauder contributors

**Thank you to [JustCallMeKoko (Just Call Me Koko)](https://github.com/justcallmekoko),
creator of [ESP32 Marauder](https://github.com/justcallmekoko/ESP32Marauder), and
the contributors who build and maintain it.** Their original work informs
Cerberus's Marauder-derived features and compatibility research. Credit for
that work remains with its authors; Unchained-specific implementations and
integration are identified separately.

From [@valleytechsolutions](https://github.com/valleytechsolutions):
“Koko gave me permission to bring this work into Cerberus, and I want to make
sure he gets his roses. Thank you for sharing your code and making this possible.”

The complete [MIT notice](LICENSES/ESP32Marauder.txt), source-level credits and
other third-party notices remain in force. See
[attribution and source provenance](docs/MARAUDER_ATTRIBUTION.md) for the exact
reference revisions and the distinction between adapted code and independently
implemented workflows. This acknowledgment does not change any license or
imply upstream endorsement of Cerberus Unchained.

**Local development checkpoint:** `0.1.0-dev.rev162` is installed on the
development board with onboard C5 capability 83. Passive **Wi-Fi Fox Hunt**
opens from selected AP, observed-client, PineScan and MultiSSID details. C5
tracks the transmitter and searches after signal loss; S3 validates the
readings, retains history and handles the screen. Channel Analyzer cleanup
errors also remain visible while release is retried.
[Tracker behavior and installation evidence](docs/WIFI_FOX_HUNT.md)

All 179 registered host suites and five native targets passed on unchanged
inputs. All nine image writes verified, and both live build fingerprints
match the package. Two idle captures 72.9 seconds apart showed mounted SD,
stable free memory and unchanged monitored SPI error counters. The saved
upload profile loads. Physical tracking/reacquisition, touchscreen navigation,
radio handoffs and sustained-use checks remain pending.

The pair also includes the [Channel Activity graph](docs/CHANNEL_ACTIVITY.md),
[Channel Analyzer](docs/CHANNEL_ANALYZER.md),
[GPS qualification and radio cleanup retries](docs/GPS_AND_RADIO_RECOVERY.md),
and [recovery after early S3 Wi-Fi startup failures](docs/SCAN_STARTUP_RECOVERY.md).
These are specific implemented improvements; complete Marauder parity remains
open in the coverage ledger.

**Tools → System health → DEVICE INFO** reports each processor's application
build, SDK, partition and memory/status information. The owner confirmed
the S3/C5 tabs, Refresh and Back on rev157. See [Device Info](docs/DEVICE_INFO.md).

The included Wardriving changes continue across private journal segments,
reduce redundant SD writes and keep one final CSV with strongest/weakest RSSI
rows. A short rev156 recording saved 151 final rows in 474 ms without a
reported radio, GPS or SD error. Physical journal rollover, long-session
save time and final CSV readback remain open. See
[the rev156 checkpoint](docs/DUAL_RADIO_REV156.md) and
[current Wardriving behavior](docs/WARDRIVE_DATA_QUALITY.md).

Open **Scan → Wardriving** and tap **START** once. Cerberus waits for a valid
GPS fix and UTC, then starts automatically. It writes one WiGLE 1.6 CSV per
session, such as `CERBWardrive001.CSV`, retaining the strongest and weakest
valid readings per address/type identity with their original position and time.
If the session ends before it can save a row, the Stop screen explains why.

After stopping, open **LOGS / UPLOAD** to review a file or upload it over home
Wi-Fi. One JSON profile holds the network settings and service credentials.
A service shows **UPLOADED** once it has acknowledged every part of the file.
The device can read that status from saved receipts when you reopen the log.
[Setup and upload guide](docs/WARDRIVE_UPLOADS.md)

A rev144 test session reported one saved file with 1,182 geotagged rows. The
upload-status update was then installed and checked on the S3/C5 board.
At that checkpoint, WiGLE had acknowledged an earlier upload and the recorded
WDGWars attempt failed during the TLS handshake. Live Wardrift uploads, long
drives and full Marauder feature parity remain open. [CSV readback](docs/DUAL_RADIO_REV138.md) ·
[Rev144 device checks](docs/DUAL_RADIO_REV144.md)

The published experimental release documented here is **`0.1.0-dev.rev144`**,
paired with onboard C5 firmware revision 75 and protocol 2.
[Install rev144 with the web flasher](https://463n7.github.io/Cerberus-unchained/)
or [download its matched package and checksums](https://github.com/463N7/Cerberus-unchained/releases/tag/v0.1.0-dev.rev144).
The guided installer programs the bridge, C5 and S3 using one USB cable in
desktop Chrome or Edge. The release images match the package used for the
owner's rev144 acceptance checks: all nine image writes and 29 paired
installation/startup checks passed, including mounted SD, buffered display, advancing processor uptimes and
clean startup. This does not establish full radio, GPS or upload-service acceptance.
The [SD assets and copy instructions](https://463n7.github.io/Cerberus-unchained/#sd-assets)
are directly below the flasher; the existing rev136 SD pack is also compatible.
See the [rev144 checkpoint](docs/DUAL_RADIO_REV144.md) and
[flasher guide](docs/WEB_FLASHER.md). The prior rev136 release remains available.
Sentinel 1.0.0 binaries are not Unchained firmware.

## Build and validation

Use Python 3.11 or newer and PlatformIO Core 6.1.19. The repository includes the
C5 platform adapter and pinned firmware configurations.

```sh
python -m pip install platformio==6.1.19 ziglang==0.14.1
python tools/run_host_checks.py
python tools/build_unified.py --list
python tools/build_unified.py --target all
```

To build and package the three matched onboard images (S3 application, onboard
C5, and temporary USB bridge):

```sh
python tools/build_unchained.py --target all --package
```

First-time dependency resolution can change generated inputs. Review those
changes and rerun the build for a stable receipt. Run firmware builds and host
checks separately. These commands do not flash hardware or publish a release.
See [Unchained build and release instructions](docs/UNCHAINED_BUILD.md) and
[the inherited validation procedures](docs/VALIDATION.md).

## Development status

The rev149 development candidate makes Wardriving START wait for a fresh GPS
fix and RMC UTC, then start automatically. GPS is always enabled on that page;
STOP or Back cancels the wait. Larger Wi-Fi and BLE counters show received
observations. After Stop, it saves the strongest and weakest complete
observation per Wi-Fi BSSID or Bluetooth address in one `CERBWardrive###.CSV`.
Compaction runs after the receivers stop with a fixed working buffer; capture
cadence is unchanged. See [data quality, recovery and validation limits](docs/WARDRIVE_DATA_QUALITY.md).
This candidate is separate from the published release described above.

Rev137 provides Wi-Fi-only, Bluetooth-only and combined Wardriving modes.
Wi-Fi-only splits S3 2.4 GHz and C5 5 GHz reception; Bluetooth-only uses both
receivers. Combined mode keeps C5 on 5 GHz and assigns S3 alternating Wi-Fi/BLE
windows. Bounded queues separate RF callbacks from CSV/SD work and preserve
repeated observations, with overflow counters. GPS AUTO, LC29H-AA, ATGM336H
and manual profiles live in one Settings branch. GNSS can be inspected before
recording; stale or absent fixes route observations to unlocated output.
See [roles, limits and validation scope](docs/DUAL_RADIO_REV137.md).

Rev136 reuses Packet Count's raw-frame scratch for in-place PCAPNG encoding,
removing a second 4,096-byte buffer from the optional S3 recording worker.
Frame limits, source/session ownership, one-time append/ACK semantics and
count-only behavior are unchanged. Focused tests inspect complete serialized
payloads, chunk boundaries, maximum frames and lost-ACK retries. Native worker
size falls from 8,424 to 4,328 bytes without fixed RAM growth. All 158 host
suites, five native targets and matched-package verification pass. Evidence is in
[the rev136 checkpoint](docs/DUAL_RADIO_REV136.md). USB export remains pending.

Rev135 corrects the C5 callback payload extent in Packet Count and Probe Sniff
to follow the installed ESP-IDF 6.0.1 contract. Live rev134 diagnostics found
all 1,926 callbacks rejected on channel 48 and all 5,267 rejected on channel 10
before address matching. The code used an additional `dump_len` equality
condition that Espressif's own sniffer does not require. The correction retains
receive-error checks, explicit frame-type admission, strict parser bounds and
recording limits. It also replaces the confusing S3 RF-absence text with its
processing role and shows a validated metadata-rejection warning. See
[the rev135 checkpoint](docs/DUAL_RADIO_REV135.md). All 158 host suites, five
native targets and package checks pass without fixed RAM growth. The owner
confirmed increasing displayed totals; the same live channel-11 session
advanced from 873 to 2,748 valid frames and 262 to 854 selected matches, with
zero metadata rejections. Stop, Back and a fresh split scan also passed; the
new scan retained 19 APs across both chips without fallback. The 5 GHz Count
path, optional recording and Probe Sniff acceptance remain pending.

Rev134 corrects Packet Count scheduling when slow UI ticks and transport calls
would otherwise let status polling starve setup or counter reads. A recent,
validated status reply permits one bounded progress operation before another
keepalive. Stop, uncertain ownership and release retain their priority. The
screen now explains the S3 analysis role for C5-only reception, distinguishes
setup from reception and shows readout progress. A read-only `packet_count`
diagnostic exposes copied controller state without controlling either radio.
All 158 host suites and five native targets pass on 1,234 unchanged inputs.
The resource audit measures 464 additional fixed bytes on S3 and no C5 fixed
RAM growth. Verified writes and two sampled boot checks pass. See
[the rev134 checkpoint](docs/DUAL_RADIO_REV134.md). The owner subsequently
confirmed advancing RX time but zero selected-AP totals on rev133; the
conditional scheduling defect is not established as that report's cause.
Actual receive and matching behavior still require the new device diagnostics.

Rev133 adds passive **Packet Count** from a selected Wi-Fi AP or observed
client, with a separate list of up to 16 targets, explicit Start/Stop and
optional per-receiver PCAPNG files on SD. Multiple-channel plans split coverage
between S3 and C5 without overlapping channel assignments; one fixed channel
uses C5 reception while S3 handles control, analysis and storage. Totals count
delivered observations, not packets per second, unique packets or proven
associations. Count-only mode allocates no raw recording queues or worker.

All 157 host suites, five native targets, the linked resource audit and matched
package verification pass on 1,230 unchanged inputs. The
[rev133 checkpoint](docs/DUAL_RADIO_REV133.md) records exact package and resource
costs. The matched rev133 pair was installed before rev134 with verified writes,
mounted SD and two passing sampled boot checks. This establishes installation, not live
Packet Count acceptance.
Physical Packet Count/recording acceptance, rev132 Mesh Save/Open and Locate
acceptance remain pending. Counter CSV, serial capture and full parity are
unfinished.

Rev132 implements native Mesh saved maps: portable `.MSH` files, asynchronous
SD save/open and historical observation browsing with CSV export. All 151 host
suites and five native targets pass on
1,202 unchanged inputs, and the verified matched development package is now
installed on both chips before rev133. Paired boot, mounted SD and sampled idle checks pass;
real SD save/reopen and Mesh reception acceptance remain pending. Fixed RAM
grows by 16 bytes on S3 and is unchanged on C5; the larger archive buffers are
optional PSRAM allocations. See [the rev132 checkpoint](docs/DUAL_RADIO_REV132.md).

Rev131 corrects the Mesh entry flow: the Scan tile is named Mesh Detection and
opens the five-tool menu before any individual tool. It also adds passive
selected-source Locate with a compact C5 filter, independent S3 signal history
and stopped CSV export. All 150 registered host suites and five native targets
pass on 1,194 unchanged inputs; the matched development package verifies.
Rev131 was installed on both chips before rev132, with C5 capability 73 online,
mounted SD and two stable idle boot samples. The owner confirms menu-first navigation and
Back work; real Mesh reception, Locate and SD report acceptance remain pending. See
[the rev131 checkpoint](docs/DUAL_RADIO_REV131.md) for scope and evidence.

Rev130 adds a passive Mesh Detection receive path, typed address model,
exact-session transfer, PAN/address/traffic views and stopped SD reports.
All 146 registered host suites and five native targets pass on 1,174 unchanged
inputs. The matched rev130 development package and native resource audit also
pass. The paired rev130 update was installed, with C5 capability 72 online,
mounted SD and two stable idle boot samples, before the rev131 update above.
Mesh touchscreen, real reception
and loaded-operation acceptance remain pending. See
[the Mesh foundation](docs/DUAL_RADIO_REV130.md) for its observation scope,
measured allocation costs and remaining work. These software checks do not
establish physical reception or complete Mesh/Marauder feature parity.

Rev129 corrects receive continuity in the existing passive 802.15.4 Beacon and
Activity sweeps: the receiver remains awake after each frame until the channel
dwell ends. The host regression reproduces the previous first-frame-only
behavior and checks later frames, passive ACK settings and cleanup. Both
worker fixtures are registered together in the rev129 139-suite host registry.
All 139 host suites and five native targets pass on 1,145 unchanged inputs; the
matched local package verifies, with unchanged linked fixed RAM on both chips.
Rev129 has not been installed. See
[the receive checkpoint](docs/DUAL_RADIO_REV129.md) for validation and limits.
[Mesh Detection](docs/MESH_DETECTION_PLAN.md) builds on that fix in rev130 and
requires no GPS.

Rev128 adds a dedicated passive Probe Sniff workflow: bounded request,
transmitter and network-name lists, stable details, incremental transfer and
explicit stopped-evidence saving to SD. C5 owns passive reception while S3
validates, groups, presents and saves the observations. All 138 host suites,
five native targets and linked resource checks pass on 1,142 unchanged inputs;
[the rev128 checkpoint](docs/DUAL_RADIO_REV128.md) records the limits and package
status. The matched local candidate and package verification also pass.
Fixed data/BSS is unchanged on S3 and grows by 48 bytes on C5; optional
capture, UI and save allocations are measured separately. At that checkpoint,
rev126 remained installed. This major milestone completed the requested PR #1 update and pause
checkpoint; the owner resumed the unfinished overall port on November 2.

Rev127 corrects Channel Survey ownership when C5 restarts
between preflight and Start. The session-bound control/readout and
renewable ownership lease preserve existing passive metrics while preventing
stale commands from controlling a replacement survey. All 136 host suites and
five native targets pass with unchanged inputs. Linked fixed data/BSS grows by
16 S3 bytes and 88 C5 bytes, with no new task or observation bank. The matched
local package passes verification. See [rev127](docs/DUAL_RADIO_REV127.md). At
that checkpoint, installed firmware was rev126, with the owner's targeted Recon
controls and priority acceptance.

Rev126 implements owner feedback on Recon: recognized hints appear first in a
stable list, Dashboard/Radar are visible main tabs, and category controls say
ALERT. Current accepted S3 Meta identifier hints now reach Recon at LOW
confidence, with no glasses/model claim. All 136 host suites and five native
targets pass on 1,120 unchanged inputs; independent review and the linked
resource audit pass. The Recon model/view sizes remain unchanged. The matched
package is installed with verified paired writes and two successful boot
captures 53 seconds apart, with no additional listed SPI errors. The owner
confirmed Dashboard/Radar, Pause/Resume, Back, the ALERT label and stable
recognized-hint priority. See [rev126](docs/DUAL_RADIO_REV126.md). That installation
is superseded by rev130; broader feature and loaded-performance acceptance
remains unfinished.

Rev125 implements offline Marauder AP JSON and SSID text import, details,
copy saving and export from native combined S3/C5 Wi-Fi reports. It preserves
optional-field presence, duplicates, station lists and name bytes, maps security
values explicitly and reports format information loss. All 136 host suites and
five native targets pass on 1,118 unchanged inputs. The linked resource audit
measures 32 additional fixed S3 bytes; existing Recon, scan and archive
allocations are unchanged, with optional interchange costs tracked separately.
The matched local rev125 package is finalized but was not installed while Recon
feedback was addressed. At that checkpoint the installed pair was rev124.
See [rev125](docs/DUAL_RADIO_REV125.md) and
[the interchange contract](docs/WIFI_MARAUDER_INTERCHANGE.md).

Rev124 implements combined S3/C5 Wi-Fi results with SD save/reopen,
AP/SSID views and copy saving, and Recon gains a factual dashboard in place
of its live mascot, plus an optional RSSI radar. All 134 host suites and five
native targets pass on the same 1,108 unchanged inputs. The linked audit measures
4,800 bytes less fixed S3 data/BSS and a 4,992-byte smaller S3 image; C5 fixed
memory is unchanged. Optional viewer/worker allocations and physical acceptance
are tracked separately. The matched rev124/C5 firmware69 pair is installed with
verified image writes, paired application boot, display buffering and mounted
SD. A second idle capture more than 46 seconds later confirms advancing uptime
without additional SPI errors. Physical touchscreen, Wi-Fi save/reopen, Recon
recording, RF and sustained-load acceptance remain pending.
See [the rev124 checkpoint](docs/DUAL_RADIO_REV124.md) for validation status and
[saved Wi-Fi lists](docs/WIFI_SAVED_LISTS.md) for the storage and provenance
contract. These changes do not establish complete Marauder parity.

Rev123 implements Marauder-compatible retained Find My JSON import, offline
inspection, copy saving and export from complete saved S3/C5 originals and
matched responses, considered independently. Imported provenance stays unknown.
The separate model is bounded to 64 records, 8,192 total payload bytes and
49,152 JSON bytes; one optional JSON viewer and the existing serialized worker
handle the files. Ordinary native `.BLE` allocation is unchanged. Focused codec,
IO/storage and UI checks and independent reviews pass. The first complete
132-suite host run passed on 1,096 unchanged inputs; all five native builds
and the linked resource audit pass. S3 fixed data/BSS increases by 32 bytes,
C5 is unchanged, and optional JSON viewer/worker allocations total 68,200 bytes
before allocator overhead and other retained stores. Matched package build
and verify-only checks pass for three targets and nine images. See
[rev123](docs/DUAL_RADIO_REV123.md) for the exact local package and evidence.
The preceding rev123/C5 firmware69 update was installed with verified image writes,
paired application boot, display buffering and mounted SD. A second idle capture
more than 42 seconds later confirmed the settled boot state. Physical RF,
touchscreen, save/reopen, GPS and sustained-load acceptance remain pending.
This offline interchange does not restore imported records into live discovery
or establish full Marauder parity.

Rev123 and the rev114–122 source milestones below are included in installed
rev124. Their individual checkpoint documents preserve the evidence and
installation status recorded when each milestone was completed.

Rev122 implements integrated GPS DATA and received-NMEA views, independent GGA
metadata freshness and retained UART cleanup. GPS remains optional and OFF
unless selected through Wardriving. Focused checks and independent reviews
pass, including stable paging, whole-line privacy masking and retained UART
cleanup. All 130 host suites, five native builds, the linked resource audit and
matched package verification pass. Measured S3 fixed data/BSS grows by 128 bytes;
the optional trace and display copy use 2,624 bytes of PSRAM, excluding allocator
overhead. C5 fixed data/BSS is unchanged. See
[rev122](docs/DUAL_RADIO_REV122.md). No GPS module is attached. This is not a
full-parity or physical acceptance claim.

Rev121 implements C5 receive cleanup and Channel Survey handoffs. Independent
review, all 128 host suites, five native builds, resource measurements and
matched package verification pass on the retained final inputs.
Rev121's Channel Survey workflow required C5 firmware 69. The current scoped
session workflow requires firmware 70; its earlier wire commands originated
in firmware 46. See
[rev121](docs/DUAL_RADIO_REV121.md) for evidence and limits.

Rev120 replaces owner-only shared BLE host release with
an atomically checked claim token. Existing scanner, central, advertiser,
Radio Lab and Wardriving callers use the exact lease. See
[rev120](docs/DUAL_RADIO_REV120.md) for validation and acceptance limits.

Rev119 binds Wardriving S3 BLE acquisition, completed
evidence and cleanup to an exact passive scan. Unresolved cleanup stays in
STOPPING. See [rev119](docs/DUAL_RADIO_REV119.md) for validation and limits.

Rev118 binds Recon S3 acquisition, completed evidence
and cleanup to an accepted passive scan, while retaining independent C5 Wi-Fi
scheduling. See [rev118](docs/DUAL_RADIO_REV118.md)
for validation status and acceptance limits.

Rev117 binds S3 Fox Hunt callbacks and cleanup to its exact tracking scan,
retaining independent C5 tracking and source-selective retry. See
[rev117](docs/DUAL_RADIO_REV117.md).
The [coverage ledger](docs/MARAUDER_COVERAGE_LEDGER.md) now distinguishes these
receive milestones from installation and physical acceptance;
[remaining-work notes](docs/RECEIVE_REMAINING_GAPS.md) record the broader audit.

Rev116 gives BLE Analyzer independent receiver intent and retry, and binds S3
Analyzer callbacks and cleanup to its exact scan. See
[rev116](docs/DUAL_RADIO_REV116.md) for validation and acceptance limits.

Rev115 makes ordinary Bluetooth discovery intent independent of either
receiver's failure, adds source-specific retries and preserves Stop/navigation
through release. See
[rev115](docs/DUAL_RADIO_REV115.md) for validation and acceptance limits.

Rev114 moves the dedicated Find My Monitor onto the existing S3 and C5
discovery owners, with separate receiver status, frozen row selection and
retained SD evidence. See [rev114](docs/DUAL_RADIO_REV114.md) for validation
and physical acceptance limits.

Rev113 adds optional compact C5 Bluetooth response-result transfer, preserving
the existing frozen evidence and full-format compatibility. See
[rev113](docs/DUAL_RADIO_REV113.md) for validation and limits. The matched
rev113/C5 firmware68 update was installed with verified writes, paired boot and
mounted SD. Physical feature and performance acceptance remain pending.

Rev112 addresses independent S3/C5 Bluetooth Fox Hunt startup, source-specific
retry, separate signal/status presentation and retained Stop/Back cleanup.
The preceding rev112/C5 firmware67 update was installed with verified writes,
paired boot and mounted SD. See [rev112](docs/DUAL_RADIO_REV112.md) for its
validation and remaining physical acceptance. Paired boot does not establish
full feature acceptance or complete Marauder parity.

Rev111 removes a redundant response-mode dispatch from the passive C5 EXT
receive path while preserving response invalidation and owner cleanup. It
also reconciles stale Bluetooth/storage coverage notes with the implemented
rev110 behavior. See [rev111](docs/DUAL_RADIO_REV111.md) for validation and
installation status. Rev111 was the verified installed baseline before the
rev112 update. Full parity and physical feature acceptance remain open.

Rev110 adds an explicit C5 response-request mode to BLE discovery and family
views, while preserving passive as the default. S3 and C5 observations remain
source-labelled; original advertisements and matched scan responses have
independent live details and v6 SD evidence. The v1-v5 readers remain supported.
See [the rev110 checkpoint](docs/DUAL_RADIO_REV110.md) for validation, memory
costs and remaining physical checks. These changes are included in installed
rev111; physical response and saved-report acceptance remains pending.

Rev109 carries the C5 full-original family record into v5 SD reports and
offline HINTS. Older v1-v4 reports remain readable with missing evidence
explicit. Source identity, original previews, S3 responses and historical
ages keep their separate meanings. Implementation, validation and resource
costs are tracked in [the rev109 checkpoint](docs/DUAL_RADIO_REV109.md).
The matched rev109/C5 firmware66 update was installed with verified writes,
paired boot and mounted SD. Physical scan/saved-report acceptance remains
pending; boot confirmation is not complete feature validation.

Rev108 adds C5 full-original classification and focused admission for the six
passive BLE family views. The S3 retains its independent evidence and controls
the combined list and owner transitions. Generic discovery, tracking and
Analyzer avoid the additional family parsing and transfer. All 124 host suites
and five native targets pass. Measured additions are 3,312 bytes across optional
S3 PSRAM stores, 240 S3 internal bytes and 3,072 C5 internal bytes. Behavior,
resource costs and limits are tracked in
[the rev108 checkpoint](docs/DUAL_RADIO_REV108.md). These changes are included
in the installed rev109 update; their physical feature checks remain pending.

Rev107 extends the existing concurrent receivers into source-labelled BLE
family lists and C5 original-advertisement hint details. It reuses committed
observations and the existing strict parsers. The complete retained-report
requirement, remaining C5 retention gaps and validation are tracked in
[the rev107 checkpoint](docs/DUAL_RADIO_REV107.md). All 124 host suites and five
native targets pass. The optional discovery store grows by 48 PSRAM bytes;
fixed S3/C5 data and BSS are unchanged. Included in installed rev109; physical
acceptance of this local checkpoint is pending.

Rev106 adds a dedicated passive C5 activity path beside the S3 BLE Analyzer.
The compact source-clock samples avoid inventory downloads and keep the two
receivers' rates, counter scopes and gaps separate. All 123 host suites and five
native targets pass. The companion state grows by 424 PSRAM bytes; fixed
data/BSS grows by 16 bytes on S3 and 48 bytes on C5. Local validation and limits
are tracked in [the rev106 checkpoint](docs/DUAL_RADIO_REV106.md).
The matched rev106/C5 firmware65 update was installed with verified writes,
both chips online and SD mounted. Touchscreen, RF and loaded-memory acceptance
remain pending.

Rev105 adds receiver filtering and file/name/signal ordering to saved BLE
reports. Browsing uses a bounded index over the loaded document, preserves
frozen selections and historical ages, and makes no radio or storage requests.
The v4 file format and older readers are unchanged. All 123 host suites and
five native targets pass. The existing optional viewer grows by 72 PSRAM bytes;
fixed internal data/BSS is unchanged. Local validation and remaining physical
acceptance are tracked in [the rev105 checkpoint](docs/DUAL_RADIO_REV105.md).
This previously local change is included in the installed rev106 update.

Rev104 combines ordinary BLE discovery into one source-labelled list, with
ALL/S3/C5 receiver filtering, stable sorting and frozen selection. It reuses
the existing concurrent receivers, keeps distinct signal/time evidence and
adds no radio task or full observation bank. Family-specific views retain
their S3 evidence. All 122 host suites and five native targets pass. The new
presentation store adds 768 PSRAM bytes; fixed internal data/BSS is unchanged.
Measured resource costs and pending physical acceptance are tracked separately
in [the rev104 checkpoint](docs/DUAL_RADIO_REV104.md). This previously local
change is included in the installed rev106 update.

Rev103 moves the read-only console's formatting scratch from its task stack to
one reusable PSRAM buffer, with cleanup only after the writer is quiescent.
It preserves the 4 KiB stack, full 64-bit formatting and output capacity. Long
archive paths and filenames now print separately to avoid combined truncation.
The formatter's compiled frame falls 848 to 80 bytes, and the observed remaining
console stack rises 524 to 1,292 bytes in the read-only sequence. All 121 host
suites and five native targets pass; the matched local update is installed with
both chips online and SD mounted. See [the rev103 checkpoint](docs/DUAL_RADIO_REV103.md)
for evidence and limits; USB handover, sustained load and full feature acceptance
remain open.

Rev102 preserves the C5 receive envelope in v4 SD reports alongside the S3
matched-response evidence. The existing offline viewer gains C5 RECEIVE INFO;
old v1/v2/v3 reports remain readable without inventing absent metadata. Saved
rows remain historical evidence and cannot become live action targets. All 120
host suites and five native targets pass; the matched local package is installed
with verified writes, both chips online and SD mounted. Fixed internal data/BSS
is unchanged; optional archive buffers grow 2,088 bytes. Physical save/reopen,
receive and runtime stress/stack acceptance remain pending. See
[the rev102 checkpoint](docs/DUAL_RADIO_REV102.md).

Rev101 preserves C5 callback source, delivered length, SID/PHY, properties,
controller status and full AD-layout validity with each original observation.
The optional metadata read still uses eleven chunks per row. The existing C5
detail view adds RAW ADV / RECEIVE INFO tabs; older peers retain explicit
unknown metadata. All 120 host suites and five native targets pass on the final
source, including the unsaved-metadata label. The matched package is installed
with both chips online and SD mounted; live reception and touchscreen acceptance
remain pending. See
[the rev101 checkpoint](docs/DUAL_RADIO_REV101.md).

Rev100 preserves S3 matched scan-response bytes separately from original ADV
evidence and extends the existing archive with v3 write and v1/v2 read support.
Selected observations retain full scan epochs. C5 v2 discovery gains bounded
32-slot replacement and frozen replacement/drop counters; legacy scans retain
their prior admission behavior. All 120 host suites and five native build
targets pass on unchanged inputs. The matched local package is installed with
verified image writes, both chips online and SD mounted. Touchscreen, live
reception and real-card save/reopen acceptance remain pending. See
[the rev100 checkpoint](docs/DUAL_RADIO_REV100.md).

Rev99 extends the existing BLE SD archive to one report containing up to 32
observations per receiver. New v2 files preserve receiver identity, S3 peak
versus C5 latest-ADV RSSI, and exact historical time versus conservative age
bounds. Old v1 files remain readable as LEGACY. Save freezes selected evidence
and waits for normal receiver cleanup before starting the SD worker; larger
result copies run outside the interrupt-disabled critical section. All 120 host
suites and five native targets pass on matching unchanged inputs. The matched
package is installed with verified images, both chips online and SD mounted.
Linked fixed RAM increases by 128 S3 bytes and zero C5 bytes; the larger report
buffers use optional PSRAM. Physical save/reopen and navigation acceptance remain
pending. See
[the rev99 checkpoint](docs/DUAL_RADIO_REV99.md)
for current build, installation and acceptance evidence.

Rev98 adds coherent C5 Bluetooth snapshots during the existing
eight-second listening sweep. S3 transfers and publishes a frozen bank while
C5 continues receiving, with separate update/sweep counters and first-result
reader timing. The existing completed-sweep path remains for older C5 peers.
It also fixes CHANGE AP / SCAN returning a dual-origin AP to the legacy C5-only
list. All 120 host suites and five native targets pass on matching unchanged
inputs. That matched package was installed with verified images, both chips online,
display buffer active and SD mounted. S3 linked internal memory is unchanged;
the BLE coordinator adds 176 bytes in PSRAM and C5 static RAM grows by 3,312
bytes. Live-result latency, corrected navigation and repeated radio transitions
still need physical acceptance. See [the rev98 checkpoint](docs/DUAL_RADIO_REV98.md)
and [live BLE snapshot scope](docs/DUAL_BLE_LIVE_SNAPSHOTS.md).

Rev97 adds a verified reuse path for a selected C5 Wi-Fi result: its completed
scan session, full relationship proof and exact AP identity must still match
before the original bank can reach the AP tools. Valid reuse starts no new
scan and retains the original observation age. S3 results still require fresh
C5 reacquisition. Analysis labels and channel-history denominators follow the
actual band coverage; reopening one proven bank cannot create another sample.
The separate C5 Bluetooth list gains signal/name/scan-order sorting, a
conservative recent filter and visible age bounds/unknown timing. These presentation
controls retain copied device identities and do not restart either radio.
All 120 host suites and five native targets pass on matching unchanged inputs.
The linked internal-memory increase is 528 bytes. The matched rev97 package
is installed with all image hashes verified, both chips online, display ready
and SD mounted. Settled internal free RAM is 134,652 bytes before new scans.
Touchscreen checks of the new controls and handoff remain pending. Evidence is in the
[rev97 checkpoint](docs/DUAL_RADIO_REV97.md).

Rev96 moves the three CPU-only directory-list banks to lazy, stable PSRAM
allocations to preserve internal radio memory. Their payload totals 21,760
bytes. A missing/exhausted PSRAM bank produces an explicit memory error and a
zero visible row count; Back and later reloads remain available. All 120 host
suites and all five native targets pass on stable inputs. The linked internal
sections shrink by 21,744 bytes. The paired rev96 package is installed, both
chips are online and SD is mounted. Cold internal free RAM is 135,268 bytes,
exactly 21,744 bytes above rev95. After BLE Stop/Back and a split Wi-Fi scan,
the console captured 19 S3 plus 11 C5 APs, no scan errors and released owners.
Warm internal free RAM is 88,036 bytes, 22,500 above scan admission, and remained
stable at a later read. The owner reported repeated split success; the captured
C5 counter independently establishes one completed job so far. The SD-list
browsing and following-scan check remains pending.

Rev95 addresses the retained S3 memory limitation observed after Bluetooth use.
It moves eligible NimBLE host allocations to PSRAM, preserves the existing
buffer counts and host lifetime, and rejects BLE initialization when optional
PSRAM is absent. It also adds cached per-radio/lifecycle diagnostics and bounded
recovery from an unknown C5 firmware cache. All 119 host suites and all five
native targets pass on stable inputs. The matched onboard package also passes
image, partition, source and console-stack checks. On the preceding installed
rev95 build, after BLE Stop/Back the owner and console confirmed a split scan:
9 S3 APs plus 17 C5 APs in 4,959 ms, no scan errors and no held radio owners.
Warm internal free RAM was 66,292 bytes, only 756 bytes above the next-scan
admission threshold. This narrow reserve motivates rev96. See
[radio memory recovery](docs/RADIO_MEMORY_RECOVERY.md).

Rev94 adds adaptive dual-radio scanning: Wi-Fi-only scans use S3 for 2.4 GHz
and C5 for 5 GHz when both are available; ordinary BLE discovery and fox hunting
gain a separate C5 receiver alongside S3. Mixed Recon and Wardriving keep C5
Wi-Fi plus S3 BLE. Receiver provenance, bounded memory, Stop/Back recovery and
fresh AP selection checks are part of the implementation. The paired rev94
build was installed on the development board; the owner confirmed completed
split Wi-Fi scans, Wi-Fi Stop/restart/Back and activity on both BLE receivers.
Diagnostics confirm repeated C5 5 GHz sweeps and active S3 BLE discovery.
Rev94 could fall back to C5 after BLE retained too much S3 internal memory;
the rev95/rev96 memory work above addresses this measured limitation.
Dual Fox Hunt, comparative
speed, coverage and sustained lifecycle acceptance remain pending. See
[dual-radio scanning and validation](docs/DUAL_RADIO_SCANNING.md).

The rev82-rev93 contribution adds bounded Wi-Fi result transfer and recovery,
scan-list controls and passive SSID/trust review, BLE analysis and SD snapshots,
Recon inventory and session recording, SD copy error handling, and a live
Recent Networks viewer in the integrated Wardriving tab. C5 Wi-Fi and S3 BLE
observations feed the existing scan services. Recent Networks reuses those
observations without another radio scan or worker task.

The local rev93 checkpoint recorded 107 passing host suites and five firmware
targets. Its installed-board evidence belongs to that original development
checkpoint; the Unchained-branded rev93 contribution was unflashed at submission.
The later rev94 installation has the limited acceptance described above. See the
[rev93 contribution and validation summary](docs/CONTRIBUTION_REV93.md).

Reported Wi-Fi **NO MEM**, **Association Sleep**, and **Bad Message** failures
remain open in the feature ledger. Later local owner checks confirmed Back and
subsequent Wi-Fi scan navigation; these do not validate every active feature.
The new Recent Networks UI, BLE archive save/reopen, Recon recording contents,
GPS reception, sustained load and six-radio expansion still need acceptance.
The full Marauder port and Wardriving integration remain in progress.

- [Rev93 contribution and remaining work](docs/CONTRIBUTION_REV93.md)
- [Rev94 dual-radio scanning](docs/DUAL_RADIO_SCANNING.md)
- [Original rev81 contribution](docs/CONTRIBUTION_REV81.md)
- [Feature coverage ledger](docs/MARAUDER_COVERAGE_LEDGER.md)
- [Integration targets and layout](INTEGRATION.md)
- [SD resources and their notices](sdcard/README.md)

## Origin and maintenance

Unchained was started by **463N7** from **valleytechsolutions**'s
[Sentinel PR #1](https://github.com/463N7/Cerberus-Sentinel/pull/1), preserving
commit `531f666d0600604c2502bdb64dd41d2a1a25a37e` and its ancestors.
The `upstream-rev81` tag identifies that exact imported snapshot.

The original contribution and validation documents retain their historical
names and claims. The [imported Sentinel README](docs/SENTINEL_REFERENCE.md)
is reference material for the original firmware. Internal `sentinel_*` source
names remain for compatibility; the product, build versions, packages, and
installer identify Unchained separately.

Bring fixes between Sentinel and Unchained selectively using their shared
history. Neither project automatically incorporates the other's features.
Existing licenses and source/data attribution remain in force; see `LICENSES/`,
[Watch notices](docs/WATCH_THIRD_PARTY_NOTICES.md), and the SD resource notices.
