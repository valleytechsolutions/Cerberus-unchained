#include "wifi_channel_analyzer_scan.h"
#include "wifi_channel_analyzer_validate.h"

#include <stddef.h>
#include <string.h>

#define ANALYZER_POLL_MS 250U
#define ANALYZER_RETRY_MS 500U
#define ANALYZER_HINT_MS 2000U

static uint64_t later(uint64_t now, uint32_t delay)
{ return now > UINT64_MAX - delay ? UINT64_MAX : now + delay; }

static uint32_t add_count(uint32_t a, uint32_t b)
{ return a > UINT32_MAX - b ? UINT32_MAX : a + b; }

static void push(wifi_channel_analyzer_scan_t *s, wifi_channel_analyzer_entry_t row)
{
    s->history[s->history_next] = row;
    s->history_next = (uint16_t)((s->history_next + 1U) % WIFI_ANALYZER_HISTORY);
    if (s->history_count < WIFI_ANALYZER_HISTORY) ++s->history_count;
}

static void gap(wifi_channel_analyzer_scan_t *s, uint32_t missing)
{
    push(s, (wifi_channel_analyzer_entry_t){.kind=WIFI_ANALYZER_GAP,
        .channel=s->channel, .missing_windows=missing});
    s->lost_windows = add_count(s->lost_windows, missing);
    s->current_valid = false;
}

bool wifi_channel_analyzer_scan_start(wifi_channel_analyzer_scan_t *s, uint8_t channel, bool keep_history)
{
    if (!s || s->phase != WIFI_ANALYZER_OFF || !cerberus_wifi_packet_monitor_channel_valid(channel)) return false;
    uint16_t count = keep_history ? s->history_count : 0U;
    uint16_t next = keep_history ? s->history_next : 0U;
    memset(s, 0, offsetof(wifi_channel_analyzer_scan_t, history));
    s->history_count = count; s->history_next = next;
    s->channel = channel; s->phase = WIFI_ANALYZER_INFO; s->new_segment = true;
    return true;
}

void wifi_channel_analyzer_scan_stop(wifi_channel_analyzer_scan_t *s)
{
    if (!s || s->phase == WIFI_ANALYZER_OFF || s->phase >= WIFI_ANALYZER_STOP) return;
    s->reading_hint = false;
    if (s->phase == WIFI_ANALYZER_INFO || s->phase == WIFI_ANALYZER_START) {
        s->phase = WIFI_ANALYZER_OFF; s->session = 0U;
    } else {
        s->phase = WIFI_ANALYZER_STOP; s->next_action_ms = 0U;
    }
}

static void release(wifi_channel_analyzer_scan_t *s)
{ s->phase = WIFI_ANALYZER_OFF; s->session = 0U; s->reading_hint = false; }

static void local_fault(wifi_channel_analyzer_scan_t *s, esp_err_t error)
{
    s->local_error = s->error = error;
    s->current_valid = false;
    wifi_channel_analyzer_scan_stop(s);
}

