# Wardriving Recent Networks

This guide describes the contributed source. New workflow acceptance on physical hardware remains pending; the installed local Unified checkpoint and the unflashed Unchained adaptation are distinguished in [CONTRIBUTION_REV93.md](CONTRIBUTION_REV93.md).

The main firmware's Wardriving page now includes **RECENT NETS** beside Start
and Stop. It presents the observations already processed by the existing
onboard C5 Wi-Fi and S3 BLE workers, together with expansion observations when
an enabled backboard supplies them. Opening the viewer does not start another
scan, read the SD card, query a radio or require GPS.

This ports the original separate Wardriving firmware's Nets workflow. The
[source contract](WARDRIVE_RECENT_SOURCE_CONTRACT.md) records the inspected
original files and their hashes. It preserves a 24-observation history and
selects the ten newest distinct identities within that history. This is a
short recent-activity view, not a complete session inventory. Repeated
observations can displace other networks from the history.

The list uses four rows per page. Each row displays Wi-Fi or BLE context,
radio source, channel/band when applicable, RSSI, processing age and logging
outcome. Wi-Fi identities use BSSID across radio slots; BLE identities include
address type. Probe requests and observed station relationships remain separate
from AP/BLE network identities. Back returns to the summary without stopping
an active session. Stop remains accessible from the list and selected details,
including when the viewer cannot allocate its optional PSRAM buffer.

Selecting a row opens a frozen copy of what was painted, with its session and
observation identifiers. New observations, a new session or a changed logging
outcome cannot silently retarget that selection. The selected detail view is a
Cerberus addition to the original paged list. Address masking uses the same
privacy format as the other Wi-Fi and BLE screens.

Receipt age is measured from **S3 processing uptime**, not RF arrival, GPS time
or UTC. Missing/backward clocks display unavailable age. A row can appear even
when its logging operation fails. `APPENDED` means the append API accepted the
row; it does not prove subsequent flush, close, sync or SD durability. Duplicate
rows are identified as not appended. A failed partial append is retained as an
error and is not blindly retried by Stop cleanup.

The recent model lives in optional PSRAM and survives Stop/Error after the
active worker workspace is freed. An accepted new worker resets the history;
failed admission or task creation preserves the previous session. Allocation
failure disables only the viewer and leaves the existing logging path usable.
Each immutable Wi-Fi/BLE result bank retains a bounded per-row attempt record,
preventing cleanup from duplicating or refreshing an already processed row.
No new task, larger worker stack, C5 firmware change or internal-heap fallback
is introduced. The UI buffer is allocated only while the viewer is open.

The existing read-only `radio` diagnostic includes a compact `WARDRIVE_RECENT`
section. It reports session/revision, retained and visible counts, processing
clock faults and the service/view error separately. It does not copy the full
row bank onto the diagnostic task's stack.

Host checks exercise the production model against an independent linear-history
oracle, the actual service with counted SDK/radio/SD boundaries, the renderer,
the live UI adapter and the console formatter. They cover Stop/restart,
failed Start, allocation failure, logging errors, partial paint failures,
frozen selection, Back and pending-start cancellation. Native builds, console
stack checks and historical installation evidence are summarized in the
[contribution notes](CONTRIBUTION_REV93.md);
host tests alone do not establish physical touch or radio behavior.

Physical acceptance remains: start Wardriving without GPS or expansions, open
Recent Nets, inspect AP/BLE rows and paging, select a row while scans continue,
use Stop and Back, and start a second session. GPS and six-C5 backboard operation
remain untested without that hardware. This milestone does not establish full
Wardriving or Marauder feature parity.
