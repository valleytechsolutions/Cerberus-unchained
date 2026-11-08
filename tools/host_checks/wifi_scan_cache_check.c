#ifdef NDEBUG
#undef NDEBUG
#endif
#include "c5_spi_link.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define UI_WIFI_VISIBLE_ROWS 6U
#define UI_WIFI_HISTORY_SAMPLES 6U
#include "cache_metadata.h"
static cerberus_c5_wifi_result_t wifi_results[UI_WIFI_VISIBLE_ROWS];
static cerberus_wifi_scan_status_payload_t wifi_scan_status;
static size_t wifi_result_count, wifi_page_offset;
static uint8_t wifi_channel_counts[256], wifi_channel_generation;
static int8_t wifi_channel_strongest[256];
static uint8_t wifi_channel_history_counts[UI_WIFI_HISTORY_SAMPLES][256];
static int8_t wifi_channel_history_strongest[UI_WIFI_HISTORY_SAMPLES][256];
static uint8_t wifi_channel_history_next_sample, wifi_channel_history_sample_count;
static uint8_t wifi_channel_history_last_generation, wifi_observed_generation;
static uint32_t wifi_observed_at_seconds;
static bool wifi_observation_valid, wifi_scan_checked, wifi_radio_stats_checked;
static esp_err_t wifi_scan_error, wifi_channel_error, wifi_analysis_error;
static esp_err_t wifi_radio_stats_error;
static uint8_t wifi_analysis_generation;
static cerberus_c5_status_t c5_status;
static cerberus_wifi_radio_stats_payload_t wifi_radio_stats;
enum { UI_PAGE_WIFI, UI_PAGE_OTHER };
static unsigned current_page = UI_PAGE_WIFI;
static unsigned transport_calls;
static struct { bool checked; } wifi_baseline;
static int64_t now_us;
static int64_t esp_timer_get_time(void) { return now_us; }
esp_err_t c5_spi_link_get_wifi_scan_result(uint8_t generation, uint8_t index,
    cerberus_c5_wifi_result_t *result)
{ (void)generation; (void)index; (void)result; ++transport_calls; return ESP_FAIL; }
esp_err_t c5_spi_link_get_wifi_scan_metadata(uint8_t generation, uint8_t index,
    cerberus_c5_wifi_result_t *result)
{ (void)generation; (void)index; (void)result; ++transport_calls; return ESP_FAIL; }
esp_err_t c5_spi_link_get_wifi_scan_status(cerberus_wifi_scan_status_payload_t *status)
{ (void)status; ++transport_calls; return ESP_FAIL; }
esp_err_t c5_spi_link_get_wifi_radio_stats(cerberus_wifi_radio_stats_payload_t *stats)
{ (void)stats; ++transport_calls; return ESP_FAIL; }
#include "cache_impl.h"

