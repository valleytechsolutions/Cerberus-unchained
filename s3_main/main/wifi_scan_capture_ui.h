/* UI-owned passive AP readout. Small bounded batches of single-attempt steps;
 * no task, RF command, per-row allocation or partial-bank publication. */
enum {
    LIVE_WIFI_BEFORE = 1, LIVE_WIFI_ROW_BEGIN, LIVE_WIFI_ROW, LIVE_WIFI_STATUS,
    LIVE_WIFI_AFTER_BEGIN, LIVE_WIFI_AFTER, LIVE_WIFI_COMMIT, LIVE_WIFI_DONE
};
enum { LIVE_WIFI_BATCH_MAX_STEPS = 4, LIVE_WIFI_BATCH_BUDGET_US = 2000 };
static bool live_wifi_attempted, live_wifi_worked;
static uint8_t live_wifi_attempt_generation;
static uint32_t live_wifi_attempt_arm;
static esp_err_t live_wifi_error;

static bool live_wifi_capture_busy(void)
{
    return live_store && live_store->wifi_transfer.phase >= LIVE_WIFI_BEFORE &&
        live_store->wifi_transfer.phase <= LIVE_WIFI_COMMIT;
}
static bool live_wifi_capture_defers_c5(void)
{ return live_wifi_capture_busy() || live_wifi_worked; }
static esp_err_t live_wifi_capture_error(void) { return live_wifi_error; }

static void live_wifi_capture_clear(void)
{
    if (live_wifi_capture_busy()) {
        c5_spi_link_wifi_scan_result_cancel(&live_store->wifi_transfer.row);
        c5_spi_link_wifi_relation_cancel(&live_store->wifi_transfer.fence);
        live_store->wifi_transfer.phase = LIVE_WIFI_DONE;
        memset(live_store->wifi_staging, 0, sizeof(live_store->wifi_staging));
        memset(live_store->wifi_staging_observations, 0,
            sizeof(live_store->wifi_staging_observations));
    }
}
static void live_wifi_capture_cancel(cerberus_wifi_readout_cancel_reason_t reason)
{
    if (live_wifi_capture_busy())
        cerberus_wifi_readout_diagnostics_finish(CERBERUS_WIFI_READOUT_CANCELLED,
            reason, ESP_OK, live_store->wifi_transfer.phase,
            live_store->wifi_transfer.next, esp_timer_get_time());
    live_wifi_capture_clear();
}
static void live_wifi_capture_reset_as(cerberus_wifi_readout_cancel_reason_t reason)
{
    live_wifi_capture_cancel(reason);
    live_wifi_attempted = false;
    live_wifi_error = ESP_OK;
}
static void live_wifi_capture_reset(void)
{ live_wifi_capture_reset_as(CERBERUS_WIFI_READOUT_CANCEL_RESET); }
static bool live_wifi_capture_fail(esp_err_t error)
{
    const bool busy = live_wifi_capture_busy();
    cerberus_wifi_readout_diagnostics_finish(CERBERUS_WIFI_READOUT_FAILED,
        CERBERUS_WIFI_READOUT_CANCEL_NONE, error,
        busy ? live_store->wifi_transfer.phase : 0U,
        busy ? live_store->wifi_transfer.next : 0U, esp_timer_get_time());
    live_wifi_capture_clear();
    live_wifi_error = error;
    live_records[SENTINEL_SCAN_WIFI].error = error;
    return true;
}

static bool live_wifi_capture_begin(uint32_t now)
{
    live_scan_record_t *record = &live_records[SENTINEL_SCAN_WIFI];
    /* Only the sweep owned by the current explicit arm may start a transfer.
     * A stale COMPLETE poll after Scan Again must not delay the new START. */
    if (live_session.desired != SENTINEL_SCAN_WIFI || live_session.paused ||
        live_session.active != SENTINEL_SCAN_WIFI ||
        live_session.active_arm_generation != live_session.arm_generation)
        return false;
    if (live_wifi_capture_busy()) return false;
    if (live_wifi_attempted && live_wifi_attempt_generation == record->generation &&
        live_wifi_attempt_arm == live_session.arm_generation) return false;
    live_wifi_attempted = true;
    live_wifi_attempt_generation = record->generation;
    live_wifi_attempt_arm = live_session.arm_generation;
    live_wifi_error = ESP_OK;
    cerberus_wifi_readout_diagnostics_begin(wifi_scan_status.stored_count, 0U,
        esp_timer_get_time());
    if (wifi_scan_status.stored_count > CERBERUS_WIFI_SCAN_MAX_RESULTS)
        return live_wifi_capture_fail(ESP_ERR_INVALID_RESPONSE);
    if (!live_store_acquire()) return live_wifi_capture_fail(ESP_ERR_NO_MEM);
    cerberus_scan_wifi_transfer_t *transfer = &live_store->wifi_transfer;
    memset(transfer, 0, sizeof(*transfer));
    memset(live_store->wifi_staging, 0, sizeof(live_store->wifi_staging));
    memset(live_store->wifi_staging_observations, 0,
        sizeof(live_store->wifi_staging_observations));
    transfer->generation = record->generation;
    transfer->count = wifi_scan_status.stored_count;
    transfer->arm = live_session.arm_generation;
    transfer->deadline_ms = now + 8000U;
    transfer->relation_error = ESP_ERR_NOT_SUPPORTED;
    transfer->phase = LIVE_WIFI_ROW_BEGIN;
    if (wifi_relation_supported()) {
        transfer->relation_error = c5_spi_link_wifi_relation_begin_check(
            &transfer->fence, transfer->generation, transfer->deadline_ms);
        if (transfer->relation_error != ESP_OK)
            return live_wifi_capture_fail(transfer->relation_error);
        transfer->phase = LIVE_WIFI_BEFORE;
    }
    return true;
}

