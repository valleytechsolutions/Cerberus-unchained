# Wardriving GPS admission and RSSI extrema

RSSI extrema filtering was introduced in rev148. Rev149 simplifies the GPS
start control. These changes affect new recordings; existing CSVs and upload
receipts are left intact.

## Starting a recording

Open **Scan → Wardriving** and tap **START**. GPS is always enabled on this
page; its pins, baud and receiver profile still come from **Settings → GPS**.
The **PREVIEW** control in Settings affects only the Settings page.
If a fresh fix and RMC UTC are already available, scanning starts immediately.
Otherwise the screen shows **WAITING FOR GPS** and starts automatically when
both are ready. There is no acquisition timeout and no second tap is needed.
Opening the page alone does not start a recording.

**STOP** or **Back** cancels a pending start. Mode changes and log uploads are
unavailable while waiting or recording. The enlarged **WI-FI** and **BLE**
counters show received observations from the participating radios; the valid
and saved row counts below them describe what reaches the log.

The service checks the same condition before allocating recording resources,
after radio preparation and before each receiver starts. The receiver must be
running with a valid monotonic clock, fresh coordinates and fresh RMC UTC;
neither sample may be older than five seconds. The upload clock's time holdover
does not qualify as a position fix.

If the fix disappears during capture, the receivers continue scanning. Rows
without qualifying position/time are excluded from the CSV and counted on the
screen. Logging resumes when the fix is usable again. A fix does not establish
survey-grade position accuracy; unknown accuracy remains blank in the file.

## One CSV, up to two observations per identity

After **STOP**, Cerberus drains and releases the receivers, then keeps the
strongest and weakest valid RSSI observations per identity in the session:

- Wi-Fi uses the BSSID. Access points sharing an SSID remain separate.
- Bluetooth uses the advertised address. Both receivers contribute to the
  same address; Wi-Fi and Bluetooth addresses are separate namespaces.
- Equal RSSI extrema keep one earliest observation. A single sighting stays
  as one row. Ties at either extreme keep the earliest matching observation.
- A retained row keeps its original timestamp, coordinates, channel, name and
  signal level. No averaged, substituted or invented location is written.
- Random Bluetooth addresses are not resolved to a person or physical device.
  One device changing its address can still have several entries.

For example, readings of -70, -50, -90, -40 and -60 dBm yield the original
-90 and -40 dBm rows. The final file remains `CERBWardrive###.CSV` in
`/CERBERUS/WARD`, with the existing WiGLE 1.6 headers and fields. This matches
the column layout used by the [official WiGLE uploader](https://github.com/wiglenet/wigle-wifi-wardriving/blob/main/wiglewifiwardriving/src/main/java/net/wigle/wigleandroid/background/ObservationUploader.java).
It is not a promise of service acceptance or contribution credit.

This policy reduces upload size, but deliberately discards intermediate
measurements along a route. Repeated sightings are not inherently invalid.
The filtering is per recording, not across files or past uploads.

## Capture and storage behavior

RF callbacks and scan schedules are unchanged. During capture, the existing
writer appends valid observations to the recovery journal. Compaction runs
after RF stops, using a fixed 10,352-byte workspace and an external merge of
16-byte row references. Its memory use does not grow with the number of
identities. This adds work to **STOP / SAVING**, not to RF callbacks.

The screen distinguishes valid observations written to the journal from rows
in the finished CSV. **SAVING RSSI EXTREMES** means the receivers have stopped
and the log is being finalized. Wait for **STOPPED** before removing the card.
The console reports `csv_rows`, `csv_devices`, `compacted_rows` and `compacting`.

The `.TMP` journal is synced before compaction. A fresh `.NEW` file is synced
and closed before publication; only then is the journal removed. Read, write,
allocation, sync, close or rename failures leave the journal available for
recovery and do not publish an incomplete CSV. Interrupted `.TMP`, `.D0`,
`.D1` and `.NEW` files are not upload candidates. Power loss can still lose
the unsynced tail of a recording. Automatic recovery is not implemented.

Compaction needs free SD space for the output plus two row-index scratch files.
It does not reduce capture-time SD writes or extend the existing 64 MiB raw
recording limit. A full card can prevent finalization while preserving the
source journal.

## Validation

Focused production-code checks cover GPS-off/no-fix/stale-fix/missing-UTC
service admission, wait/autostart/cancel routes, GPS preview ownership, a fix
lost between the two receiver starts, Stop/Back cleanup,
CSV quoting and UTF-8, equal RSSI ties, shared names and mixed radio identities.
Independent Python CSV checks compare complete retained rows against the
production C compactor, including a 100,000-row sample with more identities
than fit in its working memory. Real-file tests inject allocation, read,
write, seek, flush, sync, close and publication failures.

The 100,000-row host sample retained 36,248 rows from 19,189 identities. Host
timing is not a measurement of Cerberus SD performance. Physical GPS gating,
Stop latency and a new compacted recording/upload still require device checks.
