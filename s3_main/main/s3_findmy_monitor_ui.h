/* Presentation of the shared discovery owners and committed source banks.
 * Included after discovery + dual-live helpers. No private RF owner or bank. */
static bool ble_findmy_available, ble_findmy_paint_valid, ble_findmy_legacy_pending;
static uint8_t ble_findmy_visible_count;
#define FINDMY_ROWS_Y 132U
#define FINDMY_ROW_STEP 50U
#define FINDMY_ROW_HEIGHT 46U

static bool ble_findmy_monitor_available(void) { return ble_findmy_available; }
static bool ble_findmy_monitor_requested(void)
{ return ble_findmy_running && !ble_findmy_stopping; }
static bool ble_findmy_released(void)
{ return dual_ble_live_discovery_released() && dual_ble_ui_released(); }

/* Legacy ordinary-list handoff: the caller already copied its source proof.
 * It may wait for an existing owner, but never starts/stops a generic scanner. */
static void ble_findmy_apply_legacy_selection(void)
{
    ble_findmy_legacy_pending = false;
    ble_findmy_stopping = false;
    ble_selected_raw_frozen = true;
    ble_selected_result = ble_findmy_selected.peer;
    make_ble_name_safe(ble_selected_result.name);
    ble_selected_focus = ble_findmy_selected.focus;
    ble_selected_scan_index = ble_findmy_selected.index;
    ble_selected_sound_profile = selected_findmy_profile(
        ble_selected_scan_index, &ble_selected_result, &ble_selected_scan_ms);
    ble_selection_valid = true;
    ble_attack_capture_valid = false;
    ble_mode = UI_BLE_MODE_DETAIL;
    ble_connection_checked = ble_service_checked = false;
    ble_characteristic_checked = ble_read_checked = false;
    set_ble_action_message("SCAN STOPPED / ORIGINAL ADV SELECTED", UI_MUTED);
    if (ble_attack_selecting) {
        ble_attack_selecting = false;
        (void)ble_attack_capture_selected_ad(ble_selected_scan_index);
        open_tool_page(UI_PAGE_BLE_ATTACK);
    }
}

static void ble_select_frozen_live(const cerberus_findmy_monitor_record_t *record)
{
    if (!record) return;
    ble_selected_original_time64_valid = false;
    ble_selected_scan_epoch = 0U;
    ble_findmy_selected = *record;
    bool monitor = ble_findmy_available && ble_discovery_live &&
        ble_discovery_live->selected_source == CERBERUS_BLE_DISCOVERY_LIST_S3;
    ble_selected_from_monitor = monitor;
    ble_findmy_legacy_pending = true;
    if (ble_findmy_released() && (monitor || cerberus_s3_ble_host_owner() != CERBERUS_S3_BLE_OWNER_SCANNER))
        ble_findmy_apply_legacy_selection();
    else {
        ble_mode = UI_BLE_MODE_FINDMY_MONITOR;
        ble_findmy_running = false;
        ble_findmy_stopping = true;
        ble_findmy_wait_since = (uint32_t)(esp_timer_get_time() / 1000LL);
        ble_findmy_error = ESP_OK;
    }
}

static void ble_findmy_request_stop(bool exit, bool select)
{
    ble_findmy_running = false;
    if (!select) ble_findmy_legacy_pending = false;
    ble_findmy_exit_pending |= exit;
    if (exit) { ble_findmy_select_pending = false; ble_findmy_legacy_pending = false; }
    else if (!ble_findmy_exit_pending) ble_findmy_select_pending = select;
    if (!ble_findmy_stopping)
        ble_findmy_wait_since = (uint32_t)(esp_timer_get_time() / 1000LL);
    ble_findmy_stopping = true;
    ble_findmy_paint_valid = false;
    if (ble_discovery_live) {
        cerberus_ble_discovery_session_stop(&ble_discovery_live->session);
        ble_discovery_live->painted_count = 0U;
        if (!ble_findmy_select_pending) ble_discovery_live->selected_source = 0U;
    }
    /* Intent only: the global owner tick keeps the single transport budget. */
    dual_ble_ui_cancel();
}

