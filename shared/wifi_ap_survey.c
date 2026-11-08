#include "wifi_ap_survey.h"
#include "wifi_capture_plan.h"
#include "wifi_scan_observation.h"
#include <limits.h>
#include <string.h>

static bool zeros(const void *data, size_t length)
{
    const uint8_t *bytes = data;
    for (size_t i = 0; i < length; ++i) if (bytes[i]) return false;
    return true;
}

static uint32_t increment(cerberus_ap_survey_table_t *table, uint32_t counter)
{
    if (counter != UINT32_MAX) return counter + 1U;
    table->live.flags |= CERBERUS_AP_SURVEY_SNAPSHOT_SATURATED;
    if (table->live.saturations != UINT32_MAX) ++table->live.saturations;
    return counter;
}

static bool writable(const cerberus_ap_survey_table_t *table)
{
    return table && table->live.session_id &&
        !(table->live.flags & CERBERUS_AP_SURVEY_SNAPSHOT_EXHAUSTED);
}

static void exhausted(cerberus_ap_survey_table_t *table)
{
    if (table->accepting && table->latest_callback_us >= table->armed_us)
        table->live.receive_us += table->latest_callback_us - table->armed_us;
    table->live.flags |= CERBERUS_AP_SURVEY_SNAPSHOT_EXHAUSTED;
    table->accepting = false;
}

static void changed(cerberus_ap_survey_table_t *table)
{
    if (table->live.revision < UINT32_MAX) ++table->live.revision;
    if (table->live.revision == UINT32_MAX) {
        exhausted(table);
    }
}

bool cerberus_ap_survey_begin(cerberus_ap_survey_table_t *table,
    uint64_t session_id, uint64_t requested_channels, uint64_t effective_channels)
{
    if (!table) return false;
    memset(table, 0, sizeof(*table));
    if (!session_id || !cerberus_wifi_capture_plan_mask_valid(requested_channels) ||
        !cerberus_wifi_capture_plan_mask_valid(effective_channels) ||
        (effective_channels & ~requested_channels)) return false;
    table->live.session_id = session_id;
    table->live.requested_channels = requested_channels;
    table->live.effective_channels = effective_channels;
    table->live.revision = 1U;
    return true;
}

bool cerberus_ap_survey_note_channel(cerberus_ap_survey_table_t *table,
    uint8_t channel, bool applied)
{
    uint64_t bit = cerberus_wifi_capture_plan_channel_bit(channel);
    if (!writable(table) || table->accepting || !bit ||
        !(table->live.effective_channels & bit)) return false;
    table->live.attempted_channels |= bit;
    if (applied) table->live.applied_channels |= bit;
    else table->live.tune_failures = increment(table, table->live.tune_failures);
    changed(table);
    return true;
}

void cerberus_ap_survey_note_pass(cerberus_ap_survey_table_t *table)
{
    if (!writable(table)) return;
    table->live.completed_passes = increment(table, table->live.completed_passes);
    changed(table);
}

bool cerberus_ap_survey_arm(cerberus_ap_survey_table_t *table, uint64_t session_id,
    uint64_t arm_token, uint8_t channel, uint64_t source_us)
{
    uint64_t bit = cerberus_wifi_capture_plan_channel_bit(channel);
    if (!writable(table) || table->accepting || session_id != table->live.session_id ||
        !arm_token || arm_token <= table->arm_token || source_us > INT64_MAX ||
        source_us < table->latest_callback_us || source_us < table->control_us ||
        !(bit & table->live.applied_channels)) return false;
    table->arm_token = arm_token;
    table->armed_us = source_us;
    table->control_us = source_us;
    table->live.channel = channel;
    table->accepting = true;
    changed(table);
    return table->accepting;
}

bool cerberus_ap_survey_disarm(cerberus_ap_survey_table_t *table, uint64_t session_id,
    uint64_t arm_token, uint64_t source_us)
{
    if (!table || !session_id || session_id != table->live.session_id ||
        !arm_token || arm_token != table->arm_token) return false;
    bool valid = source_us <= INT64_MAX && source_us >= table->armed_us &&
        source_us >= table->latest_callback_us && source_us >= table->control_us;
    if (table->accepting && valid) table->live.receive_us += source_us - table->armed_us;
    if (valid) table->control_us = source_us;
    bool was_accepting = table->accepting;
    table->accepting = false;
    if (was_accepting && writable(table)) changed(table);
    return valid;
}

