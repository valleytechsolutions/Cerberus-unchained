/* Included once after the ordinary scanner. All state below uses its mutex. */
static uint32_t dual_now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000LL); }

static void dual_expire_locked(void)
{
    /* Expire only a quiescent bank. Failed SDK cancellation or pending controls
     * must retain the exact owner until cleanup actually finishes. */
    if (dual_attached && !scan_radio_owned_locked() &&
        (uint32_t)(dual_now_ms() - dual_last_contact_ms) >= CERBERUS_DUAL_BLE_ORPHAN_MS) {
        dual_attached = false;
        dual_track_seen = false;
        cerberus_ble_analyzer_counter_reset(&dual_analyzer);
        dual_snapshot.valid = false;
        response_free_locked();
    }
}

static bool dual_matches_locked(const cerberus_dual_ble_binding_t *b)
{
    return b->boot_id == dual_boot_id && dual_attached && b->job_id == dual_job_id;
}

static void dual_status_locked(uint32_t requested, int32_t error, cerberus_dual_ble_status_t *out)
{
    memset(out, 0, sizeof(*out));
    /* Keep the last job's negotiated version after detach so a lost RELEASE
     * reply can resolve as NOT_FOUND without turning into a version mismatch. */
    out->version = dual_job_id == requested &&
        (dual_version == CERBERUS_DUAL_BLE_LEGACY_VERSION || dual_version == CERBERUS_DUAL_BLE_VERSION) ?
        dual_version : CERBERUS_DUAL_BLE_VERSION;
    out->job_id = requested;
    out->state = CERBERUS_BLE_SCAN_ERROR;
    out->error_code = error;
    if (!error && dual_attached && dual_job_id == requested) {
        out->epoch = dual_epoch;
        out->generation = dual_generation;
        out->count = scan_status.stored_count;
        out->state = scan_status.state;
        out->error_code = dual_error ? dual_error : scan_status.error_code;
        out->flags = CERBERUS_DUAL_BLE_ATTACHED |
            (scan_radio_owned_locked() ? CERBERUS_DUAL_BLE_OWNED : 0U) |
            (MYNEWT_VAL(BLE_EXT_ADV) ? CERBERUS_DUAL_BLE_EXTENDED : 0U) |
            (dual_version >= 2U ? CERBERUS_DUAL_BLE_SNAPSHOTS : 0U);
    }
}

static void dual_start(const cerberus_dual_ble_start_t *r, cerberus_dual_ble_status_t *out)
{
    if (!scanner_mutex) {
        dual_status_locked(r->job_id, ESP_ERR_INVALID_STATE, out);
        return;
    }
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    dual_expire_locked();
    int32_t error = !cerberus_dual_ble_start_valid(r) ? ESP_ERR_INVALID_ARG :
        r->boot_id != dual_boot_id ? ESP_ERR_NOT_FOUND : 0;
    if (!error && dual_attached) {
        bool exact = r->job_id == dual_job_id && r->mode == dual_mode &&
            r->version == dual_version && r->reserved == dual_family &&
            r->address_type == dual_address_type && !memcmp(r->address, dual_address, 6U);
        if (exact) dual_last_contact_ms = dual_now_ms();
        dual_status_locked(r->job_id, exact ? 0 : ESP_ERR_INVALID_STATE, out);
        xSemaphoreGive(scanner_mutex);
        return; /* Lost START replies can be retried without another scan. */
    }
    if (!error && (scan_radio_owned_locked() ||
        dual_epoch_counter == UINT64_MAX)) error = ESP_ERR_INVALID_STATE;
    if (!error && (dual_job_highwater == UINT32_MAX || r->job_id != dual_job_highwater + 1U))
        error = ESP_ERR_INVALID_STATE;
    if (error) {
        dual_status_locked(r->job_id, error, out);
        xSemaphoreGive(scanner_mutex);
        return;
    }
    if (cerberus_dual_ble_response_mode(r->mode)) {
        if (dual_response) { dual_status_locked(r->job_id,ESP_ERR_INVALID_STATE,out); xSemaphoreGive(scanner_mutex); return; }
        dual_response=calloc(1U,sizeof(*dual_response));
        if (!dual_response) { dual_status_locked(r->job_id,ESP_ERR_NO_MEM,out); xSemaphoreGive(scanner_mutex); return; }
    }
    dual_attached = true;
    dual_job_highwater = r->job_id;
    dual_job_id = r->job_id;
    dual_epoch = ++dual_epoch_counter;
    dual_mode = r->mode;
    dual_version = r->version;
    dual_family = r->reserved;
    dual_snapshot.valid = false;
    dual_snapshot.request_id = 0U;
    memcpy(dual_address, r->address, 6U);
    dual_address_type = r->address_type;
    dual_track_seen = false;
    dual_track_reports = 0U;
    dual_error = 0;
    dual_last_contact_ms = dual_now_ms();
    xSemaphoreGive(scanner_mutex);
    error = start_scan(true);
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    if (error) { dual_attached = false; response_free_locked(); }
    dual_status_locked(r->job_id, error, out);
    xSemaphoreGive(scanner_mutex);
}

