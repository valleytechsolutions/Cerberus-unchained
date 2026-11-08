#pragma once

/* The page requests both receivers; a local S3 attachment is not permission
 * for C5 and a C5 error is not permission to restart S3. Only the initial
 * transition waits for the previous owners to release together. */
static bool ble_analyzer_requested(void) { return ble_analyzer_run_requested; }
static bool ble_analyzer_ready(void)
{ return !ble_analyzer_start_pending && ble_analyzer_barrier_error == ESP_OK; }
static bool ble_analyzer_s3_receiving(void)
{ return ble_analyzer_started && ble_analyzer_s3_owned && ble_analyzer_s3_receiving_cached; }
static esp_err_t ble_analyzer_s3_error(void)
{
    return ble_analyzer_cleanup_timed_out ? ESP_ERR_TIMEOUT :
        ble_analyzer_barrier_error ? ble_analyzer_barrier_error : ble_analyzer_error;
}
static const char *ble_analyzer_s3_state(void)
{
    if (ble_analyzer_exit_pending)
        return ble_analyzer_cleanup_timed_out ? "STOPPING BOTH / TIMEOUT" : "STOPPING BOTH";
    if (ble_analyzer_barrier_error) return "START BLOCKED / RETRY START";
    if (ble_analyzer_start_pending) return "WAITING FOR PREVIOUS RECEIVERS";
    if (ble_analyzer_error) return "S3 ERROR / C5 INDEPENDENT";
    if (ble_analyzer_s3_receiving()) return "S3 LISTENING";
    if (ble_analyzer_s3_owned) return "S3 WAITING FOR RADIO";
    if (ble_analyzer_retry_requested || ble_analyzer_s3_start_pending) return "S3 WAITING TO START";
    if (ble_analyzer_started) return "S3 BETWEEN SWEEPS";
    return ble_analyzer_requested() ? "S3 WAITING" : "S3 STOPPED";
}
static bool ble_analyzer_s3_retryable(void)
{
    return current_page == UI_PAGE_BLE && ble_mode == UI_BLE_MODE_ANALYZER &&
        ble_analyzer_requested() && !ble_analyzer_exit_pending &&
        (ble_analyzer_barrier_error != ESP_OK || ble_analyzer_error != ESP_OK);
}

static void ble_analyzer_open(uint32_t now)
{
    ble_discovery_leave();
    dual_ble_ui_retry();
    ble_analyzer_s3_details = false;
    ble_mode = UI_BLE_MODE_ANALYZER;
    ble_session_exit_page = UI_PAGE_BLE;
    ble_analyzer_run_requested = true;
    ble_analyzer_start_pending = true;
    ble_analyzer_started = ble_analyzer_exit_pending = false;
    ble_analyzer_retry_requested = true; /* Discard any retained prior Analyzer attachment before this run. */
    ble_analyzer_s3_owned = ble_analyzer_s3_receiving_cached = false;
    ble_analyzer_s3_start_pending = ble_analyzer_cleanup_timed_out = false;
    ble_analyzer_error = ble_analyzer_barrier_error = ESP_OK;
    ble_analyzer_last_tick_ms = ble_analyzer_last_draw_ms = 0U;
    ble_analyzer_exit_started_ms = 0U;
    ble_analyzer_start_requested_ms = now;
}

static void ble_analyzer_request_s3_retry(uint32_t now)
{
    if (!ble_analyzer_s3_retryable()) return;
    if (ble_analyzer_barrier_error) {
        ble_analyzer_barrier_error = ESP_OK;
        ble_analyzer_start_pending = true;
        ble_analyzer_start_requested_ms = now;
    } else {
        ble_analyzer_retry_requested = true;
        ble_analyzer_s3_start_pending = true;
        ble_analyzer_s3_start_requested_ms = now;
    }
    ble_analyzer_error = ESP_OK;
    ble_analyzer_last_tick_ms = 0U;
}

static void ble_analyzer_request_c5_retry(uint32_t now)
{
    (void)now;
    if (ble_analyzer_c5_retryable()) dual_ble_ui_retry();
}

