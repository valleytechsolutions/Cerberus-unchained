/* Private Recon controls on the existing UI task. No second radio owner. */
#include "recon_recorder.h"
static const char *recon_record_label(const cerberus_recon_recorder_status_t *status)
{
    switch (status->state) {
    case CERBERUS_RECON_RECORDER_STARTING: return "STARTING";
    case CERBERUS_RECON_RECORDER_RECORDING: return "RECORDING";
    case CERBERUS_RECON_RECORDER_STOPPING: return "SAVING";
    case CERBERUS_RECON_RECORDER_COMPLETE: return "SAVED";
    case CERBERUS_RECON_RECORDER_ERROR: return status->session_name[0] ? "ERROR / PARTIAL" : "ERROR";
    case CERBERUS_RECON_RECORDER_CANCELED: return "STOPPED / NO FILE";
    default: return "OFF";
    }
}
static esp_err_t recon_draw_options(void)
{
    ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(0, 0, 320, 480, UI_BACKGROUND), TAG, "recon options clear");
    ESP_RETURN_ON_ERROR(draw_header("RECON OPTIONS"), TAG, "recon options header");
    cerberus_recon_recorder_status_t status;
    cerberus_recon_recorder_get_status(&status);
    char line[52], confidence[25];
    snprintf(line, sizeof(line), "SD RECORD: %s", recon_record_label(&status));
    ESP_RETURN_ON_ERROR(draw_centered(64, line, UI_GOOD, 1), TAG, "recon record status");
    snprintf(line, sizeof(line), status.session_name[0] ? "/CERBERUS/RECON/%.6s" : "NO SD SESSION FOLDER", status.session_name);
    ESP_RETURN_ON_ERROR(draw_centered(84, line, UI_TEXT, 1), TAG, "recon record folder");
    if (status.error != ESP_OK)
        snprintf(line, sizeof(line), "%.45s", esp_err_to_name(status.error));
    else {
        uint64_t missed = (uint64_t)status.counters.dropped + status.counters.invalid + status.counters.abandoned;
        bool missed_lower_bound = missed > UINT32_MAX || status.counters.dropped == UINT32_MAX ||
            status.counters.invalid == UINT32_MAX || status.counters.abandoned == UINT32_MAX;
        snprintf(line, sizeof(line), "WRITTEN %lu%s / MISSED %lu%s", (unsigned long)status.counters.written,
            status.counters.written == UINT32_MAX ? "+" : "", (unsigned long)(missed > UINT32_MAX ? UINT32_MAX : missed),
            missed_lower_bound ? "+" : "");
    }
    ESP_RETURN_ON_ERROR(draw_centered(102, line, UI_MUTED, 1), TAG, "recon record counts");
    bool can_stop = status.worker_active && (status.state == CERBERUS_RECON_RECORDER_STARTING ||
        status.state == CERBERUS_RECON_RECORDER_RECORDING);
    ESP_RETURN_ON_ERROR(draw_button(8, 120, 304, 44, can_stop ? "STOP AND SAVE RECORDING" :
        status.worker_active ? "SAVING / PLEASE WAIT" : watch_paused ? "RESUME RECON TO RECORD" : "START SD RECORDING"), TAG, "recon record control");
    snprintf(confidence, sizeof(confidence), "ALERT: %s", watch_state.minimum_confidence == SENTINEL_WATCH_CONFIDENCE_HIGH ?
        "HIGH ONLY" : watch_state.minimum_confidence == SENTINEL_WATCH_CONFIDENCE_MEDIUM ? "MEDIUM +" : "ALL");
    const char *labels[] = {confidence, watch_state.muted ? "ALERTS: MUTED" : "ALERTS: ON", "TYPE FILTER",
        "RESET IGNORE / SNOOZE", "CLEAR LIVE HISTORY", watch_radar ? "VIEW: RADAR" : "VIEW: DASHBOARD", "NEARBY INVENTORY"};
    for (unsigned i = 0; i < 7; ++i)
        ESP_RETURN_ON_ERROR(draw_button(8 + (i % 2U) * 156, 170 + (i / 2U) * 54, 148, 44, labels[i]), TAG, "recon option");
    const char *confirmation = recon_confirm == 4 ? "TAP AGAIN: RESET IGNORE / SNOOZE" :
        recon_confirm == 5 ? "TAP AGAIN: CLEAR LIVE HISTORY" : recon_message;
    ESP_RETURN_ON_ERROR(draw_centered(390, confirmation, UI_GOOD, 1), TAG, "recon confirmation");
    ESP_RETURN_ON_ERROR(draw_centered(405, "LIVE: VOLATILE / SAVED SD: KEPT", UI_MUTED, 1), TAG, "recon saved lifetime");
    return draw_button(64, 426, 192, 44, "BACK TO RECON");
}
static esp_err_t recon_draw_actions(void)
{
    ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(0, 0, 320, 480, UI_BACKGROUND), TAG, "recon actions clear");
    ESP_RETURN_ON_ERROR(draw_header("DEVICE ACTIONS"), TAG, "recon actions header");
    ESP_RETURN_ON_ERROR(draw_centered(74, watch_detail.match.label, UI_GOOD, 1), TAG, "recon selected");
    if (watch_detail.radio == SENTINEL_WATCH_RADIO_ENVIRONMENT) {
        ESP_RETURN_ON_ERROR(draw_centered(170, "AREA OBSERVATION / NO DEVICE TARGET", UI_MUTED, 1), TAG, "recon area");
    } else {
        ESP_RETURN_ON_ERROR(draw_button(8, 106, 304, 44, "SIGNAL TRACKING"), TAG, "recon signal");
        ESP_RETURN_ON_ERROR(draw_button(8, 160, 304, 44,
            sentinel_watch_is_ignored(&watch_state, &watch_detail) ? "UN-IGNORE DEVICE" : "IGNORE DEVICE ALERTS"), TAG, "recon ignore");
        ESP_RETURN_ON_ERROR(draw_centered(218, "SNOOZE ALERTS FOR THIS ADDRESS", UI_MUTED, 1), TAG, "recon snooze");
        const char *labels[] = {"10 MIN", "30 MIN", "1 HOUR", "4 HOURS"};
        for (unsigned i = 0; i < 4; ++i)
            ESP_RETURN_ON_ERROR(draw_button(8 + (i % 2) * 156, 242 + (i / 2) * 54, 148, 44, labels[i]), TAG, "recon duration");
    }
    ESP_RETURN_ON_ERROR(draw_centered(358, recon_message, UI_GOOD, 1), TAG, "recon action status");
    ESP_RETURN_ON_ERROR(draw_centered(387, "LIVE HINT HISTORY / SD RECORD IS SEPARATE", UI_MUTED, 1), TAG, "recon action effect");
    return draw_button(64, 426, 192, 44, "BACK TO EVIDENCE");
}
static esp_err_t recon_draw_signal(uint32_t now)
{
    bool found = false;
    for (unsigned i = 0; i < watch_state.history_count; ++i) {
        const sentinel_watch_entry_t *entry = sentinel_watch_history(&watch_state, i);
        if (entry && entry->radio == watch_detail.radio && entry->address_type == watch_detail.address_type &&
            entry->match.category == watch_detail.match.category && !memcmp(entry->address, watch_detail.address, 6)) {
            watch_detail = *entry; found = true; break;
        }
    }
    uint32_t age = (uint32_t)(now - watch_detail.last_ms) / 1000U;
    bool fresh = found && age <= 30 && !watch_paused && c5_online;
    ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(0, 0, 320, 480, UI_BACKGROUND), TAG, "recon signal clear");
    ESP_RETURN_ON_ERROR(draw_header("SIGNAL TRACKING"), TAG, "recon signal header");
    ESP_RETURN_ON_ERROR(draw_centered(80, watch_detail.match.label, UI_TEXT, 1), TAG, "recon signal device");
    ESP_RETURN_ON_ERROR(draw_centered(111, "EXACT BROADCAST ADDRESS / PASSIVE", UI_MUTED, 1), TAG, "recon signal identity");
    char line[48]; snprintf(line, sizeof(line), "%d dBm", watch_detail.rssi);
    ESP_RETURN_ON_ERROR(draw_centered(166, line, fresh ? UI_GOOD : UI_MUTED, 3), TAG, "recon rssi");
    int level = watch_detail.rssi + 100; if (level < 0) level = 0; if (level > 70) level = 70;
    ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(20, 218, 280, 26, UI_PANEL), TAG, "recon gauge background");
    if (level && fresh) ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(20, 218, level * 4, 26, UI_GOOD), TAG, "recon gauge");
    snprintf(line, sizeof(line), "%s / LAST SEEN %lus AGO", fresh ? "RECENT SAMPLE" : "STALE SAMPLE", (unsigned long)age);
    ESP_RETURN_ON_ERROR(draw_centered(270, line, fresh ? UI_GOOD : UI_MUTED, 1), TAG, "recon signal age");
    ESP_RETURN_ON_ERROR(draw_centered(307, "UPDATED AFTER EACH COMPLETED SWEEP", UI_MUTED, 1), TAG, "recon signal rate");
    ESP_RETURN_ON_ERROR(draw_centered(337, "SIGNAL STRENGTH IS NOT DISTANCE", UI_MUTED, 1), TAG, "recon signal scope");
    return draw_button(64, 426, 192, 44, "STOP TRACKING");
}
static bool recon_handle_control_tap(uint16_t x, uint16_t y, uint32_t now)
{
    if (x >= 64 && x < 256 && y >= 426 && y < 470) {
        watch_page = watch_page == WATCH_OPTIONS ? WATCH_LIVE : watch_page == WATCH_SIGNAL ? WATCH_ACTIONS : WATCH_DETAILS;
        recon_confirm = 0; return true;
    }
    if (watch_page == WATCH_OPTIONS && x >= 8 && x < 312 && y >= 120 && y < 164) {
        cerberus_recon_recorder_status_t status;
        cerberus_recon_recorder_get_status(&status);
        recon_confirm = 0; recon_message[0] = 0;
        if (status.worker_active) {
            if (status.state == CERBERUS_RECON_RECORDER_STARTING || status.state == CERBERUS_RECON_RECORDER_RECORDING)
                cerberus_recon_recorder_stop();
        } else if (!watch_paused) {
            esp_err_t error = cerberus_recon_recorder_start();
            if (error != ESP_OK) snprintf(recon_message, sizeof(recon_message), "%.48s", esp_err_to_name(error));
        }
        return true;
    }
    if (watch_page == WATCH_OPTIONS && x >= 8 && x < 312 && (x - 8) % 156 < 148 &&
        y >= 170 && y < 376 && (y - 170) % 54 < 44) {
        unsigned action = ((y - 170) / 54) * 2U + (x - 8) / 156;
        if (action >= 7U) return false;
        unsigned confirmation = recon_confirm; recon_confirm = 0;
        recon_message[0] = 0;
        if (action == 0) sentinel_watch_set_alert_filter(&watch_state,
            (sentinel_watch_confidence_t)(watch_state.minimum_confidence % 3 + 1), watch_state.muted, now);
        else if (action == 1) sentinel_watch_set_alert_filter(&watch_state, watch_state.minimum_confidence, !watch_state.muted, now);
        else if (action == 2) { watch_page = WATCH_FILTERS; watch_filters_page = 0; }
        else if (action == 3) {
            if (confirmation == 4) sentinel_watch_clear_device_policies(&watch_state); else recon_confirm = 4;
        } else if (action == 4) {
            if (confirmation == 5) { sentinel_watch_clear_history(&watch_state); watch_history_page = 0; }
            else recon_confirm = 5;
        } else if (action == 5U) {
            watch_radar = !watch_radar;
            if(recon_inventory_ui)recon_inventory_ui->paint_valid=false;
            snprintf(recon_message,sizeof(recon_message),"%s",watch_radar?
                "RADAR: SIGNAL ONLY / NO DIRECTION":"DASHBOARD: RETAINED OBSERVATIONS");
        } else {
            if (recon_inventory_ui) {
                recon_inventory_ui->filter = CERBERUS_RECON_INVENTORY_ALL;
                recon_inventory_ui->page = 0U;
                recon_inventory_ui->paint_valid = false;
            }
            watch_page = WATCH_INVENTORY;
        }
        return true;
    }
    if (watch_page != WATCH_ACTIONS || watch_detail.radio == SENTINEL_WATCH_RADIO_ENVIRONMENT) return false;
    if (x >= 8 && x < 312 && y >= 106 && y < 150) { watch_page = WATCH_SIGNAL; return true; }
    bool ok;
    if (x >= 8 && x < 312 && y >= 160 && y < 204) {
        bool ignored = !sentinel_watch_is_ignored(&watch_state, &watch_detail);
        ok = sentinel_watch_set_ignored(&watch_state, &watch_detail, ignored, now);
        snprintf(recon_message, sizeof(recon_message), "%s", ok ? ignored ? "DEVICE ALERTS IGNORED" : "DEVICE ALERTS RESTORED" : "DEVICE POLICY LIST FULL (16)");
    } else if (x >= 8 && x < 312 && (x - 8) % 156 < 148 && y >= 242 && y < 340 && (y - 242) % 54 < 44) {
        const unsigned minutes[] = {10, 30, 60, 240};
        unsigned duration = minutes[((y - 242) / 54) * 2 + (x - 8) / 156];
        ok = sentinel_watch_snooze(&watch_state, &watch_detail, duration * 60000U, now);
        if (ok) snprintf(recon_message, sizeof(recon_message), "ALERTS SNOOZED FOR %u MINUTES", duration);
        else snprintf(recon_message, sizeof(recon_message), "DEVICE POLICY LIST FULL (16)");
    } else return false;
    return true;
}
