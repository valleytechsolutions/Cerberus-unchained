#include "wifi_capture_plan.h"
#include "wifi_packet_count.h"
#include <string.h>

_Static_assert(CERBERUS_WIFI_CAPTURE_PLAN_CHANNELS == CERBERUS_WIFI_PACKET_COUNT_CHANNELS,
    "Capture plans share the Packet Count channel map");

static bool unicast(const uint8_t *address)
{
    if (!address || (address[0] & 1U)) return false;
    uint8_t any = 0U;
    for (unsigned i = 0U; i < 6U; ++i) any |= address[i];
    return any != 0U;
}

void cerberus_wifi_capture_plan_clear(cerberus_wifi_capture_plan_t *plan)
{
    if (!plan) return;
    memset(plan, 0, sizeof(*plan));
    plan->dwell_ms = CERBERUS_WIFI_CAPTURE_PLAN_DWELL_MS;
}

uint8_t cerberus_wifi_capture_plan_channel(unsigned index)
{ return cerberus_wifi_packet_count_channel(index); }

uint64_t cerberus_wifi_capture_plan_channel_bit(uint8_t channel)
{ return cerberus_wifi_packet_count_channel_bit(channel); }

bool cerberus_wifi_capture_plan_mask_valid(uint64_t mask)
{ return cerberus_wifi_packet_count_mask_valid(mask); }

uint64_t cerberus_wifi_capture_plan_band_mask(uint8_t band)
{
    const uint64_t all = (UINT64_C(1) << CERBERUS_WIFI_CAPTURE_PLAN_CHANNELS) - 1U;
    const uint64_t low = (UINT64_C(1) << 14U) - 1U;
    return band == CERBERUS_WIFI_CAPTURE_PLAN_BOTH ? all :
        band == CERBERUS_WIFI_CAPTURE_PLAN_2G ? low :
        band == CERBERUS_WIFI_CAPTURE_PLAN_5G ? all ^ low : 0U;
}

bool cerberus_wifi_capture_plan_valid(const cerberus_wifi_capture_plan_t *plan)
{
    if (!plan || !cerberus_wifi_capture_plan_mask_valid(plan->channel_mask) ||
        plan->dwell_ms != CERBERUS_WIFI_CAPTURE_PLAN_DWELL_MS ||
        plan->target_count > CERBERUS_WIFI_CAPTURE_PLAN_MAX_TARGETS) return false;
    for (unsigned i = 0U; i < plan->target_count; ++i) {
        if (!unicast(plan->bssids[i])) return false;
        for (unsigned j = 0U; j < i; ++j)
            if (!memcmp(plan->bssids[i], plan->bssids[j], 6U)) return false;
    }
    return true;
}

bool cerberus_wifi_capture_plan_add_target(cerberus_wifi_capture_plan_t *plan,
    const uint8_t bssid[6], uint8_t channel)
{
    uint64_t bit = cerberus_wifi_capture_plan_channel_bit(channel);
    if (!plan || !unicast(bssid) || !bit ||
        plan->dwell_ms != CERBERUS_WIFI_CAPTURE_PLAN_DWELL_MS ||
        plan->target_count > CERBERUS_WIFI_CAPTURE_PLAN_MAX_TARGETS ||
        (plan->channel_mask && !cerberus_wifi_capture_plan_mask_valid(plan->channel_mask))) return false;
    /* Check the existing targets before changing any field. A partially built
     * plan may have no channel yet, but never invalid or repeated identities. */
    bool found = false;
    for (unsigned i = 0U; i < plan->target_count; ++i) {
        if (!unicast(plan->bssids[i])) return false;
        for (unsigned j = 0U; j < i; ++j)
            if (!memcmp(plan->bssids[i], plan->bssids[j], 6U)) return false;
        if (!memcmp(plan->bssids[i], bssid, 6U)) found = true;
    }
    if (!found && plan->target_count == CERBERUS_WIFI_CAPTURE_PLAN_MAX_TARGETS) return false;
    if (!found) memcpy(plan->bssids[plan->target_count++], bssid, 6U);
    plan->channel_mask |= bit;
    return true;
}

bool cerberus_wifi_capture_plan_matches(const cerberus_wifi_capture_plan_t *plan,
    const uint8_t bssid[6], uint8_t channel)
{
    if (!plan || !bssid || plan->target_count > CERBERUS_WIFI_CAPTURE_PLAN_MAX_TARGETS ||
        !(plan->channel_mask & cerberus_wifi_capture_plan_channel_bit(channel))) return false;
    if (!plan->target_count) return true;
    for (unsigned i = 0U; i < plan->target_count; ++i)
        if (!memcmp(plan->bssids[i], bssid, 6U)) return true;
    return false;
}
