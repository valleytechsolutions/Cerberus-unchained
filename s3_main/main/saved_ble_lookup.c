#include "saved_ble_lookup.h"
#include <string.h>

static cerberus_ble_discovery_operation_t operation(esp_err_t error)
{
    return (cerberus_ble_discovery_operation_t){error, error == ESP_ERR_NOT_FOUND};
}

static bool observation(const cerberus_saved_ble_lookup_t *s,
    const cerberus_s3_ble_discovery_meta_t *meta, cerberus_ble_discovery_observation_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!meta->attached || !meta->scan_epoch || !meta->reacquiring ||
        meta->family != CERBERUS_S3_BLE_FILTER_ALL || memcmp(meta->target_address, s->address, 6U))
        return false;
    *out = (cerberus_ble_discovery_observation_t){
        .scan_epoch = meta->scan_epoch, .revision = meta->revision,
        .error = meta->status.error_code, .state = meta->status.state,
        .family = 0U, .owned = meta->owned, .stopping = meta->stopping};
    return true;
}

static cerberus_ble_discovery_operation_t start_s3(void *context, uint8_t family,
    uint64_t previous, cerberus_ble_discovery_observation_t *out)
{
    (void)family;
    cerberus_saved_ble_lookup_t *s = context;
    cerberus_s3_ble_discovery_meta_t meta = {0};
    esp_err_t error = cerberus_s3_ble_scanner_start_reacquire(s->address, previous, &meta);
    if (!observation(s, &meta, out) && error == ESP_OK) error = ESP_ERR_INVALID_RESPONSE;
    return operation(error);
}

static cerberus_ble_discovery_operation_t poll_s3(void *context, uint64_t epoch,
    cerberus_ble_discovery_observation_t *out)
{
    cerberus_s3_ble_discovery_meta_t meta = {0};
    esp_err_t error = cerberus_s3_ble_scanner_discovery_status(epoch, &meta);
    if (!observation(context, &meta, out) && error == ESP_OK) error = ESP_ERR_INVALID_RESPONSE;
    return operation(error);
}

static cerberus_ble_discovery_operation_t stop_s3(void *context, uint64_t epoch,
    cerberus_ble_discovery_observation_t *out)
{
    cerberus_s3_ble_discovery_meta_t meta = {0};
    esp_err_t error = cerberus_s3_ble_scanner_stop_discovery(epoch, &meta);
    if (!observation(context, &meta, out) && error == ESP_OK) error = ESP_ERR_INVALID_RESPONSE;
    return operation(error);
}

static cerberus_ble_discovery_operation_t release_s3(void *context, uint64_t epoch)
{
    (void)context;
    return operation(cerberus_s3_ble_scanner_release_discovery(epoch));
}

static bool original_time(const cerberus_s3_ble_discovery_meta_t *meta,
    const cerberus_s3_ble_discovery_record_t *row, uint64_t *out)
{
    const cerberus_s3_ble_discovery_evidence_t *e = &row->focus.discovery;
    const uint8_t needed = CERBERUS_S3_BLE_EVIDENCE_ADV_VALID | CERBERUS_S3_BLE_EVIDENCE_ADV_COMPLETE;
    if (meta->source_now_ms < meta->started_ms ||
        meta->source_now_ms - meta->started_ms > UINT32_MAX ||
        (e->flags & needed) != needed || !e->advertisement_sequence ||
        e->raw_advertisement_sequence != e->advertisement_sequence ||
        e->advertisement_observed_ms != row->observed_ms) return false;
    uint32_t age = (uint32_t)meta->source_now_ms - row->observed_ms;
    if (age > meta->source_now_ms || meta->source_now_ms - age < meta->started_ms) return false;
    *out = meta->source_now_ms - age;
    return true;
}

