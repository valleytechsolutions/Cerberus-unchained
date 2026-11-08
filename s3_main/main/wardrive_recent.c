#include "wardrive_recent.h"

#include <string.h>

static bool ready(const cerberus_wardrive_recent_t *state)
{
    return state && state->session_id && state->revision &&
        state->count <= CERBERUS_WARDRIVE_RECENT_HISTORY &&
        state->next < CERBERUS_WARDRIVE_RECENT_HISTORY &&
        state->event_count >= state->count;
}

static uint32_t increment(uint32_t value)
{
    return value == UINT32_MAX ? value : value + 1U;
}

static bool valid_observation(const cerberus_wardrive_observation_t *observation)
{
    return observation &&
        (observation->type == CERBERUS_WARDRIVE_WIFI ||
         observation->type == CERBERUS_WARDRIVE_BLE) &&
        (observation->type == CERBERUS_WARDRIVE_BLE ?
            observation->address_type <= 3U : observation->address_type == 0U) &&
        cerberus_wardrive_observation_valid(observation);
}

static void copy_observation(cerberus_wardrive_observation_t *out,
    const cerberus_wardrive_observation_t *source)
{
    /* No uninitialized struct padding, unused label tail or alternate union
     * arm becomes part of a retained value. Preserve every applicable field. */
    memset(out, 0, sizeof(*out));
    out->type = source->type;
    out->radio = source->radio;
    memcpy(out->address, source->address, sizeof(out->address));
    out->rssi_dbm = source->rssi_dbm;
    out->label_length = source->label_length;
    memcpy(out->label, source->label, source->label_length);
    if (source->type == CERBERUS_WARDRIVE_WIFI) {
        out->channel = source->channel;
        out->security = source->security;
    } else {
        out->address_type = source->address_type;
        out->manufacturer_id = source->manufacturer_id;
        const cerberus_s3_ble_wardrive_evidence_t *s = &source->ble_evidence;
        cerberus_s3_ble_wardrive_evidence_t *d = &out->ble_evidence;
        d->advertisement_sequence = s->advertisement_sequence;
        d->advertisement_observed_ms = s->advertisement_observed_ms;
        d->scan_response_sequence = s->scan_response_sequence;
        d->scan_response_observed_ms = s->scan_response_observed_ms;
        d->advertisement = s->advertisement;
        d->scan_response = s->scan_response;
        d->flags = s->flags;
        d->sid = s->sid;
    }
}

static bool same_identity(const cerberus_wardrive_recent_row_t *a,
    const cerberus_wardrive_recent_row_t *b)
{
    return a->observation.type == b->observation.type &&
        (a->observation.type != CERBERUS_WARDRIVE_BLE ||
            a->observation.address_type == b->observation.address_type) &&
        memcmp(a->observation.address, b->observation.address, 6U) == 0;
}

void cerberus_wardrive_recent_init(cerberus_wardrive_recent_t *state)
{
    if (state) memset(state, 0, sizeof(*state));
}

bool cerberus_wardrive_recent_begin(cerberus_wardrive_recent_t *state,
    uint64_t session_id)
{
    if (!state || !session_id || session_id <= state->session_id) return false;
    memset(state, 0, sizeof(*state));
    state->session_id = session_id;
    state->revision = 1U;
    return true;
}

