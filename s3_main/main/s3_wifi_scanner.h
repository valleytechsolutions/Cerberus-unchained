#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CERBERUS_S3_WIFI_SCAN_CAPACITY 32U
#define CERBERUS_S3_WIFI_SCAN_SOURCE_S3 1U
#define CERBERUS_S3_WIFI_SCAN_PASSIVE_DWELL_MS 120U
#define CERBERUS_S3_WIFI_SCAN_WAIT_MS 6000U
#define CERBERUS_S3_WIFI_SCAN_CLEANUP_RETRY_MS 250U

typedef enum {
    CERBERUS_S3_WIFI_SCAN_IDLE,
    CERBERUS_S3_WIFI_SCAN_RUNNING,
    CERBERUS_S3_WIFI_SCAN_STOPPING,
    CERBERUS_S3_WIFI_SCAN_COMPLETE,
    CERBERUS_S3_WIFI_SCAN_CANCELLED,
    CERBERUS_S3_WIFI_SCAN_ERROR,
} cerberus_s3_wifi_scan_state_t;

/* SDK scan-result identity, never a C5 bank index or generation. The ESP-IDF
 * AP record supplies a NUL-terminated SSID, not an original beacon IE: bytes
 * beyond an embedded NUL cannot be recovered from this API. Do not advertise
 * these rows as original-frame/raw-SSID evidence. */
typedef struct {
    uint8_t bssid[6];
    uint8_t ssid[32];
    uint8_t ssid_length;
    uint8_t channel;
    uint8_t security;
    int8_t rssi;
    uint8_t source;
} cerberus_s3_wifi_scan_row_t;

typedef struct {
    uint32_t job_id;
    uint32_t generation;
    uint32_t elapsed_ms;
    /* Local SDK readout receipt time, NOT beacon callback time. In particular,
     * this must not set the existing C5 frame-observation `valid` bit. */
    uint32_t completed_ms;
    esp_err_t error_code;
    esp_err_t cleanup_error;
    uint16_t total_count;
    uint16_t dropped_count;
    uint8_t count;
    uint8_t state;
    bool truncated;
    bool radio_owned;
} cerberus_s3_wifi_scan_status_t;

/* Only an idle Wi-Fi and idle BLE owner can admit this passive 2.4 GHz sweep.
 * No service is stopped or preempted. Uses the SDK's configured country policy,
 * no association or probe requests, and a temporary 4096-byte worker stack.
 * Pending/result memory requires PSRAM. Each public getter copies its output.
 * There is one scanner consumer; callers retain the returned job/generation.
 * Exhausted UINT32_MAX job space fails closed until reset, never reuses an ID. */
esp_err_t cerberus_s3_wifi_scanner_start(cerberus_s3_wifi_scan_status_t *out);
esp_err_t cerberus_s3_wifi_scanner_get_status(cerberus_s3_wifi_scan_status_t *out);
esp_err_t cerberus_s3_wifi_scanner_stop(uint32_t job_id);
esp_err_t cerberus_s3_wifi_scanner_get_result(uint32_t generation, uint8_t index,
    cerberus_s3_wifi_scan_row_t *out);

/* COMPLETE replaces the retained bank only after successful cleanup. Cancel,
 * timeout and failed replacement preserve its generation/count/receipt time.
 * Cleanup uncertainty keeps the worker and SCAN owner reserved. The worker
 * retries only unfinished teardown stages; STOPPING and cleanup_error remain
 * visible until release. Stop still records cancellation during retries.
 * A persistent driver fault blocks other radio owners; it is never force-cleared.
 * SDK calls already in flight are not interrupted by the software deadline. */

/* Bounded ASCII/\\xHH display; identity bytes remain untouched. Returns full
 * required character count, even if output is truncated. capacity includes
 * the terminating NUL. NULL output is valid only with capacity zero. */
size_t cerberus_s3_wifi_scanner_ssid_display(const cerberus_s3_wifi_scan_row_t *row,
    char *out, size_t capacity);
