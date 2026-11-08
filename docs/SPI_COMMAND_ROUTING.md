# Wardriving and Packet Monitor command routing

Rev152 assigns Wardriving its own entries in the shared SPI command registry.
Earlier Wardriving headers assigned 172–176 independently, while Packet
Monitor used the same values for its session commands. The C5 dispatcher
checked Wardriving first and returned when a normal 32-byte monitor request
failed the Wardriving 64-byte transfer/16-byte query requirement. Packet
Monitor never reached its handler.

| Workflow | Commands | Minimum C5 firmware capability |
| --- | --- | --- |
| Packet Monitor session | 172–176 | 78 |
| Packet Monitor recording | 177–183 | 78 |
| Wardriving | 184–188 | 78 |

Wardriving aliases now refer to `cerberus_spi_command_t`; they do not maintain
a second numeric assignment. Both monitor gates require capability 78 because
older C5 builds contain the conflicting dispatcher. The Wardriving gate also
requires 78 because its wire command numbers changed. SPI framing stays at
protocol version 2, with the existing payload sizes and CRC checks.

Install the matched S3 and C5 images together. A new S3 rejects older C5
firmware before starting these workflows. An old S3's 16-byte Wardriving
queries do not match the monitor payload sizes on a new C5 and are rejected;
they must not start or stop a monitor session. The earlier received-packet
counter uses commands 161–171 and is a separate workflow.

## Regression evidence

The monitor bridge fixture now includes the production C5 dispatcher entry
checks and Wardriving branch before the actual monitor cases. With the old
assignments, the test fails at the first monitor INFO request. With the new
assignments, the same fixture passes session start, raw recording, lost replies,
Stop, retained ownership, navigation and repeated-session checks.

The fixture also exercises all five wide Wardriving requests, truncated
transfers, invalid payload lengths, bad CRCs, stale requests using the old
numbers, and rejection of C5 capabilities 75–77. The Wardriving stream suite
compiles every registered command into one switch: duplicate numeric values
fail compilation even if isolated service tests would pass.

These are production-source host checks. They do not establish physical
Packet Monitor/PCAP acceptance, RF performance or SD finalization on the
connected board. Those checks follow installation of the paired update.

This routing fix takes priority over the audited but unimplemented geofence
workflow. It also includes the rev150 GPX work and rev151 RSSI write filter;
their outstanding physical checks remain outstanding.
