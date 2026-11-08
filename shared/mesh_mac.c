/* Original Cerberus implementation; no upstream implementation copied. */
#include "mesh_mac.h"

#include <string.h>

/* C5 Locate calls this bounded pure parser from the receive callback. Its
 * complete call/data closure must remain internal; S3 and host placement stays
 * unchanged. Native verification checks linked symbols rather than this label. */
#if defined(ESP_PLATFORM)
#include "sdkconfig.h"
#endif
#if defined(CONFIG_IDF_TARGET_ESP32C5) && CONFIG_IDF_TARGET_ESP32C5
#include "esp_attr.h"
#define MESH_MAC_CODE IRAM_ATTR
#define MESH_MAC_DATA DRAM_ATTR
#else
#define MESH_MAC_CODE
#define MESH_MAC_DATA
#endif

typedef struct {
    const uint8_t *bytes;
    size_t length;
    size_t offset;
} reader_t;

static bool MESH_MAC_CODE overlaps(const void *a, size_t an, const void *b, size_t bn)
{
    uintptr_t ap = (uintptr_t)a, bp = (uintptr_t)b;
    return ap <= bp ? bp - ap < an : ap - bp < bn;
}

static bool MESH_MAC_CODE take(reader_t *r, size_t count, const uint8_t **bytes)
{
    if (r->offset > r->length || count > r->length - r->offset) return false;
    *bytes = r->bytes + r->offset;
    r->offset += count;
    return true;
}

static uint64_t MESH_MAC_CODE little_value(const uint8_t *bytes, size_t count)
{
    uint64_t value = 0;
    for (size_t i = 0; i < count; ++i) value |= (uint64_t)bytes[i] << (i * 8U);
    return value;
}

static bool MESH_MAC_CODE read_pan(reader_t *r, mesh_mac_pan_t *pan)
{
    const uint8_t *bytes;
    if (!take(r, 2U, &bytes)) return false;
    pan->value = (uint16_t)little_value(bytes, 2U);
    pan->origin = MESH_MAC_PAN_EXPLICIT;
    return true;
}

static bool MESH_MAC_CODE read_address(reader_t *r, mesh_mac_address_t *address)
{
    const uint8_t *bytes;
    size_t size = address->mode == MESH_MAC_ADDRESS_SHORT16 ? 2U : 8U;
    if (address->mode == MESH_MAC_ADDRESS_NONE) return true;
    if (!take(r, size, &bytes)) return false;
    address->value = little_value(bytes, size);
    address->broadcast = address->mode == MESH_MAC_ADDRESS_SHORT16 && address->value == UINT16_MAX;
    address->short_sentinel = address->mode == MESH_MAC_ADDRESS_SHORT16 && address->value == 0xfffeU;
    return true;
}

static void MESH_MAC_CODE pan_presence(const mesh_mac_observation_t *o, bool *destination, bool *source)
{
    bool d = o->destination.mode != MESH_MAC_ADDRESS_NONE;
    bool s = o->source.mode != MESH_MAC_ADDRESS_NONE;
    bool both_extended = o->destination.mode == MESH_MAC_ADDRESS_EXT64 &&
                         o->source.mode == MESH_MAC_ADDRESS_EXT64;
    *destination = d;
    *source = s && !o->pan_compression;
    if (o->version != 2U) return;
    if (!d && !s) *destination = o->pan_compression;
    else if (d && (!s || both_extended)) *destination = !o->pan_compression;
    if (both_extended) *source = false;
}

static mesh_mac_result_t MESH_MAC_CODE read_security(reader_t *r, mesh_mac_observation_t *o)
{
    const uint8_t *bytes;
    if (!o->security_enabled) return MESH_MAC_HEADER_OK;
    if (o->version == 0U) return MESH_MAC_UNSUPPORTED;
    if (!take(r, 1U, &bytes)) return MESH_MAC_MALFORMED;
    uint8_t control = bytes[0];
    if ((control & 0x80U) || (o->version == 1U && (control & 0x60U)))
        return MESH_MAC_UNSUPPORTED;
    o->security_level = control & 7U;
    if (o->security_level == 0U) return MESH_MAC_UNSUPPORTED;
    o->key_id_mode = (control >> 3U) & 3U;
    o->encrypted = o->security_level >= 4U;
    o->frame_counter_present = (control & 0x20U) == 0U;
    o->asn_in_nonce = (control & 0x40U) != 0U;
    if (o->frame_counter_present) {
        if (!take(r, 4U, &bytes)) return MESH_MAC_MALFORMED;
        o->frame_counter = (uint32_t)little_value(bytes, 4U);
    }
    static MESH_MAC_DATA const uint8_t key_lengths[4] = {0U, 1U, 5U, 9U};
    o->key_identifier_length = key_lengths[o->key_id_mode];
    if (!take(r, o->key_identifier_length, &bytes)) return MESH_MAC_MALFORMED;
    memcpy(o->key_identifier, bytes, o->key_identifier_length);
    static MESH_MAC_DATA const uint8_t mic_lengths[4] = {0U, 4U, 8U, 16U};
    o->mic_length = mic_lengths[o->security_level & 3U];
    if (o->mic_length > r->length - r->offset) return MESH_MAC_MALFORMED;
    return MESH_MAC_HEADER_OK;
}

