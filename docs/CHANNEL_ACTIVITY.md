# Channel activity

Rev158 adds **LIST / GRAPH** tabs to the existing Wi-Fi **CHANNELS** page.
Open **Scan → Wi-Fi → Survey summary → CHANNELS**, then choose **2.4 GHz** or **5 GHz**.
The graph is the default view; **NEXT** advances through six-channel pages.
Both views use the same retained measurements. Switching views does not start
another receiver or add SPI reads. Existing Stop, Back and radio-release
handling remain in place.

The C5 receives management, control and data frames on one channel at a time.
The S3 reads validated snapshots, renders the display and handles navigation.
This workflow does not run a second S3 receiver: overlapping reception would
change the meaning of these per-channel measurements without an established
coverage or responsiveness benefit. It does not transmit test traffic.

## Reading the graph

Each bar is the frames-per-second rate from that channel's latest completed
receive window. The scale follows the largest visible rate. The receive
duration appears below the rate; the List view also includes cumulative and
last-window frame counts and RSSI. These are sampled frame counts, not unique
devices, unique packets or airtime utilization. Channels are sampled at
different times, and visible rows arrive over successive snapshot reads.
This is not a simultaneous comparison of all channels.

**PENDING** means the S3 has not received that row. **WAIT** means the channel
has not completed a sample. After a completed sweep, **N/A** identifies a
channel the current region/driver could not sample. A measured zero is shown
as `0`. Stopped or failed reception uses muted bars and explicitly marks
retained values. Rates that reach the 16-bit field limit show `>=65535`.

## Receive-time correction

Previous firmware counted ten 10 ms task delays as a 100 ms sample. Scheduling
can extend those delays, overstating the rate when the nominal duration is
used. Rev158 measures the actual callback-counting window with the monotonic
microsecond timer. It waits until at least the 100 ms target has elapsed,
then calculates `frames * 1,000,000 / elapsed_us`, rounded down. The displayed
duration rounds up to milliseconds. For example, 17 frames in 120 ms report
141 frames/s, rather than 170 frames/s.

Cancelled partial windows do not replace the last completed sample. A
nonadvancing/backward timer or a duration that cannot fit the existing field
ends reception with an error. The existing scoped owner remains responsible
for radio cleanup, including retries if the driver cannot disable reception.
RSSI accumulation stops when its matching frame count saturates.

C5 capability **81** identifies measured windows. The 22-byte result and
status layouts are unchanged: status retains the 100 ms target, while each
result carries its actual duration. Compatible older scoped peers (70–80)
remain usable and the graph labels their rates as nominal 100 ms values.
Peers below 70 cannot start this scoped survey workflow.

## Reference and verification

The upstream reference is JustCallMeKoko and the ESP32 Marauder contributors'
[Channel Summary (`WIFI_SCAN_CHAN_ACT`)](https://github.com/justcallmekoko/ESP32Marauder/blob/79cc6bcba1db6679baee1dab2b0c1ad5010e6363/esp32_marauder/WiFiScan.cpp#L11215).
The live `develop` ref was checked on November 8, 2026 and matched that commit.
Cerberus implements its own scoped C5 sampling, SPI validation and S3 graph.
Marauder's period-wide channel counts and Cerberus's latest-window rates are
different measurements. The separate fixed-channel Channel Analyzer timeline
and complete upstream option parity remain open.

`check_channel_survey.py` checks the production rate helper, duration limits,
saturation and unchanged output on invalid input.
`check_channel_survey_lifecycle.py` compiles the actual C5 worker, dispatcher,
S3 SPI adapters and UI functions. Its fixtures cover delayed receive windows,
clock faults, cancellation, failed cleanup, lost/replaced projections, page
changes, legacy peers, tab hitboxes and display bounds. Production-rendered
previews use synthetic traffic and include pending, zero, unavailable,
saturated and retained rows. These checks passed during development.

## Installed checkpoint

Rev158 is installed from source `b17f62c8f5c0fc0f6e515ee7e0f7ad38414aa5d5`,
tagged `checkpoint/rev158-channel-activity`. Package:
`cerberus-unchained-0.1.0-dev.rev158-20261108T061813Z`.
Manifest SHA-256:
`43a784da92a597d2ffd91695e7f0d644f52b947e257c6ea5f0c8ce58dbe796c3`.

All 171 host suites and five native targets passed on unchanged inputs. Fresh
S3/C5 recovery backups matched the installed rev157 baseline. All nine image
writes were verified, and both processors' live application descriptors match
the packaged versions, SDK, compile time and full ELF fingerprints.

Two idle captures 49.5 seconds apart showed advancing uptimes, mounted SD,
ten S3 tasks, stable heap and no additional listed SPI errors. Four initial
READY timeouts/fallbacks were present in both captures; mutex, READY-fall,
READY-high and driver errors were zero. S3 free internal memory was 134,244
bytes, its largest block 94,208 bytes, and free PSRAM 1,426,140 bytes in both.
The saved upload profile loads; no upload or home-network connection was made.

Physical 2.4/5 GHz graph, Stop/Back, subsequent Wi-Fi scan and sustained-use
checks remain pending. This is a local checkpoint, not a published release or
complete upstream parity. The owner-confirmed Device Info check remains
documented separately under [rev157](DEVICE_INFO.md).
