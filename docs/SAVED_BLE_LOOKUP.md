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

Rev162 remains the installed baseline while rev163 is being validated.
Physical reception from an owned saved tag, touch navigation, dual-radio
tracking, sleep/wake, later Wi-Fi/BLE scans and sustained loaded memory/stack
acceptance remain pending. Host fixtures and firmware builds do not establish
those results.
