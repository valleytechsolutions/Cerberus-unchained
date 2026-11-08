/* Reuse the peer's exact snapshot/transport boundary, with the production
 * reader and session controller. Only native radio calls are substituted. */
#define main existing_dual_reader_fixture
#include "dual_ble_check.c"
#undef main
#include "saved_ble_lookup.h"

static peer_t peer;
static cerberus_saved_ble_lookup_t lookup;
static cerberus_s3_ble_discovery_meta_t native;
static cerberus_s3_ble_discovery_record_t native_rows[4];
static unsigned native_starts, native_stops, native_releases;
static bool native_fail_start, native_fail_stop, native_bad_copy;
static bool c5_invalid_rssi, c5_invalid_ad;
static const uint8_t target[6] = {1,2,3,4,5,0};

esp_err_t cerberus_s3_ble_scanner_start_reacquire(const uint8_t address[6], uint64_t previous,
    cerberus_s3_ble_discovery_meta_t *out)
{
    memset(out, 0, sizeof(*out));
    if (native_fail_start) return ESP_ERR_NO_MEM;
    assert((!native.attached && !previous) || (previous == native.scan_epoch && !native.owned));
    assert(!memcmp(address, target, 6U));
    ++native_starts;
    native = (cerberus_s3_ble_discovery_meta_t){.scan_epoch = native_starts, .revision = 1,
        .started_ms = peer.now, .source_now_ms = peer.now, .reacquiring = true,
        .attached = true, .owned = true,
        .status = {.state = CERBERUS_BLE_SCAN_RUNNING, .generation = (uint8_t)native_starts}};
    memcpy(native.target_address, address, 6U);
    memset(native_rows, 0, sizeof(native_rows));
    *out = native;
    return ESP_OK;
}
esp_err_t cerberus_s3_ble_scanner_discovery_status(uint64_t epoch,
    cerberus_s3_ble_discovery_meta_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!native.attached || epoch != native.scan_epoch) return ESP_ERR_NOT_FOUND;
    native.source_now_ms = peer.now;
    *out = native;
    return ESP_OK;
}
esp_err_t cerberus_s3_ble_scanner_copy_reacquire(uint64_t epoch,
    cerberus_s3_ble_discovery_record_t rows[4], cerberus_s3_ble_discovery_meta_t *out)
{
    esp_err_t error = cerberus_s3_ble_scanner_discovery_status(epoch, out);
    memset(rows, 0, sizeof(native_rows));
    if (!error) memcpy(rows, native_rows, sizeof(native_rows));
    if (native_bad_copy) out->target_address[0] ^= 1U;
    return error;
}
esp_err_t cerberus_s3_ble_scanner_stop_discovery(uint64_t epoch,
    cerberus_s3_ble_discovery_meta_t *out)
{
    ++native_stops;
    esp_err_t error = cerberus_s3_ble_scanner_discovery_status(epoch, out);
    if (error || native_fail_stop) return error ? error : ESP_ERR_TIMEOUT;
    native.owned = false;
    native.status.state = CERBERUS_BLE_SCAN_COMPLETE;
    ++native.revision;
    *out = native;
    return ESP_OK;
}
esp_err_t cerberus_s3_ble_scanner_release_discovery(uint64_t epoch)
{
    ++native_releases;
    if (!native.attached || native.scan_epoch != epoch) return ESP_ERR_NOT_FOUND;
    if (native.owned) return ESP_ERR_INVALID_STATE;
    native.attached = false;
    return ESP_OK;
}

