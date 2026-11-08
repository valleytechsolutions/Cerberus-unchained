#include "wardrive_gps_core.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static void increment(uint32_t *value)
{
    if (*value != UINT32_MAX) ++*value;
}

static int hex_digit(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
}

static bool decimal_uint(const char *text, unsigned maximum, unsigned *out)
{
    if (text == NULL || *text == '\0' || out == NULL) return false;
    unsigned value = 0U;
    for (const char *cursor = text; *cursor != '\0'; ++cursor) {
        if (*cursor < '0' || *cursor > '9') return false;
        unsigned digit = (unsigned)(*cursor - '0');
        if (digit > maximum || value > (maximum - digit) / 10U) return false;
        value = value * 10U + digit;
    }
    *out = value;
    return true;
}

static bool coordinate(const char *text, char hemisphere, bool latitude,
    double *out)
{
    unsigned degree_digits = latitude ? 2U : 3U;
    if (text == NULL || out == NULL || strlen(text) < degree_digits + 2U ||
        (latitude ? hemisphere != 'N' && hemisphere != 'S' :
            hemisphere != 'E' && hemisphere != 'W')) return false;
    unsigned degrees = 0U, minutes = 0U;
    for (unsigned i = 0U; i < degree_digits; ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        degrees = degrees * 10U + (unsigned)(text[i] - '0');
    }
    for (unsigned i = degree_digits; i < degree_digits + 2U; ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        minutes = minutes * 10U + (unsigned)(text[i] - '0');
    }
    double fraction = 0.0, place = 0.1;
    const char *cursor = text + degree_digits + 2U;
    if (*cursor == '.') {
        ++cursor;
        if (*cursor == '\0') return false;
        for (; *cursor != '\0'; ++cursor) {
            if (*cursor < '0' || *cursor > '9') return false;
            fraction += (double)(*cursor - '0') * place;
            place *= 0.1;
        }
    } else if (*cursor != '\0') return false;
    unsigned limit = latitude ? 90U : 180U;
    if (degrees > limit || minutes >= 60U ||
        (degrees == limit && (minutes != 0U || fraction != 0.0))) return false;
    double value = (double)degrees + ((double)minutes + fraction) / 60.0;
    if (hemisphere == 'S' || hemisphere == 'W') value = -value;
    *out = value;
    return true;
}

static bool utc_time(const char *text, unsigned *hours, unsigned *minutes,
    unsigned *seconds, unsigned *milliseconds, char second_text[7])
{
    if (text == NULL || strlen(text) < 6U) return false;
    for (unsigned i = 0U; i < 6U; ++i)
        if (text[i] < '0' || text[i] > '9') return false;
    unsigned hour = (unsigned)(text[0] - '0') * 10U + (unsigned)(text[1] - '0');
    unsigned minute = (unsigned)(text[2] - '0') * 10U + (unsigned)(text[3] - '0');
    unsigned second = (unsigned)(text[4] - '0') * 10U + (unsigned)(text[5] - '0');
    if (hour > 23U || minute > 59U || second > 59U) return false;
    unsigned millis = 0U, fraction_digits = 0U;
    const char *cursor = text + 6U;
    if (*cursor == '.') {
        ++cursor;
        if (*cursor == '\0') return false;
        for (; *cursor != '\0'; ++cursor) {
            if (*cursor < '0' || *cursor > '9' || fraction_digits >= 6U) return false;
            if (fraction_digits < 3U) millis = millis * 10U + (unsigned)(*cursor - '0');
            ++fraction_digits;
        }
        while (fraction_digits < 3U) { millis *= 10U; ++fraction_digits; }
    } else if (*cursor != '\0') return false;
    *hours = hour;
    *minutes = minute;
    *seconds = second;
    *milliseconds = millis;
    memcpy(second_text, text, 6U);
    second_text[6] = '\0';
    return true;
}