static bool dual_snapshot_matches_locked(const cerberus_dual_ble_binding_t *binding, uint64_t handle)
{
    return dual_matches_locked(binding) && dual_version == CERBERUS_DUAL_BLE_VERSION &&
        dual_snapshot.valid && handle && handle == dual_snapshot.handle &&
        dual_snapshot.job_id == binding->job_id && dual_snapshot.epoch == dual_epoch;
}

static void dual_snapshot_status_locked(int32_t error, cerberus_dual_ble_snapshot_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->boot_id = dual_boot_id;
    out->error_code = error;
    if (!error) {
        out->handle = dual_snapshot.handle;
        out->count = dual_snapshot.count;
        out->flags = dual_snapshot.flags;
    }
}

/* Snapshot commands are bounded: if a callback owns the mutex, report BUSY
 * immediately. Copy at most 32 rows with no allocation, RF operation or wait. */
static bool dual_snapshot_command(uint8_t command, const void *request,
    size_t request_size, void *response, size_t *response_size)
{
    bool compact_response = command == CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_RESPONSE_COMPACT;
    bool response_read = command == CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_RESPONSE || compact_response;
    bool family = command == CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_FAMILY;
    bool metadata = command == CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_METADATA;
    bool reading = command == CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ || metadata || family || response_read;
    bool beginning = command == CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_BEGIN;
    cerberus_dual_ble_snapshot_read_t read = {0};
    cerberus_dual_ble_snapshot_begin_t begin = {0};
    cerberus_dual_ble_snapshot_binding_t binding = {0};
    cerberus_dual_ble_snapshot_status_t status = {.boot_id = dual_boot_id};
    cerberus_dual_ble_snapshot_chunk_t chunk = {0};
    size_t expected = reading ? sizeof(read) : beginning ? sizeof(begin) : sizeof(binding);
    int32_t error = request_size == expected ? ESP_OK : ESP_ERR_INVALID_ARG;
    if (!error) {
        if (reading) {
            memcpy(&read, request, sizeof(read));
            binding.binding = read.binding; binding.handle = read.handle;
            chunk.handle = read.handle; chunk.index = read.index; chunk.chunk = read.chunk;
        } else if (beginning) {
            memcpy(&begin, request, sizeof(begin)); binding.binding = begin.binding;
        } else memcpy(&binding, request, sizeof(binding));
    }
    bool locked = false;
    if (!error && !scanner_mutex) error = ESP_ERR_INVALID_STATE;
    if (!error) {
        locked = xSemaphoreTake(scanner_mutex, 0U) == pdTRUE;
        if (!locked) error = ESP_ERR_TIMEOUT;
    }
    if (locked) {
        dual_expire_locked();
        if (!dual_matches_locked(&binding.binding) || dual_version != CERBERUS_DUAL_BLE_VERSION)
            error = ESP_ERR_NOT_FOUND;
        else if (dual_error || scan_status.error_code)
            error = dual_error ? dual_error : scan_status.error_code;
        if (!error && beginning) {
            bool retry = begin.request_id && begin.request_id == dual_snapshot.request_id &&
                dual_snapshot_matches_locked(&binding.binding, dual_snapshot.handle);
            if (!retry) {
                int64_t freeze_us = -1;
                if (!begin.request_id || dual_snapshot.request_id == UINT64_MAX ||
                    begin.request_id != dual_snapshot.request_id + 1U ||
                    dual_snapshot.counter == UINT64_MAX ||
                    !cerberus_dual_ble_snapshot_mode(dual_mode) ||
                    (scan_status.state != CERBERUS_BLE_SCAN_RUNNING &&
                     scan_status.state != CERBERUS_BLE_SCAN_COMPLETE)) error = ESP_ERR_INVALID_STATE;
                else if (scan_status.stored_count > CERBERUS_DUAL_BLE_CAPACITY) error = ESP_ERR_INVALID_RESPONSE;
                if (!error) freeze_us = esp_timer_get_time();
                if (!error && dual_mode == CERBERUS_DUAL_BLE_REACQUIRE &&
                    (scan_status.stored_count > 4U || !scan_inventory.clock_valid ||
                     freeze_us < scan_inventory.clock_highwater_us)) error = ESP_ERR_INVALID_STATE;
                for (uint8_t i = 0U; !error && i < scan_status.stored_count; ++i) {
                    const cerberus_ble_record_t *r = &scan_results[i];
                    if (r->name_length > 32U || r->uuid_length > 16U || r->advertisement_length > 31U)
                        error = ESP_ERR_INVALID_RESPONSE;
                }
                if (!error) {
                    const uint32_t freeze_ms = (uint32_t)((uint64_t)freeze_us / 1000U);
                    memset(dual_snapshot.rows, 0, sizeof(dual_snapshot.rows));
                    memset(dual_snapshot.receive, 0, sizeof(dual_snapshot.receive));
                    if (cerberus_dual_ble_family_mode(dual_mode))
                        memset(dual_snapshot.family, 0, sizeof(dual_snapshot.family));
                    for (uint8_t i = 0U; i < scan_status.stored_count; ++i) {
                        const cerberus_ble_record_t *r = &scan_results[i];
                        cerberus_dual_ble_snapshot_row_t *out = &dual_snapshot.rows[i];
                        memcpy(out->address, r->address, 6U); out->address_type = r->address_type;
                        out->latest_rssi = r->latest_rssi;
                        out->name_length = r->name_length; out->uuid_length = r->uuid_length;
                        memcpy(out->name, r->name, r->name_length); memcpy(out->uuid, r->uuid, r->uuid_length);
                        out->manufacturer_id = r->manufacturer_id; out->peer_flags = r->flags;
                        memcpy(out->raw, r->advertisement, r->advertisement_length);
                        out->raw_length = r->advertisement_length; out->event_type = r->advertisement_event_type;
                        out->raw_flags = r->advertisement_flags; out->reports = r->reports_seen;
                        if (cerberus_dual_ble_receive_metadata_valid(&r->receive,
                                r->advertisement_length, r->advertisement_event_type,
                                r->advertisement_flags))
                            dual_snapshot.receive[i] = r->receive;
                        if (cerberus_dual_ble_family_mode(dual_mode))
                            dual_snapshot.family[i] = scan_family[i];
                        if (dual_response) response_freeze_locked(i,freeze_us);
                        out->age_ms = UINT32_MAX;
                        if (dual_response && r->original_time_valid && freeze_us>=0) {
                            uint64_t original_us=dual_response->live[i].original_us;
                            if ((uint64_t)freeze_us>=original_us &&
                                (uint64_t)freeze_us/1000U-original_us/1000U<UINT32_MAX) {
                                out->age_ms=(uint32_t)((uint64_t)freeze_us/1000U-original_us/1000U);
                                out->timing_flags|=CERBERUS_DUAL_BLE_SEEN;
                            }
                        } else if (!dual_response && r->original_time_valid && freeze_us >= 0) {
                            out->age_ms = freeze_ms - r->original_observed_ms;
                            out->timing_flags |= CERBERUS_DUAL_BLE_SEEN;
                        }
                        if (r->latest_rssi >= -127 && r->latest_rssi <= 20)
                            out->timing_flags |= CERBERUS_DUAL_BLE_RSSI_VALID;
                    }
                    dual_snapshot.handle = ++dual_snapshot.counter;
                    dual_snapshot.request_id = begin.request_id;
                    dual_snapshot.epoch = dual_epoch; dual_snapshot.job_id = dual_job_id;
                    dual_snapshot.count = scan_status.stored_count;
                    dual_snapshot.replaced = scan_inventory.replaced;
                    dual_snapshot.dropped = scan_inventory.dropped;
                    dual_snapshot.inventory_enabled = scan_inventory.enabled;
                    dual_snapshot.flags = (scan_radio_owned_locked() ?
                        CERBERUS_DUAL_BLE_SNAPSHOT_RUNNING : CERBERUS_DUAL_BLE_SNAPSHOT_COMPLETE) |
                        (MYNEWT_VAL(BLE_EXT_ADV) ? CERBERUS_DUAL_BLE_SNAPSHOT_EXTENDED : 0U);
                    dual_snapshot.valid = true;
                }
            }
        } else if (!error && !dual_snapshot_matches_locked(&binding.binding, binding.handle))
            error = ESP_ERR_NOT_FOUND;
        if (!error) dual_last_contact_ms = dual_now_ms();
        if (!reading) dual_snapshot_status_locked(error, &status);
        else if (!error) {
            if ((family && !cerberus_dual_ble_family_mode(dual_mode)) ||
                (response_read && (!cerberus_dual_ble_response_mode(dual_mode) || !dual_response))) error = ESP_ERR_INVALID_STATE;
            else if (read.index >= dual_snapshot.count || read.chunk >=
                    (response_read ? CERBERUS_DUAL_BLE_SNAPSHOT_RESPONSE_CHUNKS : family ? CERBERUS_DUAL_BLE_SNAPSHOT_FAMILY_CHUNKS : CERBERUS_DUAL_BLE_SNAPSHOT_CHUNKS))
                error = ESP_ERR_INVALID_ARG;
            else {
                const size_t offset = (size_t)read.chunk * CERBERUS_DUAL_BLE_SNAPSHOT_CHUNK_BYTES;
                const size_t row_size = compact_response ?
                    cerberus_dual_ble_compact_response_size(dual_response->frozen[read.index]) :
                    response_read ? CERBERUS_DUAL_BLE_SNAPSHOT_RESPONSE_ROW_BYTES : family ? CERBERUS_DUAL_BLE_SNAPSHOT_FAMILY_ROW_BYTES :
                    metadata ? CERBERUS_DUAL_BLE_SNAPSHOT_METADATA_ROW_BYTES : CERBERUS_DUAL_BLE_SNAPSHOT_ROW_BYTES;
                if (!row_size) error = ESP_ERR_INVALID_RESPONSE;
                else if (offset >= row_size) error = ESP_ERR_INVALID_ARG;
                const size_t remaining = error ? 0U : row_size - offset;
                const size_t length = remaining < sizeof(chunk.data) ? remaining : sizeof(chunk.data);
                /* Project immutable segments without a full-row stack copy.
                 * Compact UNKNOWN/NONE omit only validated canonical suffixes;
                 * the format marker replaces the response version at byte158. */
                for (size_t i = 0U; i < length; ++i) {
                    const size_t position = offset + i;
                    if (position < sizeof(cerberus_dual_ble_snapshot_row_t))
                        chunk.data[i] = ((const uint8_t *)&dual_snapshot.rows[read.index])[position];
                    else if (position < CERBERUS_DUAL_BLE_SNAPSHOT_METADATA_ROW_BYTES)
                        chunk.data[i] = ((const uint8_t *)&dual_snapshot.receive[read.index])[
                            position - sizeof(cerberus_dual_ble_snapshot_row_t)];
                    else if (position < CERBERUS_DUAL_BLE_SNAPSHOT_FAMILY_ROW_BYTES) {
                        if (cerberus_dual_ble_family_mode(dual_mode))
                            chunk.data[i] = ((const uint8_t *)&dual_snapshot.family[read.index])[
                                position - CERBERUS_DUAL_BLE_SNAPSHOT_METADATA_ROW_BYTES];
                    } else if (compact_response && position == CERBERUS_DUAL_BLE_SNAPSHOT_FAMILY_ROW_BYTES)
                        chunk.data[i] = CERBERUS_DUAL_BLE_COMPACT_RESPONSE_VERSION;
                    else chunk.data[i] = dual_response->frozen[read.index][
                        position - CERBERUS_DUAL_BLE_SNAPSHOT_FAMILY_ROW_BYTES];
                }
                chunk.byte_count = (uint8_t)length;
            }
        }
        xSemaphoreGive(scanner_mutex);
    }
    if (reading) {
        chunk.error = !error ? CERBERUS_DUAL_BLE_SNAPSHOT_OK :
            error == ESP_ERR_TIMEOUT ? CERBERUS_DUAL_BLE_SNAPSHOT_BUSY :
            error == ESP_ERR_NOT_FOUND ? CERBERUS_DUAL_BLE_SNAPSHOT_STALE :
            error == ESP_ERR_INVALID_ARG && request_size == expected ? CERBERUS_DUAL_BLE_SNAPSHOT_RANGE :
            CERBERUS_DUAL_BLE_SNAPSHOT_INVALID;
        memcpy(response, &chunk, sizeof(chunk)); *response_size = sizeof(chunk);
    } else {
        if (!locked) status.error_code = error;
        memcpy(response, &status, sizeof(status)); *response_size = sizeof(status);
    }
    return true;
}

