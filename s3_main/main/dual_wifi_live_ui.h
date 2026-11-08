#pragma once

/* Include immediately after sentinel_live_ui.h. The merged receive-only list
 * never supplies an index to a C5 tool. Its explicit handoff obtains a fenced
 * C5 bank, either the proven original C5 bank or a fresh S3-target reacquisition,
 * and binds the copied AP identity without refreshing retained observation age. */
static bool dual_wifi_live_handoff_pending;
static cerberus_dual_wifi_row_t dual_wifi_live_target;

static bool dual_wifi_live_page(void)
{
    return current_page == UI_PAGE_WIFI && wifi_mode == UI_WIFI_MODE_DUAL;
}

static bool dual_wifi_live_session_released(const sentinel_scan_session_t *session)
{
    /* waiting is retry backoff, not ownership. An idle NONE session may no
     * longer be ticked, so that flag cannot be an admission requirement. */
    return session->desired == SENTINEL_SCAN_NONE && session->active == SENTINEL_SCAN_NONE &&
        session->pending.kind == SENTINEL_SCAN_COMMAND_NONE && !session->stop_requested;
}

static bool dual_wifi_live_released(void)
{
    return dual_wifi_live_session_released(&live_session) &&
        dual_wifi_live_session_released(&live_ble_session) && live_ble_recon_released() && !live_wifi_capture_busy() &&
        (!ble_discovery_live || cerberus_ble_discovery_session_released(&ble_discovery_live->session)) &&
        dual_ble_ui_released();
}

static bool dual_wifi_live_same_ap(const cerberus_c5_wifi_result_t *a,
    const cerberus_c5_wifi_result_t *b)
{
    const char *a_end = memchr(a->ssid, 0, sizeof(a->ssid));
    const char *b_end = memchr(b->ssid, 0, sizeof(b->ssid));
    return a_end && b_end && a_end - a->ssid == b_end - b->ssid &&
        !memcmp(a->ssid, b->ssid, (size_t)(a_end - a->ssid)) &&
        !memcmp(a->bssid, b->bssid, sizeof(a->bssid)) &&
        a->channel == b->channel && a->security == b->security;
}

static bool dual_wifi_live_selection_current(void)
{
    cerberus_dual_wifi_row_t selected;
    return dual_wifi_ui_copy_selected(&selected) == ESP_OK &&
        selected.source == dual_wifi_live_target.source &&
        selected.source_index == dual_wifi_live_target.source_index &&
        selected.source_generation == dual_wifi_live_target.source_generation &&
        dual_wifi_live_same_ap(&selected.ap, &dual_wifi_live_target.ap);
}

static void dual_wifi_live_cancel_handoff(void)
{
    if (dual_wifi_live_handoff_pending) cerberus_dual_wifi_scan_stop();
    dual_wifi_live_handoff_pending = false;
    memset(&dual_wifi_live_target, 0, sizeof(dual_wifi_live_target));
}

static void dual_wifi_live_open(bool start_scan)
{
    dual_wifi_live_cancel_handoff();
    dual_wifi_return_to_view = false;
    current_page = UI_PAGE_WIFI;
    wifi_mode = UI_WIFI_MODE_DUAL;
    wifi_attack_selecting = false;
    /* Select NONE schedules only the old owners' own stop/drain operations.
     * The regular global tick performs them; no radio is stolen here. */
    sentinel_scan_session_select(&live_session, SENTINEL_SCAN_NONE);
    sentinel_scan_session_select(&live_ble_session, SENTINEL_SCAN_NONE);
    ble_discovery_leave();
    wifi_relation_cancel();
    (void)dual_wifi_ui_enter(start_scan && !ui_capability_blocked);
}