static bool utc_date(const char *text, unsigned *year, unsigned *month,
    unsigned *day)
{
    if (text == NULL || strlen(text) != 6U) return false;
    for (unsigned i = 0U; i < 6U; ++i)
        if (text[i] < '0' || text[i] > '9') return false;
    unsigned d = (unsigned)(text[0] - '0') * 10U + (unsigned)(text[1] - '0');
    unsigned m = (unsigned)(text[2] - '0') * 10U + (unsigned)(text[3] - '0');
    unsigned yy = (unsigned)(text[4] - '0') * 10U + (unsigned)(text[5] - '0');
    unsigned y = yy >= 80U ? 1900U + yy : 2000U + yy;
    static const uint8_t days_in_month[12] =
        {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m < 1U || m > 12U) return false;
    unsigned max_day = days_in_month[m - 1U];
    if (m == 2U && y % 4U == 0U && (y % 100U != 0U || y % 400U == 0U))
        ++max_day;
    if (d < 1U || d > max_day) return false;
    *year = y;
    *month = m;
    *day = d;
    return true;
}

static void write_two_digits(char out[2], unsigned value)
{
    out[0] = (char)('0' + (value / 10U) % 10U);
    out[1] = (char)('0' + value % 10U);
}

static bool decimal_float(const char *text, float minimum, float maximum,
    float *out)
{
    if (text == NULL || *text == '\0' || out == NULL) return false;
    /* strtof also accepts hex, exponents and leading whitespace. NMEA fields
     * here are plain signed decimals, with digits on both sides of any dot. */
    const char *cursor = text;
    if (*cursor == '-' || *cursor == '+') ++cursor;
    if (*cursor < '0' || *cursor > '9') return false;
    while (*cursor >= '0' && *cursor <= '9') ++cursor;
    if (*cursor == '.') {
        ++cursor;
        if (*cursor < '0' || *cursor > '9') return false;
        while (*cursor >= '0' && *cursor <= '9') ++cursor;
    }
    if (*cursor != '\0') return false;
    char *end = NULL;
    float value = strtof(text, &end);
    if (end == text || *end != '\0' || !isfinite(value) ||
        value < minimum || value > maximum) return false;
    *out = value;
    return true;
}

static size_t split_fields(char *sentence, char *fields[], size_t capacity)
{
    size_t count = 0U;
    fields[count++] = sentence;
    for (char *cursor = sentence; *cursor != '\0'; ++cursor) {
        if (*cursor != ',') continue;
        *cursor = '\0';
        if (count < capacity) fields[count++] = cursor + 1;
    }
    return count;
}

static void invalidate_fix(cerberus_wardrive_gps_core_t *core)
{
    core->fix_live = false;
    core->latitude = 0.0;
    core->longitude = 0.0;
    core->altitude_valid = false;
    core->altitude_m = 0.0f;
    /* A later GGA must not turn a void/estimated RMC's time into FIX + UTC. */
    core->utc_seen = false;
    core->utc[0] = '\0';
}

