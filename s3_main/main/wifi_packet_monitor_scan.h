#pragma once

#include "esp_err.h"
#include "wifi_packet_monitor_core.h"
#include "wifi_packet_monitor_session.h"

typedef enum {
    WIFI_MONITOR_OFF, WIFI_MONITOR_INFO, WIFI_MONITOR_START,
    WIFI_MONITOR_WATCH, WIFI_MONITOR_STOP, WIFI_MONITOR_DRAIN, WIFI_MONITOR_RELEASE,
} wifi_packet_monitor_phase_t;

typedef struct {
    esp_err_t (*info)(cerberus_wifi_packet_monitor_session_status_t *out);
    esp_err_t (*start)(uint64_t session, uint8_t channel,
        cerberus_wifi_packet_monitor_session_status_t *out);
    esp_err_t (*status)(uint64_t session,
        cerberus_wifi_packet_monitor_session_status_t *out);
    esp_err_t (*stop)(uint64_t session,
        cerberus_wifi_packet_monitor_session_status_t *out);
    esp_err_t (*sample)(uint64_t session, uint16_t sequence,
        cerberus_wifi_packet_monitor_session_sample_t *out);
    esp_err_t (*record_start)(uint64_t session, uint8_t channel,
        cerberus_wifi_packet_monitor_session_status_t *out);
    esp_err_t (*record_bind)(uint64_t session);
    bool (*record_drained)(uint64_t session);
    esp_err_t (*record_release)(uint64_t session, bool *released);
} wifi_packet_monitor_transport_t;

typedef struct {
    cerberus_wifi_packet_monitor_status_payload_t view;
    cerberus_wifi_packet_monitor_sample_payload_t samples[CERBERUS_WIFI_PACKET_MONITOR_SAMPLES];
    uint64_t session, last_tick_ms, next_action_ms, status_due_ms;
    uint16_t fetched;
    uint8_t channel;
    wifi_packet_monitor_phase_t phase;
    esp_err_t error;
    bool have_fetched, have_tick, recording, record_bound, source_missing;
} wifi_packet_monitor_scan_t;

bool wifi_packet_monitor_scan_start(wifi_packet_monitor_scan_t *scan, uint8_t channel);
bool wifi_packet_monitor_scan_record(wifi_packet_monitor_scan_t *scan, uint8_t channel);
void wifi_packet_monitor_scan_stop(wifi_packet_monitor_scan_t *scan);
/* Runs at most one bounded transport operation. True means the view changed. */
bool wifi_packet_monitor_scan_tick(wifi_packet_monitor_scan_t *scan, uint64_t now_ms,
    const wifi_packet_monitor_transport_t *transport);
