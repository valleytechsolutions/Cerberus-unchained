#pragma once

#include "wardrive_gps_core.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CERBERUS_GPX_ROW_CAPACITY 768U
#define CERBERUS_GPX_LABEL_BYTES 48U

typedef enum { CERBERUS_GPX_TRACK, CERBERUS_GPX_POINTS } cerberus_gpx_mode_t;

/* One original fresh position and RMC UTC; no clock holdover or fabricated
 * location, altitude or accuracy. Unknown optional fields stay absent. */
bool cerberus_gpx_point_valid(const cerberus_wardrive_gps_snapshot_t *point);
bool cerberus_gpx_utc_ms(const char utc[25], uint64_t *out);
bool cerberus_gpx_label_valid(const char *label);
const char *cerberus_gpx_header(cerberus_gpx_mode_t mode);
const char *cerberus_gpx_footer(cerberus_gpx_mode_t mode);
const char *cerberus_gpx_segment_break(void);
bool cerberus_gpx_point(char *out, size_t capacity, size_t *length,
    cerberus_gpx_mode_t mode, const cerberus_wardrive_gps_snapshot_t *point,
    uint32_t number, const char *label);

typedef struct {
    double distance_m, speed_mps, latitude, longitude;
    uint64_t last_utc_ms;
    uint32_t points, segments, filtered_steps;
    bool has_position;
} cerberus_gps_track_stats_t;

/* Distance/speed estimates from successive fixes. A three-meter step floor
 * suppresses small jitter; jumps above 120 m/s are excluded from the estimate,
 * not from GPX. Gaps start a new segment instead of inventing the missed path.
 * HDOP is dimensionless and is never labelled as an accuracy in meters. */
void cerberus_gps_track_gap(cerberus_gps_track_stats_t *stats);
bool cerberus_gps_track_observe(cerberus_gps_track_stats_t *stats,
    const cerberus_wardrive_gps_snapshot_t *point, bool *new_segment);