static esp_err_t lookup_exchange(void *context, uint8_t command, const void *request,
    size_t request_size, void *reply, size_t reply_size)
{
    esp_err_t error = exchange(context, command, request, request_size, reply, reply_size);
    if (!error && command == CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_BEGIN && c5_invalid_rssi)
        for (uint8_t i = 0; i < peer.count; ++i) {
            peer.frozen[i].timing_flags = CERBERUS_DUAL_BLE_SEEN;
            peer.frozen[i].latest_rssi = 127;
        }
    if (!error && command == CERBERUS_SPI_COMMAND_DUAL_BLE_SNAPSHOT_BEGIN && c5_invalid_ad)
        for (uint8_t i = 0; i < peer.count; ++i) {
            peer.frozen_receive[i].ad_validation = CERBERUS_DUAL_BLE_AD_INVALID;
            peer.frozen[i].raw_flags = CERBERUS_BLE_ADVERTISEMENT_MALFORMED;
            peer.frozen[i].raw[0] = 8;
        }
    return error;
}
static void lookup_init(void)
{
    cerberus_dual_ble_scan_t scratch;
    init_live(&scratch, &peer);
    peer.count = 4;
    peer.metadata_supported = true;
    memset(&native, 0, sizeof(native));
    native_starts = native_stops = native_releases = 0;
    native_fail_start = native_fail_stop = native_bad_copy = false;
    c5_invalid_rssi = c5_invalid_ad = false;
    const cerberus_dual_ble_ops_t ops = {&peer, lookup_exchange, clock_ms};
    cerberus_saved_ble_lookup_init(&lookup, &ops);
    assert(cerberus_saved_ble_lookup_start(&lookup, target));
}
static void step(void)
{
    cerberus_saved_ble_lookup_tick(&lookup, peer.now);
    peer.now += 20;
}
static void feed_native(void)
{
    assert(native.attached);
    native.status.stored_count = 4;
    ++native.revision;
    for (uint8_t i = 0; i < 4; ++i) {
        cerberus_s3_ble_discovery_record_t *r = &native_rows[i];
        memset(r, 0, sizeof(*r));
        memcpy(r->peer.address, target, 6U);
        r->peer.address_type = i;
        r->peer.rssi = -50 - (int8_t)i;
        r->generation = native.status.generation;
        r->index = i;
        r->observed_ms = (uint32_t)peer.now;
        r->focus.discovery = (cerberus_s3_ble_discovery_evidence_t){
            .flags = CERBERUS_S3_BLE_EVIDENCE_ADV_VALID | CERBERUS_S3_BLE_EVIDENCE_ADV_COMPLETE,
            .advertisement_sequence = 1, .raw_advertisement_sequence = 1,
            .advertisement_observed_ms = r->observed_ms};
    }
}
static void wait_candidates(uint8_t expected)
{
    cerberus_saved_ble_lookup_view_t view;
    for (unsigned i = 0; i < 2000; ++i) {
        step();
        cerberus_saved_ble_lookup_view(&lookup, peer.now, &view);
        if (view.count == expected) return;
    }
    fprintf(stderr, "expected %u candidates, got %u; C5 error=%d snapshots=%u S3=%d\n",
        expected, view.count, view.source[1].error, lookup.c5.snapshots, view.source[0].error);
    assert(false);
}
static void stop_lookup(void)
{
    cerberus_saved_ble_lookup_stop(&lookup);
    for (unsigned i = 0; i < 2000 && !cerberus_saved_ble_lookup_released(&lookup); ++i) step();
    assert(cerberus_saved_ble_lookup_released(&lookup));
    assert(!native.owned && !native.attached && !peer.owned && !peer.attached);
}
int main(void)
{
    lookup_init();
    cerberus_saved_ble_lookup_candidate_t painted[8], out;
    assert(!cerberus_saved_ble_lookup_candidate(&lookup, 0, peer.now, &out));
    step(); feed_native(); wait_candidates(8);
    for (uint8_t i = 0; i < 8; ++i) {
        assert(cerberus_saved_ble_lookup_candidate(&lookup, i, peer.now, &painted[i]));
        assert(painted[i].source == i/4U + 1U && painted[i].peer.address_type == i%4U);
        assert(painted[i].fresh && cerberus_saved_ble_lookup_accepts(&lookup, &painted[i], peer.now));
    }
    /* Neither repeated reads nor later snapshots refresh a painted proof. */
    uint64_t expired = painted[0].anchor_ms + CERBERUS_SAVED_BLE_FRESH_MS + 1U;
    assert(!cerberus_saved_ble_lookup_accepts(&lookup, &painted[0], expired));
    assert(!cerberus_saved_ble_lookup_accepts(&lookup, &painted[0], painted[0].anchor_ms - 1U));
    assert(!cerberus_saved_ble_lookup_candidate(&lookup, 8, peer.now, &out));
    native_rows[0].focus.discovery.flags &= (uint8_t)~CERBERUS_S3_BLE_EVIDENCE_ADV_VALID;
    native_rows[1].peer.rssi = 127;
    ++native.revision;
    for (unsigned i = 0; i < 15; ++i) step();
    assert(!(lookup.seen_mask & 3U) && !lookup.s3.error);
    assert(!cerberus_saved_ble_lookup_accepts(&lookup, &painted[0], peer.now));
    feed_native(); wait_candidates(8);
    c5_invalid_rssi = true;
    unsigned publication = lookup.c5.snapshots;
    for (unsigned i = 0; i < 2000 && lookup.c5.snapshots <= publication + 1U; ++i) step();
    assert(lookup.c5.snapshots > publication + 1U && !(lookup.seen_mask & 0xf0U));
    assert(!cerberus_saved_ble_lookup_accepts(&lookup, &painted[4], peer.now));
    c5_invalid_rssi = false;
    feed_native(); wait_candidates(8);
    c5_invalid_ad = true;
    publication = lookup.c5.snapshots;
    for (unsigned i = 0; i < 2000 && lookup.c5.snapshots <= publication + 1U; ++i) step();
    assert(lookup.c5.snapshots > publication + 1U && !(lookup.seen_mask & 0xf0U) && !lookup.c5.error);
    c5_invalid_ad = false;
    feed_native(); wait_candidates(8);
    native_fail_stop = peer.fail_stop = true;
    cerberus_saved_ble_lookup_stop(&lookup);
    for (unsigned i = 0; i < 30; ++i) step();
    assert(!cerberus_saved_ble_lookup_released(&lookup));
    assert(!cerberus_saved_ble_lookup_start(&lookup, target));
    assert(native_stops && peer.stops);
    native_fail_stop = peer.fail_stop = false;
    stop_lookup();
    assert(cerberus_saved_ble_lookup_candidate(&lookup, 0, peer.now, &out));
    assert(cerberus_saved_ble_lookup_start(&lookup, target));
    assert(!cerberus_saved_ble_lookup_accepts(&lookup, &painted[0], peer.now));
    assert(!cerberus_saved_ble_lookup_candidate(&lookup, 0, peer.now, &out));
    stop_lookup();

    /* Either receiver can produce candidates without the other. */
    lookup_init(); native_fail_start = true;
    wait_candidates(4);
    assert(lookup.s3.error == ESP_ERR_NO_MEM);
    assert(cerberus_saved_ble_lookup_candidate(&lookup, 0, peer.now, &out) && out.source == 2);
    stop_lookup();
    lookup_init(); peer.unsupported = true;
    step(); feed_native(); wait_candidates(4);
    assert(cerberus_saved_ble_lookup_candidate(&lookup, 0, peer.now, &out) && out.source == 1);
    stop_lookup();

    /* Wrong source selectors cannot publish; a regressed UI clock cancels
     * both intents and invalidates every candidate, while cleanup continues. */
    lookup_init(); step(); feed_native(); native_bad_copy = true;
    for (unsigned i = 0; i < 60; ++i) step();
    assert(lookup.s3.error == ESP_ERR_INVALID_RESPONSE);
    assert(!(lookup.seen_mask & 15U));
    native_bad_copy = false;
    peer.now = 1;
    step();
    assert(!lookup.enabled && !lookup.seen_mask);
    stop_lookup();
    printf("Saved BLE lookup: typed dual candidates, fresh proof, independent failure and exact cleanup passed (%zu bytes).\n", sizeof(lookup));
    return 0;
}
