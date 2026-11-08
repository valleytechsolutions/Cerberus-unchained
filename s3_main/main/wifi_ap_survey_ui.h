#pragma once

/* Pure presentation and painted-identity hit testing. The caller owns this
 * bounded cache in PSRAM and performs all session/navigation operations. */
#include "wifi_ap_survey_model.h"
#include "display.h"
#include "privacy.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const char *state;
    uint32_t passes;
    uint64_t requested_channels, applied_channels;
    uint8_t channel;
    bool coverage_valid;
} cerberus_ap_survey_ui_receiver_t;

typedef struct {
    cerberus_ap_survey_page_t page;
    cerberus_ap_survey_selection_t selection;
    cerberus_ap_survey_ui_receiver_t receivers[2];
    uint64_t now_us;
    uint8_t ap_count, client_count, selected_client_count;
    bool detail, private_values, start, stop, back, save;
    const char *message, *back_label;
} cerberus_ap_survey_ui_view_t;

typedef enum {
    CERBERUS_AP_SURVEY_UI_NONE, CERBERUS_AP_SURVEY_UI_START,
    CERBERUS_AP_SURVEY_UI_STOP, CERBERUS_AP_SURVEY_UI_BACK,
    CERBERUS_AP_SURVEY_UI_APS, CERBERUS_AP_SURVEY_UI_CLIENTS,
    CERBERUS_AP_SURVEY_UI_SELECT, CERBERUS_AP_SURVEY_UI_PREVIOUS,
    CERBERUS_AP_SURVEY_UI_NEXT, CERBERUS_AP_SURVEY_UI_AP_CLIENTS,
    CERBERUS_AP_SURVEY_UI_SAVE,
} cerberus_ap_survey_ui_action_t;

typedef struct {
    cerberus_ap_survey_ui_action_t action;
    cerberus_ap_survey_record_t record;
} cerberus_ap_survey_ui_event_t;

typedef struct {
    char receivers[2][49], coverage[49], counts[49];
    char rows[4][3][49], details[12][49], message[49], back_label[25];
    bool detail, ap_detail, filtered, aps, start, stop, back, save, previous, next;
} cerberus_ap_survey_ui_render_t;

typedef struct {
    cerberus_ap_survey_ui_render_t painted, staging;
    cerberus_ap_survey_record_t rows[4], selected;
    uint8_t count;
    bool valid;
} cerberus_ap_survey_ui_cache_t;

static inline unsigned cerberus_ap_survey_ui_bits(uint64_t value)
{
    unsigned count = 0;
    while (value) { value &= value - 1U; ++count; }
    return count;
}

static inline void cerberus_ap_survey_ui_age(char out[49],
    const cerberus_ap_survey_record_t *record, uint8_t direction, uint64_t now_us)
{
    uint64_t age;
    if (cerberus_ap_survey_record_age(record, direction, now_us, &age))
        snprintf(out, 49, "CALLBACK AGE <=%" PRIu64 " S", age / 1000U + (age % 1000U != 0U));
    else snprintf(out, 49, "CALLBACK AGE UNAVAILABLE");
}

static inline void cerberus_ap_survey_ui_ssid(const cerberus_ap_survey_ap_t *ap,
    bool masked, char out[129])
{
    if (masked) { snprintf(out, 129, "[NAME HIDDEN]"); return; }
    if (!ap->ssid_length) { snprintf(out, 129, "[EMPTY SSID]"); return; }
    static const char hex[] = "0123456789ABCDEF";
    size_t used = 0;
    for (unsigned i = 0; i < ap->ssid_length && i < 32U; ++i) {
        uint8_t byte = ap->ssid[i];
        if (byte >= 32U && byte <= 126U && byte != '\\') out[used++] = (char)byte;
        else {
            out[used++] = '\\'; out[used++] = 'x';
            out[used++] = hex[byte >> 4U]; out[used++] = hex[byte & 15U];
        }
    }
    out[used] = '\0';
}

static inline const char *cerberus_ap_survey_ui_source(uint8_t source)
{
    return source == CERBERUS_AP_SURVEY_SOURCE_S3 ? "S3" : "C5";
}

