# Validation procedures and rev93 evidence

Rev94 adds eight dual-radio suites to the configured runner (115 total).
The recorded rev93 results below remain historical. Current rev94 build,
targeted-check results and hardware limits are recorded separately in
[Dual-radio scanning](DUAL_RADIO_SCANNING.md).

The contribution brings the rev82–93 passive observation, analysis, storage and Wardriving changes into the Unchained rev81 foundation. Host checks use synthetic fixtures and selected production C code. They run on the computer without programming a board or starting radio operations. Passing them does not establish hardware acceptance or complete Marauder feature parity.

## Host checks

Use Python 3.11 or newer and the pinned Zig Python package in the same environment:

```sh
python -m pip install ziglang==0.14.1
python tools/run_host_checks.py --list
python tools/run_host_checks.py --jobs 2
python tools/check_build_unified.py
python tools/check_field_kit.py
```

The current runner names 115 suites: 94 independent suites, followed by 21 serial UI suites that share generated headers or preview outputs. It retains all 107 rev93 suites, including every suite from the upstream 79-suite runner. `--jobs 1` reduces compiler memory use; the default is three workers. Each suite receives separate Zig global/local cache roots, and individual harnesses retain their own narrower cache settings. Do not run another copy of the runner concurrently in the same checkout.

The runner fails if a suite is missing, a suite fails, the Zig version differs, or fingerprinted inputs change during validation. Keep the checkout stable and run firmware builds separately. Source, synthetic data, production extracts and font-rendering fixtures are in this repository; no sibling integration folder, private console transcript, network capture or archived binary is required. A few older scripts contain a Windows MinGW fallback, but the full runner requires Zig and does not use it. Storage and thread fixtures retain their POSIX branches and explicit declarations for Linux.

Results are written to `artifacts/host-checks/host-checks.json`, including per-suite status/log hashes, input fingerprints, Python/Zig versions, cache roots and `hardware_validated: false`. Executables, generated headers and rendered previews stay under ignored `artifacts/` directories. The SD resource check and build-report/package-proof regression tests above are additional checks, outside the count of 115. Logs can contain local compiler paths; inspect them before sharing.

The host workflow runs the same commands on Linux and Windows. A configured workflow is not evidence that its current run passed: use the actual CI result or the receipt generated from the checkout being reviewed. Other historical scripts in `tools/` are not silently included in the 115-suite configuration.

## Firmware build matrix

Use the pinned PlatformIO Core and the package versions in the repository configurations:

```sh
python -m pip install platformio==6.1.19
python tools/build_unified.py --list
python tools/build_unified.py --target all
```

The five targets are the main S3 application, onboard C5, S3 programming bridge, WROOM C5 expansion and XIAO C5 expansion. The local platform adapter in `platform/espressif32-c5/` is required. The command builds only. Its report is `artifacts/integration/build-report.json`, with generated-toolchain identities, image sizes/hashes and input fingerprints. It checks S3 READY interrupt placement, onboard C5 raw-transmit linkage and the native console frame boundaries.

The console gate resolves Espressif GDB beside the compiler selected by the build, accepts its S3-specific or unified Xtensa executable name, and disassembles the local ELF without connecting to a target. It verifies the isolated command handlers, a permanent task-frame limit, and the known console/formatting call path. The JSON/text evidence is written under `artifacts/integration/`. This measured subset excludes deeper SDK/libc paths and interrupts; it is not a full stack bound or an observed runtime watermark. A missing debugger or changed instruction pattern fails the gate rather than being treated as a pass.

A clean build can resolve dependencies or generate SDK configurations and return `needs-review` because its inputs changed. Inspect the changes and rerun for a stable receipt. A failed compile or native gate must be fixed; rerunning alone is not evidence of correctness. Do not substitute an old binary or report. The manual candidate workflow warms up all five target configurations, runs the stable five-target gate, and then uses the Unchained matched-package tools. The standalone matched build also runs the console gate after its final S3 build. Packaging binds that proof to the exact ELF, console source and checker, rejects missing/stale evidence, and includes the JSON/text proof under `native-validation/`. Those checks also apply through the existing Unchained wrappers.

## Evidence and limitations

### Fresh Unchained contribution checks — 2026-10-31

The isolated Unchained contribution checkout passed the following checks on
Windows. Firmware compilation used Python 3.12.14 and the installed PlatformIO
Core 6.2.0; the documented CI pin remains 6.1.19. Host checks used Python 3.14.2
and an isolated `ziglang==0.14.1` environment. The first host invocation refused
the shared environment's unpinned compiler before executing any suite.

| Check | Result |
| --- | --- |
| `run_host_checks.py --jobs 3` | All 107 suites passed; no fingerprinted inputs changed. |
| `build_unified.py --target all` | All five targets and native gates passed on the stable repeat build; no fingerprinted inputs changed. |
| `build_unchained.py --target all --package` | Matched S3/C5/bridge development package created with the final S3 console proof. |
| `export_web_release.py <package> --verify-only` | Package hashes, chip headers, partitions and image bounds passed; no release files changed. |
| `check_build_unified.py` | Eight build-report and package-proof regression tests passed. |
| `check_field_kit.py` | 40,187 OUI lookups, Recon policy checks and 100,000 malformed expansion frames passed with the pinned compiler. |
| Coverage ledger | Default regeneration/check and optional verification against the clean pinned Marauder checkout passed: 95 modes and six workstreams. |
| Contribution review | Staged whitespace, new-guide local links and added-line private-path/identifier checks passed. |

The initial clean native pass compiled all five targets, but recorded
`needs-review` while generated SDK configurations and the final tooling inputs
were established. Its report was retained before the successful stable rerun.
The final native and host input fingerprints match. All three packaged onboard
application hashes match the stable native report; its console ELF is the same
one covered by the packaged proof.

| Native target | Application image bytes |
| --- | ---: |
| S3 main | 1,838,928 |
| Onboard C5 | 1,653,504 |
| S3 programming bridge | 255,248 |
| WROOM C5 expansion | 926,400 |
| XIAO C5 expansion | 930,624 |

The linked S3 console check measured a 112-byte permanent task frame and a
2,896-byte known console/formatting path within the 4,096-byte allocation,
leaving 1,200 bytes outside that measured subset. This retains the scope limits
described above. No device was flashed or exercised during contribution
preparation. These are local Windows results; Linux and remote CI results must
be read from the actual pull-request checks.

### Historical development evidence

The local rev93 development checkpoint recorded 107 passing host suites and five passing firmware targets after preserving the initial failed validation run. Those results apply to that local source/image set, not automatically to this merged Unchained checkout. The separate fresh results above cover this adaptation with upstream branding and Linux fixes. Private installation logs, raw observations, recovery images and device identifiers are intentionally excluded from the repository.

The bounded live inventories contain received address observations, not confirmed physical devices. Historical BLE archives contain retained original advertisement bytes, not a packet stream or raw scan responses. Wardriving receipt ages are S3 processing times; an accepted append is not proof of SD durability. GPS, the optional six-C5 backboard, sustained concurrent workloads and feature-specific hardware behavior need separate validation.

The local console exercise exposed a slow resource lookup and a preexisting Help command that returned its echo/prompt without the intended body. The resource command needed a separate timeout allowance; those observations are follow-ups, not proof that all diagnostic output or UI paths are correct. The PR does not repair or newly validate disruptive modes.

See [the contribution scope](CONTRIBUTION_REV93.md), [build and release guide](UNCHAINED_BUILD.md), and [coverage ledger](MARAUDER_COVERAGE_LEDGER.md). Build success, host simulation and configured CI do not establish complete integration or physical acceptance.
