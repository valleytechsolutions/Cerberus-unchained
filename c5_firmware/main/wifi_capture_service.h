#pragma once

#include "cerberus_protocol.h"
#include "wifi_capture_plan.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The Wi-Fi scanner owns radio setup and calls start/stop with its mutex held.
 * Receive is the sole producer and is safe in the promiscuous callback. */
bool cerberus_wifi_capture_service_same_request(
    const cerberus_wifi_capture_start_request_t *request);
esp_err_t cerberus_wifi_capture_service_plan_stage(
    const cerberus_wifi_capture_plan_request_t *,bool radio_held,
    cerberus_wifi_capture_plan_status_payload_t *);
esp_err_t cerberus_wifi_capture_service_plan_target(
    const cerberus_wifi_capture_target_request_t *,bool radio_held,
    cerberus_wifi_capture_plan_status_payload_t *);
uint64_t cerberus_wifi_capture_service_plan_mask(void);
uint16_t cerberus_wifi_capture_service_plan_dwell_ms(void);
esp_err_t cerberus_wifi_capture_service_start(
    const cerberus_wifi_capture_start_request_t *request);
void cerberus_wifi_capture_service_stop(esp_err_t reason);
/* Cleanup timeout is an ERROR with ownership retained, unlike a normal
 * transport lease timeout that has actually stopped the receiver. */
void cerberus_wifi_capture_service_cleanup_error(esp_err_t reason);
/* Radio owner calls this only after cleanup succeeds. Preserve last_error
 * while acknowledging explicit STOP after an already rolled-back failure. */
void cerberus_wifi_capture_service_confirm_stopped(void);
bool cerberus_wifi_capture_service_active(void);
uint32_t cerberus_wifi_capture_service_generation(void);
uint32_t cerberus_wifi_capture_service_deadline_ms(void);
void cerberus_wifi_capture_service_set_deadline(uint32_t monotonic_ms);
uint32_t cerberus_wifi_capture_service_inflight(void);
/* Sample at callback entry, before any other receive consumer. Zero means
 * that callback arrived outside an armed capture interval. Each rearm gets a
 * distinct token; this is independent of the transport's capture generation. */
uint32_t cerberus_wifi_capture_service_admission_token(void);
/* Scanner-owned admission gate. Zero pauses callbacks without stopping the
 * ring/session or its transport lease. Drain inflight before retuning and
 * publish the new channel only after promiscuous reception is enabled. */
esp_err_t cerberus_wifi_capture_service_set_channel(uint8_t channel);
void cerberus_wifi_capture_service_receive(const uint8_t *frame,size_t length,
    uint8_t channel,int8_t rssi_dbm,int64_t callback_us,uint32_t admission_token);
void cerberus_wifi_capture_service_status(cerberus_wifi_capture_status_payload_t *out);
esp_err_t cerberus_wifi_capture_service_peek(uint32_t generation,
    cerberus_wifi_capture_peek_payload_t *out);
esp_err_t cerberus_wifi_capture_service_read(
    const cerberus_wifi_capture_read_request_t *request,
    cerberus_wifi_capture_read_payload_t *out);
esp_err_t cerberus_wifi_capture_service_ack(
    const cerberus_wifi_capture_ack_request_t *request);
