#include "wifi_sae_session.h"
#include "c5_spi_link.h"
#include "esp_heap_caps.h"
#include "freertos/semphr.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int mutex_token, locked;
static bool fail_mutex = true, fail_lock, fail_alloc, finish_during_start;
static bool replace_after_status, replace_before_return;
static unsigned allocations, frees, live, starts, stops;
static size_t store_bytes;
static uint32_t firmware = 85U, generation = 40U;
static int64_t clock_us = 50000000;
static esp_err_t peer_error, start_error;
static cerberus_s3_capture_status_t capture;
static cerberus_s3_capture_options_t request;
static const uint8_t ap[6] = {2, 1, 2, 3, 4, 5};

int64_t esp_timer_get_time(void) { return clock_us; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return fail_mutex ? NULL : &mutex_token; }
int xSemaphoreTake(SemaphoreHandle_t mutex, uint32_t ticks)
{
    (void)ticks; assert(mutex == &mutex_token && !locked);
    if (fail_lock) return 0;
    locked = 1; return 1;
}
int xSemaphoreGive(SemaphoreHandle_t mutex)
{ assert(mutex == &mutex_token && locked); locked = 0; return 1; }
void *heap_caps_calloc(size_t count, size_t size, unsigned caps)
{
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (fail_alloc) return NULL;
    void *p = calloc(count, size); assert(p);
    store_bytes = count * size; ++allocations; ++live; assert(live <= 2U); return p;
}
void heap_caps_free(void *p)
{ if (p) { ++frees; --live; free(p); } }
esp_err_t c5_spi_link_get_status(cerberus_c5_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (peer_error == ESP_OK) out->firmware_version = firmware;
    return peer_error;
}
void cerberus_s3_capture_status(cerberus_s3_capture_status_t *out)
{
    *out = capture;
    if (replace_after_status) {
        replace_after_status = false; capture.generation = ++generation; capture.busy = true;
    }
}
esp_err_t cerberus_s3_capture_start_options_generation(
    const cerberus_s3_capture_options_t *options, uint32_t *out)
{
    ++starts; assert(!capture.busy && locked); *out = 0U;
    if (start_error != ESP_OK) return start_error;
    request = *options;
    assert(request.mode_mask == CERBERUS_WIFI_CAPTURE_MODE_SAE_COMMIT);
    assert(request.observer && request.observer_context);
    if (request.plan) {
        assert(request.hopping && !request.channel && !request.filter_bssid);
        assert(cerberus_wifi_capture_plan_valid(request.plan));
    }
    capture = (cerberus_s3_capture_status_t){
        .generation = ++generation, .state = CERBERUS_S3_CAPTURE_RUNNING, .busy = true,
        .channel = request.channel, .mode_mask = request.mode_mask,
        .filter_bssid = request.filter_bssid, .hopping = request.hopping,
        .save_pcap = request.save_pcap, .planned = request.plan != NULL,
    };
    *out = capture.generation;
    if (finish_during_start) {
        finish_during_start = false; capture.busy = false;
        capture.state = CERBERUS_S3_CAPTURE_ERROR; capture.error_code = ESP_ERR_TIMEOUT;
    }
    if (replace_before_return) { replace_before_return = false; capture.generation = ++generation; }
    return ESP_OK;
}
bool cerberus_s3_capture_stop_generation(uint32_t expected)
{
    if (!expected || !capture.busy || capture.generation != expected) return false;
    ++stops; capture.state = CERBERUS_S3_CAPTURE_STOPPING; return true;
}
static void finish(void)
{
    capture.busy = false;
    capture.state = capture.save_pcap ? CERBERUS_S3_CAPTURE_SAVED : CERBERUS_S3_CAPTURE_COMPLETE;
}
static cerberus_s3_sae_status_t state(void)
{
    cerberus_s3_sae_status_t out;
    assert(cerberus_s3_sae_status(&out) == ESP_OK); return out;
}
static void receive(uint32_t expected, uint32_t sequence, uint64_t source_us,
    uint8_t channel, bool wrong_bssid)
{
    uint8_t frame[30] = {0xb0U};
    const uint8_t sta[6] = {4, 1, 2, 3, 4, 5};
    memcpy(frame + 4U, sta, 6U); memcpy(frame + 10U, ap, 6U); memcpy(frame + 16U, ap, 6U);
    if (wrong_bssid) frame[21] = 99U;
    frame[24] = 3U; frame[26] = 1U; frame[28] = 1U;
    const cerberus_s3_capture_observation_t meta = {
        .generation = expected, .sequence = sequence, .channel = channel,
        .rssi_dbm = -48, .observed_us = source_us,
    };
    request.observer(&meta, frame, sizeof(frame), request.observer_context);
}

