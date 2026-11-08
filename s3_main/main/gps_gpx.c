#include "gps_gpx.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static bool leap(unsigned year)
{ return year % 4U == 0U && (year % 100U != 0U || year % 400U == 0U); }

static unsigned decimal(const char *text, unsigned count)
{
    unsigned value = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (text[i] < '0' || text[i] > '9') return UINT32_MAX;
        value = value * 10U + (unsigned)(text[i] - '0');
    }
    return value;
}

bool cerberus_gpx_utc_ms(const char utc[25], uint64_t *out)
{
    if (out) *out = 0;
    if (!utc || !out || utc[24] || utc[4] != '-' || utc[7] != '-' ||
        utc[10] != 'T' || utc[13] != ':' || utc[16] != ':' ||
        utc[19] != '.' || utc[23] != 'Z') return false;
    unsigned y = decimal(utc, 4), m = decimal(utc + 5, 2), d = decimal(utc + 8, 2);
    unsigned h = decimal(utc + 11, 2), min = decimal(utc + 14, 2);
    unsigned sec = decimal(utc + 17, 2), ms = decimal(utc + 20, 3);
    static const unsigned days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (y < 2000U || y > 2099U || m < 1U || m > 12U || d < 1U ||
        d > days[m - 1U] + (m == 2U && leap(y)) || h > 23U ||
        min > 59U || sec > 59U || ms > 999U) return false;
    uint64_t date = 0;
    for (unsigned year = 1970U; year < y; ++year) date += leap(year) ? 366U : 365U;
    for (unsigned month = 1U; month < m; ++month)
        date += days[month - 1U] + (month == 2U && leap(y));
    *out = ((((date + d - 1U) * 24U + h) * 60U + min) * 60U + sec) * 1000U + ms;
    return true;
}

bool cerberus_gpx_point_valid(const cerberus_wardrive_gps_snapshot_t *p)
{
    uint64_t utc;
    return p && p->state == CERBERUS_WARDRIVE_GPS_FRESH && p->coordinates_valid &&
        p->utc_valid && p->utc_from_rmc &&
        p->fix_age_ms <= CERBERUS_WARDRIVE_GPS_FIX_MAX_AGE_MS &&
        p->utc_age_ms <= CERBERUS_WARDRIVE_GPS_FIX_MAX_AGE_MS &&
        isfinite(p->latitude) && p->latitude >= -90.0 && p->latitude <= 90.0 &&
        isfinite(p->longitude) && p->longitude >= -180.0 && p->longitude <= 180.0 &&
        (!p->altitude_valid || (isfinite(p->altitude_m) && p->altitude_m >= -2000.0f && p->altitude_m <= 100000.0f)) &&
        (!p->hdop_valid || (isfinite(p->hdop) && p->hdop >= 0.0f && p->hdop <= 1000.0f)) &&
        cerberus_gpx_utc_ms(p->utc, &utc);
}

bool cerberus_gpx_label_valid(const char *label)
{
    if (!label) return true;
    const unsigned char *p = (const unsigned char *)label;
    size_t i = 0;
    while (i <= CERBERUS_GPX_LABEL_BYTES && p[i]) {
        uint32_t code = p[i++], minimum = 0;
        unsigned trailing = 0;
        if (code >= 0xc2U && code <= 0xdfU) { code &= 0x1fU; trailing = 1; minimum = 0x80U; }
        else if (code >= 0xe0U && code <= 0xefU) { code &= 0x0fU; trailing = 2; minimum = 0x800U; }
        else if (code >= 0xf0U && code <= 0xf4U) { code &= 7U; trailing = 3; minimum = 0x10000U; }
        else if (code >= 0x80U) return false;
        while (trailing--) {
            if (i > CERBERUS_GPX_LABEL_BYTES || (p[i] & 0xc0U) != 0x80U) return false;
            code = (code << 6) | (p[i++] & 0x3fU);
        }
        if (code < 0x20U || code == 0x7fU || code < minimum || code > 0x10ffffU ||
            (code >= 0xd800U && code <= 0xdfffU) || code == 0xfffeU || code == 0xffffU) return false;
    }
    return i <= CERBERUS_GPX_LABEL_BYTES;
}

const char *cerberus_gpx_header(cerberus_gpx_mode_t mode)
{
    static const char track[] = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<gpx version=\"1.1\" creator=\"Cerberus\" xmlns=\"http://www.topografix.com/GPX/1/1\">\n"
        "<trk><name>Cerberus Track</name><trkseg>\n";
    static const char points[] = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<gpx version=\"1.1\" creator=\"Cerberus\" xmlns=\"http://www.topografix.com/GPX/1/1\">\n";
    return mode == CERBERUS_GPX_TRACK ? track : mode == CERBERUS_GPX_POINTS ? points : NULL;
}

const char *cerberus_gpx_footer(cerberus_gpx_mode_t mode)
{
    return mode == CERBERUS_GPX_TRACK ? "</trkseg></trk></gpx>\n" :
        mode == CERBERUS_GPX_POINTS ? "</gpx>\n" : NULL;
}

