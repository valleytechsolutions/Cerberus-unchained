# GPS qualification and S3 radio recovery

Rev159 addresses two failures that can affect an otherwise completed scan:
non-satellite GPS data qualifying for Wardriving, and a transient S3 Wi-Fi
cleanup error leaving the radio reserved until reset. It includes rev158's
[Channel Activity graph and measured C5 receive windows](CHANNEL_ACTIVITY.md).

## Qualifying a GPS observation

Before this change, an RMC estimated-mode position and GGA quality 6 could
qualify as FIX + UTC. A valid GGA could also restore coordinates using time
from a void RMC at the same epoch. A malformed, checksum-valid RMC status could
leave the previous fix qualified. Reproductions against the preceding parser
confirmed all three cases.

The parser now applies these rules:

- RMC mode A, D, F or R may supply a satellite position. Missing mode remains
  compatible with older NMEA output. Estimated, manual, simulated and no-fix
  modes do not qualify; malformed mode fields invalidate the current fix.
- GGA qualities 1 through 5 may supply a satellite position. Quality 0 and
  estimated/manual/simulated qualities do not qualify.
- RMC and GGA at the same time, received within one second of one another,
  must not disagree about fix validity. This works in either sentence order.
- Invalidating a fix also removes its usable UTC. A later qualifying epoch
  restores position/time admission; stale time does not revive a rejected fix.

The LC29H's trailing RMC navigation-status field is separate from its A/V
position status. Quectel documents that trailing field as unsupported and
always V, so the parser does not reject a valid satellite fix because of it.
The mode and quality definitions follow the
[LC29H GNSS protocol specification](https://www.quectel.com/content/uploads/2022/02/Quectel_LC29H_SeriesLC79HAL_GNSS_Protocol_Specification_V1.4.pdf).

The existing five-second freshness limits, START/wait/STOP controls and
strongest/weakest RSSI policy remain in place. The same parser feeds Start
admission and CSV formatting. This change does not reconfigure the receiver,
add an RF pause or change CSV columns. It does not establish position accuracy
or acceptance by an upload service.

## Releasing the S3 after a Wi-Fi scan

Previously, the worker attempted cleanup once and exited even when an SDK
operation failed. The SCAN owner stayed reserved, but there was no remaining
worker to finish teardown. Bluetooth and subsequent local Wi-Fi scans could
then remain unavailable until reset.

The existing worker now keeps track of unfinished cleanup stages: scan stop,
AP-list release, Wi-Fi stop, event-handler removal and Wi-Fi deinitialization.
It retries the failed stage every 250 ms and does not repeat completed stages.
The owner is released only after every required operation succeeds. The
worker checks that it still owns the radio before touching the SDK.

While recovery is pending, status remains STOPPING with the cleanup error.
Stop records cancellation even if the current SDK error is returned to the
caller. The dual-scan coordinator can wait for local recovery without
immediately canceling C5's independent scan; its overall deadline still
applies. Stop and deadline cleanup continue forwarding cancellation while
the S3 worker finishes teardown.

No additional task or result bank is allocated. A retry keeps the existing
worker and pending scan buffers alive until release. Failed or canceled
replacement scans preserve the previous completed result bank. An ongoing
driver failure remains an error with retained ownership; the firmware does
not force another feature onto a radio whose teardown is unconfirmed.
Synchronous SDK calls can still block within the driver.

## Validation

Focused checks compile the production parser, UART adapter, CSV formatter,
S3 scan worker and dual-scan coordinator against controlled inputs:

- GPS cases cover accepted satellite modes/qualities, both sentence orders,
  estimated/manual/simulated fixes, malformed status, contradictory epochs,
  the LC29H trailing V field and recovery on a later valid epoch. UART checks
  verify Start stays blocked; formatter checks verify rejected rows leave the
  destination buffer unchanged.
- Cleanup faults cover all five SDK stages, repeated failures, Stop during
  recovery, stale job IDs, foreign ownership, the coordinator deadline and
  preserved result banks. After recovery, the tests acquire the BLE owner and
  run another Wi-Fi scan without resetting the owner.
- Coordinator checks retain C5's independent scan during local cleanup and
  verify Stop is forwarded even when local status still reports an error.

All 171 host suites and all five native targets passed on unchanged inputs.
The matched package passed source, SDK/toolchain, image-layout and fingerprint
checks. The linked S3 scan worker's entry frame is 288 bytes within its existing
4 KiB task allocation; the GPS feed entry frame is 64 bytes. These measurements
are individual function frames, not full call-tree or runtime stack bounds.

## Installed checkpoint

Rev159 is installed from source `090993024bf3b439964b39049ae93804fa710411`,
tagged `checkpoint/rev159-gps-radio-recovery`. Package:
`cerberus-unchained-0.1.0-dev.rev159-20261108T065320Z`.
Manifest SHA-256:
`a69cdf325d56518940cb22046052af3e7d4461d543eea6e06f8e32cce4dd36fb`.
C5 capability remains 81 and protocol remains 2.

Fresh S3/C5 recovery backups matched the installed rev158 baseline. All nine
image writes were verified. Both processors' live application descriptors
match the packaged version, SDK, compile time and full ELF fingerprints.

Two idle captures 61.6 seconds apart showed advancing uptimes, mounted SD,
ten S3 tasks and unchanged free memory: 134,244 bytes of S3 internal memory,
94,208 bytes in its largest internal block, and 1,426,140 bytes of PSRAM.
Those values also match the pre-update rev158 capture. The C5 snapshot reported
53,268 free bytes and one boot. Four initial READY timeouts/fallbacks were
present in both captures; mutex, READY-fall, READY-high and driver errors were
zero. The monitored error counters did not increase. The saved home-network,
WiGLE and WDGWars profile readiness flags are intact. No upload was requested.

Physical GNSS acquisition, a recording followed by Stop, and repeated
Wi-Fi/Bluetooth transitions need owner acceptance. The Channel Activity
graph also needs physical 2.4/5 GHz and navigation checks. Injected host faults
demonstrate recovery logic, not the frequency or recoverability of actual SDK
failures in the field. Loaded memory, moving capture and long-session behavior
remain separate checks. Full Marauder option and feature parity remains open.
