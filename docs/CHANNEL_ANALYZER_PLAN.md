# Channel Analyzer implementation plan

This is the pre-implementation source audit recorded at rev160. Rev161 adds
the [Channel Analyzer workflow](CHANNEL_ANALYZER.md); that document records
its implementation and validation boundaries. The proposal below preserves
the original acceptance criteria and does not establish physical acceptance.

## Upstream behavior

The reference is JustCallMeKoko and the ESP32 Marauder contributors' pinned
develop commit `79cc6bcba1db6679baee1dab2b0c1ad5010e6363`.

- [MenuFunctions.cpp:5206](https://github.com/justcallmekoko/ESP32Marauder/blob/79cc6bcba1db6679baee1dab2b0c1ad5010e6363/esp32_marauder/MenuFunctions.cpp#L5206)
  exposes **Channel Analyzer** under `WIFI_SNIFFER`. The bundled inventory's
  corresponding literal menu index is **201**.
- [WiFiScan.cpp:10304](https://github.com/justcallmekoko/ESP32Marauder/blob/79cc6bcba1db6679baee1dab2b0c1ad5010e6363/esp32_marauder/WiFiScan.cpp#L10304)
  counts Analyzer callbacks and occasionally updates a recent beacon
  SSID/BSSID and RSSI hint. That hint is not selected-target tracking.
- [WiFiScan.cpp:11102](https://github.com/justcallmekoko/ESP32Marauder/blob/79cc6bcba1db6679baee1dab2b0c1ad5010e6363/esp32_marauder/WiFiScan.cpp#L11102)
  samples and clears the interval counter into a scrolling history at the
  board's `BANNER_TIME` cadence, generally 50 or 100 ms. The screen supports
  channel minus/plus and exit.
- [MenuFunctions.cpp:5779](https://github.com/justcallmekoko/ESP32Marauder/blob/79cc6bcba1db6679baee1dab2b0c1ad5010e6363/esp32_marauder/MenuFunctions.cpp#L5779)
  draws history, maximum and average overlays. A
  [manual channel change](https://github.com/justcallmekoko/ESP32Marauder/blob/79cc6bcba1db6679baee1dab2b0c1ad5010e6363/esp32_marauder/WiFiScan.cpp#L10979)
  inserts a labeled channel marker.

There is a ChanHop toggle handler, but the audited
[main Analyzer branch](https://github.com/justcallmekoko/ESP32Marauder/blob/79cc6bcba1db6679baee1dab2b0c1ad5010e6363/esp32_marauder/WiFiScan.cpp#L12875)
does not call `channelHop`. Automatic hopping is not established by the toggle
alone. No direct Channel Analyzer CLI route was found in this audit.

## Existing Cerberus boundaries

[Channel Activity](CHANNEL_ACTIVITY.md) samples successive channels and shows
their latest windows. Packet Monitor has fixed-channel history for beacons,
disconnection frames and probe requests at 200 ms intervals. Those three
subtypes do not provide Analyzer's total-channel traffic history. Neither
existing view establishes an implemented Analyzer workflow.

Useful implementation surfaces are `wifi_packet_monitor_service.h` on C5,
`wifi_packet_monitor_scan.c` and `wifi_packet_monitor_ui.h` on S3, and the
bounded beacon decoder in `shared/wifi_scan_observation.h`. The Monitor service
already supplies a shared scanner worker, exclusive radio reservation, exact
session, renewable lease and retained cleanup when restoring the radio fails.

## Proposed implementation

C5 receives on one selected 2.4 or 5 GHz channel. S3 validates samples, keeps
history, renders the graph and handles navigation. An explicit Analyzer route
would belong in the existing Wi-Fi tools menu, optionally seeded from a
Channel Activity row. It would not require an AP selection, and its display
would identify channel-wide traffic.

Share the Monitor worker and reservation with a private mode discriminator.
Preserve existing Monitor commands, payloads and cap81 behavior. A proposed
cap82 extension would add Analyzer-specific INFO, START, STATUS, STOP, SAMPLE
and hint-projection commands after the currently allocated command 190.
Analyzer must reject older peers explicitly. Cross-mode calls must not adopt,
read or stop the other workflow's session.

Proposed packed payloads, each 22 bytes:

```c
struct analyzer_status {
    uint64_t session;
    uint32_t latest_sequence;
    int32_t error;
    uint16_t target_ms;
    uint8_t state, channel, stored_count, flags;
};

struct analyzer_sample {
    uint64_t session;
    uint32_t sequence, frames, window_us;
    uint8_t channel, flags;
};
```

Count successful MGMT, CTRL and DATA receive callbacks on the owned channel.
Do not inherit Monitor's minimum management-frame length when counting short
control frames. Only bounded, valid beacon parsing updates the name hint.
Labels must not equate callback counts with unique packets, device counts,
airtime utilization or authenticated identity.

Use a 100 ms target with measured receive-window duration. Handle counter
saturation and invalid clocks explicitly; canceled partial windows must not
be published as complete samples. A 64-entry ring of 14-byte sample bodies can
share a union with Monitor's existing ring. This proposal adds no radio worker.
Use 32-bit sequences and defined exhaustion behavior, not silent reuse.

Keep a small latest beacon record plus an immutable projection for chunked
readout. Bind chunks to the exact session, projection ID and part. A 22-byte
chunk can carry an eight-byte session, four-byte projection, one-byte part
and nine data bytes. A changing beacon must not mix fields between chunks.
Retain exact SSID bytes/length, BSSID, RSSI, actual channel and receipt age;
escape names for display and identify stale hints.

S3 can allocate approximately 320 history entries in optional PSRAM. The
screen should show current count/rate, measured time history, peak and average,
channel markers, recent beacon hints, CH−/CH+, Stop/Restart and Back. Define
average rate as total known frames divided by total known receive time.
Missing samples are gaps, not zeros. Retuning releases the old exact session
before starting the new channel and inserts a visible transition gap/marker.
Back must supersede pending retunes, and sleep must use the same release path.

## Acceptance gates

- Compile the actual callback/parser path with successful and failed RX,
  short control frames, foreign channels, malformed/duplicate/binary/hidden
  SSIDs and saturation cases.
- Exercise the real worker with delayed sampling, clock failures, cancellation
  mid-window, lease expiry and failed radio disable/restore retries.
- Check cross-mode ownership, stale sessions/sequences, lost START replies,
  C5 reboot, hint projection replacement and changing hints during readout.
- Check S3 history rollover, missing/duplicate samples, channel markers,
  unknown versus zero, duration-weighted averages and overflow boundaries.
- Extract and render actual UI routes, including entry, retune, Stop/Back
  during startup, repeated taps, sleep and a subsequent normal Wi-Fi scan.
- Preserve existing Monitor and cap81 tests. The production-bound worker,
  dispatcher, SPI adapter and UI harness in
  `tools/check_wifi_packet_monitor_session.py` is a suitable starting pattern.
- Run the complete host/native/package checks before any paired update.
  Physical 2.4/5 GHz reception, timeline accuracy, Stop/Back/restart,
  Wi-Fi/BLE transitions and sustained memory/throughput require separate
  device acceptance.

Keep the ledger pending until an actual Analyzer workflow exists. Subsequent
implementation, software verification and hardware acceptance must be tracked
separately; a renamed Packet Monitor or a graph label does not satisfy this scope.
