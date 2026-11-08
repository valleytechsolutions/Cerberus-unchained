# Wardriving Recent Networks source contract

This guide describes the contributed source. New workflow acceptance on physical hardware remains pending; the installed local Unified checkpoint and the unflashed Unchained adaptation are distinguished in [CONTRIBUTION_REV93.md](CONTRIBUTION_REV93.md).

This document identifies the original separate Wardriving firmware behavior
being ported into the unified firmware. It describes the bounded model and its
integration contract, not an installed checkpoint or physical acceptance.

## Preserved source and reachable behavior

The reference is the separately supplied Cerberus Wardriving firmware. Its original source is not bundled in this contribution; the inspected file identities below preserve the comparison without inventing a public source URL. This is not a newly inferred Marauder mode.

| Source file | Bytes | SHA-256 |
| --- | ---: | --- |
| `s3_main/main/wardrive_state.h` | 1518 | `0a9232c2b08040c307b17c36629d0adad2777eaa6a99732afe7e4fc10303a6ef` |
| `s3_main/main/wardrive_state.c` | 4594 | `ed0c745aa3667bd7037562f7e593093836b5d9647ea558243f016fb1da0ac8f3` |
| `s3_main/main/main.c` | 29741 | `2391ce5d8179acf426658db35f64040aa41d390339aa1ae5589f6e96201b2d34` |
| `s3_main/main/ui.c` | 108178 | `a11b7bd99cc99910e146d648bcdf0603b3734ce356f9dab48b5cad7df02c0508` |

`wardrive_state.h` defines a 24-event recent history and ten visible networks.
`cerberus_wardrive_state_ingest()` updates an identity and pushes a copied event
for both new and repeated observations. `cerberus_wardrive_state_copy_recent()`
walks that history newest first, choosing at most ten distinct
`(record_type, BSSID)` identities. Radio node is metadata rather than part of
that identity. A stream of repeated events can therefore displace other
identities from the 24-event horizon. This is not a persistent ten-device cache.

In `main.c::handle_scan_record()`, ingestion happens before the logger-active,
scan-enabled, GPS-fix, dedupe and minimum-RSSI conditions that guard writing.
The timestamp is obtained from the S3 processing clock. A valid GPS fix is not
required to populate recent history. `ui.c::draw_net_list()` obtains the ten-row
view and provides the source's Nets paging and live/stopped presentation.

## Unified model contract

[`wardrive_recent.h`](../s3_main/main/wardrive_recent.h) and
[`wardrive_recent.c`](../s3_main/main/wardrive_recent.c) retain the 24-event ring
and newest-ten-distinct policy. Each event owns a complete applicable
`cerberus_wardrive_observation_t` value. Wi-Fi uses its BSSID; BLE uses its address
and address type, and Wi-Fi/BLE record kinds remain distinct even for equal
address bytes. A changed C5 slot updates the source shown for the same AP rather
than manufacturing another network identity. Probe requests and observed
station relationships are excluded from this AP/BLE view.

The worker must offer each validated immutable row once, before logging gates,
and retain the returned `(session_id, sequence)` ticket across its processing
steps. Session and sequence IDs are nonzero full-width integers. A repeated or
backward sequence is rejected without modifying the ring. A new session resets
history only when its ID is strictly newer; Stop and viewing do not reset it.
Revision is local to a session, so consumers bind both session and revision.
Sequence, revision and event-count exhaustion rejects further mutation rather
than wrapping. Overwrite and invalid-receipt diagnostic counters saturate.

The stored `receipt_ms` is explicitly **S3 processing time**, not RF time, GPS
time or UTC. Time zero is valid when the caller marks it valid. Missing or
backward clock samples keep the observation but store an unavailable receipt
with zero time, leaving the last valid clock high-water mark unchanged. The
display age helper rejects a backward or unavailable current clock. Embedded
BLE source evidence retains its original independent meanings and times.

New events start `PENDING`. An exact retained ticket may then become `DUPLICATE`,
`APPENDED_UNLOCATED`, `APPENDED_WIGLE`, `ERROR` or `NOT_LOGGED`. Repeating that
same terminal outcome is idempotent; changing it is rejected. Outcomes do not
refresh a receipt, rewrite observation facts or reorder history. Updating an
older event of an identity cannot replace the latest event in the visible view.
An evicted ticket cannot update a different row. **APPENDED means the append API
accepted a row; it does not prove a later flush, sync, close or physical SD
durability.** A partial write error must not trigger a blind append retry.

Only applicable observation fields are copied into zeroed storage; unused
label tails, union arms and padding are not treated as identity or evidence.
Names remain explicit-length bytes, including embedded NUL and control bytes.
Display sanitization belongs in the viewer and must not modify the source.

The owner supplies synchronization. The pure model performs no allocation,
clock read, filesystem access, SDK call or radio operation. Its compact visible
count query uses the same ten-index selection as the row-copy query, avoiding
a ten-row diagnostic-console stack temporary. The service must publish copied
snapshots coherently and retain the model after freeing its active workspace.

## Focused verification and limits

[`check_wardrive_recent.py`](../tools/check_wardrive_recent.py) compiles the real
model and existing Wardriving validator with the host fixture. It checks the
24-event horizon, newest-ten ordering, typed identities, cross-slot AP updates,
canonical copied bytes, original BLE source facts, exact-ticket outcomes,
session reset, stale and repeated tickets, unavailable/backward processing
times, zero time, 64-bit exhaustion and saturating counters. A separate linear
history oracle compares thousands of mixed observation streams and every
supported output-prefix capacity. It also verifies that copied views remain
independent and that queries do not mutate the model.

Service feed, logging-error propagation, retry cursors, Stop/restart ownership,
UI painting and physical behavior require their own integration checks. The
pure model does not establish those outcomes, physical GPS/expansion hardware,
RF coverage, a complete packet stream, global unique-device counts, measured
touch latency, or full Wardriving/Marauder parity. The source's separate
160-identity logging dedupe is already a distinct unified component and is not
replaced by this presentation history.
