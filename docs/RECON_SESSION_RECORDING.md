# Recon SD session recording

This guide describes the recorder used by the current Recon workflow. Rev124's
dashboard and optional radar view share this recorder and do not start a
second writer. The last verified installed pair is rev123; the new UI's
validation is recorded in [the rev124 checkpoint](DUAL_RADIO_REV124.md).
Earlier contribution evidence remains in [CONTRIBUTION_REV93.md](CONTRIBUTION_REV93.md).

## Rev91 typed row counters

Rev91 adds accepted_wifi_rows, accepted_ble_rows, accepted_ibeacon_rows, written_wifi_rows, written_ble_rows and written_ibeacon_rows to SESSION.JSON, with row_counter_version=1. Accepted means admitted to the queue; written requires a successful full-row commit. iBeacon counts are a BLE hint subset, and repeated sweep rows remain included. These are row totals, not unique devices or per-callback events; saturated counters are lower bounds. The existing CSV and v1 schema name remain unchanged. See [Recon inventory and activity](RECON_INVENTORY.md).

Rev90 adds an optional SD journal to the existing Recon screen. C5 Wi-Fi and S3 BLE keep their existing independent scan sessions. The UI copies completed-sweep observations into a bounded queue, and a separate S3 task writes that queue to SD. Recording does not start another radio session or change radio ownership.

## Using a session

Open Recon, then its Options screen, and choose **START SD RECORDING**. Recording starts off by default. The existing mounted SD card must belong to the firmware; USB storage mode cannot own it at the same time. The screen shows the session folder, written and missed counts, and any exact storage error.

Choose **STOP AND SAVE RECORDING** to finish. Pause, leaving Recon, or successful screen sleep also request a stop. Saving is asynchronous: wait for **SAVED** before disconnecting power or attempting to hand the card to USB. Opening or resuming Recon does not automatically restart recording. Clearing live hint history does not remove saved sessions.

Each session gets a new directory, such as `/CERBERUS/RECON/R00001`, containing:

- `OBS.CSV`: observation rows, with a fixed header.
- `SESSION.JSON`: session identity, timing, counters, completion state, error code and interpretation limits.

While writing, these files have `.TMP` names. A successful finish publishes `OBS.CSV` first and `SESSION.JSON` last. The final JSON name is the software completion marker. An error can leave recoverable temporary files, or a CSV with a temporary manifest; those are not a completed pair. Existing folders and files are not silently overwritten. Session names range from R00001 to R99999, with at most 1,024 directory candidates checked per start.

## What the journal means

Rows represent the ordinary retained AP and BLE inventories consumed after a completed, validated sweep. Nonmatching devices and repeated observations across sweeps are included. This is not a capture of every received packet. Existing inventory, truncation and rejection limits still apply. Richer Watch environment/Remote ID records and station/probe relationship journals are outside this first recording implementation.

Each row keeps the radio, typed address, generation, available channel and RSSI, received name bytes, available source timing, consumption timing, classification rule and alert-queue outcome. Wi-Fi BSSIDs and BLE addresses use their respective native byte order internally and conventional display order in CSV. BLE address rotation does not establish a persistent physical-device identity. A classification remains a passive hint, not authenticated identity.

S3 consumption uptime is separate from source evidence. Wi-Fi source age retains its request anchor. BLE source time and sequence require a matching frozen advertisement record; otherwise they remain explicitly unavailable. The iBeacon field is a current qualifying hint, not a full UUID/major/minor export. A hint can come from a matched scan response; the row's labelled advertisement timestamp does not claim to be that iBeacon payload's timestamp. RSSI is labelled independently as the Wi-Fi driver aggregate or BLE sweep peak, rather than presented as a measurement taken at a source-evidence timestamp. No UTC time, GPS fix or geographic location is invented.

Received names use a reversible `bytes:` representation with escaped nonprinting bytes and CSV quoting. This prevents a received name from adding CSV rows or becoming a spreadsheet formula. The boot-local numeric session ID is not globally unique; use it together with the durable session folder.

## Resource and failure handling

The 64-row queue and formatting buffers are allocated in PSRAM only while recording, with no fallback into internal RAM. A separate 6 KiB worker stack uses internal RAM. Start checks available internal memory and its largest block before allocation; those are admission checks, not reserved memory or proof of peak operation.

UI calls perform no filesystem operations. The worker formats and writes outside the short queue lock. When the queue fills, new observations are counted as dropped and existing accepted rows remain intact. Invalid rows and rows abandoned after an error have separate counters. Displayed MISSED combines these counts; saturated counters are lower bounds. WRITTEN means a full append succeeded, not that each row is independently durable.

The writer synchronizes periodically and closes both streams before publishing a completed pair. A partial append is never blindly retried. The combined session limit is 64 MiB, with 4,096 bytes reserved for the manifest. A recording lease blocks incompatible capture, USB ownership transfer and mutations of its folder or ancestors. Unrelated historical files remain accessible under the existing storage rules. Stop can wait for filesystem work; no fixed SD latency or power-loss atomicity is promised.

The read-only `recon_record` diagnostic command exposes state, ownership, session folder/ID, uptime, queued count, and accepted/written/dropped/invalid/abandoned counters. It cannot start recording or radio activity.

## Porting scope and verification

This advances the reachable [Marauder Recon workflow](https://github.com/justcallmekoko/ESP32Marauder/blob/79cc6bcba1db6679baee1dab2b0c1ad5010e6363/esp32_marauder/ReconMission.cpp), which journals new BLE/iBeacon observations and generic BLE repeats. Cerberus uses its own CSV/JSON format and existing dual-radio scheduler. Completed-sweep recording does not reproduce a per-callback stream or establish Marauder file-format compatibility or full Recon parity.

Physical recording acceptance is pending. It requires an explicitly started session, successful stop/save, inspection of the files on the real card, and another session without resetting. Host fault injection does not establish physical card failure recovery, power-loss behavior, prolonged recording performance or peak stack/heap use. Full Marauder parity and the earlier unresolved feature reports remain separate work.