static bool parse_rmc(cerberus_wardrive_gps_core_t *core, char *fields[],
    size_t count, uint64_t monotonic_ms)
{
    core->rmc_time_valid = core->rmc_fix_valid = false;
    if (count < 10U || fields[2][1] != '\0' ||
        (fields[2][0] != 'A' && fields[2][0] != 'V')) {
        invalidate_fix(core);
        increment(&core->malformed_sentences);
        return false;
    }
    unsigned hour, minute, second, millis, year, month, day;
    char second_text[7];
    bool time_valid = utc_time(fields[1], &hour, &minute, &second, &millis, second_text) &&
        utc_date(fields[9], &year, &month, &day);
    core->rmc_time_valid = time_valid;
    if (time_valid) {
        /* Each parsed field has a fixed width. Avoid a variadic formatter in
         * the GPS reader and keep the 24-byte UTC representation exact. */
        write_two_digits(core->utc, year / 100U);
        write_two_digits(core->utc + 2U, year % 100U);
        core->utc[4] = '-';
        write_two_digits(core->utc + 5U, month);
        core->utc[7] = '-';
        write_two_digits(core->utc + 8U, day);
        core->utc[10] = 'T';
        write_two_digits(core->utc + 11U, hour);
        core->utc[13] = ':';
        write_two_digits(core->utc + 14U, minute);
        core->utc[16] = ':';
        write_two_digits(core->utc + 17U, second);
        core->utc[19] = '.';
        core->utc[20] = (char)('0' + millis / 100U);
        write_two_digits(core->utc + 21U, millis % 100U);
        core->utc[23] = 'Z';
        core->utc[24] = '\0';
        memcpy(core->utc_second, second_text, sizeof(core->utc_second));
        core->utc_seen = true;
        core->last_utc_ms = monotonic_ms;
        core->utc_millis=(uint16_t)millis;
    } else {
        core->utc_seen = false;
        core->utc[0] = '\0';
    }
    /* Mode is optional in older RMC formats. The separate navigation-status
     * field after it is not used: LC29H reports V there even with a valid fix. */
    bool satellite_mode = true;
    if (count > 12U && fields[12][0]) {
        if (fields[12][1] || !strchr("ADFREMNS", fields[12][0])) {
            invalidate_fix(core);
            increment(&core->malformed_sentences);
            return false;
        }
        satellite_mode = strchr("ADFR", fields[12][0]) != NULL;
    }
    bool gga_disagrees = time_valid && core->gga_seen && core->gga_time_valid &&
        !core->gga_fix_valid && monotonic_ms >= core->last_gga_ms &&
        monotonic_ms - core->last_gga_ms <= 1000U && core->gga_millis == millis &&
        memcmp(core->gga_second, second_text, sizeof(second_text)) == 0;
    if (fields[2][0] == 'V' || !satellite_mode || gga_disagrees) {
        invalidate_fix(core);
        return time_valid;
    }
    double latitude, longitude;
    if (!coordinate(fields[3], fields[4][0], true, &latitude) ||
        !coordinate(fields[5], fields[6][0], false, &longitude) ||
        fields[4][1] != '\0' || fields[6][1] != '\0') {
        invalidate_fix(core);
        increment(&core->malformed_sentences);
        return false;
    }
    core->latitude = latitude;
    core->longitude = longitude;
    /* Receivers differ in sentence order. Preserve altitude only from the
     * same fresh GGA epoch, never from an earlier fix or previous UTC day. */
    core->altitude_valid = core->altitude_valid && time_valid && core->gga_time_valid &&
        memcmp(core->gga_second,second_text,sizeof(second_text))==0 && core->gga_millis==millis &&
        monotonic_ms>=core->last_gga_ms && monotonic_ms-core->last_gga_ms<=1000U;
    if(!core->altitude_valid)core->altitude_m=0.0f;
    core->fix_live = true;
    core->rmc_fix_valid = time_valid;
    core->last_fix_ms = monotonic_ms;
    increment(&core->fix_updates);
    return time_valid;
}

