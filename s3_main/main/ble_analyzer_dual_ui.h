#pragma once

/* Cached presentation only. Each graph retains its own callback scope and
 * clock denominator; C5 intervals are not reconstructed S3 time buckets. */
static bool ble_analyzer_c5_retryable(void)
{
    /* Only the current Analyzer request may retry this source. An old store's
     * error and an owner still draining are not permission to start a job. */
    if (current_page != UI_PAGE_BLE || ble_mode != UI_BLE_MODE_ANALYZER ||
        !ble_analyzer_requested() || !ble_analyzer_ready() ||
        ble_analyzer_exit_pending || dual_ble_ui_mode != CERBERUS_DUAL_BLE_ANALYZER ||
        !dual_ble_ui_attempted || !dual_ble_ui_released()) return false;
    cerberus_dual_ble_analyzer_view_t view;
    dual_ble_ui_get_analyzer_view(&view);
    return view.error != ESP_OK && view.error != ESP_ERR_NOT_SUPPORTED && !view.stopping;
}

/* The same cached-only controls appear in both receiver and S3-detail views.
 * A retry action is source-specific; drawing never advances either owner. */
static esp_err_t draw_ble_analyzer_retry_controls(void)
{
    if (ble_analyzer_s3_retryable())
        ESP_RETURN_ON_ERROR(draw_button(8, 378, 148, 40,
            ble_analyzer_barrier_error != ESP_OK ? "RETRY START" : "RETRY S3"),
            TAG, "analyzer S3 retry");
    if (ble_analyzer_c5_retryable())
        ESP_RETURN_ON_ERROR(draw_button(164, 378, 148, 40, "RETRY C5"),
            TAG, "analyzer C5 retry");
    return ESP_OK;
}