static mesh_mac_result_t MESH_MAC_CODE read_header_ies(reader_t *r, mesh_mac_observation_t *o)
{
    if (!o->header_ies_present) return MESH_MAC_HEADER_OK;
    bool read_any = false;
    while (r->offset < r->length) {
        const uint8_t *bytes;
        if (!take(r, 2U, &bytes)) return MESH_MAC_MALFORMED;
        uint16_t descriptor = (uint16_t)little_value(bytes, 2U);
        if (descriptor & 0x8000U) return MESH_MAC_UNSUPPORTED;
        uint8_t length = descriptor & 0x7fU;
        uint8_t id = (descriptor >> 7U) & 0xffU;
        if ((id == 0x7eU || id == 0x7fU) && length != 0U) return MESH_MAC_MALFORMED;
        if (!take(r, length, &bytes)) return MESH_MAC_MALFORMED;
        read_any = true;
        if (id == 0x7eU) {
            /* The next region is payload (possibly encrypted), not header IEs.
             * Require space for a payload descriptor but do not interpret it. */
            if (r->length - r->offset < 2U) return MESH_MAC_MALFORMED;
            o->payload_ies_indicated = true;
            return MESH_MAC_HEADER_OK;
        }
        if (id == 0x7fU) return MESH_MAC_HEADER_OK;
    }
    /* A nonempty IE list may end exactly at the MIC boundary without HT2. */
    return read_any ? MESH_MAC_HEADER_OK : MESH_MAC_MALFORMED;
}

mesh_mac_result_t MESH_MAC_CODE mesh_mac_parse(const uint8_t *bytes, size_t length,
                                 mesh_mac_observation_t *out)
{
    if (bytes == NULL || out == NULL) return MESH_MAC_INVALID_ARGUMENT;
    if (length < 2U || length > MESH_MAC_MAX_LENGTH) return MESH_MAC_MALFORMED;
    if (overlaps(bytes, length, out, sizeof(*out))) return MESH_MAC_INVALID_ARGUMENT;
    reader_t r = {bytes, length, 0U};
    mesh_mac_observation_t o;
    memset(&o, 0, sizeof(o));
    const uint8_t *field;
    if (!take(&r, 2U, &field)) return MESH_MAC_MALFORMED;
    o.frame_control = (uint16_t)little_value(field, 2U);
    o.frame_type = o.frame_control & 7U;
    o.version = (o.frame_control >> 12U) & 3U;
    o.destination.mode = (o.frame_control >> 10U) & 3U;
    o.source.mode = (o.frame_control >> 14U) & 3U;
    if (o.frame_type > 3U || o.version == 3U || o.destination.mode == 1U ||
        o.source.mode == 1U || (o.frame_control & 0x0080U)) return MESH_MAC_UNSUPPORTED;
    if (o.version < 2U && (o.frame_control & 0x0300U)) return MESH_MAC_UNSUPPORTED;
    o.pan_compression = (o.frame_control & 0x0040U) != 0U;
    if (o.version < 2U && o.pan_compression &&
        (o.source.mode == 0U || o.destination.mode == 0U)) return MESH_MAC_UNSUPPORTED;
    o.security_enabled = (o.frame_control & 0x0008U) != 0U;
    o.frame_pending = (o.frame_control & 0x0010U) != 0U;
    o.ack_requested = (o.frame_control & 0x0020U) != 0U;
    o.sequence_present = (o.frame_control & 0x0100U) == 0U;
    o.header_ies_present = (o.frame_control & 0x0200U) != 0U;
    if (o.frame_type == 2U && o.version < 2U &&
        (o.security_enabled || o.destination.mode || o.source.mode || o.pan_compression))
        return MESH_MAC_UNSUPPORTED;
    if (o.sequence_present) {
        if (!take(&r, 1U, &field)) return MESH_MAC_MALFORMED;
        o.sequence = field[0];
    }
    bool destination_pan, source_pan;
    pan_presence(&o, &destination_pan, &source_pan);
    if (destination_pan && !read_pan(&r, &o.destination_pan)) return MESH_MAC_MALFORMED;
    if (!read_address(&r, &o.destination)) return MESH_MAC_MALFORMED;
    if (source_pan && !read_pan(&r, &o.source_pan)) return MESH_MAC_MALFORMED;
    if (!read_address(&r, &o.source)) return MESH_MAC_MALFORMED;
    bool both_extended = o.source.mode == 3U && o.destination.mode == 3U;
    if (o.pan_compression && o.source.mode && o.destination.mode &&
        !(o.version == 2U && both_extended)) {
        o.source_pan.value = o.destination_pan.value;
        o.source_pan.origin = MESH_MAC_PAN_COMPRESSED_SAME;
    }
    mesh_mac_result_t result = read_security(&r, &o);
    if (result != MESH_MAC_HEADER_OK) return result;
    r.length -= o.mic_length; /* Subsequent clear-header reads cannot consume MIC. */
    result = read_header_ies(&r, &o);
    if (result != MESH_MAC_HEADER_OK) return result;
    if (o.frame_type == 3U && o.version < 2U) {
        if (!take(&r, 1U, &field)) return MESH_MAC_MALFORMED;
        o.legacy_command_id = field[0];
        o.legacy_command_id_present = true;
    }
    o.header_length = (uint16_t)r.offset;
    o.payload_length = (uint16_t)(r.length - r.offset);
    memcpy(out, &o, sizeof(o));
    return MESH_MAC_HEADER_OK;
}
