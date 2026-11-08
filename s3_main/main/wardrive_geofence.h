#pragma once

#include "wardrive_gps_core.h"
#include <stdbool.h>
#include <stdint.h>

#define CERBERUS_GEOFENCE_SLOTS 5U
#define CERBERUS_GEOFENCE_NAME_BYTES 32U
#define CERBERUS_GEOFENCE_VERSION 1U
#define CERBERUS_GEOFENCE_MIN_MILES 0.1
#define CERBERUS_GEOFENCE_MAX_MILES 1.0

/* Fixed layout for the eventual versioned settings record. Coordinates are
 * local configuration; they must not be printed by the diagnostic console. */
typedef struct {
    double latitude, longitude, radius_miles;
    char name[CERBERUS_GEOFENCE_NAME_BYTES + 1U];
    uint8_t enabled, reserved[6];
} cerberus_geofence_area_t;

typedef struct {
    uint32_t version, reserved;
    cerberus_geofence_area_t areas[CERBERUS_GEOFENCE_SLOTS];
} cerberus_geofence_config_t;

_Static_assert(sizeof(cerberus_geofence_area_t) == 64U, "Geofence area layout");
_Static_assert(sizeof(cerberus_geofence_config_t) == 328U, "Geofence settings layout");

typedef enum {
    CERBERUS_GEOFENCE_DISABLED = 0,
    CERBERUS_GEOFENCE_CLEAR,
    CERBERUS_GEOFENCE_INSIDE,
    CERBERUS_GEOFENCE_WAIT_GPS,
    CERBERUS_GEOFENCE_INVALID,
} cerberus_geofence_state_t;

typedef struct {
    cerberus_geofence_state_t state;
    uint8_t slot; /* CERBERUS_GEOFENCE_SLOTS unless an area matched. */
    bool pause;
} cerberus_geofence_result_t;

cerberus_geofence_config_t cerberus_geofence_defaults(void);
bool cerberus_geofence_config_valid(const cerberus_geofence_config_t *config);
double cerberus_geofence_distance_miles(double lat1, double lon1, double lat2, double lon2);
/* Stateless, allocation-free evaluation. No radio, storage or settings calls.
 * Enabled areas require a fresh position; losing the fix does not mean CLEAR.
 * The recording layer independently qualifies the CSV's UTC and coordinates. */
cerberus_geofence_result_t cerberus_geofence_evaluate(
    const cerberus_geofence_config_t *config, const cerberus_wardrive_gps_snapshot_t *gps);
