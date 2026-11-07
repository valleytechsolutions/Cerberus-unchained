# Packet Monitor recording and radio release

Marauder `develop` was checked on November 7, 2026 and still resolves to
`79cc6bcba1db6679baee1dab2b0c1ad5010e6363`. Its
[RunPacketMonitor](https://github.com/justcallmekoko/ESP32Marauder/blob/79cc6bcba1db6679baee1dab2b0c1ad5010e6363/esp32_marauder/WiFiScan.cpp#L5062)
opens a `packet_monitor` capture, and its receive callback appends frames.
Cerberus's Packet Monitor has channel controls and three graphs, but no
recording path. Packet Count and selected-BSSID capture are separate tools.

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

Full-matrix validation and installation of this candidate are pending. The
connected device remains on rev145. Packet Monitor recording is still missing.

## Remaining implementation

1. Add optional bounded C5 frame recording alongside its counters. Graph-only
   operation must allocate no frame queue. Capture metadata and bytes in the
   receive callback; keep SPI, SD writes and dynamic allocation outside it.
   Preserve the documented SDK payload extent and reject payload-less callbacks.
2. Use S3 memory and the serialized SD writer to assemble validated frame reads
   and save a C5-labelled PCAPNG file. Preserve the source clock, channel and
   RSSI without inventing UTC. Report queue drops, oversized/truncated frames,
   transport loss and SD failures separately from graph counts. Retuning must
   finish the old segment before admitting the next one.
3. Add the recording control to the existing Packet Monitor page. Saving,
   Stop/Back and the next Wi-Fi scan require device acceptance, including slow
   SD, 2.4/5 GHz, repeated use and loaded memory measurements. A successful build
   or idle boot does not satisfy those checks.

C5 reception and S3 control, analysis and storage are useful work for both
processors. A second receiver should be added only with a distinct channel or
coverage benefit and independently labelled observations.
