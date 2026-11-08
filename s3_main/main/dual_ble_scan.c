#include "dual_ble_scan.h"
#include "ble_family_wire.h"
#include "ble_response.h"
#include <string.h>

_Static_assert(sizeof(cerberus_dual_ble_snapshot_response_row_t) <= UINT16_MAX,
    "Snapshot row offset must fit the bounded reader");
_Static_assert((sizeof(cerberus_dual_ble_snapshot_response_row_t) +
    CERBERUS_DUAL_BLE_SNAPSHOT_CHUNK_BYTES - 1U) /
    CERBERUS_DUAL_BLE_SNAPSHOT_CHUNK_BYTES <= UINT8_MAX,
    "Snapshot chunk index must fit the bound wire request");
_Static_assert(offsetof(cerberus_dual_ble_snapshot_family_row_t, family) ==
    sizeof(cerberus_dual_ble_snapshot_metadata_row_t), "Family wire preserves metadata prefix");
_Static_assert(offsetof(cerberus_dual_ble_snapshot_response_row_t, response) ==
    sizeof(cerberus_dual_ble_snapshot_family_row_t), "Response wire preserves family prefix");
_Static_assert(CERBERUS_DUAL_BLE_COMPACT_RESPONSE_UNKNOWN_BYTES ==
    offsetof(cerberus_dual_ble_snapshot_response_row_t, response) + 2U,
    "Compact response header follows the unchanged original prefix");

static bool response_mode(uint8_t mode)
{ return mode == CERBERUS_DUAL_BLE_RESPONSE_DISCOVERY || mode == CERBERUS_DUAL_BLE_RESPONSE_FAMILY; }
static bool family_mode(uint8_t mode)
{ return mode == CERBERUS_DUAL_BLE_FAMILY || mode == CERBERUS_DUAL_BLE_RESPONSE_FAMILY; }

enum { OFF, INFO, START, POLL, META, NAME, UUID, RAW, TIME, FENCE, TRACK,
    SNAP_BEGIN, SNAP_READ, SNAP_FENCE, STOP, RELEASE, FAILED, SNAP_INVENTORY,
    ANALYZER, ANALYZER_FENCE };

void cerberus_dual_ble_init(cerberus_dual_ble_scan_t *s, const cerberus_dual_ble_ops_t *ops)
{
    if (!s) return;
    memset(s, 0, sizeof(*s));
    if (ops) s->ops = *ops;
}

bool cerberus_dual_ble_released(const cerberus_dual_ble_scan_t *s)
{ return !s || !s->start.job_id; }

static esp_err_t begin_request(cerberus_dual_ble_scan_t *s, uint8_t mode,
    const uint8_t address[6], uint8_t type, uint8_t family)
{
    if (!s || !s->ops.exchange || mode > CERBERUS_DUAL_BLE_REACQUIRE ||
        (family_mode(mode) ? family < 1U || family > 6U : family != 0U) ||
        (mode == CERBERUS_DUAL_BLE_TRACK && (!address || type > 3U)) ||
        (mode == CERBERUS_DUAL_BLE_REACQUIRE && (!address || type != CERBERUS_DUAL_BLE_ANY_OBSERVED_TYPE))) return ESP_ERR_INVALID_ARG;
    if (s->start.job_id || s->enabled) return ESP_ERR_INVALID_STATE;
    memset(&s->start, 0, sizeof(s->start));
    s->start.mode = mode;
    s->start.reserved = family;
    s->start.version = CERBERUS_DUAL_BLE_VERSION;
    if (mode == CERBERUS_DUAL_BLE_ANALYZER) memset(&s->analyzer, 0, sizeof(s->analyzer));
    if (mode == CERBERUS_DUAL_BLE_TRACK || mode == CERBERUS_DUAL_BLE_REACQUIRE) {
        memcpy(s->start.address, address, 6U);
        s->start.address_type = type;
        s->has_track = false;
    }
    s->enabled = true; s->stop_requested = false; s->error = ESP_OK;
    s->sweeps = 0U; s->snapshots = 0U; s->first_result_ms = 0U; s->has_first_result = false;
    s->live_updates = false; s->snapshots_capable = false;
    s->inventory_query_enabled = true; s->pending_inventory_known = false;
    s->metadata_read_enabled = true;
    s->compact_response_read_enabled = true;
    s->inventory_retries = 0U;
    s->discovery_clock_valid = s->ops.now_ms != NULL;
    s->discovery_started_ms = s->ops.now_ms ? s->ops.now_ms(s->ops.context) : 0U;
    if (s->discovery_started_ms == UINT64_MAX) s->discovery_clock_valid = false;
    s->phase = INFO; s->retries = 0; s->next_poll_ms = 0; s->last_tick_ms = 0;
    return ESP_OK;
}

esp_err_t cerberus_dual_ble_begin(cerberus_dual_ble_scan_t *s, uint8_t mode,
    const uint8_t address[6], uint8_t type)
{
    if (mode > CERBERUS_DUAL_BLE_ANALYZER) return ESP_ERR_INVALID_ARG;
    return begin_request(s, mode, address, type, 0U);
}

esp_err_t cerberus_dual_ble_begin_family(cerberus_dual_ble_scan_t *s, uint8_t family)
{ return begin_request(s, CERBERUS_DUAL_BLE_FAMILY, NULL, 0U, family); }

esp_err_t cerberus_dual_ble_begin_reacquire(cerberus_dual_ble_scan_t *s, const uint8_t address[6])
{
    if (!address) return ESP_ERR_INVALID_ARG;
    cerberus_dual_ble_start_t request = {.boot_id=1U,.job_id=1U,.version=CERBERUS_DUAL_BLE_VERSION,
        .mode=CERBERUS_DUAL_BLE_REACQUIRE,.address_type=CERBERUS_DUAL_BLE_ANY_OBSERVED_TYPE};
    memcpy(request.address,address,sizeof(request.address));
    if (!cerberus_dual_ble_start_valid(&request)) return ESP_ERR_INVALID_ARG;
    return begin_request(s,CERBERUS_DUAL_BLE_REACQUIRE,address,CERBERUS_DUAL_BLE_ANY_OBSERVED_TYPE,0U);
}

esp_err_t cerberus_dual_ble_begin_response(cerberus_dual_ble_scan_t *s, uint8_t family)
{
    if (family > 6U) return ESP_ERR_INVALID_ARG;
    return begin_request(s, family ? CERBERUS_DUAL_BLE_RESPONSE_FAMILY :
        CERBERUS_DUAL_BLE_RESPONSE_DISCOVERY, NULL, 0U, family);
}

