/* Private to ui.c. Navigation waits for the claimed C5 session to release. */
static esp_err_t packet_monitor_record_exchange(void *context, unsigned source,
    uint8_t command, const void *request, size_t request_size, void *reply, size_t reply_size)
{
    (void)context;
    if (source != 1U) return ESP_ERR_INVALID_ARG;
    uint8_t mapped;
    switch (command) {
    case CERBERUS_PACKET_COUNT_COMMAND_PEEK: mapped = CERBERUS_MONITOR_RECORD_PEEK; break;
    case CERBERUS_PACKET_COUNT_COMMAND_READ: mapped = CERBERUS_MONITOR_RECORD_READ; break;
    case CERBERUS_PACKET_COUNT_COMMAND_RAW_READ: mapped = CERBERUS_MONITOR_RECORD_RAW_READ; break;
    case CERBERUS_PACKET_COUNT_COMMAND_RAW_ACK: mapped = CERBERUS_MONITOR_RECORD_RAW_ACK; break;
    case CERBERUS_PACKET_COUNT_COMMAND_STATUS: mapped = CERBERUS_MONITOR_RECORD_STATUS; break;
    default: return ESP_ERR_NOT_SUPPORTED;
    }
    return c5_spi_link_wifi_packet_monitor_record_exchange(mapped, request, request_size, reply, reply_size);
}

static esp_err_t packet_monitor_record_bind(uint64_t session)
{ return wifi_packet_count_record_bind(packet_monitor_record_job, 1U, session); }

static bool packet_monitor_record_drained(uint64_t session)
{ return wifi_packet_count_record_drained(packet_monitor_record_job, 1U, session); }

static esp_err_t packet_monitor_record_release(uint64_t session, bool *released)
{
    *released = false;
    cerberus_packet_count_request_t request = {session};
    cerberus_packet_count_control_t reply;
    esp_err_t error = c5_spi_link_wifi_packet_monitor_record_exchange(CERBERUS_MONITOR_RECORD_RELEASE,
        &request, sizeof(request), &reply, sizeof(reply));
    if (error != ESP_OK) return error;
    if (reply.session_id != session) return ESP_ERR_INVALID_RESPONSE;
    *released = !reply.flags && (reply.error_code == ESP_OK || reply.error_code == ESP_ERR_NOT_FOUND);
    return *released || reply.error_code == ESP_OK ? ESP_OK : reply.error_code;
}

static const wifi_packet_monitor_transport_t packet_monitor_transport = {
    .info = c5_spi_link_wifi_packet_monitor_session_info,
    .start = c5_spi_link_wifi_packet_monitor_session_start,
    .status = c5_spi_link_wifi_packet_monitor_session_status,
    .stop = c5_spi_link_wifi_packet_monitor_session_stop,
    .sample = c5_spi_link_wifi_packet_monitor_session_sample,
    .record_start = c5_spi_link_wifi_packet_monitor_record_start,
    .record_bind = packet_monitor_record_bind,
    .record_drained = packet_monitor_record_drained,
    .record_release = packet_monitor_record_release,
};

static bool packet_monitor_can_restart(void)
{
    if (packet_monitor_record_preparing || packet_monitor_record_job) return false;
    return packet_monitor.phase == WIFI_MONITOR_OFF ||
        (!packet_monitor.recording && packet_monitor.phase == WIFI_MONITOR_WATCH &&
        (packet_monitor.view.state == CERBERUS_WIFI_PACKET_MONITOR_IDLE ||
         packet_monitor.view.state == CERBERUS_WIFI_PACKET_MONITOR_ERROR));
}

