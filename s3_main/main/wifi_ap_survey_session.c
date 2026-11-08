#include "wifi_ap_survey_session.h"
#include <limits.h>
#include <string.h>

#define STATUS_US UINT64_C(500000)
#define RENEW_US UINT64_C(3000000)
#define SNAPSHOT_US UINT64_C(1000000)
#define RETRY_US UINT64_C(250000)
#define READ_LIMIT_US UINT64_C(15000000)

void cerberus_ap_survey_session_init(cerberus_ap_survey_session_t *session)
{ if (session) memset(session, 0, sizeof(*session)); }

bool cerberus_ap_survey_session_released(const cerberus_ap_survey_session_t *session)
{
    if (!session) return true;
    for (unsigned i = 0; i < 2U; ++i) {
        const cerberus_ap_survey_radio_session_t *radio = &session->radios[i];
        if (radio->request.session_id || radio->requested || radio->reading ||
            radio->cancel_pending || radio->phase != CERBERUS_AP_SURVEY_SESSION_IDLE) return false;
    }
    return true;
}

bool cerberus_ap_survey_session_running(const cerberus_ap_survey_session_t *session)
{ return session && (session->radios[0].requested || session->radios[1].requested); }

bool cerberus_ap_survey_session_start(cerberus_ap_survey_session_t *session,
    uint64_t run_id, uint64_t s3_channels, uint64_t c5_channels)
{
    const uint64_t low_band = (UINT64_C(1) << 14U) - 1U;
    if (!session || !run_id || run_id <= session->run_id ||
        !cerberus_ap_survey_session_released(session) || (s3_channels & ~low_band) ||
        (c5_channels & low_band)) return false;
    uint64_t masks[2] = {s3_channels, c5_channels};
    for (unsigned i = 0U; i < 2U; ++i) {
        cerberus_ap_survey_start_t request = {.session_id = 1U,
            .channel_mask = masks[i], .dwell_ms = CERBERUS_AP_SURVEY_DWELL_MS};
        if (!cerberus_ap_survey_start_valid(&request)) return false;
    }
    memset(session->radios, 0, sizeof(session->radios));
    session->run_id = run_id;
    session->cursor = 0U;
    session->clock_valid = false;
    for (unsigned i = 0U; i < 2U; ++i) {
        cerberus_ap_survey_radio_session_t *radio = &session->radios[i];
        radio->request.channel_mask = masks[i];
        radio->request.dwell_ms = CERBERUS_AP_SURVEY_DWELL_MS;
        radio->requested = true;
        radio->phase = CERBERUS_AP_SURVEY_SESSION_INFO;
    }
    return true;
}

void cerberus_ap_survey_session_stop(cerberus_ap_survey_session_t *session)
{
    if (!session) return;
    for (unsigned i = 0U; i < 2U; ++i) {
        cerberus_ap_survey_radio_session_t *radio = &session->radios[i];
        radio->requested = false;
        radio->stop_requested = true;
        radio->next_action_us = 0U;
        if (radio->reading) radio->cancel_pending = true;
    }
}

void cerberus_ap_survey_session_close(cerberus_ap_survey_session_t *session)
{
    if (!session) return;
    cerberus_ap_survey_session_stop(session);
    for (unsigned i = 0U; i < 2U; ++i) session->radios[i].close_requested = true;
}

static void unavailable(cerberus_ap_survey_radio_session_t *radio, unsigned slot,
    bool value, const cerberus_ap_survey_session_ops_t *ops)
{
    if (radio->unavailable == value) return;
    radio->unavailable = value;
    if (ops->unavailable) ops->unavailable(ops->context, (uint8_t)(slot + 1U), value);
}

static void cancel(cerberus_ap_survey_radio_session_t *radio, unsigned slot,
    const cerberus_ap_survey_session_ops_t *ops)
{
    if (radio->reading || radio->cancel_pending)
        ops->snapshot_cancel(ops->context, (uint8_t)(slot + 1U));
    radio->reading = radio->cancel_pending = false;
}

static void released(cerberus_ap_survey_radio_session_t *radio, unsigned slot,
    const cerberus_ap_survey_session_ops_t *ops)
{
    cancel(radio, slot, ops);
    radio->request.session_id = 0U;
    radio->phase = CERBERUS_AP_SURVEY_SESSION_IDLE;
    radio->requested = radio->stop_requested = radio->close_requested = false;
    radio->start_uncertain = false;
}

static void retain_control(cerberus_ap_survey_radio_session_t *radio,
    const cerberus_ap_survey_control_t *reply, uint64_t now)
{
    radio->remote = *reply;
    radio->have_control = true;
    radio->error = reply->error_code;
    radio->next_status_us = now + STATUS_US;
    if ((reply->flags & CERBERUS_AP_SURVEY_BOUND) && !(reply->flags & CERBERUS_AP_SURVEY_HELD)) {
        radio->requested = false;
        radio->stop_requested = true;
    }
}

