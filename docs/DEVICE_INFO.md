# Device Info

Rev157 adds **Tools → System health → DEVICE INFO**. The S3 and C5 tabs show
each processor's application version, project, SDK, build date, chip revision,
core count, flash size, running application partition and ELF build hash.
Current STA/AP addresses are shown when that processor's Wi-Fi driver can
provide them; otherwise the derived default address is explicitly labelled
DEFAULT. Address masking uses the existing privacy setting.

The System tab shows S3 internal/PSRAM headroom, C5 heap and protocol,
reset/uptime, SD capacity/free space, the cached battery gauge and C5 connection
status/IP. These are sequential read-only observations. The read age starts
before collection; Refresh obtains another snapshot. A gauge value is an
estimate, not proof a battery is fitted or charging. A busy/unavailable SD or
C5 read shows its own error without hiding the S3 information.

The C5 API number is not its application version. The S3 displays the C5 API
revision it expects; the C5 displays the one it implements. Rev157 uses API
revision 80 and protocol 2. The two application versions can be compared
directly, but equal version strings alone do not prove a matched release.
The ELF hash comes from the IDF app descriptor. It identifies the build and
is distinct from the flash-image hash in the install package; it is not a
signature or hardware attestation. No board/FCC certification ID is invented.

## Read and cleanup behavior

Opening or refreshing the page allocates an optional PSRAM job and starts a
temporary 4 KiB worker. Rendering never queries the SD card or C5. The worker
reads SDK metadata, the shared storage service and the C5; it does not
initialize a radio, scan, connect, upload, change a MAC, or write a file.
Back requests cancellation and immediately returns to System health. The
worker storage is retained until it publishes completion, then freed. Sleep
uses the same cleanup; reopening waits for the old job before starting another.
The view and worker are absent during ordinary scan operation.

C5 identity uses a new object type in the existing fragment transport, not a
new SPI command number. Its 220-byte snapshot is validated for length,
generation, fragment position/flags, whole-object CRC, required strings,
reserved bytes and optional-field consistency. Unsupported C5 firmware,
timeouts and malformed responses publish no partial identity. Another reader
replacing the shared fragment object causes a visible read failure; Refresh
retries. The metadata does not assert a live radio session or target identity.

Each incremental C5 read attempts one transaction, with at most a 20 ms mutex
wait and the existing passive 5 ms READY budget, within a five-second reader
deadline. An already-running hardware transaction is not preemptible. SDK/SD
reads have their own service limits, so five seconds is not a bound on the
whole worker. No new receiver task or permanent diagnostic polling is added.

The read-only USB command **`builds`** prints S3/C5 build metadata and full ELF
hashes. Its temporary reader uses PSRAM and checks console Stop between
transactions. `info`, `health`, `memory` and the other existing commands remain
available. The build command does not print MAC addresses, credentials or
captured network information.

## Reference and validation

The feature mapping uses JustCallMeKoko and the ESP32 Marauder contributors'
[`RunInfo()`](https://github.com/justcallmekoko/ESP32Marauder/blob/79cc6bcba1db6679baee1dab2b0c1ad5010e6363/esp32_marauder/WiFiScan.cpp#L4944)
and Device Info menu. Cerberus uses its own SDK collector, fragment reader,
worker and UI. The original attribution and MIT notice remain unchanged.
The live `develop` ref was again identical to this revision on November 8,
2026. `WIFI_HOSTSPOT` appears only as a header constant in that source; no
reachable hotspot workflow was found under that mode. The existing Cerberus
Web UI remains a separate feature.

`check_device_info.py` compiles the production collector, parser, fragment
service/client, worker, view, shared widgets and console formatter. Synthetic
checks cover missing SDK metadata, current/default MAC distinctions, old
peers, every transfer-position failure, corrupt/stale/replaced fragments,
clock failures, allocation/task failure, cancellation and delayed release,
reopen/sleep/refresh, masked rendering and console output. Preview images use
the production font, header and button code with simulated device data.

## Installed checkpoint

Rev157 is installed on the development board from source
`9eeb75e26624a588747e8b0cd1b4b298f5e99b15`, tagged
`checkpoint/rev157-device-info`. Package:
`cerberus-unchained-0.1.0-dev.rev157-20261108T053334Z`.
Manifest SHA-256:
`79f756edc7cfec53df096bce4b7f818461b55e3f5773fffe88c954dfa824d85b`.

All 171 host suites and five native targets passed against unchanged inputs.
Fresh S3/C5 recovery backups matched the installed rev156 images. All nine
bridge/C5/S3 image writes passed verification. The live `builds` response
matched both packaged applications' version, project, SDK, compile time and
full ELF fingerprint. This exercises the new metadata transfer on the device.

Two paired idle snapshots about 89 seconds apart showed advancing uptimes,
mounted SD and no reset or increase in the listed SPI error counters. Four
initial READY timeouts/legacy fallbacks were present in both snapshots; mutex,
READY-fall, READY-high and driver errors were zero. The saved home Wi-Fi,
WiGLE and WDGWars configuration readiness flags load correctly. No connection
or upload was requested for this checkpoint.

The owner confirmed that both chip tabs show rev157 and that Refresh and Back
work. A read-only capture after navigation found the same free internal heap
(134,260 bytes), largest internal block (94,208 bytes), free PSRAM (1,426,140
bytes) and task count (10) as the preceding idle snapshot. Listed SPI error
counters were unchanged. These are observations from one navigation check,
not an endurance result or a whole-program stack bound.

Active-service coexistence, the temporary worker's stack headroom and
prolonged use still need acceptance. Full upstream network-detail, update/OTA
and CLI option parity remain separate work; SHOW_INFO remains partial in the
ledger. This local checkpoint is not a published release or complete Marauder
parity.
