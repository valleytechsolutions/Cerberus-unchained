# Rev139: integrated saved-log management and uploads

This milestone adds two connected capabilities to the main firmware: a paged
Wardriving log manager and saved home-Wi-Fi/three-service uploads. Rev139 is
installed with verified paired boot, SD mount and sampled idle stability.
The measured capture baseline is rev138; physical upload acceptance is separate.

## Saved captures

One **Scan → Wardriving → LOGS / UPLOAD** branch lists finalized native files,
validates complete contents, displays counts/CSV UTC, and provides confirmed
Delete/Delete All. Its background worker holds an exact SD lease, excluding
capture, USB handoff and File Tools mutations while the reviewed catalog is
active. Stale confirmations cannot affect a newer catalog. Interrupted captures
and unrelated SD resources are excluded.

## Home network and transfers

The same branch contains masked AP/API setup, destination toggles, Upload/Upload
All, progress, Cancel and separate results. S3 performs station/TLS/SD work after
capture stops; C5 receives no redundant network work. Existing receive allocation
is unchanged. A dedicated NVS blob stores configuration; temporary application
buffers are scrubbed. No encryption or eFuse provisioning is implied.

Transfers use bounded buffers, retain every observation and split large files
at complete rows. Durable per-part receipts bind the complete source hash and
exact credentials. WiGLE acknowledgement remains QUEUED; ambiguous delivery
cannot become success or be blindly retried. The [setup guide](WARDRIVE_UPLOADS.md)
describes actual navigation, credential handling, contracts and limitations.

## Capture baseline in this contribution

Rev137 introduced continuous callback queues, three capture modes, S3 2.4 GHz
plus C5 5 GHz allocation, dual-receiver BLE-only mode, and GPS AUTO, LC29H-AA,
ATGM336H/manual settings. Rev138 removed unused duplicate-history work and fixed
the native WiGLE pre-header. Its 31.415-second run produced 1,438 observations:
263 S3 Wi-Fi, 121 C5 Wi-Fi and 1,054 S3 BLE. Physical SD readback accounts for
all 1,436 located and two unlocated rows, with zero reported queue/radio/SD
errors. Missing accuracy and two missing altitudes remain warnings. See
[rev137](DUAL_RADIO_REV137.md) and [rev138](DUAL_RADIO_REV138.md).

## Validation scope

Six new registered suites cover the streaming log parser, config/NVS adapter,
provider replies/lossless part plans/receipt corruption, real manager/leased
storage, real HTTP adapter with controlled SDK failures, and actual touch
routes/rendering. The existing Wardriving UI suite checks the three-button
Start/Recent Nets/Logs row. The unlocated detail view explicitly marks radio
subtotals unavailable instead of displaying unmeasured zeroes.

All **166 registered host suites** and **five native targets** pass on the same
1,294 unchanged inputs at firmware source commit
`19e8ec8986a2cebfc311f0715dbbae6e57e074f0`. The 21 CI shard/aggregation checks,
20 build/package failure-handling checks and SD-resource checks also pass.
Native READY, console-stack and C5 Locate/parser placement gates pass. The new
actual C parser also validates the private physical rev138 files: 1,436 located
rows with the expected radio counts and two structurally valid unlocated rows.
This does not turn local format checks into service acceptance.

The final matched development package is
`dist/cerberus-unchained-0.1.0-dev.rev139-20261105T025615Z`; all 22 listed package
files pass checksum verification. Manifest SHA-256:
`b13f5de72685c6569ee613f6af0d9b6c254cc7210b133af136b34dbbbcdf65e2`.
S3 application size is 2,320,336 bytes, C5 is 1,728,736 bytes and the temporary
bridge is 255,296 bytes. Immutable pre-install evidence is retained locally in
`artifacts/rev139-validation-final/local-milestone.json`.

Fresh rev138 recovery backups cover 2,387,968 S3 bytes and 2,097,152 C5 bytes,
including every erase-rounded update write. Six exact-loader identity tests
pass. All nine bridge/C5/S3 image writes verify their programmed hashes. The
bridge written before the final UI correction is byte-identical to the final
package, and its original write proof remains bound to that exact image set.
No blanket flash erase or installer NVS rewrite was performed.

Two post-install diagnostic captures 44.61 seconds apart confirm rev139, C5
capability 75 online, mounted SD, released owners and no previous-reset fault.
S3 internal free memory stays at 135,344 bytes, largest internal block at 94,208
and PSRAM free at 1,426,140; the C5 snapshot reports 53,460 free bytes. Compared
with the sampled rev138 baseline, idle S3 internal free memory decreases by 620
bytes and free PSRAM by 1,412 bytes. These are idle observations, not loaded
upload or worst-case memory bounds.

SPI exchanges advance from 82 to 288 with unchanged C5 boot count and no
additional listed transport error. Four initial READY timeouts and four legacy
fallbacks are present at both samples; they are not represented as zero lifetime
errors. Private installation proof is
`artifacts/device-rev139/installation-receipt.json`, SHA-256
`16321a0fcd45bd06616d3e48d7967d03e9bc63216a3d7b889d11463cdf8685eb`.

The owner then checked the installed **LOGS / UPLOAD** page against the saved
capture: 1,436 rows, 263 on 2.4 GHz, 121 on 5 GHz and 1,052 BLE. They confirmed
the counts, Back navigation and opening/leaving **HOME WI-FI / API SETUP** work.
This adds physical SD-read/parser/navigation acceptance. It does not validate
credential entry/persistence, destructive actions, home association or uploads.

This does not finish Marauder parity. Live service acceptance, WiGLE processing
reconciliation, physical upload/Cancel transitions, long-drive RF completeness,
sustained GPS lock, dense BLE performance and six-C5 hardware remain separate
tasks. Wardrift follows a public proposal rather than a tested live contract.
The source audit retains develop `79cc6bcba1db6679baee1dab2b0c1ad5010e6363`,
rechecked through the read-only GitHub API on 2026-11-05 UTC. This milestone
changes no attack behavior. Private captures, coordinates and credentials are
excluded from the contribution.
