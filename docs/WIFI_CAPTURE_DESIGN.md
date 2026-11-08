# Passive frame capture and export

Rev168 adds a scoped FRAME CAPTURE workspace at **Scan -> Wi-Fi -> TOOLS ->
RX TOOLS -> FRAME CAPTURE**. The same flat RX TOOLS chooser contains EAPOL
and SAE monitors. The selected-AP **CAPTURE THIS AP** shortcut opens the same
frame workspace with its AP and return destination copied. Opening a page
starts no receiver.

The candidate implementation and focused checks are described here. The
installed baseline is rev167 until a later checkpoint section records the
matched build and installation. Physical reception, recording readback and
loaded throughput remain separate acceptance checks.

## Controls and scopes

The receive filter is **RAW**, **PROBES**, or **DEAUTH / DISASSOC**. Scope is
**ALL**, **2.4 GHZ**, **5 GHZ**, **FIXED CHANNEL**, **SELECTED APS**, or the
AP-origin **THIS AP**. Selected identities and channel hints are copied at
Start. Later inventory changes cannot alter that run. The AP picker opens
paused with the held inventory; SCAN explicitly requests another sweep.

PROBES is deliberately channel-wide: its corresponding scopes say
**SELECTED CHANNELS** and **AP CHANNEL**, with no BSSID filter. Wildcard probe
requests cannot be attributed to a selected AP. RAW/disconnect target lists
use exact BSSIDs; control and WDS traffic with no resolvable BSSID is excluded
from those filtered scopes. Targetless RAW retains these classes when the
controller delivers them, without inventing an association.

SAVE PCAP defaults to **ON**, preserving the existing recording behavior.
Turn it OFF for analysis without an SD lease. Stop retains the report; Restart
replaces it only after successful ownership reservation. Back, hidden pages
and sleep cancel pending Starts and keep cleanup active until the exact
receiver generation, frame drain and file close have released. Changing
monitor family also waits for release.

## Division of work and data quality

C5 owns passive reception, filtering, channel scheduling and its existing
**two-slot, 4,096-byte frame ring**. S3 owns verified frame assembly, the
bounded report, controls and optional SD writing. No extra RF worker, queue
or SD worker is added. The new frame session and UI allocate their evidence
in PSRAM only when needed.

Planned RAW uses **100 ms after successful receiver arming**; PROBES and
DEAUTH/DISASSOC use **250 ms**. Single-channel plans receive continuously.
The canonical 42-channel plan exposes driver tune outcomes rather than
claiming all 51 upstream candidates are supported. A selected AP's channel
is a historical hint. SAE keeps 250 ms and EAPOL keeps 2,000 ms; family/dwell
mismatches fail before Start. Scoped FRAME CAPTURE requires capability **88**.

RAW explicitly snapshots the separate SDK control-subtype filter, enables
control reception and restores the original filter during cleanup. A failed
restore retains radio ownership for retry. Upstream RAW requests management
and data frames only; control reception is a Cerberus extension, subject to
actual controller delivery and independent physical acceptance.

The report keeps **C5 ADMITTED**, **S3 DELIVERED**, **SD WRITTEN**, queue-full
drops and other drops separate. C5 admission is not a count of everything
heard on the channel. Summary classes and RSSI extrema describe verified
records delivered to S3. Complete MAC header lengths, direction, QoS/WDS/HT
extensions and LLC/EAPOL bounds are checked before inferring a subtype or
EAPOL header. Protected, fragmented and A-MSDU data cannot supply a plaintext
EAPOL claim. Retry bits count observations; they do not deduplicate captures.

A bounded view retains **16 recent disconnect-evidence tuples** with BSSID,
transmitter, receiver, subtype, channel, RSSI, C5 source timestamps and S3
delivery ages. Matching tuples accumulate observations; new tuples replace
the least recently seen
slot and increment an explicit replacement count. Admission sequence changes
reset row age when a slot is reused. Plaintext reason codes require a clear,
complete, unfragmented body; otherwise the reason is unknown. Public frame
addresses and reason fields do not identify a physical device or establish
attack attribution.

Frame sequences never wrap back to an earlier ID. Exhaustion closes producer
admission and reports an error through the existing capture status; the final
pinned record can still drain. A later generation can start fresh after
release. Per-arm callback tokens, source-time fencing, CRC checks and once-only
observer delivery continue to protect the shared stream.

## Storage ownership

The production path uses `wifi_capture_service.c`, the capture SPI commands,
`wifi_capture_session.c` and the leased append writer in `storage.c`. The
serializer below is connected to that path. The worker assembles one frame
in PSRAM, verifies it, analyzes it, optionally appends the PCAPNG block, then
acknowledges its pinned C5 sequence. Duplicate transport delivery cannot append
or analyze the same record twice. A failed record still releases its pin;
its error remains visible and stops the session.

Recording has a **64 MiB file limit** and explicit manual Stop. SD access
honors filesystem and USB mass-storage ownership. A failed or interrupted
write is not silently claimed as saved. The optional file retains complete
received frames up to the frame cap, while the report retains only bounded
header evidence. This is not a lossless full-channel recorder: dense RAW
traffic can overflow the ring or outpace SPI/SD processing.

## File format and time

