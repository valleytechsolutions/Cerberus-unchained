# rev147: wardrive upload clock recovery

A manual WDGWars retry reached home Wi-Fi, then timed out at `VERIFYING UTC FOR
TLS`. It never opened an HTTP connection. The saved uncertain receipt therefore
remained `CHECK ACCOUNT`; this failure did not establish an API-key rejection.

The uploader previously required a new response from `pool.ntp.org` on every
connection. It now reuses a synchronization from this boot for up to 15 minutes,
provided the wall clock still agrees with the monotonic clock within two seconds.
A reset, backward clock, excessive drift or expired sample requires another
time source. A plausible RTC date by itself is not accepted.

After leaving the Wardriving GPS view, the uploader can also use its recent,
checksum-validated RMC time and associated valid fix. It advances that time by
the measured elapsed interval. The 15-minute limit starts at the original NMEA
receipt, not at each upload. This read-only handoff does not start the UART,
retain a receiver task, or relax coordinate freshness for CSV recording.

If neither source is available, the uploader tries `pool.ntp.org` and
`time.cloudflare.com` in turn, allowing 15 seconds per server with cancellation
checks every 100 ms. Both follow normal UTC rather than mixing leap-smear
policies. The SNTP client is released after each attempt. Failure displays
`UTC TIMEOUT / LOG NOT SENT` before any service request or receipt mutation.

The time is established when starting an upload connection; Upload All keeps
the system clock running during that operation. CA, hostname and certificate
date checks remain enabled. Uploads and uncertain-result retries remain manual.
No credential, upload endpoint, CSV format or receipt format changed.

`upload_status` now includes the selected log name and each destination's
completion flag, so a saved receipt can be distinguished from the last transfer
attempt and from an incomplete multipart upload. It does not expose credentials
or response bodies.

## Validation

The focused network test covers recent-time reuse, backup-server fallback,
stale/invalid GPS dates, leap-day conversion, monotonic reversal, clock drift,
cancellation, time-setting failure and SNTP cleanup. The GPS UART test exercises
the real reader/parser with RMC followed by GGA, normal Stop, holdover expiry,
lock failure and a no-fix sentence. It confirms that time holdover does not make
stopped coordinates available for logging.

All 166 host suites and all five native firmware targets passed at `eaff9dd`,
with unchanged and matching source/test/configuration fingerprints. The GPS
UART fixture completed 2,864 checks. The previous uploader fails the new
repeat-connection regression; this implementation passes it. The existing log
manager lifecycle, import and retry scenarios also pass. No live service
requests are made by these fixtures.

The matched rev147 package passed all 22 checksum checks. Its manifest SHA-256
is `6afaf94022b5c04cd0d8d6284397c4313b726ed3f06fc06ca7f0a6faec6cd0d7`.

The paired firmware was installed on November 7 after fresh recovery backups
were verified against rev145. All nine bridge/C5/S3 images passed flash-digest
verification. Both chips booted, the SD card mounted and the saved upload
profile loaded. Idle observations 43.2 seconds apart showed advancing
uptime and SPI traffic without a reset or additional transport errors. The
backup still matched the supplied private profile; no settings or SD logs
were replaced by the update.

Before this update, a user-triggered new CSV upload on rev145 received a valid
WDGWars HTTP 202 acknowledgement and acknowledged results for both WiGLE and
WDGWars. That confirms the preceding TLS fix for the observed request. It
does not verify the new rev147 GPS/NTP recovery paths on hardware, service
processing, account credit or repeated-upload memory use; those remain open.
Existing uncertain receipts were not changed or automatically retried.

The time API follows [ESP-IDF 6.0.1's system-time interface](https://docs.espressif.com/projects/esp-idf/en/v6.0.1/esp32s3/api-reference/system/system_time.html).
The backup server is documented in [Cloudflare's NTP guide](https://developers.cloudflare.com/time-services/ntp/usage/).
