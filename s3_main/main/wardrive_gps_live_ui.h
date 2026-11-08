/* Private Wardriving observer. Opening DATA never acquires the UART. NMEA
 * explicitly attaches to an already-running reader and copies its bounded
 * history into optional PSRAM; no raw bank is placed on the main task stack. */
#include "wardrive_gps_uart.h"
#include "wardrive_gps_log_ui.h"

typedef struct {
    cerberus_wardrive_gps_trace_snapshot_t *trace;
    uint64_t token;
    uint64_t copy_ms;
    uint32_t last_detach_ms;
    esp_err_t error;
    uint8_t page, pages;
    bool opened, nmea, logging, hidden, paint_valid, paused, copy_clock_valid, detach_pending;
} wardrive_gps_view_t;

static wardrive_gps_view_t wardrive_gps_view;

static bool wardrive_gps_view_is_open(void)
{
    return wardrive_gps_view.opened && current_page == UI_PAGE_WARDRIVE;
}

static bool wardrive_gps_view_detach(void)
{
    if (wardrive_gps_view.trace)
        memset(wardrive_gps_view.trace, 0, sizeof(*wardrive_gps_view.trace));
    if (wardrive_gps_view.token) {
        wardrive_gps_view.last_detach_ms = (uint32_t)(esp_timer_get_time() / 1000);
        esp_err_t error = cerberus_wardrive_gps_uart_trace_end(wardrive_gps_view.token);
        if (error != ESP_OK && error != ESP_ERR_NOT_FOUND) {
            wardrive_gps_view.error = error;
            wardrive_gps_view.detach_pending = true;
            return false; /* Keep only our exact cleanup token for a later retry. */
        }
    }
    wardrive_gps_view.token = 0U;
    wardrive_gps_view.detach_pending = false;
    heap_caps_free(wardrive_gps_view.trace);
    wardrive_gps_view.trace = NULL;
    return true;
}

static void wardrive_gps_view_close(void)
{
    wardrive_gps_view.opened = wardrive_gps_view.nmea = wardrive_gps_view.logging = false;
    wardrive_gps_view.hidden = true;
    wardrive_gps_view.paint_valid = false;
    wardrive_gps_view.paused = wardrive_gps_view.copy_clock_valid = false;
    wardrive_gps_view.copy_ms = 0U;
    wardrive_gps_view.page = wardrive_gps_view.pages = 0U;
    if (wardrive_gps_view_detach()) wardrive_gps_view.error = ESP_OK;
}

static void wardrive_gps_view_release_if_hidden(uint32_t now_ms, bool asleep)
{
    bool closing = asleep || current_page != UI_PAGE_WARDRIVE || !wardrive_gps_view.opened;
    if (closing) {
        wardrive_gps_view.opened = wardrive_gps_view.nmea = wardrive_gps_view.logging = wardrive_gps_view.paint_valid = false;
        wardrive_gps_view.hidden = true;
        wardrive_gps_view.paused = wardrive_gps_view.copy_clock_valid = false;
        wardrive_gps_view.page = wardrive_gps_view.pages = 0U;
        if (wardrive_gps_view.trace) memset(wardrive_gps_view.trace, 0, sizeof(*wardrive_gps_view.trace));
    }
    if (wardrive_gps_view.detach_pending &&
        (uint32_t)(now_ms - wardrive_gps_view.last_detach_ms) < 250U) return;
    if (closing && (wardrive_gps_view.token || wardrive_gps_view.trace)) {
        if (wardrive_gps_view_detach()) wardrive_gps_view.error = ESP_OK;
    }
    else if (wardrive_gps_view.detach_pending) (void)wardrive_gps_view_detach();
}

static void wardrive_gps_view_open(void)
{
    if (current_page != UI_PAGE_WARDRIVE) return;
    wardrive_gps_view_close();
    wardrive_recent_close();
    wardrive_gps_view.opened = true;
    wardrive_gps_view.hidden = true;
    wardrive_last_draw_ms = 0U;
    ESP_ERROR_CHECK(draw_detail_page());
}

static void wardrive_gps_view_attach(void)
{
    if (wardrive_gps_view.token) return;
    wardrive_gps_view.error = cerberus_wardrive_gps_uart_trace_begin(&wardrive_gps_view.token);
    if (wardrive_gps_view.error != ESP_OK) return;
    wardrive_gps_view.trace = heap_caps_calloc(1U, sizeof(*wardrive_gps_view.trace),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!wardrive_gps_view.trace) {
        wardrive_gps_view.error = ESP_ERR_NO_MEM;
        (void)wardrive_gps_view_detach();
    }
}

