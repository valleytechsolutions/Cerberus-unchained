# Recon live inventory and activity

This guide describes the current source, including the rev124 field dashboard
and rev126 priority/navigation changes.
The verified installed pair is rev124/C5 firmware69. Paired boot is confirmed;
the owner reports its dashboard works, but requested clearer Radar navigation
and priorities. Rev126 controls, SD recording and detailed physical acceptance
remain pending.
Validation and installation evidence are recorded in
[the rev124 checkpoint](DUAL_RADIO_REV124.md) and
[rev126 follow-up](DUAL_RADIO_REV126.md).
The earlier contribution's evidence remains in [CONTRIBUTION_REV93.md](CONTRIBUTION_REV93.md).

Recon retains a generic live inventory from the existing C5 Wi-Fi and S3 BLE
scan sessions. The default dashboard brings that inventory onto the main
screen. **Recon → Options → NEARBY INVENTORY** retains the expanded inventory
and radio filters. Ordinary APs and BLE advertisers appear even when they do
not match a classification rule. SD recording is optional and independent of
the view.

## Field dashboard

The dashboard replaces the live mascot. Visible **DASHBOARD** and **RADAR**
tabs select the view directly from the main Recon page. It displays the C5 Wi-Fi and S3 BLE
states separately, SD recording state, recent list changes, and four retained
observations per page. A repeated observation refreshes its typed-address row
instead of appending another dashboard row. Category accents have text labels
and classification confidence; color does not represent danger or authenticated
identity. Classified hints appear before ordinary observations, with higher
confidence first and then the newest first accepted inventory copy in each tier
(not an inferred physical arrival time).
Repeated sweep refreshes do not reorder those identities. New observations,
expiry or changed classification can change the order. Ordinary observations
remain available on later pages and in Nearby Inventory. A row's first line
shows its category/confidence; its context and details preserve source evidence.

The compact activity display covers the latest 32 one-second inventory
buckets, labelled **ALL LIST CHANGES**, **ADDED** and **EXPIRED**. These are retained-list changes,
not packet rates or physical movement. The full Activity page still provides
the existing 64-bucket view. The dashboard refreshes periodically every 500 ms
and on changed data or touch input; the old animated-art loop is not used.

The main **RADAR** tab opens a signal overview; **DASHBOARD** returns to the
priority list. **Options → VIEW** also remains available. The alternate view uses numbered list slots and retained RSSI,
with a dBm scale. It does not show compass bearings, distance, geographic
position or fabricated detections. Its numbered observations open the same
frozen details as the dashboard list. Back returns to the originating view.
Back, Pause/Resume, Log and Options remain available in both views.