static bool dual_snapshot_inventory_command(const void *request, size_t request_size,
    void *response, size_t *response_size)
{
    cerberus_dual_ble_snapshot_binding_t binding = {0};
    cerberus_dual_ble_snapshot_inventory_t out = {0};
    int32_t error = request_size == sizeof(binding) ? ESP_OK : ESP_ERR_INVALID_ARG;
    if (!error) { memcpy(&binding, request, sizeof(binding)); out.handle = binding.handle; }
    if (!error && !scanner_mutex) error = ESP_ERR_INVALID_STATE;
    bool locked = false;
    if (!error) {
        locked = xSemaphoreTake(scanner_mutex, 0U) == pdTRUE;
        if (!locked) error = ESP_ERR_TIMEOUT;
    }
    if (locked) {
        dual_expire_locked();
        if (!dual_snapshot_matches_locked(&binding.binding, binding.handle)) error = ESP_ERR_NOT_FOUND;
        else {
            dual_last_contact_ms = dual_now_ms();
            out.replaced = dual_snapshot.replaced;
            out.dropped = dual_snapshot.dropped;
            out.count = dual_snapshot.count;
            out.flags = dual_snapshot.inventory_enabled ? CERBERUS_DUAL_BLE_INVENTORY_REPLACEMENT : 0U;
        }
        xSemaphoreGive(scanner_mutex);
    }
    out.error_code = error;
    memcpy(response, &out, sizeof(out)); *response_size = sizeof(out);
    return true;
}