const char *cerberus_gpx_segment_break(void)
{ return "</trkseg><trkseg>\n"; }

static bool append(char *out, size_t capacity, size_t *used, const char *format, ...)
{
    if (*used >= capacity) return false;
    va_list args;
    va_start(args, format);
    int count = vsnprintf(out + *used, capacity - *used, format, args);
    va_end(args);
    if (count < 0 || (size_t)count >= capacity - *used) return false;
    *used += (size_t)count;
    return true;
}

bool cerberus_gpx_point(char *out, size_t capacity, size_t *length,
    cerberus_gpx_mode_t mode, const cerberus_wardrive_gps_snapshot_t *p,
    uint32_t number, const char *label)
{
    if (length) *length = 0;
    if (out && capacity) out[0] = '\0';
    if (!out || !capacity || !length || !number || !cerberus_gpx_header(mode) ||
        !cerberus_gpx_point_valid(p) || !cerberus_gpx_label_valid(label)) return false;
    size_t n = 0;
    const char *tag = mode == CERBERUS_GPX_TRACK ? "trkpt" : "wpt";
    /* GPX excludes +180. The equivalent -180 also covers values rounded to
     * the antimeridian at the seven decimal places used in the file. */
    double longitude = round(p->longitude * 10000000.0) / 10000000.0;
    if (longitude >= 180.0) longitude = -180.0;
    bool ok = append(out, capacity, &n, "<%s lat=\"%.7f\" lon=\"%.7f\">", tag, p->latitude, longitude);
    if (ok && p->altitude_valid) ok = append(out, capacity, &n, "<ele>%.3f</ele>", (double)p->altitude_m);
    if (ok) ok = append(out, capacity, &n, "<time>%s</time>", p->utc);
    if (ok && mode == CERBERUS_GPX_POINTS) {
        ok = append(out, capacity, &n, "<name>");
        if (ok && (!label || !label[0])) ok = append(out, capacity, &n, "POI %lu", (unsigned long)number);
        else if (ok) for (const char *c = label; *c && ok; ++c) {
            const char *escaped = *c == '&' ? "&amp;" : *c == '<' ? "&lt;" : *c == '>' ? "&gt;" :
                *c == '"' ? "&quot;" : *c == '\'' ? "&apos;" : NULL;
            ok = escaped ? append(out, capacity, &n, "%s", escaped) : append(out, capacity, &n, "%c", *c);
        }
        if (ok) ok = append(out, capacity, &n, "</name>");
    }
    if (ok && p->satellites_valid) ok = append(out, capacity, &n, "<sat>%u</sat>", p->satellites);
    if (ok && p->hdop_valid) ok = append(out, capacity, &n, "<hdop>%.3f</hdop>", (double)p->hdop);
    if (ok) ok = append(out, capacity, &n, "</%s>\n", tag);
    if (!ok) { memset(out, 0, capacity); return false; }
    *length = n;
    return true;
}

void cerberus_gps_track_gap(cerberus_gps_track_stats_t *s)
{
    if (s) { s->has_position = false; s->speed_mps = 0; }
}

static double distance(double lat1, double lon1, double lat2, double lon2)
{
    const double radians = 0.017453292519943295;
    double lat = sin((lat2 - lat1) * radians / 2.0);
    double lon = sin((lon2 - lon1) * radians / 2.0);
    double a = lat * lat + cos(lat1 * radians) * cos(lat2 * radians) * lon * lon;
    if (a < 0) a = 0;
    if (a > 1) a = 1;
    return 12742000.0 * atan2(sqrt(a), sqrt(1.0 - a));
}

bool cerberus_gps_track_observe(cerberus_gps_track_stats_t *s,
    const cerberus_wardrive_gps_snapshot_t *p, bool *new_segment)
{
    if (new_segment) *new_segment = false;
    uint64_t utc;
    if (!s || !new_segment || !cerberus_gpx_point_valid(p) || !cerberus_gpx_utc_ms(p->utc, &utc)) return false;
    if (s->points && utc == s->last_utc_ms) return false; /* No repeated polling of the same fix. */
    *new_segment = !s->has_position || utc < s->last_utc_ms || utc - s->last_utc_ms > 5000U;
    s->speed_mps = 0;
    if (*new_segment) {
        if (s->segments != UINT32_MAX) ++s->segments;
    } else {
        double seconds = (double)(utc - s->last_utc_ms) / 1000.0;
        double step = distance(s->latitude, s->longitude, p->latitude, p->longitude);
        if (step >= 3.0 && step <= seconds * 120.0) {
            s->distance_m += step;
            s->speed_mps = step / seconds;
        } else if (s->filtered_steps != UINT32_MAX) ++s->filtered_steps;
    }
    s->latitude = p->latitude; s->longitude = p->longitude; s->last_utc_ms = utc; s->has_position = true;
    if (s->points != UINT32_MAX) ++s->points;
    return true;
}
