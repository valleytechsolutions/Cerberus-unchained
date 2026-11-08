/* Included by wifi_packet_monitor_service.h; scanner_mutex owns commands.
 * This mode uses the same reservation, worker, lease and teardown as Monitor. */
static void channel_analyzer_reply_locked(uint64_t requested, bool info, esp_err_t error,
    cerberus_wifi_channel_analyzer_status_t *out)
{
    memset(out,0,sizeof(*out));
    out->session_id=info ? packet_monitor_session_epoch : requested;
    bool bound=channel_analyzer_mode && packet_monitor_session_id &&
        out->session_id==packet_monitor_session_id;
    if (!bound) {
        if (info && packet_monitor_held_locked()) out->flags=CERBERUS_WIFI_CHANNEL_ANALYZER_HELD;
        out->error_code=info ? (packet_monitor_held_locked() ? ESP_ERR_INVALID_STATE : ESP_OK) : error;
        return;
    }
    out->flags=CERBERUS_WIFI_CHANNEL_ANALYZER_BOUND;
    if (packet_monitor_held_locked()) out->flags|=CERBERUS_WIFI_CHANNEL_ANALYZER_HELD;
    if (atomic_load_explicit(&packet_monitor_cancelled,memory_order_acquire))
        out->flags|=CERBERUS_WIFI_CHANNEL_ANALYZER_CANCELLED;
    out->state=packet_monitor_status.state;out->channel=packet_monitor_status.channel;
    out->target_ms=CERBERUS_WIFI_CHANNEL_ANALYZER_TARGET_MS;
    out->latest_sequence=channel_analyzer_ring.latest_sequence;
    out->stored_count=channel_analyzer_ring.stored_count;
    out->error_code=error==ESP_OK ? packet_monitor_status.error_code : error;
}

void cerberus_wifi_channel_analyzer_info(cerberus_wifi_channel_analyzer_status_t *out)
{
    if (!out) return;
    memset(out,0,sizeof(*out));
    if (!scanner_mutex) {out->error_code=ESP_ERR_INVALID_STATE;return;}
    xSemaphoreTake(scanner_mutex,portMAX_DELAY);
    packet_monitor_session_init_locked();packet_monitor_expire_locked(esp_timer_get_time());
    channel_analyzer_reply_locked(0U,true,ESP_OK,out);
    xSemaphoreGive(scanner_mutex);
}

esp_err_t cerberus_wifi_channel_analyzer_start(const cerberus_wifi_channel_analyzer_start_t *request,
    cerberus_wifi_channel_analyzer_status_t *out)
{
    if (!request || !out) return ESP_ERR_INVALID_ARG;
    memset(out,0,sizeof(*out));out->session_id=request->session_id;
    if (!scanner_mutex) {out->error_code=ESP_ERR_INVALID_STATE;return ESP_ERR_INVALID_STATE;}
    esp_err_t error=request->session_id ?
        packet_monitor_start_internal(request->channel,request->session_id,false,true,NULL) : ESP_ERR_INVALID_ARG;
    xSemaphoreTake(scanner_mutex,portMAX_DELAY);
    channel_analyzer_reply_locked(request->session_id,false,error,out);
    xSemaphoreGive(scanner_mutex);return error;
}

static esp_err_t channel_analyzer_control(uint64_t session,bool stop,cerberus_wifi_channel_analyzer_status_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out,0,sizeof(*out));out->session_id=session;
    if (!scanner_mutex) {out->error_code=ESP_ERR_INVALID_STATE;return ESP_ERR_INVALID_STATE;}
    xSemaphoreTake(scanner_mutex,portMAX_DELAY);
    esp_err_t error=channel_analyzer_mode && session && session==packet_monitor_session_id ? ESP_OK : ESP_ERR_NOT_FOUND;
    bool notify=false;
    if (error==ESP_OK) {
        int64_t now=esp_timer_get_time();packet_monitor_expire_locked(now);
        if (stop) {
            atomic_store_explicit(&packet_monitor_cancelled,true,memory_order_release);
            packet_monitor_record_close_admission();
            if (packet_monitor_status.state==CERBERUS_WIFI_PACKET_MONITOR_ERROR && packet_monitor_held_locked())
                packet_monitor_cleanup_retry=true;
            notify=packet_monitor_held_locked();
        } else if (packet_monitor_status.state==CERBERUS_WIFI_PACKET_MONITOR_RUNNING &&
            !atomic_load_explicit(&packet_monitor_cancelled,memory_order_acquire)) {
            if (now>INT64_MAX-(int64_t)CERBERUS_WIFI_PACKET_MONITOR_LEASE_MS*1000)
                atomic_store_explicit(&packet_monitor_cancelled,true,memory_order_release);
            else packet_monitor_lease_deadline_us=now+(int64_t)CERBERUS_WIFI_PACKET_MONITOR_LEASE_MS*1000;
        }
    }
    channel_analyzer_reply_locked(session,false,error,out);
    xSemaphoreGive(scanner_mutex);
    if (notify) xTaskNotifyGive(scanner_task_handle);
    return error;
}

