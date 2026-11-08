# Channel Analyzer

Rev161 adds **Scan → Wi-Fi → Channel Analyzer**. It measures all successful
management, control and data receive callbacks on one selected channel. The
C5 supplies the receiver; the S3 validates readout, maintains history and
draws the screen. It does not require selecting an AP.

The upstream reference is JustCallMeKoko and the ESP32 Marauder contributors'
develop commit `79cc6bcba1db6679baee1dab2b0c1ad5010e6363`. The
[source audit](CHANNEL_ANALYZER_PLAN.md) identifies the original menu,
total-traffic history, channel controls and recent beacon hints. This is a
separate workflow from Packet Monitor's three management-frame subtype
series and Channel Activity's successive channel survey.

## Reading and controlling the screen

The screen starts on channel 1. **CH −** and **CH +** step through the
supported 2.4 and 5 GHz channels. A change first stops and releases the old
receiver session, then starts the new channel. History remains visible with
a labeled channel boundary and a gap between sessions. **STOP** retains the
result; **RESTART** starts another segment. **WI-FI MENU** waits for release
and returns to the existing Wi-Fi tools menu. Sleep cancels pending changes
and stops reception; waking does not silently restart it.

Each point represents a completed receive window with a 100 ms target and
its actual elapsed microseconds. The current line shows count, rate and
duration. The graph's horizontal axis is recent receive windows, not a fixed
wall-clock interval. A measured zero has a baseline pixel; missing windows
are gray gaps. Peak and average describe the retained history. Average is
total known callbacks divided by total known receive time, rather than an
unweighted average of windows. Saturated counts and rates display as lower
bounds.

The latest valid beacon supplies an SSID, BSSID, channel, RSSI and age hint.
SSID bytes are preserved and escaped for display; hidden names remain
explicit. The address follows the existing privacy setting. A malformed
beacon cannot refresh the hint. The name is an advertisement, not a verified
device identity or a selected-target lock. Callback counts are not unique
packets, distinct devices or airtime utilization.

## Processor and memory use

Analyzer shares the existing C5 scanner worker, exclusive reservation,
renewable lease and radio restoration with Packet Monitor. Its 64-window
ring occupies a union with the Monitor ring. There is no additional radio
task or concurrent second receiver. Analyzer explicitly enables all control
subtypes and restores both normal and control-subtype filters along with
the prior band and channel. A failed restore retains the reservation for
retry instead of allowing the next scan to inherit partial configuration.

The optional S3 PSRAM workspace retains up to 320 windows and boundary/gap
entries. Readout performs at most one bounded transport operation per UI
tick; capability probing and its request share a 20 ms budget. Drawing is
limited to four updates per second. Back frees the workspace after the exact
session has released. Allocation failure leaves Back available.

Capability 82 adds commands 191–197 with explicit Analyzer session identity.
Older peers are refused before starting a receiver. Monitor and Analyzer
cannot adopt or stop each other's sessions. Lost Start replies are resolved
by querying the same session, without replaying Start. Beacon readout pins
an immutable image across its five fragments, checking session, projection,
part, length and padding. A later beacon cannot splice fields into an older
readout. Sequence and projection exhaustion fail explicitly.

## Verification and remaining work

The host checks use the production callback, worker, dispatcher, wire adapter,
model and extracted UI routes. They exercise timing and saturation, short
control frames, malformed beacons, overwritten windows, projection changes,
lost replies, mode isolation, failed cleanup and subsequent normal scans.
Rendering checks use the production font and display bounds.

Software verification, paired installation and physical acceptance are
recorded separately. Physical reception on both bands, touchscreen
Stop/Back/retune/restart, repeated Wi-Fi/BLE handoffs, sustained memory and
field throughput remain pending. The coverage ledger therefore records this
workflow as **partial**, not complete upstream parity. Automatic hopping is
not claimed: the audited upstream Analyzer dispatch does not call its hop
handler.

## Installed checkpoint

Rev161 is installed from source `1cc9b24180e15219e38e53819de5e6b664fcca98`,
tagged `checkpoint/rev161-channel-analyzer`. Package:
`cerberus-unchained-0.1.0-dev.rev161-20261108T080518Z`.
Manifest SHA-256:
`ee5ee90e6b31b1a0ae62a04c6cbaa9804f693b092f76a326c9c8fbb14a5bf43b`.

All 175 registered host suites and five native targets passed on unchanged
inputs. The separate 21-test CI shard-verification suite also passed. The
new host checks include the complete C5 dispatcher → S3 wire adapter → model
path, as well as production UI routes and font rendering. The optional UI
workspace is 6,664 bytes of PSRAM. The C5 history union adds four bytes to
the previous Monitor history under the host ABI; native runtime measurements
below include the complete firmware, not just that type.

Fresh S3/C5 recovery backups matched the installed rev160 images and verified
against flash. All nine bridge/C5/S3 image writes passed verification. Both
live application descriptors match the packaged version, project, SDK,
compile time and full ELF hash. The private installer also binds the matched
package to the complete build-image metadata and rechecks the frozen host,
native and package reports before each update stage.

Two idle captures 43.0 seconds apart showed advancing processor uptimes,
mounted SD and stable memory. S3 reported 134,244 internal free bytes,
a 94,208-byte largest internal block, 1,426,140 free PSRAM bytes and ten tasks.
C5 reported 53,140 free bytes and one boot. The four initial READY
timeouts/fallbacks were unchanged between captures; mutex, READY-fall,
READY-high and driver errors stayed zero. Saved home Wi-Fi, WiGLE and WDGWars
profile readiness remains set. No connection or upload was requested.

This verifies the paired installation and sampled idle state. The Analyzer
was not operated over RF during this unattended update. Both-band reception,
retuning, Stop/Restart/Back, sleep/wake, following Wi-Fi/BLE scans and loaded
memory/stack headroom still require physical acceptance. No expansion board
or live upload-service acceptance is inferred from these results.