static void accept_status(wifi_channel_analyzer_scan_t *s,
    const cerberus_wifi_channel_analyzer_status_t *r, bool starting, uint64_t now)
{
    if (!wifi_analyzer_status_valid(r, false, s->session) ||
        ((r->flags & CERBERUS_WIFI_CHANNEL_ANALYZER_BOUND) && r->channel != s->channel)) {
        local_fault(s, ESP_ERR_INVALID_RESPONSE); return;
    }
    if (!(r->flags & CERBERUS_WIFI_CHANNEL_ANALYZER_BOUND)) {
        s->error = s->local_error ? s->local_error : r->error_code;
        if (starting || r->error_code == ESP_ERR_NOT_FOUND) {
            if (s->fetched < s->status.latest_sequence)
                gap(s, s->status.latest_sequence - s->fetched);
            release(s);
        }
        else s->next_action_ms = later(now, ANALYZER_RETRY_MS);
        return;
    }
    if (r->latest_sequence < s->status.latest_sequence || r->latest_sequence < s->fetched) {
        local_fault(s, ESP_ERR_INVALID_RESPONSE); return;
    }
    s->status = *r;
    if (s->fetched != r->latest_sequence) s->current_valid = false;
    s->error = s->local_error ? s->local_error : r->error_code;
    s->status_due_ms = later(now, ANALYZER_POLL_MS);
    uint32_t oldest_before = r->latest_sequence - r->stored_count;
    if (s->fetched < oldest_before) {
        gap(s, oldest_before - s->fetched);
        s->fetched = oldest_before;
    }
    if (!(r->flags & CERBERUS_WIFI_CHANNEL_ANALYZER_HELD)) {
        s->phase = WIFI_ANALYZER_DRAIN; s->reading_hint = false;
        if (s->fetched == r->latest_sequence) release(s);
    } else if (r->state == CERBERUS_WIFI_PACKET_MONITOR_ERROR || s->local_error) {
        s->phase = WIFI_ANALYZER_STOP;
        s->next_action_ms = later(now, ANALYZER_POLL_MS);
    } else if (s->phase == WIFI_ANALYZER_STOP) s->next_action_ms = later(now, ANALYZER_POLL_MS);
}

static esp_err_t read_sample(wifi_channel_analyzer_scan_t *s, const wifi_channel_analyzer_transport_t *t)
{
    uint32_t sequence = s->fetched + 1U;
    cerberus_wifi_channel_analyzer_sample_t r = {0};
    esp_err_t error = t->sample(s->session, sequence, &r);
    if (error != ESP_OK) return error;
    if (!wifi_analyzer_sample_valid(&r, s->session, sequence) || r.channel != s->channel) {
        local_fault(s, ESP_ERR_INVALID_RESPONSE); return ESP_ERR_INVALID_RESPONSE;
    }
    if (s->new_segment) {
        if (s->history_count) gap(s, 0U); /* Unknown duration between sessions. */
        push(s, (wifi_channel_analyzer_entry_t){.kind=WIFI_ANALYZER_CHANNEL, .channel=s->channel});
        s->new_segment = false;
    }
    push(s, (wifi_channel_analyzer_entry_t){.kind=WIFI_ANALYZER_WINDOW, .sequence=r.sequence,
        .frames=r.frames, .window_us=r.window_us, .channel=r.channel, .flags=r.flags});
    s->fetched = sequence; s->current_valid = sequence == s->status.latest_sequence;
    if (s->phase == WIFI_ANALYZER_DRAIN && s->fetched == s->status.latest_sequence) release(s);
    return ESP_OK;
}

static esp_err_t hint_step(wifi_channel_analyzer_scan_t *s, uint64_t now,
    const wifi_channel_analyzer_transport_t *t)
{
    if (!s->reading_hint) {
        cerberus_wifi_channel_analyzer_hint_status_t r = {0};
        esp_err_t error = t->hint_peek(s->session, &r);
        s->hint_due_ms = later(now, ANALYZER_HINT_MS);
        if (error != ESP_OK) return error;
        if (!wifi_analyzer_hint_status_valid(&r, s->session)) return ESP_ERR_INVALID_RESPONSE;
        if (r.error_code != ESP_OK) return r.error_code == ESP_ERR_NOT_FOUND ? ESP_OK : r.error_code;
        s->hint_pending = r; s->hint_pending_ms = now; s->hint_part = 0U;
        memset(s->hint_bytes, 0, sizeof(s->hint_bytes)); s->reading_hint = true;
        return ESP_OK;
    }
    cerberus_wifi_channel_analyzer_hint_part_t r = {0};
    esp_err_t error = t->hint_read(s->session, s->hint_pending.projection, s->hint_part, &r);
    if (error == ESP_OK && !wifi_analyzer_hint_part_valid(&r, s->session,
        s->hint_pending.projection, s->hint_part)) error = ESP_ERR_INVALID_RESPONSE;
    if (error != ESP_OK) { s->reading_hint = false; return error; }
    unsigned offset = s->hint_part * CERBERUS_WIFI_CHANNEL_ANALYZER_HINT_PART_BYTES;
    unsigned length = sizeof(s->hint_bytes) - offset;
    if (length > sizeof(r.bytes)) length = sizeof(r.bytes);
    memcpy(s->hint_bytes + offset, r.bytes, length);
    if (++s->hint_part == CERBERUS_WIFI_CHANNEL_ANALYZER_HINT_PARTS) {
        cerberus_wifi_channel_analyzer_hint_t hint;
        memcpy(&hint, s->hint_bytes, sizeof(hint)); s->reading_hint = false;
        if (!wifi_analyzer_hint_valid(&hint) || hint.channel != s->channel) return ESP_ERR_INVALID_RESPONSE;
        s->hint = hint; s->hint_valid = true;
        s->hint_age_at_peek_ms = s->hint_pending.age_ms; s->hint_peek_ms = s->hint_pending_ms;
    }
    return ESP_OK;
}