static cerberus_ble_discovery_operation_t copy_s3(void *context, uint64_t epoch,
    uint8_t family, cerberus_ble_discovery_observation_t *out)
{
    (void)family;
    cerberus_saved_ble_lookup_t *s = context;
    cerberus_s3_ble_discovery_meta_t meta = {0};
    esp_err_t error = cerberus_s3_ble_scanner_copy_reacquire(epoch, s->staging, &meta);
    memset(out, 0, sizeof(*out));
    if (error != ESP_OK) return operation(error);
    if (!observation(s, &meta, out) || meta.scan_epoch != epoch || meta.status.stored_count > 4U ||
        epoch < s->s3_epoch || (epoch == s->s3_epoch && meta.revision < s->s3_revision))
        return operation(ESP_ERR_INVALID_RESPONSE);
    if (epoch == s->s3_epoch && meta.revision == s->s3_revision) return operation(ESP_OK);
    /* Validate the whole compact publication before changing a candidate. */
    uint8_t types = 0U;
    uint64_t times[4] = {0};
    bool qualifying[4] = {false};
    for (uint8_t i = 0; i < meta.status.stored_count; ++i) {
        const cerberus_s3_ble_discovery_record_t *r = &s->staging[i];
        if (r->peer.address_type > 3U || memcmp(r->peer.address, s->address, 6U) ||
            r->generation != meta.status.generation || r->index != i ||
            (types & (1U << r->peer.address_type)))
            return operation(ESP_ERR_INVALID_RESPONSE);
        types |= (uint8_t)(1U << r->peer.address_type);
        /* A valid native publication may retire a malformed/incomplete
         * latest original while retaining its older raw preview. Retire that
         * candidate instead of turning it into a transport-wide failure. */
        qualifying[i] = original_time(&meta, r, &times[i]) &&
            r->peer.rssi >= -127 && r->peer.rssi <= 20;
        const cerberus_saved_ble_lookup_candidate_t *old = &s->candidates[r->peer.address_type];
        if (qualifying[i] && (s->seen_mask & (1U << r->peer.address_type)) && times[i] < old->anchor_ms)
            return operation(ESP_ERR_INVALID_RESPONSE);
    }
    for (uint8_t i = 0; i < meta.status.stored_count; ++i) {
        const cerberus_s3_ble_discovery_record_t *r = &s->staging[i];
        cerberus_saved_ble_lookup_candidate_t *candidate = &s->candidates[r->peer.address_type];
        if (!qualifying[i]) {
            memset(candidate, 0, sizeof(*candidate));
            s->seen_mask &= (uint8_t)~(1U << r->peer.address_type);
            continue;
        }
        *candidate = (cerberus_saved_ble_lookup_candidate_t){.peer = r->peer,
            .run_id = s->run_id, .epoch = epoch, .anchor_ms = times[i], .source = 1U};
        s->seen_mask |= (uint8_t)(1U << r->peer.address_type);
    }
    s->s3_epoch = epoch;
    s->s3_revision = meta.revision;
    return operation(ESP_OK);
}

void cerberus_saved_ble_lookup_init(cerberus_saved_ble_lookup_t *s, const cerberus_dual_ble_ops_t *ops)
{
    if (!s) return;
    memset(s, 0, sizeof(*s));
    cerberus_dual_ble_init(&s->c5, ops);
    cerberus_ble_discovery_session_init(&s->s3, 0U);
}

bool cerberus_saved_ble_lookup_released(const cerberus_saved_ble_lookup_t *s)
{
    return !s || (cerberus_ble_discovery_session_released(&s->s3) &&
        cerberus_dual_ble_released(&s->c5));
}

bool cerberus_saved_ble_lookup_start(cerberus_saved_ble_lookup_t *s, const uint8_t address[6])
{
    if (!s || !address || s->enabled || !cerberus_saved_ble_lookup_released(s) ||
        s->run_id == UINT64_MAX) return false;
    memcpy(s->address, address, 6U);
    ++s->run_id;
    s->enabled = true;
    s->have_clock = false;
    s->s3_epoch = s->s3_revision = s->c5_snapshots = s->seen_mask = 0U;
    memset(s->candidates, 0, sizeof(s->candidates));
    cerberus_ble_discovery_session_init(&s->s3, 0U);
    (void)cerberus_ble_discovery_session_start(&s->s3);
    s->c5_begin_error = cerberus_dual_ble_begin_reacquire(&s->c5, address);
    return true; /* Each receiver can fail independently. */
}

void cerberus_saved_ble_lookup_stop(cerberus_saved_ble_lookup_t *s)
{
    if (!s) return;
    s->enabled = false;
    cerberus_ble_discovery_session_stop(&s->s3);
    cerberus_dual_ble_stop(&s->c5);
}

static void copy_c5(cerberus_saved_ble_lookup_t *s)
{
    if (!s->c5.snapshots || s->c5.snapshots == s->c5_snapshots) return;
    cerberus_dual_ble_row_t row;
    cerberus_dual_ble_provenance_t proof;
    for (uint8_t i = 0; i < s->c5.count; ++i) {
        if (!cerberus_dual_ble_reacquire_row(&s->c5, i, s->address, &row, &proof) ||
            row.address_type > 3U) continue;
        uint8_t slot = (uint8_t)(4U + row.address_type);
        cerberus_saved_ble_lookup_candidate_t *candidate = &s->candidates[slot];
        if ((row.timing_flags & (CERBERUS_DUAL_BLE_SEEN | CERBERUS_DUAL_BLE_RSSI_VALID)) !=
            (CERBERUS_DUAL_BLE_SEEN | CERBERUS_DUAL_BLE_RSSI_VALID) ||
            row.receive.ad_validation != CERBERUS_DUAL_BLE_AD_VALID) {
            memset(candidate, 0, sizeof(*candidate));
            s->seen_mask &= (uint8_t)~(1U << slot);
            continue;
        }
        *candidate = (cerberus_saved_ble_lookup_candidate_t){
            .run_id = s->run_id, .boot_id = proof.boot_id, .epoch = proof.epoch,
            .anchor_ms = row.request_started_ms, .age_at_anchor_ms = row.callback_age_ms, .source = 2U};
        memcpy(candidate->peer.address, row.address, 6U);
        candidate->peer.address_type = row.address_type;
        candidate->peer.rssi = row.rssi;
        memcpy(candidate->peer.name, row.name, sizeof(candidate->peer.name));
        s->seen_mask |= (uint8_t)(1U << slot);
    }
    s->c5_snapshots = s->c5.snapshots;
}

