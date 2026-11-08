# C5 BLE response transfer: next receive-side milestone

This is a source-grounded proposal following [rev112](DUAL_RADIO_REV112.md).
**The compact transfer described below is not implemented in rev112.**
Rev112 installation and acceptance evidence belongs to that checkpoint document.
This audit makes no new hardware timing or RF-coverage claim.

The subsequent [rev113 implementation checkpoint](DUAL_RADIO_REV113.md) records
the chosen format and validation separately. The proposal below is retained
as the original source audit, not as proof of implementation or acceptance.

## Current cost and user-facing gap

`s3_main/main/dual_ble_scan.c:snapshot_step()` downloads every response-mode row
as the 310-byte projection in `shared/dual_ble_protocol.h`: a 158-byte original
prefix and a separate 152-byte response sidecar. Every row therefore requires
31 ten-byte data reads, including canonical UNKNOWN and NONE responses.

The existing `tools/host_checks/dual_ble_check.c:check_response_reader()` asserts
32 rows require exactly **992 data reads**. BEGIN, optional inventory metadata,
and the final status fence add exchanges. `cerberus_dual_ble_tick()` permits at
most four exchanges and eight milliseconds of scheduled work per UI tick;
one in-flight transaction can exceed that budget. `ui_task()` also retains its
20 ms delay. These are source/test facts, not measured wall-clock latency.

The pinned Marauder develop reference is
`79cc6bcba1db6679baee1dab2b0c1ad5010e6363`, locally available at
`../integration/marauder-develop-2026-10-29`. Its
`esp32_marauder/WiFiScan.cpp:bluetoothScanAllCallback::onResult()` updates local
lists, while `RunBluetoothScan()` configures continuous scanning. Cerberus adds
an inter-chip publication cost absent from that local callback path. Improving
this cost serves the existing scan-then-select workflow without another menu.

## Proposed bounded change

Add a separately versioned, optional compact-response read command. Preserve
the existing 158-byte original prefix and explicitly encode the response state:

| Frozen response state | Proposed projection | Reads per row | Reads for 32 rows |
| --- | --- | ---: | ---: |
| UNKNOWN | 160 bytes: original prefix plus version/state | 16 | 512 |
| NONE | 174 bytes: prefix plus response prefix through original sequence | 18 | 576 |
| PRESENT | Existing 310-byte full representation | 31 | 992 |

For U UNKNOWN, N NONE and P PRESENT rows, proposed data reads are
**16U + 18N + 31P**, versus **31(U + N + P)** today. These are format arithmetic,
not measured savings. An all-PRESENT bank receives no transfer reduction.
NONE must preserve its nonzero 64-bit original sequence; UNKNOWN remains
unavailable correlation evidence, not proof that a response was absent.

The producer can project the existing immutable segments in
`c5_firmware/main/dual_ble_service_impl.h:dual_snapshot_command()` without a new
result bank or a full-row automatic copy. The S3 reconstructs the existing
310-byte row shape, then runs the existing validation and publication path.
No RF mode, scan duration, acquisition default, family filter, archive version,
or active-action eligibility changes are proposed.

## Compatibility and evidence boundaries

- Older peers retain the full `READ_RESPONSE` path. An unsupported optional
  projection discards all staging and rereads the **same frozen handle** using
  the full representation. It must not begin another snapshot or RF job, move
  the first-BEGIN anchor, or silently change response acquisition to passive.
- Omitted bytes are reconstructed only where the new format explicitly defines
  canonical zero fields. A short, failed or truncated transfer is never evidence
  of UNKNOWN or NONE. Invalid state/version/length combinations fail closed.
- Preserve full boot/job/epoch ownership through the frozen handle, exact typed
  identities, per-chunk handle/index/chunk echoes and explicit error responses.
- Preserve idempotent lost-BEGIN handling, immutable frozen contents and the
  mandatory final boot/handle/count/flags fence before publishing the whole bank.
- Original and response retain separate raw previews, metadata, decoded hints,
  RSSI and age bounds. Download time may increase an age bound; it cannot make
  an observation newer. No address/name-based physical-device merge is implied.
- Keep painted selections and SD input copies immutable, Stop priority intact,
  and the current exchange/time budget. Do not expose partially staged rows.

## Validation and limits

Extend existing production fixtures:

- `dual_ble_owner_check.c`: actual frozen producer projections, canonical suffix
  validation, old full-read compatibility, mutation after freeze and invalidation.
- `dual_ble_check.c`: mixed 32-row states, exact read counts, unsupported fallback
  at every row/chunk boundary, lost BEGIN replies, retained timing, malformed
  echoes/lengths, final fences, Stop during transfer and failed-publication atomicity.
- `dual_ble_integration_check.c`: actual C5 callback to frozen projection to S3
  reconstruction, proving equivalence to the existing full response representation.
- Existing firmware-cache, live selection and archive save/reopen fixtures:
  optional-command compatibility and unchanged evidence after reconstruction.

Consider bounded cached transfer-read counts and publication-duration metrics
alongside existing first-result/publication diagnostics. Keep diagnostic getters
free of SPI and clocks, and distinguish transfer time from RF observation time.
Host checks can prove exchange reduction and reconstructed evidence equivalence.
Native checks must verify memory/frame costs. Physical acceptance still needs
measured publication latency, dense mixed-state results, Stop/Back/next-scan
transitions and sustained heap/stack headroom. No speed multiplier is promised.

## Separate later gap

The dedicated `s3_findmy_monitor_ui.h` workflow remains S3-only, even though the
ordinary Find My family list already presents both receivers. A dual-source
Find My monitor is a later substantive candidate after transfer work, with its
own source-labelled history and lifecycle design. Complete raw-payload/history
recording is also unfinished and would increase transport/storage demands; it
is not included in this proposal.