int main(void)
{
    cerberus_s3_sae_options_t options = {.channel = 6U, .filter_bssid = true};
    memcpy(options.bssid, ap, 6U);
    cerberus_s3_sae_status_t out;
    assert(cerberus_s3_sae_status(&out) == ESP_ERR_INVALID_STATE && !out.generation);
    assert(cerberus_s3_sae_init() == ESP_ERR_NO_MEM);
    fail_mutex = false; assert(cerberus_s3_sae_init() == ESP_OK);
    assert(cerberus_s3_sae_init() == ESP_OK && !allocations);
    assert(cerberus_s3_sae_start(NULL) == ESP_ERR_INVALID_ARG);
    options.channel = 32U; assert(cerberus_s3_sae_start(&options) == ESP_ERR_INVALID_ARG);
    options.channel = 6U; assert(cerberus_s3_sae_start(&options) == ESP_ERR_NOT_SUPPORTED);
    assert(!starts && !allocations);
    firmware = CERBERUS_WIFI_SAE_MONITOR_MIN_FIRMWARE;
    peer_error = ESP_ERR_TIMEOUT; assert(cerberus_s3_sae_start(&options) == ESP_ERR_TIMEOUT);
    peer_error = ESP_OK;
    fail_lock = true; assert(cerberus_s3_sae_start(&options) == ESP_ERR_TIMEOUT); fail_lock = false;
    fail_alloc = true; assert(cerberus_s3_sae_start(&options) == ESP_ERR_NO_MEM); fail_alloc = false;
    assert(cerberus_s3_sae_start(&options) == ESP_OK);
    uint32_t owned = state().generation;
    assert(state().capture.busy && !state().planned && !request.save_pcap && !request.plan);
    assert(cerberus_s3_sae_start(&options) == ESP_ERR_INVALID_STATE);
    assert(cerberus_s3_sae_release() == ESP_ERR_INVALID_STATE && live == 1U);
    receive(owned, 1U, 100U, 6U, false);
    assert(state().summary.observations == 1U && state().summary.other_status == 1U);
    cerberus_s3_sae_row_t rows[4]; cerberus_s3_sae_page_t page;
    assert(cerberus_s3_sae_page(0U, rows, 2U, &page) == ESP_OK);
    assert(page.total == 1U && page.copied == 1U && page.generation == owned);
    assert(rows[0].evidence.last_us == 100U && rows[0].last_received_age_ms == 0U);
    clock_us += 500000;
    receive(owned, 1U, 100U, 6U, false);
    receive(owned + 1U, 2U, 200U, 6U, false);
    receive(owned, 2U, 200U, 36U, false);
    receive(owned, 3U, 200U, 6U, true);
    receive(owned, 4U, UINT64_C(0x1000000000000), 6U, false);
    assert(state().summary.observations == 1U && state().rejected_provenance == 2U);
    assert(state().summary.scope_rejected == 2U && state().summary.sequence_rejected == 1U);
    assert(cerberus_s3_sae_page(0U, rows, 2U, &page) == ESP_OK && rows[0].last_received_age_ms == 500U);
    receive(owned, 4U, 300U, 6U, false);
    assert(cerberus_s3_sae_page(0U, rows, 2U, &page) == ESP_OK && rows[0].last_received_age_ms == 0U);
    assert(rows[0].first_received_age_ms == 500U && rows[0].evidence.first_sequence == 1U);
    clock_us = 0;
    assert(cerberus_s3_sae_page(0U, rows, 2U, &page) == ESP_OK && rows[0].last_received_age_ms == UINT32_MAX);
    clock_us = 51000000;
    fail_lock = true; memset(rows, 0x5a, sizeof(rows)); memset(&page, 0x5a, sizeof(page));
    assert(cerberus_s3_sae_page(0U, rows, 2U, &page) == ESP_ERR_TIMEOUT && !rows[0].generation && !page.generation);
    fail_lock = false;
    cerberus_s3_sae_stop(); assert(stops == 1U && state().capture.busy);
    capture.error_code = ESP_ERR_TIMEOUT;
    assert(cerberus_s3_sae_release() == ESP_ERR_INVALID_STATE && live == 1U);
    assert(state().capture.error_code == ESP_ERR_TIMEOUT);
    finish(); assert(!state().capture.busy && state().summary.observations == 2U);
    start_error = ESP_FAIL; assert(cerberus_s3_sae_start(&options) == ESP_FAIL); start_error = ESP_OK;
    assert(state().generation == owned && state().summary.count == 1U && live == 1U);

    cerberus_wifi_capture_plan_t plan;
    cerberus_wifi_capture_plan_clear(&plan);
    assert(cerberus_wifi_capture_plan_add_target(&plan, ap, 6U));
    assert(cerberus_wifi_capture_plan_add_target(&plan, ap, 36U));
    options = (cerberus_s3_sae_options_t){.plan = &plan, .save_pcap = true};
    firmware = 85U; assert(cerberus_s3_sae_start(&options) == ESP_ERR_NOT_SUPPORTED);
    firmware = 86U; assert(cerberus_s3_sae_start(&options) == ESP_OK);
    owned = state().generation;
    assert(state().planned && request.hopping && request.save_pcap && request.plan != &plan);
    uint64_t channels = plan.channel_mask;
    memset(&plan, 0, sizeof(plan));
    assert(request.plan->channel_mask == channels && state().plan.channel_mask == channels);
    receive(owned, 1U, 500U, 36U, false);
    receive(owned, 2U, 600U, 6U, false);
    receive(owned, 3U, 700U, 11U, false);
    receive(owned, 4U, 800U, 6U, true);
    assert(state().summary.observations == 2U && state().summary.scope_rejected == 2U);
    capture.hop_valid = true; capture.attempted_mask = channels;
    capture.applied_mask = cerberus_wifi_capture_plan_channel_bit(6U);
    capture.completed_sweeps = 7U; capture.hop_error = ESP_FAIL;
    assert(state().capture.hop_valid && state().capture.completed_sweeps == 7U);
    assert(state().capture.applied_mask != state().capture.attempted_mask && state().capture.hop_error == ESP_FAIL);
    replace_after_status = true; unsigned prior_stops = stops;
    cerberus_s3_sae_stop(); assert(stops == prior_stops);
    assert(!state().capture.busy && state().capture.error_code == ESP_ERR_INVALID_STATE);
    assert(state().generation == owned && state().capture.completed_sweeps == 7U);
    assert(cerberus_s3_sae_release() == ESP_OK && !live && capture.busy); finish();

    options = (cerberus_s3_sae_options_t){.channel = 6U};
    finish_during_start = true;
    assert(cerberus_s3_sae_start(&options) == ESP_OK && !state().capture.busy);
    assert(state().capture.error_code == ESP_ERR_TIMEOUT);
    assert(cerberus_s3_sae_release() == ESP_OK && !live);
    replace_before_return = true;
    assert(cerberus_s3_sae_start(&options) == ESP_OK);
    assert(state().generation != capture.generation && !state().capture.busy);
    assert(cerberus_s3_sae_release() == ESP_OK && !live && capture.busy); finish();
    assert(cerberus_s3_sae_release() == ESP_OK && !state().generation);
    assert(allocations == frees && store_bytes < 16384U);
    printf("SAE session: cap86, fixed/planned frozen scope, ages, cleanup and retained telemetry; %zu-byte PSRAM store\n", store_bytes);
    return 0;
}
