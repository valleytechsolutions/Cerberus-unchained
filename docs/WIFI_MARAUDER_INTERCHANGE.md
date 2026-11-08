# Marauder AP JSON and SSID text interchange

Rev125 adds offline AP JSON and SSID text import, inspection, copy saving and
export from native saved Wi-Fi observations. The native `.WFI` file remains
the report that preserves S3/C5 receiver, publication and timing information.
Implementation, build and installation evidence are recorded separately in
[the rev125 checkpoint](DUAL_RADIO_REV125.md); file compatibility alone is not
physical acceptance or complete Marauder persistence parity.

## Workflow

Open a saved `.WFI` list, choose **EXPORT**, then choose AP JSON or SSID text.
The export describes the format's information loss before starting the existing
asynchronous SD worker. The original native report and its view are retained.
Returning from the interchange view restores that report rather than assigning
an imported record to a current radio index.

For an existing file, use **SD Files → Actions → OPEN AP JSON** or **OPEN SSID
TEXT**. Ordinary `.LOG`, `.JSON` and `.TXT` file taps retain the existing text
viewer. An extension alone does not identify the contents; import is explicit
and the complete file must validate before it is published to the viewer.

Imported lists provide paged records, details and **SAVE COPY**. The AP view
retains field presence and paged station addresses. SSID records retain exact
byte lengths and a byte view as well as sanitized display text. Address privacy
also applies to imported BSSIDs and station addresses. A station listed in a
file is an imported association claim, not newly observed client activity.

Cerberus saves new generated files under `/CERBERUS/WIFI/`: `P00001.LOG` for AP
JSON and `S00001.TXT` for SSID text, with subsequent available numbers. It does
not overwrite an existing list. The pinned Marauder loaders read the fixed
names `/APs_0.log` and `/SSIDs_0.log`; a transferred copy must use the appropriate
name on that device's SD card. Cerberus does not modify another device's files.

The pinned Marauder AP loader allocates a 10,048-byte JSON document, and its
saver allocates 2,048 bytes. Those document-memory budgets are not file-size
limits with a one-to-one mapping. A format-valid Cerberus export may exceed
Marauder's available JSON memory, especially for large lists or station sets.
Cerberus's larger bounds do not guarantee that Marauder can load or re-save
every exported document. Use a smaller saved selection when needed, and check
the receiving device's reported load result.

## AP JSON contract

The root is an array. Recognized fields are `essid`, `channel`, `bssid`, `rssi`,
`packets`/`packet`, `sec`, `wps`, `man` and `stations`. Optional-field presence
is retained; absent values are not replaced by Marauder's loading defaults.
Duplicate APs, duplicate stations and order remain intact. Imported channel,
security and RSSI integers are file values, not validated current RF readings.
Receiver, clock, source generation and coverage are unknown.

The pinned upstream saver writes `packets`, while its loader reads `packet`.
Cerberus accepts either spelling, rejects conflicting values when both occur,
and emits both with the same value when the counter is present. This is
deliberate semantic normalization, not byte-for-byte JSON reproduction.
Other duplicate schema members are invalid. Unknown members directly within
each AP record object are counted. Their complete nested values are
structurally validated and discarded, without separately counting each nested
member; copy saving cannot preserve that data.

Bounds are 64 AP records, 32 decoded UTF-8 bytes per ESSID, 64 decoded UTF-8
bytes per manufacturer, 256 station addresses across the list and 65,536 input
bytes. These are Cerberus resource limits. Strings validate Unicode and retain
embedded NUL/control bytes in their explicit lengths. Malformed JSON or MAC
text, excessive nesting, overflow and capacity violations fail without
partially replacing the current document.

Export from `.WFI` keeps each receiver observation as a separate AP record.
It maps exactly representable security values to Marauder's numbering. Mixed
WPA2/WPA3, OWE, the native collapsed enterprise category and OTHER become
Marauder UNKNOWN (`255`), with a count of mappings that lost detail. An invalid
RSSI is omitted. The native report does not contain packet totals, WPS,
manufacturer strings or station lists, so export does not invent those fields.
An ESSID that is not valid UTF-8 rejects the JSON conversion atomically instead
of replacing its bytes. The native `.WFI` remains available.

## SSID text contract

The delimiter is LF, matching the pinned upstream `readStringUntil('\n')`.
There is no CRLF normalization or whitespace trimming. CR, NUL, non-UTF-8
bytes, empty names, duplicates and ordering are retained. Bounds are 64 records,
32 bytes per record and 2,112 encoded input bytes. An empty file is a valid
empty list.

The model records whether the input ends with LF: `a\n` is one record with a
trailing delimiter; `\n` is one empty record; zero bytes is zero records. A
final empty record needs its terminating LF. Export from native results keeps
all names, including empty names, and terminates a nonempty list with LF so
that its last empty record remains representable. An embedded LF in a native
name rejects the conversion rather than splitting one name into multiple rows.

SSID text retains names only. It cannot carry BSSID, channel, security, signal,
station lists, receiver, coverage or timing. This limitation is presented
before export; imported text is never proof of a currently visible network.

## Ownership, resources and remaining acceptance

The existing serialized archive worker uses distinct AP and SSID allocation
arms in optional PSRAM, without an internal-RAM fallback. The existing native
BLE, AirTag JSON and `.WFI` operation allocations remain separate. No receiver
or persistent worker task is added. Exact job ownership, cancellation and
reader pinning continue to control result publication and release.

Saving uses an exclusive temporary file, complete write/flush/sync/close checks
and final-name publication. Loading checks bounded length and EOF; the SSID
path also supports a checked zero-byte transaction. USB and other storage
ownership conflicts are reported. A failed save may leave its temporary file
for inspection. These checks do not establish power-loss atomicity or a fixed
SD latency.

Physical import/export and copy/reopen on SD, Stop/Back/sleep, privacy controls,
repeated sessions, resource headroom under load and cross-device file transfer
remain separate acceptance checks. Rev166 adds a separate [SSID Lists
workspace](SSID_LISTS.md): **EDIT COPY** previews a loaded SSID document before
explicit Append or Replace. Saving a working list creates a new file. Manual
AP editing, live-list consumer semantics, complete command/menu parity and
broader backup/restore remain open.

## Source relationship

The reference is justcallmekoko's ESP32Marauder `develop` revision
[`79cc6bcba1db6679baee1dab2b0c1ad5010e6363`](https://github.com/justcallmekoko/ESP32Marauder/tree/79cc6bcba1db6679baee1dab2b0c1ad5010e6363).
The read-only comparison at 2026-11-01 21:37:53 UTC found `develop` identical.
Relevant source is `RunLoadAPList`, `RunSaveAPList`, `RunLoadSSIDList` and
`RunSaveSSIDList` in `esp32_marauder/WiFiScan.cpp`, plus the security constants
in `WiFiScan.h`. Cerberus uses original portable codecs and its own storage/UI
integration informed by those formats. The existing
[Marauder MIT notice](../LICENSES/ESP32Marauder.txt) is retained.
