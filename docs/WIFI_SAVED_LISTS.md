# Saved Wi-Fi observations

Rev124 adds saving and reopening the combined S3/C5 Wi-Fi results. These files
retain observations from a completed scan, rather than a set of live targets.
The workflow is included in installed rev124; physical save/reopen acceptance
remains pending. Build, installation and acceptance evidence are recorded in
[the rev124 checkpoint](DUAL_RADIO_REV124.md).

## Workflow

Complete a Wi-Fi scan, then use **SAVE ALL** or select rows and use **SAVE
SELECTED**. The save selection is separate from the existing AP tools and
action selection. A successfully saved file opens in the saved-list viewer.
Later, open its `.WFI` file through SD Files.

The viewer offers AP rows and an SSID grouping view, receiver and band filters,
stable ordering, individual details, and **SAVE COPY**. Each saved receiver
observation remains separate even when two rows describe the same AP. An SSID
group is a presentation of equal received names; it is not proof that its APs
share an operator. Hidden names remain separate. Address privacy applies to
the viewer. **NEW SCAN** opens ordinary discovery and does not reuse an archived
index or generation as a current target.

SD operations run through the existing serialized archive worker. Opening a
file does not start either radio. Saving first freezes the completed result
and drains only work owned by the dual Wi-Fi workflow. Leaving or sleeping
cancels the view; an already-started save can still finish, but its completion
cannot reopen a page the user left. Files use new generated names such as
`/CERBERUS/WIFI/W00001.WFI`; existing files are not overwritten.

## Retained evidence

The coordinator publishes a small metadata record with the retained scan
bank. A retry, failed scan, Stop or AP rebind cannot mix the old rows with a
new operation's metadata. The format retains up to 32 observations from each
receiver, 64 total, together with their original publication counts, coverage,
split/fallback state and source errors. Saving a subset preserves those
original totals and marks the file as a subset.

Each row keeps its received SDK SSID string, BSSID, channel, security value,
RSSI and validity, receiver, source index/generation and original local
receipt field. The SDK string is limited to 32 bytes before its terminating
NUL; it is not a lossless raw on-air SSID or beacon. RSSI is a driver aggregate,
not a reading whose acquisition time is known from the row's receipt time.

Where a C5 matching-frame observation exists, its source-age upper bound is
saved using the same captured local clock sample. This dates matching frame
presence, not SDK signal/security measurements. S3 frame timing is unavailable.
A missing or backward clock leaves save timing explicitly unknown. Local
uptime, source generation and session identifiers remain historical metadata;
they do not establish UTC time, current presence or permission to use a live
radio target. No GPS is required or inferred.

## File and resource contract

Version 1 uses explicit little-endian fields, a 96-byte header and 80 bytes per
row, at most 5,216 encoded bytes. CRC checks accidental corruption; it does
not authenticate the file. Decoding validates the entire document before
publication, including exact length, canonical padding, source counts and
indices, flags, channel/security values, timing consistency and duplicate
source-index rejection. The codec allocates no heap and rejects overlapping
inputs/outputs. Bad or truncated files leave the current output unchanged.

The optional viewer and worker use PSRAM, with no internal-RAM fallback. The
existing BLE and Find My JSON worker allocations are unchanged. Saving uses
an exclusive temporary file, checks complete write/flush/sync/close, then
publishes the final name. Loading checks bounded length and complete EOF.
Storage ownership prevents conflict with USB mode or an active stream.
A failed save may retain a temporary file for inspection. These software
checks do not promise power-loss atomicity or a fixed SD latency.

## Upstream relationship and remaining scope

The Marauder develop reference was verified at
`79cc6bcba1db6679baee1dab2b0c1ad5010e6363` on 2026-11-01 20:37:11 UTC.
Its reachable AP and SSID save/load commands informed the workflow audit.
The Cerberus `.WFI` format and implementation are original and preserve the
two receivers' provenance. They are not Marauder `/APs_0.log` JSON or
`/SSIDs_0.log` text interoperability.

Rev125 adds a separate [Marauder AP JSON/SSID text interchange workflow](WIFI_MARAUDER_INTERCHANGE.md)
with explicit information loss; `.WFI` remains the source-preserving format.
Manual list editing, full option parity and load-to-live-list behavior remain
separate work. Saving reports does not
resolve previously reported Wi-Fi action failures. Physical save/reopen,
Stop/Back/sleep, repeated sessions and sustained resource acceptance must be
distinguished from the host fixtures and native build results.