static bool live_wifi_capture_tick_impl(uint32_t now, bool allowed,
    cerberus_wifi_readout_cancel_reason_t blocked_reason)
{
    live_wifi_worked = false;
    if (!live_wifi_capture_busy()) return false;
    cerberus_scan_wifi_transfer_t *transfer = &live_store->wifi_transfer;
    live_scan_record_t *record = &live_records[SENTINEL_SCAN_WIFI];
    if (!allowed || live_session.paused || transfer->arm != live_session.arm_generation ||
        !record->valid || record->state != SENTINEL_SCAN_COMPLETE ||
        record->generation != transfer->generation) {
        live_wifi_capture_cancel(!allowed ? blocked_reason : live_session.paused ?
            CERBERUS_WIFI_READOUT_CANCEL_STOP : CERBERUS_WIFI_READOUT_CANCEL_SUPERSEDED);
        return true;
    }
    uint32_t remaining = transfer->deadline_ms - now;
    if (!remaining || remaining > INT32_MAX) return live_wifi_capture_fail(ESP_ERR_TIMEOUT);
    bool complete = false;
    esp_err_t error = ESP_OK;
    switch (transfer->phase) {
    case LIVE_WIFI_BEFORE:
    case LIVE_WIFI_AFTER: {
        const bool before = transfer->phase == LIVE_WIFI_BEFORE;
        cerberus_c5_wifi_relation_bank_t proof = {0};
        live_wifi_worked = true;
        error = c5_spi_link_wifi_relation_step(&transfer->fence, &proof, &complete, NULL, NULL);
        /* A supported peer changing epoch/rebooting must never degrade to the
         * generation-byte legacy fallback while rows span multiple ticks. */
        if (error != ESP_OK) return live_wifi_capture_fail(error);
        if (complete) {
            transfer->relation_error = error;
            if (before && error == ESP_OK) transfer->proof = proof;
            transfer->phase = before ? LIVE_WIFI_ROW_BEGIN : LIVE_WIFI_COMMIT;
        }
        break;
    }
    case LIVE_WIFI_ROW_BEGIN:
        if (transfer->next == transfer->count) transfer->phase = LIVE_WIFI_STATUS;
        else {
            error = c5_spi_link_wifi_scan_result_begin(&transfer->row,
                transfer->generation, transfer->next, transfer->deadline_ms);
            if (error != ESP_OK) return live_wifi_capture_fail(error);
            transfer->phase = LIVE_WIFI_ROW;
        }
        break;
    case LIVE_WIFI_ROW:
        live_wifi_worked = true;
        error = c5_spi_link_wifi_scan_result_step(&transfer->row,
            &live_store->wifi_staging[transfer->next],
            &live_store->wifi_staging_observations[transfer->next], &complete);
        if (error != ESP_OK) return live_wifi_capture_fail(error);
        if (complete) { ++transfer->next; transfer->phase = LIVE_WIFI_ROW_BEGIN; }
        break;
    case LIVE_WIFI_STATUS: {
        cerberus_wifi_scan_status_payload_t end = {0};
        live_wifi_worked = true;
        error = c5_spi_link_get_wifi_scan_status_once(transfer->deadline_ms, &end);
        if (error == ESP_OK && (end.state != CERBERUS_WIFI_SCAN_COMPLETE ||
            end.generation != transfer->generation || end.stored_count != transfer->count))
            error = ESP_ERR_INVALID_STATE;
        if (error != ESP_OK) return live_wifi_capture_fail(error);
        transfer->phase = transfer->relation_error == ESP_OK ?
            LIVE_WIFI_AFTER_BEGIN : LIVE_WIFI_COMMIT;
        break;
    }
    case LIVE_WIFI_AFTER_BEGIN:
        transfer->relation_error = c5_spi_link_wifi_relation_begin_check_expected(
            &transfer->fence, &transfer->proof, transfer->deadline_ms);
        if (transfer->relation_error != ESP_OK)
            return live_wifi_capture_fail(transfer->relation_error);
        transfer->phase = LIVE_WIFI_AFTER;
        break;
    case LIVE_WIFI_COMMIT:
        memcpy(live_store->wifi, live_store->wifi_staging,
            transfer->count * sizeof(live_store->wifi[0]));
        memcpy(live_store->wifi_observations, live_store->wifi_staging_observations,
            transfer->count * sizeof(live_store->wifi_observations[0]));
        record->count = transfer->count;
        record->captured = true;
        record->saved_generation = transfer->generation;
        record->captured_ms = now;
        record->error = ESP_OK;
        live_wifi_published_us = esp_timer_get_time();
        wifi_commit_cached_results(live_store->wifi, transfer->count, transfer->generation);
        wifi_relation_publish(transfer->generation, &transfer->proof, transfer->relation_error);
        live_capture_pending &= ~(UINT32_C(1) << SENTINEL_SCAN_WIFI);
        if (live_page[SENTINEL_SCAN_WIFI] * 4U >= transfer->count)
            live_page[SENTINEL_SCAN_WIFI] = transfer->count ? (transfer->count - 1U) / 4U : 0U;
        cerberus_wifi_readout_diagnostics_finish(CERBERUS_WIFI_READOUT_COMPLETE,
            CERBERUS_WIFI_READOUT_CANCEL_NONE, ESP_OK, transfer->phase,
            transfer->next, esp_timer_get_time());
        transfer->phase = LIVE_WIFI_DONE;
        return true;
    default: return live_wifi_capture_fail(ESP_ERR_INVALID_STATE);
    }
    return false;
}