void cerberus_saved_ble_lookup_tick(cerberus_saved_ble_lookup_t *s, uint64_t now)
{
    if (!s) return;
    if (s->have_clock && now < s->last_tick_ms) {
        cerberus_saved_ble_lookup_stop(s);
        s->seen_mask = 0U;
    }
    s->have_clock = true;
    s->last_tick_ms = now;
    const cerberus_ble_discovery_ops_t ops = {
        .context = s, .start = start_s3, .poll = poll_s3, .copy = copy_s3,
        .stop = stop_s3, .release = release_s3};
    cerberus_ble_discovery_session_tick(&s->s3, now, &ops);
    cerberus_dual_ble_tick(&s->c5, now);
    if (s->enabled) copy_c5(s);
}

static void age_candidate(cerberus_saved_ble_lookup_candidate_t *out, uint64_t now)
{
    out->age_ms = UINT32_MAX;
    if (now >= out->anchor_ms && now - out->anchor_ms <= UINT32_MAX - out->age_at_anchor_ms)
        out->age_ms = (uint32_t)(now - out->anchor_ms) + out->age_at_anchor_ms;
    out->fresh = out->age_ms <= CERBERUS_SAVED_BLE_FRESH_MS;
}

bool cerberus_saved_ble_lookup_candidate(const cerberus_saved_ble_lookup_t *s, uint8_t index,
    uint64_t now, cerberus_saved_ble_lookup_candidate_t *out)
{
    if (out) memset(out, 0, sizeof(*out));
    if (!s || !out) return false;
    for (uint8_t slot = 0U; slot < CERBERUS_SAVED_BLE_CANDIDATES; ++slot) {
        if (!(s->seen_mask & (1U << slot))) continue;
        if (index--) continue;
        *out = s->candidates[slot];
        age_candidate(out, now);
        return true;
    }
    return false;
}

bool cerberus_saved_ble_lookup_accepts(const cerberus_saved_ble_lookup_t *s,
    const cerberus_saved_ble_lookup_candidate_t *candidate, uint64_t now)
{
    if (!s || !candidate || candidate->run_id != s->run_id || candidate->peer.address_type > 3U ||
        candidate->source < 1U || candidate->source > 2U || memcmp(candidate->peer.address, s->address, 6U))
        return false;
    uint8_t slot = (uint8_t)((candidate->source - 1U) * 4U + candidate->peer.address_type);
    if (!(s->seen_mask & (1U << slot)) || candidate->boot_id != s->candidates[slot].boot_id ||
        !candidate->epoch || candidate->epoch > s->candidates[slot].epoch) return false;
    cerberus_saved_ble_lookup_candidate_t copy = *candidate;
    age_candidate(&copy, now);
    return copy.fresh;
}

void cerberus_saved_ble_lookup_view(const cerberus_saved_ble_lookup_t *s, uint64_t now,
    cerberus_saved_ble_lookup_view_t *out)
{
    (void)now;
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!s) return;
    out->enabled = s->enabled;
    out->stopping = !s->enabled && !cerberus_saved_ble_lookup_released(s);
    for (uint8_t i = 0; i < CERBERUS_SAVED_BLE_CANDIDATES; ++i)
        if (s->seen_mask & (1U << i)) ++out->count;
    out->source[0] = (cerberus_saved_ble_lookup_source_t){
        .attached = !cerberus_ble_discovery_session_released(&s->s3),
        .receiving = s->s3.have_observation && s->s3.observation.owned &&
            s->s3.observation.state == CERBERUS_BLE_DISCOVERY_RUNNING && !s->s3.observation.stopping,
        .error = s->s3.error, .sweeps = s->s3.completed_sweeps};
    cerberus_dual_ble_view_t c5;
    cerberus_dual_ble_view(&s->c5, &c5);
    out->source[1] = (cerberus_saved_ble_lookup_source_t){
        .attached = c5.attached, .receiving = c5.receiving,
        .error = s->c5_begin_error ? s->c5_begin_error : c5.error, .sweeps = c5.sweeps};
}
