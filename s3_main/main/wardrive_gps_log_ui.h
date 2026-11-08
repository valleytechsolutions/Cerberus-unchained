/* GPX is an explicit local recording in the existing GPS view. */
#include "gps_recorder.h"
static cerberus_gpx_mode_t wardrive_gps_log_mode;
static esp_err_t wardrive_gps_log_error;

static esp_err_t wardrive_gps_log_draw(void)
{
    cerberus_gps_recorder_status_t s;
    cerberus_gps_recorder_status(&s);
    cerberus_gpx_mode_t mode = s.busy ? s.mode : wardrive_gps_log_mode;
    const char *state = s.state == CERBERUS_GPS_LOG_WAITING ? "WAITING FOR GPS FIX + UTC" :
        s.state == CERBERUS_GPS_LOG_RECORDING ? "RECORDING" :
        s.state == CERBERUS_GPS_LOG_STOPPING ? "SAVING / STOPPING" :
        s.state == CERBERUS_GPS_LOG_STOPPED ? "STOPPED" :
        s.state == CERBERUS_GPS_LOG_ERROR ? "ERROR" : "READY";
    ESP_RETURN_ON_ERROR(draw_small_button(8, 138, 304, 32,
        mode == CERBERUS_GPX_TRACK ? "MODE: TRACK / 1 POINT PER SECOND" : "MODE: POINTS OF INTEREST"), TAG, "GPX mode");
    ESP_RETURN_ON_ERROR(draw_centered(184, state, s.error == ESP_OK ? UI_TEXT : UI_BAD, 1), TAG, "GPX state");
    char line[53];
    snprintf(line, sizeof(line), "%lu POINTS", (unsigned long)s.written);
    ESP_RETURN_ON_ERROR(draw_centered(208, line, UI_GOOD, 2), TAG, "GPX points");
    int64_t now_us = esp_timer_get_time();
    uint64_t end = s.busy && now_us >= 0 ? (uint64_t)now_us / 1000U : s.ended_ms;
    uint64_t seconds = s.job && end >= s.started_ms ? (end - s.started_ms) / 1000U : 0U;
    snprintf(line, sizeof(line), "SESSION %llu:%02u / PENDING %lu", (unsigned long long)(seconds / 60U),
        (unsigned)(seconds % 60U), (unsigned long)s.queued);
    ESP_RETURN_ON_ERROR(draw_centered(236, line, UI_MUTED, 1), TAG, "GPX time");
    if (mode == CERBERUS_GPX_TRACK) {
        snprintf(line, sizeof(line), "EST. DISTANCE %.2f km", s.track.distance_m / 1000.0);
        ESP_RETURN_ON_ERROR(draw_centered(264, line, UI_TEXT, 1), TAG, "GPX distance");
        snprintf(line, sizeof(line), "EST. SPEED %.1f km/h / SEGMENTS %lu",
            s.fix_ready && s.busy ? s.track.speed_mps * 3.6 : 0.0, (unsigned long)s.track.segments);
        ESP_RETURN_ON_ERROR(draw_centered(286, line, UI_MUTED, 1), TAG, "GPX speed");
    } else {
        ESP_RETURN_ON_ERROR(draw_small_button(8, 258, 304, 44,
            s.busy && s.fix_ready && s.state != CERBERUS_GPS_LOG_STOPPING ? "MARK POI" : "MARK REQUIRES GPS + START"), TAG, "GPX mark");
    }
    snprintf(line, sizeof(line), "QUEUE FULL %lu / NOT SAVED %lu", (unsigned long)s.rejected, (unsigned long)s.discarded);
    ESP_RETURN_ON_ERROR(draw_centered(316, line, UI_MUTED, 1), TAG, "GPX losses");
    ESP_RETURN_ON_ERROR(draw_centered(338, s.file_name[0] ? s.file_name : s.written ? "STOP TO FINISH GPX FILE" :
        "NO FILE UNTIL FIRST VALID POINT", UI_TEXT, 1), TAG, "GPX file");
    esp_err_t error = wardrive_gps_log_error != ESP_OK ? wardrive_gps_log_error : s.error;
    if (error != ESP_OK) snprintf(line, sizeof(line), "GPX: %.40s", esp_err_to_name(error));
    else snprintf(line, sizeof(line), "SD /CERBERUS/GPS / LOCAL GPX FILES");
    ESP_RETURN_ON_ERROR(draw_centered(360, line, error == ESP_OK ? UI_MUTED : UI_BAD, 1), TAG, "GPX status");
    return draw_small_button(8, 382, 304, 36, s.state == CERBERUS_GPS_LOG_STOPPING ? "SAVING" :
        s.busy ? "STOP GPX" : "START GPX");
}

static bool wardrive_gps_log_tap(uint16_t x, uint16_t y)
{
    if (x < 8U || x >= 312U) return false;
    cerberus_gps_recorder_status_t s;
    cerberus_gps_recorder_status(&s);
    if (y >= 138U && y < 170U && !s.busy) {
        wardrive_gps_log_mode = wardrive_gps_log_mode == CERBERUS_GPX_TRACK ? CERBERUS_GPX_POINTS : CERBERUS_GPX_TRACK;
        wardrive_gps_log_error = ESP_OK;
    } else if (y >= 382U && y < 418U) {
        uint64_t job;
        wardrive_gps_log_error = s.busy ? cerberus_gps_recorder_stop(s.job) :
            cerberus_gps_recorder_start(wardrive_gps_log_mode, &job);
    } else if (y >= 258U && y < 302U && s.mode == CERBERUS_GPX_POINTS && s.busy) {
        wardrive_gps_log_error = cerberus_gps_recorder_mark(s.job, NULL);
    } else return false;
    return true;
}
