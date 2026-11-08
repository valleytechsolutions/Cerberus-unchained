# Rev117: exact S3 Fox Hunt ownership

Rev117 is a local receive-side Fox Hunt candidate following
[rev116's Analyzer binding](DUAL_RADIO_REV116.md). Rev113/C5 firmware 68 remains
installed. Full Marauder parity and physical acceptance remain unfinished.

## Exact tracking ownership

Fox Hunt now uses S3 tracking-specific attachment, start, state, stop and
release operations. An accepted sweep binds the full callback epoch and BLE
host epoch under the existing scanner mutex. The public eight-bit generation
and generic SCANNER owner label do not establish tracking ownership.
Generic scanner Start and Stop cannot take over an attached tracker. Only
callbacks from its accepted, actually receiving sweep update target history.
The existing exact address plus address-type identity and RSSI validity rules
are preserved.

A host reset or replacement invalidates the attachment and clears its signal
history. It requires explicit release and a fresh attachment before retrying.
The renderer and diagnostic publisher validate this native binding and target
again when reading history, so a reset before the next UI tick cannot expose
the old signal as current. A failed attachment or first start without an
accepted sweep displays unavailable counts. A queued Start displays WAITING
FOR RADIO; LISTENING requires actual SDK reception. Valid retained history
between sweeps stays visibly separate
from live reception.

S3 and C5 remain independent receivers. RETRY S3 leaves a healthy C5 tracking
job and history running, and the common transition barrier waits for the
previous workflow before starting either source. Stop, Back, sleep and hidden
page cleanup retain exact ownership until release. A 12-second warning does
not discard a held attachment or permit it to restart. Hidden cleanup cancels
only the matching C5 tracking request and cannot navigate away from a newer
page. Pending C5 INFO cancellation does not add an immediate SPI exchange.

No new task, discovery bank, allocation, radio mode or wire command is added.
C5 capability revision remains 68. Both receivers retain their own measurements;
their counts and RSSI are not combined into a fictitious unique-device count or
distance estimate.

## Validation

The actual scanner fixture adds 99 tracking-binding assertions in each legacy
and extended receive configuration, plus canonical-output checks for a
BLE-disabled build. Cases cover attachment admission, queued Start cancellation,
SDK startup failure, repeated passive sweeps, held cleanup, failed host release,
stale callbacks, public-generation aliasing, full-epoch replacement, token
exhaustion and host reset. A foreign scanner or advertiser cannot contribute
tracking history or be stopped by the stale binding. Existing Analyzer,
discovery, MAC and callback checks remain in the suite.

The UI tests compile production Fox lifecycle and renderer functions and the
actual C5 coordinator with a framed peer simulation. Their S3 boundary models
the exact tracking contract; the scanner tests separately exercise the actual
mutex and SDK-event implementation. Diagnostics and rendering also check
queued versus receiving states, unavailable data, retained between-sweep
history and replacement before the next UI tick. The diagnostic snapshot
remains 120 bytes and the maximum tested console chunk remains 415 bytes.
Synthetic previews are host-rendered fixtures, not device photographs.

The composite fixture passes 387 Fox lifecycle/UI assertions, including
independent source failures and retries, retained ownership past the timeout,
hidden cleanup beside another C5 workflow and failed attachment beside an
existing tracker. Analyzer's 550 assertions remain, with 1,890 separate live
discovery, monitor and navigation assertions also passing. These totals
describe host fixtures, not over-the-air observations.

All 124 host suites pass on unchanged inputs. Their retained receipt binds
every log by hash. All five native targets also pass: S3 application, onboard
C5, temporary S3/C5 bridge, expansion WROOM and expansion XIAO. The native input
map matches the retained host receipt and current source bytes.

## Native resources and package

The native comparison uses the verified local rev116 candidate as its
baseline. Rev116 was not installed on the device.

| Native measurement | Local rev116 | Local rev117 |
| --- | ---: | ---: |
| S3 fixed data and BSS | 173,628 bytes | 173,660 bytes |
| C5 fixed data and BSS | 83,160 bytes | 83,160 bytes |
| S3 tracker / signal snapshot types | 20 / 20 bytes | 20 / 20 bytes |
| Exact S3 tracking state | Absent | 28 bytes |
| Shared discovery store | 21,448 bytes | 21,448 bytes |
| Dual-BLE coordinator | 21,496 bytes | 21,496 bytes |
| Archive viewer plus worker type demand | 118,760 bytes | 118,760 bytes |

The fixed S3 increase is 32 bytes. Existing optional discovery, Analyzer and
archive allocations do not grow. Type sizes and linked sections are distinct
from runtime heap, allocator overhead and peak concurrent demand. The new
tracking state has no rev116 counterpart; absence is not a zero-byte baseline.

The selected Fox tick entry frame grows from 48 to 80 bytes, Fox rendering from
304 to 336 and diagnostic publishing from 224 to 256. The shared scanner's
Analyzer Start entry grows from 64 to 80 and SDK start callback from 48 to 64.
The UI entry remains 1,072 bytes, BLE tap 192 and generic tap 544. Inlined or
absent helpers are not assigned zero cost. These entries are not a complete
call-chain bound or evidence of runtime stack headroom. The separate native
console stack check passes against the final S3 ELF.

Final S3/C5/bridge application sizes are 1,943,760 / 1,678,960 / 255,296 bytes.
The WROOM and XIAO expansion applications are 926,400 and 930,624 bytes.
The audit is retained at `artifacts/device-rev117/native-memory-comparison.json`
with SHA-256
`9f12b54077037b749e96a1eed1e085003bb136a56ca147e007e762816a7da9a1`.
Host and native evidence is retained under `artifacts/rev117-host-evidence`
and `artifacts/rev117-native-evidence`.

The matched package is
`dist/cerberus-unchained-0.1.0-dev.rev117-20261101T164038Z`, with manifest
SHA-256 `44f5e688fdfe8c599ddcb4c8f47e448c520a9f1143e8e0e171b5cc9351ae0174`.
All three application hashes match the retained full matrix. The read-only
verifier passes image hashes, chip headers, partition tables and image bounds.
The local milestone receipt binds the committed source, matched package,
host/native receipts and native audit. No image was flashed and no release or
pull request was published. Rev113 remains installed.

## Coverage and remaining work

The coverage ledger is refreshed to `rev117-source`, including the local
rev114 Find My Monitor, rev115 ordinary discovery and rev116 Analyzer work.
It retains all 95 upstream modes: 74 partial, nine needing review, four
hardware-limited and eight pending. None is marked complete. SHOW_INFO moves
to partial based on existing dashboard, System health, SD and console source;
this revision adds no device-information page.

A read-only upstream check at 2026-11-01 16:21:17 UTC found Marauder develop
unchanged at `79cc6bcba1db6679baee1dab2b0c1ad5010e6363`.
[The remaining-work audit](RECEIVE_REMAINING_GAPS.md) records the unresolved
Recon admission boundary, Wardriving ownership and source-error policy,
GPS validity/presentation, BLE persistence and device-information gaps.
These are recorded follow-up work, not fixes claimed by this milestone.

Concurrent physical reception, touchscreen retry/Stop/Back, dense environments
and sustained heap/stack behavior still need device acceptance. GPS and the
six-radio backboard remain separate hardware work. This local candidate does
not change the installed rev113 baseline or establish full Marauder parity.
