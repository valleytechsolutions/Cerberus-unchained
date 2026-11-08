# Rev144: explain missing Wardriving logs

This checkpoint addresses a report that no Wardriving logs were visible.
The installed rev143 diagnostic captured a stopped, approximately three-second
session with 138 observations, zero SD rows, zero saved files and 138 observations
skipped for missing valid GPS/UTC. The receiver was reading checksum-valid NMEA
at 115200 baud but reported no fix or valid UTC. No service or SD error was
reported. These measurements explain why that session created no CSV; they do
not establish whether earlier files were visible on the touchscreen.

The Wardriving page now explicitly says **STOPPED / NO CSV SAVED**, retains the
reason after Stop, and distinguishes it from **CSV SAVED / OPEN LOGS BELOW**.
A later GPS fix cannot rewrite the outcome of a completed session. During a
running scan without valid GPS/UTC, the state explains that it is waiting for
GPS. Radio discovery and the existing S3/C5 division of work remain unchanged.

The log browser shows the unsuccessful last-session outcome above any older
files. An empty, successfully read catalog explains the save sequence and the
`/CERBERUS/WARD` location; a catalog failure says **LOG LIST UNAVAILABLE** rather
than claiming there are no files. Successful recordings continue to use one
`CERBWardrive###.CSV` with valid GPS/UTC rows. No placeholder coordinates,
header-only successful recordings, extra UNLOC files or automatic uploads are
introduced.

A read-only `ward_logs` diagnostic uses the same leased SD catalog as the log
browser and reports only finalized native filenames, sizes and numeric errors.
It does not read CSV rows, start the upload worker, connect to a network, delete
files or alter upload receipts. Its PSRAM allocation and SD lease are released
before console output. Active SD ownership is respected.

Regression fixtures exercise stopped sessions with GPS disabled, missing fix,
unmatched observation times, unsupported rows, no observations, and a successfully
saved CSV; later GPS recovery; an empty catalog versus a catalog error; older
files remaining visible after an unsaved session; and the actual diagnostic
catalog through leased storage, including busy and allocation failures. These
fixtures do not prove physical touchscreen acceptance or a newly recorded file.

The catalog fix in `7a6e36c94e7163cfc76794b6b572c10cb860c21b` passed all 166 host
suites and five native targets on 1,300 identical inputs. All 22 package
checksums verify. The package is
`cerberus-unchained-0.1.0-dev.rev144-20261105T080205Z`, with manifest SHA-256
`53c450c1624eb1817d95e1f8410097c9a43180a09fd416618cff71cd824d14fa`
and firmware-source digest
`e845db43eae8735d1afc058c131729e7313a7cc4c820e13d60d60dd0949b2b52`.
The S3 application is 2,333,584 bytes; C5 is 1,728,736 bytes and the bridge is
255,296 bytes. Native placement and console-stack checks pass; the known
console frame bound remains 2,256/4,096 bytes, not a complete runtime bound.

The S3 catalog fix was installed with a fresh verified 2,400,256-byte
recovery backup and all three programmed image digests verified. Its C5 and
bridge binaries exactly match the preceding verified rev144 paired update,
so they were retained. That baseline has fresh verified backups and all nine
image writes verified. Two final boot samples 91.94 seconds apart show mounted
SD, released owners, advancing uptime, C5 protocol 2/revision 75, boot count 1,
and no previous reset fault. S3 free internal memory remains 135,236 bytes,
largest block 94,208, free PSRAM 1,426,140 and task count 10. C5 free memory
remains 53,460 bytes. Four startup READY timeouts/legacy fallbacks do not
increase; other listed SPI error counters remain zero. The observed console
stack minimum after diagnostics is 1,284 bytes. Profile load succeeds; the
CRC-valid backup matches every private-profile field and supplied key.

At the time of the catalog check, `/CERBERUS/WARD` contained one
non-CSV/non-TMP entry: **zero CSV entries, zero temporary captures and zero
unrecognized CSV names**. The empty catalog matched the contents of the card.
The reason earlier recordings were absent is unknown. The last session lacked
valid GPS/UTC and added no new file. No recording, upload, file deletion or receipt modification was initiated
during this validation. Earlier private SD readbacks remain preserved locally.

The diagnostic now exposes directory-present, directory-entry, CSV, temporary
and unrecognized-CSV counts, plus one bounded hex-encoded unrecognized basename
when applicable. Host fixtures distinguish a missing directory from filtered
files, temporary captures and successfully recognized logs without reading CSV
contents.

## Confirmed uploads

Commit `336b2cc` shows **UPLOADED** for a service once all parts of the file
have confirmed receipts. A WiGLE transaction ID confirms receipt of its part.
WiGLE processes the file separately. The worker can read receipts written by
earlier builds. A regression test interrupts a two-part upload after the first
receipt and verifies that resuming sends only the second part.

Before this update, a stopped rev144 drive reported 1,182 geotagged rows, one
saved file and zero S3 or C5 queue drops. Those figures come from the device
counters. The earlier rev138 test includes a direct [CSV readback](DUAL_RADIO_REV138.md).

All 166 host suites and five firmware targets passed at `336b2cc`. The updated
S3 image was installed and verified. The C5 and bridge images matched the
installed copies. Two boot checks 59 seconds apart showed both chips online
and the SD card mounted. The saved upload profile loaded after restart. The
new status label was checked with the UI test harness. Its touchscreen check
is still open.

WiGLE previously returned a transaction ID. The last WDGWars attempt failed
during the TLS handshake on the device. WiGLE processing and live WDGWars
and Wardrift uploads still need confirmation.
