# Wi-Fi readout failure recovery

Passive result-readout READY phases share a 5,000-microsecond software allowance, capped by the absolute transfer deadline. Mutex admission is separately capped at 20 ms. The list distinguishes C5 readout timeout, invalid reply and S3 memory failure, retains the previous complete bank and leaves Back available. A new scan explicitly starts another attempt.

A successful SDK polling start is followed by synchronous polling end before stack buffers can be released. A rejected start clocks no transaction and does not arm recovery. Once a transaction starts, failure retains the required edge-recovery state so a later exchange cannot consume its stale reply. The common legacy path is affected only when recovering such an unfinished exchange; ordinary blocking relation workers keep their existing transport.

Host tests exercise actual READY logic, absent/falling/stuck edges, absolute deadlines and wrap, mutex ownership, start/end failures, zeroed outputs and later recovery. Hardware SPI already in flight cannot be preempted by this allowance. General status polling and other UI operations have their own limits; no whole-application latency bound is claimed.

A historical rev86 observation showed one completed 28-row transfer in 0.469596 seconds across 23 UI ticks, alongside owner-reported scan/client navigation. That was one uncontrolled sample of an older image, not a performance benchmark or acceptance of the current contribution. Deliberate hardware fault recovery and prolonged operation remain unverified.

See [CONTRIBUTION_REV93.md](CONTRIBUTION_REV93.md) for the installed local checkpoint, independent contribution validation and remaining physical work.
