/* Included before wifi_target_ui.h. The three receive controllers share the
 * selected-AP entry, but each retains its own scope and exact capture owner. */
static cerberus_c5_wifi_result_t capture_target;
static uint8_t capture_mode_index;
static char capture_ssid[33];
static const uint8_t capture_modes[] = {
    CERBERUS_WIFI_CAPTURE_MODE_RAW,
    CERBERUS_WIFI_CAPTURE_MODE_PROBE,
    CERBERUS_WIFI_CAPTURE_MODE_DEAUTH,
    CERBERUS_WIFI_CAPTURE_MODE_EAPOL,
    CERBERUS_WIFI_CAPTURE_MODE_SAE_COMMIT,
};

static bool capture_busy(void)
{
    if (frame_ui_busy() || frame_ui_pending() || eapol_ui_busy() ||
        eapol_ui_pending() || sae_ui_busy() || sae_ui_pending()) return true;
    cerberus_s3_capture_status_t status;
    cerberus_s3_capture_status(&status);
    return status.busy;
}

static esp_err_t rx_tools_draw(void)
{
    bool ready = !capture_busy() && !frame_ui_is_open() &&
        !eapol_ui_is_open() && !sae_ui_is_open();
    ESP_RETURN_ON_ERROR(draw_centered(72, "RX TOOLS", UI_TEXT, 2), TAG, "receive title");
    ESP_RETURN_ON_ERROR(draw_centered(108, "PASSIVE WI-FI CAPTURE AND ANALYSIS", UI_MUTED, 1), TAG, "receive scope");
    ESP_RETURN_ON_ERROR(cerberus_ui_widget_draw_button((cerberus_ui_rect_t){12,126,296,56},
        "FRAME CAPTURE", 2, ready, UI_WIDGET_THEME), TAG, "frame entry");
    ESP_RETURN_ON_ERROR(draw_centered(198, "CHANNELS / FRAMES / OPTIONAL PCAP", UI_MUTED, 1), TAG, "frame scope");
    ESP_RETURN_ON_ERROR(cerberus_ui_widget_draw_button((cerberus_ui_rect_t){12,222,296,56},
        "EAPOL MONITOR", 2, ready, UI_WIDGET_THEME), TAG, "EAPOL entry");
    ESP_RETURN_ON_ERROR(draw_centered(294, "OBSERVED KEY EXCHANGE PATTERNS", UI_MUTED, 1), TAG, "EAPOL scope");
    ESP_RETURN_ON_ERROR(cerberus_ui_widget_draw_button((cerberus_ui_rect_t){12,318,296,56},
        "SAE MONITOR", 2, ready, UI_WIDGET_THEME), TAG, "SAE entry");
    return draw_centered(390, ready ? "OBSERVED WPA3 AUTHENTICATION FRAMES" :
        "FINISHING PREVIOUS CAPTURE", UI_MUTED, 1);
}

static void rx_tools_tap(uint16_t x, uint16_t y)
{
    if (x < 12U || x >= 308U || capture_busy() || frame_ui_is_open() ||
        eapol_ui_is_open() || sae_ui_is_open()) return;
    if (y >= 126U && y < 182U) frame_ui_open(true, 0U);
    else if (y >= 222U && y < 278U) eapol_ui_open(true);
    else if (y >= 318U && y < 374U) sae_ui_open(true);
}

static void capture_enter(void)
{
    if (frame_ui_is_open() || eapol_ui_is_open() || sae_ui_is_open()) return;
    capture_target = wifi_selected_result;
    snprintf(capture_ssid, sizeof(capture_ssid), "%s",
        capture_target.ssid[0] ? capture_target.ssid : "<HIDDEN SSID>");
    for (char *p = capture_ssid; *p; ++p)
        if ((unsigned char)*p < 32U || (unsigned char)*p > 126U) *p = '?';
    capture_mode_index = 0U;
}

static esp_err_t capture_draw(void)
{
    if (frame_ui_is_open()) return frame_ui_draw();
    if (eapol_ui_is_open()) return eapol_ui_draw();
    if (sae_ui_is_open()) return sae_ui_draw();
    if (capture_mode_index >= sizeof(capture_modes) / sizeof(capture_modes[0]))
        capture_mode_index = 0U;
    if (capture_modes[capture_mode_index] == CERBERUS_WIFI_CAPTURE_MODE_EAPOL) {
        eapol_ui_open(false);
        return eapol_ui_draw();
    }
    if (capture_modes[capture_mode_index] == CERBERUS_WIFI_CAPTURE_MODE_SAE_COMMIT) {
        sae_ui_open(false);
        return sae_ui_draw();
    }
    frame_ui_open(false, capture_mode_index);
    return frame_ui_draw();
}

static void capture_tap(uint16_t x, uint16_t y)
{
    if (x < 12U || x >= 308U) return;
    if (y >= 150U && y < 190U && !capture_busy()) {
        if (frame_ui_is_open()) frame_ui_close(false);
        else if (eapol_ui_is_open()) eapol_ui_close(false);
        else if (sae_ui_is_open()) sae_ui_close(false);
        return;
    }
    if (frame_ui_is_open()) frame_ui_tap(x, y);
    else if (eapol_ui_is_open()) eapol_ui_tap(x, y);
    else if (sae_ui_is_open()) sae_ui_tap(x, y);
}

static bool capture_back(void)
{
    if (frame_ui_is_open()) { frame_ui_close(true); return false; }
    if (eapol_ui_is_open()) { eapol_ui_close(true); return false; }
    if (sae_ui_is_open()) { sae_ui_close(true); return false; }
    return true;
}
