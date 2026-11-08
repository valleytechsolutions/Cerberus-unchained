# ESP-IDF certificate bundle backport

`esp_crt_bundle.c` is copied unchanged from Espressif's [ESP-IDF v6.0.3](https://github.com/espressif/esp-idf/blob/76f5dedd9950a3012fee8fb7d5586df21fc67802/components/mbedtls/esp_crt_bundle/esp_crt_bundle.c), commit `76f5dedd9950a3012fee8fb7d5586df21fc67802`, under Apache-2.0. The source notice and license are retained here.

Cerberus currently uses PlatformIO's ESP-IDF 6.0.1 package. WDGWars sends a certificate chain containing GTS Root R4 cross-signed by the retired GlobalSign root. The bundle contains GTS Root R4, but the default verification path follows the supplied chain to the missing GlobalSign root and fails before HTTP. Espressif documents that behavior in [issue #18674](https://github.com/espressif/esp-idf/issues/18674).

Enabling cross-signed verification alone would expose the old handler's allocation leak and synthetic-root date failure. The v6.0.3 handler includes the [allocation fix](https://github.com/espressif/esp-idf/commit/0d8ff68f8a47fc05a6b1d3a5bda24764a48e5fbd), correct handling of validity dates, bundle bounds checks, and callback cleanup. Real server and intermediate certificates still have to pass date, name, signature, and trust checks.

The S3 build replaces only this component source. It does not edit the installed SDK, add a retired root, or pin a service's leaf certificate. The original source hash is checked at configure time so an SDK upgrade requires reviewing or removing this backport. Remove it when the project's PlatformIO package contains these fixes.

SHA-256 of the vendored source, with LF line endings:
`a912969d617c93bf00efc73aaa464f5e276ed421ac78b5268e0630ec7f90cde9`

## Regression check

`tools/check_wardrive_tls_bundle.py` builds the pinned SDK's Mbed TLS and PSA software implementation on the host. It needs Python `cryptography`, Zig 0.14.1, CMake, Ninja and the PlatformIO ESP-IDF 6.0.1 package:

```text
python tools/check_wardrive_tls_bundle.py --idf-path <framework-espidf> --zig <zig-executable> --cmake <cmake-executable> --ninja <ninja-executable>
```

The test generates synthetic certificates. The original verifier must reject the cross-signed chain and accept its direct equivalent. The backport must accept both, reject an unknown root, a wrong hostname, invalid leaf/intermediate dates and a damaged signature, then release all allocations across repeated verification and injected allocation failures. It also checks that detaching removes both callbacks. The SDK integration check is separate from the 166 host suites because it requires the native SDK package.

This establishes software certificate verification and cleanup, not an on-device HTTPS connection or a successful service upload.
