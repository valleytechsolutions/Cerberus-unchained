#include "wardrive_upload_config.h"
#include <string.h>

_Static_assert(sizeof(cerberus_upload_config_t) == 487U, "Wire field layout changed");

void cerberus_upload_config_clear(void *bytes, size_t length)
{
    if (!bytes) return;
    volatile uint8_t *p = bytes;
    while (length--) *p++ = 0;
}

static size_t bounded_length(const char *s, size_t capacity)
{ size_t n = 0; while (n < capacity && s[n]) ++n; return n; }

static bool visible(const char *s, size_t capacity, bool space)
{
    size_t n = bounded_length(s, capacity);
    if (n == capacity) return false;
    for (size_t i = 0; i < n; ++i)
        if ((unsigned char)s[i] < (space ? 32U : 33U) || (unsigned char)s[i] > 126U) return false;
    return true;
}

static bool hexadecimal(const char *s, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f') || (s[i] >= 'A' && s[i] <= 'F'))) return false;
    return true;
}

cerberus_upload_field_t cerberus_upload_config_invalid(const cerberus_upload_config_t *c)
{
    if (!c) return CERBERUS_UPLOAD_FIELD_SSID;
    size_t n = bounded_length(c->ssid, sizeof(c->ssid));
    if (n == sizeof(c->ssid)) return CERBERUS_UPLOAD_FIELD_SSID;
    /* SSIDs are byte strings. Preserve non-ASCII bytes without pretending the
     * touchscreen can render them, but reject controls in manual input. */
    for (size_t i = 0; i < n; ++i) if ((unsigned char)c->ssid[i] < 32U || c->ssid[i] == 127) return CERBERUS_UPLOAD_FIELD_SSID;
    n = bounded_length(c->password, sizeof(c->password));
    if (!visible(c->password, sizeof(c->password), true) ||
        (n && (n < 8U || (n == 64U && !hexadecimal(c->password, n))))) return CERBERUS_UPLOAD_FIELD_PASSWORD;
    if (!visible(c->wigle_name, sizeof(c->wigle_name), false) || strchr(c->wigle_name, ':')) return CERBERUS_UPLOAD_FIELD_WIGLE_NAME;
    if (!visible(c->wigle_token, sizeof(c->wigle_token), false)) return CERBERUS_UPLOAD_FIELD_WIGLE_TOKEN;
    n = bounded_length(c->wdgwars_token, sizeof(c->wdgwars_token));
    if (n && (n != 64U || !hexadecimal(c->wdgwars_token, n))) return CERBERUS_UPLOAD_FIELD_WDGWARS_TOKEN;
    if (!visible(c->wardrift_token, sizeof(c->wardrift_token), false)) return CERBERUS_UPLOAD_FIELD_WARDRIFT_TOKEN;
    if (c->destinations & ~7U) return CERBERUS_UPLOAD_FIELD_DESTINATIONS;
    return CERBERUS_UPLOAD_FIELD_NONE;
}

bool cerberus_upload_config_wifi_ready(const cerberus_upload_config_t *c)
{ return cerberus_upload_config_invalid(c) == CERBERUS_UPLOAD_FIELD_NONE && c->ssid[0]; }

bool cerberus_upload_config_service_ready(const cerberus_upload_config_t *c, cerberus_upload_service_t service)
{
    if (cerberus_upload_config_invalid(c) != CERBERUS_UPLOAD_FIELD_NONE) return false;
    switch (service) {
    case CERBERUS_UPLOAD_WIGLE: return c->wigle_name[0] && c->wigle_token[0];
    case CERBERUS_UPLOAD_WDGWARS: return c->wdgwars_token[0] != 0;
    case CERBERUS_UPLOAD_WARDRIFT: return c->wardrift_token[0] != 0;
    default: return false;
    }
}

static uint32_t checksum(const uint8_t *p)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < CERBERUS_UPLOAD_CONFIG_WIRE_BYTES; ++i) {
        crc ^= i >= 12U && i < 16U ? 0U : p[i];
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}

