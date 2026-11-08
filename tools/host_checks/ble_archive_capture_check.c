#ifdef NDEBUG
#undef NDEBUG
#endif
#include "ble_archive_capture.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned checks;
#define CHECK(c) do { ++checks; assert(c); } while (0)

static cerberus_c5_ble_result_t peer(void)
{
    cerberus_c5_ble_result_t p = {.address = {1,2,3,4,5,6}, .address_type = 2,
        .rssi = -55, .name = "SOURCE", .manufacturer_id = 0x4c};
    return p;
}
static cerberus_c5_ble_advertisement_t raw(void)
{
    cerberus_c5_ble_advertisement_t a = {.address = {1,2,3,4,5,6}, .address_type = 2,
        .length = 3, .event_type = 0, .reports_seen = 7, .data = {2,1,6}};
    return a;
}
static cerberus_s3_ble_focus_t focus(bool response)
{
    cerberus_s3_ble_focus_t f = {0};
    cerberus_s3_ble_discovery_evidence_t *e = &f.discovery;
    const cerberus_c5_ble_advertisement_t a = raw();
    memcpy(e->address, a.address, 6); e->address_type = a.address_type; e->generation = 9;
    e->advertisement_sequence = e->raw_advertisement_sequence = 17;
    e->advertisement_observed_ms = 1;
    e->flags = CERBERUS_S3_BLE_EVIDENCE_ADV_COMPLETE | CERBERUS_S3_BLE_EVIDENCE_ADV_VALID |
        CERBERUS_S3_BLE_EVIDENCE_RAW_COMPLETE;
    if (response) {
        e->flags |= CERBERUS_S3_BLE_EVIDENCE_RESPONSE_VALID;
        e->scan_response_sequence = 19; e->scan_response_observed_ms = 3;
        e->scan_response.length = e->scan_response.original_length = 3;
        e->scan_response.event_type = 4;
        e->scan_response.data[0] = 2; e->scan_response.data[1] = 10; e->scan_response.data[2] = 1;
    }
    return f;
}
static cerberus_ble_archive_row_t base(void)
{
    const cerberus_c5_ble_result_t p = peer(); const cerberus_c5_ble_advertisement_t a = raw();
    cerberus_ble_archive_row_t row;
    memset(&row, 0xa7, sizeof(row));
    CHECK(cerberus_ble_archive_capture_row(&row, &p, &a, 9, 3, 0x123456789ULL, true,
        0x100000001ULL, 0x100000005ULL, true));
    CHECK(row.receiver == CERBERUS_BLE_ARCHIVE_RECEIVER_S3 && !row.response_state);
    return row;
}
static void roundtrip(const cerberus_ble_archive_row_t *r)
{
    cerberus_ble_archive_t d = {.saved_uptime_ms = UINT64_MAX, .count = 1}, decoded;
    d.rows[0] = *r;
    uint8_t wire[CERBERUS_BLE_ARCHIVE_MAX_SIZE]; size_t n = 0;
    CHECK(cerberus_ble_archive_validate(&d));
    CHECK(cerberus_ble_archive_encode(&d, wire, sizeof(wire), &n));
    CHECK(cerberus_ble_archive_decode(wire, n, &decoded));
    CHECK(!memcmp(&decoded, &d, sizeof(d)));
}
static void unknown(cerberus_ble_archive_row_t r, const cerberus_s3_ble_focus_t *f,
    const cerberus_c5_ble_advertisement_t *a, uint32_t time)
{
    const cerberus_ble_archive_row_t before = r;
    CHECK(cerberus_ble_archive_capture_response(&r, f, a, time));
    CHECK(!r.response_state && !memcmp(&r, &before, sizeof(r)));
    roundtrip(&r);
}
static void rejected(cerberus_ble_archive_row_t r, const cerberus_s3_ble_focus_t *f,
    const cerberus_c5_ble_advertisement_t *a)
{
    const cerberus_ble_archive_row_t before = r;
    CHECK(!cerberus_ble_archive_capture_response(&r, f, a, 1));
    CHECK(!memcmp(&r, &before, sizeof(r)));
}
int main(void)
{
    cerberus_ble_archive_row_t row = base(), original = row;
    cerberus_c5_ble_advertisement_t a = raw(); cerberus_s3_ble_focus_t f = focus(true), changed;
    CHECK(cerberus_ble_archive_capture_response(&row, &f, &a, 1));
    CHECK(row.response_state == CERBERUS_BLE_ARCHIVE_RESPONSE_PRESENT);
    CHECK(row.response_original_sequence == 17 && row.response_sequence == 19);
    CHECK(row.response_source_uptime_ms == 0x100000003ULL && row.response_time_valid);
    CHECK(row.original_source_uptime_ms == original.original_source_uptime_ms);
    CHECK(row.copy_ceiling_ms == original.copy_ceiling_ms && row.reports_seen == original.reports_seen);
    CHECK(!memcmp(row.advertisement, original.advertisement, sizeof(row.advertisement)));
    CHECK(row.response_length == 3 && !memcmp(row.response, "\x02\x0a\x01", 3));
    roundtrip(&row);
    f = focus(false); row = base(); CHECK(cerberus_ble_archive_capture_response(&row, &f, &a, 1));
    CHECK(row.response_state == CERBERUS_BLE_ARCHIVE_RESPONSE_NONE); roundtrip(&row);
    /* Reuse cannot retain a preceding response after the bound source becomes unknown. */
    f = focus(true); CHECK(cerberus_ble_archive_capture_response(&row, &f, &a, 1));
    f.discovery.raw_advertisement_sequence = 16;
    CHECK(cerberus_ble_archive_capture_response(&row, &f, &a, 1));
    CHECK(!memcmp(&row, &original, sizeof(row)));

    /* Equal-ms newer original must not rebind old raw; generation/typed identity
     * and epoch/clock uncertainty also remain explicit UNKNOWN. */
    f = focus(true); changed = f; changed.discovery.advertisement_sequence = 18;
    unknown(base(), &changed, &a, 1);
    changed = f; changed.discovery.raw_advertisement_sequence = 0; unknown(base(), &changed, &a, 1);
    changed = f; ++changed.discovery.generation; unknown(base(), &changed, &a, 1);
    changed = f; ++changed.discovery.address_type; unknown(base(), &changed, &a, 1);
    changed = f; ++changed.discovery.address[5]; unknown(base(), &changed, &a, 1);
    changed = f; ++changed.discovery.advertisement_observed_ms; unknown(base(), &changed, &a, 1);
    changed = f; changed.discovery.flags &= (uint8_t)~CERBERUS_S3_BLE_EVIDENCE_ADV_VALID;
    unknown(base(), &changed, &a, 1);
    changed = f; changed.discovery.flags |= CERBERUS_S3_BLE_EVIDENCE_SID_INVALID;
    unknown(base(), &changed, &a, 1);
    changed = f; changed.discovery.scan_response.data[30] = 1; unknown(base(), &changed, &a, 1);
    changed = f; changed.discovery.scan_response_sequence = 17; unknown(base(), &changed, &a, 1);
    changed = f; changed.discovery.scan_response_observed_ms = 2002; unknown(base(), &changed, &a, 1);
    row = base(); row.scan_epoch = 0; row.scan_epoch_valid = false; unknown(row, &f, &a, 1);
    row = base(); row.source_time_valid = false; row.original_source_uptime_ms = row.copy_ceiling_ms = 0;
    unknown(row, &f, &a, 1);
    row = base(); row.copy_ceiling_ms = row.original_source_uptime_ms + 1; unknown(row, &f, &a, 1);
    unknown(base(), &f, &a, 2);

    rejected(base(), NULL, &a); rejected(base(), &f, NULL);
    CHECK(!cerberus_ble_archive_capture_response(NULL, &f, &a, 1));
    row = base(); row.receiver = CERBERUS_BLE_ARCHIVE_RECEIVER_C5; rejected(row, &f, &a);
    row = base(); row.address_type = 1; rejected(row, &f, &a);
    row = base(); ++row.address[0]; rejected(row, &f, &a);
    row = base(); row.advertisement_length = 2; rejected(row, &f, &a);
    row = base(); row.advertisement_event_type = 3; rejected(row, &f, &a);
    row = base(); row.advertisement_flags = 1; rejected(row, &f, &a);
    row = base(); ++row.advertisement[1]; rejected(row, &f, &a);
    row = base(); ++row.reports_seen; rejected(row, &f, &a);
    a.length = 32; rejected(base(), &f, &a); a = raw();

    /* Distinct exact source times survive low32 wrap, true zero, and the
     * uint64 limit. No retry/copy time is substituted for a radio timestamp. */
    row = base(); f = focus(true);
    row.original_source_uptime_ms = 0x1ffffffffULL; row.copy_ceiling_ms = 0x200000005ULL;
    f.discovery.advertisement_observed_ms = UINT32_MAX; f.discovery.scan_response_observed_ms = 1;
    CHECK(cerberus_ble_archive_capture_response(&row, &f, &a, UINT32_MAX));
    CHECK(row.response_source_uptime_ms == 0x200000001ULL); roundtrip(&row);
    row = base(); row.original_source_uptime_ms = row.copy_ceiling_ms = 0;
    f = focus(true); f.discovery.advertisement_observed_ms = f.discovery.scan_response_observed_ms = 0;
    CHECK(cerberus_ble_archive_capture_response(&row, &f, &a, 0));
    CHECK(row.response_state == 2 && row.response_time_valid && !row.response_source_uptime_ms); roundtrip(&row);
    row = base(); row.original_source_uptime_ms = row.copy_ceiling_ms = UINT64_MAX;
    f.discovery.advertisement_observed_ms = UINT32_MAX; f.discovery.scan_response_observed_ms = 0;
    unknown(row, &f, &a, UINT32_MAX);
    f.discovery.scan_response_observed_ms = UINT32_MAX;
    CHECK(cerberus_ble_archive_capture_response(&row, &f, &a, UINT32_MAX));
    CHECK(row.response_state == 2 && row.response_source_uptime_ms == UINT64_MAX); roundtrip(&row);

    for (unsigned delivered = 1; delivered <= 255; ++delivered) {
        row = base(); f = focus(true);
        f.discovery.scan_response.original_length = (uint8_t)delivered;
        f.discovery.scan_response.length = (uint8_t)(delivered > 31 ? 31 : delivered);
        f.discovery.scan_response.flags = delivered > 31 ? CERBERUS_BLE_ADVERTISEMENT_TRUNCATED : 0;
        memset(f.discovery.scan_response.data, 0, 31);
        for (unsigned i = 0; i < f.discovery.scan_response.length; ++i) f.discovery.scan_response.data[i] = (uint8_t)(i + 1);
        CHECK(cerberus_ble_archive_capture_response(&row, &f, &a, 1));
        CHECK(row.response_state == 2 && row.response_original_length == delivered);
        CHECK(row.response_length == (delivered > 31 ? 31 : delivered)); roundtrip(&row);
    }
    f = focus(true); f.discovery.flags |= CERBERUS_S3_BLE_EVIDENCE_SID_VALID;
    f.discovery.sid = 15; f.discovery.scan_response.event_type = 3; row = base();
    CHECK(cerberus_ble_archive_capture_response(&row, &f, &a, 1));
    CHECK(row.response_sid_valid && row.response_sid == 15 && row.response_event_type == 3); roundtrip(&row);
    printf("S3 frozen BLE response archive capture: PASS %u checks\n", checks);
    return 0;
}
