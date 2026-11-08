# Rev114: shared dual-receiver Find My monitoring

Rev114 is a local candidate for the dedicated Find My Monitor. Rev113/C5
firmware 68 remains the installed baseline. This change implements the
receive-monitoring work described in [the source audit](BLE_FINDMY_MONITOR_AUDIT.md);
the complete Marauder port and physical acceptance remain unfinished.
The coverage ledger remains the explicitly labelled `rev113-source` snapshot;
this candidate's additions are recorded here without promoting any parity status.

## Behavior

The dedicated monitor uses the same S3 discovery session and C5 companion as
the ordinary Bluetooth list. S3 supplies incremental active-scan observations;
C5 supplies focused Find My observations, passive by default and respecting the
existing explicit response-request preference. The two receivers retain their
own state, failures and evidence. A failure on one receiver does not clear the
monitor's overall run request, and each receiver has its own Retry control.

The monitor locks the Find My family and offers ALL, S3 and C5 display filters.
Rows identify their receiver and typed address. S3 peak RSSI and C5 latest
advertisement RSSI remain distinct. Original advertisement and matched-response
hints are shown separately. S3 ages use the frozen source clock; C5 ages are
upper bounds or unknown. A large C5 upper bound means recency is unconfirmed.
These rows are observations, not a count or identification of physical trackers.

S3 retains its existing bounded history, with a 120-second monitor display
horizon based on hint observation time. C5 exposes its latest committed bank.
The monitor does not add a C5 history bank. Source validation is cached by
publication; age-only repaints do not repeat packet parsing.

Four copied rows become selectable only after their complete monitor paint
succeeds. Selection retains that exact proof while both owners release, even
if a later scan replaces the live bank. S3 detail keeps its full scan epoch and
source time. C5 detail keeps its own raw bytes, receive metadata, hints and
boot/epoch proof. Fox Hunt returns through the original detail and then back to
the stopped monitor. No GATT or sound action is added by this change.

Stop, Back, selection, saving and sleep clear acquisition intent, including a
C5 request waiting for INFO. The existing global owner ticks perform cleanup;
the monitor does not introduce another radio poll loop. A 12-second cleanup
warning does not abandon an owned session or allow an automatic restart.
Returning to retained results leaves scanning stopped.

SAVE BOTH captures the retained S3 and C5 banks at one save clock before
waiting for radio release. It uses the existing asynchronous worker and v6
archive format. The saved scope includes retained observations and can differ
from the current source filter or display horizon. Selected SAVE SD preserves
the original receiver's copied evidence. The archive's Back destination is
preserved, including after a failed capture.

## Validation and resource evidence

All 124 host suites pass on unchanged inputs. The retained receipt binds every
suite log by hash. Focused regressions exercise the production discovery owner,
monitor presentation, companion controller and archive capture paths:

- Frozen S3 and C5 selection survives publication replacement between paint
  and tap. A failed paint leaves its rows unselectable. Typed-address and
  cross-receiver collisions remain separate observations.
- Find My acquisition remains locked even if the ordinary-list filter differs.
  Source validation and hint-time expiry reject stale aggregate categories,
  mismatched source evidence and apparent freshness after a 32-bit clock wrap.
- An S3 failure preserves C5 intent. Per-source retry, pending INFO cancellation,
  held cleanup beyond 12 seconds, sleep, hidden navigation and stopped return
  preserve owner boundaries and do not automatically restart discovery.
- Save freezes 64 retained rows before release. Later mutations of both live
  banks cannot alter the job, its v6 encoded/reopened data or its monitor return
  destination. Failed capture also leaves acquisition stopped.

The full production discovery fixture passes 1,320 assertions. The archive
capture/controller fixture passes 3,718 checks. Screen previews were reviewed
for mixed receivers, four rows, typed collisions, empty and unavailable-source
states, masked identifiers and the maximum conservative C5 age.

All five native targets pass: S3 application, onboard C5, temporary S3/C5 USB
bridge, expansion WROOM and expansion XIAO. Their unchanged input map exactly
matches the retained host receipt. The native audit compares the final images
with the verified installed rev113 baseline:

| Native measurement | Rev113 | Rev114 |
| --- | ---: | ---: |
| S3 fixed data and BSS | 173,612 bytes | 173,564 bytes |
| C5 fixed data and BSS | 83,160 bytes | 83,160 bytes |
| Shared discovery store | 21,440 bytes | 21,448 bytes |
| Dual-BLE coordinator | 21,496 bytes | 21,496 bytes |
| Optional C5 response context | 11,736 bytes | 11,736 bytes |
| Archive viewer plus worker type demand | 118,760 bytes | 118,760 bytes |

The old dedicated monitor cache type was 8,584 bytes. The new monitor has no
private cache pointer or allocation; it uses the existing shared stores, whose
discovery state grows by eight bytes. These are native type/allocation-demand
figures, not measured runtime heap savings. The ordinary and dedicated views
share the four copied proof slots. No task, radio mode, C5 wire command or
archive format is added. C5 firmware capability revision remains 68.

The selected S3 UI entry frame decreases from 1,136 to 1,072 bytes;
`handle_ble_tap` decreases from 208 to 192 bytes, and `handle_tap` remains
544 bytes. The monitor selection helper has a 304-byte entry frame. Inlined or
absent helpers are not treated as zero-cost, and these selected entries do not
bound complete call chains or establish runtime stack headroom.

Final S3/C5/bridge application sizes are 1,937,808 / 1,678,960 / 255,296 bytes.
Retained evidence is under `artifacts/rev114-host-evidence`,
`artifacts/rev114-native-evidence` and `artifacts/device-rev114`.
The native audit SHA-256 is
`9eb4311df29e7a35142bfeb59ac229d4ba91c2b93c843fee71e1e06fdba4876f`.
The matched package is
`dist/cerberus-unchained-0.1.0-dev.rev114-20261101T151444Z`.
Its image hashes, chip headers, partition tables and image bounds pass the
read-only package verifier. All three application hashes match the retained
five-target matrix. The local milestone receipt binds the package, source
commit, host/native receipts and native audit. No image was flashed and no
release or pull request was published. A successful compile or synthetic
fixture does not establish RF reception, touchscreen behavior or SD durability.

Physical acceptance remains pending for controlled broadcasters heard by one
or both receivers, matched responses and extended originals, repeated
Stop/Back/sleep/retry transitions, actual SD save/reopen, dense inventories and
sustained heap/stack behavior. No GPS or expansion backboard is required for
this monitor; their separate acceptance remains pending.
