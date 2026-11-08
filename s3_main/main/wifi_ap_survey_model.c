#include "wifi_ap_survey_model.h"
#include <limits.h>
#include <string.h>

static int source_index(uint8_t source)
{
    return source == CERBERUS_AP_SURVEY_SOURCE_S3 ? 0 :
        source == CERBERUS_AP_SURVEY_SOURCE_C5 ? 1 : -1;
}

static bool record_valid(const cerberus_ap_survey_record_t *r)
{
    if (!r || source_index(r->source) < 0 || !r->run_id || !r->session_id ||
        !r->order || r->request_started_us > r->receipt_us) return false;
    if (r->kind == CERBERUS_AP_SURVEY_AP)
        return cerberus_ap_survey_ap_valid(&r->evidence.ap, r->copied_source_us);
    if (r->kind != CERBERUS_AP_SURVEY_RELATION || !r->evidence.client.incarnation ||
        !cerberus_wifi_relation_row_valid(&r->evidence.client.row)) return false;
    return cerberus_wifi_relation_latest_us(&r->evidence.client.row) <= r->copied_source_us;
}

bool cerberus_ap_survey_record_equal(const cerberus_ap_survey_record_t *a,
    const cerberus_ap_survey_record_t *b)
{
    if (!a || !b || a->source != b->source || a->kind != b->kind ||
        a->run_id != b->run_id || a->session_id != b->session_id) return false;
    if (a->kind == CERBERUS_AP_SURVEY_AP)
        return a->evidence.ap.incarnation == b->evidence.ap.incarnation &&
            a->evidence.ap.channel == b->evidence.ap.channel &&
            !memcmp(a->evidence.ap.bssid, b->evidence.ap.bssid, 6U);
    return a->kind == CERBERUS_AP_SURVEY_RELATION &&
        a->evidence.client.incarnation == b->evidence.client.incarnation &&
        a->evidence.client.row.channel == b->evidence.client.row.channel &&
        !memcmp(a->evidence.client.row.bssid, b->evidence.client.row.bssid, 6U) &&
        !memcmp(a->evidence.client.row.station, b->evidence.client.row.station, 6U);
}

static void copy_record(const cerberus_ap_survey_bank_t *bank, uint8_t source,
    uint8_t kind, unsigned index, cerberus_ap_survey_record_t *out)
{
    memset(out, 0, sizeof(*out));
    out->run_id = bank->run_id;
    out->session_id = bank->snapshot.session_id;
    out->request_started_us = bank->request_started_us;
    out->receipt_us = bank->receipt_us;
    out->copied_source_us = bank->snapshot.copied_source_us;
    out->revision = bank->snapshot.revision;
    out->source = source;
    out->kind = kind;
    if (kind == CERBERUS_AP_SURVEY_AP) {
        out->evidence.ap = bank->snapshot.aps[index];
        out->order = bank->ap_order[index];
    } else {
        out->evidence.client = bank->snapshot.relations[index];
        out->order = bank->client_order[index];
    }
}

static bool find_record(const cerberus_ap_survey_bank_t *bank,
    const cerberus_ap_survey_record_t *key, cerberus_ap_survey_record_t *out)
{
    if (!bank->published) return false;
    unsigned count = key->kind == CERBERUS_AP_SURVEY_AP ? bank->snapshot.ap_count :
        bank->snapshot.relation_count;
    for (unsigned i = 0; i < count; ++i) {
        cerberus_ap_survey_record_t row;
        copy_record(bank, key->source, key->kind, i, &row);
        if (cerberus_ap_survey_record_equal(key, &row)) { *out = row; return true; }
    }
    return false;
}

static void refresh_selection(cerberus_ap_survey_model_t *model)
{
    cerberus_ap_survey_selection_t *selection = &model->selected;
    if (!selection->valid) return;
    const cerberus_ap_survey_bank_t *bank = &model->banks[source_index(selection->frozen.source)];
    selection->present = find_record(bank, &selection->frozen, &selection->latest);
    selection->unavailable = bank->unavailable;
}

void cerberus_ap_survey_model_init(cerberus_ap_survey_model_t *model)
{
    if (model) memset(model, 0, sizeof(*model));
}

static bool same_revision(const cerberus_ap_survey_snapshot_t *a,
    const cerberus_ap_survey_snapshot_t *b)
{
    /* Snapshot time and accrued receive time advance without a new callback.
     * Every row's original timestamp remains unchanged in this comparison. */
    return a->session_id == b->session_id &&
        !memcmp(&a->requested_channels, &b->requested_channels,
            offsetof(cerberus_ap_survey_snapshot_t, receive_us) -
            offsetof(cerberus_ap_survey_snapshot_t, requested_channels)) &&
        !memcmp(&a->revision, &b->revision,
            sizeof(*a) - offsetof(cerberus_ap_survey_snapshot_t, revision));
}

