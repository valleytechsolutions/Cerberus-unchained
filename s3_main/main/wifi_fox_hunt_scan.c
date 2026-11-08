#include "wifi_fox_hunt_scan.h"
#include "wifi_fox_hunt_validate.h"

#include <stddef.h>
#include <string.h>

#define FOX_STATUS_MS 500U
#define FOX_SIGNAL_MS 100U
#define FOX_RETRY_MS 250U
#define FOX_TRANSFER_MS 20U

static uint64_t fox_later(uint64_t now, uint32_t delay)
{ return now > UINT64_MAX - delay ? UINT64_MAX : now + delay; }

static uint32_t fox_age(const wifi_fox_hunt_scan_t *s, uint64_t now)
{
    if (!s->have_signal || s->signal_age_ms == UINT32_MAX || now < s->signal_anchor_ms) return UINT32_MAX;
    uint64_t elapsed = now - s->signal_anchor_ms;
    return elapsed >= UINT32_MAX - s->signal_age_ms ? UINT32_MAX : s->signal_age_ms + (uint32_t)elapsed;
}

static void fox_push(wifi_fox_hunt_scan_t *s, wifi_fox_hunt_entry_t entry)
{
    s->history[s->history_next] = entry;
    s->history_next = (uint16_t)((s->history_next + 1U) % WIFI_FOX_HISTORY);
    if (s->history_count < WIFI_FOX_HISTORY) ++s->history_count;
}

static void fox_gap(wifi_fox_hunt_scan_t *s)
{
    if (s->history_count && !s->gap_open)
        fox_push(s, (wifi_fox_hunt_entry_t){.kind=WIFI_FOX_GAP,.rssi=INT8_MIN,.age_ms=UINT32_MAX});
    s->gap_open = true;
}

bool wifi_fox_hunt_scan_start(wifi_fox_hunt_scan_t *s, const uint8_t target[6],
    uint8_t channel, uint8_t band, bool keep_history)
{
    if (!s || s->phase != WIFI_FOX_OFF || !wifi_fox_target_valid(target) ||
        !wifi_fox_band_channel_valid(band, channel)) return false;
    uint16_t count = keep_history ? s->history_count : 0U;
    uint16_t next = keep_history ? s->history_next : 0U;
    bool gap_open = count && s->history[(next + WIFI_FOX_HISTORY - 1U) % WIFI_FOX_HISTORY].kind == WIFI_FOX_GAP;
    memset(s, 0, offsetof(wifi_fox_hunt_scan_t, history));
    s->history_count = count; s->history_next = next;
    s->gap_open = gap_open;
    memcpy(s->target, target, sizeof(s->target)); s->initial_channel = channel; s->search_band = band;
    s->signal.rssi = INT8_MIN; s->signal.age_ms = s->signal_age_ms = UINT32_MAX;
    s->phase = WIFI_FOX_INFO;
    fox_gap(s);
    return true;
}

void wifi_fox_hunt_scan_stop(wifi_fox_hunt_scan_t *s)
{
    if (!s || s->phase == WIFI_FOX_OFF || s->phase >= WIFI_FOX_STOP) return;
    s->link_valid = false;
    if (s->phase == WIFI_FOX_INFO || s->phase == WIFI_FOX_START) {
        s->phase = WIFI_FOX_OFF; s->session = 0U;
    } else { s->phase = WIFI_FOX_STOP; s->next_action_ms = 0U; }
}

static void fox_release(wifi_fox_hunt_scan_t *s)
{ s->phase = WIFI_FOX_OFF; s->session = 0U; s->link_valid = false; }

static void fox_fault(wifi_fox_hunt_scan_t *s, esp_err_t error)
{
    s->local_error = s->error = error; s->link_valid = false; fox_gap(s);
    wifi_fox_hunt_scan_stop(s);
}

