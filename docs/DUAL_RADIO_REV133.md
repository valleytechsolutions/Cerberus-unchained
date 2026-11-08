# Rev133: selected AP/client Packet Count

Rev133 implements passive selected-address counting on the onboard S3 and C5.
Source `9ff38e33ad7e478a48789128f394ec274e179033` passes all 157 registered host
suites and five native targets on 1,230 unchanged inputs. The linked resource
audit and matched development package also pass. The paired rev133 development
package is installed with verified writes and sampled application boot checks.
No GPS or expansion board is required.

These results do not establish physical receive accuracy. The owner's confirmed
rev131 Mesh menu-first navigation and Back remain the accepted UI scope;
rev132 Mesh Save/Open, real Mesh reception and Locate acceptance remain pending.

## Using Packet Count

Open a frozen Wi-Fi AP detail and choose **PACKET COUNT**, or open an observed
client detail and choose **COUNT THIS STATION**. Opening either route copies
that selection without starting reception. A separate passive list holds up to
16 AP/client entries; archive save checkboxes and action targets retain their
own meanings. **ADD AP** returns to the AP picker. Role plus complete MAC address
identifies an entry. Its initial channel is a planning hint, not a match filter.

Choose **SELECTED CHANNELS**, **FIXED CHANNEL** or **ALL CHANNELS**. Leave
recording off for count-only operation or explicitly enable it, then press Start.
The canonical plan contains 42 channels, including 169, 173 and 177. Actual SDK
and regulatory acceptance is checked when tuning; a requested channel is not
proof of reception. Receiver status, errors and coverage remain distinct.

A manual channel/plan change drains and validates the previous segment before
starting the next one; prior totals accumulate once. The target list stays
fixed after acquisition begins. **NEW LIST** is available only after both
sources and any recording job finish cleanup. Stop, Back, sleep and leaving the
page retain ownership through receiver and file cleanup. An allocation error
leaves the originating AP/client view intact and reports the failure there.

## What is counted

Both receivers use the same original, bounded management/data header parser.
It checks delivered length, SDK frame class, DS bits and complete applicable
four-address, QoS and HT headers. Payload remains opaque. Unsupported forms and
control frames do not become counted data.

Each target independently counts an immediate transmitter/receiver MAC match
at most once per callback. An AP match does not consume a selected client's
count. A separate union total counts the callback once when any target matches.
BSSID context is counted separately from direct participation. Transmitted and
addressed subsets can overlap, and different targets' totals cannot be added
as unique frames.

RSSI updates only when the target transmitted the frame. Addressed-to traffic
does not refresh its signal. Last participation and transmitter signal have
separate source times. Retry, fragment and protected-frame counts remain
subsets, with explicit saturation and invalid-clock handling. The totals are
delivered SDK observations, not packets per second, deduplicated physical
packets, authenticated devices or proof of association. Unknown SDK/RF loss
cannot be inferred from software counters.

## Work shared between S3 and C5

Multi-channel plans assign disjoint channel sets. S3 receives selected 2.4 GHz
channels; C5 handles selected 5 GHz channels and any remaining 2.4 GHz assignment.
A two-channel 2.4 GHz plan can put one channel on each receiver. The full plan
puts channels 1–14 on S3 and the remaining 28 on C5. A single fixed channel uses
C5 reception while S3 handles control, validation, presentation and storage.

Only a definite, clean S3 admission rejection permits C5 fallback. Held or
uncertain ownership cannot be bypassed, and a running source is not silently
reassigned. Full session identities, clocks and software receive windows remain
separate. These windows describe accepted-callback periods, not continuous RF
coverage. Live projections are independently sampled; stopped totals require
final source validation, recording drain and exact release.

## Optional matched-packet recording

Count-only operation creates no raw packet queue or recording worker. Explicit
recording adds a two-frame queue per active receiver, each with a 4,096-byte
frame limit, plus one optional S3 recording worker. C5 queues use internal
memory; the S3 queue and recording workspace use PSRAM. Queue overflow,
oversized frames, transport faults and SD failures remain separate from receive
counts. A saved-packet count cannot be inferred from a target's observations.

The recorder uses exact sessions, a separate raw-metadata pin, part identities,
CRC32 and acknowledged FIFO retirement. A lost ACK cannot append the same
record twice. C5 raw reads carry 40 data bytes in the existing 64-byte SPI
transaction capacity; controls and metrics remain 32-byte transactions. Empty
queues back off between polls. This is a transfer-format improvement, not a
measured RF throughput or latency claim.