static void control(cerberus_ap_survey_radio_session_t *radio, unsigned slot,
    uint8_t command, uint64_t now, const cerberus_ap_survey_session_ops_t *ops)
{
    cerberus_ap_survey_request_t request = {.session_id = radio->request.session_id};
    bool info = command == CERBERUS_AP_SURVEY_COMMAND_INFO;
    const void *query = info ? NULL : command == CERBERUS_AP_SURVEY_COMMAND_START ?
        (const void *)&radio->request : (const void *)&request;
    size_t bytes = info ? 0U : command == CERBERUS_AP_SURVEY_COMMAND_START ?
        sizeof(radio->request) : sizeof(request);
    cerberus_ap_survey_control_t reply = {0};
    if (command == CERBERUS_AP_SURVEY_COMMAND_START && radio->start_attempts < UINT8_MAX)
        ++radio->start_attempts;
    int32_t error = ops->exchange(ops->context, (uint8_t)(slot + 1U), command, query, bytes, &reply);
    radio->next_action_us = now + RETRY_US;
    if (!cerberus_ap_survey_control_valid(&reply, info, request.session_id)) {
        radio->error = error ? error : CERBERUS_AP_SURVEY_SESSION_INVALID;
        if (info) {
            unavailable(radio, slot, true, ops);
            released(radio, slot, ops);
        } else if (command == CERBERUS_AP_SURVEY_COMMAND_START) {
            radio->start_uncertain = true;
            radio->phase = CERBERUS_AP_SURVEY_SESSION_ACTIVE;
            radio->next_status_us = 0U;
        }
        return;
    }
    radio->next_action_us = now;
    if (info) {
        unavailable(radio, slot, true, ops);
        radio->remote = reply;
        radio->have_control = true;
        if (reply.error_code || reply.flags || reply.session_id == UINT64_MAX) {
            radio->error = reply.error_code ? reply.error_code : CERBERUS_AP_SURVEY_SESSION_BUSY;
            released(radio, slot, ops);
            return;
        }
        radio->request.session_id = reply.session_id + 1U;
        radio->phase = CERBERUS_AP_SURVEY_SESSION_START;
        radio->have_control = false;
        radio->next_action_us = now;
        return;
    }
    if (!(reply.flags & CERBERUS_AP_SURVEY_BOUND)) {
        radio->remote = reply;
        radio->have_control = true;
        if (command == CERBERUS_AP_SURVEY_COMMAND_STATUS && radio->start_uncertain &&
            radio->requested && !radio->stop_requested && !radio->close_requested && radio->start_attempts < 3U) {
            radio->phase = CERBERUS_AP_SURVEY_SESSION_START;
            return;
        }
        radio->error = command == CERBERUS_AP_SURVEY_COMMAND_RELEASE && !reply.error_code ?
            radio->error : reply.error_code ? reply.error_code : CERBERUS_AP_SURVEY_SESSION_INVALID;
        if (command != CERBERUS_AP_SURVEY_COMMAND_RELEASE || reply.error_code)
            unavailable(radio, slot, true, ops);
        released(radio, slot, ops);
        return;
    }
    retain_control(radio, &reply, now);
    if (command == CERBERUS_AP_SURVEY_COMMAND_START || radio->start_uncertain) {
        radio->phase = CERBERUS_AP_SURVEY_SESSION_ACTIVE;
        radio->start_uncertain = false;
        radio->next_renew_us = now + RENEW_US;
        radio->next_snapshot_us = now;
    }
    if (command == CERBERUS_AP_SURVEY_COMMAND_RENEW) radio->next_renew_us = now + RENEW_US;
    if (command == CERBERUS_AP_SURVEY_COMMAND_STOP || command == CERBERUS_AP_SURVEY_COMMAND_RELEASE) {
        radio->next_action_us = now + RETRY_US;
        radio->next_status_us = now + RETRY_US;
    }
}

static void snapshot(cerberus_ap_survey_session_t *session, unsigned slot,
    uint64_t now, const cerberus_ap_survey_session_ops_t *ops)
{
    cerberus_ap_survey_radio_session_t *radio = &session->radios[slot];
    if (!radio->reading) {
        memset(&radio->staging, 0, sizeof(radio->staging));
        radio->read_started_us = now;
        radio->reading = true;
    }
    bool complete = false;
    int32_t error = now < radio->read_started_us || now - radio->read_started_us >= READ_LIMIT_US ?
        CERBERUS_AP_SURVEY_SESSION_TIMEOUT : ops->snapshot_step(ops->context, (uint8_t)(slot + 1U),
            radio->request.session_id, &radio->staging, &complete, &radio->request_started_us, &radio->receipt_us);
    if (!error && complete) {
        bool valid = radio->staging.session_id == radio->request.session_id &&
            radio->staging.requested_channels == radio->request.channel_mask &&
            radio->request_started_us >= radio->read_started_us &&
            radio->receipt_us >= radio->request_started_us && radio->receipt_us <= INT64_MAX &&
            cerberus_ap_survey_snapshot_valid(&radio->staging);
        if (!valid || !ops->publish(ops->context, (uint8_t)(slot + 1U), session->run_id,
            radio->request_started_us, radio->receipt_us, &radio->staging))
            error = CERBERUS_AP_SURVEY_SESSION_INVALID;
        else {
            if (radio->published != UINT32_MAX) ++radio->published;
            unavailable(radio, slot, false, ops);
        }
    }
    if (error || complete) {
        cancel(radio, slot, ops);
        radio->read_error = error;
        radio->next_snapshot_us = now + SNAPSHOT_US;
        if (radio->stop_requested) {
            radio->final_attempted = true;
            radio->close_requested = true;
            radio->next_action_us = 0U;
        }
    }
}