bool wifi_channel_analyzer_scan_tick(wifi_channel_analyzer_scan_t *s, uint64_t now,
    const wifi_channel_analyzer_transport_t *t)
{
    if (!s || !t || !t->info || !t->start || !t->status || !t->stop || !t->sample ||
        !t->hint_peek || !t->hint_read) return false;
    if (s->have_tick && now < s->last_tick_ms) {
        s->hint_valid = false; s->hint_age_at_peek_ms = UINT32_MAX;
        local_fault(s, ESP_ERR_INVALID_STATE); s->next_action_ms = 0U;
    }
    s->have_tick = true; s->last_tick_ms = now;
    if (s->phase == WIFI_ANALYZER_OFF || now < s->next_action_ms) return false;
    s->next_action_ms = now;
    cerberus_wifi_channel_analyzer_status_t r = {0};
    esp_err_t error = ESP_OK;
    if (s->phase == WIFI_ANALYZER_INFO) {
        error = t->info(&r);
        if (error == ESP_OK && !wifi_analyzer_status_valid(&r, true, 0U)) error = ESP_ERR_INVALID_RESPONSE;
        if (error == ESP_OK && (r.session_id == UINT64_MAX || (r.flags & CERBERUS_WIFI_CHANNEL_ANALYZER_HELD)))
            error = ESP_ERR_INVALID_STATE;
        if (error == ESP_OK) { s->session = r.session_id + 1U; s->phase = WIFI_ANALYZER_START; }
        else { s->error = error; release(s); }
    } else if (s->phase == WIFI_ANALYZER_START) {
        /* A missing START reply still owns a possible session. Never replay it. */
        s->phase = WIFI_ANALYZER_WATCH; s->status_due_ms = now;
        error = t->start(s->session, s->channel, &r);
        if (error == ESP_OK) accept_status(s, &r, true, now);
    } else if (s->phase == WIFI_ANALYZER_STOP) {
        error = t->stop(s->session, &r);
        if (error == ESP_OK) accept_status(s, &r, false, now);
    } else if (s->phase == WIFI_ANALYZER_DRAIN) {
        error = read_sample(s, t);
        if (error != ESP_OK) {
            /* An exact unheld status already proved release. A failed final
             * read loses history, but must not keep Back waiting on a radio. */
            gap(s, s->status.latest_sequence - s->fetched); release(s);
        }
    } else if (now >= s->status_due_ms) {
        error = t->status(s->session, &r);
        if (error == ESP_OK) accept_status(s, &r, false, now);
    } else if (s->reading_hint && s->status.latest_sequence - s->fetched <= 8U) {
        error = hint_step(s, now, t);
    } else if (s->fetched < s->status.latest_sequence) {
        error = read_sample(s, t);
        if (error == ESP_ERR_NOT_FOUND) s->status_due_ms = 0U;
    } else if (now >= s->hint_due_ms || s->reading_hint) {
        error = hint_step(s, now, t);
    } else return false;
    s->transport_error = error;
    if (error != ESP_OK) {
        if (s->phase != WIFI_ANALYZER_OFF) s->current_valid = false;
        s->next_action_ms = later(now, ANALYZER_RETRY_MS);
    }
    return true;
}

