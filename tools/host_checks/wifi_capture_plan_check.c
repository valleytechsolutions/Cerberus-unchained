#undef NDEBUG
#include "wifi_capture_plan.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void rejected_unchanged(cerberus_wifi_capture_plan_t *plan,
    const uint8_t *address, uint8_t channel)
{
    cerberus_wifi_capture_plan_t before = *plan;
    assert(!cerberus_wifi_capture_plan_add_target(plan, address, channel));
    assert(!memcmp(plan, &before, sizeof(before)));
}

int main(void)
{
    static const uint8_t channels[] = {
        1,2,3,4,5,6,7,8,9,10,11,12,13,14,36,40,44,48,52,56,60,64,
        100,104,108,112,116,120,124,128,132,136,140,144,149,153,157,161,165,169,173,177
    };
    const uint64_t all = (UINT64_C(1) << 42U) - 1U;
    const uint64_t low = (UINT64_C(1) << 14U) - 1U;
    assert(cerberus_wifi_capture_plan_band_mask(CERBERUS_WIFI_CAPTURE_PLAN_BOTH) == all);
    assert(cerberus_wifi_capture_plan_band_mask(CERBERUS_WIFI_CAPTURE_PLAN_2G) == low);
    assert(cerberus_wifi_capture_plan_band_mask(CERBERUS_WIFI_CAPTURE_PLAN_5G) == (all ^ low));
    assert(!cerberus_wifi_capture_plan_band_mask(3U));
    uint64_t seen = 0U;
    for (unsigned i = 0; i < sizeof(channels); ++i) {
        assert(cerberus_wifi_capture_plan_channel(i) == channels[i]);
        uint64_t bit = cerberus_wifi_capture_plan_channel_bit(channels[i]);
        assert(bit == (UINT64_C(1) << i));
        assert(!(seen & bit));
        seen |= bit;
        assert(cerberus_wifi_capture_plan_mask_valid(bit));
    }
    assert(seen == all);
    for (unsigned channel = 0U; channel <= 255U; ++channel) {
        bool listed = false;
        for (unsigned i = 0U; i < sizeof(channels); ++i) listed |= channels[i] == channel;
        assert(!!cerberus_wifi_capture_plan_channel_bit((uint8_t)channel) == listed);
    }
    assert(!cerberus_wifi_capture_plan_channel(42U));
    assert(!cerberus_wifi_capture_plan_channel(UINT32_MAX));
    assert(!cerberus_wifi_capture_plan_mask_valid(0U));
    assert(!cerberus_wifi_capture_plan_mask_valid(UINT64_C(1) << 42U));
    assert(!cerberus_wifi_capture_plan_mask_valid(UINT64_MAX));
    assert(!cerberus_wifi_capture_plan_valid(NULL));
    cerberus_wifi_capture_plan_clear(NULL);

    cerberus_wifi_capture_plan_t plan;
    memset(&plan, 0xff, sizeof(plan));
    cerberus_wifi_capture_plan_clear(&plan);
    assert(plan.dwell_ms == 250U && !plan.channel_mask && !plan.target_count);
    assert(!cerberus_wifi_capture_plan_valid(&plan));
    uint8_t address[6] = {2U,0U,0U,0U,0U,1U}, zero[6] = {0};
    assert(!cerberus_wifi_capture_plan_add_target(NULL, address, 1U));
    rejected_unchanged(&plan, NULL, 1U);
    rejected_unchanged(&plan, zero, 1U);
    rejected_unchanged(&plan, address, 0U);
    address[0] = 3U;
    rejected_unchanged(&plan, address, 36U);
    address[0] = 2U;
    assert(cerberus_wifi_capture_plan_add_target(&plan, address, 1U));
    assert(cerberus_wifi_capture_plan_add_target(&plan, address, 36U));
    assert(plan.target_count == 1U && plan.channel_mask == (1U | (UINT64_C(1) << 14U)));
    assert(cerberus_wifi_capture_plan_valid(&plan));
    assert(cerberus_wifi_capture_plan_matches(&plan, address, 36U));
    assert(!cerberus_wifi_capture_plan_matches(&plan, address, 6U));
    assert(!cerberus_wifi_capture_plan_matches(&plan, NULL, 1U));
    assert(!cerberus_wifi_capture_plan_matches(NULL, address, 1U));
    for (unsigned i = 2U; i <= 32U; ++i) {
        address[5] = (uint8_t)i;
        assert(cerberus_wifi_capture_plan_add_target(&plan, address, 6U));
    }
    assert(plan.target_count == 32U && cerberus_wifi_capture_plan_valid(&plan));
    address[5] = 33U;
    rejected_unchanged(&plan, address, 177U);
    assert(!cerberus_wifi_capture_plan_matches(&plan, address, 6U));
    address[5] = 1U;
    assert(cerberus_wifi_capture_plan_add_target(&plan, address, 177U));
    assert(plan.target_count == 32U && cerberus_wifi_capture_plan_matches(&plan, address, 177U));

    cerberus_wifi_capture_plan_t valid = plan;
    plan.target_count = 33U;
    rejected_unchanged(&plan, address, 1U);
    assert(!cerberus_wifi_capture_plan_valid(&plan));
    assert(!cerberus_wifi_capture_plan_matches(&plan, address, 1U));
    plan = valid;
    memcpy(plan.bssids[1], plan.bssids[0], 6U);
    assert(!cerberus_wifi_capture_plan_valid(&plan));
    rejected_unchanged(&plan, address, 1U);
    plan = valid;
    plan.bssids[1][0] |= 1U;
    assert(!cerberus_wifi_capture_plan_valid(&plan));
    rejected_unchanged(&plan, address, 1U);
    plan = valid;
    plan.dwell_ms = 249U;
    assert(!cerberus_wifi_capture_plan_valid(&plan));
    rejected_unchanged(&plan, address, 1U);
    plan = valid;
    plan.channel_mask |= UINT64_C(1) << 42U;
    assert(!cerberus_wifi_capture_plan_valid(&plan));
    rejected_unchanged(&plan, address, 1U);
    cerberus_wifi_capture_plan_clear(&plan);
    plan.channel_mask = all;
    assert(cerberus_wifi_capture_plan_valid(&plan));
    for (unsigned i = 0U; i < sizeof(channels); ++i)
        assert(cerberus_wifi_capture_plan_matches(&plan, address, channels[i]));
    assert(!cerberus_wifi_capture_plan_matches(&plan, address, 32U));
    printf("PASS: capture plan channels, bands, target union and unchanged rejection (%zu bytes)\n", sizeof(plan));
    return 0;
}
