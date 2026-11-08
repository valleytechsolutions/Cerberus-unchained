# Trusted Wi-Fi Baseline

This guide describes the contributed source. New workflow acceptance on physical hardware remains pending; the installed local Unified checkpoint and the unflashed Unchained adaptation are distinguished in [CONTRIBUTION_REV93.md](CONTRIBUTION_REV93.md).

## Completed snapshot and ownership

The Baseline page compares the ordinary Wi-Fi AP snapshot already committed on
S3 against the owner's saved trust entries. It does not reread AP results from
C5. The comparison is bound to the nonzero local publication token, the saved
eight-bit generation and the full-width S3 publication timestamp. The saved
generation must also match the relation publication's generation. A matching
generation byte alone is insufficient.

Entering Baseline or pressing COMPARE does not start a radio operation. The
page has NETWORKS, COMPARE, SAVE and LEARN controls, plus a WIFI MENU footer;
it has no scan action in that footer. NETWORKS returns to the ordinary scan
page, where scan controls belong. An already owned Wi-Fi sweep may drain and
publish its result while Baseline is open. The Baseline scheduler does not
rearm another sweep after completion, failure, Stop, or sleep/wake.

A failed or cancelled later transfer leaves the preceding committed AP rows
and their publication time intact. Those held rows remain usable; staged rows
and the newer remote scan state do not become comparison evidence. A reset
that invalidates the ordinary snapshot also invalidates the comparison.

The bounded comparison uses existing S3 snapshot storage and in-memory trust
settings. It adds no worker, allocation, RF operation, or settings write. Trust
mutation and comparison run on the UI task; initialization precedes that task.
Existing scheduler/status telemetry is separate from the comparison. Explicit
SAVE uses the existing SD report writer.

## What the counts mean

The maximum compared bank is 32 retained rows. Counts describe rows, including
duplicates; they are not a unique-AP count or a complete inventory of the
environment. SAVED BY YOU counts saved trust entries, whether or not their
networks appear in the snapshot.

The existing settings decision supplies each row's classification:

| Display | Meaning |
| --- | --- |
| EXACT MATCHES | A saved entry has the same received SSID string, BSSID and security value. An exact match takes precedence over another related entry. |
| REVIEW CLUES | No exact entry matches, but at least one saved entry shares the received SSID or BSSID. This includes a renamed SSID on a saved BSSID and a security change. |
| OTHER NETWORKS | No saved name or BSSID matches. A hidden SSID is classified here under the existing trust rules. |

Names retain their unsanitized received C-string identity during comparison.
Case, control bytes and high-bit bytes are not replaced with display glyphs
before matching. The existing SDK/C5 representation cannot preserve an SSID
tail beyond an embedded NUL, so this does not establish full binary/on-air SSID
identity.

Settings unavailable is different from an available, empty trust store. The
former displays TRUST STORAGE UNAVAILABLE, clears comparison counters and
disables SAVE. An available store with zero saved entries permits a comparison;
its retained rows are Other. A review clue does not prove an attack. Mesh nodes
and router changes can explain differences; an unseen saved network does not
prove loss or absence.

## Validation, empty banks and time

The core validates the entire bank before reading trust entries or publishing
counts: at most 32 rows, a NUL terminator in each received SSID buffer, nonzero
channel, and a security value no greater than OTHER. Any invalid row rejects
the whole comparison. Failures publish only checked/error state, with counters
and generation cleared. The adapter also clears its publication/time binding.

A genuinely completed empty bank is valid and displays zero compared rows.
It can have no row allocation. This differs from never having captured a bank,
an invalidated bank, or a malformed bank. Missing snapshots show START WITH A
SCAN; malformed input also exposes its error and cannot be saved.

AGE AT COMPARE is the elapsed S3 uptime between the snapshot's local commit and
the current comparison draw, rounded down to seconds. It is not a live clock,
RF receive age, measurement timestamp, or UTC. Uptime zero is valid. If the
current clock is earlier than the committed timestamp, the page displays
AGE AT COMPARE UNKNOWN instead of underflowing. A negative publication time
is invalid. Reopening or redrawing the page recomputes the local comparison
and its age, including any saved trust edits.

## SD report

SAVE validates the displayed comparison's publication token, saved generation,
row count and full-width publication timestamp against the currently committed
bank, and requires settings to remain available. If another bank was committed
before the page redrew, SAVE rejects the old comparison with COMPARISON CHANGED
/ TAP COMPARE. It does not silently save the newer unseen bank. Reuse of the
same generation byte does not bypass this guard.

The CSV includes report/firmware identification, scope explanations and these
numeric fields:

| Field | Meaning |
| --- | --- |
| `snapshot_publication` | Nonreused local publication token for the compared bank. |
| `snapshot_uptime_us` | S3 snapshot commit uptime in microseconds, not RF time or UTC. |
| `saved_baselines` | Saved trust-entry count. |
| `compared_stored_records` | Number of retained rows compared. |
| `exact_matches` | Exact name/BSSID/security matches. |
| `related_trust_differences` | Related name or BSSID matches without an exact match. |
| `other_records` | Remaining rows under the existing trust rules. |

Network names and device identifiers are not exported. The CSV states its
retained-snapshot, time and interpretation limits. SD errors are reported as
save failures; a successful host writer fixture is not physical SD acceptance.

## Source validation and limits

- `tools/check_sentinel_baseline.py`: 233,177 focused checks passed against the
  actual core and exact production trust/count decisions. Coverage includes
  all generation bytes, 0–32 rows, raw received names, classification precedence,
  unavailable settings, input immutability and whole-bank failure handling.
- `tools/check_wifi_baseline_ui.py`: 1,248 focused checks passed against the
  actual adapter, reset handler, body/footer rendering, report and applicable
  controls. SPI APIs are not linked. Coverage includes publication replacement
  before SAVE, held snapshots, empty/unavailable states, export privacy, zero
  and large uptime values, and clock rollback. Six generated previews passed
  pixel bounds and visual review. Unrelated page dispatch is outside this
  focused fixture.
- `tools/check_scan_display_store.py`: the actual page-tool selector, scheduler,
  transfer and diagnostics passed Baseline entry with and without a snapshot,
  an already owned running sweep, failure, Stop, sleep/wake, and preservation of
  committed rows/time after a later failed or cancelled readout.

Implementation is in `sentinel_baseline.c/.h`, the Baseline adapters in
`sentinel_live_ui.h`, the commit timestamp in `wifi_scan_capture_ui.h`, and the
Baseline rendering/report/controls in `ui.c`, under `s3_main/main`.

These source checks do not establish physical touch response, RF reception,
saved-file accuracy on the device, prolonged operation, or rev84 owner
acceptance. Hardware/build/install evidence must be recorded separately.