void cerberus_dual_ble_stop(cerberus_dual_ble_scan_t *s)
{
    if (!s) return;
    s->enabled = false; s->stop_requested = true; s->next_poll_ms = 0;
    if (s->start.mode == CERBERUS_DUAL_BLE_ANALYZER) cerberus_dual_ble_analyzer_gap(&s->analyzer);
    s->phase = s->start.job_id ? STOP : OFF;
}

static void fail(cerberus_dual_ble_scan_t *s, esp_err_t error)
{
    s->error = error; s->enabled = false; s->next_poll_ms = 0;
    if (s->start.mode == CERBERUS_DUAL_BLE_ANALYZER) cerberus_dual_ble_analyzer_gap(&s->analyzer);
    s->phase = s->start.job_id ? STOP : FAILED;
}

static cerberus_dual_ble_binding_t binding(const cerberus_dual_ble_scan_t *s)
{ return (cerberus_dual_ble_binding_t){s->start.boot_id, s->start.job_id}; }

static bool status_valid(const cerberus_dual_ble_scan_t *s, const cerberus_dual_ble_status_t *r)
{
    return r->version == s->start.version && !r->reserved &&
        r->job_id == s->start.job_id && r->state <= CERBERUS_BLE_SCAN_ERROR &&
        r->count <= (s->start.mode == CERBERUS_DUAL_BLE_REACQUIRE ? 4U : CERBERUS_DUAL_BLE_CAPACITY) && !(r->flags & ~(CERBERUS_DUAL_BLE_ATTACHED |
            CERBERUS_DUAL_BLE_OWNED | CERBERUS_DUAL_BLE_EXTENDED | CERBERUS_DUAL_BLE_SNAPSHOTS)) &&
        (!(r->flags & CERBERUS_DUAL_BLE_SNAPSHOTS) || s->start.version == CERBERUS_DUAL_BLE_VERSION) &&
        (!(r->flags & CERBERUS_DUAL_BLE_ATTACHED) ||
            !!(r->flags & CERBERUS_DUAL_BLE_SNAPSHOTS) == s->snapshots_capable) &&
        (!(r->flags & CERBERUS_DUAL_BLE_ATTACHED) || r->epoch != 0U);
}

static bool same_bank(const cerberus_dual_ble_scan_t *s, const cerberus_dual_ble_status_t *r)
{
    return status_valid(s, r) && r->epoch == s->remote.epoch &&
        r->generation == s->remote.generation && (r->flags & CERBERUS_DUAL_BLE_ATTACHED);
}

static esp_err_t exchange(cerberus_dual_ble_scan_t *s, uint8_t cmd,
    const void *request, size_t request_size, void *reply, size_t reply_size)
{
    memset(reply, 0, reply_size);
    return s->ops.exchange(s->ops.context, cmd, request, request_size, reply, reply_size);
}

static void no_owner(cerberus_dual_ble_scan_t *s)
{
    if (s->start.mode == CERBERUS_DUAL_BLE_ANALYZER) cerberus_dual_ble_analyzer_gap(&s->analyzer);
    s->start.job_id = 0U; memset(&s->remote, 0, sizeof(s->remote));
    s->retries = 0U; s->next_poll_ms = 0U;
    s->phase = s->enabled ? INFO : s->error ? FAILED : OFF;
}

static uint64_t publish_bank(cerberus_dual_ble_scan_t *s, uint8_t count, uint64_t now)
{
    uint64_t published = s->ops.now_ms ? s->ops.now_ms(s->ops.context) : now;
    if (published < now) s->discovery_clock_valid = false;
    memcpy(s->rows, s->staging, sizeof(s->rows));
    /* Tick and consumers share one UI owner. Commit historical source identity
     * with the bank, before mutable START/remote fields can move to another job. */
    s->published_boot_id = s->start.boot_id;
    s->published_epoch = s->remote.epoch;
    s->published_generation = s->remote.generation;
    s->published_family = family_mode(s->start.mode) ? s->start.reserved : 0U;
    s->published_response = response_mode(s->start.mode);
    s->published_reacquire = s->start.mode == CERBERUS_DUAL_BLE_REACQUIRE;
    memset(s->published_address,0,sizeof(s->published_address));
    if (s->published_reacquire) memcpy(s->published_address,s->start.address,sizeof(s->published_address));
    s->inventory_known = s->live_updates && s->pending_inventory_known;
    s->inventory_replacement_enabled = s->inventory_known &&
        (s->pending_inventory.flags & CERBERUS_DUAL_BLE_INVENTORY_REPLACEMENT) != 0U;
    s->inventory_replaced = s->inventory_known ? s->pending_inventory.replaced : 0U;
    s->inventory_dropped = s->inventory_known ? s->pending_inventory.dropped : 0U;
    s->count = count;
    if (s->snapshots != UINT32_MAX) ++s->snapshots;
    if (count && !s->has_first_result && s->discovery_clock_valid) {
        if (published >= s->discovery_started_ms && published - s->discovery_started_ms <= UINT32_MAX) {
            s->first_result_ms = (uint32_t)(published - s->discovery_started_ms);
            s->has_first_result = true;
        } else {
            /* This was the first nonempty publication. A later bank must not
             * turn an unknown/overflowed first latency into a newer success. */
            s->discovery_clock_valid = false;
        }
    }
    return published;
}

static bool zero_bytes(const uint8_t *bytes, size_t count)
{
    for (size_t i = 0U; i < count; ++i) if (bytes[i]) return false;
    return true;
}

static bool snapshot_valid(const cerberus_dual_ble_scan_t *s,
    const cerberus_dual_ble_snapshot_status_t *r)
{
    uint8_t state = r->flags & (CERBERUS_DUAL_BLE_SNAPSHOT_RUNNING |
        CERBERUS_DUAL_BLE_SNAPSHOT_COMPLETE);
    return r->boot_id == s->start.boot_id && r->handle &&
        r->count <= (s->start.mode == CERBERUS_DUAL_BLE_REACQUIRE ? 4U : CERBERUS_DUAL_BLE_CAPACITY) &&
        !(r->flags & ~(CERBERUS_DUAL_BLE_SNAPSHOT_RUNNING |
            CERBERUS_DUAL_BLE_SNAPSHOT_COMPLETE | CERBERUS_DUAL_BLE_SNAPSHOT_EXTENDED)) &&
        (state == CERBERUS_DUAL_BLE_SNAPSHOT_RUNNING || state == CERBERUS_DUAL_BLE_SNAPSHOT_COMPLETE) &&
        !!(r->flags & CERBERUS_DUAL_BLE_SNAPSHOT_EXTENDED) == s->extended;
}

