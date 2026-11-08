#pragma once

#include "c5_spi_link.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Preserve the existing summary layout. The detail renderer shows up to three
 * of the four retained peer rows; counts describe matching retained rows, not
 * deduplicated BSSIDs or a complete inventory of the surrounding network. */
typedef struct {
    uint8_t total_bssids;
    uint16_t security_mask;
    uint8_t count_24_ghz;
    uint8_t count_5_ghz;
    int8_t strongest_rssi;
    int8_t weakest_rssi;
    uint8_t peer_count;
    cerberus_c5_wifi_result_t peers[4];
} cerberus_s3_wifi_ssid_analysis_t;

/* Analyze a caller-owned immutable completed snapshot. The caller must bind
 * selected_index to its exact local publication and selected AP identity.
 * This function cannot establish that ownership or freshness on its own.
 *
 * Group exact received strings by length and unsanitized bytes before NUL.
 * The SDK/C5 wire format already truncates embedded-NUL SSIDs; this is not
 * complete binary/on-air SSID identity. Empty names are selected-row-only.
 * Display sanitization must happen after analysis and never mutate the bank.
 *
 * No SPI, RF, allocation or task operation. At most 32 rows are examined.
 * Any malformed row or invalid argument fails atomically with a zero output.
 * Output must not overlap rows. The input snapshot is never modified. */
bool cerberus_s3_wifi_ssid_analyze(const cerberus_c5_wifi_result_t *rows,
    size_t count, size_t selected_index, cerberus_s3_wifi_ssid_analysis_t *out);