static inline bool cerberus_ap_survey_ui_format(const cerberus_ap_survey_ui_view_t *view,
    cerberus_ap_survey_ui_render_t *out)
{
    if (!view || !out || view->page.copied > 4U || view->page.total > 64U ||
        view->page.offset > 64U || view->ap_count > 64U || view->client_count > 64U ||
        (view->detail && !view->selection.valid)) return false;
    memset(out, 0, sizeof(*out));
    out->detail = view->detail;
    out->ap_detail = view->detail && view->selection.frozen.kind == CERBERUS_AP_SURVEY_AP;
    out->aps = view->page.kind == CERBERUS_AP_SURVEY_AP;
    out->filtered = view->page.filtered;
    out->start = view->start; out->stop = view->stop; out->back = view->back; out->save = view->save;
    out->previous = !view->detail && view->page.offset != 0U;
    out->next = !view->detail && (unsigned)view->page.offset + view->page.copied < view->page.total;
    for (unsigned i = 0; i < 2U; ++i) {
        const cerberus_ap_survey_ui_receiver_t *r = &view->receivers[i];
        snprintf(out->receivers[i], 49, "%s %.16s / CH%u / PASS %" PRIu32,
            i ? "C5" : "S3", r->state ? r->state : "UNKNOWN", r->channel, r->passes);
    }
    char s3[12], c5[12];
    for (unsigned i = 0; i < 2U; ++i) {
        const cerberus_ap_survey_ui_receiver_t *r = &view->receivers[i];
        char *dest = i ? c5 : s3;
        if (r->coverage_valid) snprintf(dest, 12, "%u/%u", cerberus_ap_survey_ui_bits(r->applied_channels),
            cerberus_ap_survey_ui_bits(r->requested_channels));
        else snprintf(dest, 12, "UNKNOWN");
    }
    snprintf(out->coverage, 49, "COVERAGE S3 %.11s / C5 %.11s", s3, c5);
    snprintf(out->counts, 49, "APS %u / OBSERVED CLIENT LINKS %u", view->ap_count, view->client_count);
    snprintf(out->message, 49, "%.48s", view->message && view->message[0] ? view->message :
        "PASSIVE HEADERS / ASSOCIATION NOT VERIFIED");
    snprintf(out->back_label, 25, "%.24s", view->back_label ? view->back_label : "BACK");
    if (!view->detail) {
        for (unsigned i = 0; i < view->page.copied; ++i) {
            const cerberus_ap_survey_record_t *row = &view->page.rows[i];
            char mac[24];
            if (row->kind == CERBERUS_AP_SURVEY_AP) {
                const cerberus_ap_survey_ap_t *ap = &row->evidence.ap;
                char name[129]; cerberus_ap_survey_ui_ssid(ap, view->private_values, name);
                snprintf(out->rows[i][0], 49, "%.24s", name);
                cerberus_privacy_format_wifi_identifier(ap->bssid, view->private_values, mac, sizeof(mac));
                if (ap->flags & CERBERUS_AP_SURVEY_AP_RSSI_VALID)
                    snprintf(out->rows[i][1], 49, "%s CH%u / %s / %d DBM", cerberus_ap_survey_ui_source(row->source), ap->channel, mac, ap->rssi);
                else snprintf(out->rows[i][1], 49, "%s CH%u / %s / RSSI N/A", cerberus_ap_survey_ui_source(row->source), ap->channel, mac);
                cerberus_ap_survey_ui_age(out->rows[i][2], row, 0U, view->now_us);
            } else {
                const cerberus_wifi_relation_row_t *client = &row->evidence.client.row;
                cerberus_privacy_format_wifi_identifier(client->station, view->private_values, mac, sizeof(mac));
                snprintf(out->rows[i][0], 49, "%s", mac);
                cerberus_privacy_format_wifi_identifier(client->bssid, view->private_values, mac, sizeof(mac));
                snprintf(out->rows[i][1], 49, "%s CH%u / AP %s", cerberus_ap_survey_ui_source(row->source), client->channel, mac);
                snprintf(out->rows[i][2], 49, "STA>AP %" PRIu32 " / AP>STA %" PRIu32,
                    client->to_ap_frames, client->from_ap_frames);
            }
        }
        if (!view->page.copied) snprintf(out->rows[0][0], 49, "%s",
            view->page.filtered ? "NO MATCHING CLIENT LINKS" : out->aps ? "NO AP OBSERVATIONS" : "NO CLIENT OBSERVATIONS");
        return true;
    }
    const cerberus_ap_survey_record_t *row = &view->selection.latest;
    if (!cerberus_ap_survey_record_equal(&view->selection.frozen, row)) row = &view->selection.frozen;
    char mac[24];
    if (out->ap_detail) {
        const cerberus_ap_survey_ap_t *ap = &row->evidence.ap;
        char name[129]; cerberus_ap_survey_ui_ssid(ap, view->private_values, name);
        size_t length = strlen(name);
        snprintf(out->details[0], 49, "SSID / %u BYTES", ap->ssid_length);
        for (unsigned i = 0; i < 3U; ++i)
            if (i * 43U < length) snprintf(out->details[i + 1U], 49, "%.43s", name + i * 43U);
        cerberus_privacy_format_wifi_identifier(ap->bssid, view->private_values, mac, sizeof(mac));
        snprintf(out->details[4], 49, "BSSID %s", mac);
        snprintf(out->details[5], 49, "%s CH%u / OBSERVATIONS %" PRIu32,
            cerberus_ap_survey_ui_source(row->source), ap->channel, ap->frames);
        if (ap->flags & CERBERUS_AP_SURVEY_AP_RSSI_VALID) snprintf(out->details[6], 49, "LAST SIGNAL %d DBM", ap->rssi);
        else snprintf(out->details[6], 49, "SIGNAL UNAVAILABLE");
        cerberus_ap_survey_ui_age(out->details[7], row, 0U, view->now_us);
        snprintf(out->details[8], 49, "BEACON %s / PROBE RESPONSE %s",
            ap->flags & CERBERUS_AP_SURVEY_AP_BEACON ? "YES" : "NO",
            ap->flags & CERBERUS_AP_SURVEY_AP_PROBE_RESPONSE ? "YES" : "NO");
        snprintf(out->details[9], 49, "ADVERTISED CAPABILITIES 0x%04X", ap->capabilities);
        snprintf(out->details[10], 49, "%u CLIENT LINKS / SAME AP AND CHANNEL", view->selected_client_count);
    } else {
        const cerberus_wifi_relation_row_t *client = &row->evidence.client.row;
        cerberus_privacy_format_wifi_identifier(client->bssid, view->private_values, mac, sizeof(mac));
        snprintf(out->details[0], 49, "AP %s", mac);
        cerberus_privacy_format_wifi_identifier(client->station, view->private_values, mac, sizeof(mac));
        snprintf(out->details[1], 49, "STA %s", mac);
        snprintf(out->details[2], 49, "%s / RX CHANNEL %u", cerberus_ap_survey_ui_source(row->source), client->channel);
        for (unsigned i = 0; i < 2U; ++i) {
            uint8_t direction = (uint8_t)(1U << i);
            unsigned line = 3U + i * 3U;
            uint32_t frames = i ? client->from_ap_frames : client->to_ap_frames;
            snprintf(out->details[line], 49, "%s / %" PRIu32 " OBSERVATIONS", i ? "AP > STA" : "STA > AP", frames);
            if (!(client->directions & direction)) snprintf(out->details[line + 1U], 49, "NOT OBSERVED");
            else if (!(client->rssi_valid & direction)) snprintf(out->details[line + 1U], 49, "SIGNAL UNAVAILABLE");
            else snprintf(out->details[line + 1U], 49, "%s SIGNAL %d DBM", i ? "AP" : "STATION", i ? client->from_ap_rssi : client->to_ap_rssi);
            cerberus_ap_survey_ui_age(out->details[line + 2U], row, direction, view->now_us);
        }
        snprintf(out->details[9], 49, "COUNTS INCLUDE REPEAT OBSERVATIONS");
        snprintf(out->details[10], 49, "ADDRESS HEADERS / NO PAYLOAD INSPECTION");
    }
    snprintf(out->details[11], 49, "%s", view->selection.unavailable ? "RECEIVER UNAVAILABLE / RETAINED RECORD" :
        view->selection.present ? "LATEST MATCH / FROZEN SOURCE IDENTITY" : "NO LONGER RETAINED / FROZEN RECORD");
    return true;
}

