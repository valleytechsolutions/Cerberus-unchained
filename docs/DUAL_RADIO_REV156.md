# Rev156: longer Wardriving sessions and cheaper CSV finalization

Rev156 is installed on the development Cerberus, paired with C5 capability 79
and protocol 2. This is a local checkpoint, not a new published release or a
claim of complete Marauder parity.

The preceding rev149 recording reached the 64 MiB journal limit after 712,083
accepted rows. It stopped with `ESP_ERR_INVALID_SIZE`, then finished saving
one CSV with 1,118 retained rows across 608 address/type identities. The save
was allowed to finish before the update. Those are firmware counters; the
file's contents still need host readback.

## What changed

- Located recordings continue across private journal segments instead of
  stopping at the first 64 MiB boundary. Stop merges the whole session into
  one `CERBWardrive###.CSV`; the extra journals never appear as upload files.
- The writer's optional PSRAM cache avoids redundant journal appends. The
  final merge retains the earliest strongest and weakest readings per
  identity across every segment, with their original position and UTC.
- Each external-sort pass removes intermediate duplicate references. The
  compaction workspace remains 10,352 bytes. A 500,000-row host comparison
  reduced logical scratch-index traffic from 176,000,000 to 16,059,524 bytes
  while producing byte-identical CSV output. This is not an SD timing result.
- The matched pair includes the Wardriving/Packet Monitor command-routing
  correction. It also includes the optional GPS track/POI and geofence work;
  their physical feature checks remain pending.

The GPS start flow is unchanged: tap START once, wait for a qualifying fix
and UTC if necessary, then record automatically. STOP or Back cancels the
wait. The owner previously confirmed wait/cancel and the larger counter layout.

## Validation and installation

Installed source: `3586bde6a73ce4ad1daec49c68515fc4f3ee8274`.
Source tag: `checkpoint/rev156-wardrive-journals`.
Package: `cerberus-unchained-0.1.0-dev.rev156-20261108T043555Z`.
Manifest SHA-256:
`0dfbbf55cddbd8416e226e801952cb1fe8ec2a0ba833673321de351efee74d7f`.

All 170 host suites and five native targets passed against unchanged inputs.
A real-file test accepted 645,278 rows, crossed the actual 64 MiB boundary,
and published one CSV containing extrema from both journals. Multi-segment
tests also cover equal file offsets, ties, block boundaries, inconsistent
headers, file collisions, failed cleanup and injected I/O failures.

Both chips were backed up and their baseline flash digests verified before
programming. All nine bridge/C5/S3 image writes passed verification. Paired
boot and a second idle capture about 44 seconds later confirmed mounted SD,
released radio owners, advancing uptimes and no additional listed SPI errors.
Four initial READY timeouts/fallbacks were already present at the first capture;
neither count increased between the captures. No prior-reset fault was reported.

The saved upload profile loads after the update. A private comparison against
the verified pre-update backup matched the supplied profile fields. No upload
or authentication request was made during this checkpoint. The read-only log
catalog lists three completed CSVs, including `CERBWardrive005.CSV`, with no
catalog or lease-release error. Existing temporary recovery files were retained.

## Remaining checks

Physical journal rollover, final CSV readback, Stop/save latency, runtime
heap/stack headroom, moving GPS capture and sustained RF performance remain
open. The new counters make save time and index traffic measurable on the
device; host results do not establish field performance. Optional GPX,
geofence boundaries, expansion hardware and ATGM336H also need acceptance.

The final CSV still has a 64 MiB size bound, and enough card space is needed
for the journals, scratch indexes and output. Failed finalization preserves
the journals. Automatic interrupted-session recovery is not implemented.

See [data quality and filtering](WARDRIVE_DATA_QUALITY.md),
[journal storage](WARDRIVE_LONG_SESSIONS.md),
[geofences](WARDRIVE_GEOFENCES.md) and the
[coverage ledger](MARAUDER_COVERAGE_LEDGER.md) for the remaining scope.