static void ble_findmy_monitor_open(void)
{
    if (ble_findmy_stopping || ble_findmy_legacy_pending) return;
    ble_findmy_available = true;
    ble_findmy_page = 0U;
    ble_findmy_exit_pending = ble_findmy_select_pending = false;
    ble_findmy_running = true;
    ble_findmy_paint_valid = false;
    ble_findmy_last_draw = 0U;
    ble_mode = UI_BLE_MODE_FINDMY_MONITOR;
    ble_filter = CERBERUS_S3_BLE_FILTER_FIND_MY;
    ble_findmy_error = ble_discovery_monitor_start();
    /* S3 allocation/start failure does not cancel the C5 requested receiver. */
    dual_ble_ui_retry();
}

static void ble_findmy_monitor_return(void)
{
    ble_findmy_available = true;
    ble_mode = UI_BLE_MODE_FINDMY_MONITOR;
    ble_filter = CERBERUS_S3_BLE_FILTER_FIND_MY;
    ble_findmy_exit_pending = ble_findmy_select_pending = false;
    ble_findmy_legacy_pending = false;
    ble_findmy_request_stop(false, false);
}

static bool ble_findmy_s3_retryable(void)
{
    return ble_findmy_running && !ble_findmy_stopping &&
        (ble_findmy_error != ESP_OK || (ble_discovery_live && ble_discovery_live->session.faulted));
}

static bool ble_findmy_c5_retryable(const cerberus_dual_ble_view_t *view)
{
    return view && ble_findmy_running && !ble_findmy_stopping &&
        view->error != ESP_OK && !view->stopping;
}

/* Compact display facts only; full UUID widths remain in selected detail.
 * Source ages are exact S3 values or conservative C5 bounds, never merged. */
static void ble_findmy_age_line(char *out, size_t capacity, const char *source,
    const char *hint, bool valid, uint64_t age, bool upper)
{
    if (!valid) snprintf(out, capacity, "%s %s / AGE ?", source, hint);
    else if (!upper && age > UINT32_MAX)
        snprintf(out, capacity, "%s %s / >49d", source, hint);
    else {
        uint64_t seconds = age / 1000U + (upper && age % 1000U != 0U);
        snprintf(out, capacity, "%s %s / %s%llus%s", source, hint, upper ? "<=" : "",
            (unsigned long long)seconds, upper && age > 30000U ? " RECENCY?" : "");
    }
}

static bool ble_findmy_s3_hint_age(const cerberus_s3_ble_discovery_entry_t *entry,
    uint32_t observed, uint64_t now, uint64_t *age)
{
    if (!entry || !age || now < entry->source_now_ms ||
        entry->source_now_ms < (uint64_t)(uint32_t)(entry->source_now_ms - observed)) return false;
    uint64_t source = entry->source_now_ms - (uint32_t)(entry->source_now_ms - observed);
    if (source < entry->original_source_ms || source > now) return false;
    *age = now - source; return true;
}

