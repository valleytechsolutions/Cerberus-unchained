#pragma once
#include "wifi_archive.h"
#include <stdio.h>
#include <string.h>

/* Independent mixed-source seed for production worker/storage round trips.
 * Duplicate AP identities across receivers remain two observations. */
static void archive_test_wifi_document(cerberus_wifi_archive_t *d, unsigned count)
{
    memset(d, 0, sizeof(*d));
    unsigned s3 = count < 32 ? count : 32;
    d->publication = (cerberus_wifi_archive_publication_t){
        .received_us = UINT64_C(0x100000001), .c5_session_id = UINT64_C(0x1234567887654321),
        .c5_relation_epoch = UINT64_C(0x2234567887654321), .generation = 77,
        .elapsed_ms = 8123, .s3_total = (uint16_t)s3, .c5_total = (uint16_t)(count-s3),
        .s3_count = (uint8_t)s3, .c5_count = (uint8_t)(count-s3),
        .s3_coverage = CERBERUS_WIFI_ARCHIVE_COVERAGE_2G,
        .c5_coverage = CERBERUS_WIFI_ARCHIVE_COVERAGE_5G, .split = true};
    d->saved_us = d->publication.received_us + 1001;
    d->saved_time_valid = true;
    d->count = (uint8_t)count;
    for (unsigned i=0; i<count; ++i) {
        cerberus_wifi_archive_row_t *r = &d->rows[i];
        unsigned index = i%32;
        snprintf(r->ssid, sizeof(r->ssid), "shared-ap-%02u", index);
        r->bssid[0] = 2; r->bssid[5] = (uint8_t)index;
        r->source = i<32 ? CERBERUS_WIFI_ARCHIVE_SOURCE_S3 : CERBERUS_WIFI_ARCHIVE_SOURCE_C5;
        r->source_index = (uint8_t)index;
        r->channel = i<32 ? 6 : 36;
        r->security = (uint8_t)(i%10);
        r->rssi = i%3 ? -42 : 0;
        r->rssi_valid = i%3 != 0;
        r->source_generation = i<32 ? 0x1234 : 250;
        r->received_ms = 0x12345678;
        if (i>=32 && i%2) {
            r->frame_present = true;
            r->frame_request_started_us = d->publication.received_us - 1000;
            r->frame_source_age_ms = 100+i;
            r->frame_age_upper_valid = true;
            r->frame_age_upper_at_save_ms = r->frame_source_age_ms + 3;
        }
    }
}
