#pragma once

#include "esp_err.h"
#include "c5_spi_link.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool checked;
    esp_err_t error;
    uint8_t generation;
    uint8_t exact;
    uint8_t review;
    uint8_t other;
    uint8_t compared;
    size_t saved;
} sentinel_baseline_t;

/* Compare one caller-validated completed AP publication against local trust.
 * The caller owns publication/age validation, keeps rows immutable and distinct
 * from summary, and serializes this call with settings mutations on the UI task.
 * NULL rows is valid only for an empty completed bank. All rows are validated
 * before any settings lookup. Names retain their received C-string identity;
 * this is not full binary/on-air SSID identity. Hidden names remain untrusted.
 * No transport, RF, allocation, task or persistence operations are performed.
 * On failure only checked/error are populated; no partial counts are published. */
esp_err_t sentinel_baseline_compare(sentinel_baseline_t *summary,
    const cerberus_c5_wifi_result_t *rows, size_t count, uint8_t generation,
    bool settings_available);