uint8_t cerberus_ap_survey_decode(const uint8_t *frame, size_t length,
    uint8_t channel, int rssi, uint64_t callback_us, cerberus_ap_survey_decoded_t *out)
{
    if (!out) return CERBERUS_AP_SURVEY_REJECTED;
    memset(out, 0, sizeof(*out));
    if (!frame || length < 2U || length > CERBERUS_AP_SURVEY_MAX_FRAME ||
        callback_us > INT64_MAX || !cerberus_wifi_relation_channel_valid(channel))
        return CERBERUS_AP_SURVEY_REJECTED;
    if (frame[0] == 0x80U || frame[0] == 0x50U) {
        cerberus_wifi_observation_frame_t decoded;
        if (!cerberus_wifi_observation_decode(frame, length, channel, &decoded))
            return CERBERUS_AP_SURVEY_REJECTED;
        cerberus_ap_survey_ap_t *ap = &out->ap;
        memcpy(ap->bssid, decoded.key.bssid, sizeof(ap->bssid));
        memcpy(ap->ssid, decoded.key.ssid, sizeof(ap->ssid));
        ap->ssid_length = decoded.key.ssid_length;
        ap->channel = channel;
        ap->flags = frame[0] == 0x80U ? CERBERUS_AP_SURVEY_AP_BEACON : CERBERUS_AP_SURVEY_AP_PROBE_RESPONSE;
        ap->rssi = CERBERUS_WIFI_RELATION_RSSI_UNAVAILABLE;
        if (cerberus_wifi_relation_rssi_valid(rssi)) {
            ap->rssi = (int8_t)rssi;
            ap->flags |= CERBERUS_AP_SURVEY_AP_RSSI_VALID;
        }
        ap->capabilities = (uint16_t)frame[34] | (uint16_t)((uint16_t)frame[35] << 8U);
        ap->frames = 1U;
        ap->first_callback_us = ap->last_callback_us = callback_us;
        out->kind = CERBERUS_AP_SURVEY_AP;
    } else if (cerberus_wifi_relation_decode(frame, length, channel, rssi, callback_us, &out->relation)) {
        out->kind = CERBERUS_AP_SURVEY_RELATION;
    }
    return out->kind;
}

bool cerberus_ap_survey_ap_valid(const cerberus_ap_survey_ap_t *ap, uint64_t source_us)
{
    if (!ap || !ap->incarnation || !ap->frames || source_us > INT64_MAX ||
        !cerberus_wifi_relation_mac_valid(ap->bssid) ||
        !cerberus_wifi_relation_channel_valid(ap->channel) || ap->ssid_length > 32U ||
        (ap->flags & ~7U) || !(ap->flags & 6U) ||
        !zeros(ap->ssid + ap->ssid_length, 32U - ap->ssid_length) ||
        ap->first_callback_us > ap->last_callback_us || ap->last_callback_us > source_us) return false;
    return ap->flags & CERBERUS_AP_SURVEY_AP_RSSI_VALID ?
        cerberus_wifi_relation_rssi_valid(ap->rssi) : ap->rssi == CERBERUS_WIFI_RELATION_RSSI_UNAVAILABLE;
}

static bool next_incarnation(cerberus_ap_survey_table_t *table, uint32_t *incarnation)
{
    if (table->incarnation_counter == UINT32_MAX) {
        exhausted(table);
        changed(table);
        return false;
    }
    *incarnation = ++table->incarnation_counter;
    return true;
}

