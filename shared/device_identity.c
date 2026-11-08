#include "device_identity.h"
#include <string.h>

static bool text_valid(const char *text, size_t capacity)
{
    bool ended = false;
    for (size_t i = 0; i < capacity; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (!c) ended = true;
        else if (ended || c < 32U || c > 126U) return false;
    }
    return ended;
}

static bool zero_bytes(const void *data, size_t size)
{
    const uint8_t *bytes = data;
    for (size_t i = 0; i < size; ++i) if (bytes[i]) return false;
    return true;
}

bool cerberus_device_identity_valid(const cerberus_device_identity_t *d)
{
    if (!d || d->magic != CERBERUS_DEVICE_IDENTITY_MAGIC || !d->cores ||
        d->cores > 2U || (d->flags & 0x80U) || d->reserved ||
        !zero_bytes(d->reserved_tail, sizeof(d->reserved_tail))) return false;
    if (!text_valid(d->project, sizeof(d->project)) || !d->project[0] ||
        !text_valid(d->version, sizeof(d->version)) || !d->version[0] ||
        !text_valid(d->idf, sizeof(d->idf)) || !d->idf[0] ||
        !text_valid(d->date, sizeof(d->date)) || !text_valid(d->time, sizeof(d->time)) ||
        !text_valid(d->partition_label, sizeof(d->partition_label))) return false;
    if ((d->flags & CERBERUS_ID_FLASH) ? !d->flash_bytes : d->flash_bytes != 0U) return false;
    if (d->flags & CERBERUS_ID_PARTITION) {
        if (!d->partition_bytes || !d->partition_label[0] ||
            d->partition_address > UINT32_MAX - d->partition_bytes) return false;
        if ((d->flags & CERBERUS_ID_FLASH) &&
            d->partition_address + d->partition_bytes > d->flash_bytes) return false;
    } else if (d->partition_address || d->partition_bytes || d->partition_subtype ||
               !zero_bytes(d->partition_label, sizeof(d->partition_label))) return false;
    if (!(d->flags & CERBERUS_ID_BASE_MAC) && !zero_bytes(d->base_mac, 6U)) return false;
    if (!(d->flags & CERBERUS_ID_STA_MAC) &&
        (!zero_bytes(d->sta_mac, 6U) || (d->flags & CERBERUS_ID_STA_CURRENT))) return false;
    if (!(d->flags & CERBERUS_ID_AP_MAC) &&
        (!zero_bytes(d->ap_mac, 6U) || (d->flags & CERBERUS_ID_AP_CURRENT))) return false;
    return true;
}
