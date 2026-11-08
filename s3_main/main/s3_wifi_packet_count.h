#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Local counterpart of the scoped capability74 Packet Count commands. All
 * replies use the same 22-byte protocol (RAW_READ54); no SPI or implicit RF start. A bound
 * error can retain cleanup ownership. Only canonical unbound replies prove
 * release. S3 rejects plans containing non-2.4-GHz channels. */
esp_err_t cerberus_s3_wifi_packet_count_exchange(uint8_t command,
    const void *request,size_t request_size,void *reply,size_t reply_size);
/* Diagnostic/admission only, never authority to stop another session. */
bool cerberus_s3_wifi_packet_count_active(void);
