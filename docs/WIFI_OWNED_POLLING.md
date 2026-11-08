# Wi-Fi owned-scan status polling

While Cerberus owns an ordinary Wi-Fi sweep, its UI observer issues one passive Wi-Fi status attempt instead of also querying unrelated Energy, Beacon, Activity, Defense and station status. S3 BLE observation continues independently.

The query clears output before use and has a fresh 25 ms absolute deadline covering the separate 20 ms mutex admission and shared 5 ms READY allowance. Hardware calls already in flight remain synchronous. This owned-only view can resolve or stop known ownership, including an ambiguous START; it cannot admit another C5 operation. Once ownership clears, a full survey checks other owners before admission.

Result-transfer deferral, exact complete-bank publication and cancellation remain intact. Missing status cannot publish stale COMPLETE/generation data. The focused fixture tests actual owning-session logic, stale outputs, ambiguous START, Stop/failure and later admission.

Initial admission surveys, START/STOP, other polling and in-flight SPI retain their existing limits. These changes do not establish physical scan speed, Stop latency or sustained throughput.

See [CONTRIBUTION_REV93.md](CONTRIBUTION_REV93.md) for the installed local checkpoint, independent contribution validation and remaining physical work.
