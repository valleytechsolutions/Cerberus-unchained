# SAE capture plan: superseded

The 2026-10-30 proposal has been replaced by the implemented
[passive SAE monitor](WIFI_SAE_MONITOR.md). Use that document and
`shared/wifi_capture_plan.h` / `shared/cerberus_protocol.h` for the current
interface and validation boundary.

The implementation uses capability **86** and commands **203, 204 and 205**.
It keeps protocol version 2 and the existing capture frame-transfer layouts.
Planned START uses **HOP | PLAN**, channel zero and SAE mode; an optional list
contains up to 32 exact BSSIDs. Both the targetless Wi-Fi tools entry and the
selected-AP capture selector open the same monitor.

The canonical plan has **42 channel candidates**, shared with Packet Count,
and a 250 ms successful dwell. It intentionally does not copy upstream's
51-entry candidate list. Actual country/driver tuning, attempted/applied masks
and cleanup ownership determine the reported receive coverage.

Live analysis uses 16 stable address-pair rows rather than a rolling frame
list. It retains public fixed metadata and body-shape counts, not token or
key bytes. Displayed ages use S3 delivery time; C5 source time is not treated
as S3 time or UTC. Optional PCAPNG records have no frame timestamps under the
current time-mapping contract.

The implementation document records the rev165 host/native checks, matched
package and paired installation separately from pending physical RF and
touchscreen acceptance. This superseded proposal is not evidence of full
upstream parity or device acceptance.