cerberus_ap_survey_model_result_t cerberus_ap_survey_model_publish(
    cerberus_ap_survey_model_t *model, uint8_t source, uint64_t run_id,
    uint64_t request_started_us, uint64_t receipt_us,
    const cerberus_ap_survey_snapshot_t *snapshot)
{
    int index = source_index(source);
    if (!model || index < 0 || !run_id || request_started_us > receipt_us ||
        !cerberus_ap_survey_snapshot_valid(snapshot)) return CERBERUS_AP_SURVEY_MODEL_INVALID;
    cerberus_ap_survey_bank_t *bank = &model->banks[index];
    bool continuing = bank->published && bank->run_id == run_id;
    if (bank->published && (run_id < bank->run_id || receipt_us < bank->receipt_us ||
        request_started_us < bank->request_started_us)) return CERBERUS_AP_SURVEY_MODEL_STALE;
    if (continuing && snapshot->session_id != bank->snapshot.session_id)
        return CERBERUS_AP_SURVEY_MODEL_INVALID;
    if (continuing && (snapshot->revision < bank->snapshot.revision ||
        snapshot->copied_source_us < bank->snapshot.copied_source_us ||
        snapshot->receive_us < bank->snapshot.receive_us)) return CERBERUS_AP_SURVEY_MODEL_STALE;
    bool duplicate = continuing && snapshot->revision == bank->snapshot.revision;
    if (duplicate && !same_revision(snapshot, &bank->snapshot)) return CERBERUS_AP_SURVEY_MODEL_INVALID;

    uint64_t ap_order[CERBERUS_AP_SURVEY_AP_CAPACITY] = {0};
    uint64_t client_order[CERBERUS_AP_SURVEY_RELATION_CAPACITY] = {0};
    uint64_t next_order = model->next_order;
    for (unsigned kind = CERBERUS_AP_SURVEY_AP; kind <= CERBERUS_AP_SURVEY_RELATION; ++kind) {
        unsigned count = kind == CERBERUS_AP_SURVEY_AP ? snapshot->ap_count : snapshot->relation_count;
        for (unsigned i = 0; i < count; ++i) {
            uint64_t order = 0U;
            if (continuing) {
                unsigned old_count = kind == CERBERUS_AP_SURVEY_AP ? bank->snapshot.ap_count : bank->snapshot.relation_count;
                for (unsigned j = 0; j < old_count; ++j) {
                    bool same = kind == CERBERUS_AP_SURVEY_AP ?
                        snapshot->aps[i].incarnation == bank->snapshot.aps[j].incarnation :
                        snapshot->relations[i].incarnation == bank->snapshot.relations[j].incarnation;
                    if (same) {
                        if (kind == CERBERUS_AP_SURVEY_AP) {
                            const cerberus_ap_survey_ap_t *a = &snapshot->aps[i], *b = &bank->snapshot.aps[j];
                            if (a->channel != b->channel || memcmp(a->bssid, b->bssid, 6U) ||
                                a->first_callback_us != b->first_callback_us || a->last_callback_us < b->last_callback_us ||
                                a->frames < b->frames) return CERBERUS_AP_SURVEY_MODEL_INVALID;
                            order = bank->ap_order[j];
                        } else {
                            const cerberus_wifi_relation_row_t *a = &snapshot->relations[i].row, *b = &bank->snapshot.relations[j].row;
                            if (a->channel != b->channel || memcmp(a->bssid, b->bssid, 6U) ||
                                memcmp(a->station, b->station, 6U) || (a->directions & b->directions) != b->directions ||
                                a->to_ap_frames < b->to_ap_frames || a->from_ap_frames < b->from_ap_frames ||
                                a->to_ap_callback_us < b->to_ap_callback_us || a->from_ap_callback_us < b->from_ap_callback_us)
                                return CERBERUS_AP_SURVEY_MODEL_INVALID;
                            order = bank->client_order[j];
                        }
                        break;
                    }
                }
            }
            if (!order) {
                if (next_order == UINT64_MAX) return CERBERUS_AP_SURVEY_MODEL_INVALID;
                order = ++next_order;
            }
            if (kind == CERBERUS_AP_SURVEY_AP) ap_order[i] = order;
            else client_order[i] = order;
        }
    }
    bank->snapshot = *snapshot;
    bank->run_id = run_id;
    bank->request_started_us = request_started_us;
    bank->receipt_us = receipt_us;
    memcpy(bank->ap_order, ap_order, sizeof(ap_order));
    memcpy(bank->client_order, client_order, sizeof(client_order));
    bank->published = true;
    bank->unavailable = false;
    model->next_order = next_order;
    refresh_selection(model);
    return duplicate ? CERBERUS_AP_SURVEY_MODEL_DUPLICATE : CERBERUS_AP_SURVEY_MODEL_APPLIED;
}

void cerberus_ap_survey_model_unavailable(cerberus_ap_survey_model_t *model,
    uint8_t source, bool unavailable)
{
    int index = source_index(source);
    if (!model || index < 0) return;
    model->banks[index].unavailable = unavailable;
    refresh_selection(model);
}