static void ble_analyzer_request_stop(uint32_t now, bool asleep)
{
    ble_analyzer_run_requested = false;
    ble_analyzer_start_pending = ble_analyzer_s3_start_pending = ble_analyzer_retry_requested = false;
    if (!ble_analyzer_exit_pending) {
        ble_analyzer_exit_pending = true;
        ble_analyzer_exit_started_ms = now;
    }
    if (asleep) ble_session_exit_page = UI_PAGE_BLE;
    /* Pure cancellation includes INFO with no allocated/accepted C5 job. A
     * hidden stale Analyzer must never cancel a newer companion workflow. */
    if (dual_ble_ui_mode == CERBERUS_DUAL_BLE_ANALYZER) dual_ble_ui_cancel();
    ble_analyzer_last_tick_ms = 0U;
}

static bool ble_analyzer_c5_released(void)
{
    return dual_ble_ui_released() ||
        (dual_ble_ui_mode != CERBERUS_DUAL_BLE_ANALYZER &&
         (!dual_ble_ui_store || dual_ble_ui_store->start.mode != CERBERUS_DUAL_BLE_ANALYZER));
}

static void ble_analyzer_observe_s3(const cerberus_s3_ble_analyzer_state_t *state)
{
    ble_analyzer_started = state->attached;
    ble_analyzer_s3_owned = state->owned;
    ble_analyzer_s3_receiving_cached = state->owned && state->receiving;
}

static void ble_analyzer_finish_exit(bool asleep, bool host_held)
{
    /* An elapsed deadline is never proof of release. The caller has already
     * released the exact S3 attachment and waits for this C5 workflow only. */
    if (host_held || ble_analyzer_started || !ble_analyzer_c5_released()) return;
    ble_analyzer_run_requested = false;
    ble_analyzer_start_pending = ble_analyzer_exit_pending = ble_analyzer_retry_requested = false;
    ble_analyzer_s3_start_pending = ble_analyzer_s3_owned = ble_analyzer_s3_receiving_cached = false;
    ble_analyzer_cleanup_timed_out = false;
    ble_analyzer_error = ble_analyzer_barrier_error = ESP_OK;
    ble_analyzer_exit_started_ms = 0U;
    if (current_page != UI_PAGE_BLE || ble_mode != UI_BLE_MODE_ANALYZER) return;
    ble_mode = UI_BLE_MODE_MENU;
    ui_page_t exit_page = ble_session_exit_page;
    ble_session_exit_page = UI_PAGE_BLE;
    if (exit_page == UI_PAGE_BLE_ATTACK) {
        ble_attack_selecting = false;
        open_tool_page(UI_PAGE_BLE_ATTACK);
    } else if (exit_page != UI_PAGE_BLE) return_to_launcher();
    else if (!asleep) ESP_ERROR_CHECK(draw_detail_page());
}