static void retained_bank_age_and_history(void)
{
    cerberus_c5_wifi_result_t rows[2] = {{.channel=36,.rssi=-60}, {.channel=40,.rssi=-70}};
    cerberus_c5_wifi_relation_bank_t proof = {0};
    proof.status.epoch = UINT64_C(0x100000007);
    proof.status.generation = 7;
    proof.status.state = CERBERUS_WIFI_RELATION_COMPLETE;
    proof.firmware_version = 63;
    proof.capabilities = CERBERUS_C5_CAP_WIFI_2_4_GHZ | CERBERUS_C5_CAP_WIFI_5_GHZ;
    proof.protocol_version = CERBERUS_PROTOCOL_VERSION;
    proof.boot_count = 4;
    proof.request_started_us = UINT64_C(500000000);
    proof.source_now_us = UINT64_C(9900000);
    proof.peer_uptime_ms = 9999;
    proof.clock_valid = true;
    const int64_t receipt = INT64_C(5000000000000);
    const uint64_t session = UINT64_C(0xaabbccdd01020304);
    now_us = receipt + 900000000;
    wifi_scan_status = (cerberus_wifi_scan_status_payload_t){
        .generation=7,.state=CERBERUS_WIFI_SCAN_COMPLETE,.stored_count=2};
    clear_wifi_channel_history();
    wifi_commit_cached_results_at(rows, 2, 7, receipt, CERBERUS_WIFI_SCAN_BAND_5G, &proof, session);
    assert(wifi_channel_history_sample_count == 1 && wifi_observed_at_seconds == receipt / 1000000);
    assert(!strcmp(wifi_scan_coverage_text(wifi_snapshot_band), "5 GHZ ONLY"));
    assert(wifi_channel_history_band_samples(CERBERUS_WIFI_SCAN_BAND_2G) == 0);
    assert(wifi_channel_history_band_samples(CERBERUS_WIFI_SCAN_BAND_5G) == 1);
    now_us += 100000000;
    wifi_commit_cached_results_at(rows, 2, 7, receipt, CERBERUS_WIFI_SCAN_BAND_5G, &proof, session);
    assert(wifi_channel_history_sample_count == 1 && wifi_observed_at_seconds == receipt / 1000000);
    assert(wifi_channel_history_total(36) == 1 && wifi_channel_history_seen(36) == 1);
    assert(wifi_channel_history_best_rssi(40) == -70);

    /* A normal imported cache is usable. Actual offline/online invalidation
     * must not let gen8-only legacy reads alter scope, age, rows or history. */
    assert(refresh_wifi_scan_status() == ESP_OK && rebuild_wifi_channel_summary() == ESP_OK);
    assert(transport_calls == 0);
    cerberus_c5_wifi_result_t displayed[UI_WIFI_VISIBLE_ROWS];
    memcpy(displayed, wifi_results, sizeof(displayed));
    wifi_history_identity_t retained_identity = wifi_channel_current_identity;
    telemetry_recovery(true);
    assert(!wifi_scan_checked && wifi_channel_generation == 0 && wifi_channel_error == ESP_ERR_INVALID_STATE);
    assert(refresh_wifi_scan_status() == ESP_ERR_INVALID_STATE);
    assert(rebuild_wifi_channel_summary() == ESP_ERR_INVALID_STATE);
    assert(load_wifi_results() == ESP_ERR_INVALID_STATE);
    assert(!memcmp(displayed, wifi_results, sizeof(displayed)) && wifi_result_count == 2);
    assert(wifi_channel_history_sample_count == 1 && wifi_channel_counts[36] == 1);
    assert(wifi_snapshot_band == CERBERUS_WIFI_SCAN_BAND_5G && wifi_observed_at_seconds == receipt / 1000000);
    assert(wifi_history_same_identity(&retained_identity, &wifi_channel_current_identity));
    assert(transport_calls == 0);

    /* A separate legacy publication keeps its original semantics, even with
     * identical gen8. Reopening the older proven bank must still deduplicate. */
    wifi_commit_cached_results(rows, 2, 7);
    assert(wifi_channel_history_sample_count == 2 && wifi_snapshot_band == CERBERUS_WIFI_SCAN_BAND_ALL);
    assert(!wifi_channel_current_identity.valid && wifi_scan_error == ESP_OK);
    assert(wifi_observed_at_seconds == now_us / 1000000);
    wifi_commit_cached_results_at(rows, 2, 7, receipt, CERBERUS_WIFI_SCAN_BAND_5G, &proof, session);
    assert(wifi_channel_history_sample_count == 2);
    assert(wifi_channel_history_band_samples(CERBERUS_WIFI_SCAN_BAND_2G) == 1);
    assert(wifi_channel_history_band_samples(CERBERUS_WIFI_SCAN_BAND_5G) == 2);

    /* Full-width epochs and session/peer identity prevent gen8-only merging. */
    ++proof.status.epoch;
    wifi_commit_cached_results_at(rows, 2, 7, receipt + 1, CERBERUS_WIFI_SCAN_BAND_5G, &proof, session);
    assert(wifi_channel_history_sample_count == 3);
    ++proof.boot_count;
    wifi_commit_cached_results_at(rows, 2, 7, receipt + 2, CERBERUS_WIFI_SCAN_BAND_5G, &proof, session);
    assert(wifi_channel_history_sample_count == 4);
    wifi_commit_cached_results_at(rows, 2, 7, receipt + 3, CERBERUS_WIFI_SCAN_BAND_5G, &proof, session + 1);
    assert(wifi_channel_history_sample_count == 5);
    wifi_history_identity_t identity = wifi_channel_current_identity, changed;
    assert(wifi_history_same_identity(&identity, &identity));
#define DIFFERENT(member) do { changed = identity; ++changed.member; \
    assert(!wifi_history_same_identity(&identity, &changed)); } while (0)
    DIFFERENT(proof.status.epoch); DIFFERENT(proof.status.generation);
    DIFFERENT(proof.status.count); DIFFERENT(proof.status.flags);
    DIFFERENT(proof.request_started_us); DIFFERENT(proof.source_now_us);
    DIFFERENT(proof.firmware_version); DIFFERENT(proof.capabilities);
    DIFFERENT(proof.peer_uptime_ms); DIFFERENT(proof.protocol_version);
    DIFFERENT(proof.reset_reason); DIFFERENT(proof.boot_count);
    DIFFERENT(session_id); DIFFERENT(band);