static bool parse_gga(cerberus_wardrive_gps_core_t *core, char *fields[],
    size_t count, uint64_t monotonic_ms)
{
    /* These fields belong to this GGA, independently of later RMC fixes and
     * independently of whether GGA currently reports a navigation fix. */
    core->gga_seen = true;
    core->gga_time_valid = false;
    core->gga_fix_valid = false;
    core->last_gga_ms = monotonic_ms;
    core->satellites_valid = false;
    core->hdop_valid = false;
    core->satellites = 0U;
    core->hdop = 0.0f;
    unsigned quality;
    if (count < 10U || !decimal_uint(fields[6], 8U, &quality)) {
        invalidate_fix(core);
        increment(&core->malformed_sentences);
        return false;
    }
    unsigned satellites = 0U;
    float hdop = 0.0f;
    bool satellites_ok = !fields[7][0] || decimal_uint(fields[7], UINT8_MAX, &satellites);
    bool hdop_ok = !fields[8][0] || decimal_float(fields[8], 0.0f, 99.9f, &hdop);
    core->satellites_valid = fields[7][0] && satellites_ok;
    core->hdop_valid = fields[8][0] && hdop_ok;
    if (core->satellites_valid) core->satellites = (uint8_t)satellites;
    if (core->hdop_valid) core->hdop = hdop;
    unsigned hour, minute, second, millis;
    char second_text[7];
    bool gga_time_valid = utc_time(fields[1], &hour, &minute, &second,
        &millis, second_text);
    core->gga_time_valid=gga_time_valid;
    if(gga_time_valid){memcpy(core->gga_second,second_text,sizeof(second_text));core->gga_millis=(uint16_t)millis;}
    if (!gga_time_valid || !core->utc_seen ||
        memcmp(core->utc_second, second_text, sizeof(core->utc_second)) != 0 ||
        core->utc_millis!=millis ||
        monotonic_ms < core->last_utc_ms ||
        monotonic_ms - core->last_utc_ms > 1000U) {
        core->utc_seen = false;
        core->utc[0] = '\0';
    }
    /* Qualities 1-5 describe satellite fixes (including differential/PPS/RTK).
     * Estimated, manually supplied and simulated positions are not a GNSS fix. */
    bool rmc_disagrees = gga_time_valid && core->rmc_time_valid && !core->rmc_fix_valid &&
        monotonic_ms >= core->last_utc_ms && monotonic_ms - core->last_utc_ms <= 1000U &&
        core->utc_millis == millis && memcmp(core->utc_second, second_text, sizeof(second_text)) == 0;
    if (quality == 0U || quality > 5U || rmc_disagrees) {
        invalidate_fix(core);
        if (!satellites_ok || !hdop_ok) increment(&core->malformed_sentences);
        return gga_time_valid && satellites_ok && hdop_ok;
    }
    double latitude, longitude;
    if (!coordinate(fields[2], fields[3][0], true, &latitude) ||
        !coordinate(fields[4], fields[5][0], false, &longitude) ||
        fields[3][1] != '\0' || fields[5][1] != '\0') {
        invalidate_fix(core);
        increment(&core->malformed_sentences);
        return false;
    }
    float altitude = 0.0f;
    if (!satellites_ok || !hdop_ok) {
        invalidate_fix(core);
        increment(&core->malformed_sentences);
        return false;
    }
    bool altitude_valid = fields[9][0] && count >= 11U &&
        strcmp(fields[10], "M") == 0 &&
        decimal_float(fields[9], -1000.0f, 20000.0f, &altitude);
    if (fields[9][0] && !altitude_valid) {
        invalidate_fix(core);
        increment(&core->malformed_sentences);
        return false;
    }
    core->latitude = latitude;
    core->longitude = longitude;
    core->altitude_valid = altitude_valid;
    core->altitude_m = altitude_valid ? altitude : 0.0f;
    core->fix_live = true;
    core->gga_fix_valid = gga_time_valid;
    core->last_fix_ms = monotonic_ms;
    increment(&core->fix_updates);
    return gga_time_valid;
}

void cerberus_wardrive_gps_core_reset(cerberus_wardrive_gps_core_t *core)
{
    if (core != NULL) memset(core, 0, sizeof(*core));
}

