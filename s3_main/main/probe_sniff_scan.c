#include "probe_sniff_scan.h"
#include <string.h>

static void discard(cerberus_probe_sniff_scan_t *s)
{
    memset(&s->staging, 0, sizeof(s->staging));
    memset(&s->scratch, 0, sizeof(s->scratch));
    memset(&s->before, 0, sizeof(s->before));
    s->pin = 0U; s->part = s->row = 0U; s->phase = CERBERUS_PROBE_SCAN_IDLE;
}
void cerberus_probe_sniff_scan_init(cerberus_probe_sniff_scan_t *s)
{ if (s) memset(s, 0, sizeof(*s)); }

static bool settings_valid(uint8_t mode, uint8_t channel)
{
    return mode <= CERBERUS_PROBE_SNIFF_HOP_BOTH &&
        (mode == CERBERUS_PROBE_SNIFF_FIXED ? cerberus_probe_sniff_channel_valid(channel) : channel == 0U);
}
esp_err_t cerberus_probe_sniff_scan_begin(cerberus_probe_sniff_scan_t *s, uint8_t mode, uint8_t channel)
{
    if (!s || !settings_valid(mode, channel)) return ESP_ERR_INVALID_ARG;
    if (s->request.session_id || s->phase != CERBERUS_PROBE_SCAN_IDLE) {
        if (s->requested || !s->have_control || (s->remote.flags & CERBERUS_PROBE_SNIFF_OWNED))
            return ESP_ERR_INVALID_STATE;
        s->restart_requested = true; s->restart_mode = mode; s->restart_channel = channel;
        s->close_requested = s->stop_requested = true; discard(s); return ESP_OK;
    }
    s->request = (cerberus_probe_sniff_start_t){.mode=mode,.channel=channel};
    s->requested = true; s->stop_requested = s->close_requested = s->restart_requested = false;
    s->have_control = false; s->error = ESP_OK; s->next_action_ms = s->next_cycle_ms = 0U;
    memset(&s->remote, 0, sizeof(s->remote));
    s->phase = CERBERUS_PROBE_SCAN_INFO;
    return ESP_OK;
}
void cerberus_probe_sniff_scan_stop(cerberus_probe_sniff_scan_t *s)
{
    if (!s) return;
    s->requested = s->restart_requested = false; s->stop_requested = true;
    s->next_action_ms = 0U; discard(s);
}
void cerberus_probe_sniff_scan_close(cerberus_probe_sniff_scan_t *s)
{
    if (!s) return;
    cerberus_probe_sniff_scan_stop(s); s->close_requested = true;
}
bool cerberus_probe_sniff_scan_released(const cerberus_probe_sniff_scan_t *s)
{ return !s || (!s->request.session_id && s->phase == CERBERUS_PROBE_SCAN_IDLE && !s->requested); }

static bool accept_control(cerberus_probe_sniff_scan_t *s,
    const cerberus_probe_sniff_control_t *r, uint64_t now)
{
    if (!cerberus_probe_sniff_control_valid(r, false, false, s->request.session_id)) {
        s->error = ESP_ERR_INVALID_RESPONSE; return false;
    }
    if (!(r->flags & CERBERUS_PROBE_SNIFF_BOUND)) {
        s->request.session_id = 0U; s->have_control = false; s->requested = false;
        s->restart_requested = false; discard(s); s->error = r->error_code;
        return false;
    }
    if (r->mode != s->request.mode || r->capacity != CERBERUS_PROBE_SNIFF_CAPACITY ||
        (s->request.mode == CERBERUS_PROBE_SNIFF_FIXED && r->channel &&
            r->channel != s->request.channel) ||
        (s->have_control && r->revision < s->remote.revision)) {
        s->error = ESP_ERR_INVALID_RESPONSE; return false;
    }
    s->remote = *r; s->have_control = true; s->last_status_ms = now;
    s->error = r->error_code;
    if (!(r->flags & CERBERUS_PROBE_SNIFF_OWNED)) s->requested = false;
    return true;
}

static void fail_cycle(cerberus_probe_sniff_scan_t *s, esp_err_t error, uint64_t now)
{ discard(s); s->error = error; s->next_cycle_ms = now + CERBERUS_PROBE_SNIFF_RETRY_MS; }

