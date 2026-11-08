#pragma once

/* Local S3 admission is definite. Only a token returned by our own passive
 * Start can supply observations or authorize cleanup; global status and the
 * public generation byte are never a substitute for this lease. */
static uint64_t live_ble_lease_epoch, live_ble_capture_epoch;
static cerberus_s3_ble_passive_state_t live_ble_lease_state, live_ble_capture_state;
static bool live_ble_stop_pending, live_ble_sleeping;
static esp_err_t live_ble_lease_error;

static bool live_ble_recon_released(void) { return live_ble_lease_epoch == 0U; }

static bool live_ble_fence_equal(const cerberus_s3_ble_passive_state_t *a,
    const cerberus_s3_ble_passive_state_t *b)
{
    return a->epoch && a->scan_epoch && a->attached && b->attached && !a->owned && !b->owned &&
        !a->stopping && !b->stopping && !a->error && !b->error &&
        a->scan.state == CERBERUS_BLE_SCAN_COMPLETE &&
        b->scan.state == CERBERUS_BLE_SCAN_COMPLETE &&
        a->epoch == b->epoch && a->scan_epoch == b->scan_epoch &&
        a->host_epoch == b->host_epoch && a->started_ms == b->started_ms &&
        !memcmp(&a->scan, &b->scan, sizeof(a->scan));
}

static esp_err_t live_ble_capture_validate(void)
{
    if (!live_ble_capture_epoch || live_ble_capture_epoch != live_ble_lease_epoch)
        return ESP_ERR_INVALID_STATE;
    cerberus_s3_ble_passive_state_t end = {0};
    esp_err_t error = cerberus_s3_ble_scanner_passive_get_state(live_ble_capture_epoch, &end);
    if (error == ESP_OK && !live_ble_fence_equal(&live_ble_capture_state, &end))
        error = ESP_ERR_INVALID_STATE;
    return error;
}

static void live_ble_forget_lease(void)
{
    live_ble_lease_epoch = 0U;
    memset(&live_ble_lease_state, 0, sizeof(live_ble_lease_state));
    live_ble_stop_pending = false;
}

static void live_ble_observe(void)
{
    cerberus_s3_ble_passive_state_t state = {0};
    esp_err_t error = ESP_OK;
    bool abandon = live_ble_sleeping || live_ble_session.desired == SENTINEL_SCAN_NONE ||
        live_ble_session.paused || live_ble_session.faulted;
    if (live_ble_lease_epoch) {
        error = cerberus_s3_ble_scanner_passive_get_state(live_ble_lease_epoch, &state);
        if (error == ESP_ERR_NOT_FOUND) {
            /* Authoritative absence of our token cannot grant permission over
             * the scanner which replaced it. Retained publications stay put. */
            live_ble_forget_lease();
            live_ble_lease_error = ESP_ERR_INVALID_STATE;
            error = ESP_OK;
        } else if (error == ESP_OK) {
            live_ble_lease_state = state;
            if (state.error) live_ble_lease_error = state.error;
            if (state.error && state.owned) live_ble_stop_pending = true;
        }
        if (live_ble_lease_epoch &&
            (live_ble_stop_pending || abandon || live_ble_lease_error != ESP_OK) &&
            (error != ESP_OK || state.owned)) {
            /* Continue exact cleanup even after scheduler retry exhaustion.
             * Neither a timeout nor an unknown observation releases a lease. */
            live_ble_stop_pending = true;
            error = cerberus_s3_ble_scanner_passive_stop(live_ble_lease_epoch, &state);
            if (error == ESP_ERR_NOT_FOUND) {
                live_ble_forget_lease();
                live_ble_lease_error = ESP_ERR_INVALID_STATE;
                error = ESP_OK;
            } else if (error == ESP_OK) live_ble_lease_state = state;
        }
        if (live_ble_lease_epoch && error == ESP_OK && !state.owned &&
            (live_ble_stop_pending || abandon || live_ble_lease_error != ESP_OK)) {
            error = cerberus_s3_ble_scanner_passive_release(live_ble_lease_epoch);
            if (error == ESP_OK || error == ESP_ERR_NOT_FOUND) {
                live_ble_forget_lease();
                memset(&state, 0, sizeof(state));
                error = ESP_OK;
            }
        }
    }
    ble_scan_checked = true;
    ble_scan_error = error;
    if (error != ESP_OK) {
        /* Keep the previous lease, but expose no fresh global scan state. */
        live_ble_lease_error = error;
        memset(&ble_scan_status, 0, sizeof(ble_scan_status));
        live_status(SENTINEL_SCAN_BLE, error, SENTINEL_SCAN_ERROR, 0U, 0U, error);
        return;
    }
    ble_scan_status = state.scan;
    uint8_t observed = state.owned ? SENTINEL_SCAN_RUNNING : state.scan.state;
    if (!state.attached) observed = live_ble_lease_error ? SENTINEL_SCAN_ERROR : SENTINEL_SCAN_IDLE;
    else if (state.error && !state.owned) observed = SENTINEL_SCAN_ERROR;
    live_status(SENTINEL_SCAN_BLE, ESP_OK, observed, state.scan.generation, 0U,
        live_ble_lease_error ? live_ble_lease_error : state.error);
    /* A held cleanup error is not IDLE or a foreign RUNNING observation.
     * Keep it visible while the exact owner continues to drain. */
    if (live_ble_lease_error) live_records[SENTINEL_SCAN_BLE].error = live_ble_lease_error;
}

static esp_err_t live_ble_execute(bool start)
{
    cerberus_s3_ble_passive_state_t state = {0};
    esp_err_t error;
    if (start) {
        error = cerberus_s3_ble_scanner_passive_start(live_ble_lease_epoch, &state);
        if (state.attached && state.epoch) {
            live_ble_lease_epoch = state.epoch;
            live_ble_lease_state = state;
        }
        if (error == ESP_OK && (!state.attached || !state.epoch)) error = ESP_ERR_INVALID_RESPONSE;
        live_ble_lease_error = error;
        live_ble_stop_pending = error != ESP_OK && live_ble_lease_epoch != 0U;
    } else {
        if (!live_ble_lease_epoch) return ESP_OK;
        live_ble_stop_pending = true;
        error = cerberus_s3_ble_scanner_passive_stop(live_ble_lease_epoch, &state);
        if (error == ESP_ERR_NOT_FOUND) {
            live_ble_forget_lease();
            return ESP_OK;
        }
        if (error == ESP_OK) live_ble_lease_state = state;
        else live_ble_lease_error = error;
    }
    if (state.attached) ble_scan_status = state.scan;
    return error;
}
