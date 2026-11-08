#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* C5 source-clock intervals, not S3 time buckets. An invalid entry is a gap;
 * it must not be rendered as an observed zero. No allocation or clock reads. */
#define CERBERUS_DUAL_BLE_ANALYZER_HISTORY 32U
typedef struct {
    uint32_t reports, duration_ms;
    bool valid;
} cerberus_dual_ble_analyzer_interval_t;
typedef struct {
    cerberus_dual_ble_analyzer_interval_t history[CERBERUS_DUAL_BLE_ANALYZER_HISTORY];
    uint64_t epoch;
    uint32_t job_id, reports, elapsed_ms, samples;
    uint32_t baseline_reports, baseline_elapsed_ms;
    uint8_t head, count;
    bool has_sample, baseline, sample_valid, saturated, final;
} cerberus_dual_ble_analyzer_history_t;

static inline void cerberus_dual_ble_analyzer_append(
    cerberus_dual_ble_analyzer_history_t *h, cerberus_dual_ble_analyzer_interval_t value)
{
    h->history[h->head] = value;
    h->head = (uint8_t)((h->head + 1U) % CERBERUS_DUAL_BLE_ANALYZER_HISTORY);
    if (h->count < CERBERUS_DUAL_BLE_ANALYZER_HISTORY) ++h->count;
}
static inline void cerberus_dual_ble_analyzer_gap(cerberus_dual_ble_analyzer_history_t *h)
{
    if (!h) return;
    if (h->count && h->history[(h->head + CERBERUS_DUAL_BLE_ANALYZER_HISTORY - 1U) %
            CERBERUS_DUAL_BLE_ANALYZER_HISTORY].valid)
        cerberus_dual_ble_analyzer_append(h, (cerberus_dual_ble_analyzer_interval_t){0});
    h->baseline = false; h->sample_valid = false;
}
/* Caller has already checked the bound wire envelope. Refuse regressions
 * without changing retained measurements, even after a transport gap. */
static inline bool cerberus_dual_ble_analyzer_accept(
    cerberus_dual_ble_analyzer_history_t *h, uint64_t epoch, uint32_t job,
    uint32_t reports, uint32_t elapsed, bool saturated, bool final)
{
    if (!h || !epoch || !job || (saturated != (reports == UINT32_MAX))) return false;
    bool same = h->has_sample && h->epoch == epoch && h->job_id == job;
    if (same && (elapsed < h->elapsed_ms || reports < h->reports ||
        (h->saturated && !saturated) || (h->final && !final))) return false;
    if (!same) cerberus_dual_ble_analyzer_gap(h);
    if (same && elapsed > h->baseline_elapsed_ms && h->baseline && !saturated && !h->saturated)
        cerberus_dual_ble_analyzer_append(h, (cerberus_dual_ble_analyzer_interval_t){
            .reports = reports - h->baseline_reports,
            .duration_ms = elapsed - h->baseline_elapsed_ms, .valid = true});
    if (saturated) cerberus_dual_ble_analyzer_gap(h);
    /* Millisecond quantization permits new callbacks at the same elapsed
     * value. Keep the rate baseline until time advances, conserving those
     * callbacks in the next measurable interval. */
    if (!saturated && (!h->baseline || elapsed > h->baseline_elapsed_ms)) {
        h->baseline_reports = reports; h->baseline_elapsed_ms = elapsed;
    }
    h->epoch = epoch; h->job_id = job; h->reports = reports; h->elapsed_ms = elapsed;
    h->has_sample = true; h->sample_valid = true; h->baseline = !saturated;
    h->saturated = saturated; h->final = final;
    if (h->samples != UINT32_MAX) ++h->samples;
    return true;
}
/* Integer floor callbacks/s for this interval. Overflow is unknown rather
 * than a clipped rate presented as exact. Zero callbacks is a valid rate. */
static inline bool cerberus_dual_ble_analyzer_rate(
    const cerberus_dual_ble_analyzer_interval_t *interval, uint32_t *out)
{
    if (out) *out = 0U;
    if (!out || !interval || !interval->valid || !interval->duration_ms) return false;
    uint64_t rate = (uint64_t)interval->reports * 1000U / interval->duration_ms;
    if (rate > UINT32_MAX) return false;
    *out = (uint32_t)rate; return true;
}