esp_err_t cerberus_wifi_channel_analyzer_status(uint64_t session,cerberus_wifi_channel_analyzer_status_t *out)
{return channel_analyzer_control(session,false,out);}
esp_err_t cerberus_wifi_channel_analyzer_stop(uint64_t session,cerberus_wifi_channel_analyzer_status_t *out)
{return channel_analyzer_control(session,true,out);}

esp_err_t cerberus_wifi_channel_analyzer_sample(const cerberus_wifi_channel_analyzer_read_t *request,
    cerberus_wifi_channel_analyzer_sample_t *out)
{
    if (!request || !out) return ESP_ERR_INVALID_ARG;
    memset(out,0,sizeof(*out));if (!scanner_mutex) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(scanner_mutex,portMAX_DELAY);
    cerberus_wifi_channel_analyzer_window_t sample;
    bool valid=channel_analyzer_mode && request->session_id && request->session_id==packet_monitor_session_id &&
        cerberus_wifi_channel_analyzer_get(&channel_analyzer_ring,request->sequence,&sample);
    if (valid) *out=(cerberus_wifi_channel_analyzer_sample_t){request->session_id,sample.sequence,
        sample.frames,sample.window_us,sample.channel,sample.flags};
    xSemaphoreGive(scanner_mutex);return valid ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t cerberus_wifi_channel_analyzer_hint_peek(uint64_t session,cerberus_wifi_channel_analyzer_hint_status_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out,0,sizeof(*out));out->session_id=session;out->age_ms=UINT32_MAX;
    if (!scanner_mutex) {out->error_code=ESP_ERR_INVALID_STATE;return ESP_ERR_INVALID_STATE;}
    xSemaphoreTake(scanner_mutex,portMAX_DELAY);
    esp_err_t error=channel_analyzer_mode && session && session==packet_monitor_session_id ? ESP_OK : ESP_ERR_NOT_FOUND;
    if (error==ESP_OK) {
        channel_analyzer_projection_valid=false;
        portENTER_CRITICAL(&packet_monitor_mux);
        cerberus_wifi_channel_analyzer_hint_t hint=channel_analyzer_hint;
        int64_t observed=channel_analyzer_hint_us;
        portEXIT_CRITICAL(&packet_monitor_mux);
        int64_t now=esp_timer_get_time();
        if (!(hint.flags & CERBERUS_WIFI_CHANNEL_ANALYZER_HINT_VALID)) error=ESP_ERR_NOT_FOUND;
        else if (observed<0 || now<observed || channel_analyzer_projection_id==UINT32_MAX) error=ESP_ERR_INVALID_STATE;
        else {
            channel_analyzer_projection=hint;
            channel_analyzer_projection_valid=true;
            out->projection=++channel_analyzer_projection_id;
            uint64_t age=(uint64_t)(now-observed)/1000U;
            out->age_ms=age>=UINT32_MAX ? UINT32_MAX : (uint32_t)age;
            out->length=CERBERUS_WIFI_CHANNEL_ANALYZER_HINT_BYTES;
            out->flags=CERBERUS_WIFI_CHANNEL_ANALYZER_HINT_VALID;
        }
    }
    out->error_code=error;
    xSemaphoreGive(scanner_mutex);return error;
}

esp_err_t cerberus_wifi_channel_analyzer_hint_read(const cerberus_wifi_channel_analyzer_hint_request_t *request,
    cerberus_wifi_channel_analyzer_hint_part_t *out)
{
    if (!request || !out) return ESP_ERR_INVALID_ARG;
    memset(out,0,sizeof(*out));if (!scanner_mutex) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(scanner_mutex,portMAX_DELAY);
    bool valid=channel_analyzer_mode && request->session_id && request->session_id==packet_monitor_session_id &&
        channel_analyzer_projection_valid && request->projection==channel_analyzer_projection_id &&
        request->part<CERBERUS_WIFI_CHANNEL_ANALYZER_HINT_PARTS;
    if (valid) {
        out->session_id=request->session_id;out->projection=request->projection;out->part=request->part;
        size_t offset=(size_t)request->part*CERBERUS_WIFI_CHANNEL_ANALYZER_HINT_PART_BYTES;
        size_t bytes=sizeof(channel_analyzer_projection)-offset;
        if (bytes>sizeof(out->bytes)) bytes=sizeof(out->bytes);
        memcpy(out->bytes,(const uint8_t *)&channel_analyzer_projection+offset,bytes);
    }
    xSemaphoreGive(scanner_mutex);return valid ? ESP_OK : ESP_ERR_NOT_FOUND;
}
