#pragma once

#include "c5_spi_link.h"

typedef struct {
    cerberus_c5_wifi_scan_reader_t row;
    cerberus_c5_wifi_relation_reader_t fence;
    cerberus_c5_wifi_relation_bank_t proof;
    uint32_t deadline_ms, arm;
    esp_err_t relation_error;
    uint8_t phase, generation, count, next;
} cerberus_scan_wifi_transfer_t;

/* UI-owned snapshots are ordinary CPU data, not radio/DMA buffers. Keep the
 * last completed snapshots and their shared staging area in external RAM.
 * One allocation survives page changes so Back still shows the same sweep. */
typedef struct {
    cerberus_c5_wifi_result_t wifi[CERBERUS_WIFI_SCAN_MAX_RESULTS];
    cerberus_s3_wifi_observation_t wifi_observations[CERBERUS_WIFI_SCAN_MAX_RESULTS];
    cerberus_s3_wifi_observation_t wifi_staging_observations[CERBERUS_WIFI_SCAN_MAX_RESULTS];
    /* Wi-Fi readout spans UI ticks. BLE/beacon captures may use the union below
     * between steps, so their staging must never alias this pending AP bank. */
    cerberus_c5_wifi_result_t wifi_staging[CERBERUS_WIFI_SCAN_MAX_RESULTS];
    cerberus_scan_wifi_transfer_t wifi_transfer;
    cerberus_c5_ble_result_t ble[CERBERUS_BLE_SCAN_MAX_RESULTS];
    cerberus_ieee802154_beacon_scan_result_payload_t
        beacons[CERBERUS_IEEE802154_BEACON_MAX_RESULTS];
    union {
        cerberus_c5_ble_result_t ble[CERBERUS_BLE_SCAN_MAX_RESULTS];
        cerberus_ieee802154_beacon_scan_result_payload_t
            beacon[CERBERUS_IEEE802154_BEACON_MAX_RESULTS];
    } staging;
} cerberus_scan_display_store_t;
