#pragma once

#include "esp_err.h"
#include "wifi_capture_session.h"
#include "wifi_sae_analysis.h"

typedef struct {
    uint8_t channel;
    bool filter_bssid;
    uint8_t bssid[6];
    bool save_pcap;
    const cerberus_wifi_capture_plan_t *plan;
} cerberus_s3_sae_options_t;

typedef struct {
    cerberus_s3_capture_status_t capture;
    cerberus_wifi_sae_summary_t summary;
    cerberus_wifi_capture_plan_t plan;
    uint32_t generation, rejected_provenance;
    uint8_t bssid[6], channel;
    bool planned, filter_bssid;
} cerberus_s3_sae_status_t;

typedef struct {
    cerberus_wifi_sae_record_t evidence;
    uint32_t generation;
    /* S3 delivery ages only. UINT32_MAX means unknown or saturated. */
    uint32_t first_received_age_ms, last_received_age_ms;
} cerberus_s3_sae_row_t;

typedef struct {
    uint32_t generation;
    uint16_t total, copied;
} cerberus_s3_sae_page_t;

esp_err_t cerberus_s3_sae_init(void);
/* Copies the scope before the worker starts; caller may discard its plan on
 * return. Capability 86 is required for fixed and planned monitor sessions.
 * save_pcap=false needs no SD card. Failed Start preserves retained results. */
esp_err_t cerberus_s3_sae_start(const cerberus_s3_sae_options_t *options);
void cerberus_s3_sae_stop(void);
esp_err_t cerberus_s3_sae_status(cerberus_s3_sae_status_t *out);
/* Copy at most four rows atomically. No retained pointers into the model. */
esp_err_t cerberus_s3_sae_page(uint16_t offset, cerberus_s3_sae_row_t *rows,
    size_t capacity, cerberus_s3_sae_page_t *page);
/* Stop retains rows. Release refuses until exact receiver/file cleanup ends. */
esp_err_t cerberus_s3_sae_release(void);