static void ble_analyzer_tick(uint32_t now_ms, bool asleep)
{
    bool visible = current_page == UI_PAGE_BLE && ble_mode == UI_BLE_MODE_ANALYZER;
    if (!visible && !ble_analyzer_requested() && !ble_analyzer_started && !ble_analyzer_exit_pending) return;
    if ((asleep || !visible) && !ble_analyzer_exit_pending)
        ble_analyzer_request_stop(now_ms, asleep);
    if (ble_analyzer_last_tick_ms && (uint32_t)(now_ms - ble_analyzer_last_tick_ms) < 250U) return;
    ble_analyzer_last_tick_ms = now_ms;

    if (ble_analyzer_start_pending) {
        bool previous_released = dual_ble_live_discovery_released() && live_ble_recon_released() && dual_ble_ui_released() &&
            live_session.active == SENTINEL_SCAN_NONE && live_session.pending.kind == SENTINEL_SCAN_COMMAND_NONE &&
            live_ble_session.active == SENTINEL_SCAN_NONE && live_ble_session.pending.kind == SENTINEL_SCAN_COMMAND_NONE &&
            cerberus_s3_ble_host_owner() == CERBERUS_S3_BLE_OWNER_NONE;
        if ((uint32_t)(now_ms - ble_analyzer_start_requested_ms) >= 12000U) {
            ble_analyzer_start_pending = false;
            ble_analyzer_barrier_error = ESP_ERR_TIMEOUT;
        } else if (previous_released) {
            ble_analyzer_start_pending = false;
            ble_analyzer_s3_start_pending = true;
            ble_analyzer_s3_start_requested_ms = now_ms;
        }
    }

    cerberus_s3_ble_analyzer_state_t state = {0};
    esp_err_t error = cerberus_s3_ble_scanner_analyzer_get_state(&state);
    if (error == ESP_OK) {
        ble_analyzer_observe_s3(&state);
        if (state.error && !ble_analyzer_retry_requested) ble_analyzer_error = state.error;
    } else ble_analyzer_error = error;

    if (ble_analyzer_s3_start_pending &&
        (uint32_t)(now_ms - ble_analyzer_s3_start_requested_ms) >= 12000U) {
        ble_analyzer_error = ESP_ERR_TIMEOUT;
        ble_analyzer_s3_start_pending = ble_analyzer_retry_requested = false;
    }
    bool cleanup = ble_analyzer_exit_pending || ble_analyzer_retry_requested || ble_analyzer_error != ESP_OK;
    if (cleanup && ble_analyzer_started) {
        if (error != ESP_OK || state.owned) {
            error = cerberus_s3_ble_scanner_analyzer_stop(&state);
            if (error == ESP_OK) ble_analyzer_observe_s3(&state);
            else if (error != ESP_ERR_INVALID_STATE) ble_analyzer_error = error;
        }
        if (error == ESP_OK && !state.owned) {
            error = cerberus_s3_ble_scanner_analyzer_release();
            if (error == ESP_OK) {
                ble_analyzer_started = ble_analyzer_s3_owned = ble_analyzer_s3_receiving_cached = false;
                memset(&state, 0, sizeof(state));
            } else ble_analyzer_error = error;
        }
    }
    if (ble_analyzer_exit_pending) {
        if (!ble_analyzer_started && ble_analyzer_c5_released() && dual_ble_live_discovery_released()) {
            ble_analyzer_finish_exit(asleep, false);
            return;
        }
        if ((uint32_t)(now_ms - ble_analyzer_exit_started_ms) >= 12000U)
            ble_analyzer_cleanup_timed_out = true;
    } else if (ble_analyzer_requested() && ble_analyzer_ready() && error == ESP_OK) {
        if (ble_analyzer_retry_requested && !ble_analyzer_started) {
            ble_analyzer_retry_requested = false;
            ble_analyzer_error = ESP_OK;
        }
        if (!ble_analyzer_error && !ble_analyzer_retry_requested) {
            if (!ble_analyzer_started && ble_analyzer_s3_start_pending) {
                error = cerberus_s3_ble_scanner_analyzer_begin();
                if (error == ESP_OK) ble_analyzer_started = true;
            }
            if (error == ESP_OK && ble_analyzer_started && !state.owned) {
                error = cerberus_s3_ble_scanner_analyzer_start(&state);
                if (error == ESP_OK) {
                    ble_analyzer_observe_s3(&state);
                    ble_analyzer_s3_start_pending = false;
                }
            }
            if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
                ble_analyzer_error = error;
                ble_analyzer_s3_start_pending = false;
            } else if (error == ESP_ERR_INVALID_STATE && !ble_analyzer_s3_start_pending) {
                ble_analyzer_s3_start_pending = true;
                ble_analyzer_s3_start_requested_ms = now_ms;
            }
        }
    }
    if (visible && !asleep && (!ble_analyzer_last_draw_ms ||
        (uint32_t)(now_ms - ble_analyzer_last_draw_ms) >= UI_BLE_REFRESH_MS)) {
        ble_analyzer_last_draw_ms = now_ms;
        ESP_ERROR_CHECK(draw_detail_page());
    }
}
