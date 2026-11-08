# Rev142: WDGWars queue support and manual service retry

The installed rev141 log details worked, but the owner could not upload again:
the worker held an uncertain WDGWars receipt with no on-device recovery route.
Rev142 adds an explicitly confirmed retry for one selected file and service.
Normal Upload/Upload All still skip acknowledged parts and hold uncertain ones.
The retry appends to the existing journal, preserves the CSV, skips acknowledged
parts of the same service and does not send to other destinations. No automatic
upload or retry is added; the single v1 JSON profile is unchanged.

Open the affected log, tap **UPLOAD THIS LOG**, then the service's
**REVIEW RETRY** button. The confirmation identifies the file and service and
explains that the earlier attempt may already have arrived. Cancel returns to
the upload confirmation, then Back returns to the file. **CONFIRM SERVICE RETRY**
requests only that service for the reviewed file. There is no bulk uncertainty
override, receipt deletion or bypass for damaged journals.

The current Marauder `develop` branch was checked on 2026-11-05 at
`79cc6bcba1db6679baee1dab2b0c1ad5010e6363`. Its `MenuFunctions.cpp` offers individual
WiGLE/WDGWars/Both actions and passes an explicit retry flag to `uploadFile`.
Its `WiFiScan.cpp` uses `/api/v2/upload-csv` and recognizes HTTP 202 for WDGWars.
Cerberus now uses that v2 endpoint with `Accept: application/json` and records
a complete `ok: true` response with a valid positive job ID as **QUEUED**.
The job ID is retained in the existing receipt. Queue admission is not proof
of processing, import counts or credit; automatic job polling is not implemented.
Synchronous 200/import-count replies remain supported. TLS CA/hostname validation,
bounded buffers, cancellation and redirect rejection remain in force.

Credit: JustCallMeKoko and the ESP32 Marauder contributors supplied the upload
workflow/API reference. Cerberus's worker, confirmation gates, receipt protocol
and strict response parsing are independent implementations. See the
[attribution record](MARAUDER_ATTRIBUTION.md), Marauder's
[uploader](https://github.com/justcallmekoko/ESP32Marauder/blob/79cc6bcba1db6679baee1dab2b0c1ad5010e6363/esp32_marauder/WiFiScan.cpp#L11997)
and [menu](https://github.com/justcallmekoko/ESP32Marauder/blob/79cc6bcba1db6679baee1dab2b0c1ad5010e6363/esp32_marauder/MenuFunctions.cpp#L4198).
The provider's [uploader reference](https://github.com/LOCOSP/pineapple_pager_wdgwars/blob/main/wdgwars/uploader/wdgwars.py)
also documents v2 job IDs and read-only upload history. A bounded authenticated
history check found no exact match for the affected generated filename among
the latest 50 entries. That is not proof of historical non-delivery; the
original uncertain attempt's HTTP response was not retained.

Focused validation passes 200 service/JSON cases, 23 actual worker/storage
scenarios and the production network/UI fixtures. Cases cover stale confirmation,
wrong file/service, disabled destination, cancellation, network failure, damaged
receipts, a receipt becoming acknowledged after review, preserving other-service
acknowledgements, two-part files and a queued retry being skipped on the next pass.
The actual rendered retry screens were checked for layout and Cancel/Back routing.
These fixtures use synthetic files and network boundaries; live service
acknowledgement remains a separate acceptance step.

Firmware source `8cfe45f6d5cb3c085a442fc4289cc62d1c6766a1` passes all 166 host
suites and five native targets on 1,299 identical inputs. Native READY, C5
Locate/parser placement and console-stack gates pass. The known console frame
path uses 2,240 of 4,096 bytes; this is a partial static bound. The S3 application
is 2,329,744 bytes, 2,336 bytes above rev141. C5 remains 1,728,736 bytes and the
bridge 255,296 bytes. All 22 package checksums verify for
`cerberus-unchained-0.1.0-dev.rev142-20261105T055639Z`.
Manifest SHA-256:
`501167b7ca9e5da013589a774d04938c2e55a0d806a06db86026a97c9f2d736e`.
Effective firmware-source digest:
`3bfbaa8e336321fe419a3d8dcc8da71ee96afea2466555474c77e3089b3f36be`.
This is a local development package, not a published binary release.

The matched rev142 pair is installed. Fresh rev141 recovery backups cover
2,396,160 S3 bytes and 2,097,152 C5 bytes, with baseline-image and flash-digest
verification. All nine bridge/C5/S3 image writes verify. No blanket erase, NVS
rewrite or SD-file deletion was performed. Two post-update idle captures
62.45 seconds apart confirm mounted SD, released radio and log-manager owners,
advancing S3/C5 uptime, C5 protocol 2/capability 75 and no previous reset fault.

Across those samples, S3 free internal memory stays at 135,268 bytes, largest
block 94,208 bytes, free PSRAM 1,426,140 bytes and task count 10. C5 free memory
stays at 53,460 bytes. Four startup READY timeouts and legacy fallbacks remain
visible; none were added between samples, and the other listed SPI error
counters stayed zero. The observed console stack minimum after diagnostics
was 1,252 bytes; this is not a loaded-feature bound. These captures do not
establish RF completeness or upload performance.

The saved home/WiGLE/WDGWars profile remains ready after the paired update;
Wardrift remains empty/disabled. Both idle upload snapshots show zero attempts.
The owner reported another CHECK ACCOUNT message. The console recorded the
normal Upload action held on the existing WDGWars receipt, with zero new HTTP
attempts. A later read showed inspection of a legacy unlocated log, which cannot
upload. The new explicit retry itself has no physical success evidence yet.
This feedback drives the clearer direct retry button and single-session CSV
layout in rev143; the original uncertain receipt remains retained.

An offline comparison of the fresh verified recovery backup found one complete,
CRC-valid v1 settings blob. Every field matched the private profile, and its
WiGLE name/token and WDGWars token matched the owner-supplied key files. The
post-update profile load succeeded. Only match/readiness results are recorded;
credentials and backup contents remain private. This verifies configuration
persistence, not current network reachability or service delivery.

A later snapshot at 2026-11-05 06:41 UTC recorded the explicit retry action for
WDGWars, with an attempted connection and `ESP_ERR_HTTP_CONNECT` (28674).
HTTP status and response bytes were zero; WiGLE remained QUEUED and WDGWars
remained UNKNOWN. This supersedes the earlier zero-attempt observation for that
later action only. Rev142 has no retained socket/TLS detail, so it cannot
distinguish the underlying connection cause. Rev143 adds those diagnostics.
