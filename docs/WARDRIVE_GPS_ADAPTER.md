# Optional GPS adapter in the main firmware

The integrated receiver uses **S3 UART2 RX on GPIO1, 9600 baud, 8N1**. GPS is
optional and defaults OFF. The UART driver and existing reader task are
allocated only when GPS is enabled for a Wardriving session; opening GPS DATA
or received NMEA does not start them. There is no GPS receiver connected to the
current test device, and physical pin routing/input remain unaccepted.

The preserved Wardriving firmware also describes GPIO2 TX and GPIO46 PPS.
The integrated adapter does not drive GPIO2 or claim GPIO46. Its original pin
reference and source hashes are recorded in
[the preserved Wardriving inventory](WARDRIVING_SOURCE_INVENTORY.md).
This is one optional peripheral service in the main application, not a separate
GPS firmware image or a second UART owner.

## Parser and evidence

`wardrive_gps_core.c` is original hardware-independent Cerberus code. The
bounded assembler accepts UART chunks, requires an exact two-digit XOR
checksum, and parses two-letter-talker RMC/GGA sentences. Coordinates, numeric
fields, dates and times are validated. It never substitutes a placeholder
location. The two-digit year uses an 80 pivot: 00-79 map to 2000-2079 and 80-99
to 1980-1999; future century handling remains a separate concern.

| Internal state | Meaning | Coordinates |
| --- | --- | --- |
| `ABSENT` | No checksum-valid sentence yet or within 10 seconds | Withheld |
| `NO_FIX` | Recent sentence, no current valid navigation fix | Withheld |
| `STALE` | Recent sentence, last valid fix older than 5 seconds | Withheld |
| `FRESH` | Current valid fix no older than 5 seconds | Available |

`ABSENT` describes received evidence, not proof of missing hardware. The GPS
view calls this **NO RECENT NMEA**. A checksum-valid sentence is not necessarily
a supported or semantically valid RMC/GGA message.

An explicit RMC V or GGA quality zero invalidates coordinates. UTC requires a
checksum-checked RMC date/time; a GGA can retain that UTC only for the same UTC
second within one second of receipt. Snapshot UTC/coordinates remain withheld
unless their existing validity rules pass. A fresh location can still have
unknown UTC, and therefore fail the stricter geotagged-row gate.

Rev122 adds GGA age and independent satellite/HDOP validity. RMC or unrelated
sentences cannot freshen those fields. Empty, invalid or older-than-five-second
values are unavailable, not known zero. Quality-zero GGA can still update valid
satellite/HDOP metadata independently of coordinates. HDOP is dimensionless;
it is never labelled accuracy in metres. Plain signed decimal grammar excludes
exponents, hexadecimal values, whitespace, NaN and infinity.

## Reader, observer and cleanup

The UART uses a 1,024-byte RX ring and the existing 3,584-byte reader stack.
The reader polls at 100 ms and feeds one synchronized parser. GPS OFF does not
consume this driver/ring/task allocation. The S3 keeps parsing optional GPS
and receiving BLE while C5 performs Wardriving Wi-Fi work.

Rev122 separates installed driver, reader, startup, stop request and error
state. An error after driver installation preserves cleanup ownership.
Current-valid data is withheld after reader failure or Stop. The existing
Wardriving worker remains STOPPING while driver cleanup is unresolved, retrying
before it frees its workspace or reports a terminal state. Radio/file draining
precedes this paced GPS cleanup. Successful release clears the current cleanup
error; the adapter retains the last UART failure separately. Snapshot reads do
not perform SDK cleanup. Start/Stop remain serialized by the existing
Wardriving worker; observer APIs do not become additional driver owners.

The received-NMEA viewer optionally attaches to that already-running reader.
An eight-row PSRAM trace stores bounded original payload bytes, LF/CRLF
termination, sequence, receiver epoch, UART receipt time and parser outcome.
Its UI copy is also optional PSRAM. There is no per-line allocation, extra
reader task or internal-RAM fallback. GPS OFF entry allocates neither trace nor
copy. Viewer exit releases the exact observer token; a lock timeout retains that
token and storage for a 250 ms paced UI retry, also while hidden or showing
DATA. Each observer lock attempt is bounded at 25 ms; begin uses two attempts
around allocation. Those are configured wait/retry limits, not measured UI
latency or a guarantee of release within one interval.

At most 127 payload bytes are retained per line. Full retention and validity
are separate: a complete bad-checksum line remains invalid; overflow is
explicitly incomplete. Parser overflow, framing/control/overflow discards and
trace evictions are reported as distinct, potentially overlapping counters.
UART loss before parsing is not measured by those counters. Sequence exhaustion
stops new trace publication without stopping the parser; retained rows remain
readable. A new receiver epoch clears the ring, preventing mixed-session rows.

The UI shows two rows per page. Paging pauses a stable value copy and displays
receipt ages at that copy time. Location starts hidden; every raw payload is
erased from hidden copies, including malformed and unrecognized lines. Hiding
also erases a paused copy immediately. Showing location again requires LIVE
to obtain new unmasked bytes. This viewer preference does not alter saved
logging or the separate address-display preference. Back to Summary and
observer cleanup do not themselves stop the existing Wardriving session.

## Recording and acceptance

Current main-firmware logging supports both unlocated and WiGLE streams; see
[GPS logging](WARDRIVE_GPS_LOGGING.md). Valid observations use exactly one
stream. GPS at result receipt is not synchronized RF observation time. Opening
these views does not change logging eligibility or initiate recording.

[Rev122](DUAL_RADIO_REV122.md) records the completed implementation, focused
checks and independent reviews. All 130 host suites, five native builds, the
linked resource audit and matched package verification pass. The native trace
ring is 1,304 bytes and its UI copy 1,320 bytes: 2,624 optional PSRAM bytes before
allocator overhead or transient failed-attachment candidates. S3 fixed data/BSS
increases by 128 bytes; C5 fixed data/BSS is unchanged. The parser core grows
from 240 to 264 bytes while its snapshot remains 104 bytes. The selected reader
entry frame grows from 288 to 320 bytes within the unchanged configured reader
stack; this is not a full call-chain bound or a measured runtime safety margin.

GPS hardware, live location display,
located-file content, sustained UART operation and PPS remain physically
unverified. Standalone GPS acquisition, generated NMEA/serial output, tracker,
POI and geofence parity are separate work.
