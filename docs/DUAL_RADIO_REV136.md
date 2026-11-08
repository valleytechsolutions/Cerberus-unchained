# Rev136: Packet Count recording memory checkpoint

The optional Packet Count recording worker previously allocated separate
4,096-byte raw-frame and 4,144-byte PCAPNG buffers. The existing serializer
already supports overlapping input/output: it moves the payload before writing
the PCAPNG and radiotap headers. Rev136 receives raw chunks directly into the
output buffer, validates their CRC, then encodes in place.

The source retains that scratch until its ACK completes. Consumed-sequence
marking still precedes storage append, so a lost ACK cannot serialize or append
the same frame again. The receive limit remains 4,096 bytes even though the
output buffer has additional header space. S3 and C5 still alternate after ACK;
no new task, queue, radio command or protocol capability is introduced.

The focused production-worker fixture checks complete raw payload preservation,
PCAPNG block extents, radiotap source-band metadata, padding, every short length
from 24 through 96, boundaries around each 40-byte transport chunk up to the
maximum, and 4,095/4,096-byte frames. Lost ACK, bad CRC/metadata, partial tails,
SD failure, source rebind and canceled staging cases remain covered.
The focused recorder passed 307,450 checks and the SPI adapter passed 338.
The host worker context is 4,344 bytes; native size is measured independently.

## Completed local validation

The completed validation supersedes the preparation-stage native-check note in
the editorial ledger. All 158 host suites and five native targets passed on
1,234 unchanged inputs at source commit
`59c3f4a50a00232f77841a9eb6d66704c2ee25b7`. Native READY, console-stack and C5
Locate/parser placement checks passed. The 21 shard/aggregate helper tests,
20 build/package failure-handling tests and SD-resource checks also passed.
The resource checks cover 40,187 OUI lookups, Recon policies and 100,000 malformed
expansion frames. These are software checks, not hardware acceptance.

The native S3 recording context falls from **8,424 to 4,328 bytes**, saving
**4,096 bytes of optional PSRAM** (48.6%). Fixed linked data/BSS stays at 170,124
bytes on S3 and 83,464 bytes on C5. Other compared model/context/archive sizes
and configured task stacks are unchanged; the recorder stack remains 6,144
bytes. Count-only mode never allocates this recorder. The savings therefore
apply during optional recording and do not imply an idle-heap increase or a
measured RF throughput gain. Allocator and SDK/VFS overhead are separate.

The matched candidate is
`dist/cerberus-unchained-0.1.0-dev.rev136-20261103T050655Z`. Its S3 application is
2,117,120 bytes, C5 is 1,721,872 bytes and bridge is 255,296 bytes. Package
verification passed and the packaged applications match the native matrix.
Manifest SHA-256:
`411ab61dacd059bef52c8c2f404ca7804db82bdce8595fa2270f7cec06879ac3`.
The retained local milestone receipt is
`artifacts/rev136-validation/local-milestone.json`, SHA-256
`875067ed35d809d6a001c252cf4a2139ae9197fa04858337b19df2cd93c6df12`.

Rev136 has not been installed. Documentation-only follow-up commits do not
change the exact source/configuration input map of this candidate. Hosted CI
belongs to the updated PR head and is reported separately from these local
checks.

## Installed baseline and remaining scope

Rev135 remains the installed and tested pair. Its channel-11 Count session
finished with 4,306 valid frames and 1,303 selected matches, zero metadata
rejections and normal Stop. A fresh subsequent Wi-Fi scan advanced generation
and successful C5 scans to two, returned 19 APs (10 S3, nine C5), and completed
without fallback or error after Count released its workspace. See
[the rev135 evidence](DUAL_RADIO_REV135.md).

This checkpoint updates existing Unchained PR #1 as requested, preserving
its branch history and earlier CI corrections. It does not select a public
binary release. The unfinished USB framing/transport/receiver draft is preserved
locally outside the build inputs and is not part of this contribution.

The latest read-only upstream develop comparison still resolves to
`79cc6bcba1db6679baee1dab2b0c1ad5010e6363`. Existing attribution and licenses
remain intact. No feature is promoted to complete on the basis of this memory
optimization. Full Marauder parity, USB capture, SD recording acceptance,
5 GHz Count acceptance, Mesh Save/Open/Locate reception, GPS and six-C5 expansion
validation remain separate unfinished work.
