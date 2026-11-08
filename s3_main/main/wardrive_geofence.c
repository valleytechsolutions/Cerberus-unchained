#include "wardrive_geofence.h"
#include <math.h>
#include <stddef.h>
#include <string.h>

static bool coordinates_valid(double latitude, double longitude)
{
    return isfinite(latitude) && isfinite(longitude) &&
        latitude >= -90.0 && latitude <= 90.0 && longitude >= -180.0 && longitude <= 180.0;
}

static bool name_valid(const char name[CERBERUS_GEOFENCE_NAME_BYTES + 1U])
{
    const unsigned char *p = (const unsigned char *)name;
    const char *end = memchr(name, 0, CERBERUS_GEOFENCE_NAME_BYTES + 1U);
    if (!end) return false;
    size_t length = (size_t)(end - name), i = 0;
    while (i < length) {
        uint32_t value = p[i++], minimum = 0;
        unsigned count = 0;
        if (value >= 0xc2U && value <= 0xdfU) { value &= 0x1fU; minimum = 0x80U; count = 1; }
        else if (value >= 0xe0U && value <= 0xefU) { value &= 0x0fU; minimum = 0x800U; count = 2; }
        else if (value >= 0xf0U && value <= 0xf4U) { value &= 7U; minimum = 0x10000U; count = 3; }
        else if (value >= 0x80U) return false;
        if (count > length - i) return false;
        while (count--) {
            if ((p[i] & 0xc0U) != 0x80U) return false;
            value = (value << 6U) | (p[i++] & 0x3fU);
        }
        if (value < 0x20U || value == 0x7fU || value < minimum || value > 0x10ffffU ||
            (value >= 0xd800U && value <= 0xdfffU) || value == 0xfffeU || value == 0xffffU)
            return false;
    }
    return true;
}

cerberus_geofence_config_t cerberus_geofence_defaults(void)
{
    cerberus_geofence_config_t config = {.version = CERBERUS_GEOFENCE_VERSION};
    for (unsigned i = 0; i < CERBERUS_GEOFENCE_SLOTS; ++i)
        config.areas[i].radius_miles = CERBERUS_GEOFENCE_MIN_MILES;
    return config;
}

bool cerberus_geofence_config_valid(const cerberus_geofence_config_t *config)
{
    if (!config || config->version != CERBERUS_GEOFENCE_VERSION || config->reserved) return false;
    for (unsigned i = 0; i < CERBERUS_GEOFENCE_SLOTS; ++i) {
        const cerberus_geofence_area_t *area = &config->areas[i];
        if (area->enabled > 1U || !coordinates_valid(area->latitude, area->longitude) ||
            !isfinite(area->radius_miles) || area->radius_miles < CERBERUS_GEOFENCE_MIN_MILES ||
            area->radius_miles > CERBERUS_GEOFENCE_MAX_MILES || !name_valid(area->name) ||
            (area->enabled && !area->name[0])) return false;
        for (unsigned j = 0; j < sizeof(area->reserved); ++j)
            if (area->reserved[j]) return false;
    }
    return true;
}

double cerberus_geofence_distance_miles(double lat1, double lon1, double lat2, double lon2)
{
    if (!coordinates_valid(lat1, lon1) || !coordinates_valid(lat2, lon2)) return NAN;
    const double radians = 0.017453292519943295;
    const double latitude = sin((lat2 - lat1) * radians * 0.5);
    const double longitude = sin(remainder(lon2 - lon1, 360.0) * radians * 0.5);
    double a = latitude * latitude + cos(lat1 * radians) * cos(lat2 * radians) * longitude * longitude;
    /* Rounding near antipodes can otherwise put a just outside [0, 1]. */
    a = fmax(0.0, fmin(1.0, a));
    return 3958.7613 * 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
}

cerberus_geofence_result_t cerberus_geofence_evaluate(
    const cerberus_geofence_config_t *config, const cerberus_wardrive_gps_snapshot_t *gps)
{
    cerberus_geofence_result_t result = {
        .state = CERBERUS_GEOFENCE_INVALID, .slot = CERBERUS_GEOFENCE_SLOTS, .pause = true};
    if (!cerberus_geofence_config_valid(config)) return result;
    unsigned enabled = 0;
    for (unsigned i = 0; i < CERBERUS_GEOFENCE_SLOTS; ++i) enabled += config->areas[i].enabled;
    if (!enabled) { result.state = CERBERUS_GEOFENCE_DISABLED; result.pause = false; return result; }
    result.state = CERBERUS_GEOFENCE_WAIT_GPS;
    if (!gps || gps->state != CERBERUS_WARDRIVE_GPS_FRESH || !gps->coordinates_valid ||
        gps->fix_age_ms > CERBERUS_WARDRIVE_GPS_FIX_MAX_AGE_MS ||
        !coordinates_valid(gps->latitude, gps->longitude)) return result;
    for (unsigned i = 0; i < CERBERUS_GEOFENCE_SLOTS; ++i) {
        const cerberus_geofence_area_t *area = &config->areas[i];
        if (area->enabled && cerberus_geofence_distance_miles(gps->latitude, gps->longitude,
                area->latitude, area->longitude) <= area->radius_miles) {
            result.state = CERBERUS_GEOFENCE_INSIDE; result.slot = (uint8_t)i; return result;
        }
    }
    result.state = CERBERUS_GEOFENCE_CLEAR; result.pause = false; return result;
}
