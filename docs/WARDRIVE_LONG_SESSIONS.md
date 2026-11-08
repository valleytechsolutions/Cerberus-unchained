# Wardriving across journal boundaries

Rev156 removes the first 64 MiB journal boundary as a normal reason to stop
located Wardriving. The S3 writer continues into private journal segments while
the S3 and C5 radio workers keep their existing scan schedules. Stop merges the
whole session into one `CERBWardrive###.CSV`. Logs / Upload still shows one
completed recording, with the same WiGLE headers and per-identity RSSI extrema.

The failure was observed on the installed rev149 device: after 712,083 accepted
journal rows, it entered STOPPING/compaction with `ESP_ERR_INVALID_SIZE` (260).
The source rejects the next row when its single journal reaches 64 MiB. The
storage diagnostic timed out while finalization held the storage mutex; that
observation alone does not mean the card was unmounted. The development fix
does not modify or interrupt that recording.

## Capture and save

The first journal keeps the existing `.TMP` name. Continuations use
`.TMP.P00001`, `.TMP.P00002`, and so on. They are internal recovery files, not
upload candidates or separate recordings. There is no new menu or setting.

At a boundary, the writer opens the next file exclusively, copies the original
header, syncs it, then syncs/closes the previous file before accepting the next
row. It does not sort or change radio state during capture. Observations can
still be dropped if an SD operation blocks longer than the existing bounded
receive queues can absorb; this change does not claim lossless RF reception.

After Stop, the existing external sort reads the journals in capture order.
Its 16-byte references now identify a segment and an offset within it. Equal
offsets in different files remain distinct observations. Each segment must
pass CSV validation and have exactly the same two header lines. The merge
retains the earliest strongest and weakest observation for each address/type
across the whole session, including its original coordinates and timestamp.

The 10,352-byte compaction workspace is unchanged. A two-handle source cache
limits file reopen work when extrema come from different segments. Along with
the two scratch indexes, output, optional GPX and legacy stream, it stays below
the mount's existing eight-file limit. The new source handle adds normal stdio
buffer overhead while saving; runtime heap/stack and SD latency need physical
measurement. No new task or radio buffer is added.

The output is synced and closed before its single CSV name is published.
Only then are continuation files removed, followed by the base journal. If a
continuation cannot be removed, the base stays too, reserving that file family
even if the user subsequently deletes its completed CSV. Existing continuation
files are never overwritten. A failed rollover, short write, close, read, seek,
sync or publication keeps the recovery journals and does not publish a partial
CSV. Automatic recovery of interrupted sessions remains unimplemented.

## Bounds and verification

Each segment remains at most 64 MiB. The implementation supports up to 65,535
segments and rejects row-count or signed scratch-offset overflow. The final
uploadable CSV still has the existing 64 MiB limit; if retained unique/extrema
rows exceed it, publication fails and the journals remain. Adequate free card
space is still required for journals, scratch indexes and output. This removes
the observed raw-journal stop, not all possible storage limits.

`check_wardrive_compact.py` compares single-file and multi-file results with an
independent CSV oracle. Five additional cases cover equal offsets in separate
files, ties across segments, 17 segments around sort-block boundaries, mismatched
headers and empty segments. Original input bytes must remain unchanged.

`check_wardrive_storage.py` executes the production append/rollover/compaction
code using real files. A 645,278-row case crosses the actual 64 MiB boundary,
continues accepting rows and publishes one two-row CSV containing extrema from
both journals. It verifies cleanup and exercises every explicit I/O failure
position in single- and three-journal finalization and rollover, including
short writes. Existing-file collisions, failed continuation cleanup, filename
reservation, USB leases and GPX stream checks are also covered.

These are source and host results. Installation, physical boundary crossing,
Stop latency, CSV readback, concurrent GPS/GPX and prolonged field performance
remain pending. Rev152's frozen installation package is retained separately;
rev156 is a development checkpoint and does not establish full Marauder parity.