static esp_err_t dual_wifi_live_publish(void)
{
    if (!dual_wifi_live_selection_current() || !dual_wifi_live_released() ||
        live_wifi_capture_busy()) return ESP_ERR_INVALID_STATE;
    if (!live_store_acquire()) return ESP_ERR_NO_MEM;
    cerberus_wifi_scan_status_payload_t status;
    cerberus_c5_wifi_relation_bank_t proof;
    cerberus_dual_wifi_handoff_t handoff;
    uint8_t selected = 0U;
    esp_err_t error = cerberus_dual_wifi_scan_copy_legacy_bank(live_store->wifi_staging,
        live_store->wifi_staging_observations, CERBERUS_WIFI_SCAN_MAX_RESULTS,
        &status, &proof, &selected, &handoff);
    if (error != ESP_OK) return error;
    if (status.state != CERBERUS_WIFI_SCAN_COMPLETE || status.error_code != ESP_OK ||
        !status.stored_count || status.stored_count > CERBERUS_WIFI_SCAN_MAX_RESULTS ||
        selected >= status.stored_count || proof.status.generation != status.generation ||
        !cerberus_s3_wifi_relation_meta_valid(&proof) ||
        !dual_wifi_live_same_ap(&live_store->wifi_staging[selected], &dual_wifi_live_target.ap))
        return ESP_ERR_INVALID_RESPONSE;
    const int64_t now_us = esp_timer_get_time();
    if (handoff.received_us < 0 || handoff.received_us > now_us || !handoff.session_id ||
        !cerberus_wifi_scan_band_valid(handoff.band) ||
        (handoff.reused && dual_wifi_live_target.source != CERBERUS_DUAL_WIFI_SOURCE_C5) ||
        (!handoff.reused && handoff.band != CERBERUS_WIFI_SCAN_BAND_ALL))
        return ESP_ERR_INVALID_RESPONSE;
    for (uint8_t i = 0U; i < status.stored_count; ++i) {
        const uint8_t channel = live_store->wifi_staging[i].channel;
        if ((handoff.band == CERBERUS_WIFI_SCAN_BAND_5G && channel <= 14U) ||
            (handoff.band == CERBERUS_WIFI_SCAN_BAND_2G && channel > 14U))
            return ESP_ERR_INVALID_RESPONSE;
    }
    if (wifi_relation_exhausted || wifi_relation_publication == UINT64_MAX)
        return ESP_ERR_INVALID_STATE;

    /* All fallible work is complete. Publish the C5 bank and the same proof on
     * this UI task before making its selected C5 index reachable by old tools. */
    live_wifi_capture_reset();
    memcpy(live_store->wifi, live_store->wifi_staging, sizeof(live_store->wifi));
    memcpy(live_store->wifi_observations, live_store->wifi_staging_observations,
        sizeof(live_store->wifi_observations));
    wifi_scan_status = status;
    live_records[SENTINEL_SCAN_WIFI] = (live_scan_record_t){
        .valid = true, .captured = true, .state = SENTINEL_SCAN_COMPLETE,
        .generation = status.generation, .saved_generation = status.generation,
        .count = status.stored_count, .captured_ms = (uint32_t)(handoff.received_us / 1000LL),
        .error = ESP_OK};
    live_wifi_published_us = handoff.received_us; /* Original receipt, never RF time. */
    live_capture_pending &= ~(UINT32_C(1) << SENTINEL_SCAN_WIFI);
    memset(&live_wifi_built, 0, sizeof(live_wifi_built));
    memset(&live_wifi_painted, 0, sizeof(live_wifi_painted));
    live_page[SENTINEL_SCAN_WIFI] = selected / 4U;
    wifi_commit_cached_results_at(live_store->wifi, status.stored_count, status.generation,
        handoff.received_us, handoff.band, &proof, handoff.session_id);
    wifi_relation_publish(status.generation, &proof, ESP_OK);
    /* This explicit single-AP choice supersedes any previous multi-select.
     * Otherwise the old menu may silently replace it with that set's first AP. */
    wifi_targets_clear();
    wifi_selected_result = live_store->wifi[selected];
    wifi_selected_index = selected;
    wifi_selection_valid = true;
    wifi_relation_bind_selection(true);
    wifi_analysis_generation = 0U;
    wifi_analysis_error = ESP_ERR_INVALID_STATE;
    wifi_trust_error = ESP_OK;
    /* Existing AP tools refresh connection state on demand. Do not present a
     * prior target's cached connection result as a successful new connection. */
    wifi_connection_error = ESP_ERR_INVALID_STATE;
    memset(&wifi_connection_status, 0, sizeof(wifi_connection_status));
    dual_wifi_return_to_view = true;
    dual_wifi_live_handoff_pending = false;
    dual_wifi_ui_leave();
    wifi_target_finish_selection();
    return ESP_OK;
}

