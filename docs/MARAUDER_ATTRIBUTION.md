# Cerberus Unchained: credit to JustCallMeKoko and the ESP32 Marauder contributors

**Thank you to [JustCallMeKoko (Just Call Me Koko)](https://github.com/justcallmekoko),
creator of [ESP32 Marauder](https://github.com/justcallmekoko/ESP32Marauder),
and the contributors who help build it.** Cerberus's Marauder-derived features
benefit from their original code, implementation work, and shared knowledge.
Credit for that upstream work stays with its authors; Cerberus-specific ports,
adaptations, and integration are described separately.

From [@valleytechsolutions](https://github.com/valleytechsolutions):
"Koko gave me permission to bring this work into Cerberus, and I want to make
sure he gets his roses. Thank you for sharing your code and making this possible."

## License and original notices

The retained [ESP32 Marauder MIT license](../LICENSES/ESP32Marauder.txt) names
`Copyright (c) 2020 Just Call Me Koko`. Keep the complete license and original
copyright notices with the copied or derived material. Public acknowledgments
supplement those notices. The permission acknowledgment above records the
contributor's thanks; it does not change the upstream license or the separate
licenses of other code and data in this repository.

## Source provenance and later Unchained work

The [recorded baseline](MARAUDER_DEVELOP_BASELINE.md) identifies the pinned
upstream snapshots: `master` commit
[`511bb3694d0c4dc064888e762bde44fcc9b7f48a`](https://github.com/justcallmekoko/ESP32Marauder/tree/511bb3694d0c4dc064888e762bde44fcc9b7f48a)
and `develop` commit
[`79cc6bcba1db6679baee1dab2b0c1ad5010e6363`](https://github.com/justcallmekoko/ESP32Marauder/tree/79cc6bcba1db6679baee1dab2b0c1ad5010e6363).
Those snapshots have the same source tree. Future ports should record the
specific revision actually used rather than treating a moving branch as a pin.

Examples of source-level attribution already retained in this contribution:

| Cerberus source | Credited upstream material |
| --- | --- |
| [`shared/wifi_ap_clone.c`](../shared/wifi_ap_clone.c) | Marauder frame layout and post-SSID IE sequence; the comment names the upstream revision and file locations and identifies the Cerberus construction, bounds checking, and parser work. |
| [`shared/wifi_ap_job.c`](../shared/wifi_ap_job.c) | Packet layouts derived from the pinned Marauder revision, with Koko's copyright and a link to the retained MIT notice. |
| [`shared/wifi_sae_commit.c`](../shared/wifi_sae_commit.c) | Frame layout following the pinned Marauder source; the comment identifies the new bounded serializer and retains upstream credit. |

Later Unchained receive and persistence work includes original bounded codecs
and ownership/UI integrations informed by the pinned upstream workflows. The
Find My JSON schema and AP JSON/SSID text compatibility implementations are
described in [rev123](DUAL_RADIO_REV123.md) and
[Wi-Fi interchange](WIFI_MARAUDER_INTERCHANGE.md). Their format compatibility
does not imply imported observations have live source identity or that upstream
implementation code was copied.

The passive Probe Sniff reference is `WiFiScan.cpp`'s `RunProbeScan` and ordinary
`WIFI_SCAN_PROBE` callback branch, plus the reachable `MenuFunctions.cpp` entry,
at the pinned develop revision above. The rev128 Cerberus implementation
separates passive C5 capture from S3 validation, presentation and bounded SD
reports. Its [checkpoint documentation](DUAL_RADIO_REV128.md) records the scope
and remaining raw-capture/physical-acceptance limits. Source review, tests,
builds and installation are separate from over-the-air acceptance.

These are examples, not an exhaustive authorship inventory. The
[source inventory](MARAUDER_SOURCE_INVENTORY.json) and
[coverage ledger](MARAUDER_COVERAGE_LEDGER.md) record the wider reference baseline
and implementation status. Studying a feature as a reference, adapting its code,
and completing its hardware acceptance are distinct claims.

The selected AP/client Packet Count work uses JustCallMeKoko and the ESP32
Marauder contributors' `WIFI_SCAN_PACKET_RATE` workflow as a reference at
[`79cc6bc`](https://github.com/justcallmekoko/ESP32Marauder/commit/79cc6bcba1db6679baee1dab2b0c1ad5010e6363):
`MenuFunctions.cpp`'s Packet Count entry, `WiFiScan.cpp`'s `RunPacketMonitor`,
selected-target callback and count rendering, and `CommandLine.cpp`'s
`packetcount`/serial options. Cerberus's parser, independent AP/client accounting,
dual receiver ownership, scoped transport, controller and SD recording worker
are original implementations. This is behavioral reference credit, not a claim
that upstream implementation code was copied or that all options are complete.
The [design and acceptance contract](WIFI_PACKET_COUNT_PLAN.md) keeps serial
capture and physical count/recording acceptance separate.

## Wardriving upload workflow reference

Rev142's manual retry and WDGWars v2 integration reference JustCallMeKoko and
the Marauder contributors' `MenuFunctions.cpp` upload actions and `WiFiScan.cpp`
`uploadFile`/`wdgwarsUpload` at develop revision
[`79cc6bc`](https://github.com/justcallmekoko/ESP32Marauder/tree/79cc6bcba1db6679baee1dab2b0c1ad5010e6363),
rechecked on 2026-11-05. This is behavior/API reference credit. Cerberus's leased
worker, explicit service retry, strict response parser and durable receipt
journal are independently implemented. [Rev142](DUAL_RADIO_REV142.md) records
the provider references, differences and separate acceptance limits.

Rev143 references the same revision's `WiFiScan.cpp` wardrive CSV writer and
shared log stream. Cerberus independently implements the CERBWardrive filename,
one-file lifecycle, strict GPS/UTC/age checks, repeated-row retention and direct
retry UI. The file schema is checked against WiGLE's official CSV v1.6
specification; [rev143](DUAL_RADIO_REV143.md) records the behavior and limits.

## Carry this credit into future contributions

- Name **JustCallMeKoko and the ESP32 Marauder contributors** in the PR description
  whenever the change ports, adapts, or directly builds on their work. Link the
  [original project](https://github.com/justcallmekoko/ESP32Marauder).
- Identify the affected features, upstream files, and exact revision. Say which
  code was copied or adapted and which behavior was used only as a reference.
- Preserve the relevant source comments, original copyright notices, and full
  license text. Keep other upstream contributors' credits too.
- Describe the Cerberus-specific changes and validation separately, so readers
  can follow both the original work and the integration work.
- Keep this acknowledgment visible in the README and include the appropriate
  notices with distributions that contain the derived work.

The [pull request template](../.github/pull_request_template.md) includes an
attribution prompt to help carry these details forward.

Rev150 uses the same develop revision as a behavioral reference for GPS tracks
and POIs (`WiFiScan.cpp`, `MenuFunctions.cpp`, `CommandLine.cpp` and
`GpsTrackerStats.cpp`). The shared GPS consumer, bounded recorder, GPX formatter,
SD lifecycle and UI are Cerberus implementations. No upstream source file was
copied. See [GPS recording](GPS_RECORDING.md) for provenance and validation.
