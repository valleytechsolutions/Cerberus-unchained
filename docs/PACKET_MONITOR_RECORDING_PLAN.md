# Packet Monitor recording and radio release

Rev152 corrects a shared-command collision that blocked this newer monitor
path at the C5 dispatcher. See [SPI command routing](SPI_COMMAND_ROUTING.md)
for the reproduced failure, paired capability-78 requirement and expanded
regression coverage. Physical acceptance of this recording path remains open.

Marauder `develop` was checked on November 7, 2026 and still resolves to
`79cc6bcba1db6679baee1dab2b0c1ad5010e6363`. Its
[RunPacketMonitor](https://github.com/justcallmekoko/ESP32Marauder/blob/79cc6bcba1db6679baee1dab2b0c1ad5010e6363/esp32_marauder/WiFiScan.cpp#L5062)
opens a `packet_monitor` capture, and its receive callback appends frames.
Cerberus's installed Packet Monitor has channel controls and three graphs.
The rev148 implementation adds the recording path described below. Packet
Count and selected-BSSID capture remain separate tools with different scopes.

## Stop correction

The C5 accepts Stop before its worker disables reception and restores the
previous band/channel. If that cleanup fails, the worker publishes ERROR and
retains the radio reservation. The S3 used to stop waiting when the state
changed from RUNNING to ERROR, then clear its attempted flag and allow Back or
a channel change. It could therefore leave a reserved C5 behind.

Commit `c6762ac` first kept cleanup retryable after that error. Its extracted
production-function test reproduced the original failure, and the S3 build
passed. The rev146 candidate replaces that blocking loop with a session
controller and tests the same failure through the complete C5/S3 path.

## Session control in rev146

Start, Stop, status and sample reads now carry a 64-bit session ID. C5 assigns
each accepted request a distinct ID within a boot, including after the legacy
generation byte wraps. A rejected request cannot become an accepted scan when
replayed later. The boot seed is random; it is not persistent identity.

The S3 records its claim before sending Start. A missing reply leads to an
exact-session status query. It never adopts another receiver because its channel
matches. Stale commands cannot stop or read a replacement job. C5 expires an
abandoned receiver after ten seconds without a matching status poll; diagnostic
INFO queries do not renew that lease.

Each UI tick performs at most one transport operation with a shared 20 ms
deadline, including any capability refresh. Back, restart, survey and channel
changes wait for confirmed release. C5 disable/restore retries execute on its
existing scanner worker. Sleep cancels pending navigation and keeps cleanup
running; waking does not restart reception. Reopening during a failed release
waits for the old channel to close before admitting the new one.

The graphs retain their 200 ms, channel-wide scope. This adds no task, dynamic
allocation or raw-frame queue. Packet Monitor requires the paired C5 capability
76; the legacy read-only status remains available for diagnostics.

Focused host checks pass for the actual service, dispatcher, SPI adapter, S3
controller and touchscreen routes. They cover malformed frames, lost replies,
capability and lock failures, cleanup retry, stale ownership, sample reads,
lease expiry, clock reversal, repeated navigation and 300 session cycles across
generation rollover. These checks run under the registered monitor-worker
suite. The adjacent Channel Survey and AP target checks also pass.

The standalone MultiSSID and PineScan UI checks pass. Their older, unregistered
navigation fixtures still refer to retired BLE/GPS/live-view helpers and do not
compile; they are not included in these passing claims.

At source commit `756b2a6`, all 166 registered host suites and all five native
firmware targets pass with unchanged input hashes. The S3 READY ISR and console
checks, C5 IRAM checks and C5 link checks also pass. The retired-session UI
regression fails before its correction and passes afterward.

The matched development package is
`cerberus-unchained-0.1.0-dev.rev146-20261107T172248Z`. Its S3, C5 and recovery
bridge all report rev146, and all 22 package checksums verify.

Rev146 was not installed. The later paired rev148 and rev149 packages were
installed with verified backups and image readback; rev149 is the current
installed baseline for the rev150 work. These packages include the monitor
changes, but physical PCAP readback and sustained recording remain unaccepted.

## Recording candidate in rev148

PCAP is optional on the existing page. C5 keeps a bounded two-frame queue only
when recording is enabled, admits SDK management/data payloads, and copies
their channel, RSSI and monotonic capture time outside the graph's critical
section. Payload-less callbacks and receive errors are excluded. Stop closes
admission; filter/band/channel restoration and callback drain must finish before
the reservation clears. The original filter is now restored as well.

Raw transfers carry the exact session, frame sequence and CRC. Immutable
metadata/statistics pins and idempotent acknowledgements let the S3 recover a
lost response without appending a frame twice. The optional queue requires C5
capability 77. It is released by the existing scanner worker, never a new C5
task. An abandoned stopped queue expires after 30 seconds; disposal can wait
until that worker returns from another radio operation. Raw reads do not renew
the ten-second receive lease.

The S3 reuses the Packet Count SD worker and its PSRAM assembly buffer. It
prepares the SD lease before asking C5 to receive. Each recording segment gets
`/CERBERUS/MONITOR/Mnnnnn/C5.PCAPNG` and `C5.CSV`. The CSV maps packet numbers
to the 64-bit session, source sequence, C5 uptime in microseconds, channel,
RSSI and frame length. Unknown RSSI is blank. PCAPNG uses radiotap and untimed
Simple Packet Blocks: the C5 uptime is not labelled UTC or GPS time.

Both files must flush and close successfully before the PCAP filename is
published. Failed or interrupted writes retain temporary data (`C5.TMP` and
`C5.IDX`); a failed final rename can leave the completed CSV beside a temporary
PCAP. The device reports that failure. USB storage and File Tools deletion
remain excluded while the SD lease is held.

Stop, Back, retune, Survey and sleep wait for receiver cleanup, queue drain,
exact-session release and file closure. Retuning starts a new segment. The
final status separates saved frames, full-queue drops, other rejected frames,
transport loss and storage errors from the graph counters. These diagnostic
counters are not yet persisted in a session manifest. Recording counter
refreshes are limited to four per second; state and error changes are immediate.

Focused host checks exercise the actual C5 service, wire adapters, S3 controller,
SD worker and filesystem implementation. Their fault boundaries are injected;
they are not RF throughput or physical SD durability measurements. Full build
and registered-suite validation are in progress for this candidate.

## Remaining acceptance

Installation, SD-file inspection, 2.4/5 GHz reception, Stop/Back and the next
scan, repeated use, slow-card behavior and loaded memory measurements remain
open. The bounded queue can drop frames under load and this is not a claim of
lossless channel capture. The coverage ledger remains partial until these
checks and the remaining upstream behavior are resolved.

C5 reception and S3 control, analysis and storage are useful work for both
processors. A second receiver should be added only with a distinct channel or
coverage benefit and independently labelled observations.
