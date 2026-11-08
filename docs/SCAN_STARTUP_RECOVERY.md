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

The focused scanner and dual-scan coordinator checks pass. Complete host/native
validation and installation are pending at this source checkpoint. No new task,
result bank, radio operation or wire command is added; C5 capability remains 81.
Physical Wi-Fi/Bluetooth transition and sustained-use checks remain separate.

The independent review also compared the GPS qualification changes and found
no actionable regression in the reviewed cases. This is a review result,
not proof of every receiver's output or field behavior.