bool cerberus_wardrive_recent_observe(cerberus_wardrive_recent_t *state,
    uint64_t session_id, uint64_t sequence,
    const cerberus_wardrive_observation_t *observation,
    uint64_t receipt_ms, bool receipt_valid,
    cerberus_wardrive_recent_ticket_t *ticket)
{
    if (ticket) memset(ticket, 0, sizeof(*ticket));
    if (!ticket || !ready(state) || session_id != state->session_id ||
        !sequence || sequence <= state->last_sequence ||
        state->revision == UINT64_MAX || state->event_count == UINT64_MAX ||
        !valid_observation(observation)) return false;
    cerberus_wardrive_recent_row_t row;
    memset(&row, 0, sizeof(row));
    copy_observation(&row.observation, observation);
    row.session_id = session_id;
    row.sequence = sequence;
    row.outcome = CERBERUS_WARDRIVE_RECENT_PENDING;
    row.receipt_valid = receipt_valid &&
        (!state->has_receipt || receipt_ms >= state->last_receipt_ms);
    if (row.receipt_valid) {
        row.receipt_ms = receipt_ms;
        state->last_receipt_ms = receipt_ms;
        state->has_receipt = true;
    } else {
        state->invalid_receipts = increment(state->invalid_receipts);
    }
    state->rows[state->next] = row;
    state->next = (uint8_t)((state->next + 1U) % CERBERUS_WARDRIVE_RECENT_HISTORY);
    if (state->count < CERBERUS_WARDRIVE_RECENT_HISTORY) ++state->count;
    else state->overwritten = increment(state->overwritten);
    state->last_sequence = sequence;
    ++state->event_count;
    ++state->revision;
    *ticket = (cerberus_wardrive_recent_ticket_t){session_id, sequence};
    return true;
}

bool cerberus_wardrive_recent_set_outcome(cerberus_wardrive_recent_t *state,
    cerberus_wardrive_recent_ticket_t ticket,
    cerberus_wardrive_recent_outcome_t outcome)
{
    if (!ready(state) || ticket.session_id != state->session_id || !ticket.sequence ||
        (unsigned)outcome > CERBERUS_WARDRIVE_RECENT_NOT_LOGGED) return false;
    for (size_t i = 0U; i < state->count; ++i) {
        cerberus_wardrive_recent_row_t *row = &state->rows[i];
        if (row->session_id != ticket.session_id || row->sequence != ticket.sequence) continue;
        if (row->outcome == outcome) return true;
        if (row->outcome != CERBERUS_WARDRIVE_RECENT_PENDING ||
            state->revision == UINT64_MAX) return false;
        row->outcome = outcome;
        ++state->revision;
        return true;
    }
    return false;
}

static size_t recent_indexes(const cerberus_wardrive_recent_t *state,
    uint8_t indexes[CERBERUS_WARDRIVE_RECENT_VISIBLE], size_t capacity)
{
    if (!ready(state) || !capacity) return 0U;
    if (capacity > CERBERUS_WARDRIVE_RECENT_VISIBLE) capacity = CERBERUS_WARDRIVE_RECENT_VISIBLE;
    size_t count = 0U;
    for (size_t age = 0U; age < state->count && count < capacity; ++age) {
        size_t index = (state->next + CERBERUS_WARDRIVE_RECENT_HISTORY - 1U - age) %
            CERBERUS_WARDRIVE_RECENT_HISTORY;
        const cerberus_wardrive_recent_row_t *row = &state->rows[index];
        bool duplicate = false;
        for (size_t i = 0U; !state->all_observations && i < count; ++i)
            duplicate |= same_identity(row, &state->rows[indexes[i]]);
        if (!duplicate) indexes[count++] = (uint8_t)index;
    }
    return count;
}

size_t cerberus_wardrive_recent_copy_recent(const cerberus_wardrive_recent_t *state,
    cerberus_wardrive_recent_row_t *out, size_t capacity)
{
    if (!out) return 0U;
    uint8_t indexes[CERBERUS_WARDRIVE_RECENT_VISIBLE];
    size_t count = recent_indexes(state, indexes, capacity);
    for (size_t i = 0U; i < count; ++i) out[i] = state->rows[indexes[i]];
    return count;
}

size_t cerberus_wardrive_recent_visible_count(const cerberus_wardrive_recent_t *state)
{
    uint8_t indexes[CERBERUS_WARDRIVE_RECENT_VISIBLE];
    return recent_indexes(state, indexes, CERBERUS_WARDRIVE_RECENT_VISIBLE);
}

bool cerberus_wardrive_recent_age(const cerberus_wardrive_recent_row_t *row,
    uint64_t now_ms, bool now_valid, uint64_t *age_ms)
{
    if (age_ms) *age_ms = 0U;
    if (!age_ms || !row || !row->session_id || !row->sequence ||
        !row->receipt_valid || !now_valid || now_ms < row->receipt_ms) return false;
    *age_ms = now_ms - row->receipt_ms;
    return true;
}