static void wardrive_gps_view_age(uint32_t age_ms, char out[20])
{
    if (age_ms == UINT32_MAX) snprintf(out, 20U, "UNKNOWN");
    else snprintf(out, 20U, "%lu.%lus", (unsigned long)(age_ms / 1000U),
        (unsigned long)((age_ms % 1000U) / 100U));
}

static const char *wardrive_gps_view_line_result(uint8_t result)
{
    switch (result) {
    case CERBERUS_WARDRIVE_GPS_LINE_OTHER: return "OTHER / CHECKSUM OK";
    case CERBERUS_WARDRIVE_GPS_LINE_RMC: return "RMC";
    case CERBERUS_WARDRIVE_GPS_LINE_GGA: return "GGA";
    case CERBERUS_WARDRIVE_GPS_LINE_FIELDS_INVALID: return "INVALID FIELDS";
    case CERBERUS_WARDRIVE_GPS_LINE_BAD_CHECKSUM: return "BAD CHECKSUM";
    case CERBERUS_WARDRIVE_GPS_LINE_FRAME_INVALID: return "INVALID FRAME";
    case CERBERUS_WARDRIVE_GPS_LINE_OVERFLOW: return "OVERFLOW / PREFIX";
    case CERBERUS_WARDRIVE_GPS_LINE_CONTROL: return "CONTROL BYTE";
    default: return "RESULT UNKNOWN";
    }
}

static esp_err_t wardrive_gps_view_data(const cerberus_wardrive_gps_snapshot_t *gps)
{
    char line[53], age[20];
    const char *fix = gps->state == CERBERUS_WARDRIVE_GPS_FRESH ? "FRESH" :
        gps->state == CERBERUS_WARDRIVE_GPS_STALE ? "STALE" :
        gps->state == CERBERUS_WARDRIVE_GPS_NO_FIX ? "NO FIX" : "NO RECENT NMEA";
    snprintf(line, sizeof(line), "FIX: %s", fix);
    ESP_RETURN_ON_ERROR(draw_centered(142, line, UI_TEXT, 1), TAG, "GPS fix");
    wardrive_gps_view_age(gps->sentence_age_ms, age);
    snprintf(line, sizeof(line), "CHECKSUM-VALID SENTENCE AGE: %s", age);
    ESP_RETURN_ON_ERROR(draw_centered(160, line, UI_MUTED, 1), TAG, "GPS sentence age");
    wardrive_gps_view_age(gps->fix_age_ms, age);
    snprintf(line, sizeof(line), "POSITION AGE: %s", age);
    ESP_RETURN_ON_ERROR(draw_centered(178, line, UI_MUTED, 1), TAG, "GPS fix age");
    wardrive_gps_view_age(gps->gga_age_ms, age);
    snprintf(line, sizeof(line), "GGA FIELD AGE: %s", age);
    ESP_RETURN_ON_ERROR(draw_centered(196, line, UI_MUTED, 1), TAG, "GPS GGA age");
    char satellites[12], hdop[16];
    if (gps->satellites_valid) snprintf(satellites, sizeof(satellites), "%u", gps->satellites);
    else snprintf(satellites, sizeof(satellites), "UNKNOWN");
    if (gps->hdop_valid && gps->hdop >= 0.0f && gps->hdop <= 1000.0f)
        snprintf(hdop, sizeof(hdop), "%.2f", (double)gps->hdop);
    else snprintf(hdop, sizeof(hdop), "UNKNOWN");
    snprintf(line, sizeof(line), "SATS %s / HDOP %s (NOT METERS)", satellites, hdop);
    ESP_RETURN_ON_ERROR(draw_centered(218, line, UI_TEXT, 1), TAG, "GPS satellite quality");
    if (wardrive_gps_view.hidden) snprintf(line, sizeof(line), "LATITUDE / LONGITUDE HIDDEN");
    else if (gps->coordinates_valid && gps->latitude >= -90.0 && gps->latitude <= 90.0)
        snprintf(line, sizeof(line), "LATITUDE: %.6f", gps->latitude);
    else snprintf(line, sizeof(line), "LATITUDE: UNAVAILABLE");
    ESP_RETURN_ON_ERROR(draw_centered(244, line, UI_TEXT, 1), TAG, "GPS latitude");
    if (wardrive_gps_view.hidden) snprintf(line, sizeof(line), "LOCATION DISPLAY ONLY / LOGGING UNCHANGED");
    else if (gps->coordinates_valid && gps->longitude >= -180.0 && gps->longitude <= 180.0)
        snprintf(line, sizeof(line), "LONGITUDE: %.6f", gps->longitude);
    else snprintf(line, sizeof(line), "LONGITUDE: UNAVAILABLE");
    ESP_RETURN_ON_ERROR(draw_centered(262, line, UI_MUTED, 1), TAG, "GPS longitude");
    if (wardrive_gps_view.hidden) snprintf(line, sizeof(line), "ALTITUDE HIDDEN");
    else if (gps->altitude_valid && gps->altitude_m >= -2000.0f && gps->altitude_m <= 100000.0f)
        snprintf(line, sizeof(line), "ALTITUDE: %.1f m", (double)gps->altitude_m);
    else snprintf(line, sizeof(line), "ALTITUDE: UNAVAILABLE");
    ESP_RETURN_ON_ERROR(draw_centered(280, line, UI_MUTED, 1), TAG, "GPS altitude");
    snprintf(line, sizeof(line), "UTC: %s", gps->utc_valid && gps->utc_from_rmc ? gps->utc : "UNAVAILABLE");
    ESP_RETURN_ON_ERROR(draw_centered(304, line, UI_TEXT, 1), TAG, "GPS UTC");
    wardrive_gps_view_age(gps->utc_age_ms, age);
    snprintf(line, sizeof(line), "RMC UTC AGE: %s", age);
    ESP_RETURN_ON_ERROR(draw_centered(322, line, UI_MUTED, 1), TAG, "GPS UTC age");
    snprintf(line, sizeof(line), "CHECKSUM OK %lu / BAD %lu", (unsigned long)gps->valid_sentences,
        (unsigned long)gps->bad_checksums);
    ESP_RETURN_ON_ERROR(draw_centered(344, line, UI_MUTED, 1), TAG, "GPS checksum counts");
    snprintf(line, sizeof(line), "MALFORMED %lu / OVERFLOW %lu", (unsigned long)gps->malformed_sentences,
        (unsigned long)gps->overflow_lines);
    ESP_RETURN_ON_ERROR(draw_centered(362, line, UI_MUTED, 1), TAG, "GPS parser counts");
    return draw_small_button(8, 382, 304, 36,
        wardrive_gps_view.hidden ? "SHOW LOCATION" : "HIDE LOCATION");
}

