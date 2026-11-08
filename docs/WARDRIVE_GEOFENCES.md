# Wardriving geofences

The development work adds the radio pause/resume contract and the GPS boundary
policy needed for geofences. It does **not** expose a working geofence menu yet.
Settings persistence, touchscreen editing and the Wardriving coordinator still
need to connect these parts. Existing recordings do not use geofences.

The reference is Marauder develop
[`79cc6bc`](https://github.com/justcallmekoko/ESP32Marauder/tree/79cc6bcba1db6679baee1dab2b0c1ad5010e6363),
checked again on November 8, 2026 UTC. `Settings.cpp:779` supports five named
areas with a radius of 0.1–1.0 miles. `WiFiScan.cpp:6304` pauses scan progression
inside an enabled area; `WiFiScan.cpp:6509` evaluates the first matching area
every 500 ms. This is a radio behavior, not just a CSV row filter.

## Implemented parts

The native S3 and C5 receive engine accepts PAUSE and RESUME for its exact
Wardriving session. The common command registry assigns 189 and 190; the C5
adapter requires firmware capability 79 for those two requests. Capability 78
remains sufficient for the existing start/status/read/stop/release commands.
No frame size, payload size or CRC format changes.

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

With an enabled area, an absent or stale fix produces WAIT GPS and requests a
pause. Upstream clears its inside flag when GPS loses its fix. Cerberus instead
requires a fresh outside position before resuming; loss of position cannot
establish that it has left an area. CSV timestamp qualification remains the
recording layer's separate responsibility.

## Verification and remaining integration

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
These checks do not prove physical radio shutdown, moving GPS accuracy or a
complete geofence workflow.

Remaining work:

- Persist and edit five areas, with manual coordinates or the current fresh fix,
  enable/disable, radius, name and clear actions in one navigation branch.
- Coordinate pause/resume of both onboard radios from fresh GPS snapshots while
  retaining one recording. Handle queued observations and transport uncertainty
  explicitly; a displayed pause must reflect both radio acknowledgements.
- Apply the same policy to optional expansion radios, or reject that combination
  before capture until their protocol can provide the required acknowledgement.
- Add the live state, matched area and CLI workflow, then test boundary crossing,
  GPS loss, Stop/Back, saved CSV contents and subsequent scans on hardware.

Rev152 remains the prepared installation candidate. This development branch
does not replace that frozen package or its recovery evidence.