static cerberus_wardrive_gps_line_result_t ingest_line_result(
    cerberus_wardrive_gps_core_t *core, const char *line, size_t length,
    uint64_t monotonic_ms)
{
    if (core == NULL || line == NULL) return CERBERUS_WARDRIVE_GPS_LINE_FRAME_INVALID;
    if (length >= CERBERUS_WARDRIVE_GPS_LINE_CAPACITY) {
        increment(&core->overflow_lines);
        return CERBERUS_WARDRIVE_GPS_LINE_OVERFLOW;
    }
    if (length > 0U && line[length - 1U] == '\r') --length;
    if (length < 10U || line[0] != '$' || line[length - 3U] != '*') {
        increment(&core->malformed_sentences);
        return CERBERUS_WARDRIVE_GPS_LINE_FRAME_INVALID;
    }
    int high = hex_digit(line[length - 2U]);
    int low = hex_digit(line[length - 1U]);
    if (high < 0 || low < 0) {
        increment(&core->malformed_sentences);
        return CERBERUS_WARDRIVE_GPS_LINE_FRAME_INVALID;
    }
    uint8_t checksum = 0U;
    for (size_t i = 1U; i < length - 3U; ++i) {
        unsigned char byte = (unsigned char)line[i];
        if (byte < 0x20U || byte > 0x7EU || byte == '*') {
            increment(&core->malformed_sentences);
            return byte == '*' ? CERBERUS_WARDRIVE_GPS_LINE_FRAME_INVALID :
                CERBERUS_WARDRIVE_GPS_LINE_CONTROL;
        }
        checksum ^= byte;
    }
    if (checksum != (uint8_t)((high << 4) | low)) {
        increment(&core->bad_checksums);
        return CERBERUS_WARDRIVE_GPS_LINE_BAD_CHECKSUM;
    }
    char sentence[CERBERUS_WARDRIVE_GPS_LINE_CAPACITY];
    memcpy(sentence, line, length - 3U);
    sentence[length - 3U] = '\0';
    core->sentence_seen = true;
    core->last_sentence_ms = monotonic_ms;
    increment(&core->valid_sentences);
    if (sentence[1] < 'A' || sentence[1] > 'Z' ||
        sentence[2] < 'A' || sentence[2] > 'Z' ||
        sentence[6] != ',') return CERBERUS_WARDRIVE_GPS_LINE_OTHER;
    bool rmc = memcmp(sentence + 3U, "RMC", 3U) == 0;
    bool gga = memcmp(sentence + 3U, "GGA", 3U) == 0;
    if (!rmc && !gga) return CERBERUS_WARDRIVE_GPS_LINE_OTHER;
    char *fields[20] = {0};
    size_t count = split_fields(sentence, fields, 20U);
    if (rmc) return parse_rmc(core, fields, count, monotonic_ms) ?
        CERBERUS_WARDRIVE_GPS_LINE_RMC : CERBERUS_WARDRIVE_GPS_LINE_FIELDS_INVALID;
    return parse_gga(core, fields, count, monotonic_ms) ?
        CERBERUS_WARDRIVE_GPS_LINE_GGA : CERBERUS_WARDRIVE_GPS_LINE_FIELDS_INVALID;
}

bool cerberus_wardrive_gps_core_ingest_line(cerberus_wardrive_gps_core_t *core,
    const char *line, size_t length, uint64_t monotonic_ms)
{
    cerberus_wardrive_gps_line_result_t result = ingest_line_result(core, line, length, monotonic_ms);
    return result == CERBERUS_WARDRIVE_GPS_LINE_OTHER || result == CERBERUS_WARDRIVE_GPS_LINE_RMC ||
        result == CERBERUS_WARDRIVE_GPS_LINE_GGA || result == CERBERUS_WARDRIVE_GPS_LINE_FIELDS_INVALID;
}

static void retain_payload_byte(cerberus_wardrive_gps_core_t *core, uint8_t byte)
{
    increment(&core->line_total_length);
    if ((byte < 0x20U || byte > 0x7EU) && !core->line_control) {
        core->line_control = true;
        core->discarding_line = true;
        increment(&core->malformed_sentences);
    }
    if (core->line_length < CERBERUS_WARDRIVE_GPS_LINE_CAPACITY - 1U) {
        core->line[core->line_length++] = (char)byte;
    } else if (!core->line_overflow) {
        core->line_overflow = true;
        core->discarding_line = true;
        increment(&core->overflow_lines);
    }
}

void cerberus_wardrive_gps_core_feed_observed(cerberus_wardrive_gps_core_t *core,
    const uint8_t *bytes, size_t count, uint64_t monotonic_ms,
    cerberus_wardrive_gps_line_observer_t observer, void *context)
{
    if (core == NULL || (bytes == NULL && count != 0U)) return;
    for (size_t i = 0U; i < count; ++i) {
        uint8_t byte = bytes[i];
        bool crlf = core->pending_cr && byte == '\n';
        if (core->pending_cr) {
            core->pending_cr = false;
            /* A CR not followed by LF is an original payload control byte,
             * not a terminator. Keep it in the bounded raw prefix. */
            if (!crlf) retain_payload_byte(core, '\r');
        }
        if (byte == '\n') {
            if (core->line_total_length != 0U) {
                cerberus_wardrive_gps_line_result_t result = core->line_overflow ?
                    CERBERUS_WARDRIVE_GPS_LINE_OVERFLOW : core->line_control ?
                    CERBERUS_WARDRIVE_GPS_LINE_CONTROL : ingest_line_result(core,
                        core->line, core->line_length, monotonic_ms);
                if (observer) observer(context, (const uint8_t *)core->line,
                    core->line_length, core->line_total_length, monotonic_ms, result,
                    crlf ? CERBERUS_WARDRIVE_GPS_TERMINATION_CRLF : CERBERUS_WARDRIVE_GPS_TERMINATION_LF,
                    !core->line_overflow && core->line_total_length == core->line_length);
            }
            core->line_length = 0U;
            core->line_total_length = 0U;
            core->discarding_line = false;
            core->line_control = false;
            core->line_overflow = false;
            continue;
        }
        if (byte == '\r') {
            core->pending_cr = true;
            continue;
        }
        retain_payload_byte(core, byte);
    }
}