static esp_err_t wardrive_gps_view_nmea(bool clock_valid)
{
    char line[53];
    if (!wardrive_gps_view.paused && wardrive_gps_view.token && wardrive_gps_view.trace) {
        wardrive_gps_view.error = cerberus_wardrive_gps_uart_trace_read(
            wardrive_gps_view.token, wardrive_gps_view.hidden, wardrive_gps_view.trace);
        if (wardrive_gps_view.error != ESP_OK) {
            memset(wardrive_gps_view.trace, 0, sizeof(*wardrive_gps_view.trace));
            wardrive_gps_view.copy_clock_valid = false;
        } else {
            int64_t now_us = esp_timer_get_time();
            wardrive_gps_view.copy_clock_valid = clock_valid && now_us >= 0;
            wardrive_gps_view.copy_ms = now_us >= 0 ? (uint64_t)now_us / 1000U : 0U;
        }
    }
    const cerberus_wardrive_gps_trace_snapshot_t *trace = wardrive_gps_view.trace;
    bool valid = trace && wardrive_gps_view.error == ESP_OK &&
        trace->observer_token == wardrive_gps_view.token && trace->count <= CERBERUS_WARDRIVE_GPS_TRACE_CAPACITY;
    uint8_t count = valid ? trace->count : 0U;
    wardrive_gps_view.pages = count ? (uint8_t)((count + 1U) / 2U) : 1U;
    if (wardrive_gps_view.page >= wardrive_gps_view.pages) wardrive_gps_view.page = 0U;
    ESP_RETURN_ON_ERROR(draw_centered(136, "S3 UART RECEIPTS / NOT GNSS CAPTURE TIME", UI_MUTED, 1), TAG, "NMEA receipt scope");
    if (valid) snprintf(line, sizeof(line), "[ %s ] %u LINES / PAGE %u OF %u", wardrive_gps_view.paused ?
        "PAUSED" : "LIVE", count, wardrive_gps_view.page + 1U, wardrive_gps_view.pages);
    else snprintf(line, sizeof(line), "NMEA VIEW IDLE / TAP NMEA TO RETRY");
    ESP_RETURN_ON_ERROR(draw_centered(154, line, UI_TEXT, 1), TAG, "NMEA count");
    if (valid) {
        snprintf(line, sizeof(line), "EVICT %lu / DROP %lu / OVF %lu", (unsigned long)trace->evicted_lines,
            (unsigned long)trace->discarded_lines, (unsigned long)trace->overflow_lines);
        ESP_RETURN_ON_ERROR(draw_centered(172, line, UI_MUTED, 1), TAG, "NMEA loss counters");
        ESP_RETURN_ON_ERROR(draw_centered(188, trace->sequence_exhausted ? "SEQUENCE LIMIT / NO NEW TRACE ROWS" :
            wardrive_gps_view.paused ? "AGES AT COPY / TAP PAUSED TO RESUME" :
            trace->capturing ? "LAST 8 / TAP LIVE TO PAUSE" : "RETAINED / RECEIVER NOT CAPTURING",
            UI_MUTED, 1), TAG, "NMEA capture scope");
    }
    uint64_t now_ms = wardrive_gps_view.copy_ms;
    for (uint8_t slot = 0U; slot < 2U && wardrive_gps_view.page * 2U + slot < count; ++slot) {
        const cerberus_wardrive_gps_trace_row_t *row = &trace->rows[wardrive_gps_view.page * 2U + slot];
        uint16_t y = 208U + slot * 72U;
        ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(8, y, 304, 60, UI_PANEL), TAG, "NMEA row");
        char age[20];
        if ((row->flags & CERBERUS_WARDRIVE_GPS_TRACE_RECEIPT_VALID) &&
            wardrive_gps_view.copy_clock_valid && now_ms >= row->received_ms &&
            now_ms - row->received_ms < UINT32_MAX) {
            if (now_ms - row->received_ms > 99999000U) snprintf(age, sizeof(age), ">99999s");
            else wardrive_gps_view_age((uint32_t)(now_ms - row->received_ms), age);
        } else snprintf(age, sizeof(age), "UNKNOWN");
        if (wardrive_gps_view.hidden)
            snprintf(line, sizeof(line), "%s %luB RX %s", wardrive_gps_view_line_result(row->result),
                (unsigned long)row->total_length, age);
        else snprintf(line, sizeof(line), "%s %u/%luB RX %s", wardrive_gps_view_line_result(row->result),
            row->retained_length, (unsigned long)row->total_length, age);
        /* A bounded label never runs beyond the 50-column row. */
        line[50] = '\0';
        ESP_RETURN_ON_ERROR(cerberus_display_draw_text(10, y + 2U, line, UI_MUTED, UI_PANEL, 1), TAG, "NMEA line result");
        if (wardrive_gps_view.hidden || (row->flags & CERBERUS_WARDRIVE_GPS_TRACE_MASKED)) {
            ESP_RETURN_ON_ERROR(cerberus_display_draw_text(10, y + 22U,
                wardrive_gps_view.hidden ? "[LOCATION / RAW LINE HIDDEN]" :
                "[RAW HIDDEN IN COPY / RESUME LIVE]", UI_TEXT, UI_PANEL, 1), TAG, "NMEA hidden");
        } else {
            uint8_t length = row->retained_length <= CERBERUS_WARDRIVE_GPS_TRACE_PAYLOAD_BYTES ?
                row->retained_length : CERBERUS_WARDRIVE_GPS_TRACE_PAYLOAD_BYTES;
            for (uint8_t part = 0U; part < 3U; ++part) {
                uint8_t begin = part * 50U;
                uint8_t n = length > begin ? (uint8_t)(length - begin) : 0U;
                if (n > 50U) n = 50U;
                for (uint8_t i = 0U; i < n; ++i) {
                    uint8_t byte = row->payload[begin + i];
                    line[i] = byte >= 32U && byte <= 126U ? (char)byte : '.';
                }
                line[n] = '\0';
                ESP_RETURN_ON_ERROR(cerberus_display_draw_text(10, y + 16U + part * 13U,
                    line, UI_TEXT, UI_PANEL, 1), TAG, "NMEA payload");
            }
        }
        snprintf(line, sizeof(line), "%s / %s",
            row->termination == CERBERUS_WARDRIVE_GPS_TERMINATION_CRLF ? "CRLF" :
            row->termination == CERBERUS_WARDRIVE_GPS_TERMINATION_LF ? "LF" : "END UNKNOWN",
            row->flags & CERBERUS_WARDRIVE_GPS_TRACE_PAYLOAD_COMPLETE ? "ALL BYTES RETAINED" : "RETAINED PREFIX");
        ESP_RETURN_ON_ERROR(cerberus_display_draw_text(10, y + 62U, line, UI_MUTED, UI_BACKGROUND, 1), TAG, "NMEA framing");
    }
    if (!count) {
        ESP_RETURN_ON_ERROR(draw_centered(228, "NO RECENT NMEA", UI_TEXT, 1), TAG, "NMEA empty");
        ESP_RETURN_ON_ERROR(draw_centered(250, "VIEWING DOES NOT START GPS OR LOGGING", UI_MUTED, 1), TAG, "NMEA passive view");
        if (wardrive_gps_view.error != ESP_OK && wardrive_gps_view.error != ESP_ERR_NOT_FINISHED) {
            snprintf(line, sizeof(line), "VIEW: %.40s", esp_err_to_name(wardrive_gps_view.error));
            ESP_RETURN_ON_ERROR(draw_centered(276, line, UI_BAD, 1), TAG, "NMEA view error");
        }
        ESP_RETURN_ON_ERROR(draw_centered(306, "GPS LISTENS WHILE WARDRIVING IS OPEN", UI_MUTED, 1), TAG, "NMEA config route");
    }
    ESP_RETURN_ON_ERROR(draw_small_button(8, 382, 88, 36, "PREV"), TAG, "NMEA previous");
    ESP_RETURN_ON_ERROR(draw_small_button(104, 382, 112, 36,
        wardrive_gps_view.hidden ? "SHOW LOCATION" : "HIDE LOCATION"), TAG, "NMEA privacy");
    return draw_small_button(224, 382, 88, 36, "NEXT");
}