static void fox_control(wifi_fox_hunt_scan_t *s, const cerberus_wifi_fox_hunt_status_t *r,
    bool starting, uint64_t now)
{
    if (!wifi_fox_status_valid(r, false, s->session) ||
        ((r->flags & CERBERUS_WIFI_FOX_HUNT_BOUND) &&
            ((r->current_channel && !wifi_fox_band_channel_valid(s->search_band, r->current_channel)) ||
             (r->last_target_channel && !wifi_fox_band_channel_valid(s->search_band, r->last_target_channel))))) {
        fox_fault(s, ESP_ERR_INVALID_RESPONSE); return;
    }
    if (!(r->flags & CERBERUS_WIFI_FOX_HUNT_BOUND)) {
        s->error = s->local_error ? s->local_error : r->error_code;
        if (starting || r->error_code == ESP_ERR_NOT_FOUND) { fox_gap(s); fox_release(s); }
        else s->next_action_ms = fox_later(now, FOX_RETRY_MS);
        return;
    }
    if (r->latest_sequence < s->status.latest_sequence ||
        (s->have_signal && (r->latest_sequence < s->signal.sequence ||
            (r->latest_sequence == s->signal.sequence && r->last_target_channel != s->signal.channel))) ||
        r->sweeps < s->status.sweeps) {
        fox_fault(s, ESP_ERR_INVALID_RESPONSE); return;
    }
    s->status = *r;
    s->error = s->local_error ? s->local_error :
        r->state == CERBERUS_WIFI_PACKET_MONITOR_ERROR ? r->error_code : ESP_OK;
    s->status_due_ms = fox_later(now, FOX_STATUS_MS);
    if ((r->flags & CERBERUS_WIFI_FOX_HUNT_SEARCHING) || !(r->flags & CERBERUS_WIFI_FOX_HUNT_RECEIVING))
        fox_gap(s);
    if (!(r->flags & CERBERUS_WIFI_FOX_HUNT_HELD)) {
        s->phase = WIFI_FOX_FINAL;
        if (!r->latest_sequence || (s->have_signal && s->signal.sequence == r->latest_sequence)) fox_release(s);
    } else if (s->phase == WIFI_FOX_STOP || s->error) {
        s->phase = WIFI_FOX_STOP; s->next_action_ms = fox_later(now, FOX_RETRY_MS);
    }
}

static esp_err_t fox_signal(wifi_fox_hunt_scan_t *s, const cerberus_wifi_fox_hunt_signal_t *r, uint64_t now)
{
    if (!wifi_fox_signal_valid(r, s->session) ||
        (r->sequence && !wifi_fox_band_channel_valid(s->search_band, r->channel)) ||
        (r->sequence && r->sequence == s->status.latest_sequence && r->channel != s->status.last_target_channel) ||
        r->sequence < s->status.latest_sequence || (s->have_signal && r->sequence < s->signal.sequence))
        return ESP_ERR_INVALID_RESPONSE;
    if (!r->sequence) { s->link_valid = true; return ESP_OK; }
    bool newer = !s->have_signal || r->sequence > s->signal.sequence;
    if (!newer && (r->observed_ms != s->signal.observed_ms || r->rssi != s->signal.rssi ||
        r->channel != s->signal.channel || (s->signal.age_ms != UINT32_MAX && r->age_ms < s->signal.age_ms)))
        return ESP_ERR_INVALID_RESPONSE;
    /* Add the transport's complete budget to the source-reported age. Neither
     * a duplicate read nor repaint turns a retained observation into a new one. */
    uint32_t ceiling = r->age_ms >= UINT32_MAX - FOX_TRANSFER_MS ? UINT32_MAX : r->age_ms + FOX_TRANSFER_MS;
    if (!newer) {
        uint32_t previous = fox_age(s, now);
        if (previous > ceiling) ceiling = previous;
    }
    if (newer) {
        if (ceiling >= CERBERUS_WIFI_FOX_HUNT_LOST_MS) fox_gap(s);
        fox_push(s, (wifi_fox_hunt_entry_t){.kind=WIFI_FOX_SIGNAL,.sequence=r->sequence,
            .observed_ms=r->observed_ms,.age_ms=ceiling,.rssi=r->rssi,.channel=r->channel});
        s->gap_open = false;
        if (!(s->status.flags & CERBERUS_WIFI_FOX_HUNT_RECEIVING) ||
            (s->status.flags & CERBERUS_WIFI_FOX_HUNT_SEARCHING) || r->channel != s->status.current_channel)
            s->status_due_ms = now;
    }
    s->signal = *r; s->have_signal = true; s->signal_age_ms = ceiling; s->signal_anchor_ms = now;
    s->link_valid = true;
    return ESP_OK;
}

