#ifdef NDEBUG
#undef NDEBUG
#endif
#include "sentinel_baseline.h"
#include "settings.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "baseline_settings_impl.h"

static unsigned checks, count_calls, trust_calls;
static sentinel_baseline_t *active_output;
static cerberus_c5_wifi_result_t rows[CERBERUS_WIFI_SCAN_MAX_RESULTS];
#define CHECK(value) do { ++checks; assert(value); } while (0)

static void check_unpublished(void)
{
    sentinel_baseline_t pending = {0}; pending.checked = true;
    CHECK(active_output && !memcmp(active_output, &pending, sizeof(pending)));
}

size_t cerberus_settings_wifi_trusted_count(void)
{
    ++count_calls; check_unpublished();
    return production_trusted_count();
}

cerberus_wifi_trust_state_t cerberus_settings_wifi_trust_state(
    const char *ssid, const uint8_t bssid[6], uint8_t security)
{
    ++trust_calls; check_unpublished();
    return production_trust_state(ssid, bssid, security);
}

static void prepare(void)
{
    memset(rows, 0, sizeof(rows));
    memset(&wifi_trusted, 0, sizeof(wifi_trusted));
    settings_ready = true;
    for (size_t i = 0; i < CERBERUS_WIFI_SCAN_MAX_RESULTS; ++i) {
        snprintf(rows[i].ssid, sizeof(rows[i].ssid), "network-%u", (unsigned)i);
        rows[i].bssid[0] = 2U; rows[i].bssid[5] = (uint8_t)i;
        rows[i].channel = i % 2U ? 36U : 1U;
        rows[i].security = CERBERUS_WIFI_SECURITY_WPA2;
        rows[i].rssi = -50;
    }
}

static void trust(size_t slot, size_t row)
{
    CHECK(slot < CERBERUS_WIFI_TRUST_MAX && row < CERBERUS_WIFI_SCAN_MAX_RESULTS);
    memcpy(wifi_trusted.entries[slot].bssid, rows[row].bssid, 6U);
    memcpy(wifi_trusted.entries[slot].ssid, rows[row].ssid, sizeof(rows[row].ssid));
    wifi_trusted.entries[slot].security = rows[row].security;
    if (wifi_trusted.count <= slot) wifi_trusted.count = (uint8_t)(slot + 1U);
}

static sentinel_baseline_t compare(const cerberus_c5_wifi_result_t *bank,
    size_t count, uint8_t generation, bool available, esp_err_t expected)
{
    struct { unsigned left; sentinel_baseline_t value; unsigned right; } guarded;
    memset(&guarded, 0xA5, sizeof(guarded));
    unsigned canary = guarded.left;
    cerberus_c5_wifi_result_t before[32]; memcpy(before, rows, sizeof(rows));
    wifi_trusted_record_t saved = wifi_trusted;
    count_calls = trust_calls = 0U;
    active_output = &guarded.value;
    CHECK(sentinel_baseline_compare(active_output, bank, count, generation, available) == expected);
    active_output = NULL;
    CHECK(guarded.left == canary && guarded.right == canary);
    CHECK(!memcmp(rows, before, sizeof(rows)));
    CHECK(!memcmp(&wifi_trusted, &saved, sizeof(saved)));
    CHECK(guarded.value.checked && guarded.value.error == expected);
    if (expected == ESP_OK) {
        CHECK(count_calls == 1U && trust_calls == count);
        CHECK(guarded.value.generation == generation && guarded.value.compared == count);
        CHECK(guarded.value.saved == wifi_trusted.count);
        CHECK(guarded.value.exact + guarded.value.review + guarded.value.other == count);
    } else {
        sentinel_baseline_t failure = {0}; failure.checked = true; failure.error = expected;
        CHECK(!memcmp(&guarded.value, &failure, sizeof(failure)));
        CHECK(!count_calls && !trust_calls);
    }
    return guarded.value;
}

static void exact_and_related(void)
{
    prepare(); trust(0U, 0U);
    /* Same name with another BSSID, same BSSID with a changed name/security,
     * unrelated name, and hidden name follow the actual settings semantics. */
    memcpy(rows[1].ssid, rows[0].ssid, sizeof(rows[0].ssid));
    memcpy(rows[2].bssid, rows[0].bssid, 6U);
    rows[3] = rows[0]; rows[3].security = CERBERUS_WIFI_SECURITY_OPEN;
    rows[5] = rows[0]; rows[5].ssid[0] = '\0';
    sentinel_baseline_t out = compare(rows, 6U, 255U, true, ESP_OK);
    CHECK(out.exact == 1U && out.review == 3U && out.other == 2U);
    trust(1U, 1U); /* An earlier related entry cannot hide a later exact one. */
    out = compare(rows, 6U, 255U, true, ESP_OK);
    CHECK(out.exact == 2U && out.review == 2U && out.other == 2U);
    wifi_trusted.count = 0U;
    out = compare(rows, 6U, 255U, true, ESP_OK);
    CHECK(out.other == 6U && !out.exact && !out.review);
    prepare(); rows[1] = rows[0]; trust(0U, 0U);
    out = compare(rows, 2U, 0U, true, ESP_OK);
    CHECK(out.exact == 2U); /* Counts retained rows, not unique BSSIDs. */
}

