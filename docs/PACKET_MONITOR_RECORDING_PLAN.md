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

The correction keeps the error and attempted state so cleanup can be retried.
The production-function fixture fails on the original implementation and passes
after the correction. It covers a delayed cleanup error, another failed retry,
a successful retry, missing error detail, normal completion, timeout and
transport failure. The existing C5 restore and shared-worker checks also pass.
The S3 native build and its READY ISR/console checks pass with unchanged build
inputs. This targeted correction has not been flashed or physically exercised.

This is a source correction, not a diagnosis of every earlier physical Wi-Fi
failure. The connected device remains on the rev145 TLS checkpoint. This branch
has not been installed, and Packet Monitor recording remains unimplemented.

## Remaining implementation

1. Bind Start, Stop, status and frame reads to an exact monitor session. The
   current generation byte and same-channel fallback do not identify an owner
   after a lost Start reply or peer restart. Preserve the claim through transport
   errors, keep cleanup retryable, and prevent old commands from affecting a
   replacement session. A receive lease must end abandoned reception.
2. Move cleanup polling out of the blocking touchscreen Stop loop. Back,
   channel changes, restart and sleep need explicit pending actions which
   complete only after the claimed receiver releases its radio. Preserve the
   existing channel-wide scope; opening from an AP must not imply BSSID filtering.
3. Add optional bounded C5 frame recording alongside its counters. Graph-only
   operation must allocate no frame queue. Capture metadata and bytes in the
   receive callback; keep SPI, SD writes and dynamic allocation outside it.
   Preserve the documented SDK payload extent and reject payload-less callbacks.
4. Use S3 memory and the serialized SD writer to assemble validated frame reads
   and save a C5-labelled PCAPNG file. Preserve the source clock, channel and
   RSSI without inventing UTC. Report queue drops, oversized/truncated frames,
   transport loss and SD failures separately from graph counts. Retuning must
   finish the old segment before admitting the next one.
5. Add the recording control to the existing Packet Monitor page. Saving,
   Stop/Back and the next Wi-Fi scan require device acceptance, including slow
   SD, 2.4/5 GHz, repeated use and loaded memory measurements. A successful build
   or idle boot does not satisfy those checks.

C5 reception and S3 control, analysis and storage are useful work for both
processors. A second receiver should be added only with a distinct channel or
coverage benefit and independently labelled observations.
