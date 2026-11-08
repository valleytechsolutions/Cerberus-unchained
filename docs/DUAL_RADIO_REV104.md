# Rev104: unified ordinary BLE discovery

Rev104 presents the existing concurrent S3 and C5 observations in one ordinary
ALL-device discovery list. All 122 host suites and five native targets pass
on matching unchanged inputs. The device remains on the installed rev103
checkpoint. This is a presentation
and selection milestone, not full Marauder parity or measured RF acceptance.

## Workflow and evidence

The left summary retains the family selector. In the ordinary ALL family,
the lower-right summary control cycles SOURCE ALL, SOURCE S3 and SOURCE C5.
The upper-right control cycles signal, name and source order. Source order
means retained S3 index order followed by committed C5 index order; it does
not claim a shared first-seen chronology. Four rows appear per page, up to
64 source observations. A typed address observed by both radios remains two
source observations, not a proven physical-device identity or deduplicated
device count. Address types remain visible and masked-address mode also
protects unnamed-row titles.

S3 rows identify their sweep peak RSSI, original advertisement age and
CURRENT/EARLIER inventory meaning (EARLIER is shown explicitly). C5 rows
identify latest original-advertisement RSSI and a conservative age upper
bound. A C5 bound above 30 seconds says RECENCY?; it does not establish that
the actual observation is stale. Unknown ages remain unknown. Comparing
reported RSSI values does not calibrate the receivers or normalize their
different sampling windows.

S3 family selection changes receiver admission. Those family views therefore
remain S3-only and keep their existing evidence and age controls. The ALL-view
source preference survives switching to a family and back. Unclassified C5
rows never acquire S3 family, response or action evidence.

The four displayed rows are copied with full source provenance before drawing.
Only a successful complete paint enables their taps. Replacing either bank
between paint and tap cannot retarget the selection; a failed paint disables
row selection until a successful repaint. S3 details keep the existing release
barrier. C5 selections open the existing RAW ADV / RECEIVE INFO details, with
Back returning to the combined list. C5 Save and passive Fox Hunt reuse the
frozen selected row and existing ownership barriers. SAVE BOTH continues to
save all committed retained observations, independent of the display filter.

## Resource and scheduling scope

The UI adds a bounded 64-entry reference index and four displayed C5 proofs to
the existing optional PSRAM store. It does not allocate a third observation
bank, start another task, change scan parameters, or issue transport/RF work
when sorting, filtering, paging or drawing. Ordering is cached by committed
source revisions and presentation settings. Ages repaint without rebuilding
the ordering; saturated counters bypass the cache. The ordinary 32-row family
cache is invalidated when returning from combined presentation.

C5 protocol firmware remains 64 and the saved-report format remains v4.
Scan-response acquisition, full payload retention, saved-report source
filters and complete upstream option parity are separate unfinished work.

## Validation status

The pure reference-order helper passes 64,896 checks, including an independent
sort oracle, capacity and address-type collisions, absent sources, time bounds
and source immutability. The production ordinary-discovery UI fixture passes
669 checks for filtering, 64-row paging, cache invalidation, failed drawing,
privacy, frozen selection and source-correct C5 Save/Fox/Back. Existing
companion, legacy route and archive fixtures also pass. Source filtering,
sorting and paging make no mocked RF/transport calls; age-only repaints do not
re-sort. Four rendered body previews were inspected for legibility and overlap.
Their owned region is y48..410; they do not establish full-device footer or
touchscreen acceptance.

All 122 registered suites pass with unchanged inputs, using Python 3.14.2 and
Zig 0.14.1. Exact receipt-bound logs are retained under
`artifacts/rev104-host-evidence`. The first full run found an older route
fixture missing the new presentation declarations; its preserved failure is
separate from the final passing run. All five native targets pass on the same
source inputs: S3, onboard C5, bridge, WROOM and XIAO. Their final ELF/map,
SDK configuration, logs and source-bound reports are retained under
`artifacts/rev104-native-evidence`.

The final S3 ELF comparison with rev103 measures the optional discovery store
at 20,560 bytes, up from 19,792: **768 additional PSRAM bytes**, before allocator
overhead. This includes the 128-byte reference index, 480 bytes of displayed
C5 rows, 96 bytes of C5 provenance, cache/control fields and padding. Existing
full source-bank types are unchanged. Fixed internal `.dram0.data` and
`.dram0.bss` remain 26,412 and 146,160 bytes. The selected `live_make_view`
frame grows from 256 to 304 bytes; `cerberus_ui_run` stays 1,136 and the
companion draw frame stays 304. The main task remains 8,192 bytes. These are
native type/section and selected-frame measurements, not free-heap readings,
a complete stack bound, scan-latency benchmarks or sustained-load acceptance.
The comparison and raw offline debugger evidence are under
`artifacts/device-rev104/native-ui-comparison.json`.

No rev104 installation, touchscreen acceptance, actual RF result or SD
save/reopen acceptance is claimed. The retained rev103 installation and its
measured console-stack improvement remain the last installed evidence.

Physical acceptance still needs live source filtering and selected-row checks,
Back/Stop/restart, a C5-selected passive Fox handoff, save/reopen, and the next
split Wi-Fi scan. No GPS or six-radio expansion acceptance is implied.

## Local matched package

Implementation commit `e7c5775b9d6a5a8e5726e79fe3a036d1203859d1` is retained
locally. Package
`dist/cerberus-unchained-0.1.0-dev.rev104-20261101T091414Z` passes hash, chip
header, partition and image-bound verification. All three application hashes
match the retained five-target matrix. The firmware source SHA-256 is
`9d39d23c08f419b8a5a9f2f270619fb9163108f12dfab8fd092f1069a6398631`.

`artifacts/device-rev104/local-milestone.json` binds that package to the host,
native and memory evidence. The manifest, matched receipt and package log are
also retained with native evidence. No image was flashed, no device command
was sent, and no PR, push or public release was created for this milestone.
Rev103 remains the last installed checkpoint.
