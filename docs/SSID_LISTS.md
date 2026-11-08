# SSID lists

The rev166 candidate adds an editable, names-only workspace at **Tools ->
Extras -> SSID Lists**. It keeps a working list in optional S3 PSRAM, supports
explicit imports and saves new SD copies through the existing archive worker.
Opening or editing a list does not acquire a radio or change scan results.

This document describes the candidate source. The complete 194-suite host
matrix, native builds and physical acceptance remain pending at this checkpoint;
it does not claim that rev166 is installed. C5 capability remains 86.

## Using the workspace

The list displays four names per page. Tap a name for its length and exact hex
bytes, or tap its checkbox to select it. **ADD NAME**, **EDIT TEXT** and
**EDIT HEX** create or change a name. Text editing accepts printable ASCII;
hex editing preserves binary bytes. **USE EMPTY** creates a zero-length name.
Privacy masking hides names and hex values and cancels an open editor when
privacy is enabled.

**LIST OPTIONS** provides Select All, Select None, Remove Selected, Clear
Working List and generated names. Removal and replacement require an explicit
confirmation. Generated names use `CERB-` followed by eight uppercase hex
digits from four random bytes. The count is adjustable from 1 to 64, subject
to free list capacity; generated names are not guaranteed unique.

**OPEN FILE** starts the SD picker in `/CERBERUS/WIFI`. Choose an SSID `.TXT`
or `.LOG` file to load a preview. **APPEND** adds the preview to the working
list, while **REPLACE LIST** confirms replacement. **APPEND: KEEP ALL** is the
default; **APPEND: UNIQUE** skips byte-identical names already present or
earlier in that import. Replacement preserves duplicates and order.

Names can also enter the same explicit import preview from these routes:

| Source | Action and scope |
| --- | --- |
| Saved SSID text viewer | **EDIT COPY** copies the loaded document before closing the historical viewer. The original file is unchanged. AP JSON retains its separate viewer. |
| Wi-Fi AP detail | **COPY NAME** copies the existing displayed, NUL-terminated name, up to 32 bytes. This legacy AP-display path is not a guarantee of the raw binary SSID from the received frame. |
| Stopped Probe Sniff detail | **COPY NAME TO LIST** copies the frozen row's exact bytes and length, including binary or empty names. The row must belong to the current stopped session and the receiver must be released. Active observations cannot be copied through this control. |

A failed copied import leaves the source view available. A successful import
copies the document before the source view is disposed. Back returns to the
originating screen; returning to retained dual Wi-Fi results does not start a
new scan. Neither a copied AP name nor a copied probe name carries a
BSSID, channel, RSSI, live selection or authorization to act on an AP.

**SAVE COPY** saves the entire working list, including unselected rows. It
creates a new `/CERBERUS/WIFI/S#####.TXT`, such as `S00001.TXT`; selection is
workspace state and is not part of the file. Back and sleep retain the working
list and its unsaved status for the current boot. Save and later Open File are
the persistence path across a restart.

## File and model contract

The bounded model holds at most **64 names of 0-32 bytes each**. The existing
SSID text codec separates rows only at LF (`0A`). It does not trim names,
normalize CRLF, validate UTF-8 or remove duplicates. NUL, CR and non-UTF-8
bytes are retained; an LF inside one name cannot be represented and is
rejected by editing/import validation. A CRLF input therefore retains CR as
the final byte of each name rather than silently changing it.

An empty file contains zero names. `a\n` contains one name and records a final
LF; `\n` contains one empty name. Imported framing is preserved by Replace.
Manual content changes and Append use a final LF when nonempty. The maximum
encoded size is 2,112 bytes. An invalid document, capacity failure or exhausted
revision counter leaves the complete working model unchanged; imports never
partially append.

Byte length and bytes define equality, including empty and binary names.
Unique append retains the first occurrence and original order. Existing
selection survives Append, new rows are unselected, and Replace clears
selection. Removing rows remaps selection to the remaining rows.

Actual changes advance a nonwrapping 64-bit revision. Content changes mark
the list dirty; selection alone does not. UI actions are checked against the
revision that was displayed. Import application also checks the exact preview
identity. Stale taps, drafts or replacement confirmations cannot apply to a
different list. If a preview becomes unavailable, the UI clears its copied
preview and returns to the working list.

## Storage ownership and resource use

`ssid_list.[ch]` is a pure model with no allocation, filesystem or radio calls.
`ssid_list_session.[ch]` lazily allocates one optional PSRAM workspace and a
separate import preview. The UI owns a separately allocated view. There is no
new worker, queue or C5 command for this feature: serialized load/save work
uses `ble_archive_io`, the existing codec and the existing storage lock.