An exact storage lease creates `/CERBERUS/PCOUNT/Pnnnnn/`, with separately named,
lazily opened `S3.PCAPNG` and `C5.PCAPNG` files. Untimestamped Simple Packet Blocks
avoid invented UTC or synchronized receiver times. Files begin as exclusive
`.TMP` entries. Checked writes, flush, sync and close precede publication;
failed partial files remain visible for inspection. Active storage ownership
blocks conflicting streams, unmount and USB handoff. Power-loss durability is
not established by these checks.

## Compiled resource measurements

The native audit compares final rev133 images with rev132. Linked fixed
data/BSS grows by **224 bytes on S3** and **64 bytes on C5**. Optional allocations:

| Object | Bytes | Placement |
| --- | ---: | --- |
| S3 count context | 2,112 | Internal memory |
| C5 count context | 2,152 | Internal memory |
| UI, including its 13,080-byte controller | 14,144 | S3 PSRAM |
| Recording ring, per receiver | 8,312 | S3 PSRAM / C5 internal |
| Recording worker context | 8,424 | S3 PSRAM |
| Storage context | 184 | S3 PSRAM |

C5 count plus recording requests 10,464 internal bytes. The shown S3 UI, ring,
recording and storage objects overlap at 31,064 PSRAM bytes, excluding the
separate internal receive context. S3 receive and recording workers have
configured stacks of 4,096 and 6,144 bytes respectively. C5 reuses its existing
4,096-byte Wi-Fi task. Stack allocations are separate from the object sizes;
existing archive allocation arms are unchanged.

These are linked object and selected function-frame measurements, excluding
allocator, SDK/VFS and unrelated retained-store costs. They do not establish
full call-chain stack use, loaded memory headroom or runtime performance.

## Validation and matched package

All **157 host suites**, **five native targets** and **1,230 unchanged inputs**
pass. Production-bound fixtures cover parser/model, both producers, transport,
controller, UI, recorder and filesystem behavior, including clock faults,
uncertain commands, failed cleanup, retunes, queue pressure and SD failures.
The matched package has three onboard targets and nine images; 22 checksum
entries verify. Software checks do not prove physical reception or UI behavior.

- Source: `9ff38e33ad7e478a48789128f394ec274e179033`.
- Package: `cerberus-unchained-0.1.0-dev.rev133-20261102T235911Z`.
- Manifest SHA256: `b8dd80effb7baea89e4f3695e7238f276ac36d51057e381eeacc0115d91bfb8d`.
- Local milestone SHA256: `5e259b05406f58ae3cacb2e77fa7287b6c48785cc46cf32b168b61d2edb935f5`.
- Resource audit SHA256: `ef0b9fcebe6c9f4a901705df703f047c6efbb65af928efd383c15a7617d811ff`.

## Installation and remaining acceptance

**The paired rev133 development package is installed.** Fresh, verified rev132
backups cover the complete erase-rounded write extents: 2,179,072 S3 bytes and
2,097,152 C5 bytes. The three bridge, three C5 and three final S3 image writes
were verified. An initial active Mesh session prevented admission; the owner
stopped it and returned Home before the successful idle check and any writes.

The separate installation receipt has SHA256
`531177d2c84ce1cef150c5465ee0495910f153f999adbb70faf8e87963af96f6`.
The application reports rev133 and C5 capability 74, a buffered display and
mounted SD. Two sampled idle captures 62.87 seconds apart show advancing S3/C5
uptime and SPI exchanges, unchanged C5 boot count and no previous reset fault.
The four startup READY timeouts and four legacy fallbacks remain unchanged;
the observed interval has no new listed SPI errors, not a zero-error history.

Settled S3 internal free memory is 137,184 bytes, with a largest free block of
94,208 bytes and 1,427,552 free PSRAM bytes. The sampled C5 free memory is
53,572 bytes. These are idle measurements before Packet Count, not loaded
headroom or sustained-performance acceptance. No PR or public release was
created for this checkpoint.

Physical AP/client count accuracy, split coverage, real SD packet contents,
recording/retune/Stop behavior, repeated Back/rescan and sustained operation
remain acceptance work. Counter CSV export and Marauder's serial capture
transport are unimplemented. Full Mesh and Marauder parity remain unfinished.

The reference is pinned Marauder develop
[`79cc6bcba1db6679baee1dab2b0c1ad5010e6363`](https://github.com/justcallmekoko/ESP32Marauder/commit/79cc6bcba1db6679baee1dab2b0c1ad5010e6363).
Credit JustCallMeKoko and the ESP32 Marauder contributors for the Packet Count
workflow. Cerberus's implementation is original; see
[source attribution](MARAUDER_ATTRIBUTION.md) and the
[Packet Count contract](WIFI_PACKET_COUNT_PLAN.md).
