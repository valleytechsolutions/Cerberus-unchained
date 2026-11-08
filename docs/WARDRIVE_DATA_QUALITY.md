# Wardriving GPS admission and RSSI extrema

RSSI extrema filtering was introduced in rev148. Rev149 simplifies the GPS
start control. Rev151 reduces capture-time SD writes; rev155 reduces the
scratch-index work needed to save the final RSSI extremes after Stop. Both
preserve the same final rows. These changes affect new recordings; existing CSVs and
upload receipts are left intact.
Rev156 adds [private journal segments](WARDRIVE_LONG_SESSIONS.md) so the first
64 MiB of journal data no longer ends a recording. Stop still publishes one CSV.

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
counters show received observations from the participating radios. Below them,
**SD ROWS** counts accepted journal appends; **FILTERED** counts valid readings
already between the strongest and weakest saved readings for their identity.

The service checks the same condition before allocating recording resources,
after radio preparation and before each receiver starts. The receiver must be
running with a valid monotonic clock, fresh coordinates and fresh RMC UTC;
neither sample may be older than five seconds. The upload clock's time holdover
does not qualify as a position fix.

Rev159 also checks the receiver's fix class. Estimated, simulated and manually
supplied positions do not qualify. A void RMC and valid GGA for the same recent
epoch cannot combine into FIX + UTC, regardless of their arrival order.
A later qualifying satellite fix restores logging. The LC29H's separate,
unsupported navigation-status field remains compatible; it is not confused
with RMC's position-status field. See [qualification rules and checks](GPS_AND_RADIO_RECOVERY.md).

With geofences disabled, if the fix disappears during capture, the receivers continue scanning. Rows
without qualifying position/time are excluded from the CSV and counted on the
screen. Logging resumes when the fix is usable again. The optional
[geofence workflow](WARDRIVE_GEOFENCES.md) pauses both receivers when a qualifying
position is unavailable. A fix does not establish
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

RF callbacks, receive queues and scan schedules are unchanged. The S3 writer
uses an optional 4,736-byte PSRAM cache to remember accepted RSSI extremes for
up to 512 identities. It checks four entries per lookup, after GPS, RF-time
and CSV validation. The first valid sighting and any new extreme reach the
journal. A reading between previously appended extremes needs no SD write.
Only a successful append updates the cache; a failed write cannot suppress
a later valid reading. A full cache replaces an entry within its group.
Evictions cause additional writes, never loss of a new extreme. If allocation
fails, capture uses the original append-every-valid-row path without taking
memory from the internal radio heap.

Final compaction runs after RF stops, using a fixed 10,352-byte workspace
and an external merge of 16-byte row references. Rev155 reduces each sorted
512-row block to its RSSI extremes, then reduces again while merging blocks.
Discarding an intermediate reference at either stage cannot discard a global
extreme: each block retains its earliest strongest and weakest observation.
Counted runs allow unequal block lengths without keeping duplicate references
through every merge pass. A 64-reference output buffer fits within the existing
sort workspace. It handles evicted identities and changing extrema over the
entire recording, preserving the same earliest ties and complete original rows.
No sorting or file I/O runs in RF callbacks.
Cache memory is released with the recording workspace after finalization.

The screen distinguishes valid observations written to the journal from rows
in the finished CSV. **SAVING RSSI EXTREMES** means the receivers have stopped
and the log is being finalized. Wait for **STOPPED** before removing the card.
The console reports `csv_rows`, `csv_devices`, `compacted_rows` and `compacting`.
`rssi_filtered` counts avoided journal appends; `rssi_filter_enabled` says
whether that session obtained the cache. These are separate from GPS/format
exclusions and RF queue drops. `sd_rows` and the per-radio logged counters
count actual appends, not all valid observations.

Rev155 adds `compact_save_ms`, `compact_passes`, `compact_index_read_bytes` and
`compact_index_write_bytes` to the console. The duration includes stream close,
storage-lock retries, compaction and publication for the last located log save.
Index counters measure successful transfers through C stdio, including run
headers; they are not physical SD block counts or total recording I/O. They
exclude source CSV reads and output CSV writes. The next session resets them.