static esp_err_t decode_snapshot_row(cerberus_dual_ble_scan_t *s)
{
    const cerberus_dual_ble_snapshot_row_t *r = &s->snapshot_row;
    if (r->address_type > 3U || r->name_length > CERBERUS_BLE_NAME_MAX_LENGTH ||
        (r->uuid_length && r->uuid_length != 2U && r->uuid_length != 4U && r->uuid_length != 16U) ||
        r->raw_length > CERBERUS_DUAL_BLE_RAW_BYTES || r->event_type > 4U ||
        (r->peer_flags & ~CERBERUS_BLE_RESULT_CONNECTABLE) ||
        (r->raw_flags & ~(CERBERUS_BLE_ADVERTISEMENT_TRUNCATED | CERBERUS_BLE_ADVERTISEMENT_MALFORMED)) ||
        (r->timing_flags & ~(CERBERUS_DUAL_BLE_SEEN | CERBERUS_DUAL_BLE_RSSI_VALID)) ||
        ((r->timing_flags & CERBERUS_DUAL_BLE_RSSI_VALID) && (r->latest_rssi < -127 || r->latest_rssi > 20)) ||
        !zero_bytes(r->name + r->name_length, sizeof(r->name) - r->name_length) ||
        !zero_bytes(r->uuid + r->uuid_length, sizeof(r->uuid) - r->uuid_length) ||
        !zero_bytes(r->raw + r->raw_length, sizeof(r->raw) - r->raw_length) ||
        !cerberus_dual_ble_receive_metadata_valid(&s->snapshot_metadata.receive,
            r->raw_length, r->event_type, r->raw_flags)) return ESP_ERR_INVALID_RESPONSE;
    if (s->start.mode == CERBERUS_DUAL_BLE_REACQUIRE &&
        (memcmp(r->address,s->start.address,6U) || r->event_type > 3U ||
         s->snapshot_metadata.receive.source_kind == CERBERUS_DUAL_BLE_RECEIVE_UNKNOWN ||
         !(r->timing_flags & CERBERUS_DUAL_BLE_SEEN) || r->age_ms == UINT32_MAX))
        return ESP_ERR_INVALID_RESPONSE;
    if (family_mode(s->start.mode)) {
        cerberus_dual_ble_family_evidence_t evidence;
        if (s->snapshot_family.family.family_code != s->start.reserved ||
            !cerberus_dual_ble_family_wire_decode(&s->snapshot_family.family,
                &s->snapshot_metadata.receive, r->raw_length, r->event_type,
                r->raw_flags, r->address, r->address_type, &evidence)) return ESP_ERR_INVALID_RESPONSE;
    }
    if (response_mode(s->start.mode)) {
        if ((!family_mode(s->start.mode) &&
                !zero_bytes((const uint8_t *)&s->snapshot_family.family, sizeof(s->snapshot_family.family))) ||
            !cerberus_dual_ble_response_valid(&s->snapshot_response.response,
                &s->snapshot_family, s->start.reserved)) return ESP_ERR_INVALID_RESPONSE;
        const cerberus_dual_ble_response_wire_t *response = &s->snapshot_response.response;
        if (response->state == CERBERUS_DUAL_BLE_RESPONSE_PRESENT &&
            (response->flags & CERBERUS_DUAL_BLE_RESPONSE_AGE_VALID) &&
            (r->timing_flags & CERBERUS_DUAL_BLE_SEEN)) {
            /* Both ages were frozen on the C5 at one instant. Millisecond
             * quantization permits floor/ceil of the full-us interval only. */
            const uint32_t lower = response->interval_us / 1000U;
            const uint32_t upper = (response->interval_us + 999U) / 1000U;
            if (r->age_ms == UINT32_MAX || response->age_ms > r->age_ms ||
                r->age_ms - response->age_ms < lower || r->age_ms - response->age_ms > upper)
                return ESP_ERR_INVALID_RESPONSE;
        }
    }
    for (uint8_t i = 0U; i < s->read_index; ++i)
        if (s->staging[i].address_type == r->address_type &&
            !memcmp(s->staging[i].address, r->address, sizeof(r->address))) return ESP_ERR_INVALID_RESPONSE;
    cerberus_dual_ble_row_t *row = &s->staging[s->read_index];
    memcpy(row->address, r->address, sizeof(row->address)); row->address_type = r->address_type;
    row->rssi = r->latest_rssi; row->flags = r->peer_flags; row->manufacturer_id = r->manufacturer_id;
    row->reports = r->reports; row->uuid_length = r->uuid_length;
    memcpy(row->uuid, r->uuid, r->uuid_length);
    for (uint8_t i = 0U; i < r->name_length; ++i)
        row->name[i] = r->name[i] >= 32U && r->name[i] <= 126U ? (char)r->name[i] : '?';
    row->raw_length = r->raw_length; row->raw_flags = r->raw_flags; row->event_type = r->event_type;
    memcpy(row->raw, r->raw, r->raw_length); row->timing_flags = r->timing_flags;
    row->callback_age_ms = r->age_ms;
    /* All rows use FIRST BEGIN's anchor, including an idempotent retry whose
     * reply arrives later. Download/fence time can only make the upper age grow. */
    row->request_started_ms = s->snapshot_request_started_ms;
    row->receive = s->snapshot_metadata.receive;
    row->family = s->snapshot_family.family;
    row->response = s->snapshot_response.response;
    return ESP_OK;
}