Category preferences use **OFF**, **HISTORY ONLY** and **ALERT**. The old
ANIMATED ALERT wording is removed. Current S3 Meta identifier evidence can now
produce a low-confidence **META HINT** row and use these same preferences.
It is not a confirmed glasses/model detection; see
[the exact projection contract](WATCH_SIGNATURES.md#recon-meta-identifier-projection-rev126).

SD recording still starts explicitly in Options and uses the same recorder;
switching visual mode does not create another log or radio session. The
dashboard reports recording/saving/saved/error state, while Options provides
the session folder and complete counters. See [session recording](RECON_SESSION_RECORDING.md).

## Inventory and selection

The inventory holds up to 32 Wi-Fi addresses and 32 BLE typed addresses. Four rows appear per page, ordered by the newest accepted sweep copy. Selecting a row opens a frozen copy of that observation, so a later scan, expiry or capacity replacement cannot silently change the selected target. Back returns to the inventory; the Options and Recon routes remain available. The existing privacy setting masks addresses on these screens.

## Evidence and timing

Each completed scan bank is validated and staged before publication. A failed or invalid batch leaves the previous rows unchanged and adds no addition or expiry events; the time-based history window still advances. Errors remain associated with their radio: a successful BLE sweep cannot hide a Wi-Fi failure. A valid empty bank can still apply expiry for its own radio.

Rows preserve radio, address type, address bytes, received name bytes, generation, available channel and RSSI, source timing and classification. A typed address is an observation key, not proof of a distinct physical device. BLE address rotation may produce multiple keys; passive classifications remain hints. Received names can be inspected in their hexadecimal representation.

The age label states its basis. Genuine S3 source timing is used when available. C5 source age is retained as an upper age bound anchored to the S3 request start, including transport and rounding uncertainty; it is not treated as a precisely translated C5 timestamp. When source evidence is unavailable, a new row explicitly uses sweep-copy time. A later row without source timing cannot refresh a previously retained genuine source age. RSSI remains a driver aggregate or sweep peak and is independent of the age basis.

Rows reach the expiry policy at 120 seconds on their retained age basis, but expiry runs only after a successful completed sweep of that same radio. Paused or failed scans do not manufacture evidence of absence. C5 upper bounds can reach the policy threshold conservatively. Expired means removed by the retention policy, not a confirmed physical departure. No UTC timestamp, GPS position or physical identity is inferred.

## Activity and resource limits

**ACTIVITY** shows 64 one-second buckets of inventory additions and expirations across both radios. These are changes to retained observation keys, not measured arrivals and departures of people or devices. Capacity replacements and invalid input have separate counters. Repeated sweeps refresh existing keys without counting another addition. Long pauses advance the bounded graph without unbounded catch-up work.

The inventory and its selected/displayed row copies use one bounded PSRAM allocation while Recon is open. It adds no task, filesystem operation or radio session. Allocation failure displays an unavailable state while the existing Recon and recording controls remain usable. Leaving Recon frees this allocation; reopening starts a fresh volatile inventory. Screen sleep retains the open page's inventory under the existing pause behavior. Clearing classified hint history does not reset this separate inventory or delete recorded files.

The C5 continues Wi-Fi work and the S3 continues BLE work through the existing independent scheduler. This view consumes their completed results. It does not force both chips onto the same radio operation or change active transmit behavior. GPS and the optional six-C5 expansion board are not required.

## Optional SD summaries

The existing **START SD RECORDING** and **STOP AND SAVE RECORDING** controls remain in Options. Inventory navigation does not start recording. See [Recon SD session recording](RECON_SESSION_RECORDING.md) for session files and storage ownership.

Session summaries and the read-only `recon_record` console now separately report accepted and written Wi-Fi, BLE and iBeacon rows. iBeacon counts use each received row's iBeacon hint flag and are a subset of BLE rows; this does not retain the complete iBeacon payload. These are observation-row counts, including repeats across sweeps, not unique device counts. Accepted means admitted to the bounded queue; written means a complete append succeeded. A failed append or abandoned queued row cannot inflate the written total. The six counters saturate independently and then represent lower bounds; their differences after saturation are not exact pending-row counts. The additive JSON fields carry `row_counter_version: 1`; the CSV format is unchanged.

## Verification and remaining scope

Focused host checks cover atomic publication, typed identity, capacity replacement, source-age bounds, clock rollback and wrap, expiry isolation, radio-specific failures, frozen selection, allocation cleanup, pagination, the production Recon feed and typed recording totals. UI previews cover Options, inventory, details, activity and unavailable states. Native builds and installation evidence are recorded separately in the milestone checkpoint.

Physical acceptance of the new inventory, activity view and typed recording summary remains pending. Earlier owner reports do not validate these new controls. Recording on the real SD card, stop/save and a second session without reset also remain pending from rev90. Host checks do not establish prolonged radio-load performance or peak stack and heap use.

The source comparison uses the reachable [Marauder Recon workflow](https://github.com/justcallmekoko/ESP32Marauder/blob/79cc6bcba1db6679baee1dab2b0c1ad5010e6363/esp32_marauder/ReconMission.cpp). Cerberus consumes completed sweeps; upstream per-callback repeat/update gating remains a separate gap. This is not complete Marauder parity.
