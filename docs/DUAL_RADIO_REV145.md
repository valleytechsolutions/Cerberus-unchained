# rev145: upload certificate verification

The last WDGWars attempt failed in the S3 TLS handshake before receiving an HTTP response. Its error did not establish an API-key rejection.

On November 7, a credential-free TLS 1.2 check found this server chain:

```text
wdgwars.pl → WE1 → GTS Root R4 → GlobalSign Root CA
```

The current firmware bundle contains GTS Root R4 and verifies WE1's signature with that key. It does not contain the old GlobalSign root. ESP-IDF's default bundle path follows the supplied cross-signed certificate to the missing root, matching [Espressif's documented failure](https://github.com/espressif/esp-idf/issues/18674).

The S3 configuration now enables cross-signed root selection and certificate date checks. The [certificate handler backport](../third_party/esp_crt_bundle/README.md) supplies Espressif's later fixes for allocation cleanup, synthetic-root dates, bundle bounds and detach behavior. The project replaces one component source during the build; the installed SDK package is unchanged. An SDK source mismatch stops configuration for review.

Uploads still require the saved home AP, current UTC, CA and hostname verification, and an explicit user action. Receipt handling and service retry confirmations are unchanged.

The matched package includes the vendored handler and its CMake hook in its source fingerprint. Changing either requires a new matched build. S3, C5 and the recovery bridge share the rev145 package version; the C5 protocol and radio behavior are unchanged.

## Validation

The SDK integration test reproduces the old rejection and verifies the corrected chain path using real Mbed TLS/PSA cryptography. It rejects unknown roots, incorrect hostnames, expired/future leaf and intermediate certificates, and a modified signature. It completes 128 repeated verifications and 16 allocation-failure trials without retained allocations.

All 166 standard host suites and all five firmware targets passed at `5939108`, with unchanged input hashes. The build/report tests also reject packaging after a vendored TLS source change. All 22 package checksums verified. The additional SDK integration check passed separately against the unchanged handler.

A TLS 1.2 handshake from the PC, using the actual SDK verifier and firmware certificate bundle, gave these results. The test stopped after TLS; it sent no HTTP request or API credentials.

| Host | Original handler | Corrected handler |
| --- | --- | --- |
| `wdgwars.pl` | `-0x3000`, matching the recorded device error | Verified |
| `api.wigle.net` | Verified | Verified |
| `wardrift.net` | Verified | Verified |

The matched rev145 package was installed on November 7 after fresh S3 and C5 recovery backups were checked against the installed images. Each programmed image passed flash-digest verification. Both chips booted, the SD card mounted and the saved upload profile loaded. Two idle observations 41.8 seconds apart showed advancing uptime and SPI traffic without a reset or additional transport errors.

A later user-triggered upload of a newly recorded CSV succeeded on the installed rev145 firmware. Read-only device diagnostics recorded WDGWars HTTP 202, a complete validated response and no TLS or transport error. Both WiGLE and WDGWars had acknowledged results. This verifies the device upload path for that request; server processing and account credit were not checked. The diagnostic capture was read-only; it sent no additional log or retry.

An earlier service retry separately timed out while establishing UTC, before any HTTP request. The [rev147 clock-recovery change](DUAL_RADIO_REV147.md) addresses that dependency. The earlier uncertain receipt remains unchanged and still requires service retry confirmation. Repeated-upload memory use remains unverified on hardware.