static esp_err_t snapshot_step(cerberus_dual_ble_scan_t *s, uint64_t now)
{
    cerberus_dual_ble_binding_t b = binding(s);
    if (s->phase == SNAP_BEGIN) {
        cerberus_dual_ble_snapshot_begin_t request = {b, s->snapshot_request_id};
        cerberus_dual_ble_snapshot_status_t result;
        if (!s->snapshot_anchor_valid) {
            s->snapshot_request_started_ms = now; s->snapshot_anchor_valid = true;
        }
        esp_err_t error = exchange(s, CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_BEGIN,
            &request, sizeof(request), &result, sizeof(result));
        if (error != ESP_OK) return error;
        if (result.boot_id != s->start.boot_id) return ESP_ERR_INVALID_RESPONSE;
        if (result.error_code) return result.error_code;
        if (!snapshot_valid(s, &result) || result.handle <= s->snapshot.handle) return ESP_ERR_INVALID_RESPONSE;
        s->snapshot = result;
        s->pending_inventory_known = false; s->inventory_retries = 0U;
        memset(&s->pending_inventory, 0, sizeof(s->pending_inventory));
        memset(s->staging, 0, sizeof(s->staging));
        memset(&s->snapshot_response, 0, sizeof(s->snapshot_response));
        s->read_index = 0U; s->copied = 0U;
        s->phase = result.count ? SNAP_READ : s->inventory_query_enabled ? SNAP_INVENTORY : SNAP_FENCE;
    } else if (s->phase == SNAP_READ) {
        cerberus_dual_ble_snapshot_read_t request = {b, s->snapshot.handle, s->read_index,
            (uint8_t)(s->copied / CERBERUS_DUAL_BLE_SNAPSHOT_CHUNK_BYTES)};
        cerberus_dual_ble_snapshot_chunk_t result;
        bool response = response_mode(s->start.mode);
        bool family = family_mode(s->start.mode);
        bool compact = response && s->compact_response_read_enabled;
        uint8_t command = compact ? CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_RESPONSE_COMPACT :
            response ? CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_RESPONSE :
            family ? CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_FAMILY :
            s->metadata_read_enabled ? CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ_METADATA :
            CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_READ;
        esp_err_t error = exchange(s, command,
            &request, sizeof(request), &result, sizeof(result));
        if (error == ESP_ERR_NOT_SUPPORTED && (compact || (!response && !family && s->metadata_read_enabled && s->start.mode != CERBERUS_DUAL_BLE_REACQUIRE))) {
            /* Optional projection only. Discard every partially copied row,
             * including completed metadata rows, then reread this exact frozen
             * handle through the full projection. FIRST BEGIN's age anchor
             * must not move, and response acquisition remains explicit. */
            if (compact) s->compact_response_read_enabled = false;
            else s->metadata_read_enabled = false;
            memset(s->staging, 0, sizeof(s->staging));
            memset(&s->snapshot_response, 0, sizeof(s->snapshot_response));
            s->read_index = 0U; s->copied = 0U;
            return ESP_OK;
        }
        if (error != ESP_OK) return error;
        if (result.handle != request.handle || result.index != request.index || result.chunk != request.chunk ||
            result.error > CERBERUS_DUAL_BLE_SNAPSHOT_INVALID) return ESP_ERR_INVALID_RESPONSE;
        if (result.error) {
            if (result.byte_count || !zero_bytes(result.data, sizeof(result.data))) return ESP_ERR_INVALID_RESPONSE;
            return result.error == CERBERUS_DUAL_BLE_SNAPSHOT_BUSY ? ESP_ERR_TIMEOUT :
                result.error == CERBERUS_DUAL_BLE_SNAPSHOT_STALE ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_RESPONSE;
        }
        uint16_t wire_size = response ? sizeof(s->snapshot_response) : family ? sizeof(s->snapshot_family) :
            s->metadata_read_enabled ? sizeof(s->snapshot_metadata) : sizeof(s->snapshot_row);
        if (compact) {
            /* The first 160 bytes always arrive in 16 full chunks. Only then
             * is the response projection header available to bound the rest. */
            wire_size = s->copied < CERBERUS_DUAL_BLE_COMPACT_RESPONSE_UNKNOWN_BYTES ?
                CERBERUS_DUAL_BLE_COMPACT_RESPONSE_UNKNOWN_BYTES : cerberus_dual_ble_compact_response_wire_size(
                s->snapshot_response.response.version, s->snapshot_response.response.state);
            if (!wire_size) return ESP_ERR_INVALID_RESPONSE;
        }
        if (s->copied >= wire_size) return ESP_ERR_INVALID_RESPONSE;
        uint16_t amount = wire_size - s->copied;
        if (amount > CERBERUS_DUAL_BLE_SNAPSHOT_CHUNK_BYTES) amount = CERBERUS_DUAL_BLE_SNAPSHOT_CHUNK_BYTES;
        if (result.byte_count != amount || !zero_bytes(result.data + amount, sizeof(result.data) - amount))
            return ESP_ERR_INVALID_RESPONSE;
        memcpy((uint8_t *)&s->snapshot_response + s->copied, result.data, amount); s->copied += amount;
        if (compact && s->copied == CERBERUS_DUAL_BLE_COMPACT_RESPONSE_UNKNOWN_BYTES) {
            wire_size = cerberus_dual_ble_compact_response_wire_size(
                s->snapshot_response.response.version, s->snapshot_response.response.state);
            if (!wire_size) return ESP_ERR_INVALID_RESPONSE;
        }
        if (s->copied == wire_size) {
            /* Compact UNKNOWN's format header is not response evidence.
             * NONE already has the full canonical zero suffix from row reset. */
            if (compact && s->snapshot_response.response.state == CERBERUS_DUAL_BLE_RESPONSE_UNKNOWN)
                memset(&s->snapshot_response.response, 0, sizeof(s->snapshot_response.response));
            error = decode_snapshot_row(s);
            if (error != ESP_OK) return error;
            s->copied = 0U; ++s->read_index;
            memset(&s->snapshot_response, 0, sizeof(s->snapshot_response));
            if (s->read_index == s->snapshot.count)
                s->phase = s->inventory_query_enabled ? SNAP_INVENTORY : SNAP_FENCE;
        }
    } else if (s->phase == SNAP_INVENTORY) {
        cerberus_dual_ble_snapshot_binding_t request = {b, s->snapshot.handle};
        cerberus_dual_ble_snapshot_inventory_t result;
        esp_err_t error = exchange(s, CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_INVENTORY,
            &request, sizeof(request), &result, sizeof(result));
        if (error == ESP_OK) {
            if (result.error_code) error = result.error_code;
            else if (result.handle != s->snapshot.handle || result.count != s->snapshot.count ||
                (result.flags & ~CERBERUS_DUAL_BLE_INVENTORY_REPLACEMENT) ||
                (!(result.flags & CERBERUS_DUAL_BLE_INVENTORY_REPLACEMENT) && result.replaced))
                error = ESP_ERR_INVALID_RESPONSE;
            else { s->pending_inventory = result; s->pending_inventory_known = true; }
        }
        if (error == ESP_ERR_TIMEOUT && s->inventory_retries++ < 2U) {
            s->next_poll_ms = now + 100U;
            return ESP_OK;
        }
        /* Optional diagnostics never make a validated bank unreadable. A
         * missing/invalid response remains unknown, never a zero-count claim.
         * The final STATUS fence is mandatory even after diagnostics failure. */
        if (error == ESP_ERR_NOT_SUPPORTED) s->inventory_query_enabled = false;
        s->phase = SNAP_FENCE; s->next_poll_ms = 0U;
    } else {
        cerberus_dual_ble_snapshot_binding_t request = {b, s->snapshot.handle};
        cerberus_dual_ble_snapshot_status_t result;
        esp_err_t error = exchange(s, CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_STATUS,
            &request, sizeof(request), &result, sizeof(result));
        if (error != ESP_OK) return error;
        if (result.boot_id != s->start.boot_id) return ESP_ERR_INVALID_RESPONSE;
        if (result.error_code) return result.error_code;
        if (!snapshot_valid(s, &result) || result.handle != s->snapshot.handle ||
            result.count != s->snapshot.count || result.flags != s->snapshot.flags) return ESP_ERR_INVALID_RESPONSE;
        uint64_t published = publish_bank(s, result.count, now);
        if (result.flags & CERBERUS_DUAL_BLE_SNAPSHOT_COMPLETE) {
            if (s->sweeps != UINT32_MAX) ++s->sweeps;
            s->phase = RELEASE;
        } else {
            s->next_snapshot_ms = published + 500U; s->phase = POLL; s->next_poll_ms = 0U;
        }
    }
    return ESP_OK;
}