#undef DIFFERENT
    changed = identity; changed.proof.clock_valid = false;
    assert(!wifi_history_same_identity(&identity, &changed));
    changed = identity; changed.valid = false;
    assert(!wifi_history_same_identity(&identity, &changed));

    /* Reset keeps the current bank's identity/coverage aligned with slot0. */
    reset_wifi_channel_history_to_current();
    assert(wifi_channel_history_sample_count == 1 && wifi_channel_history_next_sample == 1);
    assert(wifi_history_same_identity(&wifi_channel_history_identities[0], &identity));
    assert(wifi_channel_history_band_samples(CERBERUS_WIFI_SCAN_BAND_2G) == 0);
    wifi_commit_cached_results_at(rows, 2, 7, receipt + 3, CERBERUS_WIFI_SCAN_BAND_5G, &proof, session + 1);
    assert(wifi_channel_history_sample_count == 1);
    clear_wifi_channel_history();
    assert(wifi_channel_history_sample_count == 0 && !wifi_channel_history_identities[0].valid);
    assert(!strcmp(wifi_scan_coverage_text(CERBERUS_WIFI_SCAN_BAND_2G), "2.4 GHZ ONLY"));
    assert(!strcmp(wifi_scan_coverage_text(CERBERUS_WIFI_SCAN_BAND_ALL), "2.4 + 5 GHZ"));
}

int main(void)
{
    cerberus_c5_wifi_result_t rows[CERBERUS_WIFI_SCAN_MAX_RESULTS] = {0}, before[CERBERUS_WIFI_SCAN_MAX_RESULTS];
    for (unsigned i = 0; i < CERBERUS_WIFI_SCAN_MAX_RESULTS; ++i) {
        rows[i].channel = i < 8 ? 6 : 36;
        rows[i].rssi = -90 + (int)i;
        rows[i].bssid[5] = (uint8_t)i;
        memcpy(rows[i].ssid, "AP\001", 4);
    }
    memcpy(before, rows, sizeof(rows));
    wifi_scan_status.generation = 255;
    wifi_page_offset = 12;
    wifi_baseline.checked = wifi_radio_stats_checked = true;
    now_us = INT64_C(5000000000000); /* Beyond the uint32 millisecond wrap. */
    wifi_commit_cached_results(rows, CERBERUS_WIFI_SCAN_MAX_RESULTS, 255);
    assert(!memcmp(before, rows, sizeof(rows)));
    assert(wifi_result_count == 6 && wifi_page_offset == 12);
    for (unsigned i = 0; i < 6; ++i) {
        assert(wifi_results[i].bssid[5] == i + 12);
        assert(!strcmp(wifi_results[i].ssid, "AP?"));
    }
    assert(wifi_channel_counts[6] == 8 && wifi_channel_counts[36] == CERBERUS_WIFI_SCAN_MAX_RESULTS - 8);
    assert(wifi_channel_strongest[6] == -83 && wifi_channel_strongest[36] == -90 + (int)CERBERUS_WIFI_SCAN_MAX_RESULTS - 1);
    assert(wifi_channel_strongest[1] == INT8_MIN);
    assert(wifi_observed_at_seconds == 5000000 && wifi_observed_generation == 255);
    assert(wifi_observation_valid && wifi_scan_checked && wifi_scan_error == ESP_OK && wifi_channel_error == ESP_OK);
    assert(!wifi_baseline.checked && !wifi_radio_stats_checked && wifi_analysis_error == ESP_ERR_INVALID_STATE);
    assert(wifi_channel_history_sample_count == 1);
    record_wifi_channel_history(false); /* Same legacy page must not add a sample. */
    assert(wifi_channel_history_sample_count == 1);
    /* A new publication with the same gen8 is distinct even after wrap. */
    rows[0].channel = 1;
    wifi_commit_cached_results(rows, 1, 255);
    assert(wifi_channel_history_sample_count == 2 && wifi_channel_history_last_generation == 255);
    assert(wifi_result_count == 1 && wifi_page_offset == 0 && wifi_channel_counts[1] == 1);
    assert(wifi_channel_counts[6] == 0 && wifi_channel_counts[36] == 0);
    for (unsigned i = 1; i < 6; ++i) {
        cerberus_c5_wifi_result_t zero = {0};
        assert(!memcmp(&wifi_results[i], &zero, sizeof(zero)));
    }
    wifi_scan_status.generation = 0;
    wifi_commit_cached_results(NULL, 0, 0);
    assert(wifi_result_count == 0 && wifi_page_offset == 0 && wifi_channel_generation == 0);
    for (unsigned i = 0; i < 256; ++i) assert(wifi_channel_counts[i] == 0 && wifi_channel_strongest[i] == INT8_MIN);
    for (unsigned i = 0; i < 8; ++i) wifi_commit_cached_results(rows, 1, 0);
    assert(wifi_channel_history_sample_count == UI_WIFI_HISTORY_SAMPLES && wifi_channel_history_next_sample == 5);
    retained_bank_age_and_history();
    printf("History metadata: %zu bytes for current+%u retained keys and coverage.\n",
        sizeof(wifi_channel_current_identity) + sizeof(wifi_channel_history_identities) +
        sizeof(wifi_channel_history_bands) + sizeof(wifi_snapshot_band), UI_WIFI_HISTORY_SAMPLES);
    assert(transport_calls == 0);
    puts("Wi-Fi cached publication: pagination, immutable SSIDs, channels, gen8 reuse, retained age/history and telemetry recovery passed; zero transport calls.");
    return 0;
}
