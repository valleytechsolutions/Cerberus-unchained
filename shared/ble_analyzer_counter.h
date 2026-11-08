#pragma once

/* Constant-space callback accounting. The caller serializes access and binds
 * the full owner epoch; this private callback token excludes late RF events.
 * No clock reads, parsing, allocation, inventory, transport or RF operations. */
#include "dual_ble_protocol.h"
#include <string.h>

typedef struct {
    uint64_t started_us, last_callback_us, last_query_us;
    uint32_t callback_epoch, reports, elapsed_ms;
    int8_t rssi;
    bool armed, accepted, finished, clock_invalid, saturated;
} cerberus_ble_analyzer_counter_t;

static inline void cerberus_ble_analyzer_counter_reset(cerberus_ble_analyzer_counter_t *c)
{ memset(c, 0, sizeof(*c)); c->rssi = 127; }

static inline void cerberus_ble_analyzer_counter_arm(cerberus_ble_analyzer_counter_t *c,
    uint32_t epoch, int64_t now_us)
{
    cerberus_ble_analyzer_counter_reset(c);
    c->callback_epoch = epoch; c->armed = epoch != 0U;
    c->clock_invalid = now_us < 0;
    if (now_us >= 0) c->started_us = c->last_callback_us = c->last_query_us = (uint64_t)now_us;
}

static inline void cerberus_ble_analyzer_counter_accept(cerberus_ble_analyzer_counter_t *c,
    bool accepted)
{
    if (!accepted) cerberus_ble_analyzer_counter_reset(c);
    else c->accepted = c->armed;
}

static inline bool cerberus_ble_analyzer_counter_time(cerberus_ble_analyzer_counter_t *c,
    int64_t now_us, bool callback)
{
    if (c->clock_invalid) return false;
    /* A callback timestamp is captured before taking the mutex. A query may
     * run between capture and delivery, so compare callbacks to callbacks,
     * not to a newer query. Queries/finish must follow both prior timelines. */
    if (now_us < 0 || (uint64_t)now_us < c->started_us ||
        (uint64_t)now_us < c->last_callback_us ||
        (!callback && (uint64_t)now_us < c->last_query_us) ||
        ((uint64_t)now_us - c->started_us) / 1000U > UINT32_MAX) {
        c->clock_invalid = true; return false;
    }
    if (callback) c->last_callback_us = (uint64_t)now_us;
    else c->last_query_us = (uint64_t)now_us;
    return true;
}

static inline void cerberus_ble_analyzer_counter_observe(cerberus_ble_analyzer_counter_t *c,
    uint32_t epoch, uint8_t address_type, int8_t rssi, int64_t callback_us)
{
    if (!c->armed || c->finished || !epoch || epoch != c->callback_epoch || address_type > 3U ||
        !cerberus_ble_analyzer_counter_time(c, callback_us, true)) return;
    if (c->reports < UINT32_MAX) ++c->reports;
    if (c->reports == UINT32_MAX) c->saturated = true;
    c->rssi = rssi >= -127 && rssi <= 20 ? rssi : 127;
}

static inline void cerberus_ble_analyzer_counter_finish(cerberus_ble_analyzer_counter_t *c,
    int64_t now_us)
{
    if (!c->armed || !c->accepted || c->finished) return;
    if (cerberus_ble_analyzer_counter_time(c, now_us, false))
        c->elapsed_ms = (uint32_t)(((uint64_t)now_us - c->started_us) / 1000U);
    c->finished = true;
}

static inline void cerberus_ble_analyzer_counter_sample(cerberus_ble_analyzer_counter_t *c,
    int64_t now_us, cerberus_dual_ble_analyzer_sample_t *out)
{
    memset(out, 0, sizeof(*out)); out->rssi = 127;
    if (!c->armed || !c->accepted) return;
    if (!cerberus_ble_analyzer_counter_time(c, now_us, false)) {
        out->flags = CERBERUS_DUAL_BLE_ANALYZER_CLOCK_INVALID; return;
    }
    out->reports = c->reports;
    out->elapsed_ms = c->finished ? c->elapsed_ms :
        (uint32_t)(((uint64_t)now_us - c->started_us) / 1000U);
    out->flags = CERBERUS_DUAL_BLE_ANALYZER_VALID |
        (c->finished ? CERBERUS_DUAL_BLE_ANALYZER_FINAL : 0U) |
        (c->saturated ? CERBERUS_DUAL_BLE_ANALYZER_SATURATED : 0U);
    if (c->reports && c->rssi != 127) { out->rssi = c->rssi; out->flags |= CERBERUS_DUAL_BLE_ANALYZER_RSSI_VALID; }
}