/* Independent passive sample: never wait behind a callback, copy an inventory
 * or mutate radio state. The existing owner protocol handles Stop/release. */
static bool dual_analyzer_sample_command(const void *request, size_t request_size,
    void *response, size_t *response_size)
{
    cerberus_dual_ble_binding_t binding = {0};
    cerberus_dual_ble_analyzer_sample_t out = {.rssi = 127};
    if (request_size == sizeof(binding)) {
        memcpy(&binding, request, sizeof(binding)); out.job_id = binding.job_id;
        if (scanner_mutex) {
            if (xSemaphoreTake(scanner_mutex, 0U) != pdTRUE) out.flags = CERBERUS_DUAL_BLE_ANALYZER_BUSY;
            else {
                dual_expire_locked();
                if (dual_matches_locked(&binding) && dual_version == CERBERUS_DUAL_BLE_VERSION &&
                    dual_mode == CERBERUS_DUAL_BLE_ANALYZER) {
                    dual_last_contact_ms = dual_now_ms();
                    if (!dual_error && !scan_status.error_code)
                        cerberus_ble_analyzer_counter_sample(&dual_analyzer, esp_timer_get_time(), &out);
                    out.epoch = dual_epoch; out.job_id = binding.job_id;
                }
                xSemaphoreGive(scanner_mutex);
            }
        }
    }
    memcpy(response, &out, sizeof(out)); *response_size = sizeof(out);
    return true;
}