The codec emits [PCAPNG](https://datatracker.ietf.org/doc/draft-ietf-opsawg-pcapng/)
with one Section Header Block and one Interface Description Block at file
creation. The IDB uses LINKTYPE_IEEE802_11_RADIOTAP (127), snaplen 4112,
`if_name` of `C5`, `S3` or `R1` through `R6`, and explicit microsecond
`if_tsresol`. **Use one file per physical receiver.** A Simple Packet Block
implicitly belongs to the first interface, so combining radios in one file
would mislabel one of them.

Each record begins with a 16-byte [radiotap](https://radiotap.org/) header:
Flags, [Channel](https://radiotap.org/fields/Channel) frequency and band flag,
and optional signed [Antenna signal](https://radiotap.org/fields/Antenna%20signal.html)
in dBm. The channel flag is `0x0080` for 2 GHz and `0x0100` for 5 GHz.
The [Flags](https://radiotap.org/fields/Flags.html) `0x10` bit is set only when
the captured frame actually includes an FCS. The caller must establish that
from driver metadata; the codec does not infer it from frame bytes. RSSI is
omitted from radiotap when unknown. The supported channel range is 1–14 or
32–177; the codec does not claim a regulatory channel policy or 6 GHz support.

For **verified Unix microseconds**, a record is an Enhanced Packet Block with
an explicit UTC timestamp. A plausible date by itself does not verify the
clock. For **monotonic-only time**, a record is a Simple Packet Block with no
timestamp. The [PCAPNG specification](https://datatracker.ietf.org/doc/draft-ietf-opsawg-pcapng/)
defines the enhanced packet timestamp against UTC, while the simple block has
none. A boot-relative count in the enhanced packet field would appear as a
false wall-clock time. The capture owner must save the monotonic microseconds,
radio boot/session ID and record sequence in a sidecar index if relative timing
is needed. GPS absence does not prevent capture; it means no GPS-derived UTC.
Until C5 and S3 clock offset and error are measured, do not convert one radio's
monotonic count using the other radio's wall clock.

The current FRAME CAPTURE path writes untimed Simple Packet Blocks and does
not write a timing sidecar. Verified-UTC Enhanced Packet Blocks are a codec
capability, not an enabled feature of this capture path.

`original_length` is the raw 802.11 length, excluding the synthetic radiotap
prefix. The codec enforces a 4096-byte maximum captured frame and a 65,535-byte
maximum reported original frame. It emits at most 4144 bytes per record. It
rejects malformed channels, invalid length relationships, a known positive
RSSI, an FCS claim shorter than four bytes, an implausible UTC numeric value,
invalid time kind or insufficient output capacity without modifying the output.
Simple Packet Blocks cannot encode an arbitrary captured length; truncation is
accepted there only when the 4096-byte frame cap is reached, so a reader can
recover the captured length from snaplen. Otherwise the caller must drop the
record and count it. Enhanced Packet Blocks encode both lengths explicitly.

## Source comparison and validation boundary

The 2026-11-08 develop comparison still resolves to
`79cc6bcba1db6679baee1dab2b0c1ad5010e6363`:

- [Raw Capture menu](https://github.com/justcallmekoko/ESP32Marauder/blob/79cc6bcba1db6679baee1dab2b0c1ad5010e6363/esp32_marauder/MenuFunctions.cpp#L5218) and [initialization](https://github.com/justcallmekoko/ESP32Marauder/blob/79cc6bcba1db6679baee1dab2b0c1ad5010e6363/esp32_marauder/WiFiScan.cpp#L6917).
- [Raw callback](https://github.com/justcallmekoko/ESP32Marauder/blob/79cc6bcba1db6679baee1dab2b0c1ad5010e6363/esp32_marauder/WiFiScan.cpp#L9149), [report](https://github.com/justcallmekoko/ESP32Marauder/blob/79cc6bcba1db6679baee1dab2b0c1ad5010e6363/esp32_marauder/WiFiScan.cpp#L11363) and [100 ms scheduling](https://github.com/justcallmekoko/ESP32Marauder/blob/79cc6bcba1db6679baee1dab2b0c1ad5010e6363/esp32_marauder/WiFiScan.cpp#L12896).
- [Deauth Sniff menu](https://github.com/justcallmekoko/ESP32Marauder/blob/79cc6bcba1db6679baee1dab2b0c1ad5010e6363/esp32_marauder/MenuFunctions.cpp#L5152) and [passive callback](https://github.com/justcallmekoko/ESP32Marauder/blob/79cc6bcba1db6679baee1dab2b0c1ad5010e6363/esp32_marauder/WiFiScan.cpp#L9407).

Cerberus does not copy upstream's unsafe fixed-offset EAPOL check or classify
all non-management frames as data. Class totals are bounded observations, and
unknown/malformed input stays explicit. The two-second EAPOL and quarter-second
SAE workflows remain separate monitors. Selected scope, reason visibility and
control reception are explicit Cerberus behaviors, not claims of exact CLI or
all-option parity.

Required acceptance includes controlled raw/probe/disconnect reception on
both bands, comparison against an independent receiver, control-subtype
coverage and restoration, PCAPNG readback, slow/full/removed SD behavior,
repeated Stop/Back/next-scan and sleep/wake, and loaded heap/stack/throughput/drop
measurements. Host fixtures and an idle boot cannot establish those outcomes.
