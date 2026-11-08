# BLE extended receive: native configuration gap

Native extended advertising reception remains disabled in the current S3 configuration. Parser support for 255-byte synthetic reports does not demonstrate reception of those reports on hardware.

The local SDK audit used ESP-IDF 6.0.1 (PlatformIO framework package 4.60001.0). In that SDK, `CONFIG_BT_NIMBLE_EXT_ADV` controls `MYNEWT_VAL(BLE_EXT_ADV)`. The separate `BT_NIMBLE_EXT_SCAN` option does not override that gate. See the repository's [radio defaults](../sdkconfig.radio-lab.defaults) and [scanner start/event paths](../s3_main/main/s3_ble_scanner.c): with EXT_ADV disabled the native start path calls legacy `ble_gap_disc`, and the EXT_DISC event branch is excluded.

Enabling that flag alone is not a complete fix. In the inspected NimBLE implementation, legacy advertising start/stop/set-data APIs are compiled for the opposite gate. Migration requires checking all callers, controller support, event completeness and truncation, scan-response provenance, concurrent ownership and cleanup before changing configuration. No such migration is included in this contribution.

The existing strict parsers, source-aware lists, Analyzer and archive codecs retain their bounded contracts. Saved BLE evidence contains at most 31 retained original ADV bytes and no raw response payload. Neither those APIs nor synthetic host fixtures establish coded-PHY or native extended-report reception.

This is a Cerberus capability gap, not proof that every pinned Marauder board configuration exposes the same extended workflow. Hardware acceptance and broader upstream parity remain separate. See [BLE SD snapshots](BLE_SD_SNAPSHOTS.md) and [the contribution summary](CONTRIBUTION_REV93.md).