static esp_err_t read_part(cerberus_probe_sniff_scan_t *s,
    const cerberus_probe_sniff_transport_t *t, bool *done)
{
    bool row = s->phase == CERBERUS_PROBE_SCAN_ROW;
    size_t bytes = row ? sizeof(s->scratch.row) : sizeof(s->scratch.summary);
    cerberus_probe_sniff_read_t request = {.session_id=s->request.session_id,
        .pin=s->pin,.kind=row?CERBERUS_PROBE_SNIFF_READ_ROW:CERBERUS_PROBE_SNIFF_READ_SUMMARY,
        .index=row?s->row:0U,.part=s->part};
    cerberus_probe_sniff_chunk_t reply;
    *done = false;
    esp_err_t error = t->read(t->context, &request, &reply);
    if (error != ESP_OK) return error;
    if (reply.session_id != request.session_id || reply.part != request.part ||
        !reply.pin || (s->part && reply.pin != s->pin)) return ESP_ERR_INVALID_RESPONSE;
    size_t offset = (size_t)s->part * CERBERUS_PROBE_SNIFF_CHUNK_BYTES;
    if (offset >= bytes) return ESP_ERR_INVALID_RESPONSE;
    size_t amount = bytes - offset;
    if (amount > CERBERUS_PROBE_SNIFF_CHUNK_BYTES) amount = CERBERUS_PROBE_SNIFF_CHUNK_BYTES;
    for (size_t i = amount; i < CERBERUS_PROBE_SNIFF_CHUNK_BYTES; ++i)
        if (reply.data[i]) return ESP_ERR_INVALID_RESPONSE;
    memcpy(s->scratch.bytes + offset, reply.data, amount);
    s->pin = reply.pin; ++s->part;
    *done = offset + amount == bytes;
    if (*done) { s->part = 0U; s->pin = 0U; }
    return ESP_OK;
}

static bool summaries_same(const cerberus_probe_sniff_summary_t *a,
    const cerberus_probe_sniff_summary_t *b)
{
    cerberus_probe_sniff_summary_t left = *a, right = *b;
    left.copied_source_us = right.copied_source_us = 0U;
    left.flags &= (uint16_t)~CERBERUS_PROBE_SNIFF_COPY_TIME_VALID;
    right.flags &= (uint16_t)~CERBERUS_PROBE_SNIFF_COPY_TIME_VALID;
    return memcmp(&left, &right, sizeof(left)) == 0;
}

