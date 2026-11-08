#pragma once
#include "wifi_ap_survey_model.h"
#include "wifi_capture_plan.h"
#include <stdio.h>
#include <string.h>

static void survey_fixture_begin(cerberus_ap_survey_table_t *table, uint64_t session, uint8_t channel)
{
    uint64_t mask = cerberus_wifi_capture_plan_channel_bit(channel);
    CHECK(cerberus_ap_survey_begin(table, session, mask, mask));
    CHECK(cerberus_ap_survey_note_channel(table, channel, true));
    CHECK(cerberus_ap_survey_arm(table, session, 1, channel, 0));
}

static void survey_fixture_ap(cerberus_ap_survey_table_t *table, unsigned id, uint64_t at)
{
    cerberus_ap_survey_decoded_t decoded = {.kind = CERBERUS_AP_SURVEY_AP};
    decoded.ap = (cerberus_ap_survey_ap_t){.bssid = {2, 1, 2, 3, 4, (uint8_t)id},
        .channel = table->live.channel, .rssi = -42, .flags = 3, .frames = 1,
        .first_callback_us = at, .last_callback_us = at, .capabilities = 0x11};
    char name[33]; int length = snprintf(name, sizeof(name), "TEST-AP-%02u", id);
    CHECK(length > 0 && length <= 32);
    memcpy(decoded.ap.ssid, name, (size_t)length); decoded.ap.ssid_length = (uint8_t)length;
    CHECK(cerberus_ap_survey_commit(table, table->live.session_id, table->arm_token,
        table->live.channel, at, &decoded));
}

static void survey_fixture_client(cerberus_ap_survey_table_t *table, unsigned ap,
    unsigned station, uint8_t direction, uint64_t at)
{
    uint8_t frame[24] = {8, direction};
    uint8_t bssid[6] = {2, 1, 2, 3, 4, (uint8_t)ap};
    uint8_t sta[6] = {2, 5, 6, 7, 8, (uint8_t)station};
    memcpy(frame + (direction == 1 ? 4 : 10), bssid, 6);
    memcpy(frame + (direction == 1 ? 10 : 4), sta, 6);
    memcpy(frame + 16, bssid, 6);
    cerberus_ap_survey_decoded_t decoded;
    CHECK(cerberus_ap_survey_decode(frame, sizeof(frame), table->live.channel,
        direction == 1 ? -77 : -31, at, &decoded) == CERBERUS_AP_SURVEY_RELATION);
    CHECK(cerberus_ap_survey_commit(table, table->live.session_id, table->arm_token,
        table->live.channel, at, &decoded));
}

static void survey_fixture_publish(cerberus_ap_survey_model_t *model,
    cerberus_ap_survey_table_t *table, uint8_t source, uint64_t run,
    uint64_t at, uint64_t request)
{
    cerberus_ap_survey_snapshot_t snapshot;
    CHECK(cerberus_ap_survey_snapshot(table, at, &snapshot));
    cerberus_ap_survey_model_result_t result = cerberus_ap_survey_model_publish(model,
        source, run, request, request + 100U, &snapshot);
    CHECK(result == CERBERUS_AP_SURVEY_MODEL_APPLIED || result == CERBERUS_AP_SURVEY_MODEL_DUPLICATE);
}
