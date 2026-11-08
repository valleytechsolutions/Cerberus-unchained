#pragma once

#include "esp_err.h"
#include "wifi_fox_hunt.h"

#define WIFI_FOX_HISTORY 160U

typedef enum { WIFI_FOX_OFF, WIFI_FOX_INFO, WIFI_FOX_START, WIFI_FOX_WATCH,
    WIFI_FOX_STOP, WIFI_FOX_FINAL } wifi_fox_hunt_phase_t;
typedef enum { WIFI_FOX_WAITING, WIFI_FOX_TRACKING, WIFI_FOX_SEARCHING,
    WIFI_FOX_STALE, WIFI_FOX_STOPPED, WIFI_FOX_FAULT } wifi_fox_hunt_view_state_t;
typedef enum { WIFI_FOX_SIGNAL, WIFI_FOX_GAP } wifi_fox_hunt_entry_kind_t;

typedef struct {
    uint32_t sequence, observed_ms, age_ms;
    uint8_t channel, kind;
    int8_t rssi;
} wifi_fox_hunt_entry_t;

typedef struct {
    esp_err_t (*info)(cerberus_wifi_fox_hunt_status_t *out);
    esp_err_t (*start)(const cerberus_wifi_fox_hunt_start_t *request, cerberus_wifi_fox_hunt_status_t *out);
    esp_err_t (*status)(uint64_t session, cerberus_wifi_fox_hunt_status_t *out);
    esp_err_t (*stop)(uint64_t session, cerberus_wifi_fox_hunt_status_t *out);
    esp_err_t (*signal)(uint64_t session, cerberus_wifi_fox_hunt_signal_t *out);
} wifi_fox_hunt_transport_t;

typedef struct {
    wifi_fox_hunt_view_state_t state;
    uint32_t age_ms, sequence;
    uint16_t sweeps;
    int8_t rssi;
    uint8_t observed_channel, listening_channel;
    bool have_signal, current, channels_skipped, sweeps_saturated;
    esp_err_t warning;
} wifi_fox_hunt_view_t;

/* UI-owned optional PSRAM. The selected label/origin belongs to the UI and is
 * historical; this model starts with no current signal and allocates no task. */
typedef struct {
    cerberus_wifi_fox_hunt_status_t status;
    cerberus_wifi_fox_hunt_signal_t signal;
    uint64_t session, last_tick_ms, next_action_ms, status_due_ms, signal_due_ms;
    uint64_t signal_anchor_ms;
    uint32_t signal_age_ms;
    esp_err_t error, transport_error, local_error;
    uint16_t history_count, history_next;
    uint8_t target[6], initial_channel, search_band;
    wifi_fox_hunt_phase_t phase;
    bool have_tick, have_signal, link_valid, gap_open;
    wifi_fox_hunt_entry_t history[WIFI_FOX_HISTORY];
} wifi_fox_hunt_scan_t;

bool wifi_fox_hunt_scan_start(wifi_fox_hunt_scan_t *scan, const uint8_t target[6],
    uint8_t initial_channel, uint8_t search_band, bool keep_history);
void wifi_fox_hunt_scan_stop(wifi_fox_hunt_scan_t *scan);
/* At most one bounded transport operation per tick. OFF proves release. */
bool wifi_fox_hunt_scan_tick(wifi_fox_hunt_scan_t *scan, uint64_t now_ms,
    const wifi_fox_hunt_transport_t *transport);
void wifi_fox_hunt_scan_view(const wifi_fox_hunt_scan_t *scan, uint64_t now_ms, wifi_fox_hunt_view_t *out);
bool wifi_fox_hunt_scan_history(const wifi_fox_hunt_scan_t *scan, unsigned oldest_index,
    wifi_fox_hunt_entry_t *out);