void cerberus_probe_sniff_scan_tick(cerberus_probe_sniff_scan_t *s,
    uint64_t now, const cerberus_probe_sniff_transport_t *t)
{
    if (!s || !t || !t->control || !t->read || !t->clock_us) return;
    int64_t begun = t->clock_us(t->context);
    if (begun < 0) { s->error = ESP_ERR_INVALID_STATE; return; }
    unsigned calls = 0U;
    while (calls < CERBERUS_PROBE_SNIFF_TICK_CALLS) {
        int64_t current = t->clock_us(t->context);
        if (current < begun || (uint64_t)(current - begun) >= CERBERUS_PROBE_SNIFF_TICK_BUDGET_US) return;
        if (now < s->next_action_ms) return;
        cerberus_probe_sniff_control_t reply;
        esp_err_t error;
        if (s->stop_requested && s->request.session_id &&
            (!s->have_control || (s->remote.flags & CERBERUS_PROBE_SNIFF_OWNED))) {
            ++calls; discard(s);
            error = t->control(t->context, CERBERUS_PROBE_SNIFF_COMMAND_STOP, &s->request, &reply);
            s->next_action_ms = now + CERBERUS_PROBE_SNIFF_RETRY_MS;
            if (error != ESP_OK) { s->error = error; return; }
            if (!accept_control(s, &reply, now)) return;
            if (reply.flags & CERBERUS_PROBE_SNIFF_OWNED) return;
            s->next_action_ms = 0U;
            if (calls == CERBERUS_PROBE_SNIFF_TICK_CALLS) return;
        }
        if (s->close_requested && s->request.session_id && s->have_control &&
            !(s->remote.flags & CERBERUS_PROBE_SNIFF_OWNED)) {
            ++calls;
            error = t->control(t->context, CERBERUS_PROBE_SNIFF_COMMAND_RELEASE, &s->request, &reply);
            s->next_action_ms = now + CERBERUS_PROBE_SNIFF_RETRY_MS;
            if (error != ESP_OK) { s->error = error; return; }
            if (!cerberus_probe_sniff_control_valid(&reply, false, true, s->request.session_id)) {
                s->error = ESP_ERR_INVALID_RESPONSE; return;
            }
            if (reply.flags & CERBERUS_PROBE_SNIFF_BOUND) {
                (void)accept_control(s, &reply, now); return;
            }
            bool restart = s->restart_requested; uint8_t mode = s->restart_mode, channel = s->restart_channel;
            s->request.session_id = 0U; s->have_control = false; s->requested = false;
            s->close_requested = s->stop_requested = s->restart_requested = false;
            discard(s); s->error = ESP_OK; s->next_action_ms = 0U;
            if (restart) (void)cerberus_probe_sniff_scan_begin(s, mode, channel);
            else return;
        }
        if (s->request.session_id && !s->have_control && s->phase == CERBERUS_PROBE_SCAN_IDLE) {
            ++calls;
            error = t->control(t->context, CERBERUS_PROBE_SNIFF_COMMAND_STATUS, &s->request, &reply);
            if (error != ESP_OK) { s->error = error; s->next_action_ms = now + CERBERUS_PROBE_SNIFF_RETRY_MS; return; }
            if (!accept_control(s, &reply, now)) return;
            if (calls == CERBERUS_PROBE_SNIFF_TICK_CALLS) return;
        }
        if (s->request.session_id && s->have_control &&
            now - s->last_status_ms >= CERBERUS_PROBE_SNIFF_KEEPALIVE_MS) {
            ++calls;
            error = t->control(t->context, CERBERUS_PROBE_SNIFF_COMMAND_STATUS, &s->request, &reply);
            if (error != ESP_OK) { s->error = error; s->next_action_ms = now + CERBERUS_PROBE_SNIFF_RETRY_MS; return; }
            if (!accept_control(s, &reply, now)) return;
            if (calls == CERBERUS_PROBE_SNIFF_TICK_CALLS) return;
        }
        if (s->phase == CERBERUS_PROBE_SCAN_IDLE) {
            if (!s->request.session_id || !s->have_control || now < s->next_cycle_ms ||
                !(s->remote.flags & (CERBERUS_PROBE_SNIFF_RECEIVING | CERBERUS_PROBE_SNIFF_DATA_READY))) return;
            if (s->published.frozen && s->published.control.session_id == s->request.session_id) return;
            s->phase = CERBERUS_PROBE_SCAN_BEFORE;
        }
        unsigned cost = s->phase == CERBERUS_PROBE_SCAN_INFO || s->phase == CERBERUS_PROBE_SCAN_START ? 2U : 1U;
        if (calls + cost > CERBERUS_PROBE_SNIFF_TICK_CALLS) return;
        calls += cost;
        if (s->phase == CERBERUS_PROBE_SCAN_INFO) {
            error = t->control(t->context, CERBERUS_PROBE_SNIFF_COMMAND_INFO, NULL, &reply);
            if (error != ESP_OK || !cerberus_probe_sniff_control_valid(&reply, true, false, 0U) ||
                !reply.session_id || reply.session_id == UINT64_MAX ||
                (reply.flags & (CERBERUS_PROBE_SNIFF_BOUND | CERBERUS_PROBE_SNIFF_OWNED))) {
                s->error = error != ESP_OK ? error : ESP_ERR_INVALID_STATE;
                s->requested = false; discard(s); return;
            }
            s->request.session_id = reply.session_id + 1U; s->phase = CERBERUS_PROBE_SCAN_START;
        } else if (s->phase == CERBERUS_PROBE_SCAN_START) {
            error = t->control(t->context, CERBERUS_PROBE_SNIFF_COMMAND_START, &s->request, &reply);
            s->phase = CERBERUS_PROBE_SCAN_IDLE;
            if (error != ESP_OK) {
                s->error = error; s->have_control = false;
                s->next_action_ms = now + CERBERUS_PROBE_SNIFF_RETRY_MS; return;
            }
            if (!accept_control(s, &reply, now)) return;
        } else if (s->phase == CERBERUS_PROBE_SCAN_BEFORE) {
            error = t->control(t->context, CERBERUS_PROBE_SNIFF_COMMAND_STATUS, &s->request, &reply);
            if (error != ESP_OK) { fail_cycle(s, error, now); return; }
            if (!accept_control(s, &reply, now)) return;
            if (!(reply.flags & (CERBERUS_PROBE_SNIFF_RECEIVING | CERBERUS_PROBE_SNIFF_DATA_READY))) {
                fail_cycle(s, reply.error_code ? reply.error_code : ESP_ERR_NOT_FINISHED, now); return;
            }
            memset(&s->staging, 0, sizeof(s->staging)); s->staging.control = reply;
            s->staging.requested_channel = s->request.channel;
            s->staging.count = reply.count; s->row = s->part = 0U; s->pin = 0U;
            s->cycle_frozen = (reply.flags & CERBERUS_PROBE_SNIFF_DATA_READY) &&
                !(reply.flags & (CERBERUS_PROBE_SNIFF_OWNED | CERBERUS_PROBE_SNIFF_RECEIVING));
            s->phase = CERBERUS_PROBE_SCAN_SUMMARY_BEFORE;
        } else if (s->phase == CERBERUS_PROBE_SCAN_AFTER) {
            error = t->control(t->context, CERBERUS_PROBE_SNIFF_COMMAND_STATUS, &s->request, &reply);
            if (error != ESP_OK) { fail_cycle(s, error, now); return; }
            if (!accept_control(s, &reply, now)) return;
            bool frozen = (reply.flags & CERBERUS_PROBE_SNIFF_DATA_READY) &&
                !(reply.flags & (CERBERUS_PROBE_SNIFF_OWNED | CERBERUS_PROBE_SNIFF_RECEIVING));
            if (frozen != s->cycle_frozen || (frozen &&
                (reply.revision != s->staging.control.revision || reply.count != s->staging.count ||
                    !summaries_same(&s->before, &s->staging.summary)))) {
                fail_cycle(s, ESP_ERR_INVALID_RESPONSE, now); return;
            }
            s->staging.control = reply; s->staging.frozen = frozen;
            if (!cerberus_probe_sniff_snapshot_valid(&s->staging) || s->publication == UINT64_MAX) {
                fail_cycle(s, ESP_ERR_INVALID_RESPONSE, now); return;
            }
            s->published = s->staging; ++s->publication; discard(s);
            s->error = reply.error_code; s->next_cycle_ms = now + CERBERUS_PROBE_SNIFF_REFRESH_MS;
        } else {
            bool done;
            error = read_part(s, t, &done);
            if (error != ESP_OK) { fail_cycle(s, error, now); return; }
            if (!done) continue;
            if (s->phase == CERBERUS_PROBE_SCAN_ROW) {
                if (!cerberus_probe_sniff_projection_valid(&s->scratch.row)) {
                    fail_cycle(s, ESP_ERR_INVALID_RESPONSE, now); return;
                }
                s->staging.rows[s->row++] = s->scratch.row;
                if (s->row == s->staging.count) s->phase = CERBERUS_PROBE_SCAN_SUMMARY_AFTER;
            } else {
                if (!cerberus_probe_sniff_summary_valid(&s->scratch.summary) ||
                    s->scratch.summary.count < s->staging.count) {
                    fail_cycle(s, ESP_ERR_INVALID_RESPONSE, now); return;
                }
                if (s->phase == CERBERUS_PROBE_SCAN_SUMMARY_BEFORE) {
                    s->before = s->scratch.summary;
                    s->phase = s->staging.count ? CERBERUS_PROBE_SCAN_ROW : CERBERUS_PROBE_SCAN_SUMMARY_AFTER;
                } else { s->staging.summary = s->scratch.summary; s->phase = CERBERUS_PROBE_SCAN_AFTER; }
            }
            memset(&s->scratch, 0, sizeof(s->scratch));
        }
    }
}
