# Find a saved Bluetooth address

Rev163 adds a live lookup from a saved native `.BLE` record or Marauder Find My
JSON record. Open the record, choose **FIND LIVE**, then **START**. The S3 and
C5 independently listen for that saved address. Select a fresh receiver/type
row to enter the existing dual-receiver Fox Hunt. Back returns to the original
saved detail, including its selected record, raw page and parent document.

Opening a file or the lookup page does not start either radio. Stop, Back and
sleep cancel the lookup intent; exact attachments continue draining until
their owners release. If the selected observation expires during cleanup,
the page asks for a fresh selection instead of opening the tracker.

## What the rows mean

Saved JSON contains an address and opaque payload, without a proven current
address type, receive time, RSSI or receiver. Native snapshots also contain
historical observations. Neither format supplies a current tracking sample.
The lookup uses only the six-byte saved address as its selector, discovers
actual address types from new callbacks, and retains separate source/type
rows. The maximum is eight candidates: four types from each receiver.

S3 signal is the current sweep's peak RSSI; C5 signal is from the retained
original advertisement. Advertisement age remains source-derived. C5 age is
an upper bound anchored before its immutable snapshot request. Copying or
painting a row never makes it new. Only a candidate no older than five seconds
can be selected, and freshness is checked again after both lookup owners
release. Fox Hunt begins with no current signal until its own callbacks arrive.

A malformed newer original retires the corresponding candidate. An
unavailable signal is not presented as a usable RSSI. A matching address does
not authenticate a physical tag, establish full-payload equality, or follow a
device after its address rotates. The workflow adds no connection, GATT,
advertising or sound operation.

## Radio and memory use

Both receivers filter the address before inventory admission and activity
accounting, so unrelated advertisements cannot fill the candidate list.
The S3 uses a scoped passive attachment; ordinary active discovery is
unchanged. The C5 uses passive mode 6 on the existing 22-byte START request,
with an explicit unknown-type selector. Capability 84 is required before
that request can be sent. Its existing immutable 110-byte metadata snapshots
can be read during reception; no new SPI command is needed.

The coordinator reuses the existing C5 reader and S3 discovery controller.
It adds four S3 staging records and eight compact candidates in an optional
PSRAM workspace. The model is 23,712 bytes and the complete optional UI
workspace is 24,328 bytes under the host ABI; these are allocation
measurements, not measurements of loaded hardware headroom.
No extra radio task or packet queue is added. Each receiver retains its
existing finite scan procedure and continuous rearm lifecycle. Either can
report failure independently while the other continues.

## Upstream reference and acceptance

Marauder `develop` was checked on 2026-11-08 and still resolves to
`79cc6bcba1db6679baee1dab2b0c1ad5010e6363`. The reference workflow includes
`RunLoadATList()` in `WiFiScan.cpp`, **Load Airtags** in `MenuFunctions.cpp`,
and the selected-MAC `BT_SCAN_FOX_HUNT` route. Cerberus keeps imports separate
from live observations and adds a fresh, typed reacquisition step before
tracking. This closes part of the saved-list workflow gap; full upstream
feature parity remains open.

Focused fixtures exercise production S3 callbacks, C5 callbacks/service,
the SPI adapter/reader, both lifecycle controllers and the UI. They cover
crowded unrelated traffic, all four types, stale epochs, malformed reports,
source age, old firmware, lost replies, independent failure, failed Stop,
candidate expiry, restart and saved-document retention.

All 181 registered host suites and five native targets passed on unchanged
inputs. The separate 21-test CI shard verifier also passed. Real-font previews
were inspected for empty, S3, C5, privacy and partial-error states.

Physical reception from an owned saved tag, touch navigation, dual-radio
tracking, sleep/wake, later Wi-Fi/BLE scans and sustained loaded memory/stack
acceptance remain pending. Host fixtures and firmware builds do not establish
those results.

## Installed checkpoint

Rev163 is installed from source `20c69fecf9e1a18ac771fd5d203fdad43ebfb45e`, tagged
`checkpoint/rev163-saved-ble-lookup`. Package:
`cerberus-unchained-0.1.0-dev.rev163-20261108T094208Z`.
Manifest SHA-256:
`d63582f684f36f22d8c3e0611471872a459d9967f832c18e58446f9036ffa2db`.

Fresh rev162 S3/C5 recovery backups were verified against flash and the
installed baseline. All nine bridge/C5/S3 image writes verified. Both live
application descriptors match the packaged version, project, SDK, compile
time and full ELF hash. The card mounted and saved home Wi-Fi, WiGLE and
WDGWars readiness remained set. No network connection or upload was requested.

Two idle captures 52.7 seconds apart showed advancing uptimes and SPI
exchanges, one C5 boot, and no additional listed transport errors. The four
initial READY timeouts/fallbacks did not increase; mutex, READY-fall,
READY-high and driver errors remained zero. S3 retained 134,196 internal
free bytes, a 90,112-byte largest internal block, 1,426,140 free PSRAM bytes
and ten tasks across those captures. C5 retained 53,012 free bytes.

Compared with the captured rev162 baseline, S3 internal free memory is
48 bytes lower and the largest internal block is 4,096 bytes smaller; PSRAM
and task counts match. These idle measurements do not establish loaded
headroom. The console stack low-water changed from 2,084 to 916 bytes after
diagnostic commands; it is recorded rather than treated as constant or as
loaded-service acceptance. The lookup's optional workspace was not allocated
during these idle checks.

Installation and idle checks do not validate physical saved-tag reception,
source/type selection, Fox Hunt, touchscreen return paths, sleep/wake or
sustained performance. Those remain open alongside full Marauder parity.
