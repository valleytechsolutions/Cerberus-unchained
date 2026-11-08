#pragma once

#include "mac_monitor_protocol.h"

/* Clear transmitter headers only. Retry, encrypted and fragmented frames
 * remain observations. ACK/CTS/wrapper and variable BAR/BA have no supported
 * transmitter layout here. callback_type uses the 802.11 type bits (0/1/2). */
static inline bool cerberus_wifi_transmitter(const uint8_t *p, size_t n,
    unsigned callback_type, uint8_t address[6], uint8_t *kind)
{
    if (!p || !address || !kind || n < 2U || (p[0] & 3U)) return false;
    unsigned frame_type = (p[0] >> 2U) & 3U, subtype = p[0] >> 4U;
    size_t minimum;
    if (callback_type == 0U && frame_type == 0U) {
        if (p[1] & 3U) return false;
        minimum = 24U + ((p[1] & 0x80U) ? 4U : 0U);
        *kind = CERBERUS_MAC_WIFI_MGMT;
    } else if (callback_type == 2U && frame_type == 2U) {
        minimum = 24U + ((p[1] & 3U) == 3U ? 6U : 0U);
        if (subtype & 8U) minimum += 2U + ((p[1] & 0x80U) ? 4U : 0U);
        *kind = CERBERUS_MAC_WIFI_DATA;
    } else if (callback_type == 1U && frame_type == 1U &&
        (subtype == 10U || subtype == 11U || subtype == 14U || subtype == 15U)) {
        if ((p[1] & 3U) || n != 16U) return false;
        minimum = 16U;
        *kind = CERBERUS_MAC_WIFI_CONTROL;
    } else return false;
    if (n < minimum || (p[10] & 1U)) return false;
    uint8_t any = 0U;
    for (unsigned i = 0; i < 6U; ++i) any |= p[10U + i];
    if (!any) return false;
    memcpy(address, p + 10U, 6U);
    return true;
}
