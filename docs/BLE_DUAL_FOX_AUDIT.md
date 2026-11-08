# Dual-receiver BLE Fox Hunt: next receive-side milestone

This is the source audit that defined the follow-up to
[rev111](DUAL_RADIO_REV111.md). [Rev112](DUAL_RADIO_REV112.md) records the
implementation and validation status separately. The gaps and source line
references below describe rev111; this audit is not physical acceptance.
Rev111 changes C5 callback dispatch only and does not change Fox Hunt. The
wider port remains partial.

The useful next scope is to make the existing two-receiver Fox Hunt behave as
two independent, clearly labelled observers through start, retry and cleanup.
It needs no new RF mode, advertising, connection, GATT operation or attack work.
Two nearby receivers do not establish bearing, distance or triangulation.

## Existing behavior and upstream basis

The pinned Marauder develop source is
`79cc6bcba1db6679baee1dab2b0c1ad5010e6363`, available in the local reference
`../integration/marauder-develop-2026-10-29`. In its `esp32_marauder/WiFiScan.cpp`:

- `RunBluetoothScan()` configures the BLE receiver and starts scanning without
  a fixed completion duration (around lines7356–7369).
- `updateBluetoothFoxHuntRssi()` updates a selected target from matching callback
  address information (line12466).
- `runFoxHunt()` presents the selected RSSI and one meter, with a 100ms UI update
  gate (line12538). It is a single-receiver reference, not a dual-chip design.

Cerberus already follows one immutable **address plus address type** on both
receivers. It deliberately does not follow rotating addresses by name or infer
that observations belong to one physical device. Source ages and RSSI remain
separate. `BT_SCAN_FOX_HUNT` therefore remains `partial` in
[the coverage status](MARAUDER_COVERAGE_STATUS.json); earlier single-S3 owner
acceptance is not evidence of dual-receiver physical acceptance.

## Concrete gaps in the current call paths

Line references below describe the rev111 source checkpoint; function names
identify the owning routes if later edits move lines.

1. **The page-wide state and only meter describe S3.**
   `draw_ble_fox_hunt()` in `s3_main/main/ui.c:4774` chooses LIVE, SEARCHING or
   STALE from the S3 tracker and draws its meter around lines4795–4829.
   `dual_ble_ui_draw_fox()` in `s3_main/main/dual_ble_ui.h:420` adds one C5 text
   line. A target heard only by C5 can consequently appear under a page-wide
   SEARCHING or STALE label while C5 reports a usable observation.

2. **A C5-only error has no in-place retry action.**
   The Fox branch of `handle_ble_tap()` in `ui.c:9301` offers retry only when
   `ble_fox_hunt_error` is nonzero. A C5 TRACK ERROR with a healthy S3 leaves
   STOP TRACKING as the action; pressing it exits. Analyzer already has a
   source-aware retry predicate in `ble_analyzer_dual_ui.h` and its own route
   immediately before the Fox handler. That is a lifecycle reference, not
   permission to stop or restart an unrelated scanner.

3. **S3 tracker attachment gates C5 startup.**
   `ble_fox_hunt_open_peer()` and `dual_ble_live_prepare_fox()` in
   `s3_main/main/dual_ble_live_ui.h:16` and `:35` correctly wait for the old S3
   discovery and C5 owner to drain. However, `ble_fox_hunt_start_pending` clears
   only after `cerberus_s3_ble_scanner_track_begin()` succeeds. The C5 TRACK
   request in `dual_ble_live_tick()` at line107 also requires that flag clear.
   A failure to attach the S3 tracker therefore prevents an otherwise available
   C5 from starting. Distinguish this from a later S3 RF start error, which is a
   different branch after tracker attachment.

4. **The Fox cleanup deadline ends retry ownership.**
   `ble_fox_hunt_tick()` in `ui.c:9760` calls `ble_fox_hunt_finish_exit()` after
   12seconds even if a radio remains held (around lines9802–9810). The finish
   function ends the local tracker and leaves the Fox page with
   `RADIO CLEANUP HELD / RESET REQUIRED`. Further Fox S3 Stop retries cease;
   the C5 global coordinator can still finish its own cleanup. This is an
   explicit existing failure state, not a claim of use-after-free. The nearby
   Analyzer controller instead retains its pending exit and retries cleanup
   after reporting its deadline. That behavior is a suitable bounded model.

## Bounded implementation plan