static void step(cerberus_dual_ble_scan_t *s, uint64_t now)
{
    esp_err_t error = ESP_OK;
    cerberus_dual_ble_binding_t b = binding(s);
    cerberus_dual_ble_status_t status = {0};
    uint8_t phase = s->phase;
    if (phase == OFF || phase == FAILED || now < s->next_poll_ms) return;
    if (phase == INFO) {
        cerberus_dual_ble_info_t info;
        error = exchange(s, CERBERUS_SPI_COMMAND_DUAL_BLE_INFO, NULL, 0U, &info, sizeof(info));
        if (error == ESP_OK) {
            bool reserved = false;
            for (size_t i = 0; i < sizeof(info.reserved); ++i) reserved |= info.reserved[i] != 0;
            if (!info.boot_id || (info.version != CERBERUS_DUAL_BLE_VERSION &&
                    info.version != CERBERUS_DUAL_BLE_LEGACY_VERSION) || reserved ||
                info.capacity != CERBERUS_DUAL_BLE_CAPACITY || info.raw_bytes != CERBERUS_DUAL_BLE_RAW_BYTES ||
                (info.flags & ~(CERBERUS_DUAL_BLE_READY | CERBERUS_DUAL_BLE_EXTENDED |
                    CERBERUS_DUAL_BLE_ATTACHED | CERBERUS_DUAL_BLE_SNAPSHOTS)) ||
                (info.version == CERBERUS_DUAL_BLE_LEGACY_VERSION && (info.flags & CERBERUS_DUAL_BLE_SNAPSHOTS)))
                error = ESP_ERR_INVALID_RESPONSE;
            else if ((s->start.mode == CERBERUS_DUAL_BLE_ANALYZER || family_mode(s->start.mode) ||
                response_mode(s->start.mode) || s->start.mode == CERBERUS_DUAL_BLE_REACQUIRE) &&
                info.version != CERBERUS_DUAL_BLE_VERSION)
                error = ESP_ERR_NOT_SUPPORTED;
            else if ((family_mode(s->start.mode) || response_mode(s->start.mode) || s->start.mode == CERBERUS_DUAL_BLE_REACQUIRE) &&
                !(info.flags & CERBERUS_DUAL_BLE_SNAPSHOTS))
                error = ESP_ERR_NOT_SUPPORTED;
            else if (!(info.flags & CERBERUS_DUAL_BLE_READY) || (info.flags & CERBERUS_DUAL_BLE_ATTACHED))
                error = ESP_ERR_INVALID_STATE;
            else if (!info.next_job_id) error = ESP_ERR_INVALID_STATE;
            else {
                s->start.boot_id = info.boot_id; s->start.job_id = info.next_job_id;
                s->start.version = info.version;
                s->job_counter = info.next_job_id;
                s->extended = (info.flags & CERBERUS_DUAL_BLE_EXTENDED) != 0;
                s->snapshots_capable = (info.flags & CERBERUS_DUAL_BLE_SNAPSHOTS) != 0U;
                s->live_updates = cerberus_dual_ble_snapshot_mode(s->start.mode) && s->snapshots_capable;
                s->snapshot_request_id = 0U; s->snapshot_anchor_valid = false;
                memset(&s->snapshot, 0, sizeof(s->snapshot));
                s->phase = START;
            }
        }
    } else if (phase == START) {
        error = exchange(s, CERBERUS_SPI_COMMAND_DUAL_BLE_START, &s->start, sizeof(s->start), &status, sizeof(status));
        /* The transport gates Analyzer/Family/Response START before any RF command on
         * older peers. NOT_SUPPORTED establishes that no owner was acquired. */
        if (error == ESP_ERR_NOT_SUPPORTED) s->start.job_id = 0U;
        if (error == ESP_OK) {
            if (!status_valid(s, &status)) error = ESP_ERR_INVALID_RESPONSE;
            else if (!(status.flags & CERBERUS_DUAL_BLE_ATTACHED)) {
                s->start.job_id = 0U; error = status.error_code ? status.error_code : ESP_ERR_INVALID_RESPONSE;
            } else {
                s->remote = status; s->phase = POLL;
                s->next_snapshot_ms = now + 500U;
            }
        }
    } else if (phase == POLL || phase == FENCE || phase == STOP || phase == RELEASE || phase == ANALYZER_FENCE) {
        uint8_t cmd = phase == STOP ? CERBERUS_SPI_COMMAND_DUAL_BLE_STOP :
            phase == RELEASE ? CERBERUS_SPI_COMMAND_DUAL_BLE_RELEASE : CERBERUS_SPI_COMMAND_DUAL_BLE_STATUS;
        error = exchange(s, cmd, &b, sizeof(b), &status, sizeof(status));
        if (error == ESP_OK) {
            if (!status_valid(s, &status)) error = ESP_ERR_INVALID_RESPONSE;
            else if (status.error_code == ESP_ERR_NOT_FOUND && !(status.flags & CERBERUS_DUAL_BLE_ATTACHED)) {
                if (phase != STOP && phase != RELEASE) { s->error = ESP_ERR_INVALID_STATE; s->enabled = false; }
                no_owner(s); return;
            } else if (phase == RELEASE && !(status.flags & CERBERUS_DUAL_BLE_ATTACHED) &&
                status.epoch == s->remote.epoch && !status.error_code) { no_owner(s); return; }
            else {
                /* A START may reach C5 even when all its replies are lost.
                 * The bound STOP reply may establish cleanup identity only;
                 * it must never publish that unacknowledged result bank. */
                if (phase == STOP && !s->remote.epoch &&
                    (status.flags & CERBERUS_DUAL_BLE_ATTACHED)) s->remote = status;
                if (!same_bank(s, &status)) error = ESP_ERR_INVALID_RESPONSE;
                else if (phase == STOP) {
                s->remote = status;
                s->phase = status.flags & CERBERUS_DUAL_BLE_OWNED ? STOP : RELEASE;
                s->next_poll_ms = status.flags & CERBERUS_DUAL_BLE_OWNED ? now + 250U : 0U;
            } else if (status.error_code || status.state == CERBERUS_BLE_SCAN_ERROR) {
                error = status.error_code ? status.error_code : ESP_FAIL;
            } else if (phase == ANALYZER_FENCE) {
                if (status.state != CERBERUS_BLE_SCAN_COMPLETE || (status.flags & CERBERUS_DUAL_BLE_OWNED) ||
                    status.count || !s->analyzer.final) error = ESP_ERR_INVALID_RESPONSE;
                else {
                    s->remote = status;
                    if (s->sweeps != UINT32_MAX) ++s->sweeps;
                    s->phase = RELEASE;
                }
            } else if (phase == FENCE) {
                if (status.state != CERBERUS_BLE_SCAN_COMPLETE || (status.flags & CERBERUS_DUAL_BLE_OWNED) ||
                    status.count != s->remote.count) error = ESP_ERR_INVALID_RESPONSE;
                else {
                    publish_bank(s, status.count, now);
                    if (s->sweeps != UINT32_MAX) ++s->sweeps;
                    s->phase = RELEASE;
                }
            } else if (status.state != CERBERUS_BLE_SCAN_RUNNING &&
                status.state != CERBERUS_BLE_SCAN_COMPLETE) error = ESP_ERR_INVALID_RESPONSE;
            else {
                s->remote = status;
                if (s->start.mode == CERBERUS_DUAL_BLE_ANALYZER) {
                    if (status.count) error = ESP_ERR_INVALID_RESPONSE;
                    else s->phase = ANALYZER;
                }
                else if (s->start.mode == CERBERUS_DUAL_BLE_TRACK) s->phase = TRACK;
                else if (s->live_updates && (now >= s->next_snapshot_ms ||
                    (status.state == CERBERUS_BLE_SCAN_COMPLETE && !(status.flags & CERBERUS_DUAL_BLE_OWNED)))) {
                    if (s->snapshot_request_id == UINT64_MAX) error = ESP_ERR_INVALID_STATE;
                    else {
                        ++s->snapshot_request_id; s->snapshot_anchor_valid = false;
                        s->phase = SNAP_BEGIN;
                    }
                }
                else if (s->live_updates) s->next_poll_ms = now + 250U;
                else if (status.state == CERBERUS_BLE_SCAN_COMPLETE && !(status.flags & CERBERUS_DUAL_BLE_OWNED)) {
                    memset(s->staging, 0, sizeof(s->staging)); s->read_index = 0U;
                    s->phase = status.count ? META : FENCE;
                } else { s->next_poll_ms = now + 250U; }
            }
            }
        }
    } else if (phase == SNAP_BEGIN || phase == SNAP_READ || phase == SNAP_FENCE || phase == SNAP_INVENTORY) {
        error = snapshot_step(s, now);
    } else if (phase == ANALYZER) {
        cerberus_dual_ble_analyzer_sample_t sample;
        error = exchange(s, CERBERUS_SPI_COMMAND_DUAL_BLE_ANALYZER_SAMPLE,
            &b, sizeof(b), &sample, sizeof(sample));
        if (error == ESP_OK) {
            const uint8_t allowed = CERBERUS_DUAL_BLE_ANALYZER_VALID | CERBERUS_DUAL_BLE_ANALYZER_BUSY |
                CERBERUS_DUAL_BLE_ANALYZER_SATURATED | CERBERUS_DUAL_BLE_ANALYZER_FINAL |
                CERBERUS_DUAL_BLE_ANALYZER_RSSI_VALID | CERBERUS_DUAL_BLE_ANALYZER_CLOCK_INVALID;
            bool valid = (sample.flags & CERBERUS_DUAL_BLE_ANALYZER_VALID) != 0U;
            bool rssi = (sample.flags & CERBERUS_DUAL_BLE_ANALYZER_RSSI_VALID) != 0U;
            bool saturated = (sample.flags & CERBERUS_DUAL_BLE_ANALYZER_SATURATED) != 0U;
            if (sample.job_id != s->start.job_id || (sample.flags & ~allowed) ||
                (valid && (sample.flags & (CERBERUS_DUAL_BLE_ANALYZER_BUSY | CERBERUS_DUAL_BLE_ANALYZER_CLOCK_INVALID))) ||
                (!valid && (sample.reports || sample.elapsed_ms || sample.rssi != 127)) ||
                (valid && ((rssi && (!sample.reports || sample.rssi < -127 || sample.rssi > 20)) ||
                    (!rssi && sample.rssi != 127) || (saturated != (sample.reports == UINT32_MAX)))))
                error = ESP_ERR_INVALID_RESPONSE;
            else if (sample.flags == CERBERUS_DUAL_BLE_ANALYZER_BUSY && !sample.epoch) error = ESP_ERR_TIMEOUT;
            else if (sample.epoch != s->remote.epoch) error = ESP_ERR_INVALID_RESPONSE;
            else if (!valid) {
                if (sample.flags || s->remote.state == CERBERUS_BLE_SCAN_COMPLETE) error = ESP_ERR_INVALID_RESPONSE;
                else {
                    cerberus_dual_ble_analyzer_gap(&s->analyzer);
                    s->phase = POLL; s->next_poll_ms = now + 500U;
                }
            } else if (!cerberus_dual_ble_analyzer_accept(&s->analyzer, sample.epoch, sample.job_id,
                sample.reports, sample.elapsed_ms, saturated,
                (sample.flags & CERBERUS_DUAL_BLE_ANALYZER_FINAL) != 0U)) error = ESP_ERR_INVALID_RESPONSE;
            else if (sample.flags & CERBERUS_DUAL_BLE_ANALYZER_FINAL) s->phase = ANALYZER_FENCE;
            else if (s->remote.state == CERBERUS_BLE_SCAN_COMPLETE) error = ESP_ERR_INVALID_RESPONSE;
            else { s->phase = POLL; s->next_poll_ms = now + 500U; }
        }
    } else if (phase == TRACK) {
        cerberus_dual_ble_track_t track;
        error = exchange(s, CERBERUS_SPI_COMMAND_DUAL_BLE_TRACK_STATUS, &b, sizeof(b), &track, sizeof(track));
        if (error == ESP_OK) {
            if (track.job_id != s->start.job_id || track.epoch != s->remote.epoch ||
                (track.flags & ~(CERBERUS_DUAL_BLE_SEEN | CERBERUS_DUAL_BLE_RSSI_VALID)) ||
                ((track.flags & CERBERUS_DUAL_BLE_RSSI_VALID) && (track.rssi < -127 || track.rssi > 20)))
                error = ESP_ERR_INVALID_RESPONSE;
            else {
                if (track.flags & CERBERUS_DUAL_BLE_SEEN) { s->track = track; s->track_request_ms = now; s->has_track = true; }
                if (s->remote.state == CERBERUS_BLE_SCAN_COMPLETE && !(s->remote.flags & CERBERUS_DUAL_BLE_OWNED)) {
                    if (s->sweeps != UINT32_MAX) ++s->sweeps;
                    s->phase = RELEASE;
                } else { s->phase = POLL; s->next_poll_ms = now + 250U; }
            }
        }
    } else if (phase == META) {
        cerberus_ble_scan_result_request_t request = {s->remote.generation, s->read_index};
        error = exchange(s, CERBERUS_SPI_COMMAND_BLE_SCAN_RESULT, &request, sizeof(request), &s->metadata, sizeof(s->metadata));
        if (error == ESP_OK) {
            const cerberus_ble_scan_result_payload_t *m = &s->metadata;
            if (m->generation != s->remote.generation || m->index != s->read_index || m->address_type > 3U ||
                m->name_length > CERBERUS_BLE_NAME_MAX_LENGTH || (m->uuid_length && m->uuid_length != 2U && m->uuid_length != 4U && m->uuid_length != 16U) ||
                m->advertisement_length > CERBERUS_DUAL_BLE_RAW_BYTES || m->advertisement_event_type > 4U ||
                (m->advertisement_flags & ~(CERBERUS_BLE_ADVERTISEMENT_TRUNCATED | CERBERUS_BLE_ADVERTISEMENT_MALFORMED)) || m->reserved[0] || m->reserved[1])
                error = ESP_ERR_INVALID_RESPONSE;
            else {
                cerberus_dual_ble_row_t *row = &s->staging[s->read_index];
                memcpy(row->address, m->address, 6U); row->address_type = m->address_type;
                row->flags = m->flags; row->manufacturer_id = m->manufacturer_id; row->reports = m->reports_seen;
                row->uuid_length = m->uuid_length; row->raw_length = m->advertisement_length;
                row->raw_flags = m->advertisement_flags; row->event_type = m->advertisement_event_type;
                s->copied = 0U; s->phase = NAME;
            }
        }
    } else if (phase == NAME || phase == UUID || phase == RAW) {
        cerberus_dual_ble_row_t *row = &s->staging[s->read_index];
        uint8_t length = phase == NAME ? s->metadata.name_length : phase == UUID ? row->uuid_length : row->raw_length;
        if (s->copied == length) {
            if (phase == NAME) for (uint8_t i = 0U; i < length; ++i)
                if ((unsigned char)row->name[i] < 32U || (unsigned char)row->name[i] > 126U) row->name[i] = '?';
            s->copied = 0U; ++s->phase; return;
        }
        uint8_t kind = phase == NAME ? CERBERUS_BLE_DATA_NAME : phase == UUID ? CERBERUS_BLE_DATA_SERVICE_UUID : CERBERUS_BLE_DATA_RAW_ADVERTISEMENT;
        cerberus_ble_scan_data_request_t request = {s->remote.generation, s->read_index, kind, s->copied / CERBERUS_BLE_DATA_CHUNK_SIZE};
        cerberus_ble_scan_data_payload_t data;
        error = exchange(s, CERBERUS_SPI_COMMAND_BLE_SCAN_DATA, &request, sizeof(request), &data, sizeof(data));
        if (error == ESP_OK) {
            uint8_t amount = length - s->copied;
            if (amount > CERBERUS_BLE_DATA_CHUNK_SIZE) amount = CERBERUS_BLE_DATA_CHUNK_SIZE;
            if (data.generation != request.generation || data.index != request.index || data.kind != kind ||
                data.chunk != request.chunk || data.byte_count != amount) error = ESP_ERR_INVALID_RESPONSE;
            else {
                uint8_t *destination = phase == NAME ? (uint8_t *)row->name : phase == UUID ? row->uuid : row->raw;
                memcpy(destination + s->copied, data.bytes, amount); s->copied += amount;
            }
        }
    } else if (phase == TIME) {
        cerberus_dual_ble_row_request_t request = {b, s->read_index, 0U};
        cerberus_dual_ble_row_time_t stamp;
        error = exchange(s, CERBERUS_SPI_COMMAND_DUAL_BLE_ROW_TIME, &request, sizeof(request), &stamp, sizeof(stamp));
        if (error == ESP_OK) {
            if (stamp.epoch != s->remote.epoch || stamp.job_id != s->start.job_id || stamp.index != s->read_index ||
                stamp.generation != s->remote.generation || stamp.reports != s->metadata.reports_seen ||
                (stamp.flags & ~(CERBERUS_DUAL_BLE_SEEN | CERBERUS_DUAL_BLE_RSSI_VALID)) ||
                ((stamp.flags & CERBERUS_DUAL_BLE_RSSI_VALID) && (stamp.latest_rssi < -127 || stamp.latest_rssi > 20)))
                error = ESP_ERR_INVALID_RESPONSE;
            else {
                cerberus_dual_ble_row_t *row = &s->staging[s->read_index];
                row->rssi = stamp.latest_rssi; row->timing_flags = stamp.flags;
                row->callback_age_ms = stamp.age_ms; row->request_started_ms = now;
                ++s->read_index; s->phase = s->read_index < s->remote.count ? META : FENCE;
            }
        }
    }
    if (error != ESP_OK) {
        if (s->start.mode == CERBERUS_DUAL_BLE_ANALYZER) cerberus_dual_ble_analyzer_gap(&s->analyzer);
        if (phase == STOP || phase == RELEASE) { s->error = error; s->phase = STOP; s->next_poll_ms = now + 250U; }
        else if (error == ESP_ERR_TIMEOUT && s->retries++ < 2U) {
            s->next_poll_ms = now + (s->start.mode == CERBERUS_DUAL_BLE_ANALYZER ? 500U : 100U);
        }
        else fail(s, error);
    } else s->retries = 0U;
}

