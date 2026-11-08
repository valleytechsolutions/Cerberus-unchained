# Continuous AP/client survey

Rev170 adds **Scan → AP/CLIENT SURVEY**. The workspace opens paused; START
assigns passive 2.4 GHz reception to the S3 and passive 5 GHz reception to the
C5. Both receivers keep collecting while the S3 copies and displays results.
The ordinary finite Wi-Fi scan remains available. The expansion-board survey
remains under the Wi-Fi tools footer, labeled **R1–R6**.

## Operation and evidence

APS and CLIENTS share one workspace, with four stable rows per page. Selecting
an AP opens its details and an observed-client filter. Client details separate
STA → AP from AP → STA counts, latest signal and observation age. A selection
retains the exact source, run, session, row incarnation and key even as live
results change. Refreshing one receiver does not reorder the other receiver's
retained rows. Replaced rows cannot silently become a different selection.

These are received management frames and clear data-header relationships, not
proof that a station is currently connected, authenticated or physically unique.
Counts include repeated callbacks. Protected payloads are not decoded. The AP
key is BSSID plus actual receive channel; a relationship also includes the
station address. Direction-specific signal belongs to that direction's
transmitter. Invalid latest signal remains unavailable rather than borrowing an
older measurement. SSIDs retain their original bytes, including hidden names.

Each receiver retains at most 32 APs and 32 relationships. Full tables replace
the least recently observed row, increment explicit replacement counts and
assign a new incarnation. Counters saturate rather than wrap. Session,
projection, incarnation and revision exhaustion close admission instead of
reusing an identity. A client observation can precede its AP's beacon.

STOP closes reception, obtains the final available snapshots and releases both
backends. The stopped results remain visible. BACK TO SCAN and sleep cancel
partial transfers and drain only this workspace's exact owned sessions.
Back then frees the workspace; sleep retains its results. Neither wake nor
opening the page starts reception. SAVE REPORT is
available after cleanup and writes a bounded local evidence report through the
existing Wi-Fi report writer. Binary SSIDs are hex encoded. This report uses
receiver uptime and transfer anchors; it is not a GPS/WiGLE recording or PCAP.

## Receiver scheduling and memory

The default split gives each receiver a separate band. The S3 intersects its
2.4 GHz plan with the configured country. The C5 reports requested, attempted
and successfully applied channels; SDK-rejected channels remain visible as
coverage gaps. Successful arms receive for 250 ms before hopping. A
single-channel plan stays armed. A completed pass does not imply that every
requested channel was accepted or every nearby network was observed.

The C5 uses its existing scanner worker, with a 7,792-byte host-ABI service
context including two 3,816-byte banks. The S3 native receiver allocates its
large banks in PSRAM and starts an optional 4 KiB worker; its control context is
88 host-ABI bytes. The UI workspace, including its model and coordinator,
allocates 21,648 host-ABI bytes in PSRAM on entry. The stopped report uses a
temporary 64 KiB PSRAM buffer. These sizes exclude SDK driver allocations and
do not measure loaded device heap or stack headroom. The fixed-size snapshot
copy's actual receive-lock duration also needs hardware measurement.

Capability 90 adds exact 64-bit session control and immutable projections.
Snapshot copying does not pause reception, change channel or renew a lease.
The C5 transfer takes bounded steps, with a fresh peer proof before and after
the image. The coordinator runs at most one backend operation per tick, gives
cleanup and explicit renewal priority, and allows either receiver to progress
when its companion fails. A malformed or incomplete image preserves the last
complete bank. A failed cleanup retains ownership and retries; it cannot make
the radio available prematurely. The S3 BLE admission check excludes an active
native Wi-Fi survey.

## Upstream scope and validation

The reachable reference is Marauder develop
[`79cc6bc`](https://github.com/justcallmekoko/ESP32Marauder/commit/79cc6bcba1db6679baee1dab2b0c1ad5010e6363),
rechecked against `develop` on 2026-11-08. `WIFI_SCAN_AP_STA` is reached through
Scan AP/STA and `scanall`; its passive callbacks and continuous channel hopping
are the relevant behavior. Legacy mode constants without a reachable start
route do not establish extra implemented workflows.

The rev170 source implements the continuous AP/client inventory and direct
navigation. Full upstream option parity remains open, including its additional
security/vendor presentation, probe/disconnect feed and AP/STA PCAP behavior.
This work does not change active transmission features or claim their repair.

Focused host fixtures cover the actual collector, native receiver, C5 service
and wire reader, stable model, renderer and coordinator. They include late
callbacks, channel failure, table replacement, counter exhaustion, country
intersection, lost control replies, foreign owners, malformed images, lease
expiry, clock rollback, partial-copy cancellation and repeated runs. Physical
dual-band reception, touchscreen navigation,
stopped report readback, sleep/wake, the next ordinary Wi-Fi/BLE scan and loaded
performance remain pending.

## Checkpoint

Rev170 source `5f49203` passed all 207 host suites and five native targets
(S3, onboard C5, USB bridge, expansion WROOM and expansion XIAO). The host and
native receipts cover the same 1,506 unchanged input fingerprints. The source
checkpoint is tagged `checkpoint/rev170-host-validated`; later contribution
documentation changes do not alter those firmware inputs.

Rev170 has not been packaged or installed. Rev169 remains the verified
installed baseline. Matched packaging, fresh recovery backups, verified writes
and physical reception/navigation are still required for rev170 acceptance.
No live survey or active RF test has been started for this change.