bool cerberus_ap_survey_commit(cerberus_ap_survey_table_t *table, uint64_t session_id,
    uint64_t arm_token, uint8_t channel, uint64_t callback_us,
    const cerberus_ap_survey_decoded_t *decoded)
{
    if (!writable(table) || !table->accepting || !decoded ||
        session_id != table->live.session_id || arm_token != table->arm_token ||
        channel != table->live.channel || callback_us > INT64_MAX ||
        callback_us < table->armed_us) return false;
    if (callback_us < table->latest_callback_us) {
        table->live.stale_frames = increment(table, table->live.stale_frames);
        changed(table);
        return false;
    }
    cerberus_ap_survey_ap_t ap = decoded->ap;
    ap.incarnation = 1U;
    if (decoded->kind == CERBERUS_AP_SURVEY_AP &&
        (!cerberus_ap_survey_ap_valid(&ap, callback_us) || decoded->ap.incarnation ||
         ap.frames != 1U || ap.first_callback_us != callback_us || ap.last_callback_us != callback_us ||
         ap.channel != channel || !zeros(&decoded->relation, sizeof(decoded->relation)))) return false;
    const cerberus_wifi_relation_row_t *incoming = &decoded->relation;
    if (decoded->kind == CERBERUS_AP_SURVEY_RELATION &&
        (!cerberus_wifi_relation_row_valid(incoming) || incoming->channel != channel ||
         cerberus_wifi_relation_latest_us(incoming) != callback_us ||
         incoming->directions != incoming->latest_direction ||
         (uint64_t)incoming->to_ap_frames + incoming->from_ap_frames != 1U ||
         !zeros(&decoded->ap, sizeof(decoded->ap)))) return false;
    if (decoded->kind > CERBERUS_AP_SURVEY_RELATION ||
        (decoded->kind == CERBERUS_AP_SURVEY_REJECTED &&
         (!zeros(&decoded->ap, sizeof(decoded->ap)) || !zeros(incoming, sizeof(*incoming))))) return false;
    table->latest_callback_us = callback_us;
    if (decoded->kind == CERBERUS_AP_SURVEY_AP) {
        unsigned index = 0U;
        while (index < table->live.ap_count &&
            (table->live.aps[index].channel != channel || memcmp(table->live.aps[index].bssid, ap.bssid, 6U))) ++index;
        if (index == table->live.ap_count) {
            uint32_t incarnation;
            if (!next_incarnation(table, &incarnation)) return false;
            ap.incarnation = incarnation;
            if (index == CERBERUS_AP_SURVEY_AP_CAPACITY) {
                index = 0U;
                for (unsigned i = 1U; i < CERBERUS_AP_SURVEY_AP_CAPACITY; ++i)
                    if (table->live.aps[i].last_callback_us < table->live.aps[index].last_callback_us) index = i;
                table->live.ap_replacements = increment(table, table->live.ap_replacements);
            } else ++table->live.ap_count;
        } else {
            const cerberus_ap_survey_ap_t *old = &table->live.aps[index];
            ap.incarnation = old->incarnation;
            ap.first_callback_us = old->first_callback_us;
            ap.frames = old->frames;
            ap.frames = increment(table, ap.frames);
            ap.flags |= old->flags & 6U;
        }
        table->live.aps[index] = ap;
        table->live.ap_frames = increment(table, table->live.ap_frames);
    } else if (decoded->kind == CERBERUS_AP_SURVEY_RELATION) {
        unsigned index = 0U;
        while (index < table->live.relation_count &&
            (table->live.relations[index].row.channel != channel ||
             memcmp(table->live.relations[index].row.bssid, incoming->bssid, 6U) ||
             memcmp(table->live.relations[index].row.station, incoming->station, 6U))) ++index;
        if (index == table->live.relation_count) {
            uint32_t incarnation;
            if (!next_incarnation(table, &incarnation)) return false;
            if (index == CERBERUS_AP_SURVEY_RELATION_CAPACITY) {
                index = 0U;
                for (unsigned i = 1U; i < CERBERUS_AP_SURVEY_RELATION_CAPACITY; ++i)
                    if (cerberus_wifi_relation_latest_us(&table->live.relations[i].row) <
                        cerberus_wifi_relation_latest_us(&table->live.relations[index].row)) index = i;
                table->live.relation_replacements = increment(table, table->live.relation_replacements);
            } else ++table->live.relation_count;
            table->live.relations[index] = (cerberus_ap_survey_relation_t){.row = *incoming, .incarnation = incarnation};
        } else {
            cerberus_wifi_relation_row_t *row = &table->live.relations[index].row;
            uint8_t direction = incoming->latest_direction;
            row->directions |= direction;
            row->latest_direction = direction;
            row->rssi_valid = (uint8_t)((row->rssi_valid & ~direction) | incoming->rssi_valid);
            if (direction == CERBERUS_WIFI_RELATION_TO_AP) {
                row->to_ap_frames = increment(table, row->to_ap_frames);
                row->to_ap_callback_us = callback_us;
                row->to_ap_rssi = incoming->to_ap_rssi;
            } else {
                row->from_ap_frames = increment(table, row->from_ap_frames);
                row->from_ap_callback_us = callback_us;
                row->from_ap_rssi = incoming->from_ap_rssi;
            }
        }
        table->live.relation_frames = increment(table, table->live.relation_frames);
    } else table->live.rejected_frames = increment(table, table->live.rejected_frames);
    table->live.received_frames = increment(table, table->live.received_frames);
    changed(table);
    return decoded->kind != CERBERUS_AP_SURVEY_REJECTED;
}

bool cerberus_ap_survey_snapshot(const cerberus_ap_survey_table_t *table,
    uint64_t source_us, cerberus_ap_survey_snapshot_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!table || !table->live.session_id || source_us > INT64_MAX ||
        source_us < table->latest_callback_us || source_us < table->control_us ||
        (table->accepting && source_us < table->armed_us)) return false;
    *out = table->live;
    out->copied_source_us = source_us;
    if (table->accepting) {
        out->flags |= CERBERUS_AP_SURVEY_SNAPSHOT_RECEIVING;
        out->receive_us += source_us - table->armed_us;
    }
    return true;
}

