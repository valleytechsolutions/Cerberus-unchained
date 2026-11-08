#pragma once

#include "esp_err.h"
#include "wifi_channel_analyzer.h"

#define WIFI_ANALYZER_HISTORY 320U

typedef enum {
    WIFI_ANALYZER_OFF, WIFI_ANALYZER_INFO, WIFI_ANALYZER_START,
    WIFI_ANALYZER_WATCH, WIFI_ANALYZER_STOP, WIFI_ANALYZER_DRAIN,
} wifi_channel_analyzer_phase_t;

typedef enum { WIFI_ANALYZER_WINDOW, WIFI_ANALYZER_GAP, WIFI_ANALYZER_CHANNEL }
    wifi_channel_analyzer_entry_kind_t;

typedef struct {
    uint32_t sequence, frames, window_us, missing_windows;
    uint8_t channel, flags, kind;
} wifi_channel_analyzer_entry_t;

typedef struct {
    esp_err_t (*info)(cerberus_wifi_channel_analyzer_status_t *out);
    esp_err_t (*start)(uint64_t session, uint8_t channel, cerberus_wifi_channel_analyzer_status_t *out);
    esp_err_t (*status)(uint64_t session, cerberus_wifi_channel_analyzer_status_t *out);
    esp_err_t (*stop)(uint64_t session, cerberus_wifi_channel_analyzer_status_t *out);
    esp_err_t (*sample)(uint64_t session, uint32_t sequence, cerberus_wifi_channel_analyzer_sample_t *out);
    esp_err_t (*hint_peek)(uint64_t session, cerberus_wifi_channel_analyzer_hint_status_t *out);
    esp_err_t (*hint_read)(uint64_t session, uint32_t projection, uint8_t part,
        cerberus_wifi_channel_analyzer_hint_part_t *out);
} wifi_channel_analyzer_transport_t;

typedef struct {
    uint64_t frames, window_us;
    uint32_t current_rate, peak_rate, average_rate, missing_windows;
    uint16_t windows, gap_count;
    bool current_valid, lower_bound, rate_limited;
} wifi_channel_analyzer_summary_t;

/* UI-owned optional PSRAM. No internal allocation or background task. */
typedef struct {
    cerberus_wifi_channel_analyzer_status_t status;
    cerberus_wifi_channel_analyzer_hint_t hint;
    cerberus_wifi_channel_analyzer_hint_status_t hint_pending;
    uint64_t session, last_tick_ms, next_action_ms, status_due_ms, hint_due_ms;
    uint64_t hint_peek_ms, hint_pending_ms;
    uint32_t fetched, hint_age_at_peek_ms, lost_windows;
    esp_err_t error, transport_error, local_error;
    uint16_t history_count, history_next;
    uint8_t channel, hint_part;
    wifi_channel_analyzer_phase_t phase;
    bool have_tick, new_segment, hint_valid, reading_hint, current_valid;
    uint8_t hint_bytes[CERBERUS_WIFI_CHANNEL_ANALYZER_HINT_BYTES];
    wifi_channel_analyzer_entry_t history[WIFI_ANALYZER_HISTORY];
} wifi_channel_analyzer_scan_t;

/* Only OFF can start. New entry clears history; retune/restart can retain it.
 * A retained segment marker is inserted with its first valid completed window. */
bool wifi_channel_analyzer_scan_start(wifi_channel_analyzer_scan_t *s, uint8_t channel, bool keep_history);
void wifi_channel_analyzer_scan_stop(wifi_channel_analyzer_scan_t *s);
/* At most one bounded transport operation per tick. False means no view change. */
bool wifi_channel_analyzer_scan_tick(wifi_channel_analyzer_scan_t *s, uint64_t now_ms,
    const wifi_channel_analyzer_transport_t *transport);
bool wifi_channel_analyzer_scan_history(const wifi_channel_analyzer_scan_t *s, unsigned index,
    wifi_channel_analyzer_entry_t *out);
void wifi_channel_analyzer_scan_summary(const wifi_channel_analyzer_scan_t *s,
    wifi_channel_analyzer_summary_t *out);
/* Age advances from PEEK, including chunk transfer time. UINT32_MAX is unknown. */
uint32_t wifi_channel_analyzer_scan_hint_age(const wifi_channel_analyzer_scan_t *s, uint64_t now_ms);
size_t wifi_channel_analyzer_scan_ssid_display(const cerberus_wifi_channel_analyzer_hint_t *hint,
    char *out, size_t capacity);