static esp_err_t ble_findmy_draw_row(unsigned row, uint64_t now)
{
    ble_discovery_live_store_t *store = ble_discovery_live;
    char line[64], identifier[24];
    uint16_t y = FINDMY_ROWS_Y + (uint16_t)row * FINDMY_ROW_STEP;
    ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(8, y, 304, FINDMY_ROW_HEIGHT, UI_PANEL), TAG, "monitor row");
    if (store->painted_source[row] == CERBERUS_BLE_DISCOVERY_LIST_S3) {
        const cerberus_s3_ble_discovery_entry_t *entry = &store->painted[row];
        const cerberus_s3_ble_discovery_evidence_t *h = &entry->record.focus.discovery;
        cerberus_privacy_format_ble_identifier(entry->record.peer.address,
            privacy_addresses_masked(), identifier, sizeof(identifier));
        if (cerberus_ble_rssi_valid(entry->record.peer.rssi))
            snprintf(line, sizeof(line), "S3 %s T%u / PEAK %ddB", identifier,
                entry->record.peer.address_type, entry->record.peer.rssi);
        else snprintf(line, sizeof(line), "S3 %s T%u / RSSI ?", identifier, entry->record.peer.address_type);
        ESP_RETURN_ON_ERROR(cerberus_display_draw_text(14, y + 2U, line, UI_TEXT, UI_PANEL, 1), TAG, "monitor S3 identity");
        uint64_t age;
        bool valid = h->findmy_advertisement.bits &&
            ble_findmy_s3_hint_age(entry, h->advertisement_observed_ms, now, &age);
        ble_findmy_age_line(line, sizeof(line), "ADV", cerberus_findmy_hint_summary(h->findmy_advertisement.bits), valid, valid ? age : 0U, false);
        ESP_RETURN_ON_ERROR(cerberus_display_draw_text(14, y + 13U, line, UI_MUTED, UI_PANEL, 1), TAG, "monitor S3 ADV");
        valid = h->findmy_scan_response.bits &&
            ble_findmy_s3_hint_age(entry, h->scan_response_observed_ms, now, &age);
        ble_findmy_age_line(line, sizeof(line), "RSP", h->findmy_scan_response.bits ?
            cerberus_findmy_hint_summary(h->findmy_scan_response.bits) : "NO MATCHED HINT",
            valid, valid ? age : 0U, false);
        ESP_RETURN_ON_ERROR(cerberus_display_draw_text(14, y + 24U, line, UI_MUTED, UI_PANEL, 1), TAG, "monitor S3 RSP");
        snprintf(line, sizeof(line), "S3 RETAINED HISTORY / FORMAT HINTS ONLY");
    } else {
        const cerberus_dual_ble_row_t *peer = &store->painted_c5[row];
        const cerberus_ble_discovery_list_ref_t ref = store->order[ble_findmy_page + row];
        bool adv = (store->c5_family_bits[ref.index] & CERBERUS_DUAL_BLE_FAMILY_FIND_MY) != 0U;
        bool rsp = (store->c5_response_bits[ref.index] & CERBERUS_DUAL_BLE_FAMILY_FIND_MY) != 0U;
        cerberus_privacy_format_ble_identifier(peer->address, privacy_addresses_masked(), identifier, sizeof(identifier));
        if (cerberus_dual_ble_list_rssi_valid(peer)) snprintf(line, sizeof(line), "C5 %s T%u / ADV %ddB", identifier, peer->address_type, peer->rssi);
        else snprintf(line, sizeof(line), "C5 %s T%u / RSSI ?", identifier, peer->address_type);
        ESP_RETURN_ON_ERROR(cerberus_display_draw_text(14, y + 2U, line, UI_TEXT, UI_PANEL, 1), TAG, "monitor C5 identity");
        uint32_t age;
        bool valid = adv && cerberus_dual_ble_list_age(peer, now, &age);
        ble_findmy_age_line(line, sizeof(line), "ADV", adv ? "FIND MY HINT" : "NO FIND MY HINT", valid, valid ? age : 0U, true);
        ESP_RETURN_ON_ERROR(cerberus_display_draw_text(14, y + 13U, line, UI_MUTED, UI_PANEL, 1), TAG, "monitor C5 ADV");
        valid = rsp && cerberus_dual_ble_response_age(peer, now, &age);
        const char *hint = rsp ? "FIND MY HINT" : peer->response.state == CERBERUS_DUAL_BLE_RESPONSE_UNKNOWN ?
            "UNKNOWN" : peer->response.state == CERBERUS_DUAL_BLE_RESPONSE_NONE ? "NONE ADMITTED" : "NO FIND MY HINT";
        ble_findmy_age_line(line, sizeof(line), "RSP", hint, valid, valid ? age : 0U, true);
        ESP_RETURN_ON_ERROR(cerberus_display_draw_text(14, y + 24U, line, UI_MUTED, UI_PANEL, 1), TAG, "monitor C5 RSP");
        snprintf(line, sizeof(line), "C5 COMMITTED BANK / FORMAT HINTS ONLY");
    }
    return cerberus_display_draw_text(14, y + 35U, line, UI_MUTED, UI_PANEL, 1);
}

