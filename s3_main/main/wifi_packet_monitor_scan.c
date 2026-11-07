#include "wifi_packet_monitor_scan.h"

#include <string.h>

#define MONITOR_POLL_MS 250U
#define MONITOR_RETRY_MS 700U

static uint64_t later(uint64_t now, unsigned delay)
{
    return now > UINT64_MAX - delay ? UINT64_MAX : now + delay;
}

bool wifi_packet_monitor_scan_start(wifi_packet_monitor_scan_t *s, uint8_t channel)
{
    if (!s || s->phase != WIFI_MONITOR_OFF ||
        !cerberus_wifi_packet_monitor_channel_valid(channel)) return false;
    memset(s, 0, sizeof(*s));
    s->channel = channel;
    s->phase = WIFI_MONITOR_INFO;
    return true;
}

void wifi_packet_monitor_scan_stop(wifi_packet_monitor_scan_t *s)
{
    if (!s || s->phase == WIFI_MONITOR_OFF || s->phase == WIFI_MONITOR_STOP) return;
    /* INFO and START have not sent a mutation. Only WATCH holds a claim. */
    if (s->phase == WIFI_MONITOR_INFO || s->phase == WIFI_MONITOR_START) {
        s->phase = WIFI_MONITOR_OFF;
        s->session = 0U;
    } else {
        s->phase = WIFI_MONITOR_STOP;
        s->next_action_ms = 0U;
    }
}

static void release(wifi_packet_monitor_scan_t *s)
{
    s->session = 0U;
    s->phase = WIFI_MONITOR_OFF;
}

static bool bound_reply(const wifi_packet_monitor_scan_t *s,
    const cerberus_wifi_packet_monitor_session_status_t *r)
{
    return r->session_id == s->session && r->channel == s->channel &&
        (r->flags & CERBERUS_WIFI_PACKET_MONITOR_BOUND);
}

static void publish(wifi_packet_monitor_scan_t *s,
    const cerberus_wifi_packet_monitor_session_status_t *r)
{
    s->view.state = r->state;
    s->view.channel = r->channel;
    s->view.stored_count = r->stored_count;
    s->view.latest_sequence = r->latest_sequence;
    s->view.elapsed_ms = r->elapsed_ms;
    s->view.sample_ms = CERBERUS_WIFI_PACKET_MONITOR_SAMPLE_MS;
    s->view.error_code = r->error_code;
    s->error = r->error_code;
    if (r->state == CERBERUS_WIFI_PACKET_MONITOR_ERROR && s->error == ESP_OK)
        s->error = ESP_FAIL;
    if (!s->have_fetched) {
        s->fetched = (uint16_t)(r->latest_sequence - r->stored_count);
        s->have_fetched = true;
    }
    uint16_t missing = (uint16_t)(r->latest_sequence - s->fetched);
    unsigned available = r->stored_count < 4U ? r->stored_count : 4U;
    if (missing > available)
        s->fetched = (uint16_t)(r->latest_sequence - available);
}

static void accept(wifi_packet_monitor_scan_t *s,
    const cerberus_wifi_packet_monitor_session_status_t *r, bool starting, uint64_t now)
{
    if (!bound_reply(s, r)) {
        if (r->session_id != s->session || r->flags || r->error_code == ESP_OK) {
            s->error = ESP_ERR_INVALID_RESPONSE;
            s->next_action_ms = later(now, MONITOR_RETRY_MS);
            return;
        }
        s->error = r->error_code;
        if (starting) {
            release(s);
            if (r->error_code == ESP_ERR_INVALID_STATE) {
                s->phase = WIFI_MONITOR_INFO;
                s->next_action_ms = later(now, MONITOR_RETRY_MS);
            }
        } else if (r->error_code == ESP_ERR_NOT_FOUND) {
            /* The exact session was retired or the peer rebooted. Never adopt
             * or stop the current receiver reported by a later INFO. */
            s->view.state = CERBERUS_WIFI_PACKET_MONITOR_ERROR;
            s->view.error_code = r->error_code;
            release(s);
        } else s->next_action_ms = later(now, MONITOR_RETRY_MS);
        return;
    }
    publish(s, r);
    s->status_due_ms = later(now, MONITOR_POLL_MS);
    if (s->phase == WIFI_MONITOR_STOP) {
        if (!(r->flags & CERBERUS_WIFI_PACKET_MONITOR_HELD)) release(s);
        else s->next_action_ms = later(now, MONITOR_POLL_MS);
    }
}

