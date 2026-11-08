#pragma once
#include "wifi_ap_survey_model.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>

#define CERBERUS_AP_SURVEY_REPORT_CAPACITY 65536U

static inline bool cerberus_ap_survey_report_append(char *out, size_t capacity,
    size_t *used, const char *format, ...)
{
    if (*used >= capacity) return false;
    va_list args; va_start(args, format);
    int length = vsnprintf(out + *used, capacity - *used, format, args);
    va_end(args);
    if (length < 0 || (size_t)length >= capacity - *used) return false;
    *used += (size_t)length;
    return true;
}

/* Local evidence report, not a GPS/WiGLE recording. SSIDs are exact hex bytes;
 * text, newlines and commas cannot alter the CSV shape. No RF or storage work. */
static inline size_t cerberus_ap_survey_report(const cerberus_ap_survey_model_t *model,
    char *out, size_t capacity)
{
    if (!model || !out || !capacity) return 0;
    size_t used = 0;
#define SURVEY_REPORT(...) do { if (!cerberus_ap_survey_report_append(out, capacity, &used, __VA_ARGS__)) return 0; } while (0)
    SURVEY_REPORT("# Cerberus passive AP/client survey; observations are not verified associations\r\n");
    for (unsigned i = 0; i < 2; ++i) if (model->banks[i].published) {
        const cerberus_ap_survey_snapshot_t *s = &model->banks[i].snapshot;
        SURVEY_REPORT("# %s requested=%" PRIx64 " effective=%" PRIx64 " attempted=%" PRIx64
            " applied=%" PRIx64 " rx_us=%" PRIu64 " passes=%" PRIu32
            " ap_replaced=%" PRIu32 " client_replaced=%" PRIu32 " flags=%u\r\n",
            i ? "C5" : "S3", s->requested_channels, s->effective_channels, s->attempted_channels,
            s->applied_channels, s->receive_us, s->completed_passes, s->ap_replacements,
            s->relation_replacements, s->flags);
    }
    SURVEY_REPORT("type,receiver,run,session,incarnation,bssid,station,channel,ssid_hex,ssid_bytes,"
        "ap_observations,sta_to_ap,ap_to_sta,ap_rssi,sta_rssi,downlink_rssi,"
        "first_ap_callback_us,last_ap_callback_us,sta_callback_us,downlink_callback_us,"
        "source_snapshot_us,request_started_us,receipt_us\r\n");
    for (unsigned kind = CERBERUS_AP_SURVEY_AP; kind <= CERBERUS_AP_SURVEY_RELATION; ++kind) {
        cerberus_ap_survey_page_t page;
        for (size_t offset = 0; offset < 64; offset += 4) {
            if (!cerberus_ap_survey_model_page(model, (uint8_t)kind, NULL, offset, &page)) return 0;
            for (unsigned i = 0; i < page.copied; ++i) {
                const cerberus_ap_survey_record_t *r = &page.rows[i];
                const uint8_t *mac = kind == CERBERUS_AP_SURVEY_AP ? r->evidence.ap.bssid : r->evidence.client.row.bssid;
                uint32_t incarnation = kind == CERBERUS_AP_SURVEY_AP ? r->evidence.ap.incarnation : r->evidence.client.incarnation;
                SURVEY_REPORT("%s,%s,%" PRIu64 ",%" PRIu64 ",%" PRIu32 ",%02X:%02X:%02X:%02X:%02X:%02X,",
                    kind == CERBERUS_AP_SURVEY_AP ? "AP" : "CLIENT", r->source == 1 ? "S3" : "C5",
                    r->run_id, r->session_id, incarnation, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
                if (kind == CERBERUS_AP_SURVEY_AP) {
                    const cerberus_ap_survey_ap_t *ap = &r->evidence.ap;
                    SURVEY_REPORT(",%u,", ap->channel);
                    for (unsigned byte = 0; byte < ap->ssid_length; ++byte) SURVEY_REPORT("%02X", ap->ssid[byte]);
                    SURVEY_REPORT(",%u,%" PRIu32 ",,,", ap->ssid_length, ap->frames);
                    if (ap->flags & CERBERUS_AP_SURVEY_AP_RSSI_VALID) SURVEY_REPORT("%d", ap->rssi);
                    SURVEY_REPORT(",,,%" PRIu64 ",%" PRIu64 ",,,", ap->first_callback_us, ap->last_callback_us);
                } else {
                    const cerberus_wifi_relation_row_t *client = &r->evidence.client.row;
                    mac = client->station;
                    SURVEY_REPORT("%02X:%02X:%02X:%02X:%02X:%02X,%u,,,,%" PRIu32 ",%" PRIu32 ",,",
                        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], client->channel,
                        client->to_ap_frames, client->from_ap_frames);
                    if (client->rssi_valid & 1U) SURVEY_REPORT("%d", client->to_ap_rssi);
                    SURVEY_REPORT(",");
                    if (client->rssi_valid & 2U) SURVEY_REPORT("%d", client->from_ap_rssi);
                    SURVEY_REPORT(",,,%" PRIu64 ",%" PRIu64 ",", client->to_ap_callback_us, client->from_ap_callback_us);
                }
                SURVEY_REPORT("%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\r\n",
                    r->copied_source_us, r->request_started_us, r->receipt_us);
            }
            if (offset + page.copied >= page.total) break;
        }
    }
#undef SURVEY_REPORT
    return used;
}