static bool service(cerberus_ap_survey_session_t *session, unsigned slot,
    unsigned priority, uint64_t now, const cerberus_ap_survey_session_ops_t *ops)
{
    cerberus_ap_survey_radio_session_t *radio = &session->radios[slot];
    if (now < radio->next_action_us || radio->phase == CERBERUS_AP_SURVEY_SESSION_IDLE) return false;
    bool held = radio->have_control && (radio->remote.flags & CERBERUS_AP_SURVEY_HELD);
    if (priority == 0U) {
        if (radio->close_requested) {
            control(radio, slot, CERBERUS_AP_SURVEY_COMMAND_RELEASE, now, ops);
            return true;
        }
        if (radio->stop_requested && (held || !radio->have_control || radio->start_uncertain)) {
            control(radio, slot, CERBERUS_AP_SURVEY_COMMAND_STOP, now, ops);
            return true;
        }
        return false;
    }
    if (priority == 1U) {
        if (radio->requested && held && !radio->stop_requested && now >= radio->next_renew_us) {
            control(radio, slot, CERBERUS_AP_SURVEY_COMMAND_RENEW, now, ops);
            return true;
        }
        return false;
    }
    if (radio->phase == CERBERUS_AP_SURVEY_SESSION_INFO) {
        control(radio, slot, CERBERUS_AP_SURVEY_COMMAND_INFO, now, ops);
        return true;
    }
    if (radio->phase == CERBERUS_AP_SURVEY_SESSION_START) {
        control(radio, slot, CERBERUS_AP_SURVEY_COMMAND_START, now, ops);
        return true;
    }
    if (radio->start_uncertain || now >= radio->next_status_us) {
        control(radio, slot, CERBERUS_AP_SURVEY_COMMAND_STATUS, now, ops);
        return true;
    }
    if (radio->stop_requested && !held) {
        if (!radio->final_attempted && (radio->remote.flags & CERBERUS_AP_SURVEY_DATA_READY)) {
            snapshot(session, slot, now, ops);
            return true;
        }
        radio->close_requested = true;
        radio->next_action_us = 0U;
        return false;
    }
    if (radio->requested && (radio->remote.flags & CERBERUS_AP_SURVEY_DATA_READY) &&
        (radio->reading || now >= radio->next_snapshot_us)) {
        snapshot(session, slot, now, ops);
        return true;
    }
    return false;
}

void cerberus_ap_survey_session_tick(cerberus_ap_survey_session_t *session,
    uint64_t now, const cerberus_ap_survey_session_ops_t *ops)
{
    if (!session || !ops || !ops->exchange || !ops->snapshot_step ||
        !ops->snapshot_cancel || !ops->publish) return;
    if (now > INT64_MAX - READ_LIMIT_US || (session->clock_valid && now < session->clock_us)) {
        cerberus_ap_survey_session_close(session);
        for (unsigned i = 0U; i < 2U; ++i) {
            session->radios[i].error = CERBERUS_AP_SURVEY_SESSION_CLOCK;
            session->radios[i].read_error = CERBERUS_AP_SURVEY_SESSION_CLOCK;
        }
    }
    session->clock_us = now;
    session->clock_valid = true;
    for (unsigned i = 0U; i < 2U; ++i) {
        cerberus_ap_survey_radio_session_t *radio = &session->radios[i];
        if (radio->cancel_pending) cancel(radio, i, ops);
        if (!radio->requested && (!radio->request.session_id ||
            (radio->phase == CERBERUS_AP_SURVEY_SESSION_START && !radio->start_attempts))) released(radio, i, ops);
    }
    for (unsigned priority = 0U; priority < 3U; ++priority) {
        for (unsigned offset = 0U; offset < 2U; ++offset) {
            unsigned slot = (session->cursor + offset) % 2U;
            if (service(session, slot, priority, now, ops)) {
                session->cursor = (uint8_t)((slot + 1U) % 2U);
                return;
            }
        }
    }
}
