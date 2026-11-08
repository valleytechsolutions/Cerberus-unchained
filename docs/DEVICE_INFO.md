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

This is a development checkpoint. Physical navigation, active-service
coexistence, runtime stack/heap, actual descriptor comparison and prolonged
use still need acceptance. Full upstream network-detail, update/OTA and CLI
option parity remain separate work; SHOW_INFO remains partial in the ledger.
