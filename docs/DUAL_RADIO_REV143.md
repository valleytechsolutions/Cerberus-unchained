# Rev143: one CERBWardrive CSV and direct upload recovery

The owner found the separate WIGLE/UNLOC outputs confusing and requested one
Marauder-style WiGLE-compatible CSV named CERBWardrive for each recording.
Production Wardriving now lazily opens **CERBWardrive001.CSV** through
**CERBWardrive999.CSV** when its first qualifying observation arrives. Wi-Fi and
BLE rows from both onboard receivers, and located expansion observations, share
that one file. A GPS outage does not open another file or stop radio discovery.
Only observations with valid position, UTC, supported fields and acceptable RF
age enter the CSV. Repeated qualifying observations remain separate rows.

Missing fix/UTC, uncertain RF age and unsupported CSV observations have separate
counters. The screen displays the total not saved and the no-fix count, plus a
scan-only/waiting-for-fix message when appropriate. No placeholder coordinates
or receipt-time substitutes are added to an old RF observation. A session with
no qualifying rows creates no CSV. The existing 64 MiB per-file bound remains:
reaching it stops the session and preserves successfully written rows in its
single file, with a size-limit message, instead of silently rotating another
CSV. Write/sync failures retain recoverable TMP files rather than publishing
them as complete. Legacy non-production host fixtures retain their historical
evidence-stream tests; the ESP-IDF task always selects the production stream.

The log catalog accepts the longer names and shows new CERBWardrive recordings
before legacy WIGLE and UNLOC files. Old recordings are not renamed, rewritten
or deleted. UNLOC entries are labeled **LEGACY NO GPS**, and their details
explain that they cannot upload. The catalog is bounded to 2,997 possible native
names, with its entries allocated in PSRAM. Upload part names preserve the full
CERBWardrive stem. Existing receipt identities are unchanged.

For a selected log with one enabled uncertain service, the primary details
button now reads **RETRY WDGWARS UPLOAD** (or the corresponding service). It opens
the existing file/service confirmation directly. Cancel returns to the log.
If several services are uncertain, the upload confirmation asks the user to
choose one; its main button cannot accidentally issue an ordinary upload that
immediately hits the same hold. No HTTP request occurs before confirmation.
Other services and acknowledged parts remain skipped during a service retry.
Uploads remain manual and use the existing single v1 configuration profile.

A subsequent read-only rev142 snapshot recorded an actual WDGWars service retry,
with `ESP_ERR_HTTP_CONNECT` (28674), HTTP status 0 and no response bytes. This
establishes a connection-opening failure, not a CSV rejection or a bad API key.
It does not resolve the earlier uncertain delivery. Rev143 therefore also copies
the socket errno, ESP-TLS error, underlying TLS code and certificate flags before
closing the transport. The read-only diagnostic exposes numeric values only;
successful attempts reset them. This uses the installed ESP-IDF 6.0.1
[HTTP client diagnostic API](https://docs.espressif.com/projects/esp-idf/en/v6.0.1/esp32s3/api-reference/protocols/esp_http_client.html).
No certificate checks, redirects, retry confirmations or receipt protections
are relaxed. The underlying connection cause and live upload remain unverified.

The CSV schema follows WiGLE's [official v1.6 specification](https://api.wigle.net/csvFormat-1_6.html),
including the pre-header, Earth metadata, fixed 14 columns, UTC and CSV escaping.
JustCallMeKoko and the Marauder contributors' [WiFiScan.cpp](https://github.com/justcallmekoko/ESP32Marauder/blob/79cc6bcba1db6679baee1dab2b0c1ad5010e6363/esp32_marauder/WiFiScan.cpp#L6409)
is the behavioral reference for a shared WiGLE-compatible wardrive stream.
Cerberus's implementation and strict GPS/UTC gates are independent. Unlike that
reference's MAC history, the onboard stream continues to retain repeated valid
sightings, as requested. Format conformance is not proof of provider processing
or account credit.

Focused checks pass for GPS loss/recovery within one stream, no extra UNLOC
file, equal sightings, stale observations, full-file stop, longer-name parser
boundaries, actual storage/manager upload/delete paths, and rendered navigation.
Network fixtures additionally exercise connection failure, diagnostic collection
before transport destruction, zero CSV body writes and reset on a later success.
The worker suite covers 24 scenarios, including the new name through the actual
storage and receipt lifecycle. Firmware source
`6f57021b1d37cb9a53f3de442f296e0459d50eb5` passes all 166 host suites and five
native targets on 1,299 identical inputs. Native READY, C5 Locate/parser
placement and console-stack gates pass. The known console frame path uses
2,256 of 4,096 bytes; this is a partial static bound, not a whole-program or
loaded runtime guarantee. The S3 application is 2,331,248 bytes, C5 is
1,728,736 bytes and the bridge is 255,296 bytes. All 22 checksums verify for
`cerberus-unchained-0.1.0-dev.rev143-20261105T064800Z`.
Manifest SHA-256:
`8326908116c37900865dba938993c9aa75702d15e9edd99266165ef6c3f65bd1`.
Effective firmware-source digest:
`467ea23d997733b880464bbdd4c3600c31a67e2b0518956b81a620473c40426f`.

The matched rev143 pair is installed. Before restarting the completed rev142
upload error page, the installed source and fresh diagnostics established that
the worker had closed its CSV, completed receipt synchronization/close and
released Wi-Fi before publishing idle. All existing radio and storage checks
remained in place. A guarded non-flash restart released the idle page's logical
SD lease; fresh cold captures then passed the original strict flash admission.
No active recording, transfer, settings import or receipt deletion was triggered.

Fresh verified rev142 recovery backups cover 2,400,256 S3 bytes and 2,097,152
C5 bytes. All nine bridge/C5/S3 image writes verify. Two post-update idle
captures 79.47 seconds apart confirm mounted SD, released owners, advancing
uptime, C5 protocol 2/capability 75, boot count 1 and no previous reset fault.
S3 free internal memory remains 135,236 bytes, largest internal block 94,208
bytes, free PSRAM 1,426,140 bytes and task count 10. C5 free memory remains
53,460 bytes. Four startup READY timeouts and legacy fallbacks are retained;
neither increases, and the other listed SPI error counters remain zero. The
observed console stack minimum after the diagnostic sequence is 1,284 bytes.

The saved home/WiGLE/WDGWars profile loads after the update; Wardrift remains
empty/disabled. The complete CRC-valid profile in the fresh verified backup
matches every private configuration field and the owner-supplied key files.
Only match/readiness results are retained in public reporting. No upload was
started during installation or verification. A public PC connection also
verified WDGWars TLS 1.2 reachability; this does not establish the device's
connection cause or service acceptance.

Physical single-file capture, the direct retry controls and a rev143 WDGWars
response remain pending. The prior rev138 SD readback and rev142 connection
failure keep their original scope; installation alone does not establish these
new feature behaviors, long-drive completeness or complete Marauder parity.
