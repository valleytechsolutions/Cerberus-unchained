#pragma once

#include "esp_err.h"
#include "wifi_capture_session.h"
#include "wifi_eapol_analysis.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The C5 receives the frozen fixed/planned scope. The S3 analyzes verified
 * EAPOL and independently labeled beacon context, with optional PCAPNG. */
typedef struct {
    uint8_t channel;
    bool filter_bssid;
    uint8_t bssid[6];
    bool save_pcap;
    const cerberus_wifi_capture_plan_t *plan;
} cerberus_s3_eapol_options_t;

typedef struct {
    cerberus_s3_capture_status_t capture;
    cerberus_wifi_eapol_summary_t summary;
    uint32_t generation;
    uint32_t rejected_provenance;
    uint8_t bssid[6], channel;
    cerberus_wifi_capture_plan_t plan;
    bool planned, filter_bssid;
} cerberus_s3_eapol_status_t;

typedef struct {
    cerberus_wifi_eapol_record_t evidence;
    uint32_t generation;
    /* S3 delivery ages, not RF ages. C5 callback times remain in evidence.
     * UINT32_MAX means unknown (including a local clock rewind) or saturated. */
    uint32_t first_received_age_ms, last_received_age_ms;
} cerberus_s3_eapol_row_t;

typedef struct {
    cerberus_wifi_eapol_context_t evidence;
    uint32_t generation;
    uint32_t first_received_age_ms, last_received_age_ms;
} cerberus_s3_eapol_context_row_t;

typedef struct {
    uint32_t generation;
    uint16_t total, copied;
} cerberus_s3_eapol_page_t;

/* Call once before UI/worker entry. Evidence is allocated in PSRAM on Start. */
esp_err_t cerberus_s3_eapol_init(void);
/* Analysis-only needs no SD card. Start copies the scope and clears
 * evidence only after it successfully reserves its exact capture generation.
 * Capability 87 is required for scoped EAPOL and reserved-slot beacon capture. */
esp_err_t cerberus_s3_eapol_start_options(const cerberus_s3_eapol_options_t *options);
/* Compatibility entry for a fixed selected AP; same capability and lifecycle. */
esp_err_t cerberus_s3_eapol_start(uint8_t channel, const uint8_t bssid[6],
    bool save_pcap);
void cerberus_s3_eapol_stop(void);
esp_err_t cerberus_s3_eapol_status(cerberus_s3_eapol_status_t *out);
/* Atomic copies of at most four retained peer rows. Failed getters clear
 * valid caller output buffers. Retained rows never borrow a later session. */
esp_err_t cerberus_s3_eapol_page(uint16_t offset,
    cerberus_s3_eapol_row_t *rows, size_t capacity,
    cerberus_s3_eapol_page_t *page);
esp_err_t cerberus_s3_eapol_context_page(uint16_t offset,
    cerberus_s3_eapol_context_row_t *rows, size_t capacity,
    cerberus_s3_eapol_page_t *page);
/* Stop retains evidence. Release refuses while this capture still owns its
 * receiver, final frame drain or SD close; callers must retry after cleanup. */
esp_err_t cerberus_s3_eapol_release(void);
