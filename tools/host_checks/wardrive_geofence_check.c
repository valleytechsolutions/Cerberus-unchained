#include "wardrive_geofence.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void expected(cerberus_geofence_config_t *config, cerberus_wardrive_gps_snapshot_t *gps,
    cerberus_geofence_state_t state, unsigned slot, bool pause)
{
    cerberus_geofence_result_t result = cerberus_geofence_evaluate(config, gps);
    assert(result.state == state && result.slot == slot && result.pause == pause);
}

static void invalid_configs(void)
{
    cerberus_geofence_config_t base = cerberus_geofence_defaults(), value;
    assert(sizeof(base) == 328U && cerberus_geofence_config_valid(&base));
    assert(!cerberus_geofence_config_valid(NULL));
    value = base; value.version = 0; assert(!cerberus_geofence_config_valid(&value));
    value = base; value.reserved = 1; assert(!cerberus_geofence_config_valid(&value));
    const double bad[] = {NAN, INFINITY, -INFINITY, -0.1, 0.0, 0.099999, 1.000001};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        value = base; value.areas[2].radius_miles = bad[i];
        expected(&value, NULL, CERBERUS_GEOFENCE_INVALID, CERBERUS_GEOFENCE_SLOTS, true);
    }
    for (unsigned i = 0; i < CERBERUS_GEOFENCE_SLOTS; ++i) {
        value = base; value.areas[i].latitude = NAN; assert(!cerberus_geofence_config_valid(&value));
        value = base; value.areas[i].longitude = INFINITY; assert(!cerberus_geofence_config_valid(&value));
        value = base; value.areas[i].latitude = -90.001; assert(!cerberus_geofence_config_valid(&value));
        value = base; value.areas[i].longitude = 180.001; assert(!cerberus_geofence_config_valid(&value));
        value = base; value.areas[i].reserved[5] = 1; assert(!cerberus_geofence_config_valid(&value));
        value = base; value.areas[i].enabled = 2; assert(!cerberus_geofence_config_valid(&value));
        value = base; value.areas[i].enabled = 1; assert(!cerberus_geofence_config_valid(&value));
    }
    const char *bad_names[] = {"line\nbreak", "\x7f", "\xc0\xaf", "\xed\xa0\x80",
        "\xf4\x90\x80\x80", "\xf0\x9f", "\x80", "\xef\xbf\xbf"};
    for (unsigned i = 0; i < sizeof(bad_names) / sizeof(bad_names[0]); ++i) {
        value = base; strcpy(value.areas[0].name, bad_names[i]);
        assert(!cerberus_geofence_config_valid(&value));
    }
    value = base; memset(value.areas[0].name, 'x', sizeof(value.areas[0].name));
    assert(!cerberus_geofence_config_valid(&value));
    value.areas[0].name[CERBERUS_GEOFENCE_NAME_BYTES] = 0;
    value.areas[0].enabled = 1; assert(cerberus_geofence_config_valid(&value));
    strcpy(value.areas[0].name, "caf\xc3\xa9 \xf0\x9f\x93\x8d");
    assert(cerberus_geofence_config_valid(&value));
}