static esp_err_t draw_ble_findmy_monitor(void)
{
    char line[64];
    const uint64_t now = (uint64_t)(esp_timer_get_time() / 1000LL);
    cerberus_dual_ble_view_t c5; dual_ble_ui_get_view(&c5);
    ble_findmy_paint_valid = false;
    if (ble_discovery_live) ble_discovery_live->painted_count = ble_discovery_live->built_count = 0U;
    ESP_RETURN_ON_ERROR(draw_centered(56, "FIND MY MONITOR / TWO RECEIVERS", UI_GOOD, 1), TAG, "monitor header");
    const cerberus_ble_discovery_session_t *s3 = ble_discovery_live ? &ble_discovery_live->session : NULL;
    const char *s3_state = s3 && s3->error ? "ERROR" : ble_findmy_stopping ? "STOPPING" :
        s3 && s3->enabled ? "RUNNING" : s3 && s3->epoch ? "RELEASING" : "STOPPED";
    snprintf(line, sizeof(line), "S3 ACTIVE / %s / %lu SWEEPS", s3_state, (unsigned long)(s3 ? s3->completed_sweeps : 0U));
    ESP_RETURN_ON_ERROR(draw_centered(72, line, UI_MUTED, 1), TAG, "monitor S3 state");
    snprintf(line, sizeof(line), "C5 %s / %s / %lu UPDATES", dual_ble_ui_response_requests ? "RSP REQS" : "PASSIVE",
        c5.error == ESP_ERR_NOT_SUPPORTED ? "UNSUPPORTED" : c5.error ? "ERROR" : c5.stopping ? "STOPPING" :
        c5.receiving ? "RECEIVING" : c5.reading ? "READING" : c5.enabled ? "STARTING" : "STOPPED", (unsigned long)c5.snapshots);
    ESP_RETURN_ON_ERROR(draw_centered(86, line, c5.error ? UI_BAD : UI_MUTED, 1), TAG, "monitor C5 state");
    cerberus_ble_discovery_list_source_t source = ble_discovery_live ? ble_discovery_live->source_filter : CERBERUS_BLE_DISCOVERY_LIST_ALL;
    ESP_RETURN_ON_ERROR(draw_small_button(8, 102, 148, 26, source == CERBERUS_BLE_DISCOVERY_LIST_S3 ? "SOURCE S3" :
        source == CERBERUS_BLE_DISCOVERY_LIST_C5 ? "SOURCE C5" : "SOURCE ALL"), TAG, "monitor source");
    ESP_RETURN_ON_ERROR(cerberus_display_draw_text(172, 111, "ADV / MATCHED RSP", UI_MUTED, UI_BACKGROUND, 1), TAG, "monitor evidence scope");
    ble_findmy_visible_count = ble_discovery_live ? ble_discovery_combined_order() : 0U;
    if (ble_findmy_page >= ble_findmy_visible_count) ble_findmy_page = 0U;
    unsigned count = 0U;
    for (; count < 4U && ble_findmy_page + count < ble_findmy_visible_count; ++count) {
        if (!ble_discovery_build_unified_row(count, ble_findmy_page + count, now)) return ESP_ERR_INVALID_RESPONSE;
        ESP_RETURN_ON_ERROR(ble_findmy_draw_row(count, now), TAG, "monitor proof row");
    }
    if (!count) ESP_RETURN_ON_ERROR(draw_centered(210, "NO RETAINED FIND MY HINTS FOR THIS SOURCE", UI_MUTED, 1), TAG, "monitor empty");
    ESP_RETURN_ON_ERROR(draw_small_button(8, 340, 72, 30, "PREV"), TAG, "monitor prev");
    ESP_RETURN_ON_ERROR(draw_small_button(86, 340, 72, 30, "NEXT"), TAG, "monitor next");
    ESP_RETURN_ON_ERROR(draw_small_button(164, 340, 148, 30, ble_findmy_stopping ? "STOPPING" :
        ble_findmy_running ? "STOP BOTH" : "START BOTH"), TAG, "monitor toggle");
    ESP_RETURN_ON_ERROR(draw_small_button(8, 376, 148, 26, ble_findmy_s3_retryable() ? "RETRY S3" : "S3"), TAG, "monitor S3 retry");
    ESP_RETURN_ON_ERROR(draw_small_button(164, 376, 148, 26, ble_findmy_c5_retryable(&c5) ? "RETRY C5" : "C5"), TAG, "monitor C5 retry");
    if (ble_findmy_error) snprintf(line, sizeof(line), "CONTROL: %s", esp_err_to_name(ble_findmy_error));
    else if (s3 && s3->error) snprintf(line, sizeof(line), "S3: %s", esp_err_to_name(s3->error));
    else if (c5.error) snprintf(line, sizeof(line), "C5: %s", esp_err_to_name(c5.error));
    else snprintf(line, sizeof(line), "%u OBS / PAGE %u OF %u / TAP DETAILS", ble_findmy_visible_count,
        (unsigned)(ble_findmy_page / 4U + 1U), (unsigned)(ble_findmy_visible_count ? (ble_findmy_visible_count + 3U) / 4U : 1U));
    ESP_RETURN_ON_ERROR(draw_centered(408, line, ble_findmy_error || (s3 && s3->error) || c5.error ? UI_BAD : UI_MUTED, 1), TAG, "monitor status");
    if (ble_discovery_live) ble_discovery_live->painted_count = (uint8_t)count;
    ble_findmy_paint_valid = true;
    return ESP_OK;
}