static bool filter_matches(const cerberus_ap_survey_record_t *row,
    const cerberus_ap_survey_record_t *filter)
{
    return !filter || (row->source == filter->source && row->run_id == filter->run_id &&
        row->session_id == filter->session_id && row->kind == CERBERUS_AP_SURVEY_RELATION &&
        row->evidence.client.row.channel == filter->evidence.ap.channel &&
        !memcmp(row->evidence.client.row.bssid, filter->evidence.ap.bssid, 6U));
}

bool cerberus_ap_survey_model_page(const cerberus_ap_survey_model_t *model, uint8_t kind,
    const cerberus_ap_survey_record_t *filter, size_t offset, cerberus_ap_survey_page_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!model || (kind != CERBERUS_AP_SURVEY_AP && kind != CERBERUS_AP_SURVEY_RELATION) ||
        offset > 64U || (filter && (kind != CERBERUS_AP_SURVEY_RELATION ||
        filter->kind != CERBERUS_AP_SURVEY_AP || !record_valid(filter)))) return false;
    out->kind = kind; out->filtered = filter != NULL; out->offset = (uint8_t)offset;
    for (unsigned source = 1U; source <= 2U; ++source) {
        const cerberus_ap_survey_bank_t *bank = &model->banks[source - 1U];
        if (!bank->published) continue;
        unsigned count = kind == CERBERUS_AP_SURVEY_AP ? bank->snapshot.ap_count : bank->snapshot.relation_count;
        for (unsigned i = 0; i < count; ++i) {
            cerberus_ap_survey_record_t row;
            copy_record(bank, (uint8_t)source, kind, i, &row);
            if (filter_matches(&row, filter)) ++out->total;
        }
    }
    uint64_t previous = 0U;
    for (unsigned nth = 0; nth < out->total && out->copied < CERBERUS_AP_SURVEY_PAGE_ROWS; ++nth) {
        cerberus_ap_survey_record_t next = {0};
        for (unsigned source = 1U; source <= 2U; ++source) {
            const cerberus_ap_survey_bank_t *bank = &model->banks[source - 1U];
            if (!bank->published) continue;
            unsigned count = kind == CERBERUS_AP_SURVEY_AP ? bank->snapshot.ap_count : bank->snapshot.relation_count;
            for (unsigned i = 0; i < count; ++i) {
                cerberus_ap_survey_record_t row;
                copy_record(bank, (uint8_t)source, kind, i, &row);
                if (row.order > previous && (!next.order || row.order < next.order) &&
                    filter_matches(&row, filter)) next = row;
            }
        }
        if (!next.order) return false;
        previous = next.order;
        if (nth >= offset) out->rows[out->copied++] = next;
    }
    return true;
}

bool cerberus_ap_survey_model_select(cerberus_ap_survey_model_t *model,
    const cerberus_ap_survey_record_t *painted)
{
    if (!model || !record_valid(painted)) return false;
    const cerberus_ap_survey_bank_t *bank = &model->banks[source_index(painted->source)];
    if (!bank->published || painted->run_id > bank->run_id || painted->receipt_us > bank->receipt_us)
        return false;
    if (painted->run_id == bank->run_id && painted->session_id == bank->snapshot.session_id &&
        painted->revision == bank->snapshot.revision) {
        cerberus_ap_survey_record_t current;
        if (!find_record(bank, painted, &current) || painted->order != current.order ||
            memcmp(&painted->evidence, &current.evidence, sizeof(painted->evidence))) return false;
    }
    memset(&model->selected, 0, sizeof(model->selected));
    model->selected.valid = true;
    model->selected.frozen = model->selected.latest = *painted;
    refresh_selection(model);
    return true;
}

void cerberus_ap_survey_model_clear_selection(cerberus_ap_survey_model_t *model)
{
    if (model) memset(&model->selected, 0, sizeof(model->selected));
}

bool cerberus_ap_survey_record_age(const cerberus_ap_survey_record_t *row,
    uint8_t direction, uint64_t now_us, uint64_t *age_ms)
{
    if (!age_ms) return false;
    *age_ms = 0U;
    if (!record_valid(row) || now_us < row->request_started_us) return false;
    uint64_t callback;
    if (row->kind == CERBERUS_AP_SURVEY_AP) {
        if (direction) return false;
        callback = row->evidence.ap.last_callback_us;
    } else {
        const cerberus_wifi_relation_row_t *client = &row->evidence.client.row;
        if ((direction != CERBERUS_WIFI_RELATION_TO_AP && direction != CERBERUS_WIFI_RELATION_FROM_AP) ||
            !(client->directions & direction)) return false;
        callback = direction == CERBERUS_WIFI_RELATION_TO_AP ? client->to_ap_callback_us : client->from_ap_callback_us;
    }
    if (callback > row->copied_source_us) return false;
    uint64_t source_age = row->copied_source_us - callback;
    uint64_t elapsed = now_us - row->request_started_us;
    if (source_age > UINT64_MAX - elapsed) return false;
    uint64_t bound = source_age + elapsed;
    *age_ms = bound / 1000U + (bound % 1000U != 0U);
    return true;
}