static void positions(void)
{
    const double pi = acos(-1.0), radius = 3958.7613;
    assert(cerberus_geofence_distance_miles(0, 180, 0, -180) == 0.0);
    assert(fabs(cerberus_geofence_distance_miles(0, 0, 90, 0) - radius * pi / 2.0) < 1e-8);
    assert(fabs(cerberus_geofence_distance_miles(90, 0, -90, 180) - radius * pi) < 1e-7);
    assert(cerberus_geofence_distance_miles(90, -180, 90, 180) == 0.0);
    assert(isnan(cerberus_geofence_distance_miles(0, 0, NAN, 0)));
    /* Independent great-circle reference: atan2(cross product, dot product). */
    for (int lat = -89; lat <= 89; lat += 7) for (int lon = -179; lon <= 179; lon += 13) {
        double a = lat * pi / 180.0, b = lon * pi / 180.0;
        double c = (-lat + 0.123) * pi / 180.0, d = (lon / 2.0) * pi / 180.0;
        double x[3] = {cos(a)*cos(b), cos(a)*sin(b), sin(a)};
        double y[3] = {cos(c)*cos(d), cos(c)*sin(d), sin(c)};
        double cross[3] = {x[1]*y[2]-x[2]*y[1], x[2]*y[0]-x[0]*y[2], x[0]*y[1]-x[1]*y[0]};
        double dot = x[0]*y[0]+x[1]*y[1]+x[2]*y[2];
        double reference = radius * atan2(sqrt(cross[0]*cross[0]+cross[1]*cross[1]+cross[2]*cross[2]), dot);
        assert(fabs(cerberus_geofence_distance_miles(lat, lon, -lat+0.123, lon/2.0) - reference) < 1e-7);
    }
    cerberus_geofence_config_t config = cerberus_geofence_defaults();
    cerberus_wardrive_gps_snapshot_t gps = {.state = CERBERUS_WARDRIVE_GPS_FRESH, .coordinates_valid = true};
    expected(&config, NULL, CERBERUS_GEOFENCE_DISABLED, CERBERUS_GEOFENCE_SLOTS, false);
    strcpy(config.areas[3].name, "area 4"); config.areas[3].enabled = 1;
    config.areas[3].radius_miles = 0.5;
    expected(&config, &gps, CERBERUS_GEOFENCE_INSIDE, 3, true);
    gps.latitude = 0.01; expected(&config, &gps, CERBERUS_GEOFENCE_CLEAR, CERBERUS_GEOFENCE_SLOTS, false);
    config.areas[3].radius_miles = cerberus_geofence_distance_miles(0, 0, gps.latitude, 0);
    expected(&config, &gps, CERBERUS_GEOFENCE_INSIDE, 3, true); /* Boundary is inclusive. */
    config.areas[1] = config.areas[3];
    expected(&config, &gps, CERBERUS_GEOFENCE_INSIDE, 1, true); /* Stable slot order for overlap. */
    gps.state = CERBERUS_WARDRIVE_GPS_STALE;
    expected(&config, &gps, CERBERUS_GEOFENCE_WAIT_GPS, CERBERUS_GEOFENCE_SLOTS, true);
    gps.state = CERBERUS_WARDRIVE_GPS_FRESH; gps.coordinates_valid = false;
    expected(&config, &gps, CERBERUS_GEOFENCE_WAIT_GPS, CERBERUS_GEOFENCE_SLOTS, true);
    gps.coordinates_valid = true; gps.fix_age_ms = CERBERUS_WARDRIVE_GPS_FIX_MAX_AGE_MS;
    expected(&config, &gps, CERBERUS_GEOFENCE_INSIDE, 1, true);
    ++gps.fix_age_ms; expected(&config, &gps, CERBERUS_GEOFENCE_WAIT_GPS, CERBERUS_GEOFENCE_SLOTS, true);
    gps.fix_age_ms = 0; gps.latitude = NAN;
    expected(&config, &gps, CERBERUS_GEOFENCE_WAIT_GPS, CERBERUS_GEOFENCE_SLOTS, true);
    gps.latitude = 0; gps.longitude = -179.9999;
    config.areas[1].longitude = 179.9999;
    expected(&config, &gps, CERBERUS_GEOFENCE_INSIDE, 1, true);
    config.areas[1].enabled = config.areas[3].enabled = 0;
    expected(&config, NULL, CERBERUS_GEOFENCE_DISABLED, CERBERUS_GEOFENCE_SLOTS, false);
}

int main(void)
{
    invalid_configs(); positions();
    puts("Geofence core: five areas, inclusive/overlapping boundaries, antimeridian/poles, independent distance reference, stale fixes and invalid configuration passed.");
    return 0;
}
