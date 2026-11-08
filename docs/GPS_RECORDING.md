# GPS tracks and points of interest

Rev150 adds explicit GPX recording to **Scan → Wardriving → GPS DATA → GPX**.
The existing DATA and NMEA tabs remain available. The receiver line now shows
the configured RX pin and detected baud instead of a hardcoded 9600-baud label.

Choose TRACK or POINTS OF INTEREST before pressing START GPX. Start waits for a
fresh position and RMC UTC. TRACK writes at most one point per second; POINTS
OF INTEREST records a point only when MARK POI is pressed with a valid fix.
Marks use sequential names, such as `POI 1`. Stop finishes the file. Returning
to the Wardriving summary keeps an explicitly started GPX recording active;
leaving Wardriving requests both recordings to stop and waits for cleanup.

GPX files are local records under `/CERBERUS/GPS`: `GPS-T00001.GPX` for a track
or `GPS-P00002.GPX` for points. Use SD Files to copy them after stopping. They
are not WiGLE upload candidates. An ordinary wardrive still produces one
`CERBWardrive###.CSV`; enabling GPX is a separate action.

## Receiver and storage ownership

The S3 owns the GPS UART, UI and SD writer. GPS preview, Wardriving and GPX have
separate consumers of the same receiver, so starting or stopping GPX does not
reset a live fix or release another consumer's UART. During a concurrent
wardrive the C5 continues its assigned radio work. Standalone GPS recording
does not start an unnecessary radio scan.

The optional GPX task uses a 4 KiB stack and a fixed PSRAM context (1,648 bytes
in the host ABI). Up to four POIs can wait for the writer, with one in flight.
Each mark snapshots its position and UTC when accepted; later GPS loss does
not replace that point. A full queue rejects the new mark visibly. No file
I/O runs in the touch handler, GPS parser, or radio callback.

GPX and Wardriving serialize their SD writes through the existing mutex.
The GPX writer synchronizes at two-second intervals and on Stop. USB storage,
File Tools mutations and other exclusive recording workflows remain blocked
while its file is open. The UI also prevents opening LOGS / UPLOAD while GPX
is waiting or recording. Capture and upload remain separate, manual actions.

Files open lazily on the first valid point, using exclusive `.TMP` creation.
Only a successfully flushed, synchronized and closed document is renamed to
`.GPX`. Write or finalization errors retain the temporary file and report an
error. Existing files are never overwritten. There is a 64 MiB session limit.
Power loss may lose the unsynced tail; interrupted GPX recovery is not automatic.

## Position and motion data

Points require a checksum-validated, fresh position and RMC UTC, each at most
five seconds old. TLS clock holdover is not eligible. Unknown altitude,
satellite count or HDOP are omitted. HDOP is dimensionless, not a measured
accuracy in meters. Coordinates use seven decimal places; the antimeridian
is written as -180 to meet GPX's longitude bound.

Track segments restart after missing fixes, a backward UTC change, or a gap
longer than five seconds. Repeated polling of the same UTC fix adds no point.
Distance and speed are estimates from successive positions. Steps under three
meters and implied speeds over 120 m/s are excluded from those estimates, but
the original valid points remain in the GPX. These filters do not establish
position accuracy, and slow movement below the step floor can be undercounted.

The writer follows the [GPX 1.1 schema](https://www.topografix.com/GPX/1/1/),
including element order, UTF-8 names, XML escaping and optional fields.
Synthetic track and waypoint files also passed .NET XML schema validation
against the official `gpx.xsd` (SHA-256
`9e4d1988b862edbe556305b130f8f6f1b29864fefd0dc02d5dab04ccdd1f34d6`).

## Reference and verification limits

The behavioral reference is **JustCallMeKoko and the ESP32 Marauder
contributors**, [develop at 79cc6bc](https://github.com/justcallmekoko/ESP32Marauder/tree/79cc6bcba1db6679baee1dab2b0c1ad5010e6363):
`MenuFunctions.cpp` GPS/POI controls, `WiFiScan.cpp` GPX recording and Wardrive
POIs, `CommandLine.cpp` GPS POI commands, and `GpsTrackerStats.cpp` motion
estimates. Cerberus implements its own shared ownership, bounded worker,
storage lifecycle, segmented GPX formatter and UI; no upstream source file
was copied into this change. Existing Marauder notices and license remain in
the distribution.

Production-code host tests exercise formatting, UTC and coordinate bounds,
UTF-8/XML escaping, segments, queue limits, snapshot retention, allocation and
task failure, cancellation during startup, SD timeouts, partial writes and
flush/sync/close/rename failures. UI tests render DATA/NMEA/GPX and exercise
Start, Mark, Stop, mode locking, Back and cleanup. UART tests verify all three
consumers share one reader.

GPX recording, movement estimates, concurrent radio/SD performance and file
readback still need physical acceptance. Custom POI names are supported by
the bounded service API, but the touchscreen currently numbers marks. A map,
Marauder CLI command compatibility, geofencing, generated NMEA, PPS timing,
ATGM336H hardware acceptance and full Marauder parity remain open.