static void raw_names(void)
{
    for (unsigned byte = 1U; byte <= UINT8_MAX; ++byte) {
        prepare();
        rows[0].ssid[0] = (char)byte; rows[0].ssid[1] = '\0';
        strcpy(rows[1].ssid, "?");
        trust(0U, 0U);
        sentinel_baseline_t out = compare(rows, 2U, 9U, true, ESP_OK);
        CHECK(out.exact == 1U && out.review == (byte == '?') && out.other == (byte != '?'));
    }
    for (size_t length = 1U; length <= 32U; ++length) {
        prepare(); memset(rows[0].ssid, 'x', length); rows[0].ssid[length] = '\0';
        trust(0U, 0U); rows[1] = rows[0]; rows[1].bssid[5] = 1U;
        rows[1].ssid[length - 1U] = 'X';
        sentinel_baseline_t out = compare(rows, 2U, 1U, true, ESP_OK);
        CHECK(out.exact == 1U && out.other == 1U && !out.review);
    }
    prepare(); rows[0].ssid[0] = '\0'; trust(0U, 0U);
    CHECK(compare(rows, 1U, 1U, true, ESP_OK).other == 1U);
    prepare(); strcpy(rows[0].ssid, "one"); trust(0U, 0U);
    rows[0].ssid[4] = 'X'; /* Bytes after terminator are not received-string identity. */
    CHECK(compare(rows, 1U, 1U, true, ESP_OK).exact == 1U);
}

static void bounded_and_empty(void)
{
    prepare();
    for (size_t i = 0U; i < CERBERUS_WIFI_TRUST_MAX; ++i) trust(i, i);
    for (size_t count = 0U; count <= CERBERUS_WIFI_SCAN_MAX_RESULTS; ++count) {
        for (unsigned generation = 0U; generation <= UINT8_MAX; ++generation) {
            sentinel_baseline_t out = compare(rows, count, (uint8_t)generation, true, ESP_OK);
            size_t exact = count < CERBERUS_WIFI_TRUST_MAX ? count : CERBERUS_WIFI_TRUST_MAX;
            CHECK(out.exact == exact && !out.review && out.other == count - exact);
        }
    }
    CHECK(compare(NULL, 0U, 0U, true, ESP_OK).saved == CERBERUS_WIFI_TRUST_MAX);
    for (unsigned security = 0U; security <= CERBERUS_WIFI_SECURITY_OTHER; ++security) {
        prepare(); rows[0].security = (uint8_t)security; trust(0U, 0U);
        CHECK(compare(rows, 1U, 0U, true, ESP_OK).exact == 1U);
    }
}

static void malformed_and_unavailable(void)
{
    prepare();
    count_calls = trust_calls = 0U;
    CHECK(sentinel_baseline_compare(NULL, rows, 1U, 1U, true) == ESP_ERR_INVALID_ARG);
    CHECK(!count_calls && !trust_calls);
    compare(NULL, 1U, 1U, true, ESP_ERR_INVALID_ARG);
    compare(rows, 33U, 1U, true, ESP_ERR_INVALID_RESPONSE);
    compare(rows, SIZE_MAX, 1U, true, ESP_ERR_INVALID_RESPONSE);
    compare(NULL, 0U, 1U, false, ESP_ERR_INVALID_STATE);
    for (size_t index = 0U; index < CERBERUS_WIFI_SCAN_MAX_RESULTS; ++index) {
        for (unsigned failure = 0U; failure < 3U; ++failure) {
            prepare(); trust(0U, 0U);
            if (!failure) memset(rows[index].ssid, 'X', sizeof(rows[index].ssid));
            else if (failure == 1U) rows[index].channel = 0U;
            else rows[index].security = CERBERUS_WIFI_SECURITY_OTHER + 1U;
            compare(rows, 32U, 4U, true, ESP_ERR_INVALID_RESPONSE);
        }
    }
    prepare(); trust(0U, 0U);
    compare(rows, 32U, 4U, false, ESP_ERR_INVALID_STATE);
    settings_ready = false;
    compare(rows, 32U, 4U, false, ESP_ERR_INVALID_STATE);
}

int main(void)
{
    exact_and_related(); raw_names(); bounded_and_empty(); malformed_and_unavailable();
    printf("Cached baseline: %u checks; actual trust semantics, raw names, atomic validation and unavailable settings pass\n", checks);
    return 0;
}
