# Rev125: Marauder AP and SSID file interchange

Source version is `0.1.0-dev.rev125`; onboard C5 capability revision remains
69. Implementation and independent source reviews are complete. All 136 host
suites and five native targets pass on the same 1,118 unchanged inputs; the
linked native resource audit also passes. The matched local package is finalized.
The last verified installed pair is [rev124](DUAL_RADIO_REV124.md); rev125 was
not installed. Owner feedback on Recon is being addressed in the next revision
before the next device update.

The milestone adds offline AP JSON and SSID text import, inspection, copy
saving and export from the native combined S3/C5 saved Wi-Fi report. See
[the interchange contract](WIFI_MARAUDER_INTERCHANGE.md) for upstream schema
differences, explicit information loss, file ownership and resource bounds.
The full Marauder port remains unfinished; imported files do not create live
radio targets or change transmit behavior.

Focused checks exercise 18,191 codec assertions, 4,687,524 storage assertions
across 219 file cases, 67,236 worker assertions and 2,312 new UI assertions.
The existing native Wi-Fi and AirTag JSON UI suites pass compatibility checks.
The read-only archive console now reports the captured format and only its
matching union's conversion statistics; its formatter/lifecycle fixture passes
4,911 assertions. These are host results, not physical device measurements.

The complete host receipt and its 136 hash-verified logs are retained in
`artifacts/rev125-host-evidence`. Native S3, onboard C5, temporary USB bridge,
expansion WROOM and expansion XIAO evidence is retained in
`artifacts/rev125-native-evidence`. Sixteen production-renderer previews using
synthetic data are bound to the final host receipt in
`artifacts/device-rev125/previews/manifest.json`; they are not device captures.

The initial full host run caught an outdated exact console-output expectation
after the format diagnostic was added. The fixture now requires the additional
format line without weakening its full-string comparison. Both complete
matrices were rerun on the corrected inputs. Initial evidence is preserved in
`artifacts/rev125-first-validation`.

## Native resources

The comparison is bound to the retained rev124 and rev125 ELFs and installed
rev124 receipt in `artifacts/device-rev125/native-memory-comparison.json`.

| Resource | Rev124 | Rev125 |
|---|---:|---:|
| S3 application image | 1,973,984 B | 1,993,440 B |
| S3 fixed data/BSS | 169,356 B | 169,388 B |
| C5 fixed data/BSS | 83,184 B | 83,184 B |
| Optional interchange view | absent | 10,168 B |
| AP JSON worker allocation | absent | 75,784 B |
| SSID text worker allocation | absent | 8,080 B |
| Existing native Wi-Fi worker | 11,176 B | 11,176 B |

Existing BLE and AirTag worker allocations remain 57,648 and 58,760 bytes.
The archive context's maximum union size grows, but operation-specific
allocation keeps those older paths unchanged. The AP arm includes a 65,536-byte
file buffer; the SSID arm shares its conversion-source space with its encoded
bytes. The optional view plus AP worker requires 85,952 bytes of PSRAM, or
18,248 bytes for the SSID path, excluding retained native reports, allocator
overhead and other services. No additional persistent radio or worker task is
introduced. Fixed S3 data/BSS increases by 32 bytes.

The Recon inventory, dashboard, dual Wi-Fi coordinator, live Wi-Fi view and
existing saved Wi-Fi/BLE/AirTag models retain their rev124 sizes. Configured
task stacks and C5 fixed sections are unchanged. Selected entry frames include
1,088 bytes for the UI loop, 864 for native Wi-Fi archive polling, 240 for AP
parsing, 80 for the archive worker, and 48 per recursive JSON skip entry. The
bounded recursive parser still accumulates caller and nested frames; these
measurements are not a whole-stack or runtime peak bound.

The console gate reports an 80-byte formatter frame and 2,128 bytes in its
measured frame subset, leaving 1,968 bytes of the configured 4,096-byte stack
outside that subset. Deeper SDK/libc/interrupt use and loaded high-water marks
remain unmeasured. The first debugger audit reached GDB's default value-size
ceiling when inspecting the larger union; its outputs are retained under
`first-native-audit`. Raising only the debugger inspection ceiling allowed the
unchanged ELF to be measured successfully.

Physical SD import/export, copy/reopen, Stop/Back/sleep, repeated sessions,
cross-device file transfer and loaded memory/performance remain acceptance
work. Upstream Marauder's smaller JSON document budgets can reject a
format-valid Cerberus export; the format guide describes that limitation.

## Current upstream reference

A read-only GitHub comparison at 2026-11-01 21:37:53 UTC found Marauder
`develop` identical to `79cc6bcba1db6679baee1dab2b0c1ad5010e6363`.
The response is retained in
`artifacts/device-rev125/marauder-develop-check.json`.

## Final local package

Source commit: `df4a89f7a0e1c89f644629ca7d94bdc2d7cf6123`.
Package: `dist/cerberus-unchained-0.1.0-dev.rev125-20261101T221400Z`.
Manifest SHA-256:
`e6ae7d16e53e5650466d3a8ca0afb4ca425e4fd68babb4d7e633b8931aa42013`.
The immutable `artifacts/device-rev125/local-milestone.json` binds the source,
all nine matched images, 136-suite host evidence, five-target native evidence
and the resource audit. No rev125 image was written to the device. No push,
release or pull request was made.
