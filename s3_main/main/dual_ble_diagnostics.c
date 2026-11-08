#include "dual_ble_diagnostics.h"
#include "freertos/FreeRTOS.h"
#include <string.h>

static portMUX_TYPE diagnostics_lock = portMUX_INITIALIZER_UNLOCKED;
static cerberus_dual_ble_diagnostics_t published;
static uint64_t last_valid_ms;
static bool have_clock;

void cerberus_dual_ble_diagnostics_publish(const cerberus_dual_ble_diagnostics_t *input, int64_t now)
{
    /* Build the value before locking. Only the UI task may publish. */
    cerberus_dual_ble_diagnostics_t next = {0};
    if (input) next = *input;
    next.version = 3U;
    next.available = input != NULL;
    next.clock_valid = now >= 0 && (!have_clock || (uint64_t)now >= last_valid_ms);
    if (next.clock_valid) { last_valid_ms = (uint64_t)now; have_clock = true; }
    next.published_ms = have_clock ? last_valid_ms : 0U;
    if (!next.clock_valid) {
        next.s3.fresh = next.c5.fresh = false;
        next.s3.age_ms = next.c5.age_ms = UINT32_MAX;
    }
    portENTER_CRITICAL(&diagnostics_lock);
    next.sequence_saturated = published.sequence == UINT64_MAX;
    next.sequence = next.sequence_saturated ? UINT64_MAX : published.sequence + 1U;
    published = next;
    portEXIT_CRITICAL(&diagnostics_lock);
}

void cerberus_dual_ble_diagnostics_get(cerberus_dual_ble_diagnostics_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&diagnostics_lock);
    *out = published;
    portEXIT_CRITICAL(&diagnostics_lock);
}