static esp_err_t wardrive_gps_view_draw(void)
{
    wardrive_gps_view.paint_valid = false;
    cerberus_wardrive_gps_uart_state_t state = {0};
    esp_err_t error = cerberus_wardrive_gps_uart_get_state(&state);
    cerberus_wardrive_recent_status_t status;
    cerberus_wardrive_get_recent_status(&status);
    ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(0, 0, 320, 480, UI_BACKGROUND), TAG, "GPS clear");
    ESP_RETURN_ON_ERROR(draw_header("GPS"), TAG, "GPS header");
    ESP_RETURN_ON_ERROR(draw_small_button(8, 54, 96, 32,
        wardrive_gps_view.nmea || wardrive_gps_view.logging ? "DATA" : "[ DATA ]"), TAG, "GPS data tab");
    ESP_RETURN_ON_ERROR(draw_small_button(112, 54, 96, 32,
        wardrive_gps_view.nmea ? "[ NMEA ]" : "NMEA"), TAG, "GPS NMEA tab");
    ESP_RETURN_ON_ERROR(draw_small_button(216, 54, 96, 32,
        wardrive_gps_view.logging ? "[ GPX ]" : "GPX"), TAG, "GPS log tab");
    const char *transport = error != ESP_OK ? "S3 GPS STATE UNAVAILABLE" :
        state.stopping ? "S3 GPS UART STOPPING" : state.starting ? "S3 GPS UART STARTING" :
        state.reader_running ? "S3 GPS UART LISTENING" : state.driver_active ?
        "S3 GPS UART HELD / READER STOPPED" : "S3 GPS UART OFF";
    ESP_RETURN_ON_ERROR(draw_centered(98, transport, UI_TEXT, 1), TAG, "GPS transport");
    char line[53];
    if (error != ESP_OK || state.error != ESP_OK)
        snprintf(line, sizeof(line), "UART: %.40s", esp_err_to_name(error != ESP_OK ? error : state.error));
    else if (state.historical_error != ESP_OK && !state.driver_active && !state.reader_running && !state.starting)
        snprintf(line, sizeof(line), "LAST UART ERROR: %.34s", esp_err_to_name(state.historical_error));
    else snprintf(line, sizeof(line), "RX GPIO%u / %lu / %s",
        state.config.rx_gpio, (unsigned long)state.active_baud,
        status.state == CERBERUS_WARDRIVE_STOPPING ? "WARDRIVE STOPPING" :
        state.detecting_baud ? "DETECTING" :
        state.driver_active || state.reader_running || state.starting ? "RECEIVER ACTIVE" : "GPS OFF");
    ESP_RETURN_ON_ERROR(draw_centered(116, line, error != ESP_OK || state.error != ESP_OK ? UI_BAD : UI_MUTED, 1), TAG, "GPS ownership");
    if (wardrive_gps_view.logging) ESP_RETURN_ON_ERROR(wardrive_gps_log_draw(), TAG, "GPX body");
    else if (wardrive_gps_view.nmea) ESP_RETURN_ON_ERROR(wardrive_gps_view_nmea(error == ESP_OK && state.clock_valid), TAG, "NMEA body");
    else if (error == ESP_OK) ESP_RETURN_ON_ERROR(wardrive_gps_view_data(&state.gps), TAG, "GPS data body");
    ESP_RETURN_ON_ERROR(draw_small_button(8, 430, 148, 40, "BACK TO SUMMARY"), TAG, "GPS back");
    bool active = wardrive_recent_active(status.state);
    ESP_RETURN_ON_ERROR(draw_small_button(164, 430, 148, 40,
        status.state == CERBERUS_WARDRIVE_STOPPING ? "STOPPING" :
        active || wardrive_start_pending ? "STOP WARDRIVE" : "INACTIVE"), TAG, "GPS stop");
    wardrive_gps_view.paint_valid = true;
    return ESP_OK;
}

