#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Per-scan selection, not the persistent driver band mode. IDF's bitmap bit0
 * bypasses that entire band; the remaining bits are filtered by country rules. */
enum {
    CERBERUS_WIFI_SCAN_BAND_ALL = 0U,
    CERBERUS_WIFI_SCAN_BAND_2G = 1U,
    CERBERUS_WIFI_SCAN_BAND_5G = 2U,
};
#define CERBERUS_WIFI_SCAN_BAND_MIN_FIRMWARE 61U

static inline bool cerberus_wifi_scan_band_valid(uint8_t band)
{
    return band <= CERBERUS_WIFI_SCAN_BAND_5G;
}

static inline uint16_t cerberus_wifi_scan_band_2g_bitmap(uint8_t band)
{
    return band == CERBERUS_WIFI_SCAN_BAND_2G ? UINT16_C(0x7ffe) :
        band == CERBERUS_WIFI_SCAN_BAND_5G ? UINT16_C(1) : UINT16_C(0);
}

static inline uint32_t cerberus_wifi_scan_band_5g_bitmap(uint8_t band)
{
    return band == CERBERUS_WIFI_SCAN_BAND_5G ? UINT32_C(0x1ffffffe) :
        band == CERBERUS_WIFI_SCAN_BAND_2G ? UINT32_C(1) : UINT32_C(0);
}