static inline esp_err_t cerberus_ap_survey_ui_text(uint16_t x, uint16_t y,
    const char *text, uint16_t color, uint16_t background, uint8_t scale)
{
    return cerberus_display_draw_text(x, y, text, color, background, scale);
}

static inline esp_err_t cerberus_ap_survey_ui_button(uint16_t x, uint16_t y,
    uint16_t width, uint16_t height, const char *text, bool enabled)
{
    esp_err_t error = cerberus_display_fill_rect(x, y, width, height, 0x0841);
    if (error != ESP_OK) return error;
    size_t length = strlen(text);
    uint16_t left = (uint16_t)(x + (width - length * 6U) / 2U);
    return cerberus_ap_survey_ui_text(left, (uint16_t)(y + (height - 7U) / 2U), text,
        enabled ? 0xFFFF : 0x8410, 0x0841, 1);
}

static inline esp_err_t cerberus_ap_survey_ui_update(cerberus_ap_survey_ui_cache_t *cache,
    const cerberus_ap_survey_ui_view_t *view, bool force)
{
    if (!cache || !cerberus_ap_survey_ui_format(view, &cache->staging)) {
        if (cache) cache->valid = false;
        return ESP_ERR_INVALID_ARG;
    }
    const cerberus_ap_survey_ui_render_t *next = &cache->staging;
    const cerberus_ap_survey_ui_render_t *old = &cache->painted;
    bool changed = force || !cache->valid || memcmp(next, &cache->painted, sizeof(*next));
    bool identity_changed[4] = {false};
    for (unsigned i = 0; i < view->page.copied; ++i) {
        identity_changed[i] = i >= cache->count ||
            !cerberus_ap_survey_record_equal(&cache->rows[i], &view->page.rows[i]);
        changed = changed || identity_changed[i];
    }
    bool selected_changed = view->detail &&
        !cerberus_ap_survey_record_equal(&cache->selected, &view->selection.frozen);
    changed = changed || selected_changed;
    if (!changed) {
        memcpy(cache->rows, view->page.rows, sizeof(cache->rows));
        cache->selected = view->selection.frozen;
        cache->count = view->page.copied;
        return ESP_OK;
    }
    esp_err_t error;
    force = force || !cache->valid || old->detail != next->detail || old->ap_detail != next->ap_detail;
#define AP_UI_TRY(call) do { error = (call); if (error != ESP_OK) { cache->valid = false; return error; } } while (0)
    if (force) AP_UI_TRY(cerberus_display_fill_rect(0, 0, 320, 480, 0x0000));
    if (force || memcmp(old->receivers, next->receivers, sizeof(next->receivers)) ||
        strcmp(old->coverage, next->coverage) || strcmp(old->counts, next->counts)) {
    AP_UI_TRY(cerberus_display_fill_rect(0, 0, 320, 120, 0x0000));
    AP_UI_TRY(cerberus_ap_survey_ui_text(34, 16, "AP / CLIENT SURVEY", 0xFFFF, 0x0000, 2));
    AP_UI_TRY(cerberus_ap_survey_ui_text(14, 38, "PASSIVE / S3 2.4 GHZ + C5 5 GHZ", 0x8410, 0x0000, 1));
    for (unsigned i = 0; i < 2U; ++i)
        AP_UI_TRY(cerberus_ap_survey_ui_text(14, (uint16_t)(59U + i * 15U), next->receivers[i], i ? 0xBDBF : 0x2EFD, 0x0000, 1));
    AP_UI_TRY(cerberus_ap_survey_ui_text(14, 89, next->coverage, 0x8410, 0x0000, 1));
    AP_UI_TRY(cerberus_ap_survey_ui_text(14, 106, next->counts, 0xFFFF, 0x0000, 1));
    }
    if (!next->detail) {
        if (force || old->filtered != next->filtered || old->aps != next->aps) {
        AP_UI_TRY(cerberus_ap_survey_ui_button(8, 124, 148, 34, "APS", true));
        AP_UI_TRY(cerberus_ap_survey_ui_button(164, 124, 148, 34, next->filtered ? "THIS AP'S CLIENTS" : "CLIENTS", true));
        AP_UI_TRY(cerberus_display_fill_rect(next->aps ? 8 : 164, 156, 148, 2, 0x2EFD));
        }
        for (unsigned i = 0; i < 4U; ++i) if (force || identity_changed[i] || memcmp(old->rows[i], next->rows[i], sizeof(next->rows[i]))) {
            uint16_t y = (uint16_t)(170U + i * 52U);
            AP_UI_TRY(cerberus_display_fill_rect(8, y, 304, 46, 0x0841));
            AP_UI_TRY(cerberus_ap_survey_ui_text(14, y + 3U, next->rows[i][0], 0xFFFF, 0x0841, 2));
            AP_UI_TRY(cerberus_ap_survey_ui_text(14, y + 22U, next->rows[i][1], 0x8410, 0x0841, 1));
            AP_UI_TRY(cerberus_ap_survey_ui_text(14, y + 34U, next->rows[i][2], 0x8410, 0x0841, 1));
        }
        if (force || old->previous != next->previous || old->next != next->next || old->save != next->save) {
        AP_UI_TRY(cerberus_ap_survey_ui_button(8, 398, 90, 26, "PREVIOUS", next->previous));
        AP_UI_TRY(cerberus_ap_survey_ui_button(106, 398, 90, 26, "NEXT", next->next));
        AP_UI_TRY(cerberus_ap_survey_ui_button(204, 398, 108, 26, "SAVE REPORT", next->save));
        }
    } else {
        if (force || selected_changed || memcmp(old->details, next->details, sizeof(next->details))) {
        AP_UI_TRY(cerberus_display_fill_rect(8, 124, 304, 248, 0x0000));
        for (unsigned i = 0; i < 12U; ++i)
            AP_UI_TRY(cerberus_ap_survey_ui_text(14, (uint16_t)(134U + i * 21U), next->details[i],
                i == 3U || i == 6U ? 0x2EFD : 0xFFFF, 0x0000, 1));
        }
        if (force && next->ap_detail)
            AP_UI_TRY(cerberus_ap_survey_ui_button(8, 398, 304, 26, "CLIENTS FOR THIS AP", true));
    }
    if (force || strcmp(old->message, next->message)) {
    AP_UI_TRY(cerberus_display_fill_rect(8, 378, 304, 15, 0x0000));
    AP_UI_TRY(cerberus_ap_survey_ui_text(14, 383, next->message, 0x8410, 0x0000, 1));
    }
    if (force || old->back != next->back || old->start != next->start || old->stop != next->stop ||
        strcmp(old->back_label, next->back_label)) {
    AP_UI_TRY(cerberus_ap_survey_ui_button(8, 432, 148, 42, next->back_label, next->back));
    AP_UI_TRY(cerberus_ap_survey_ui_button(164, 432, 148, 42, next->stop ? "STOP" : "START", next->stop || next->start));
    }
#undef AP_UI_TRY
    cache->painted = *next;
    memcpy(cache->rows, view->page.rows, sizeof(cache->rows));
    cache->selected = view->selection.frozen;
    cache->count = view->page.copied;
    cache->valid = true;
    return ESP_OK;
}

