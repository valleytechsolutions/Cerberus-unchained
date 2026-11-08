# Ordinary Wi-Fi list sorting and filters

The ordinary AP list offers SIGNAL, NAME A-Z and CHANNEL sorting, independently combined with ALL BANDS, 2.4 GHZ and 5 GHZ. Its existing ALL AGES/WITHIN 30S control remains independent. Signal/channel ties use ASCII case-insensitive received names; hidden names use BSSID text. Equal keys retain source order. Unknown channels remain ALL-only and unavailable RSSI never outranks measured values.

S3 builds a bounded index map over at most 32 immutable rows. Controls reset page zero and persist for the UI runtime; they do not scan, access SPI or read SD. The explicit action-target picker retains its prior ordering and controls. Opening a detail page retains its existing detail-specific status/storage behavior.

A row tap must match the successfully painted map token, exact committed publication, saved generation and bank count. A new unpainted map, reused generation byte or partial paint failure cannot silently select a different row. The viewer refreshes and requires a new tap. Selected-SSID analysis compares the cached received C strings before display sanitization; SDK/wire truncation prevents a claim of full binary on-air SSID identity.

Host tests cover source identity, ties, hidden/raw names, every channel/RSSI value, age bounds, all control combinations and failed paint. Physical controls, selection/expiry and concurrent repeated scanning still need acceptance.

The related ledger audit found `WIFI_SCAN_STATION_WAR_DRIVE=33` dormant in pinned Marauder develop: its CLI branch is commented out and ordinary Wardrive uses mode 32. Existing main-firmware station-relation CSV is separate. This classification correction does not invent another missing service or establish complete AP/station parity.

See [CONTRIBUTION_REV93.md](CONTRIBUTION_REV93.md) for the installed local checkpoint, independent contribution validation and remaining physical work.
