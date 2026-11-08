/* Included by the sole Wardriving writer. The immutable area configuration
 * lives in its PSRAM workspace; RF callbacks never evaluate GPS or touch SD. */
typedef struct {
    cerberus_geofence_result_t result;
    uint32_t checked_ms;
    bool enabled, checked, hold_rows;
    uint8_t phase;
} wardrive_stream_geofence_t;

static void stream_area_poll(wardrive_stream_geofence_t *area, bool force)
{
    if (!area->enabled || (!force && area->checked &&
            (uint32_t)(now_ms() - area->checked_ms) < 500U)) return;
    cerberus_wardrive_gps_snapshot_t gps;
    esp_err_t error;
    snapshot_optional_gps(true, &gps, &error);
    area->checked = true; area->checked_ms = now_ms();
    area->result = cerberus_geofence_evaluate(&wardrive_geofences, &gps);
    if (area->result.state != CERBERUS_GEOFENCE_INVALID &&
            (error != ESP_OK || !gps.utc_valid || !gps.utc_from_rmc ||
             gps.utc_age_ms > CERBERUS_WARDRIVE_GPS_FIX_MAX_AGE_MS)) {
        area->result = (cerberus_geofence_result_t){.state = CERBERUS_GEOFENCE_WAIT_GPS,
            .slot = CERBERUS_GEOFENCE_SLOTS, .pause = true};
    }
    portENTER_CRITICAL(&wardrive_lock);
    wardrive_status.geofence = area->result;
    wardrive_status.geofence_name[0] = '\0';
    if (area->result.slot < CERBERUS_GEOFENCE_SLOTS)
        memcpy(wardrive_status.geofence_name, wardrive_geofences.areas[area->result.slot].name,
            sizeof(wardrive_status.geofence_name));
    portEXIT_CRITICAL(&wardrive_lock);
}

static void stream_area_phase(wardrive_stream_geofence_t *area, uint8_t phase)
{
    portENTER_CRITICAL(&wardrive_lock);
    if (phase == CERBERUS_WARDRIVE_AREA_PAUSED && area->phase != phase)
        wardrive_status.geofence_pauses = plus_one(wardrive_status.geofence_pauses);
    area->phase = phase; wardrive_status.geofence_phase = phase;
    if (!stop_requested && phase == CERBERUS_WARDRIVE_AREA_PAUSED)
        wardrive_status.state = CERBERUS_WARDRIVE_RUNNING;
    wardrive_status.elapsed_ms = (uint32_t)(now_ms64() - started_ms);
    portEXIT_CRITICAL(&wardrive_lock);
}

static esp_err_t stream_area_discard(const cerberus_wd_record_t *row)
{
    if ((row->kind != 1U && row->kind != 2U) || row->label_length > 32U)
        return ESP_ERR_INVALID_RESPONSE;
    portENTER_CRITICAL(&wardrive_lock);
    wardrive_status.geofence_skipped = plus_one(wardrive_status.geofence_skipped);
    portEXIT_CRITICAL(&wardrive_lock);
    return ESP_OK;
}

static bool stream_area_retry(esp_err_t error)
{
    return error == ESP_ERR_TIMEOUT || error == ESP_ERR_INVALID_RESPONSE || error == ESP_ERR_NOT_FINISHED;
}

static esp_err_t stream_area_hold(wardrive_stream_geofence_t *area,
    cerberus_wd_request_t queries[2], cerberus_wd_status_t radios[2],
    bool attempted[2], bool begun[2], uint32_t last_ok[2])
{
    bool paused[2] = {false, false}, drained[2] = {false, false};
    bool resuming = false;
    uint32_t transition = now_ms();
    area->hold_rows = true;
    stream_area_phase(area, CERBERUS_WARDRIVE_AREA_PAUSING);
    while (!wants_stop()) {
        stream_area_poll(area, false);
        if (area->result.state == CERBERUS_GEOFENCE_INVALID) return ESP_ERR_INVALID_STATE;
        if (resuming && area->result.pause) {
            resuming = false; transition = now_ms();
            memset(paused, 0, sizeof(paused)); memset(drained, 0, sizeof(drained));
            stream_area_phase(area, CERBERUS_WARDRIVE_AREA_PAUSING);
        }
        bool all_running = true;
        for (unsigned chip = 0; chip < 2U && !wants_stop(); ++chip) {
            if (!attempted[chip]) { paused[chip] = drained[chip] = true; continue; }
            cerberus_wd_request_t *q = &queries[chip];
            uint8_t command = resuming ? CERBERUS_WD_RESUME : CERBERUS_WD_PAUSE;
            esp_err_t error = stream_exchange(chip == 1U, command, q, &radios[chip]);
            cerberus_wd_status_t *radio = &radios[chip];
            if ((error == ESP_ERR_INVALID_STATE || error == ESP_OK) &&
                    radio->state == CERBERUS_WD_OFF && radio->error == ESP_ERR_INVALID_STATE) {
                /* A START may never have arrived. This exact candidate is absent. */
                attempted[chip] = begun[chip] = false;
                paused[chip] = drained[chip] = true; stream_publish(chip == 1U, radio); continue;
            }
            if (error == ESP_OK) {
                error = (esp_err_t)radio->error;
                if (error == ESP_OK && (!radio->held || radio->state == CERBERUS_WD_STOPPED)) error = ESP_FAIL;
            }
            if (error != ESP_OK) {
                all_running = false;
                if (stream_area_retry(error) && (uint32_t)(now_ms() - last_ok[chip]) < 5000U) continue;
                publish_error(error, chip ? 'C' : 'B'); return error;
            }
            begun[chip] = true; last_ok[chip] = now_ms(); stream_publish(chip == 1U, radio);
            if (resuming) { all_running &= radio->state == CERBERUS_WD_RUNNING; continue; }
            paused[chip] = radio->state == CERBERUS_WD_PAUSED && !radio->phase && !radio->channel;
            /* Keep both queues moving while the RF shutdown completes. Only a
             * zero READ after confirmed PAUSED proves a pre-resume queue empty. */
            for (unsigned batch = 0; batch < 16U && !wants_stop(); ++batch) {
                cerberus_wd_record_t row = {0};
                error = stream_exchange(chip == 1U, CERBERUS_WD_READ, q, &row);
                if (error != ESP_OK) break;
                if (!row.sequence) { drained[chip] = paused[chip]; break; }
                drained[chip] = false;
                if (row.sequence <= q->ack) { error = ESP_ERR_INVALID_RESPONSE; break; }
                error = stream_area_discard(&row);
                if (error != ESP_OK) break;
                q->ack = row.sequence;
            }
            if (error != ESP_OK && !stream_area_retry(error)) {
                publish_error(error, chip ? 'C' : 'B'); return error;
            }
        }
        if (wants_stop()) break;
        if (resuming && all_running) {
            area->hold_rows = false;
            stream_area_phase(area, CERBERUS_WARDRIVE_AREA_OPEN);
            return ESP_OK;
        }
        if (!resuming && paused[0] && paused[1] && drained[0] && drained[1]) {
            stream_area_phase(area, CERBERUS_WARDRIVE_AREA_PAUSED);
            if (!area->result.pause) {
                resuming = true; transition = now_ms();
                stream_area_phase(area, CERBERUS_WARDRIVE_AREA_RESUMING);
            }
        } else if ((uint32_t)(now_ms() - transition) >= 5000U) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(50U) ? pdMS_TO_TICKS(50U) : 1U);
    }
    return ESP_OK; /* Stop cleanup still excludes the remaining boundary rows. */
}