static inline cerberus_ap_survey_ui_event_t cerberus_ap_survey_ui_tap(
    const cerberus_ap_survey_ui_cache_t *cache, uint16_t x, uint16_t y)
{
    cerberus_ap_survey_ui_event_t event = {0};
    if (!cache || !cache->valid) return event;
    const cerberus_ap_survey_ui_render_t *p = &cache->painted;
    if (y >= 432U && y < 474U) {
        if (x >= 8U && x < 156U && p->back) event.action = CERBERUS_AP_SURVEY_UI_BACK;
        else if (x >= 164U && x < 312U && (p->stop || p->start))
            event.action = p->stop ? CERBERUS_AP_SURVEY_UI_STOP : CERBERUS_AP_SURVEY_UI_START;
    } else if (p->detail) {
        if (p->ap_detail && x >= 8U && x < 312U && y >= 398U && y < 424U) {
            event.action = CERBERUS_AP_SURVEY_UI_AP_CLIENTS;
            event.record = cache->selected;
        }
    } else if (y >= 124U && y < 158U) {
        if (x >= 8U && x < 156U) event.action = CERBERUS_AP_SURVEY_UI_APS;
        else if (x >= 164U && x < 312U) event.action = CERBERUS_AP_SURVEY_UI_CLIENTS;
    } else if (x >= 8U && x < 312U && y >= 170U && y < 372U && (y - 170U) % 52U < 46U) {
        unsigned row = (y - 170U) / 52U;
        if (row < cache->count) { event.action = CERBERUS_AP_SURVEY_UI_SELECT; event.record = cache->rows[row]; }
    } else if (y >= 398U && y < 424U) {
        if (x >= 8U && x < 98U && p->previous) event.action = CERBERUS_AP_SURVEY_UI_PREVIOUS;
        else if (x >= 106U && x < 196U && p->next) event.action = CERBERUS_AP_SURVEY_UI_NEXT;
        else if (x >= 204U && x < 312U && p->save) event.action = CERBERUS_AP_SURVEY_UI_SAVE;
    }
    return event;
}
