# Rev124: saved Wi-Fi observations and Recon field dashboard

Source version is `0.1.0-dev.rev124`; onboard C5 capability revision remains
69. Implementation and independent source reviews are complete. The final
134-suite host run and all five native targets pass on the same 1,108 unchanged
inputs, and the linked resource audit passes. The matched rev124 pair is now
installed with verified writes and paired application boot. Physical feature
acceptance remains separate; the new touchscreen and SD workflows are pending.

## User-facing scope

- Save the complete or selected combined S3/C5 Wi-Fi results to SD, reopen
  them as AP or SSID lists, inspect historical observations and save a copy.
  See [Saved Wi-Fi observations](WIFI_SAVED_LISTS.md).
- Replace the Recon mascot with a factual field dashboard, retaining the
  independent C5 Wi-Fi/S3 BLE scheduler, existing inventory and optional SD
  recorder. The dashboard and optional signal/radar view use the same
  observations; the design does not infer bearing, distance or threat from a
  category. See [Recon inventory and dashboard](RECON_INVENTORY.md).

## Validation and resources

Final host evidence is retained in `artifacts/rev124-host-evidence`, with all
134 receipt-listed log hashes verified. Native evidence for S3, onboard C5,
temporary USB bridge, expansion WROOM and expansion XIAO is retained in
`artifacts/rev124-native-evidence`. The resource report is
`artifacts/device-rev124/native-memory-comparison.json`.

The first host run passed, but the first native S3/bridge compilation rejected
two dashboard `snprintf` expressions whose inferred maximum output exceeded
their buffer. Explicit bounds now retain the complete formatted address and
signal strings. The actual Recon UI fixture, full host run and full native
matrix were rerun on the corrected source. The original host, preview and
failed-native evidence remains under `artifacts/device-rev124/first-*`.

| Resource | Rev123 | Rev124 |
|---|---:|---:|
| S3 application image | 1,978,976 B | 1,973,984 B |
| S3 fixed data/BSS | 174,156 B | 169,356 B |
| C5 fixed data/BSS | 83,184 B | 83,184 B |
| Optional Recon inventory/view store | 18,184 B | 18,944 B |
| Existing dual Wi-Fi coordinator | 10,784 B | 10,840 B |
| Existing dual Wi-Fi live view | 5,648 B | 5,664 B |
| Optional saved Wi-Fi view | absent | 5,920 B |
| Wi-Fi archive worker allocation | absent | 11,176 B |

Removing the old art renderer, stripe and render cache reduces fixed S3
data/BSS by **4,800 bytes** while adding the dashboard. Its presentation state
adds 760 bytes to the existing optional Recon PSRAM store; the inventory model
and frozen observation copies remain unchanged. No new Recon task, receiver,
framebuffer or logger is created.

The Wi-Fi viewer plus worker requires 17,096 bytes of optional PSRAM, excluding
allocator overhead. Including the retained dual Wi-Fi coordinator and live
view gives 33,600 bytes across those four allocations, not a whole-device peak
bound. Existing BLE and JSON worker allocations remain 57,648 and 58,760 bytes.
Configured task stacks are unchanged. Selected native entry frames include
1,088 bytes for the UI loop (unchanged), 320 for `watch_draw`, 848 for
`wifi_archive_tick`, 176 for capture, and 240 for decode. These figures do not
sum to a worst-case call chain or establish loaded runtime headroom.

Final production-renderer previews, using synthetic fixture data rather than
device captures, are retained with hashes in
`artifacts/device-rev124/previews/manifest.json`. Tests cover typed deduplication,
latest graph buckets, frozen row selection, failed painting, low-memory radio
and SD status, coverage warnings, view switching, recording controls and
ownership cleanup. Saved Wi-Fi tests cover full and selected reports, malformed
files, source provenance, sorting/grouping, cancellation, exact job ownership,
storage failure and reopen behavior. Real touchscreen/radio/SD performance,
power-loss recovery and sustained-use acceptance remain separate.

## Local package and installation

The installed package is
`dist/cerberus-unchained-0.1.0-dev.rev124-20261101T212044Z`, built from source
commit `064fde3d39d645c63992ee02afd8051d63e6634e`. Its manifest SHA-256 is
`f4cfde5f151a3a6215142af39d8e87b512e6f5c48495e70746077860941ced8e`.
The compiled/package checkpoint remains in
`artifacts/device-rev124/local-milestone.json`; its original `installed: false`
records that earlier gate and is not rewritten after flashing.

The separate `artifacts/device-rev124/installation-receipt.json` records
verified paired installation and application boot. Its SHA-256 is
`390333fc3119ea339e2c38938d3a6cb7a8df5f931264ab280e9302c6acbd0f81`.
Fresh 2 MiB backups of each processor matched the installed rev123 images,
passed flash verification and covered every erase-rounded update write. The
temporary bridge and both final applications each passed verification of all
three programmed images. Higher flash, including existing crash evidence,
was preserved. No release, push or pull request was made for this checkpoint.

The application reports rev124, C5 capability 69 online, a buffered display,
mounted SD and idle radio/storage owners. Two captures 46.03 seconds apart
show advancing S3/C5 uptime, C5 boot count 1 and no additional listed SPI
errors. The four initial READY timeouts and four legacy fallbacks were stable;
mutex, falling/high READY and driver errors remained zero. The current boot
reports no previous reset fault; an older crash record remains intact.

Both candidate captures report 137,612 bytes of free S3 internal memory,
a 94,208-byte largest internal block, 1,427,552 bytes of free PSRAM and ten
tasks. The fresh rev123 baseline had 132,812 internal bytes free, consistent
with the 4,800-byte fixed-memory saving. The minimum observed console stack
margin across these captures was 1,372 bytes. These are sampled idle values,
not loaded-operation, worst-case stack or sustained-use bounds.

Physical dashboard/radar navigation, Pause/Resume/Log/Back, recording and
stop/save, Wi-Fi Save All/Selected and reopen/copy, repeated sessions and
Stop/Back/sleep still need device acceptance. No RF or SD-write acceptance
was inferred from the image transfer, boot or SD mount.

## Current upstream reference

A read-only comparison at **2026-11-01 21:08:33 UTC** found Marauder `develop`
identical to
[`79cc6bcba1db6679baee1dab2b0c1ad5010e6363`](https://github.com/justcallmekoko/ESP32Marauder/tree/79cc6bcba1db6679baee1dab2b0c1ad5010e6363):
zero ahead, behind or intervening commits. The clean local source matches
that revision. The response is retained at
`artifacts/device-rev124/marauder-develop-check.json`.
This is a point-in-time source check, not a full port or acceptance claim.

The Recon classifier, evidence integration and UI are Cerberus code. Existing
[signature references](WATCH_SIGNATURES.md) and
[third-party notices](WATCH_THIRD_PARTY_NOTICES.md) remain. No SquachWatch or
SquatchWatch code, signature table, artwork or character is imported.

## Remaining scope

The full Marauder port is unfinished. The source ledger retains its existing
status assignments; this checkpoint does not mark all modes complete.
Historical Wi-Fi/ BLE action reports, native AP/SSID interchange and editing,
GPS and six-C5 expansion acceptance, complete receive payload/history and
sustained-load testing remain open. No new transmit engine or frame-rate
change is part of this checkpoint.
