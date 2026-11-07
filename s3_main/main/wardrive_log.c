#include "wardrive_log.h"
#include "wardrive_core.h"
#include "wardrive_wigle.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

cerberus_ward_log_kind_t cerberus_ward_log_name(const char *name)
{
    size_t length = 0;
    if (name) while (length < CERBERUS_WARD_LOG_NAME_CAPACITY && name[length]) ++length;
    const size_t prefix = sizeof(CERBERUS_WARD_LOG_PREFIX) - 1U;
    const bool current = name && length == prefix + 7U &&
        !memcmp(name, CERBERUS_WARD_LOG_PREFIX, prefix);
    const size_t digits = current ? prefix : 5U;
    if (!name || (!current && length != 12U) || strcmp(name + digits + 3U, ".CSV"))
        return CERBERUS_WARD_LOG_UNKNOWN;
    unsigned index = 0;
    for (size_t i = digits; i < digits + 3U; ++i) {
        if (name[i] < '0' || name[i] > '9') return CERBERUS_WARD_LOG_UNKNOWN;
        index = index * 10U + (unsigned)(name[i] - '0');
    }
    if (!index) return CERBERUS_WARD_LOG_UNKNOWN;
    return current || !memcmp(name, "WIGLE", 5) ? CERBERUS_WARD_LOG_WIGLE :
        !memcmp(name, "UNLOC", 5) ? CERBERUS_WARD_LOG_UNLOCATED : CERBERUS_WARD_LOG_UNKNOWN;
}

static bool fail(cerberus_ward_log_parser_t *p, cerberus_ward_log_error_t error)
{
    if (p->summary.error == CERBERUS_WARD_LOG_OK) p->summary.error = error;
    return false;
}

void cerberus_ward_log_init(cerberus_ward_log_parser_t *p)
{ if (p) memset(p, 0, sizeof(*p)); }

static bool utf8(const unsigned char *s)
{
    while (*s) {
        unsigned c = *s++, n;
        if (c < 128U) { if (c < 32U || c == 127U) return false; continue; }
        if (c >= 0xC2 && c <= 0xDF) n = 1;
        else if (c >= 0xE0 && c <= 0xEF) n = 2;
        else if (c >= 0xF0 && c <= 0xF4) n = 3;
        else return false;
        unsigned second = *s;
        if ((c == 0xE0 && second < 0xA0) || (c == 0xED && second > 0x9F) ||
            (c == 0xF0 && second < 0x90) || (c == 0xF4 && second > 0x8F)) return false;
        while (n--) { if ((*s & 0xC0U) != 0x80U) return false; ++s; }
    }
    return true;
}

/* Unquote into the same bounded line. A closing quote must be followed by
 * delimiter/end; quotes in unquoted fields and malformed escapes reject. */
static unsigned columns(char *line, char **fields, unsigned capacity)
{
    char *read = line, *write = line;
    unsigned count = 0;
    for (;;) {
        if (count == capacity) return 0;
        fields[count++] = write;
        if (*read == '"') {
            ++read;
            for (;;) {
                if (!*read) return 0;
                if (*read != '"') { *write++ = *read++; continue; }
                if (read[1] == '"') { *write++ = '"'; read += 2; continue; }
                ++read; break;
            }
            if (*read && *read != ',') return 0;
        } else {
            while (*read && *read != ',') {
                if (*read == '"') return 0;
                *write++ = *read++;
            }
        }
        bool more = *read == ',';
        if (more) ++read;
        *write++ = '\0';
        if (!more) return count;
    }
}

static bool integer(const char *s, long minimum, long maximum, long *value)
{
    const char *p = s;
    if (*p == '-') ++p;
    if (!*p) return false;
    while (*p) if (*p < '0' || *p++ > '9') return false;
    errno = 0; char *end; long result = strtol(s, &end, 10);
    if (errno || *end || result < minimum || result > maximum) return false;
    if (value) *value = result;
    return true;
}

static bool number(const char *s, double minimum, double maximum, double *value)
{
    if (!*s || *s == ' ' || *s == '\t') return false;
    /* Decimal only: strtod's hexadecimal and locale/space extensions are not CSV. */
    for (const char *p = s; *p; ++p)
        if (!((*p >= '0' && *p <= '9') || *p == '-' || *p == '+' || *p == '.' || *p == 'e' || *p == 'E')) return false;
    errno = 0; char *end; double result = strtod(s, &end);
    if (errno || *end || !isfinite(result) || result < minimum || result > maximum) return false;
    if (value) *value = result;
    return true;
}

