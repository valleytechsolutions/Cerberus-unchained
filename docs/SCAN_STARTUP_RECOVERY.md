# S3 scan startup cleanup

Rev160 fixes a remaining startup failure path in the rev159 S3 cleanup worker.
If Wi-Fi initialization succeeded but storage, mode, event registration or
driver start failed, cleanup attempted to clear the scan result list even
though the driver had never started. The staged retry could remain at that
operation and never reach deinitialization.

The SDK documents that `esp_wifi_clear_ap_list()` can return
`ESP_ERR_WIFI_NOT_STARTED` when the driver is stopped. This is present in the
pinned ESP-IDF header used by this build and the
[Espressif API reference](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/network/esp_wifi.html#_CPPv422esp_wifi_clear_ap_listv).
The earlier fixture returned success for that case, hiding the failure.

Cleanup now requests AP-list clearing only after a successful driver start.
If a previously started driver reports NOT_STARTED during cleanup, the worker
continues to handler removal and deinitialization. Successful deinitialization
is still required before releasing the owner. Other errors retain the existing
250 ms retry policy. The original startup error remains visible, and the
previous completed result bank is preserved.

The fixture now enforces the SDK's stopped-driver precondition. Applying that
fixture to the uncorrected worker reproduced repeated cleanup failure and hit
the test's 100-retry limit. The corrected worker passes every setup-failure
case, preserves the previous result bank, releases ownership and permits a
Bluetooth claim followed by another Wi-Fi scan. A separate stopped-driver case
fails deinitialization, confirms ownership remains held, then verifies that
recovery retries only deinitialization before release.

All 171 host suites and five native targets passed on unchanged inputs. The
matched package also passed its source, SDK/toolchain and image checks.
No new task, result bank, radio operation or wire command is added; C5
capability remains 81. Physical Wi-Fi/Bluetooth transition and sustained-use
checks remain separate.

The independent review also compared the GPS qualification changes and found
no actionable regression in the reviewed cases. This is a review result,
not proof of every receiver's output or field behavior.

## Installed checkpoint

Rev160 is installed from source `5cefe1f4432ebabe41b5e8b5de34d61683b4c731`,
tagged `checkpoint/rev160-scan-startup-recovery`. Package:
`cerberus-unchained-0.1.0-dev.rev160-20261108T072137Z`.
Manifest SHA-256:
`411c697e885abde64be1e5b35e264b357502460706ab3a48c4a4cf57054462ab`.

Fresh S3/C5 backups matched the installed rev159 baseline and verified against
flash. All nine image writes passed verification. Both processors' live build
descriptors match the packaged versions, SDK, compile times and full ELF hashes.

Two idle captures 58.3 seconds apart showed advancing uptimes, mounted SD and
unchanged free memory: 134,244 internal bytes on S3, a 94,208-byte largest
internal block and 1,426,140 bytes of PSRAM. S3 had ten tasks; the C5 snapshot
reported 53,268 free bytes and one boot. Four initial READY timeouts/fallbacks
were present in both captures; mutex, READY-fall, READY-high and driver errors
were zero. The monitored error counters did not increase. The saved home
Wi-Fi, WiGLE and WDGWars profile readiness flags remain set. No upload was
requested.

This establishes the tested startup-cleanup logic and paired idle installation.
It does not inject a real SDK startup failure on the board or establish
sustained RF/GUI behavior. Repeated Wi-Fi/Bluetooth transitions and earlier
Channel Activity/GPS field checks remain pending. The distinct
[Channel Analyzer workflow](CHANNEL_ANALYZER_PLAN.md) is also still pending;
the source audit records its requirements without adding a placeholder menu.
