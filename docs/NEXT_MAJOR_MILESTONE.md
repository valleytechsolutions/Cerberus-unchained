# Rev136 contribution checkpoint and resumption priorities

The owner requested one additional optimization, a saved checkpoint and a PR
update. Rev136 reuses the optional Packet Count recorder scratch, reducing its
native S3 context from 8,424 to 4,328 bytes. All 158 host suites, five native
targets, linked resource comparison and matched-package verification pass.
The current contribution updates existing Unchained PR #1; see
[the exact checkpoint](DUAL_RADIO_REV136.md).

Rev135 remains installed. Its 2.4 GHz Packet Count reception/matching, Stop,
Back and a fresh split S3/C5 Wi-Fi scan are confirmed. Rev136 has not been
flashed. Next work should preserve that known baseline and complete optional
recording and 5 GHz Count acceptance before making wider performance claims.
USB packet export remains pending; the unfinished local draft is excluded from
the checkpoint. Mesh Save/Open/Locate reception and full Marauder parity remain
unfinished. GPS and the six-C5 backboard stay deferred until hardware is present.

## Historical rev128 stopping point

The rev128 milestone and requested PR update were completed at `7248b50`, and
the goal was paused. The owner resumed the goal on 2026-11-02. The historical
stopping conditions below describe that completed checkpoint, not a new pause.

The CI correction is published at `fb37720`; its installer and full Ubuntu jobs
passed, while Windows jobs were still running at the local rev129 checkpoint.
The 802.15.4 receive-continuity fix has a verified local rev129 package: 139 host
suites and five native targets on 1,145 unchanged inputs, with unchanged linked
fixed RAM on both chips. It is not flashed or published. See
[rev129](DUAL_RADIO_REV129.md). The next feature priority is to implement
[Scan → Mesh Detection](MESH_DETECTION_PLAN.md) as requested. GPS and the six-C5
backboard are deferred. The broader Marauder feature and dual-processor
optimization objective remains active and unfinished. Hardware acceptance is
separate from source and build validation.

The owner requested one more major milestone, followed by an update to existing
PR #1 in `463N7/Cerberus-unchained`, then a pause of the unfinished broader mission.
Preserve the existing PR branch history and its remote CI fixes; do not open a
second overlapping PR or force-push the stale local contribution branch.
Do not treat completion of this checkpoint as full Marauder feature parity.

The implemented milestone is the passive Probe Sniff workflow: a dedicated
receive entry, bounded deduplicated transmitter/requested-network inventories,
stable selected details, and explicit SD evidence saving. The C5 owns passive
Wi-Fi capture; the S3 owns collection validation, aggregation, presentation and
storage. Processor roles should improve the workflow rather than duplicate
work. The existing Wardrive probe-clue path must retain its current semantics.

Requirements include exact SSID bytes (including wildcard/binary names),
strict frame/IE validation, exact receive-session and snapshot identity,
checked Stop/Back/hidden/sleep cleanup, explicit retention/drop limits, and
honest channel/time/source evidence. Observed probes are not authenticated
clients, confirmed associations or proof of physical-device identity.

The rev127 reliability checkpoint and rev128 implementation both have verified
local packages. Rev128 passes 138 host suites, five native targets and the
linked resource audit on 1,142 unchanged inputs. Its matched package and all
package checksum entries verify; see [the final checkpoint](DUAL_RADIO_REV128.md).
Rev126 remains installed. Probe reception, saving/reopening, warm repeated use
and loaded responsiveness still require physical acceptance.

Update the existing PR with the accumulated contribution and final local
validation, preserving its later remote CI fixes and upstream attribution.
Keep hosted CI and physical acceptance separate from the completed local
checks. No new image was installed during this milestone's preparation.

Once the major milestone and requested PR are complete, mark the existing goal
paused at the owner's explicit request. Leave a concrete resumption checkpoint
and stop further feature work. No additional PR or open-ended continuation is
authorized by this stopping condition.