static bool live_wifi_capture_tick(uint32_t now, bool allowed,
    cerberus_wifi_readout_cancel_reason_t blocked_reason)
{
    live_wifi_worked = false;
    if (!live_wifi_capture_busy()) return false;
    const int64_t started_us = esp_timer_get_time();
    int64_t sampled_us = started_us;
    bool clock_valid = started_us >= 0;
    bool changed = false, worked = false;
    if (!clock_valid) changed = live_wifi_capture_fail(ESP_ERR_INVALID_STATE);
    for (unsigned step = 0; clock_valid && live_wifi_capture_busy() &&
            step < LIVE_WIFI_BATCH_MAX_STEPS; ++step) {
        const int64_t previous_us = sampled_us;
        if (step) sampled_us = esp_timer_get_time();
        if (sampled_us < previous_us) {
            clock_valid = false;
            changed = live_wifi_capture_fail(ESP_ERR_INVALID_STATE) || changed;
            break;
        }
        /* Admission only: an in-flight exchange retains its existing mutex/
         * READY timeout and may overrun this budget. Never start another step
         * afterward. The count limit also bounds a frozen monotonic clock. */
        if ((uint64_t)(sampled_us - started_us) >= LIVE_WIFI_BATCH_BUDGET_US) break;
        cerberus_scan_wifi_transfer_t *transfer = &live_store->wifi_transfer;
        const uint8_t phase = transfer->phase, next = transfer->next;
        const uint8_t row_phase = transfer->row.phase, copied = transfer->row.copied;
        const uint8_t fence_phase = transfer->fence.phase;
        /* Refresh the absolute deadline clock even for local COMMIT; the
         * caller's UI-loop timestamp may predate other service work. */
        now = (uint32_t)((uint64_t)sampled_us / 1000U);
        changed = live_wifi_capture_tick_impl(now, allowed, blocked_reason) || changed;
        worked |= live_wifi_worked;
        const int64_t finished_us = esp_timer_get_time();
        if (finished_us < sampled_us) {
            clock_valid = false;
            if (live_wifi_capture_busy())
                changed = live_wifi_capture_fail(ESP_ERR_INVALID_STATE) || changed;
        }
        sampled_us = finished_us;
        if (!clock_valid || !live_wifi_capture_busy() ||
            (uint64_t)(sampled_us - started_us) >= LIVE_WIFI_BATCH_BUDGET_US) break;
        /* Check-only fences advance phase; SSID chunks may advance copied
         * without changing row phase. Time samples alone are not progress. */
        if (phase == transfer->phase && next == transfer->next &&
            row_phase == transfer->row.phase && copied == transfer->row.copied &&
            fence_phase == transfer->fence.phase) break;
    }
    const int64_t finished_us = esp_timer_get_time();
    if (clock_valid && finished_us < sampled_us) {
        clock_valid = false;
        if (live_wifi_capture_busy())
            changed = live_wifi_capture_fail(ESP_ERR_INVALID_STATE) || changed;
    }
    /* A trailing local transition must not clear earlier I/O ownership and
     * enable unrelated C5 telemetry in this same UI iteration. */
    live_wifi_worked = worked;
    /* A negative start marks the entire measurement invalid if any intermediate
     * sample went backward, even when the final clock value recovered. */
    cerberus_wifi_readout_diagnostics_step(live_store->wifi_transfer.phase,
        live_store->wifi_transfer.next, clock_valid ? started_us : -1, finished_us);
    return changed;
}
