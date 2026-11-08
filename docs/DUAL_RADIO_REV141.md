# Rev141: manual uploads and receipt error clarity

This update keeps Upload and Upload All manual, with the existing explicit
confirmation. One v1 JSON profile holds home Wi-Fi, WiGLE API name/token,
WDGWars token, Wardrift token and destination switches. The public example is
blank. Importing settings, stopping capture or rebooting never starts an upload.
No private values are compiled into firmware or included in this contribution.
Capture must stop before the existing worker takes SD and S3 home Wi-Fi/TLS
ownership. There is no additional task or network polling while recording.

The reported rev140 error was separate from profile import. Read-only device
flags confirmed saved home/WiGLE/WDGWars settings; physical SD inspection confirmed
the imported JSON was removed. A guarded restart preserved the saved readiness
flags and destination mask, with C5 online and no previous-reset fault. That check
ran with the card on the PC and did not connect or upload. The recorded CSV hash
was unchanged. Its journals
contained WiGLE SENDING→QUEUED and WDGWars SENDING→UNKNOWN. Both journals had valid
lengths and CRCs. A documented read-only WiGLE API query found the matching queued
transaction. A WDGWars `/api/me` query authenticated the key, but does not prove
acceptance of that CSV. No retry, receipt deletion or new upload was performed
by these checks. Provider processing/credit and the original WDGWars HTTP failure
remain unresolved; the rev140 journal did not retain that cause.

The worker now names the service when a saved uncertain receipt blocks a retry,
instead of leaving `VALIDATING SAVED CSV` above `ESP_ERR_INVALID_STATE`. New
attempts expose HTTP status, response length/completeness and transport error,
without credentials, response text or transaction IDs. Home Wi-Fi initialization
stages are identified before the operation that may fail. `upload_status` is a
read-only console snapshot; current-attempt diagnostics are not persisted.

Firmware source `ca7d42e213349af2e6e54491f710ca0ac9b8e852` passes all 166
registered host suites and five native targets (S3, onboard C5, temporary bridge,
expansion WROOM and expansion XIAO), bound to 1,299 unchanged inputs. Focused
coverage includes 52 strict JSON cases, 13 actual worker/storage scenarios,
mixed queued/uncertain retry, uncertainty holds, cleanup, HTTP diagnostics and
actual UI rendering. These synthetic checks do not establish provider delivery
or physical uploader cancellation. The Ubuntu GitHub Actions job also passed
for that source; Windows shards were still running when this checkpoint was
recorded.

Native READY, C5 Locate/parser placement and console-stack checks pass. The
known console frame path uses 2,240 of 4,096 bytes, leaving 1,856 bytes; this is
not a full SDK/libc/interrupt stack bound. S3 application size is 2,327,408 bytes,
1,760 bytes above rev140; C5 is 1,728,736 bytes and the bridge is 255,296 bytes.
All 22 package checksums verify for
`cerberus-unchained-0.1.0-dev.rev141-20261105T050937Z`, whose manifest SHA-256 is
`d4b955fb1d9e9df76994503f5da7988af5815b0d54839813431014da7b34b469`.
The effective firmware-source digest is
`ce17ebd86d15517c9c8378e7478879fc71dd399d1ce0c5aaa258bb445bf97df3`.
This local development package is not a published binary release.

The identified board is installed with the matched rev141 pair. Fresh rev140
recovery backups cover 2,396,160 S3 bytes and 2,097,152 C5 bytes, with baseline
image and flash-digest verification. All nine bridge/C5/S3 programmed image
digests verify. There was no blanket flash erase, NVS rewrite, SD-file deletion
or service upload during installation.

Two post-update idle captures 85.07 seconds apart confirm mounted SD, released
radio owners, advancing S3/C5 uptime, C5 protocol 2/capability 75 and no previous
reset fault. S3 internal free memory stayed at 135,268 bytes, largest block
94,208 bytes, free PSRAM 1,426,140 bytes and task count 10. Internal free memory
is 76 bytes below the rev140 baseline; the other listed memory values are
unchanged. C5 free memory stayed at 53,460 bytes. Four startup READY timeouts and
legacy fallbacks remain visible; none were added between samples, and the other
listed SPI error counters stayed zero. The observed console stack minimum after
diagnostic reads was 1,172 bytes, not a loaded-feature bound.

The saved profile remains readable after the paired update, with the same
home/service readiness flags and destination mask as before it. The new
`upload_status` command returned an idle state with zero upload attempts.
The owner then opened the saved log and confirmed that details and Back work,
with `WIGLE: QUEUED` and `WDGWARS: CHECK ACCOUNT` displayed without an error.
A subsequent console snapshot recorded a manual upload request held at
`WDGWARS / CHECK ACCOUNT FIRST`, with WiGLE QUEUED, WDGWars UNKNOWN and zero
new HTTP attempts. The service-specific hold is verified, but rev141 has no
on-device path to explicitly retry that uncertain receipt. This remains a
workflow gap. Capture behavior, RF completeness and live-service delivery are
not inferred from idle boot or these UI checks.

The [upload guide](WARDRIVE_UPLOADS.md) contains setup, limits and retry behavior.
The [rev140 checkpoint](DUAL_RADIO_REV140.md) retains installation evidence.
Wardrift remains provisional; full Marauder parity and longer capture/upload
endurance are outside this checkpoint.