static void ble_findmy_monitor_tick(uint32_t now, bool asleep)
{
    bool visible = current_page == UI_PAGE_BLE && ble_mode == UI_BLE_MODE_FINDMY_MONITOR;
    if (ble_findmy_legacy_pending) {
        if (!visible || asleep) ble_findmy_legacy_pending = false;
        else if (ble_findmy_released() && cerberus_s3_ble_host_owner() != CERBERUS_S3_BLE_OWNER_SCANNER) {
            ble_findmy_apply_legacy_selection();
            ESP_ERROR_CHECK(draw_detail_page());
            return;
        } else {
            if ((uint32_t)(now - ble_findmy_wait_since) >= 12000U) ble_findmy_error = ESP_ERR_TIMEOUT;
            if (!ble_findmy_last_draw || (uint32_t)(now - ble_findmy_last_draw) >= UI_BLE_REFRESH_MS) {
                ble_findmy_last_draw = now;
                ESP_ERROR_CHECK(draw_detail_page());
            }
            return;
        }
    }
    if (!ble_findmy_available && !ble_findmy_stopping) return;
    if (!visible || asleep) {
        ble_findmy_exit_pending = ble_findmy_select_pending = false;
        if (ble_discovery_live) ble_discovery_live->selected_source = 0U;
        if (ble_findmy_running) ble_findmy_request_stop(false, false);
    }
    if (ble_findmy_stopping) {
        if (ble_findmy_released()) {
            ble_findmy_stopping = false;
            if (ble_findmy_exit_pending && visible && !asleep) {
                ble_findmy_exit_pending = false; ble_findmy_available = false;
                ble_mode = UI_BLE_MODE_LIST; ble_filter = CERBERUS_S3_BLE_FILTER_FIND_MY;
                ble_scan_checked = false;
            } else if (ble_findmy_select_pending && visible && !asleep) {
                ble_findmy_select_pending = false;
                if (!ble_discovery_monitor_apply_selection()) ble_findmy_error = ESP_ERR_INVALID_STATE;
            }
            if (visible && !asleep) ESP_ERROR_CHECK(draw_detail_page());
            return;
        }
        if ((uint32_t)(now - ble_findmy_wait_since) >= 12000U) ble_findmy_error = ESP_ERR_TIMEOUT;
    }
    if (visible && !asleep && (!ble_findmy_last_draw || (uint32_t)(now - ble_findmy_last_draw) >= UI_BLE_REFRESH_MS)) {
        ble_findmy_last_draw = now;
        ESP_ERROR_CHECK(draw_detail_page());
    }
}

static void ble_findmy_monitor_tap(uint16_t x, uint16_t y)
{
    if (ble_findmy_stopping || ble_findmy_legacy_pending) return;
    if (y >= FINDMY_ROWS_Y && y < FINDMY_ROWS_Y + 4U * FINDMY_ROW_STEP) {
        unsigned row = (y - FINDMY_ROWS_Y) / FINDMY_ROW_STEP;
        if (ble_findmy_paint_valid && ble_discovery_monitor_freeze_selection(row))
            ble_findmy_request_stop(false, true);
    } else if (y >= 102U && y < 128U && x >= 8U && x < 156U && ble_discovery_live) {
        ble_discovery_live->source_filter = (cerberus_ble_discovery_list_source_t)
            ((ble_discovery_live->source_filter + 1U) % CERBERUS_BLE_DISCOVERY_LIST_SOURCE_COUNT);
        ble_discovery_live->order_valid = false;
        ble_findmy_page = 0U;
    } else if (y >= 340U && y < 370U) {
        if (x >= 8U && x < 80U) ble_findmy_page = ble_findmy_page >= 4U ? ble_findmy_page - 4U : 0U;
        else if (x >= 86U && x < 158U && ble_findmy_page + 4U < ble_findmy_visible_count) ble_findmy_page += 4U;
        else if (x >= 164U && x < 312U) {
            if (ble_findmy_running) ble_findmy_request_stop(false, false);
            else {
                ble_findmy_running = true;
                ble_findmy_error = ble_discovery_monitor_start();
                dual_ble_ui_retry();
            }
        }
    } else if (y >= 376U && y < 402U) {
        if (x >= 8U && x < 156U && ble_findmy_s3_retryable())
            ble_findmy_error = ble_discovery_monitor_start();
        else if (x >= 164U && x < 312U) {
            cerberus_dual_ble_view_t view; dual_ble_ui_get_view(&view);
            if (ble_findmy_c5_retryable(&view)) dual_ble_ui_retry();
        }
    }
    ESP_ERROR_CHECK(draw_detail_page());
}
