/* Private to ui.c. Navigation waits for the claimed C5 session to release. */
static const wifi_packet_monitor_transport_t packet_monitor_transport = {
    .info = c5_spi_link_wifi_packet_monitor_session_info,
    .start = c5_spi_link_wifi_packet_monitor_session_start,
    .status = c5_spi_link_wifi_packet_monitor_session_status,
    .stop = c5_spi_link_wifi_packet_monitor_session_stop,
    .sample = c5_spi_link_wifi_packet_monitor_session_sample,
};

static void packet_monitor_start_ui(void)
{
    if (wifi_packet_monitor_scan_start(&packet_monitor, packet_monitor_channel))
        packet_monitor_action = PACKET_MONITOR_NONE;
    else if (packet_monitor.phase != WIFI_MONITOR_OFF) {
        /* Reopening after sleep can race a pending release. Keep the old
         * channel visible until that receiver has actually stopped. */
        packet_monitor_next_channel = packet_monitor_channel;
        packet_monitor_channel = packet_monitor.channel;
        packet_monitor_action = PACKET_MONITOR_CHANNEL;
        wifi_packet_monitor_scan_stop(&packet_monitor);
    }
}

static void packet_monitor_request_action(packet_monitor_action_t action)
{
    /* Back can replace a pending retune. Repeated taps never restart cleanup. */
    if (packet_monitor_action != PACKET_MONITOR_NONE &&
        action != PACKET_MONITOR_BACK_AP && action != PACKET_MONITOR_BACK_LIST &&
        action != PACKET_MONITOR_BACK_MENU) return;
    packet_monitor_action = action;
    wifi_packet_monitor_scan_stop(&packet_monitor);
}

static void packet_monitor_step_channel(int direction)
{
    if (packet_monitor_action != PACKET_MONITOR_NONE) return;
    uint16_t candidate = packet_monitor_channel;
    do {
        candidate = direction > 0 ? candidate == 165U ? 1U : candidate + 1U :
            candidate == 1U ? 165U : candidate - 1U;
    } while (!cerberus_wifi_packet_monitor_channel_valid((uint8_t)candidate));
    packet_monitor_next_channel = (uint8_t)candidate;
    packet_monitor_request_action(PACKET_MONITOR_CHANNEL);
}

static bool packet_monitor_tick(uint64_t now_ms, bool visible)
{
    if (!visible) {
        packet_monitor_action = PACKET_MONITOR_NONE;
        wifi_packet_monitor_scan_stop(&packet_monitor);
    }
    bool changed = wifi_packet_monitor_scan_tick(&packet_monitor, now_ms,
        &packet_monitor_transport);
    if (!visible || packet_monitor.phase != WIFI_MONITOR_OFF ||
        packet_monitor_action == PACKET_MONITOR_NONE) return changed;
    packet_monitor_action_t action = packet_monitor_action;
    packet_monitor_action = PACKET_MONITOR_NONE;
    switch (action) {
    case PACKET_MONITOR_CHANNEL:
        packet_monitor_channel = packet_monitor_next_channel;
        packet_monitor_start_ui();
        break;
    case PACKET_MONITOR_RESTART:
        packet_monitor_start_ui();
        break;
    case PACKET_MONITOR_SURVEY:
        wifi_mode = packet_monitor_channel <= 14U ?
            UI_WIFI_MODE_CHANNELS_24 : UI_WIFI_MODE_CHANNELS_5;
        wifi_channel_survey_begin_ui();
        break;
    case PACKET_MONITOR_BACK_AP:
        wifi_mode = UI_WIFI_MODE_TARGET;
        break;
    case PACKET_MONITOR_BACK_LIST:
        wifi_mode = UI_WIFI_MODE_LIST;
        wifi_scan_error = load_wifi_results();
        break;
    case PACKET_MONITOR_BACK_MENU:
        wifi_mode = UI_WIFI_MODE_MENU;
        break;
    default:
        break;
    }
    return true;
}
