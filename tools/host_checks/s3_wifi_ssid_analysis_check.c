#ifdef NDEBUG
#undef NDEBUG
#endif
#include "s3_wifi_ssid_analysis.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static unsigned checks;
#define CHECK(value) do { ++checks; assert(value); } while (0)
static cerberus_c5_wifi_result_t rows[CERBERUS_WIFI_SCAN_MAX_RESULTS];

static void fill(size_t count, const char *name)
{
    memset(rows, 0, sizeof(rows));
    for (size_t i = 0; i < count; ++i) {
        memcpy(rows[i].ssid, name, strlen(name) + 1U);
        rows[i].bssid[0] = 2U; rows[i].bssid[5] = (uint8_t)i;
        rows[i].channel = i % 2U ? 36U : 1U;
        rows[i].security = (uint8_t)(i % (CERBERUS_WIFI_SECURITY_OTHER + 1U));
        rows[i].rssi = (int8_t)(-30 - (int)i);
    }
}

static cerberus_s3_wifi_ssid_analysis_t analyze(size_t count, size_t selected)
{
    cerberus_c5_wifi_result_t before[CERBERUS_WIFI_SCAN_MAX_RESULTS];
    memcpy(before, rows, sizeof(rows));
    struct { unsigned before; cerberus_s3_wifi_ssid_analysis_t result; unsigned after; } guarded;
    memset(&guarded, 0x5A, sizeof(guarded));
    unsigned canary = guarded.before;
    CHECK(cerberus_s3_wifi_ssid_analyze(rows, count, selected, &guarded.result));
    CHECK(guarded.before == canary && guarded.after == canary);
    CHECK(!memcmp(before, rows, sizeof(rows)));
    return guarded.result;
}

static void reject(const cerberus_c5_wifi_result_t *bank, size_t count, size_t selected)
{
    cerberus_s3_wifi_ssid_analysis_t output, zero = {0};
    memset(&output, 0xA5, sizeof(output));
    CHECK(!cerberus_s3_wifi_ssid_analyze(bank, count, selected, &output));
    CHECK(!memcmp(&output, &zero, sizeof(output)));
}

static void exact_received_names(void)
{
    /* Every nonzero single byte stays distinct from a literal question mark,
     * even when the UI renders its nonprintable value as '?'. */
    for (unsigned value = 1U; value <= UINT8_MAX; ++value) {
        fill(4U, "?");
        rows[0].ssid[0] = rows[2].ssid[0] = (char)value;
        cerberus_s3_wifi_ssid_analysis_t out = analyze(4U, 0U);
        CHECK(out.total_bssids == (value == '?' ? 4U : 2U));
        CHECK(out.peer_count == (value == '?' ? 3U : 1U));
        if (value != '?') CHECK(out.peers[0].bssid[5] == 2U);
    }
    fill(4U, "Alpha");
    memcpy(rows[1].ssid, "alpha", 6U);
    memcpy(rows[2].ssid, "Alph", 5U);
    rows[3].ssid[5] = '!'; rows[3].ssid[6] = '\0';
    CHECK(analyze(4U, 0U).total_bssids == 1U);
    fill(3U, "12345678901234567890123456789012");
    rows[1].ssid[31] = 'X';
    cerberus_s3_wifi_ssid_analysis_t out = analyze(3U, 2U);
    CHECK(out.total_bssids == 2U && out.peer_count == 1U && out.peers[0].bssid[5] == 0U);
    fill(3U, "");
    out = analyze(3U, 1U);
    CHECK(out.total_bssids == 1U && !out.peer_count);
    CHECK(out.count_24_ghz == 0U && out.count_5_ghz == 1U);
    CHECK(out.security_mask == (1U << rows[1].security));
    CHECK(out.strongest_rssi == rows[1].rssi && out.weakest_rssi == rows[1].rssi);
}

static void bounded_summary(void)
{
    for (size_t count = 1U; count <= CERBERUS_WIFI_SCAN_MAX_RESULTS; ++count) {
        for (size_t selected = 0U; selected < count; ++selected) {
            fill(count, "network");
            cerberus_s3_wifi_ssid_analysis_t out = analyze(count, selected);
            CHECK(out.total_bssids == count);
            CHECK(out.count_24_ghz == (count + 1U) / 2U && out.count_5_ghz == count / 2U);
            CHECK(out.strongest_rssi == -30 && out.weakest_rssi == -30 - (int)count + 1);
            CHECK(out.peer_count == (count < 5U ? count - 1U : 4U));
            unsigned mask = 0U;
            for (size_t i = 0; i < count; ++i) mask |= 1U << rows[i].security;
            CHECK(out.security_mask == mask);
            for (size_t i = 0; i < out.peer_count; ++i) {
                size_t source = i >= selected ? i + 1U : i;
                CHECK(!memcmp(&out.peers[i], &rows[source], sizeof(rows[source])));
            }
        }
    }
    fill(4U, "network");
    memcpy(rows[1].bssid, rows[0].bssid, 6U);
    rows[0].rssi = INT8_MIN; rows[3].rssi = INT8_MAX;
    rows[2].channel = 14U; rows[3].channel = 15U;
    cerberus_s3_wifi_ssid_analysis_t out = analyze(4U, 0U);
    CHECK(out.total_bssids == 4U && out.peer_count == 2U); /* No silent dedupe. */
    CHECK(out.peers[0].bssid[5] == 2U && out.peers[1].bssid[5] == 3U);
    CHECK(out.weakest_rssi == INT8_MIN && out.strongest_rssi == INT8_MAX);
    CHECK(out.count_24_ghz == 2U && out.count_5_ghz == 2U);
}

static void malformed_atomicity(void)
{
    fill(3U, "network");
    CHECK(!cerberus_s3_wifi_ssid_analyze(rows, 3U, 0U, NULL));
    reject(NULL, 1U, 0U); reject(rows, 0U, 0U);
    reject(rows, CERBERUS_WIFI_SCAN_MAX_RESULTS + 1U, 0U);
    reject(rows, 3U, 3U); reject(rows, 3U, SIZE_MAX); reject(rows, SIZE_MAX, 0U);
    for (size_t bad = 0U; bad < 3U; ++bad) {
        for (unsigned failure = 0U; failure < 3U; ++failure) {
            fill(3U, "network");
            if (failure == 0U) memset(rows[bad].ssid, 'X', sizeof(rows[bad].ssid));
            if (failure == 1U) rows[bad].channel = 0U;
            if (failure == 2U) rows[bad].security = CERBERUS_WIFI_SECURITY_OTHER + 1U;
            reject(rows, 3U, 0U);
        }
    }
    fill(3U, "network"); rows[0].ssid[0] = '\0';
    memset(rows[2].ssid, 'X', sizeof(rows[2].ssid));
    reject(rows, 3U, 0U); /* Even hidden selection does not bypass bank validation. */
}

int main(void)
{
    exact_received_names(); bounded_summary(); malformed_atomicity();
    printf("Received-SSID analysis: %u checks; raw string identity, bounded summary, peer order and atomic validation pass\n", checks);
    return 0;
}