static bool hex(const char *s, size_t length)
{
    if (strlen(s) != length) return false;
    while (*s) { char c = *s++; if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false; }
    return true;
}

static bool mac(const char *s)
{
    if (strlen(s) != 17) return false;
    for (unsigned i = 0; i < 17; ++i) {
        if (i % 3 == 2) { if (s[i] != ':') return false; }
        else { char x[2] = {s[i], 0}; if (!hex(x, 1)) return false; }
    }
    return true;
}

static unsigned decimal(const char *s, unsigned length)
{ unsigned n = 0; while (length--) n = n * 10U + (unsigned)(*s++ - '0'); return n; }

static bool utc(const char *s)
{
    if (strlen(s) != 19 || s[4] != '-' || s[7] != '-' || s[10] != ' ' || s[13] != ':' || s[16] != ':') return false;
    for (unsigned i = 0; i < 19; ++i)
        if (i != 4 && i != 7 && i != 10 && i != 13 && i != 16 && (s[i] < '0' || s[i] > '9')) return false;
    unsigned y = decimal(s, 4), m = decimal(s + 5, 2), d = decimal(s + 8, 2);
    static const unsigned days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (y < 1980 || m < 1 || m > 12 || d < 1) return false;
    unsigned limit = days[m - 1] + (m == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    return d <= limit && decimal(s + 11, 2) <= 23 && decimal(s + 14, 2) <= 59 && decimal(s + 17, 2) <= 59;
}

static bool metadata(cerberus_ward_log_parser_t *p)
{
    if (!strcmp(p->line, "# Cerberus unlocated v4; RX!=UTC")) {
        p->summary.kind = CERBERUS_WARD_LOG_UNLOCATED; return true;
    }
    char *field[32]; unsigned count = columns(p->line, field, 32);
    if (count < 8 || strcmp(field[0], "WigleWifi-1.6")) return false;
    static const char *const keys[] = {"appRelease", "model", "release", "device", "display", "board", "brand", "star", "body", "subBody"};
    unsigned found = 0;
    for (unsigned i = 1; i < count; ++i) {
        char *equals = strchr(field[i], '=');
        if (!equals || equals == field[i] || !equals[1]) return false;
        *equals = 0;
        for (unsigned j = 1; j < i; ++j) if (!strcmp(field[i], field[j])) return false;
        for (unsigned k = 0; k < sizeof(keys)/sizeof(keys[0]); ++k) {
            if (strcmp(field[i], keys[k])) continue;
            found |= 1U << k;
            if ((k == 7 && strcmp(equals + 1, "Sol")) || (k == 8 && strcmp(equals + 1, "3")) ||
                (k == 9 && strcmp(equals + 1, "0"))) return false;
        }
    }
    if ((found & 127U) != 127U) return false;
    p->summary.kind = CERBERUS_WARD_LOG_WIGLE;
    p->summary.legacy_metadata = (found & 896U) != 896U;
    return true;
}

static bool wigle_row(cerberus_ward_log_parser_t *p, char **f, unsigned count)
{
    if (count != 14 || !mac(f[0]) || !utc(f[3]) || !integer(f[6], -127, 0, NULL)) return false;
    double lat, lon;
    if (!number(f[7], -90, 90, &lat) || !number(f[8], -180, 180, &lon)) return false;
    if (*f[9] && !number(f[9], -1e9, 1e9, NULL)) return false;
    double accuracy;
    if (*f[10] && (!number(f[10], 0, 1e9, &accuracy) || accuracy <= 0)) return false;
    long channel, frequency;
    if (!integer(f[4], 0, 165, &channel)) return false;
    if (!strcmp(f[13], "WIFI")) {
        long expected = channel >= 1 && channel <= 13 ? 2407 + channel * 5 : channel == 14 ? 2484 :
            (((channel >= 36 && channel <= 64) || (channel >= 100 && channel <= 144)) && channel % 4 == 0) ||
            (channel >= 149 && channel <= 165 && (channel - 149) % 4 == 0) ? 5000 + channel * 5 : 0;
        if (!expected || !integer(f[5], 1, 6000, &frequency) || frequency != expected || *f[12]) return false;
        /* Native formatter has no RCOIs; allow valid upstream hex OI groups. */
        const char *rcoi = f[11];
        while (*rcoi) {
            size_t n = strcspn(rcoi, " "); if (!n) return false;
            for (size_t i = 0; i < n; ++i) { char x[2] = {rcoi[i],0}; if (!hex(x,1)) return false; }
            rcoi += n; if (*rcoi && !*++rcoi) return false;
        }
        if (channel <= 14) ++p->summary.wifi_24; else ++p->summary.wifi_5;
    } else if (!strcmp(f[13], "BLE")) {
        if (channel || *f[5] || *f[11] || (*f[12] && !integer(f[12], 0, 65534, NULL))) return false;
        ++p->summary.ble;
    } else return false;
    if (!*p->summary.first_utc || strcmp(f[3], p->summary.first_utc) < 0) memcpy(p->summary.first_utc, f[3], 20);
    if (!*p->summary.last_utc || strcmp(f[3], p->summary.last_utc) > 0) memcpy(p->summary.last_utc, f[3], 20);
    if (!*f[9]) ++p->summary.missing_altitude;
    if (!*f[10]) ++p->summary.missing_accuracy;
    if (lat == 0 && lon == 0) ++p->summary.zero_coordinates;
    for (unsigned i = 0; i < 6; ++i) {
        char octet[3] = {f[0][i * 3], f[0][i * 3 + 1], 0};
        p->last_row.address[i] = (uint8_t)strtoul(octet, NULL, 16);
    }
    p->last_row.type = !strcmp(f[13], "WIFI") ? 1U : 2U;
    p->last_row.rssi = (int8_t)strtol(f[6], NULL, 10);
    return true;
}

static bool consume_line(cerberus_ward_log_parser_t *p)
{
    ++p->summary.lines;
    if (p->used && p->line[p->used - 1] == '\r') --p->used;
    p->line[p->used] = 0;
    if (!utf8((const unsigned char *)p->line)) return fail(p, CERBERUS_WARD_LOG_ENCODING);
    if (p->summary.lines == 1) return metadata(p) || fail(p, CERBERUS_WARD_LOG_METADATA);
    if (p->summary.lines == 2) {
        const char *expected = p->summary.kind == CERBERUS_WARD_LOG_WIGLE ? CERBERUS_WIGLE_COLUMN_HEADER :
            strchr(CERBERUS_WARDRIVE_UNLOCATED_HEADER, '\n') + 1;
        size_t n = strlen(expected) - 1;
        return (p->used == n && !memcmp(p->line, expected, n)) || fail(p, CERBERUS_WARD_LOG_COLUMNS);
    }
    char *f[34]; unsigned count = columns(p->line, f, 34);
    bool valid = p->summary.kind == CERBERUS_WARD_LOG_WIGLE ? wigle_row(p, f, count) :
        count == 34 && *f[0] && *f[1] && *f[2] && *f[3] && mac(f[4]);
    if (!valid) return fail(p, CERBERUS_WARD_LOG_ROW);
    ++p->summary.rows;
    return true;
}

bool cerberus_ward_log_feed(cerberus_ward_log_parser_t *p, const void *data, size_t length)
{
    if (!p) return false;
    if ((!data && length) || p->summary.complete) return fail(p, CERBERUS_WARD_LOG_ARGUMENT);
    if (p->summary.error != CERBERUS_WARD_LOG_OK) return false;
    if (length > CERBERUS_WARD_LOG_MAX_BYTES - p->summary.bytes) return fail(p, CERBERUS_WARD_LOG_LIMIT);
    const unsigned char *bytes = data;
    for (size_t i = 0; i < length; ++i) {
        ++p->summary.bytes;
        if (bytes[i] == '\n') {
            if (!consume_line(p)) return false;
            p->used = 0;
        } else {
            if (!bytes[i]) return fail(p, CERBERUS_WARD_LOG_ENCODING);
            if (p->used + 1 >= sizeof(p->line)) return fail(p, CERBERUS_WARD_LOG_LIMIT);
            p->line[p->used++] = (char)bytes[i];
        }
    }
    return true;
}

bool cerberus_ward_log_finish(cerberus_ward_log_parser_t *p)
{
    if (!p || p->summary.error != CERBERUS_WARD_LOG_OK) return false;
    if (p->summary.complete) return true;
    if (p->used || p->summary.lines < 2) return fail(p, CERBERUS_WARD_LOG_TRUNCATED);
    if (!p->summary.rows) return fail(p, CERBERUS_WARD_LOG_EMPTY);
    p->summary.complete = true; return true;
}

const char *cerberus_ward_log_error_name(cerberus_ward_log_error_t error)
{
    static const char *const names[] = {"OK", "INVALID REQUEST", "FILE OR ROW TOO LARGE",
        "INVALID UTF-8", "UNKNOWN CSV HEADER", "WRONG CSV COLUMNS", "INVALID CSV ROW",
        "INCOMPLETE FILE", "NO OBSERVATIONS"};
    return (unsigned)error < sizeof(names)/sizeof(names[0]) ? names[error] : "UNKNOWN ERROR";
}
