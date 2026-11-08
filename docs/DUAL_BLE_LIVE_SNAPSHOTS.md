# Rev98: C5 receive snapshots during a sweep

The rev98 source, all 120 host suites and all five native targets pass on
matching unchanged inputs. The candidate pairs the S3 with C5 protocol
firmware 62. Packaging, installation and subsequent device evidence are tracked
in [the rev98 checkpoint](DUAL_RADIO_REV98.md). Physical latency, transition
behavior and sustained-load acceptance remain separate from build verification.

## Implemented behavior

Ordinary Bluetooth discovery keeps the existing eight-second C5 listening
period and the independent S3 receiver. The C5 copies a bounded snapshot of
its current receive records while listening continues. The S3 transfers that
immutable snapshot into its existing staging bank and publishes only after
all rows and the final snapshot identity validate. New snapshots coalesce
for 500 ms after complete publication; there is no queue of stale copies.

This addresses the previous wait for RF completion before any C5 list could
be published. It does not promise subsecond results. A full 32-row bank needs
352 ten-byte chunks, plus control exchanges; the UI's four-transaction and
eight-ms local-work limits still apply, and one SDK transaction may overrun
that local limit. Actual first-result latency must be measured separately.

The host fixture measures 32 packed 102-byte projections (3,264 bytes) plus
40 bytes of C5 metadata, for 3,304 bytes of frozen snapshot storage. The S3
coordinator store measures 7,512 bytes in the host fixture. It reuses its
published and staging banks and adds one 102-byte wire-row scratch buffer
with bounded control state. No worker or task is added. These are host type
sizes, not measured native heap recovery or a bound on callback lock duration.

## Protocol and provenance

Commands 126–128 implement BEGIN, READ and STATUS. Every request binds the
existing full boot/job identity. BEGIN has a strict per-job request counter:
retrying the same request returns the same frozen bank; old, gapped or exhausted
requests fail. The C5 issues nonzero boot-specific, monotonically increasing
handles without wrapping. Every READ reply echoes the handle, row and chunk;
the final STATUS fence also checks boot identity, count and immutable flags.
New snapshot handlers use a zero-wait mutex attempt and explicit BUSY replies.

Row bytes contain bounded typed address, aggregate peer name/UUID/vendor hints,
latest retained original-ADV bytes, original-ADV RSSI and frozen callback age.
The aggregate hints are not claimed to have come from that one advertisement.
The S3 retains the first BEGIN request's clock anchor across lost-reply retries,
so a later retry cannot make old observations appear newer.

A snapshot taken during RUNNING is an intermediate receive bank, not a
completed sweep. A final completed snapshot is collected before release.
Completed RF sweeps and published snapshots have separate counters. A snapshot
counter includes empty coherent publications; it is not a packet count. A new
explicit public discovery or tracking begin resets its counters while retaining
historical rows. Internal automatic sweep restarts continue the current run's
counters. Stop/Back discard incomplete staging, stop only the bound owner and
keep previously published rows historical. Name sorting, recent filtering and copied tap
identities remain presentation-only.

New S3 firmware negotiates protocol v2 with C5 firmware 62, while retaining
the v1 completed-sweep path for firmware 61. Older S3 firmware may reject the
v2 INFO reply before starting RF; normal deployment uses the matched pair.

## Presentation and diagnostics

The C5 pane distinguishes live updates from the v1 completed-sweep path and
can show `RECEIVING + READING` while RF and transfer overlap. Its first-results
measurement is elapsed local time from the current public discovery begin to
the first new nonempty C5 bank publication, sampled after the successful final
fence exchange. Previously retained rows do not satisfy it. An unavailable,
backward or overflowed first timing remains unavailable; a later publication
cannot replace that first latency with a newer value. This is reader latency,
not touch latency or an RF observation timestamp.

Version 2 of the cached BLE diagnostic snapshot adds `c5_snapshots`,
`c5_first_result_ms`, `c5_has_first_result` and `c5_live_updates`. Sweep and
snapshot counters describe the current public run. Reading diagnostics copies
cached aggregates; it does not query a radio, renew a lease or expose selected
device identities.

## Focused verification

`python tools/check_dual_ble.py` passes with three executables:

- The actual S3 coordinator is exercised against bounded adversarial replies:
  32 rows and 352 READ chunks, four-call/eight-ms scheduling, coalescing,
  lost-BEGIN anchors, BUSY retries, 29 malformed header/chunk/row/fence cases,
  stale preceding-bank handles, negotiated capability consistency, exhausted
  request counters, unknown/overflow timing, Stop throughout transfer, and
  two-sweep/current-run restart counters. Failures retain the old published bank.
- The actual C5 owner service, record types, state and advertisement callback
  are exercised with mocked RF, mutex and clock dependencies. The checks cover
  frozen row coherence, typed identities, scoped ownership, idempotent retries,
  bounded lock attempts and legacy compatibility.
- The integration fixture connects both production ends. Only controller/RF
  completion, mutex, clock, packet parsing and transport failure are mocked.
  It verifies a RUNNING publication during the same eight-second scan, a
  callback mutation after the first transferred chunk, a lost BEGIN reply,
  the final COMPLETE bank before release, Stop and the v1 completed-bank path.
  The v1 test changes the INFO capability/version reply to model firmware 61;
  its accepted v1 session and result/data getters remain the actual C5 code.

The live C5 list and cached diagnostic rendering have their own focused checks
in `tools/check_dual_ble_ui.py` and `tools/check_dual_ble_diagnostics.py`.
An eight-bit SPI sequence can repeat; it is not a unique bank identity. The
full boot/job binding and 64-bit snapshot handle provide the semantic fence
when an old response has an otherwise matching command, row or chunk position.

The implementation is in [dual_ble_protocol.h](../shared/dual_ble_protocol.h),
[the C5 service](../c5_firmware/main/dual_ble_service_impl.h),
[the S3 coordinator](../s3_main/main/dual_ble_scan.c), and
[the C5 pane](../s3_main/main/dual_ble_ui.h). Host checks do not establish RF
coverage, real SPI latency, memory headroom under load or hardware acceptance.

## Remaining acceptance

Measure the matched rev98 package's first nonempty publication and subsequent
updates on the device, including crowded/full banks and repeated Stop/Back
followed by split Wi-Fi scans. Confirm final sweep counts, live UI activity,
both owner releases, memory headroom and callback contention over repeated
sessions. Native memory/stack reports and the full host-suite receipt are
recorded in the linked rev98 checkpoint. The native C5 frozen store is 3,304
bytes; its total static RAM increases by 3,312 bytes including alignment. S3's
native coordinator grows by 176 bytes in PSRAM with no linked internal-section
increase. These figures do not establish physical callback latency or runtime
heap minimum under load.

The existing [BLE parity audit](BLE_RECEIVE_LIST_PARITY.md) and
[rev97 installation record](DUAL_RADIO_REV97.md) retain their historical
evidence and limitations. None of that earlier evidence constitutes physical
acceptance of this new snapshot protocol. Marauder BLE parity remains partial;
family/archive integration, remaining upstream suboptions, S3 native extended
reception and controlled C5 extended/coded reception are still open.

## Related diagnostic clarification

`wifi_dual_rebind_source` records the selected receiver (`0` ordinary scan,
`1` S3 row, `2` C5 row), without publishing an AP identity. It distinguishes
the expected S3-to-C5 fresh reacquisition from the C5 cached path. The fixed
Wi-Fi diagnostic snapshot remains 88 bytes in its focused host check.