void cerberus_dual_ble_tick(cerberus_dual_ble_scan_t *s, uint64_t now)
{
    if (!s || !s->ops.exchange) return;
    if (now < s->last_tick_ms) { fail(s, ESP_ERR_INVALID_STATE); return; }
    s->last_tick_ms = now;
    uint64_t started = now;
    for (unsigned i = 0; i < 4U; ++i) {
        if (s->ops.now_ms) now = s->ops.now_ms(s->ops.context);
        if (now < started || now - started >= 8U) break;
        uint8_t before = s->phase, index = s->read_index;
        uint16_t copied = s->copied;
        step(s, now);
        if ((s->phase == before && s->copied == copied && s->read_index == index) ||
            s->phase == OFF || s->phase == FAILED || now < s->next_poll_ms) break;
    }
}

void cerberus_dual_ble_view(const cerberus_dual_ble_scan_t *s, cerberus_dual_ble_view_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out)); if (!s) return;
    out->enabled = s->enabled; out->attached = s->start.job_id != 0U;
    out->receiving = (s->remote.flags & CERBERUS_DUAL_BLE_OWNED) != 0;
    out->reading = (s->phase >= META && s->phase <= FENCE) ||
        (s->phase >= SNAP_BEGIN && s->phase <= SNAP_FENCE) || s->phase == SNAP_INVENTORY;
    out->stopping = s->phase == STOP || s->phase == RELEASE;
    out->extended = s->extended; out->count = s->count; out->sweeps = s->sweeps; out->error = s->error;
    out->requested_family = family_mode(s->start.mode) ? s->start.reserved : 0U;
    out->published_family = s->published_family;
    out->requested_response = response_mode(s->start.mode);
    out->published_response = s->published_response;
    out->snapshots = s->snapshots; out->first_result_ms = s->first_result_ms;
    out->has_first_result = s->has_first_result; out->live_updates = s->live_updates;
    out->inventory_known = s->inventory_known;
    out->inventory_replacement_enabled = s->inventory_replacement_enabled;
    out->inventory_replaced = s->inventory_replaced; out->inventory_dropped = s->inventory_dropped;
}