void cerberus_wardrive_gps_core_feed(cerberus_wardrive_gps_core_t *core,
    const uint8_t *bytes, size_t count, uint64_t monotonic_ms)
{
    cerberus_wardrive_gps_core_feed_observed(core, bytes, count, monotonic_ms, NULL, NULL);
}

static uint32_t age_ms(uint64_t now, uint64_t then)
{
    if (now < then) return UINT32_MAX;
    uint64_t difference = now - then;
    return difference > UINT32_MAX ? UINT32_MAX : (uint32_t)difference;
}

void cerberus_wardrive_gps_core_snapshot(const cerberus_wardrive_gps_core_t *core,
    uint64_t monotonic_ms, cerberus_wardrive_gps_snapshot_t *out)
{
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    out->sentence_age_ms = UINT32_MAX;
    out->fix_age_ms = UINT32_MAX;
    out->utc_age_ms = UINT32_MAX;
    out->gga_age_ms = UINT32_MAX;
    if (core == NULL) return;
    out->valid_sentences = core->valid_sentences;
    out->bad_checksums = core->bad_checksums;
    out->malformed_sentences = core->malformed_sentences;
    out->overflow_lines = core->overflow_lines;
    out->fix_updates = core->fix_updates;
    if (core->sentence_seen)
        out->sentence_age_ms = age_ms(monotonic_ms, core->last_sentence_ms);
    if (core->fix_updates != 0U)
        out->fix_age_ms = age_ms(monotonic_ms, core->last_fix_ms);
    if (core->utc_seen)
        out->utc_age_ms = age_ms(monotonic_ms, core->last_utc_ms);
    if (core->gga_seen)
        out->gga_age_ms = age_ms(monotonic_ms, core->last_gga_ms);
    if (!core->sentence_seen ||
        out->sentence_age_ms > CERBERUS_WARDRIVE_GPS_SENTENCE_MAX_AGE_MS) {
        out->state = CERBERUS_WARDRIVE_GPS_ABSENT;
        return;
    }
    if (out->gga_age_ms <= CERBERUS_WARDRIVE_GPS_FIX_MAX_AGE_MS) {
        out->satellites_valid = core->satellites_valid;
        out->hdop_valid = core->hdop_valid;
        if (out->satellites_valid) out->satellites = core->satellites;
        if (out->hdop_valid) out->hdop = core->hdop;
    }
    if (!core->fix_live) {
        out->state = CERBERUS_WARDRIVE_GPS_NO_FIX;
        return;
    }
    if (out->fix_age_ms > CERBERUS_WARDRIVE_GPS_FIX_MAX_AGE_MS) {
        out->state = CERBERUS_WARDRIVE_GPS_STALE;
        return;
    }
    out->state = CERBERUS_WARDRIVE_GPS_FRESH;
    out->coordinates_valid = true;
    out->latitude = core->latitude;
    out->longitude = core->longitude;
    out->altitude_valid = core->altitude_valid;
    out->altitude_m = core->altitude_valid ? core->altitude_m : 0.0f;
    if (core->utc_seen && out->utc_age_ms <=
        CERBERUS_WARDRIVE_GPS_FIX_MAX_AGE_MS) {
        out->utc_valid = true;
        out->utc_from_rmc = true;
        memcpy(out->utc, core->utc, sizeof(out->utc));
    }
}