static esp_err_t draw_ble_analyzer_both(void)
{
    cerberus_ble_analyzer_view_t s3 = {0};
    esp_err_t s3_error = cerberus_s3_ble_scanner_analyzer_status(&s3);
    cerberus_dual_ble_analyzer_view_t c5;
    dual_ble_ui_get_analyzer_view(&c5);
    char line[80];
    ESP_RETURN_ON_ERROR(draw_small_button(8, 54, 304, 26,
        "BOTH RECEIVERS / S3 DETAILS >"), TAG, "analyzer view tab");
    ESP_RETURN_ON_ERROR(draw_centered(94, ble_analyzer_s3_state(),
        ble_analyzer_s3_error() != ESP_OK ? UI_BAD : UI_TEXT, 1), TAG, "analyzer S3 state");
    bool s3_sample = s3_error == ESP_OK;
    if (s3_sample) snprintf(line, sizeof(line), "S3 CALLBACKS %s%lu / OPEN BUCKET %s%u",
        s3.total_events == UINT32_MAX ? ">=" : "", (unsigned long)s3.total_events,
        s3.current_events == UINT16_MAX ? ">=" : "", (unsigned)s3.current_events);
    else snprintf(line, sizeof(line), "S3 CALLBACKS UNKNOWN / NO SOURCE SAMPLE");
    ESP_RETURN_ON_ERROR(draw_centered(110, line, UI_TEXT, 1), TAG, "analyzer S3 count");
    cerberus_ble_analyzer_window_t window = {0};
    bool valid = s3_sample && cerberus_ble_analyzer_window(&s3, &window);
    if (valid) snprintf(line, sizeof(line), "S3: %u COMPLETE 200ms BUCKETS / ADV+RSP", window.buckets);
    else snprintf(line, sizeof(line), "S3: NO SOURCE WINDOW / ADV+RSP");
    ESP_RETURN_ON_ERROR(draw_centered(126, line, UI_MUTED, 1), TAG, "analyzer S3 denominator");
    ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(20, 142, 280, 60, UI_PANEL), TAG, "analyzer S3 graph");
    uint32_t scale = valid && window.peak ? window.peak : 1U;
    for (unsigned x = 0U; valid && x < CERBERUS_BLE_ANALYZER_HISTORY; ++x) {
        unsigned age = CERBERUS_BLE_ANALYZER_HISTORY - 1U - x;
        uint16_t count = age < window.buckets ? s3.history[age] : 0U;
        if (!count) continue;
        uint16_t height = (uint16_t)((uint32_t)count * 52U / scale);
        if (!height) height = 1U;
        ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(32U + x * 4U, 198U - height,
            3, height, UI_GOOD), TAG, "analyzer S3 bar");
    }
    const char *c5_state = c5.stopping ? "C5 STOPPING" :
        c5.error == ESP_ERR_NOT_SUPPORTED ? "C5 UNAVAILABLE / NEEDS FIRMWARE 65" :
        c5.error ? "C5 TELEMETRY ERROR" :
        c5.receiving ? "C5 LISTENING" : c5.attached ? "C5 BETWEEN SWEEPS" : "C5 WAITING";
    ESP_RETURN_ON_ERROR(draw_centered(214, c5_state,
        c5.error && c5.error != ESP_ERR_NOT_SUPPORTED ? UI_BAD : UI_TEXT, 1), TAG, "analyzer C5 state");
    bool c5_sample = c5.sample_valid && c5.error == ESP_OK;
    if (c5_sample) snprintf(line, sizeof(line), "C5 COUNT %s%lu / SWEEP %lu ms",
        c5.saturated ? ">=" : "", (unsigned long)c5.reports, (unsigned long)c5.elapsed_ms);
    else snprintf(line, sizeof(line), "C5 COUNT UNKNOWN / NO CURRENT SAMPLE");
    ESP_RETURN_ON_ERROR(draw_centered(230, line, UI_TEXT, 1), TAG, "analyzer C5 count");
    unsigned count = c5.history_count <= CERBERUS_DUAL_BLE_ANALYZER_HISTORY ? c5.history_count : 0U;
    const cerberus_dual_ble_analyzer_interval_t *last = count ? &c5.history[count - 1U] : NULL;
    uint32_t rate = 0U;
    if (c5_sample && cerberus_dual_ble_analyzer_rate(last, &rate))
        snprintf(line, sizeof(line), "C5 %lu CALLBACKS/s OVER %lu ms", (unsigned long)rate,
            (unsigned long)last->duration_ms);
    else snprintf(line, sizeof(line), "C5 RATE UNKNOWN / WAITING OR GAP");
    ESP_RETURN_ON_ERROR(draw_centered(246, line, UI_MUTED, 1), TAG, "analyzer C5 interval");
    ESP_RETURN_ON_ERROR(draw_centered(262, "C5: RETAINED SOURCE INTERVALS / GAPS", UI_MUTED, 1), TAG, "analyzer C5 scope");
    ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(20, 278, 280, 60, UI_PANEL), TAG, "analyzer C5 graph");
    scale = 1U;
    for (unsigned i = 0U; i < count; ++i)
        if (cerberus_dual_ble_analyzer_rate(&c5.history[i], &rate) && rate > scale) scale = rate;
    for (unsigned i = 0U; i < count; ++i) {
        uint16_t x = (uint16_t)(32U + (CERBERUS_DUAL_BLE_ANALYZER_HISTORY - count + i) * 8U);
        if (!cerberus_dual_ble_analyzer_rate(&c5.history[i], &rate)) {
            ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(x, 331, 5, 3, UI_MUTED), TAG, "analyzer C5 gap");
            continue;
        }
        if (!rate) continue; /* A measured zero differs from the gap marker. */
        uint16_t height = (uint16_t)((uint64_t)rate * 52U / scale);
        if (!height) height = 1U;
        ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(x, 334U - height, 5, height, UI_GOOD), TAG, "analyzer C5 bar");
    }
    ESP_RETURN_ON_ERROR(draw_centered(350, "SEPARATE CLOCKS / NOT UNIQUE DEVICES", UI_MUTED, 1), TAG, "analyzer receiver scope");
    ESP_RETURN_ON_ERROR(draw_centered(362, "C5 ORIGINALS + PARTIAL / NO RESPONSES", UI_MUTED, 1), TAG, "analyzer callback scope");
    return draw_ble_analyzer_retry_controls();
}