The save API copies the complete document before returning. Edits can continue
while that immutable snapshot is saved. A successful save clears dirty only
if its saved revision still matches the working revision; a later edit stays
unsaved. The storage writer selects an unused index, exclusively creates its
temporary file, flushes and closes it, checks the destination again, then
renames it. It does not overwrite the imported file or an existing saved
copy. Failed temporary files are retained and their indices are skipped.

Back, hidden navigation and sleep cancel/release only the exact archive job
owned by this session. The closing page remains until ownership is released;
lost or contradictory status cannot authorize early disposal or a different
job. An in-flight save may still complete, which the closing page states.
The deliberate Open File detour retains the workspace while the picker is
open. Worker-owned buffers are never freed by the view.

The S3 performs this local document work; the C5 is not started just to copy
names. This milestone does not add list consumption by attacks or claim that
those workflows are complete.

## Upstream comparison

The reference is JustCallMeKoko's Marauder develop commit
`79cc6bcba1db6679baee1dab2b0c1ad5010e6363`, inspected in the pinned local
checkout. Its reachable Wi-Fi General menu includes Generate SSIDs, selected
probe-name import, Add SSID and Clear SSIDs; the CLI exposes list/select,
add/generate/remove and save/load operations. See the upstream
[menu implementation](https://github.com/justcallmekoko/ESP32Marauder/blob/79cc6bcba1db6679baee1dab2b0c1ad5010e6363/esp32_marauder/MenuFunctions.cpp#L3557-L3656)
and [SSID CLI handling](https://github.com/justcallmekoko/ESP32Marauder/blob/79cc6bcba1db6679baee1dab2b0c1ad5010e6363/esp32_marauder/CommandLine.cpp#L2179-L2214).

Marauder's load routine appends LF-delimited names from `/SSIDs_0.log`; its
save routine writes the list without a final LF after the last name. Cerberus
reuses that names-only text representation, accepts either final-LF framing,
and makes import application explicit. Cerberus writes numbered copies in its
own SD namespace. See upstream
[load/save routines](https://github.com/justcallmekoko/ESP32Marauder/blob/79cc6bcba1db6679baee1dab2b0c1ad5010e6363/esp32_marauder/WiFiScan.cpp#L4292-L4363).

Upstream `addSSID` and generation also assign random channels and MAC values.
Cerberus's workspace deliberately stores names only; it does not invent AP
identity. Its generated naming format also differs from upstream's six-letter
names. See upstream
[add/generate routines](https://github.com/justcallmekoko/ESP32Marauder/blob/79cc6bcba1db6679baee1dab2b0c1ad5010e6363/esp32_marauder/WiFiScan.cpp#L2332-L2362).

`LV_ADD_SSID` is a legacy numeric definition in `WiFiScan.h` and `Display.h`
with a display-update exclusion in `MenuFunctions.cpp`; the pinned source has
no executable Add SSID route through that mode. The comparison above uses the
reachable menu/CLI workflows, not that dormant label. This workspace supplies
local list authoring and interchange; it is not a claim of complete CLI or
transmission parity.

## Focused validation

The following source-bound host fixtures passed before the complete candidate
matrix. Counts are assertions within each fixture, not hardware tests.

| Check | Assertions | Coverage |
| --- | ---: | --- |
| `tools/check_ssid_list.py` | 15,254 | Atomic model edits, bounds, all removal positions, raw bytes/framing, unique append, revision exhaustion and save acknowledgement |
| `tools/check_ssid_list_session.py` | 868 | Actual session/model, copied previews/saves, allocation failures, exact job ownership, lost status/release, cancellation and dirty revision handling |
| `tools/check_ssid_list_ui.py` | 3,849 | Actual UI/model with counted session boundary, editors, stale display/preview rejection, confirmations, privacy and closing behavior |
| `tools/check_ssid_list_routes.py` | 3,266 | Parent navigation, file-picker lifetime, copied imports, allocation/error handling, retained dual Wi-Fi return and sleep/wake cleanup |
| `tools/check_wifi_interchange_ui.py` | 2,640 | Saved SSID Edit Copy, immutable bytes, failure retention, native return route and unchanged AP JSON controls |
| `tools/check_probe_sniff_ui.py` | 3,279 | Stopped/current-session copy gate, exact binary/empty names, ownership denial, failure retention and successful source cleanup |

Physical acceptance still needs to check the on-device editors, preview and
Back flow, a save/reopen round trip on SD, and cancellation while archive work
is active. Successful host fixtures do not establish touchscreen behavior,
SD timing or device memory headroom.