static void wardrive_gps_view_handle_tap(uint16_t x, uint16_t y)
{
    if (y >= 430U && y < 470U) {
        if (x >= 8U && x < 156U) wardrive_gps_view_close();
        else if (x >= 164U && x < 312U) {
            cerberus_wardrive_recent_status_t status;
            cerberus_wardrive_get_recent_status(&status);
            wardrive_start_pending = false;
            if (wardrive_recent_active(status.state)) cerberus_wardrive_request_stop();
        } else return;
    } else if (!wardrive_gps_view.paint_valid) return;
    else if (y >= 54U && y < 86U) {
        if ((x >= 8U && x < 104U) || (x >= 216U && x < 312U)) {
            wardrive_gps_view.nmea = false;
            wardrive_gps_view.logging = x >= 216U;
            wardrive_gps_view.paused = false;
            (void)wardrive_gps_view_detach();
        } else if (x >= 112U && x < 208U) {
            wardrive_gps_view.nmea = true;
            wardrive_gps_view.logging = false;
            if (wardrive_gps_view.error == ESP_ERR_NOT_FOUND || wardrive_gps_view.detach_pending) {
                (void)wardrive_gps_view_detach();
                wardrive_gps_view.paused = false;
            }
            if (!wardrive_gps_view.token)
                wardrive_gps_view_attach();
        } else return;
    } else if (wardrive_gps_view.logging) {
        if (!wardrive_gps_log_tap(x, y)) return;
    } else if (wardrive_gps_view.nmea && y >= 148U && y < 166U &&
        x >= 8U && x < 312U && wardrive_gps_view.trace && wardrive_gps_view.error == ESP_OK) {
        wardrive_gps_view.paused = !wardrive_gps_view.paused;
    } else if (y >= 382U && y < 418U) {
        if ((!wardrive_gps_view.nmea && x >= 8U && x < 312U) ||
            (wardrive_gps_view.nmea && x >= 104U && x < 216U)) {
            wardrive_gps_view.hidden = !wardrive_gps_view.hidden;
            if (wardrive_gps_view.hidden && wardrive_gps_view.trace)
                for (uint8_t i = 0U; i < CERBERUS_WARDRIVE_GPS_TRACE_CAPACITY; ++i) {
                    cerberus_wardrive_gps_trace_row_t *row = &wardrive_gps_view.trace->rows[i];
                    memset(row->payload, 0, sizeof(row->payload));
                    row->retained_length = 0U;
                    row->flags |= CERBERUS_WARDRIVE_GPS_TRACE_MASKED;
                }
        } else if (wardrive_gps_view.nmea && x >= 8U && x < 96U && wardrive_gps_view.page) {
            wardrive_gps_view.paused = true;
            --wardrive_gps_view.page;
        } else if (wardrive_gps_view.nmea && x >= 224U && x < 312U &&
            wardrive_gps_view.page + 1U < wardrive_gps_view.pages) {
            wardrive_gps_view.paused = true;
            ++wardrive_gps_view.page;
        } else return;
    } else return;
    wardrive_gps_view.paint_valid = false;
    wardrive_last_draw_ms = 0U;
    ESP_ERROR_CHECK(draw_detail_page());
}
