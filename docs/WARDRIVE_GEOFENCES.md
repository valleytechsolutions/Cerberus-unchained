# Wardriving geofences

Rev154 connects five saved areas to the native S3/C5 recording service. Inside
an enabled area, the writer closes CSV admission and pauses both onboard radios.
Once a fresh outside position and UTC are available, both radios resume in the
same recording. This is a development implementation; physical boundary and
CSV acceptance remain pending. The connected device is still on rev149.

The reference is Marauder develop
[`79cc6bc`](https://github.com/justcallmekoko/ESP32Marauder/tree/79cc6bcba1db6679baee1dab2b0c1ad5010e6363),
checked again on November 8, 2026 UTC. `Settings.cpp:779` supports five named
areas with a radius of 0.1–1.0 miles. `WiFiScan.cpp:6304` pauses scan progression
inside an enabled area; `WiFiScan.cpp:6509` evaluates the first matching area
every 500 ms. This is a radio behavior, not just a CSV row filter.

## Controls

Open **Scan → Wardriving → Geofences**. Tap one of five rows to add or edit an
area. Set its name, latitude and longitude in decimal degrees, and radius in
0.1-mile steps up to 1 mile. **Use current GPS position** fills the center from
a fresh fix. A new area cannot be saved until both coordinates are provided.
Turn the area on or off, then **Save area**. Back discards unsaved edits.

**Clear area** removes one entry after confirmation. **Reset all areas** also
requires confirmation. Stored areas are local NVS configuration; save commits
the blob and verifies a readback before reporting success. A malformed or
unreadable saved record blocks new recordings and requires an explicit reset
or a successful retry; it does not silently disable protection. The existing
general settings backup format does not include this separate record.

The editor uses optional PSRAM and frees its draft when closed, left or put to
sleep. Active or GPS-waiting recordings can view the list but cannot edit it.
The recording takes an immutable copy before starting. All areas are off by
default. Ordinary capture with all areas off has no boundary polling cost.

The main screen shows **PAUSING BOTH RADIOS**, **PAUSED / INSIDE AREA** with
the area name, **PAUSED / WAITING FOR GPS**, or **RESUMING BOTH RADIOS**.
The AREA counter counts discarded transition observations separately from
RSSI filtering and rows rejected for missing GPS/time. Stop retains the usual
save/cleanup behavior; it also works before either radio starts inside an area.

## Recording and radio behavior

The S3 writer evaluates the five areas in slot order every 500 ms using fresh
GPS coordinates and qualified RMC UTC. Detection follows the receiver update
rate and this polling interval, not an exact physical boundary instant. GPS
accuracy still limits boundary accuracy.

The writer syncs its existing file on entry, sends PAUSE to both exact sessions,
and waits for both radio states to confirm RF shutdown. It then drains and
acknowledges queued observations without appending them. Those transition
observations cannot later inherit an outside coordinate. After both queues are
empty and GPS permits recording, RESUME retains the same session, FIFO sequence
and RSSI cache. No new file or radio allocation is required. No file is created
if the whole session remains inside an area without any admitted observations.

Lost replies are reconciled against the same candidate session, including a
START that was applied but not acknowledged. Transport retries and incomplete
pause/resume transitions have bounded deadlines. A shutdown failure reports
an error and proceeds through exact-session Stop cleanup; it cannot claim a
successful pause. Stop during a transition keeps its remaining rows excluded.

The native S3 and C5 receive engine accepts PAUSE and RESUME for its exact
Wardriving session. The common command registry assigns 189 and 190; the C5
adapter requires firmware capability 79 for those two requests. Capability 78
remains sufficient for the existing start/status/read/stop/release commands.
No frame size, payload size or CRC format changes. Pair the S3 and C5 firmware
when installing this revision.

PAUSE immediately closes observation admission, then the radio worker disables
promiscuous receive and cancels BLE discovery. PAUSED is published only after
both have stopped. The worker retains its driver initialization, radio leases,
FIFO, sequence numbers and session identity. Queued observations remain readable
and acknowledgeable. RESUME uses the same session and queue, without a new heap
allocation. A resume arriving before shutdown still waits for that shutdown
barrier. Repeated pause/resume requests are idempotent.

Each pause retires the receiving callback epoch. An earlier BLE registration or
a callback that was already decoding when pause arrived cannot append through
the old epoch. Stop takes precedence over pause and resume. An unresponsive
host still expires the existing ten-second lease while paused, and failed
shutdown retains ownership until cleanup succeeds.

The GPS policy is allocation-free and evaluates five named areas in slot order.
The boundary is inclusive. It validates the complete settings record, finite
coordinates and radius, bounded UTF-8 labels, the GPS fix state and fix age.
Distance calculation handles the antimeridian, poles and rounding at antipodes.
All areas disabled leaves the ordinary continuous scan unchanged.

With an enabled area, an absent or stale fix or UTC produces WAIT GPS and requests a
pause. Upstream clears its inside flag when GPS loses its fix. Cerberus instead
requires a fresh outside position before resuming; loss of position cannot
establish that it has left an area. CSV timestamp qualification remains the
recording layer's separate responsibility.

R1–R6 currently acknowledge a requested configuration before their scan task
confirms hardware shutdown. They also lack observation age/generation evidence
for a boundary drain. **Enabled geofences therefore require R1–R6 off**, enforced
before radio setup. Ordinary expansion recording remains available with all
areas disabled. Expansion geofencing needs a real quiescence acknowledgement
and hardware validation before it can be offered.

## Verification and remaining work

`check_wardrive_radio.py` compiles the production engine for S3 and C5 at 1 ms
and 10 ms scheduler ticks. It covers Wi-Fi, BLE and combined mode; repeated
pause/resume; pauses longer than the lease with keepalives; lease expiry without
keepalives; Stop while paused; pause before the worker starts; resume before
shutdown; delayed BLE cancellation; shutdown failures; callback epoch exhaustion;
and rejection of stale sessions and invalid acknowledgements.

`check_wifi_packet_monitor_session.py` exercises the actual C5 dispatcher prefix
and S3 adapter with all seven Wardriving commands. It checks old-firmware gates,
malformed requests and coexistence with the distinct monitor command family.
`check_wardrive_stream.py` rejects duplicate numbers across the full registry.

`check_wardrive_geofence.py` checks the production GPS policy against an
independent vector-based great-circle calculation, boundary/overlap cases,
invalid settings, UTF-8 limits, stale fixes and antimeridian/polar positions.
It also executes the actual settings functions with NVS boundaries, including
missing/corrupt records, readback mismatch, commit failure, restart and explicit
recovery. Production writer fixtures run at 1 ms and 10 ms scheduler ticks.
They exercise entry/exit, initial and long holds, Stop inside an area, GPS/UTC
loss, lost START/PAUSE/RESUME replies, reentry while resuming, stalled pause and
radio error. They require no appends during exclusion and no resume before both
paused queues drain.

The editor suite in `check_wardrive_ui.py` compiles the real UI and keyboard,
checks Start admission, coordinate validation, active-session locking,
save/failure/clear/reset, Back and memory cleanup, and renders synthetic
screenshots. Console diagnostics expose state, phase, slot and counters without
saved coordinates or area names. These checks do not prove physical radio
shutdown, moving GPS accuracy or peak field performance.

Remaining work:

- Paired installation, moving boundary and GPS-loss tests, physical RF shutdown
  confirmation, Stop/Back and subsequent scans, and final CSV inspection.
- CLI editing and expansion-radio quiescence/coordinator support.
- GPX track/POI recording is a separate optional local output and is not
  geofence-filtered by this writer.

Rev152 remains the frozen installation candidate while rev149 is recording.
This development checkpoint does not replace its package or recovery evidence,
and does not establish full Marauder parity.