static void canonical(cerberus_upload_config_t *out, const cerberus_upload_config_t *in)
{
    memset(out, 0, sizeof(*out));
#define COPY_FIELD(field) memcpy(out->field, in->field, strlen(in->field))
    COPY_FIELD(ssid); COPY_FIELD(password); COPY_FIELD(wigle_name); COPY_FIELD(wigle_token);
    COPY_FIELD(wdgwars_token); COPY_FIELD(wardrift_token);
#undef COPY_FIELD
    out->destinations = in->destinations;
}

static bool overlap(const void *a, size_t na, const void *b, size_t nb)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return x <= y ? y - x < na : x - y < nb;
}

bool cerberus_upload_config_encode(const cerberus_upload_config_t *c, uint8_t out[CERBERUS_UPLOAD_CONFIG_WIRE_BYTES])
{
    if (!out || cerberus_upload_config_invalid(c) != CERBERUS_UPLOAD_FIELD_NONE ||
        overlap(c, sizeof(*c), out, CERBERUS_UPLOAD_CONFIG_WIRE_BYTES)) return false;
    cerberus_upload_config_t clean; canonical(&clean, c);
    memset(out, 0, CERBERUS_UPLOAD_CONFIG_WIRE_BYTES);
    memcpy(out, "CBUPCFG1", 8); out[8] = 0xE7; out[9] = 1; /* 487 bytes */
    memcpy(out + 16, &clean, sizeof(clean));
    cerberus_upload_config_clear(&clean, sizeof(clean));
    uint32_t crc = checksum(out);
    for (unsigned i = 0; i < 4; ++i) out[12 + i] = (uint8_t)(crc >> (8U * i));
    return true;
}

bool cerberus_upload_config_decode(const uint8_t *bytes, size_t length, cerberus_upload_config_t *out)
{
    if (!out) return false;
    if (bytes && overlap(bytes, length, out, sizeof(*out))) return false;
    if (!bytes || length != CERBERUS_UPLOAD_CONFIG_WIRE_BYTES) {
        cerberus_upload_config_clear(out, sizeof(*out)); return false;
    }
    cerberus_upload_config_t candidate, clean;
    memset(&candidate, 0, sizeof(candidate)); memset(&clean, 0, sizeof(clean));
    bool valid = !memcmp(bytes, "CBUPCFG1", 8) && bytes[8] == 0xE7 && bytes[9] == 1 && !bytes[10] && !bytes[11];
    uint32_t expected = 0;
    for (unsigned i = 0; i < 4; ++i) expected |= (uint32_t)bytes[12 + i] << (8U * i);
    valid = valid && expected == checksum(bytes);
    for (size_t i = 16 + sizeof(candidate); i < length; ++i) valid = valid && bytes[i] == 0;
    if (valid) {
        memcpy(&candidate, bytes + 16, sizeof(candidate));
        valid = cerberus_upload_config_invalid(&candidate) == CERBERUS_UPLOAD_FIELD_NONE;
        if (valid) { canonical(&clean, &candidate); valid = !memcmp(&clean, &candidate, sizeof(clean)); }
    }
    if (valid) *out = clean;
    else cerberus_upload_config_clear(out, sizeof(*out));
    cerberus_upload_config_clear(&candidate, sizeof(candidate));
    cerberus_upload_config_clear(&clean, sizeof(clean));
    return valid;
}

const char *cerberus_upload_field_name(cerberus_upload_field_t field)
{
    static const char *const names[] = {"OK", "HOME WI-FI SSID", "HOME WI-FI PASSWORD", "WIGLE API NAME",
        "WIGLE API TOKEN", "WDGWARS TOKEN", "WARDRIFT TOKEN", "DESTINATIONS"};
    return (unsigned)field < sizeof(names)/sizeof(names[0]) ? names[field] : "INVALID FIELD";
}

const char *cerberus_upload_service_name(cerberus_upload_service_t service)
{
    static const char *const names[] = {"WIGLE", "WDGWARS", "WARDRIFT"};
    return (unsigned)service < CERBERUS_UPLOAD_SERVICE_COUNT ? names[service] : "UNKNOWN";
}