static void dual_wifi_live_action(dual_wifi_ui_action_t action)
{
    switch (action) {
    case DUAL_WIFI_UI_SAVE_ALL: wifi_archive_save_live(false); return;
    case DUAL_WIFI_UI_SAVE_SELECTED: wifi_archive_save_live(true); return;
    case DUAL_WIFI_UI_BACK:
        dual_wifi_live_cancel_handoff();
        dual_wifi_return_to_view = false;
        return_to_launcher();
        return;
    case DUAL_WIFI_UI_UNSUPPORTED:
        /* The coordinator returned before RF. Older C5 peers retain their
         * original all-band scan path, explicitly armed for this user action. */
        dual_wifi_live_cancel_handoff();
        dual_wifi_ui_leave();
        wifi_mode = UI_WIFI_MODE_LIST;
        dual_wifi_return_to_view = false;
        sentinel_scan_session_select(&live_session, SENTINEL_SCAN_WIFI);
        sentinel_scan_session_pause(&live_session, false);
        live_last_poll = 0U;
        break;
    case DUAL_WIFI_UI_OPEN_PACKET_COUNT: {
        cerberus_dual_wifi_row_t row;
        if (dual_wifi_ui_copy_selected(&row)!=ESP_OK) return;
        wifi_packet_count_selection_t selection={.target={.role=CERBERUS_WIFI_PACKET_COUNT_AP,
            .channel_hint=row.ap.channel},.discovery_source=row.source,.discovery_publication=row.source_generation};
        memcpy(selection.target.mac,row.ap.bssid,6U); memcpy(selection.bssid,row.ap.bssid,6U);
        memcpy(selection.label,row.ap.ssid,32U);
        esp_err_t error=wifi_packet_count_open_selection(&selection,UI_WIFI_MODE_DUAL);
        if (error!=ESP_OK) dual_wifi_ui_set_error(error);
        break;
    }
    case DUAL_WIFI_UI_OPEN_AP_TOOLS: {
        if (dual_wifi_live_handoff_pending) return;
        esp_err_t error = dual_wifi_live_released() ?
            dual_wifi_ui_copy_selected(&dual_wifi_live_target) : ESP_ERR_INVALID_STATE;
        if (error == ESP_OK) error = cerberus_dual_wifi_scan_begin_rebind(&dual_wifi_live_target);
        if (error == ESP_OK) dual_wifi_live_handoff_pending = true;
        else dual_wifi_ui_set_error(error);
        break;
    }
    case DUAL_WIFI_UI_REDRAW: break;
    default: return;
    }
    ESP_ERROR_CHECK(draw_detail_page());
}

static void dual_wifi_live_tap(uint16_t x, uint16_t y)
{
    bool stop_handoff = dual_wifi_live_handoff_pending &&
        dual_wifi_ui_inside(x, y, 164, 426, 148, 44);
    dual_wifi_ui_action_t action = dual_wifi_ui_tap(x, y);
    if (dual_wifi_live_handoff_pending && (stop_handoff || !dual_wifi_live_selection_current()))
        dual_wifi_live_cancel_handoff();
    dual_wifi_live_action(action);
}

static void dual_wifi_live_tick(uint32_t now, bool asleep)
{
    bool page = dual_wifi_live_page();
    bool visible = page && !asleep && !ui_capability_blocked;
    if (!visible) {
        dual_wifi_live_cancel_handoff();
        if (!page && dual_wifi_ui_opened) dual_wifi_ui_leave();
        else if (page) {
            /* Wake leaves the retained view stopped; it cannot silently rearm
             * a scan or an AP-tool handoff that was requested before sleep. */
            dual_wifi_ui_set_error(ESP_OK);
            cerberus_dual_wifi_scan_stop();
        }
    }
    bool released = dual_wifi_live_released();
    dual_wifi_ui_action_t action = dual_wifi_ui_tick(now, visible, visible && released);
    if (!visible) return; /* tick above still advances hidden cleanup. */
    if (dual_wifi_live_handoff_pending) {
        cerberus_dual_wifi_status_t status;
        cerberus_dual_wifi_scan_get_status(&status);
        if (!dual_wifi_live_selection_current()) dual_wifi_live_cancel_handoff();
        else if (status.rebind_ready && !status.cleanup_pending && !status.rebinding && released) {
            esp_err_t error = dual_wifi_live_publish();
            if (error != ESP_OK) {
                dual_wifi_live_cancel_handoff();
                dual_wifi_ui_set_error(error);
            }
            ESP_ERROR_CHECK(draw_detail_page());
            return;
        } else if (!status.rebinding && !status.cleanup_pending &&
            status.state != CERBERUS_DUAL_WIFI_RUNNING && status.state != CERBERUS_DUAL_WIFI_READING &&
            status.state != CERBERUS_DUAL_WIFI_STOPPING) {
            dual_wifi_live_cancel_handoff();
            dual_wifi_ui_set_error(status.error != ESP_OK ? status.error : ESP_ERR_INVALID_STATE);
            action = DUAL_WIFI_UI_REDRAW;
        }
    }
    dual_wifi_live_action(action);
}