static void packet_monitor_start_ui(void)
{
    if (packet_monitor.phase != WIFI_MONITOR_OFF || packet_monitor_record_job) {
        /* Reopening after sleep can race a pending release. Keep the old
         * channel visible until that receiver has actually stopped. */
        packet_monitor_next_channel = packet_monitor_channel;
        packet_monitor_channel = packet_monitor.channel;
        packet_monitor_action = PACKET_MONITOR_CHANNEL;
        wifi_packet_monitor_scan_stop(&packet_monitor);
        packet_monitor_record_preparing = false;
        return;
    }
    packet_monitor_action = PACKET_MONITOR_NONE;
    packet_monitor_record_redraw_ms = 0U;
    memset(&packet_monitor_record_status, 0, sizeof(packet_monitor_record_status));
    if (packet_monitor_record_enabled) {
        static const wifi_packet_count_transport_t transport = {.exchange = packet_monitor_record_exchange};
        packet_monitor_record_status.error = wifi_packet_count_record_start_monitor(&transport,
            &packet_monitor_record_job);
        packet_monitor_record_preparing = packet_monitor_record_status.error == ESP_OK;
    } else wifi_packet_monitor_scan_start(&packet_monitor, packet_monitor_channel);
}

static void packet_monitor_request_action(packet_monitor_action_t action)
{
    /* Back can replace a pending retune. Repeated taps never restart cleanup. */
    if (packet_monitor_action != PACKET_MONITOR_NONE &&
        action != PACKET_MONITOR_BACK_AP && action != PACKET_MONITOR_BACK_LIST &&
        action != PACKET_MONITOR_BACK_MENU) return;
    packet_monitor_action = action;
    packet_monitor_record_preparing = false;
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
        packet_monitor_record_preparing = false;
        wifi_packet_monitor_scan_stop(&packet_monitor);
    }
    bool changed = false;
    if (packet_monitor_record_job) {
        wifi_packet_count_record_status_t status;
        if (wifi_packet_count_record_status(packet_monitor_record_job, &status)) {
            /* Publish counters without repainting all graphs for every frame.
             * Ownership and errors still become visible on the next UI tick. */
            changed = status.ready != packet_monitor_record_status.ready ||
                status.busy != packet_monitor_record_status.busy ||
                status.error != packet_monitor_record_status.error ||
                status.source[1].error != packet_monitor_record_status.source[1].error ||
                status.source[1].statistics_valid != packet_monitor_record_status.source[1].statistics_valid ||
                now_ms >= packet_monitor_record_redraw_ms;
            if (changed) packet_monitor_record_redraw_ms = now_ms > UINT64_MAX - 250U ?
                UINT64_MAX : now_ms + 250U;
            packet_monitor_record_status = status;
            if (packet_monitor_record_preparing) {
                if (status.ready) {
                    wifi_packet_monitor_scan_record(&packet_monitor, packet_monitor_channel);
                    packet_monitor_record_preparing = false;
                } else if (!status.busy) packet_monitor_record_preparing = false;
            }
        }
    }
    changed |= wifi_packet_monitor_scan_tick(&packet_monitor, now_ms,
        &packet_monitor_transport);
    if (packet_monitor.phase == WIFI_MONITOR_OFF && packet_monitor_record_job &&
        !packet_monitor_record_preparing) {
        wifi_packet_count_record_finish(packet_monitor_record_job);
        if (packet_monitor_record_status.busy) return changed;
        if (wifi_packet_count_record_release(packet_monitor_record_job) != ESP_OK) return changed;
        packet_monitor_record_job = 0U;
        changed = true;
    }
    if (!visible || packet_monitor.phase != WIFI_MONITOR_OFF ||
        packet_monitor_record_preparing || packet_monitor_record_job ||
        packet_monitor_action == PACKET_MONITOR_NONE) return changed;
    packet_monitor_action_t action = packet_monitor_action;
    packet_monitor_action = PACKET_MONITOR_NONE;
    switch (action) {
    case PACKET_MONITOR_TOGGLE_RECORD:
        packet_monitor_record_enabled = !packet_monitor_record_enabled;
        packet_monitor_start_ui();
        break;
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