bool cerberus_ap_survey_snapshot_valid(const cerberus_ap_survey_snapshot_t *snapshot)
{
    if (!snapshot || !snapshot->session_id || !snapshot->revision || snapshot->copied_source_us > INT64_MAX ||
        !cerberus_wifi_capture_plan_mask_valid(snapshot->requested_channels) ||
        !cerberus_wifi_capture_plan_mask_valid(snapshot->effective_channels) ||
        (snapshot->effective_channels & ~snapshot->requested_channels) ||
        (snapshot->attempted_channels & ~snapshot->effective_channels) ||
        (snapshot->applied_channels & ~snapshot->attempted_channels) ||
        (snapshot->flags & ~7U) || snapshot->receive_us > snapshot->copied_source_us ||
        (!!(snapshot->flags & CERBERUS_AP_SURVEY_SNAPSHOT_SATURATED) != !!snapshot->saturations) ||
        (snapshot->revision == UINT32_MAX && !(snapshot->flags & CERBERUS_AP_SURVEY_SNAPSHOT_EXHAUSTED)) ||
        snapshot->ap_count > CERBERUS_AP_SURVEY_AP_CAPACITY ||
        snapshot->relation_count > CERBERUS_AP_SURVEY_RELATION_CAPACITY ||
        (snapshot->channel && !(cerberus_wifi_capture_plan_channel_bit(snapshot->channel) & snapshot->applied_channels)) ||
        ((snapshot->flags & CERBERUS_AP_SURVEY_SNAPSHOT_RECEIVING) &&
         (!snapshot->channel || (snapshot->flags & CERBERUS_AP_SURVEY_SNAPSHOT_EXHAUSTED))) ||
        !zeros(snapshot->aps + snapshot->ap_count, sizeof(snapshot->aps) - snapshot->ap_count * sizeof(snapshot->aps[0])) ||
        !zeros(snapshot->relations + snapshot->relation_count,
            sizeof(snapshot->relations) - snapshot->relation_count * sizeof(snapshot->relations[0]))) return false;
    uint64_t ap_frames = 0U, relation_frames = 0U;
    for (unsigned i = 0U; i < snapshot->ap_count; ++i) {
        const cerberus_ap_survey_ap_t *ap = &snapshot->aps[i];
        if (!cerberus_ap_survey_ap_valid(ap, snapshot->copied_source_us) ||
            !(cerberus_wifi_capture_plan_channel_bit(ap->channel) & snapshot->applied_channels)) return false;
        ap_frames += ap->frames;
        for (unsigned j = 0U; j < i; ++j)
            if (ap->incarnation == snapshot->aps[j].incarnation ||
                (ap->channel == snapshot->aps[j].channel && !memcmp(ap->bssid, snapshot->aps[j].bssid, 6U))) return false;
    }
    for (unsigned i = 0U; i < snapshot->relation_count; ++i) {
        const cerberus_ap_survey_relation_t *relation = &snapshot->relations[i];
        if (!relation->incarnation || !cerberus_wifi_relation_row_valid(&relation->row) ||
            cerberus_wifi_relation_latest_us(&relation->row) > snapshot->copied_source_us ||
            !(cerberus_wifi_capture_plan_channel_bit(relation->row.channel) & snapshot->applied_channels)) return false;
        relation_frames += (uint64_t)relation->row.to_ap_frames + relation->row.from_ap_frames;
        for (unsigned j = 0U; j < snapshot->ap_count; ++j)
            if (relation->incarnation == snapshot->aps[j].incarnation) return false;
        for (unsigned j = 0U; j < i; ++j) {
            const cerberus_ap_survey_relation_t *other = &snapshot->relations[j];
            if (relation->incarnation == other->incarnation ||
                (relation->row.channel == other->row.channel &&
                 !memcmp(relation->row.bssid, other->row.bssid, 6U) &&
                 !memcmp(relation->row.station, other->row.station, 6U))) return false;
        }
    }
    if ((snapshot->ap_frames != UINT32_MAX && ap_frames > snapshot->ap_frames) ||
        (snapshot->relation_frames != UINT32_MAX && relation_frames > snapshot->relation_frames)) return false;
    uint64_t frames = (uint64_t)snapshot->ap_frames + snapshot->relation_frames + snapshot->rejected_frames;
    return snapshot->received_frames == UINT32_MAX ? frames >= UINT32_MAX : frames == snapshot->received_frames;
}