The `.TMP` journal and any `.TMP.P#####` continuations are synced before
compaction. A fresh `.NEW` file is synced and closed before publication;
only then are the journals removed. Read, write,
allocation, sync, close or rename failures leave the journal available for
recovery and do not publish an incomplete CSV. Interrupted `.TMP`, `.TMP.P#####`, `.D0`,
`.D1` and `.NEW` files are not upload candidates. Power loss can still lose
the unsynced tail of a recording. Automatic recovery is not implemented.

Compaction needs free SD space for the output plus two row-index scratch files.
The capture filter can reduce journal and sorting work when identities repeat;
savings depend on signal changes, population size and cache churn. Each private
journal is limited to 64 MiB; capture can continue into further segments.
The final uploadable CSV retains its existing 64 MiB validation limit. A full
card or an oversized final result can prevent finalization while preserving
the source journal. Power-loss recovery contains accepted candidate extrema,
not every intermediate valid observation.

## Validation

Focused production-code checks cover GPS-off/no-fix/stale-fix/missing-UTC
service admission, wait/autostart/cancel routes, GPS preview ownership, a fix
lost between the two receiver starts, Stop/Back cleanup,
CSV quoting and UTF-8, equal RSSI ties, shared names and mixed radio identities.
Independent Python CSV checks compare complete retained rows against the
production C compactor, including a 100,000-row sample with more identities
than fit in its working memory. Filtered and allocation-fallback pipelines
must produce byte-for-byte identical final CSVs to the original pipeline.
Cases cover monotonic RSSI, ties, mixed Wi-Fi/BLE addresses, changed names and
channels, cache evictions, invalid GPS/time/format and failed append decisions.
Real-file tests inject allocation, read, write, seek, flush, sync, close and
publication failures. A 1,025-row case exercises every explicit write (161),
read (160) and seek (168) failure position during finalization, including short
writes and run-header backpatches. All 489 failures must preserve the original
journal bytes, remove incomplete scratch files and publish no CSV.

An optional comparison builds the rev154 compactor beside the current one:

```text
python tools/check_wardrive_compact.py --baseline-source /path/to/rev154_compact.c --extended
```

The baseline source comes from `f288fc3f96d70925c4c84f6dd7a725d71bf402a6`.
Both versions produced byte-identical complete CSVs in all valid cases, and the
independent Python oracle also checked identity ordering, ties and every retained
row. The following synthetic workloads measure summed scratch-index reads and
writes; the existing 10,352-byte workspace is unchanged.

| Input rows / identity distribution | rev154 index bytes | rev155 index bytes | Retained CSV rows |
| --- | ---: | ---: | ---: |
| 100,000 / 64 repeated identities | 28,800,000 | 1,619,956 | 128 |
| 100,000 / 19,189 mixed identities | 28,800,000 | 23,980,052 | 36,248 |
| 100,000 / all unique | 28,800,000 | 28,803,956 | 100,000 |
| 500,000 / 128 repeated identities | 176,000,000 | 16,059,524 | 256 |

With no repeats, run headers add 3,956 bytes in this sample (about 0.014%).
Savings depend on the observations remaining after the capture-time cache;
these synthetic percentages must not be added to the cache's savings or read
as SD throughput measurements. Rev155 still needs physical Stop timing and
final-CSV readback on the device.

The 100,000-row host sample retained 36,248 rows from 19,189 identities. Host
timing is not a measurement of Cerberus SD performance. On rev149 the user
confirmed GPS waiting, Stop cancellation and the revised layout. A later
read-only console capture showed a live located recording with over 75,000
valid observations and no reported service error. That sampled RUNNING state
does not establish the exact automatic-start transition, finalization latency,
or correctness of the new compacted file; those checks remain open.
The rev151 filter also needs physical capture, final-CSV readback and Stop
latency checks. Host savings do not establish device throughput or endurance.