bool wifi_fox_hunt_scan_tick(wifi_fox_hunt_scan_t *s, uint64_t now, const wifi_fox_hunt_transport_t *t)
{
    if (!s || !t || !t->info || !t->start || !t->status || !t->stop || !t->signal) return false;
    if (s->have_tick && now < s->last_tick_ms) {
        s->signal_age_ms = UINT32_MAX; fox_fault(s, ESP_ERR_INVALID_STATE); s->next_action_ms = 0U;
    }
    s->have_tick = true; s->last_tick_ms = now;
    if (s->phase == WIFI_FOX_OFF) return false;
    if (s->have_signal && fox_age(s, now) >= CERBERUS_WIFI_FOX_HUNT_LOST_MS) fox_gap(s);
    if (now < s->next_action_ms) return false;
    s->next_action_ms = now;
    cerberus_wifi_fox_hunt_status_t control = {0};
    esp_err_t error = ESP_OK;
    if (s->phase == WIFI_FOX_INFO) {
        error = t->info(&control);
        if (error == ESP_OK && !wifi_fox_status_valid(&control, true, 0U)) error = ESP_ERR_INVALID_RESPONSE;
        if (error == ESP_OK && ((control.flags & CERBERUS_WIFI_FOX_HUNT_HELD) || control.session_id == UINT64_MAX))
            error = ESP_ERR_INVALID_STATE;
        if (error == ESP_OK) { s->session = control.session_id + 1U; s->phase = WIFI_FOX_START; }
        else { s->error = error; fox_release(s); }
    } else if (s->phase == WIFI_FOX_START) {
        cerberus_wifi_fox_hunt_start_t request = {.session_id=s->session,
            .initial_channel=s->initial_channel,.search_band=s->search_band};
        memcpy(request.target, s->target, sizeof(request.target));
        s->phase = WIFI_FOX_WATCH; s->status_due_ms = now;
        error = t->start(&request, &control);
        if (error == ESP_OK) fox_control(s, &control, true, now);
        else if (error == ESP_ERR_NOT_SUPPORTED) { s->error = error; fox_release(s); }
    } else if (s->phase == WIFI_FOX_STOP) {
        error = t->stop(s->session, &control);
        if (error == ESP_OK) fox_control(s, &control, false, now);
    } else if (s->phase == WIFI_FOX_FINAL) {
        cerberus_wifi_fox_hunt_signal_t signal = {0};
        error = t->signal(s->session, &signal);
        if (error == ESP_OK) error = fox_signal(s, &signal, now);
        if (error != ESP_OK) fox_gap(s);
        fox_release(s); /* HELD-clear was already proven by the exact status. */
    } else if (now >= s->status_due_ms) {
        error = t->status(s->session, &control);
        if (error == ESP_OK) fox_control(s, &control, false, now);
    } else if (now >= s->signal_due_ms) {
        cerberus_wifi_fox_hunt_signal_t signal = {0};
        error = t->signal(s->session, &signal);
        if (error == ESP_OK) {
            error = fox_signal(s, &signal, now);
            if (error != ESP_OK) fox_fault(s, error);
        }
        if (error == ESP_ERR_NOT_FOUND) s->status_due_ms = 0U;
        s->signal_due_ms = fox_later(now, FOX_SIGNAL_MS);
    } else return false;
    s->transport_error = error;
    if (error != ESP_OK) { s->link_valid = false; fox_gap(s); s->next_action_ms = fox_later(now, FOX_RETRY_MS); }
    return true;
}

void wifi_fox_hunt_scan_view(const wifi_fox_hunt_scan_t *s, uint64_t now, wifi_fox_hunt_view_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out)); out->age_ms = UINT32_MAX; out->rssi = INT8_MIN;
    out->state = WIFI_FOX_STOPPED;
    if (!s) return;
    out->have_signal = s->have_signal; out->age_ms = fox_age(s, now);
    out->sequence = s->signal.sequence; out->sweeps = s->status.sweeps;
    out->channels_skipped = (s->status.flags & CERBERUS_WIFI_FOX_HUNT_CHANNELS_SKIPPED) != 0U;
    out->sweeps_saturated = (s->status.flags & CERBERUS_WIFI_FOX_HUNT_SWEEPS_SATURATED) != 0U;
    out->warning = out->channels_skipped ? s->status.error_code : ESP_OK;
    if (s->have_signal) { out->rssi = s->signal.rssi; out->observed_channel = s->signal.channel; }
    bool receiving = (s->status.flags & CERBERUS_WIFI_FOX_HUNT_RECEIVING) != 0U;
    if (s->phase == WIFI_FOX_WATCH && s->link_valid && receiving) out->listening_channel = s->status.current_channel;
    if (s->error || (s->phase != WIFI_FOX_OFF && s->transport_error)) out->state = WIFI_FOX_FAULT;
    else if (s->phase == WIFI_FOX_OFF || s->phase >= WIFI_FOX_STOP) out->state = WIFI_FOX_STOPPED;
    else if (s->phase < WIFI_FOX_WATCH) out->state = WIFI_FOX_WAITING;
    else if (s->have_signal && s->link_valid && receiving && out->age_ms < CERBERUS_WIFI_FOX_HUNT_LOST_MS &&
        s->signal.channel == s->status.current_channel && !(s->status.flags & CERBERUS_WIFI_FOX_HUNT_SEARCHING)) {
        out->current = true; out->state = WIFI_FOX_TRACKING;
    } else if (s->status.flags & CERBERUS_WIFI_FOX_HUNT_SEARCHING) out->state = WIFI_FOX_SEARCHING;
    else out->state = s->have_signal ? WIFI_FOX_STALE : WIFI_FOX_WAITING;
}

bool wifi_fox_hunt_scan_history(const wifi_fox_hunt_scan_t *s, unsigned index, wifi_fox_hunt_entry_t *out)
{
    if (!s || !out || index >= s->history_count) return false;
    unsigned first = (s->history_next + WIFI_FOX_HISTORY - s->history_count) % WIFI_FOX_HISTORY;
    *out = s->history[(first + index) % WIFI_FOX_HISTORY];
    return true;
}
