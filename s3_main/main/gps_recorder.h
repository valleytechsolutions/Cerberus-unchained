#pragma once

#include "esp_err.h"
#include "gps_gpx.h"

typedef enum {
    CERBERUS_GPS_LOG_IDLE, CERBERUS_GPS_LOG_WAITING, CERBERUS_GPS_LOG_RECORDING,
    CERBERUS_GPS_LOG_STOPPING, CERBERUS_GPS_LOG_STOPPED, CERBERUS_GPS_LOG_ERROR
} cerberus_gps_log_state_t;

typedef struct {
    uint64_t job, started_ms, ended_ms, bytes;
    cerberus_gps_track_stats_t track;
    uint32_t written, queued, rejected, discarded;
    cerberus_gps_log_state_t state;
    cerberus_gpx_mode_t mode;
    esp_err_t error;
    bool busy, fix_ready;
    char file_name[16];
} cerberus_gps_recorder_status_t;

/* One optional worker and bounded PSRAM queue. APIs do not perform file I/O.
 * Start waits for a fix; MARK freezes its own valid point before queueing.
 * Stop retains the job through queue drain, SD close and GPS-consumer release. */
esp_err_t cerberus_gps_recorder_start(cerberus_gpx_mode_t mode, uint64_t *job);
esp_err_t cerberus_gps_recorder_mark(uint64_t job, const char *label);
esp_err_t cerberus_gps_recorder_stop(uint64_t job);
void cerberus_gps_recorder_status(cerberus_gps_recorder_status_t *out);
bool cerberus_gps_recorder_busy(void);
