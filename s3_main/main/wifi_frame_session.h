#pragma once

#include "esp_err.h"
#include "wifi_capture_session.h"
#include "wifi_frame_analysis.h"

typedef struct {
    uint8_t channel, mode_mask;
    bool filter_bssid;
    uint8_t bssid[6];
    bool save_pcap;
    const cerberus_wifi_capture_plan_t *plan;
} cerberus_s3_frame_options_t;

typedef struct {
    cerberus_s3_capture_status_t capture;
    cerberus_wifi_frame_summary_t summary;
    uint32_t generation, rejected_provenance, delivered_age_ms;
    uint8_t bssid[6], channel, mode_mask;
    cerberus_wifi_capture_plan_t plan;
    bool planned, filter_bssid, delivered;
} cerberus_s3_frame_status_t;

typedef struct {
    cerberus_wifi_frame_record_t evidence;
    uint32_t generation, first_received_age_ms, last_received_age_ms;
} cerberus_s3_frame_row_t;

typedef struct {
    uint32_t generation;
    uint16_t total, copied;
} cerberus_s3_frame_page_t;

esp_err_t cerberus_s3_frame_init(void);
/* Capability 88 required. Copies options/plan, owns no extra RF worker.
 * Failure preserves the previous stopped report. SAVE OFF needs no SD lease. */
esp_err_t cerberus_s3_frame_start(const cerberus_s3_frame_options_t *);
void cerberus_s3_frame_stop(void);
esp_err_t cerberus_s3_frame_status(cerberus_s3_frame_status_t *);
/* Copies at most four rows. All ages are S3 delivery ages; UINT32_MAX means
 * unknown/rewound/saturated. Evidence source times remain C5 monotonic time. */
esp_err_t cerberus_s3_frame_page(uint16_t offset, cerberus_s3_frame_row_t *,
    size_t capacity, cerberus_s3_frame_page_t *);
/* Refuses while exact-generation RX drain or SD close is still pending. */
esp_err_t cerberus_s3_frame_release(void);
