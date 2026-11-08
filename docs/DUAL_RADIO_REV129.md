# Rev129: sustained passive 802.15.4 reception

The existing Beacon and Activity sweeps started reception once per channel but
did not enable receive-when-idle. ESP-IDF 6.0.1 initializes that setting to false
and returns the radio to sleep after a received frame. An unrelated first frame
could therefore hide later Zigbee beacons during the same dwell.

Both modes now enable receive-when-idle after entering promiscuous mode. They
retain the existing explicit sleep at each channel boundary and clear the
setting before restoring promiscuous mode and disabling the radio. Setup,
receive and cleanup failures preserve the first error. Energy detection remains
unchanged. In the installed SDK, promiscuous reception disables automatic ACKs;
this change introduces no transmission, new worker or observation buffer.

C5 performs reception in its existing shared worker; S3 retains the existing
status, result presentation and radio arbitration. The protocol layout and
capability revision 71 remain compatible. Application versions distinguish the
corrected candidate from rev128.

## Evidence and limits

The new fixture includes the actual production worker and receive callback,
with a deterministic SDK boundary modeled from the installed IDF source. The
pre-fix code delivers only the first of three offered frames on each channel:
16 callbacks and no retained beacons. The corrected path delivers 48 callbacks,
including 32 qualifying beacons, retained as 16 network/channel rows. Activity
also receives all five offered frame classes on each of 16 channels.

The fixture checks exact buffer return, no callback delivery after shutdown,
passive ACK settings, retries, first-error precedence across setup/receive/
cleanup, and unchanged Energy behavior. The existing admission, stale-worker
notification and tick-wrap tests run alongside it in `check_radio_worker.py`.
That suite is now explicitly registered in the rev129 full host runner, bringing
the registry to 139 suites (113 parallel and 26 serial). Earlier 138-suite
receipts did not include this standalone worker suite and remain historical
evidence; they do not validate the expanded registry. Independent source review
found no remaining blocker for this narrow fix.

Final local validation passes at source commit
`9fb8b0572c080a8528ef2bcee501bcc43a6b7107`: all 139 registered host suites and all
five native targets use the same 1,145 unchanged inputs. The 21 shard-selection
and aggregate rejection tests also pass. Host checks use Python 3.14.2 and
Zig 0.14.1; native builds use the existing PlatformIO 6.2.0 / IDF 6.0.1 toolchain.
The S3 READY/console and C5 link gates pass. These are local results; the hosted
rev128 CI follow-up is a different source revision and execution.

The matched development package is
`dist/cerberus-unchained-0.1.0-dev.rev129-20261102T190446Z`.
Its three application images match the retained native matrix, all 20 checksum
entries verify, and read-only web-package verification passes chip headers,
partitions and image bounds. The Marauder notice remains included. Manifest
SHA-256 is `ad2c73790c953fdf3660bfce898c0fbc47a72556f0a481f89ca87c4dc76ed1d9`;
package-source SHA-256 is
`a687281eb1f506d73f2093eef5504bec036314fb21a6d00fd7f5915b41bb563a`.

Compared with the retained rev128 images, linked fixed data/BSS is unchanged on
both chips. No worker, observation bank or task allocation was added. This is
not a runtime heap, stack high-water or throughput measurement. Local evidence
is retained in `artifacts/rev129-validation/local-milestone.json`, with bound host
logs, native reports/ELFs and package manifest. Earlier 138-suite evidence is
retained separately in `artifacts/rev129-initial-host-evidence`.

Rev126 remains the last verified installation; rev129 has not been flashed or
accepted on real 802.15.4 traffic. No public image or web-flasher release was
selected. This checkpoint does not close the broader Marauder parity gaps.

This fix does not establish lossless reception, packet timing, mesh membership
or topology. Existing finite sweeps still drain rather than exposing a Stop
API. Queue-loss reporting, bounded typed node/edge observations, scoped control,
frozen reads and SD reports belong to the separate
[Mesh Detection implementation](MESH_DETECTION_PLAN.md). GPS and the optional
six-C5 board are deferred.
