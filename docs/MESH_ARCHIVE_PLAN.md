# Mesh saved observation maps

Implemented in the installed rev132 development pair, with host/native/package
and sampled boot evidence recorded in [the checkpoint](DUAL_RADIO_REV132.md).
This plan is not a complete parity or hardware-acceptance claim. The owner has
accepted rev131's menu-first entry and Back controls; reception and SD behavior
still require their own checks.

The workflow reopens a saved Mesh survey as browsable PAN contexts, addresses,
directed traffic and retained raw evidence. The existing CSV is an external
report, not an importable map. The versioned `.MSH` document preserves
complete retained aggregates, final coverage/loss counters and the
first 64 raw witnesses. Replaying those witnesses cannot reconstruct later
aggregate updates, so the archive must store both explicitly.

## Boundaries

- C5 retains its existing passive receive and exact Stop/drain/release path.
  No additional C5 task, radio owner or wire protocol is needed for persistence.
- S3 copies only a validated frozen survey into a separate historical document.
  The document contains observations and final source metadata, never commands,
  live lease state or permission to resume a radio operation.
- The portable codec uses explicit little-endian fields, versioned layout,
  bounded counts and lengths, reserved-byte validation and a corruption CRC.
  The checksum does not authenticate the source or prevent deliberate editing.
- The existing serialized SD worker owns encoding, loading and publication.
  Optional PSRAM allocations match the operation; old formats retain their
  existing allocation sizes. Decode scratch is caller-owned, not a large stack
  local. Failed loads must not replace a previously displayed document.
- Save creates a new `/CERBERUS/MESH/Mxxxxx.MSH` through an exclusive temporary
  file. It preserves existing files and reports partial-write failures.
  Byte bounds do not guarantee storage latency or power-loss durability.
- The historical viewer exposes the same observation categories and raw
  inspection, with explicit historical status and no Start or Locate action.
  CSV export remains available from the saved document. Back returns to Files.

## Required evidence

Use independent byte-layout checks and round trips with typed 16/64-bit
addresses, unknown versus known PAN zero, records beyond witness retention,
source/destination distinctions, raw omissions, saturation, unknown clocks and
source errors. Reject truncation, excess lengths/counts, reserved bits, invalid
references, checksum failures and overlapping buffers without publishing a
partial document.

Exercise actual asynchronous worker cancel/close/result pinning and storage
short-write, flush/sync/close/rename failures. UI checks must cover Files entry,
stable selection, privacy, load failure, CSV export, Back and absence of live
actions. Run the registered host matrix, native builds and resource checks
before packaging. Real SD save/reopen, repeated navigation and warm radio
handoffs remain distinct physical acceptance requirements.
