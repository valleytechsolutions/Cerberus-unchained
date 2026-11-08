#pragma once

#include "wifi_fox_hunt.h"
#include "wifi_transmitter.h"

static const uint8_t cerberus_wifi_fox_hunt_5g_channels[] = {
    36,40,44,48,52,56,60,64,100,104,108,112,116,120,124,128,
    132,136,140,144,149,153,157,161,165,169,173,177
};

static inline uint32_t cerberus_wifi_fox_hunt_age(int64_t now, int64_t observed)
{
    if (observed < 0 || now < observed) return UINT32_MAX;
    uint64_t elapsed = (uint64_t)(now - observed);
    uint64_t age = elapsed / 1000U + (elapsed % 1000U != 0U);
    return age >= UINT32_MAX ? UINT32_MAX : (uint32_t)age;
}