- Keep the shared discovery-release barrier and immutable typed target. After
  that barrier, track S3 attachment/start state and C5 attachment/start state
  independently. One source's attach failure must not silently substitute its
  evidence or prevent the other available source from operating. Preserve
  ownership checks before every retry and stop.
- Give S3 and C5 their own state, RSSI meter, age and report label. An optional
  page summary may say which sources are listening; it must not promote a
  retained sample to current reception. Keep pending-target suppression on
  both sources. C5 `age_upper_ms` is a conservative bound: exceeding the recent
  threshold does not prove an exact stale age. Unknown/overflowed age remains
  explicit. No combined RSSI, rate, device count, distance or directional cue.
- Preserve report-count semantics. S3's tracker currently observes matching
  callbacks including scan responses (`s3_ble_scanner.c:188` and `:225`). C5
  TRACK observes admitted original callbacks in
  `c5_firmware/main/ble_scanner.c:280`; its count resets when a new C5 job starts
  (`dual_ble_service_impl.h:92`). Do not compare these as equivalent packet
  totals or fabricate a cross-job cumulative count from a decreasing counter.
- Expose retry when either source has a retryable error. Retry only the failed
  source where its owner state permits; preserve the healthy source and target.
  A separate explicit Stop/Back still wins over any pending retry.
- On Stop, Back or sleep, retain the appropriate cleanup state until both
  owned sources release. A deadline reports a timeout without clearing the
  ownership barrier, rearming, or stopping a foreign owner. Do not claim
  cleanup will succeed when cancellation continues to fail.
- Reuse the existing cached C5 TRACK reply and local S3 tracker getter. This
  first milestone needs no protocol change, extra history bank, task or
  callback allocation. Rendering remains free of SPI. Any new status fields
  should be small controller state, with native sizes and frames measured.

## Regression and acceptance plan

Extend the production extraction tests rather than create a substitute Fox
controller:

- `tools/check_dual_ble_ui.py` extracts the real `draw_ble_fox_hunt()`.
  `tools/host_checks/dual_ble_ui_check.c` already checks exact-target C5
  binding and suppression of the previous S3 target. Add both-heard, S3-only,
  C5-only, neither-heard, source error, unknown age and source-specific count
  screens. Verify layouts with rendered previews.
- `tools/check_s3_ble_discovery_ui.py` extracts the actual Fox finish/tick,
  pending-start helpers and `dual_ble_live_tick()`.
  `tools/host_checks/s3_ble_discovery_live_check.c` contains
  `companion_pane_and_fox_barrier()` and
  `unified_frozen_c5_selection_save_fox()`. Extend these for independent attach
  errors, C5-only retry, no restart of the healthy source, target changes,
  Stop/Back before start, sleep, cancellation held past 12seconds followed by
  release, and permanent failure that retains the ownership barrier. Assert
  no foreign scanner is stopped and no delayed completion rearms tracking.
- Reuse `tools/check_dual_ble.py` for C5 boot/job/epoch fences, retained samples,
  age overflow/backward-clock handling and per-job counter reset. Add only
  regressions required by changed coordinator behavior.

Host results would establish control and rendering behavior, not RF quality.
Device acceptance still requires a controlled broadcaster heard by both
receivers and cases heard by only one, movement with independently changing
RSSI, repeated Fox/Stop/Back/normal-scan transitions, recovery after a radio
error, and measured heap/stack under load. Address rotation must end the exact
track rather than silently select another advertiser.

## Later acquisition responsiveness work

The next separate candidate is response-snapshot transfer cost, not a shorter
RF sweep. A full response row is310bytes in31 ten-byte chunks, so32rows require
992 reads before the final bank fence (`dual_ble_scan.c:snapshot_step()`).
The controller caps work at four exchanges/eight milliseconds per UI tick,
and `ui_task` retains its20ms delay. These are source arithmetic and scheduling
constraints, not measured wall-clock latency. Marauder's local callback list
updates do not incur this inter-chip full-bank transfer.

If physical dense-result latency warrants it, consider a separately versioned
compact projection for canonical UNKNOWN/NONE response suffixes, while keeping
the existing full310-byte path compatible. Explicitly encoded omission must
reconstruct only proven canonical fields; never infer absent bytes from a
truncated transfer. Preserve the frozen handle, full source binding, first-BEGIN
age anchor, per-chunk echo, final fence, immutable selection and Stop priority.
Do not publish staging rows as completed evidence just to make results appear
sooner. Measure transfer counts and end-to-end publication latency before
claiming improvement. This optimization is not part of the proposed Fox work.