bool cerberus_ble_dual_command(uint8_t command, const void *request,
    size_t request_size, void *response, size_t *response_size)
{
    if (!response || !response_size || (!request && request_size)) return false;
    *response_size = 0U;
    if (command == CERBERUS_SPI_COMMAND_DUAL_BLE_ANALYZER_SAMPLE)
        return dual_analyzer_sample_command(request, request_size, response, response_size);
    if (command == CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_INVENTORY)
        return dual_snapshot_inventory_command(request, request_size, response, response_size);
    if ((command >= CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_BEGIN &&
        command <= CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_STATUS) ||
        command == CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_METADATA ||
        command == CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_FAMILY ||
        command == CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_RESPONSE ||
        command == CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_RESPONSE_COMPACT)
        return dual_snapshot_command(command, request, request_size, response, response_size);
    if (command == CERBERUS_SPI_COMMAND_DUAL_BLE_INFO) {
        if (request_size) return false;
        cerberus_dual_ble_info_t out = { .boot_id = dual_boot_id, .source_now_ms = dual_now_ms(),
            .version = CERBERUS_DUAL_BLE_VERSION, .capacity = CERBERUS_DUAL_BLE_CAPACITY,
            .raw_bytes = CERBERUS_DUAL_BLE_RAW_BYTES };
        if (scanner_mutex) {
            xSemaphoreTake(scanner_mutex, portMAX_DELAY);
            dual_expire_locked();
            out.next_job_id = dual_job_highwater == UINT32_MAX ? 0U : dual_job_highwater + 1U;
            out.flags = CERBERUS_DUAL_BLE_SNAPSHOTS | (host_synced ? CERBERUS_DUAL_BLE_READY : 0U) |
                (MYNEWT_VAL(BLE_EXT_ADV) ? CERBERUS_DUAL_BLE_EXTENDED : 0U) |
                (dual_attached ? CERBERUS_DUAL_BLE_ATTACHED : 0U);
            xSemaphoreGive(scanner_mutex);
        }
        memcpy(response, &out, sizeof(out)); *response_size = sizeof(out); return true;
    }
    if (command == CERBERUS_SPI_COMMAND_DUAL_BLE_START) {
        if (request_size != sizeof(cerberus_dual_ble_start_t)) return false;
        cerberus_dual_ble_start_t r; cerberus_dual_ble_status_t out;
        memcpy(&r, request, sizeof(r)); dual_start(&r, &out);
        memcpy(response, &out, sizeof(out)); *response_size = sizeof(out); return true;
    }
    size_t expected = command == CERBERUS_SPI_COMMAND_DUAL_BLE_ROW_TIME ?
        sizeof(cerberus_dual_ble_row_request_t) : sizeof(cerberus_dual_ble_binding_t);
    if (request_size != expected || !scanner_mutex) return false;
    cerberus_dual_ble_binding_t binding;
    memcpy(&binding, request, sizeof(binding));
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    dual_expire_locked();
    bool valid = dual_matches_locked(&binding);
    if (valid) dual_last_contact_ms = dual_now_ms();
    if (command == CERBERUS_SPI_COMMAND_DUAL_BLE_ROW_TIME) {
        cerberus_dual_ble_row_request_t r; memcpy(&r, request, sizeof(r));
        cerberus_dual_ble_row_time_t out = {.job_id = binding.job_id, .age_ms = UINT32_MAX};
        if (valid && !r.reserved && scan_status.state == CERBERUS_BLE_SCAN_COMPLETE &&
            !scan_radio_owned_locked() && r.index < scan_status.stored_count) {
            const cerberus_ble_record_t *row = &scan_results[r.index];
            out.epoch = dual_epoch; out.index = r.index; out.generation = dual_generation;
            out.latest_rssi = row->latest_rssi; out.reports = row->reports_seen;
            if (row->original_time_valid) {
                out.flags |= CERBERUS_DUAL_BLE_SEEN;
                out.age_ms = dual_now_ms() - row->original_observed_ms;
            }
            if (row->latest_rssi >= -127 && row->latest_rssi <= 20)
                out.flags |= CERBERUS_DUAL_BLE_RSSI_VALID;
        }
        xSemaphoreGive(scanner_mutex);
        memcpy(response, &out, sizeof(out)); *response_size = sizeof(out); return true;
    }
    if (command == CERBERUS_SPI_COMMAND_DUAL_BLE_TRACK_STATUS) {
        cerberus_dual_ble_track_t out = {.job_id = binding.job_id, .age_ms = UINT32_MAX};
        if (valid && dual_mode == CERBERUS_DUAL_BLE_TRACK) {
            out.epoch = dual_epoch; out.reports = dual_track_reports; out.rssi = dual_track_rssi;
            if (dual_track_seen) {
                out.flags = CERBERUS_DUAL_BLE_SEEN | CERBERUS_DUAL_BLE_RSSI_VALID;
                out.age_ms = dual_now_ms() - dual_track_ms;
            }
        }
        xSemaphoreGive(scanner_mutex);
        memcpy(response, &out, sizeof(out)); *response_size = sizeof(out); return true;
    }
    if (command != CERBERUS_SPI_COMMAND_DUAL_BLE_STATUS &&
        command != CERBERUS_SPI_COMMAND_DUAL_BLE_STOP && command != CERBERUS_SPI_COMMAND_DUAL_BLE_RELEASE) {
        xSemaphoreGive(scanner_mutex); return false;
    }
    bool owned = scan_radio_owned_locked();
    if (valid && command == CERBERUS_SPI_COMMAND_DUAL_BLE_STOP && owned) {
        xSemaphoreGive(scanner_mutex);
        esp_err_t stop_error = stop_scan(true);
        xSemaphoreTake(scanner_mutex, portMAX_DELAY);
        valid = dual_matches_locked(&binding);
        owned = scan_radio_owned_locked();
        if (valid && stop_error == ESP_OK) dual_snapshot.valid = false;
    }
    cerberus_dual_ble_status_t out;
    dual_status_locked(binding.job_id, valid ? 0 : ESP_ERR_NOT_FOUND, &out);
    if (valid && command == CERBERUS_SPI_COMMAND_DUAL_BLE_STOP && !owned) dual_snapshot.valid = false;
    if (valid && command == CERBERUS_SPI_COMMAND_DUAL_BLE_RELEASE) {
        if (owned) out.error_code = ESP_ERR_INVALID_STATE;
        else {
            dual_attached = false; dual_track_seen = false;
            cerberus_ble_analyzer_counter_reset(&dual_analyzer);
            dual_snapshot.valid = false;
            response_free_locked();
            out.state = CERBERUS_BLE_SCAN_IDLE; out.flags = 0U; out.error_code = 0;
        }
    }
    xSemaphoreGive(scanner_mutex);
    memcpy(response, &out, sizeof(out)); *response_size = sizeof(out); return true;
}
