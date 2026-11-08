# Remote ID message details

Rev169 adds **Scan → Remote ID → source → MESSAGES**. The flat PREV/NEXT
control selects Basic ID, Location, Self ID, Operator ID, Authentication or
System. DETAILS, MOTION, FOLLOW and Back remain on the same selected source.
Opening or paging through evidence starts no radio work.

## Received fields

| Group | Presentation |
| --- | --- |
| Basic ID | ID kind, aircraft type and bounded text or binary ID in hexadecimal |
| Location | Status, coordinates, pressure/geodetic altitude, height/reference, horizontal/vertical speed, direction, accuracy and advertised time within the hour |
| Self ID | Description type and bounded description |
| Operator ID | ID type and bounded broadcast identifier |
| Authentication | Type, observed page and page-zero length/timestamp metadata when present |
| System | Advertised operator-position type/coordinates/altitude, area bounds, classification and advertised timestamp |

These are unverified broadcast claims. Authentication payloads are not retained
or verified; displaying an Operator ID or System position does not establish a
person's identity or location. Receiver GPS never fills missing broadcast
fields. The existing privacy setting masks identifiers, descriptions and
positions on the new pages. Absent fields, unknown ages and unavailable
profiles remain distinct from valid zero values.

The existing strict BLE, Wi-Fi beacon and NAN containers are unchanged. The
last complete group in an accepted message pack replaces that group's values;
duplicate Location handling remains strict. Fragment assembly, unsupported
containers, cryptographic authentication and full upstream parity remain open.
Binary ID kinds retain their bytes and cannot become printable legacy IDs.
Changing the Basic ID clears the previous identity's other fields and ages.

## Two receivers, one consistent view

The C5 continues passive Wi-Fi reception on its existing worker. The S3
continues independent BLE reception and owns the inventory, frozen selection
and display. A 128-byte canonical profile holds the six groups. Each group
has its own callback age; receiving one group does not refresh another.
The age bounds describe receive callbacks, not transmitter UTC or RF time.

C5 capability 89 adds commands 206/207. BEGIN freezes a 224-byte projection
of the row, motion, profile and timing. Fourteen bounded READ chunks copy that
same projection; retries cannot mix snapshots. Full 64-bit capture epochs,
generation, row identity, projection identity and peer checks bind the result.
Old peers report the profile unavailable. A failed new-protocol read cannot
silently fall back to older data. The native S3 getter copies the equivalent
evidence under one bounded 25 ms mutex acquisition without allocation.

The UI advances one incremental row step per tick. It serializes copies into
one staging area while radio jobs continue independently, and publishes a
bank only after validating its before/after state and final owner identity.
Ordinary status polling is limited to once per 500 ms during a copy; any
command or ownership transition invalidates that cached observation. Stop
cancels unfinished copies and drains radio ownership before freeing the
workspace. A 15-second capture deadline bounds failed snapshot recovery.

A full 12-row C5 bank needs 204 row exchanges plus bank-status and ownership
checks. At the 20 ms UI cadence, this has an approximately 4.1-second cadence
floor before polling and transport overhead; it is not a measured throughput
claim. The new-protocol row exchange has a 20 ms budget, but other status work
can add time to that UI tick. Older-peer reads retain their existing budget.

Host layouts measure the optional live workspace at 23,992 bytes, including
the 14,960-byte inventory, 744-byte C5 reader and 416-byte result. These nested
sizes are not additional allocations. Each shared collector is 3,024 bytes,
2,128 more than rev168. No worker or per-row/chunk heap allocation was added.
Target build and loaded-device measurements are separate from these layouts.

## Validation and remaining acceptance

Source, host fixtures, native builds, packaging and installation are recorded
separately. Until the installation checkpoint below is populated, rev168 is
the installed baseline. Physical six-family reception, touchscreen paging,
Stop/Back/sleep, repeated following scans and loaded memory/performance require
device acceptance. Host tests and idle boot checks do not establish those.

The decoder follows the pinned OpenDroneID
[field definitions](https://github.com/opendroneid/opendroneid-core-c/blob/849c03adbe0cdd68997a462fafab397edcfe53f3/libopendroneid/opendroneid.h)
and [conversion rules](https://github.com/opendroneid/opendroneid-core-c/blob/849c03adbe0cdd68997a462fafab397edcfe53f3/libopendroneid/opendroneid.c).
No third-party implementation was imported. Marauder `develop` was rechecked
on 2026-11-08 and remains at
`79cc6bcba1db6679baee1dab2b0c1ad5010e6363`.
