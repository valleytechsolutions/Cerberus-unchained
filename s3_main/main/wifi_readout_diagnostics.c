#include "wifi_readout_diagnostics.h"
#include "freertos/FreeRTOS.h"
#include <string.h>

static portMUX_TYPE diagnostics_lock = portMUX_INITIALIZER_UNLOCKED;
static cerberus_wifi_readout_diagnostics_t published;
/* Single UI writer. Readers only access published under diagnostics_lock. */
static struct {
    cerberus_wifi_readout_diagnostics_t value;
    uint64_t began_us, last_sample_us, last_step_start_us, last_step_end_us;
    bool sampled, stepped;
} model;

static void publish(void)
{
    portENTER_CRITICAL(&diagnostics_lock);
    published = model.value;
    portEXIT_CRITICAL(&diagnostics_lock);
}

static void increment(uint32_t *counter)
{
    if (*counter == UINT32_MAX) model.value.counters_saturated = true;
    else ++*counter;
}

static void invalidate_time(void)
{
    model.value.timing_valid = false;
    model.value.elapsed_us = 0U;
    model.value.max_step_us = 0U;
    model.value.max_tick_gap_us = 0U;
}

static void observe_time(int64_t now_us)
{
    if (now_us < 0) {
        invalidate_time();
        model.value.sampled_us = 0U;
        return;
    }
    uint64_t now = (uint64_t)now_us;
    if ((model.sampled && now < model.last_sample_us) || now < model.began_us)
        invalidate_time();
    model.sampled = true;
    model.last_sample_us = now;
    model.value.sampled_us = now;
}

void cerberus_wifi_readout_diagnostics_begin(uint16_t rows_total, uint8_t phase,
    int64_t now_us)
{
    /* A caller replacing an unfinished attempt still accounts for its outcome.
     * This affects only diagnostics; it never cancels RF or UI work itself. */
    if (model.value.state == CERBERUS_WIFI_READOUT_READING)
        increment(&model.value.cancelled);
    increment(&model.value.attempts);
    model.value.state = CERBERUS_WIFI_READOUT_READING;
    model.value.phase = phase;
    model.value.cancel_reason = CERBERUS_WIFI_READOUT_CANCEL_NONE;
    model.value.error = 0;
    model.value.rows_total = rows_total;
    model.value.rows_copied = 0U;
    model.value.step_calls = 0U;
    model.value.elapsed_us = model.value.max_step_us = model.value.max_tick_gap_us = 0U;
    model.value.timing_valid = now_us >= 0;
    model.began_us = now_us >= 0 ? (uint64_t)now_us : 0U;
    model.sampled = model.stepped = false;
    observe_time(now_us);
    publish();
}

void cerberus_wifi_readout_diagnostics_step(uint8_t phase, uint16_t rows_copied,
    int64_t started_us, int64_t finished_us)
{
    if (model.value.state == CERBERUS_WIFI_READOUT_IDLE) return;
    increment(&model.value.step_calls);
    /* finish can occur inside this span, so started_us may precede the most
     * recently published terminal sample. Compare it with the prior step. */
    if (started_us < 0 || finished_us < started_us ||
        (started_us >= 0 && ((uint64_t)started_us < model.began_us ||
            (model.stepped && (uint64_t)started_us < model.last_step_end_us))))
        invalidate_time();
    observe_time(finished_us);
    if (model.value.timing_valid) {
        uint64_t duration = (uint64_t)(finished_us - started_us);
        if (duration > model.value.max_step_us) model.value.max_step_us = duration;
        if (model.stepped) {
            uint64_t gap = (uint64_t)started_us - model.last_step_start_us;
            if (gap > model.value.max_tick_gap_us) model.value.max_tick_gap_us = gap;
        }
        if (model.value.state == CERBERUS_WIFI_READOUT_READING)
            model.value.elapsed_us = (uint64_t)finished_us - model.began_us;
    }
    if (started_us >= 0 && finished_us >= started_us) {
        model.last_step_start_us = (uint64_t)started_us;
        model.last_step_end_us = (uint64_t)finished_us;
        model.stepped = true;
    }
    if (model.value.state == CERBERUS_WIFI_READOUT_READING) {
        model.value.phase = phase;
        model.value.rows_copied = rows_copied;
    }
    publish();
}

void cerberus_wifi_readout_diagnostics_finish(cerberus_wifi_readout_outcome_t outcome,
    cerberus_wifi_readout_cancel_reason_t cancel_reason, int32_t error,
    uint8_t phase, uint16_t rows_copied, int64_t now_us)
{
    if (model.value.state != CERBERUS_WIFI_READOUT_READING ||
        outcome < CERBERUS_WIFI_READOUT_COMPLETE || outcome > CERBERUS_WIFI_READOUT_CANCELLED)
        return;
    observe_time(now_us);
    if (model.value.timing_valid) model.value.elapsed_us = (uint64_t)now_us - model.began_us;
    model.value.state = (uint8_t)outcome;
    model.value.phase = phase;
    model.value.error = error;
    model.value.rows_copied = rows_copied;
    model.value.cancel_reason = outcome == CERBERUS_WIFI_READOUT_CANCELLED ? (uint8_t)cancel_reason :
        CERBERUS_WIFI_READOUT_CANCEL_NONE;
    if (outcome == CERBERUS_WIFI_READOUT_COMPLETE) increment(&model.value.completed);
    else if (outcome == CERBERUS_WIFI_READOUT_FAILED) increment(&model.value.failed);
    else increment(&model.value.cancelled);
    publish();
}

void cerberus_wifi_readout_diagnostics_get(cerberus_wifi_readout_diagnostics_t *snapshot)
{
    if (!snapshot) return;
    portENTER_CRITICAL(&diagnostics_lock);
    *snapshot = published;
    portEXIT_CRITICAL(&diagnostics_lock);
}