bool cerberus_dual_ble_row(const cerberus_dual_ble_scan_t *s, uint8_t index, cerberus_dual_ble_row_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out)); if (!s || index >= s->count) return false;
    *out = s->rows[index]; return true;
}

bool cerberus_dual_ble_copy_row(const cerberus_dual_ble_scan_t *s, uint8_t index,
    cerberus_dual_ble_row_t *out, cerberus_dual_ble_provenance_t *provenance)
{
    if (out) memset(out, 0, sizeof(*out));
    if (provenance) memset(provenance, 0, sizeof(*provenance));
    if (!out || !provenance || !s || index >= s->count ||
        s->count > CERBERUS_DUAL_BLE_CAPACITY || !s->published_boot_id ||
        !s->published_epoch) return false;
    *out = s->rows[index];
    *provenance = (cerberus_dual_ble_provenance_t){
        .boot_id = s->published_boot_id, .epoch = s->published_epoch,
        .generation = s->published_generation, .index = index,
    };
    return true;
}

void cerberus_dual_ble_track_view(const cerberus_dual_ble_scan_t *s, uint64_t now,
    cerberus_dual_ble_track_view_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out)); if (!s || !s->has_track) return;
    out->seen = (s->track.flags & (CERBERUS_DUAL_BLE_SEEN | CERBERUS_DUAL_BLE_RSSI_VALID)) ==
        (CERBERUS_DUAL_BLE_SEEN | CERBERUS_DUAL_BLE_RSSI_VALID);
    out->rssi = s->track.rssi; out->reports = s->track.reports;
    out->age_upper_ms = cerberus_dual_ble_age_upper(s->track.age_ms, s->track_request_ms, now);
    out->fresh = out->seen && out->age_upper_ms <= CERBERUS_DUAL_BLE_STALE_MS;
}