bool wifi_channel_analyzer_scan_history(const wifi_channel_analyzer_scan_t *s, unsigned index,
    wifi_channel_analyzer_entry_t *out)
{
    if (!s || !out || index >= s->history_count) return false;
    unsigned first = (s->history_next + WIFI_ANALYZER_HISTORY - s->history_count) % WIFI_ANALYZER_HISTORY;
    *out = s->history[(first + index) % WIFI_ANALYZER_HISTORY]; return true;
}

static uint32_t rate(uint64_t frames, uint64_t duration, bool *limited)
{
    uint64_t value = duration ? frames * 1000000U / duration : 0U;
    if (value > UINT32_MAX) { *limited = true; return UINT32_MAX; }
    return (uint32_t)value;
}

void wifi_channel_analyzer_scan_summary(const wifi_channel_analyzer_scan_t *s,
    wifi_channel_analyzer_summary_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out)); if (!s) return;
    for (unsigned i = 0; i < s->history_count; ++i) {
        wifi_channel_analyzer_entry_t row;
        if (!wifi_channel_analyzer_scan_history(s, i, &row)) break;
        if (row.kind == WIFI_ANALYZER_GAP) {
            ++out->gap_count; out->missing_windows = add_count(out->missing_windows, row.missing_windows);
        } else if (row.kind == WIFI_ANALYZER_WINDOW) {
            ++out->windows; out->frames += row.frames; out->window_us += row.window_us;
            out->current_rate = rate(row.frames, row.window_us, &out->rate_limited);
            if (out->current_rate > out->peak_rate) out->peak_rate = out->current_rate;
            out->lower_bound |= (row.flags & CERBERUS_WIFI_CHANNEL_ANALYZER_SATURATED) != 0U;
        }
    }
    out->current_valid = s->current_valid && out->windows;
    out->average_rate = rate(out->frames, out->window_us, &out->rate_limited);
}

uint32_t wifi_channel_analyzer_scan_hint_age(const wifi_channel_analyzer_scan_t *s, uint64_t now)
{
    if (!s || !s->hint_valid || s->hint_age_at_peek_ms == UINT32_MAX || now < s->hint_peek_ms)
        return UINT32_MAX;
    uint64_t elapsed = now - s->hint_peek_ms;
    return elapsed >= UINT32_MAX - s->hint_age_at_peek_ms ? UINT32_MAX :
        s->hint_age_at_peek_ms + (uint32_t)elapsed;
}

size_t wifi_channel_analyzer_scan_ssid_display(const cerberus_wifi_channel_analyzer_hint_t *hint,
    char *out, size_t capacity)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t length = 0U;
    if (out && capacity) out[0] = '\0';
    if (!hint || hint->ssid_length > 32U) return 0U;
    for (unsigned i = 0; i < hint->ssid_length; ++i) {
        uint8_t byte = hint->ssid[i]; char text[4] = {(char)byte, 0, 0, 0};
        unsigned count = 1U;
        if (byte < 32U || byte > 126U || byte == '\\') {
            text[0] = '\\'; text[1] = 'x'; text[2] = hex[byte >> 4]; text[3] = hex[byte & 15U]; count = 4U;
        }
        for (unsigned n = 0; n < count; ++n) {
            if (out && length + 1U < capacity) out[length] = text[n];
            ++length;
        }
    }
    if (out && capacity) out[length < capacity ? length : capacity - 1U] = '\0';
    return length;
}
