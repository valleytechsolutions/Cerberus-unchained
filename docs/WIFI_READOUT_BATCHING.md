# Wi-Fi result readout batching

Passive AP readout admits at most four existing state-machine steps per UI tick, under a 2,000-microsecond soft admission budget. Each transport step makes at most one exchange. C5 continues Wi-Fi reception; S3 assembles the result bank and runs its existing BLE/UI schedule. No task or radio engine is added.

A fresh absolute clock guards every substep, including local COMMIT, against the transfer deadline. No-progress detection yields without immediate retry. Before/after publication proofs and atomic staged publication remain mandatory. Stop, leaving the page, sleep and a superseding scan cancel the staging bank while preserving the previous committed snapshot. Ownership is accumulated across a batch so a trailing local transition cannot re-enable unrelated C5 telemetry during that iteration.

The soft budget cannot interrupt an admitted SDK call. Invalid clocks fail unfinished transfers and invalidate timing; terminal outcomes remain terminal. Diagnostic `max_step_us` measures the whole batch, while `step_calls` counts measured UI ticks. Elapsed result-transfer time excludes RF scanning, presentation and physical touch latency. It is not an application-wide response-time guarantee.

Focused checks cover budget admission, slow calls, no progress, frozen and backward clocks, deadlines, cancellation, supported-peer fence errors, failed/restarted generations and atomic publication. Historical owner reports confirmed result display, but Stop during the short readout and controlled speed comparisons remain unverified.

See [CONTRIBUTION_REV93.md](CONTRIBUTION_REV93.md) for the installed local checkpoint, independent contribution validation and remaining physical work.
