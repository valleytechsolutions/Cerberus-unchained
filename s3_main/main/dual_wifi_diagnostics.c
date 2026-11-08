#include "dual_wifi_diagnostics.h"
#include "freertos/FreeRTOS.h"

_Static_assert(sizeof(cerberus_dual_wifi_diagnostics_t) <= 128U,
    "Keep dual Wi-Fi console snapshot compact");
static portMUX_TYPE diagnostics_lock = portMUX_INITIALIZER_UNLOCKED;
static cerberus_dual_wifi_diagnostics_t published;

void cerberus_dual_wifi_diagnostics_publish(
    const cerberus_dual_wifi_diagnostic_values_t *value, int64_t now_us)
{
    if (!value) return;
    /* Copy caller-owned UI data before the brief publication lock. */
    cerberus_dual_wifi_diagnostic_values_t copy = *value;
    portENTER_CRITICAL(&diagnostics_lock);
    published.value = copy;
    if (published.publication_sequence != UINT64_MAX) ++published.publication_sequence;
    else published.sequence_saturated = true;
    published.time_valid = now_us >= 0 && (uint64_t)now_us >= published.published_us;
    if (published.time_valid) published.published_us = (uint64_t)now_us;
    published.available = true;
    portEXIT_CRITICAL(&diagnostics_lock);
}

void cerberus_dual_wifi_diagnostics_get(cerberus_dual_wifi_diagnostics_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&diagnostics_lock);
    *out = published;
    portEXIT_CRITICAL(&diagnostics_lock);
}
