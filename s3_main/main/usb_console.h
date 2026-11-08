#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the bounded, read-only native USB diagnostic console. Lifecycle calls
 * are serialized by the main/UI task; stop waits for commands to finish before
 * releasing its PSRAM output scratch and the shared internal USB PHY for mass
 * storage. No PSRAM means the optional console fails without an internal-RAM
 * fallback. A stop timeout retains resources until a later quiescent stop. */
esp_err_t cerberus_usb_console_start(void);
esp_err_t cerberus_usb_console_stop(void);

#ifdef __cplusplus
}
#endif