bool wifi_packet_monitor_scan_tick(wifi_packet_monitor_scan_t *s, uint64_t now,
    const wifi_packet_monitor_transport_t *t)
{
    if (!s || !t || !t->info || !t->start || !t->status || !t->stop || !t->sample)
        return false;
    if (s->have_tick && now < s->last_tick_ms) {
        wifi_packet_monitor_scan_stop(s);
        s->error = ESP_ERR_INVALID_STATE;
        s->next_action_ms = 0U;
    }
    s->have_tick = true;
    s->last_tick_ms = now;
    if (s->phase == WIFI_MONITOR_OFF || now < s->next_action_ms) return false;
    cerberus_wifi_packet_monitor_session_status_t r = {0};
    esp_err_t error;
    if (s->phase == WIFI_MONITOR_INFO) {
        error = t->info(&r);
        if (error != ESP_OK) {
            s->error = error;
            if (error == ESP_ERR_NOT_SUPPORTED) release(s);
            else s->next_action_ms = later(now, MONITOR_RETRY_MS);
        } else if (!r.session_id || r.session_id == UINT64_MAX ||
            (r.flags & CERBERUS_WIFI_PACKET_MONITOR_HELD)) {
            s->error = ESP_ERR_INVALID_STATE;
            s->next_action_ms = later(now, MONITOR_RETRY_MS);
        } else {
            s->session = r.session_id + 1U;
            s->error = ESP_OK;
            s->phase = WIFI_MONITOR_START;
        }
        return true;
    }
    if (s->phase == WIFI_MONITOR_START) {
        /* Claim before sending. A lost reply is followed by STATUS, never a
         * replay of START or a channel-based guess at the owner. */
        s->phase = WIFI_MONITOR_WATCH;
        error = t->start(s->session, s->channel, &r);
        s->status_due_ms = now;
        if (error == ESP_OK) accept(s, &r, true, now);
    } else if (s->phase == WIFI_MONITOR_STOP) {
        error = t->stop(s->session, &r);
        if (error == ESP_OK) accept(s, &r, false, now);
    } else if (now >= s->status_due_ms) {
        error = t->status(s->session, &r);
        if (error == ESP_OK) accept(s, &r, false, now);
    } else {
        if (!s->have_fetched || s->fetched == s->view.latest_sequence) return false;
        uint16_t sequence = (uint16_t)(s->fetched + 1U);
        cerberus_wifi_packet_monitor_session_sample_t reply = {0};
        error = t->sample(s->session, sequence, &reply);
        if (error == ESP_OK && (reply.session_id != s->session ||
            reply.sample.sequence != sequence || reply.sample.channel != s->channel ||
            !reply.sample.generation || (s->view.generation &&
                reply.sample.generation != s->view.generation) ||
            reply.sample.updated_ms > s->view.elapsed_ms)) error = ESP_ERR_INVALID_RESPONSE;
        if (error == ESP_OK) {
            s->view.generation = reply.sample.generation;
            s->samples[sequence % CERBERUS_WIFI_PACKET_MONITOR_SAMPLES] = reply.sample;
            s->fetched = sequence;
        }
    }
    if (error != ESP_OK) {
        s->error = error;
        s->next_action_ms = later(now, MONITOR_RETRY_MS);
    }
    return true;
}