void cerberus_dual_ble_analyzer_view(const cerberus_dual_ble_scan_t *s,
    cerberus_dual_ble_analyzer_view_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!s || s->start.mode != CERBERUS_DUAL_BLE_ANALYZER) return;
    out->enabled = s->enabled; out->attached = s->start.job_id != 0U;
    out->receiving = (s->remote.flags & CERBERUS_DUAL_BLE_OWNED) != 0U;
    out->stopping = s->phase == STOP || s->phase == RELEASE;
    out->extended = s->extended; out->error = s->error; out->sweeps = s->sweeps;
    const cerberus_dual_ble_analyzer_history_t *h = &s->analyzer;
    out->epoch = h->epoch; out->samples = h->samples; out->reports = h->reports;
    out->elapsed_ms = h->elapsed_ms; out->sample_valid = h->sample_valid;
    out->saturated = h->saturated; out->final = h->final; out->history_count = h->count;
    for (uint8_t i = 0U; i < h->count; ++i)
        out->history[i] = h->history[(h->head + CERBERUS_DUAL_BLE_ANALYZER_HISTORY - h->count + i) %
            CERBERUS_DUAL_BLE_ANALYZER_HISTORY];
}


bool cerberus_dual_ble_reacquire_row(const cerberus_dual_ble_scan_t *s, uint8_t index,
    const uint8_t address[6], cerberus_dual_ble_row_t *out, cerberus_dual_ble_provenance_t *provenance)
{
    if (out) memset(out,0,sizeof(*out));
    if (provenance) memset(provenance,0,sizeof(*provenance));
    if (!s || !address || !out || !provenance || s->start.mode != CERBERUS_DUAL_BLE_REACQUIRE ||
        !s->snapshots || !s->published_reacquire || memcmp(address,s->published_address,6U) ||
        memcmp(address,s->start.address,6U)) return false;
    return cerberus_dual_ble_copy_row(s,index,out,provenance);
}
