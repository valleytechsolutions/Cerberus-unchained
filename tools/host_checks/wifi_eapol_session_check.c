#include "wifi_eapol_session.h"
#include "c5_spi_link.h"
#include "esp_heap_caps.h"
#include "freertos/semphr.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int mutex_token, lock_depth;
static bool fail_mutex = true, fail_lock, fail_alloc, finish_during_start;
static bool replace_after_status, replace_before_start_return;
static unsigned allocated, freed, live, starts, stops, status_queries;
static size_t largest_store;
static uint32_t firmware = 84U, next_generation = 20U;
static int64_t local_clock = 50000000;
static esp_err_t peer_error, start_error;
static cerberus_s3_capture_status_t capture;
static cerberus_s3_capture_options_t options;
static const uint8_t ap[6] = {0x20, 1, 2, 3, 4, 5};

int64_t esp_timer_get_time(void) { return local_clock; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return fail_mutex ? NULL : &mutex_token; }
int xSemaphoreTake(SemaphoreHandle_t mutex, uint32_t ticks)
{
    (void)ticks;
    assert(mutex == &mutex_token && !lock_depth);
    if (fail_lock) return 0;
    lock_depth = 1;
    return 1;
}
int xSemaphoreGive(SemaphoreHandle_t mutex)
{
    assert(mutex == &mutex_token && lock_depth == 1);
    lock_depth = 0;
    return 1;
}
void *heap_caps_calloc(size_t count, size_t size, unsigned caps)
{
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (fail_alloc) return NULL;
    void *p = calloc(count, size);
    assert(p);
    if (count * size > largest_store) largest_store = count * size;
    ++allocated;
    ++live;
    assert(live <= 2U);
    return p;
}
void heap_caps_free(void *p)
{
    if (p) { ++freed; --live; free(p); }
}
esp_err_t c5_spi_link_get_status(cerberus_c5_status_t *out)
{
    ++status_queries;
    memset(out, 0, sizeof(*out));
    if (peer_error == ESP_OK) out->firmware_version = firmware;
    return peer_error;
}
void cerberus_s3_capture_status(cerberus_s3_capture_status_t *out)
{
    *out = capture;
    if (replace_after_status) {
        replace_after_status = false;
        capture.generation = ++next_generation;
        capture.busy = true;
    }
}
esp_err_t cerberus_s3_capture_start_options_generation(
    const cerberus_s3_capture_options_t *request, uint32_t *generation)
{
    ++starts;
    assert(!capture.busy && lock_depth == 1);
    *generation = 0U;
    if (start_error != ESP_OK) return start_error;
    options = *request;
    assert(options.observer && options.observer_context);
    assert(options.mode_mask == (CERBERUS_WIFI_CAPTURE_MODE_EAPOL | CERBERUS_WIFI_CAPTURE_MODE_BEACON));
    if (options.plan) assert(options.hopping && !options.filter_bssid && !options.channel);
    else assert(!options.hopping && (!options.filter_bssid || !memcmp(options.bssid, ap, 6U)));
    capture = (cerberus_s3_capture_status_t){
        .generation = ++next_generation, .state = CERBERUS_S3_CAPTURE_RUNNING,
        .busy = true, .channel = options.channel, .mode_mask = options.mode_mask,
        .filter_bssid = options.filter_bssid, .save_pcap = options.save_pcap,
        .hopping = options.hopping, .planned = options.plan != NULL,
    };
    *generation = capture.generation;
    if (finish_during_start) {
        finish_during_start = false;
        capture.busy = false;
        capture.state = CERBERUS_S3_CAPTURE_ERROR;
        capture.error_code = ESP_ERR_TIMEOUT;
    }
    if (replace_before_start_return) {
        replace_before_start_return = false;
        capture.generation = ++next_generation;
    }
    return ESP_OK;
}
bool cerberus_s3_capture_stop_generation(uint32_t generation)
{
    if (!generation || !capture.busy || capture.generation != generation) return false;
    ++stops;
    capture.state = CERBERUS_S3_CAPTURE_STOPPING;
    return true;
}

static void finish(void)
{
    capture.busy = false;
    capture.state = capture.save_pcap ? CERBERUS_S3_CAPTURE_SAVED : CERBERUS_S3_CAPTURE_COMPLETE;
}

static cerberus_s3_eapol_status_t state(void)
{
    cerberus_s3_eapol_status_t result;
    assert(cerberus_s3_eapol_status(&result) == ESP_OK);
    return result;
}

static size_t m1_frame(uint8_t frame[160], uint8_t station, uint64_t replay)
{
    memset(frame, 0, 160U);
    frame[0] = 8U; frame[1] = 2U; /* From DS: AP transmitter -> station. */
    const uint8_t sta[6] = {0x30, 1, 2, 3, 4, station};
    memcpy(frame + 4U, sta, 6U);
    memcpy(frame + 10U, ap, 6U);
    memcpy(frame + 16U, ap, 6U);
    const uint8_t llc[8] = {0xaa, 0xaa, 3, 0, 0, 0, 0x88, 0x8e};
    memcpy(frame + 24U, llc, 8U);
    uint8_t *eapol = frame + 32U;
    eapol[0] = 2U; eapol[1] = 3U; eapol[3] = 95U;
    eapol[4] = 2U; eapol[6] = 0x8aU; eapol[8] = 16U;
    for (unsigned i = 0U; i < 8U; ++i) eapol[9U + i] = (uint8_t)(replay >> (56U - 8U * i));
    memset(eapol + 17U, 0x11, 32U);
    return 131U;
}

static void send_m1(uint32_t generation, uint32_t sequence, uint8_t station,
    uint64_t source_us, uint8_t channel)
{
    uint8_t frame[160], saved[160];
    size_t length = m1_frame(frame, station, 1U);
    memcpy(saved, frame, sizeof(frame));
    cerberus_s3_capture_observation_t metadata = {
        .generation = generation, .sequence = sequence, .channel = channel,
        .rssi_dbm = -47, .observed_us = source_us,
    };
    options.observer(&metadata, frame, length, options.observer_context);
    assert(!memcmp(frame, saved, sizeof(frame)));
}

static void observe(uint32_t generation, uint32_t sequence, uint64_t source_us,
    uint8_t channel, const uint8_t *frame, size_t length)
{
    cerberus_s3_capture_observation_t metadata = {.generation=generation,.sequence=sequence,
        .observed_us=source_us,.channel=channel,.rssi_dbm=-39};
    options.observer(&metadata,frame,length,options.observer_context);
}

static size_t beacon(uint8_t frame[160], const uint8_t bssid[6])
{
    memset(frame,0,160); frame[0]=0x80; memset(frame+4,0xff,6);
    memcpy(frame+10,bssid,6); memcpy(frame+16,bssid,6);
    frame[34]=0x11; frame[36]=0; frame[37]=4;
    memcpy(frame+38,"A\0\r\xff",4); frame[42]=3; frame[43]=1; frame[44]=36;
    return 45;
}

static void planned_and_context(void)
{
    const uint8_t other_ap[6]={0x22,1,2,3,4,5};
    cerberus_wifi_capture_plan_t plan;
    cerberus_wifi_capture_plan_clear(&plan);
    plan.dwell_ms=CERBERUS_WIFI_CAPTURE_PLAN_EAPOL_DWELL_MS;
    assert(cerberus_wifi_capture_plan_add_target(&plan,ap,6));
    assert(cerberus_wifi_capture_plan_add_target(&plan,other_ap,36));
    const cerberus_wifi_capture_plan_t frozen=plan;
    cerberus_s3_eapol_options_t request={.plan=&plan};
    unsigned old_starts=starts;
    request.channel=6; assert(cerberus_s3_eapol_start_options(&request)==ESP_ERR_INVALID_ARG);
    request.channel=0; request.filter_bssid=true;
    assert(cerberus_s3_eapol_start_options(&request)==ESP_ERR_INVALID_ARG);
    request.filter_bssid=false; plan.dwell_ms=250;
    assert(cerberus_s3_eapol_start_options(&request)==ESP_ERR_INVALID_ARG);
    plan=frozen; firmware=86;
    assert(cerberus_s3_eapol_start_options(&request)==ESP_ERR_NOT_SUPPORTED);
    assert(starts==old_starts&&!live); firmware=CERBERUS_WIFI_EAPOL_PLAN_MIN_FIRMWARE;
    assert(cerberus_s3_eapol_start_options(&request)==ESP_OK);
    assert(options.hopping&&options.plan&&options.plan!=&plan&&!options.filter_bssid&&!options.save_pcap);
    assert(state().planned&&state().capture.planned&&state().capture.hopping&&state().channel==0);
    uint32_t generation=state().generation;
    memset(&plan,0,sizeof(plan));
    cerberus_s3_eapol_status_t current=state();
    assert(!memcmp(&current.plan,&frozen,sizeof(frozen)));
    assert(!memcmp(options.plan,&frozen,sizeof(frozen)));
    uint8_t frame[160]; size_t length;
    local_clock=70000000; length=m1_frame(frame,1,1); observe(generation,1,1000,6,frame,length);
    local_clock+=500000; memcpy(frame+10,other_ap,6); observe(generation,2,2000,36,frame,length);
    assert(state().summary.count==2&&state().summary.keys==2);
    cerberus_s3_eapol_row_t rows[4]; cerberus_s3_eapol_context_row_t contexts[4];
    cerberus_s3_eapol_page_t page;
    assert(cerberus_s3_eapol_page(0,rows,2,&page)==ESP_OK&&page.copied==2);
    assert(!memcmp(rows[0].evidence.bssid,ap,6)&&!memcmp(rows[1].evidence.bssid,other_ap,6));
    assert(!memcmp(rows[0].evidence.station,rows[1].evidence.station,6));
    local_clock+=500000; length=beacon(frame,ap); observe(generation,3,3000,6,frame,length);
    assert(state().summary.keys==2&&state().summary.beacons==1&&state().summary.context_count==1);
    assert(cerberus_s3_eapol_page(0,rows,2,&page)==ESP_OK);
    assert(rows[0].last_received_age_ms==1000&&rows[1].last_received_age_ms==500);
    assert(cerberus_s3_eapol_context_page(0,contexts,2,&page)==ESP_OK&&page.copied==1);
    assert(contexts[0].evidence.sequence==3&&contexts[0].evidence.last_us==3000);
    assert(contexts[0].last_received_age_ms==0&&contexts[0].evidence.channel==6&&contexts[0].evidence.advertised_channel==36);
    assert(contexts[0].evidence.ssid_length==4&&!memcmp(contexts[0].evidence.ssid,"A\0\r\xff",4));
    const cerberus_s3_eapol_context_row_t saved_context=contexts[0];
    local_clock+=250000; observe(generation,3,3000,6,frame,length);
    observe(generation+1,4,4000,6,frame,length);
    observe(generation,4,4000,14,frame,length);
    assert(state().summary.sequence_rejected==1&&state().rejected_provenance==2);
    assert(cerberus_s3_eapol_context_page(0,contexts,2,&page)==ESP_OK);
    assert(contexts[0].last_received_age_ms==250&&!memcmp(&saved_context.evidence,&contexts[0].evidence,sizeof(saved_context.evidence)));
    observe(generation,4,1000,6,frame,length); assert(state().summary.clock_rejected==1);
    local_clock+=250000; observe(generation,5,4000,6,frame,length);
    assert(cerberus_s3_eapol_context_page(0,contexts,2,&page)==ESP_OK);
    assert(contexts[0].last_received_age_ms==0&&contexts[0].first_received_age_ms==500);
    assert(contexts[0].evidence.frames==2&&contexts[0].evidence.first_sequence==3);
    local_clock+=250000; length=m1_frame(frame,1,1); observe(generation,6,5000,6,frame,length);
    assert(cerberus_s3_eapol_page(0,rows,2,&page)==ESP_OK&&rows[0].last_received_age_ms==0);
    assert(rows[1].last_received_age_ms==1250&&state().summary.keys==3);
    assert(cerberus_s3_eapol_context_page(0,contexts,2,&page)==ESP_OK&&contexts[0].last_received_age_ms==250);
    local_clock=-1;
    assert(cerberus_s3_eapol_context_page(0,contexts,2,&page)==ESP_OK&&contexts[0].last_received_age_ms==UINT32_MAX);
    local_clock=72000000;
    assert(cerberus_s3_eapol_context_page(1,contexts,2,&page)==ESP_OK&&!page.copied&&page.total==1);
    memset(contexts,0x5a,sizeof(contexts)); memset(&page,0x5a,sizeof(page)); fail_lock=true;
    assert(cerberus_s3_eapol_context_page(0,contexts,2,&page)==ESP_ERR_TIMEOUT);
    assert(!contexts[0].generation&&!page.generation); fail_lock=false;
    assert(cerberus_s3_eapol_context_page(0,contexts,5,&page)==ESP_ERR_INVALID_ARG);
    assert(cerberus_s3_eapol_context_page(0,NULL,1,&page)==ESP_ERR_INVALID_ARG);
    assert(cerberus_s3_eapol_context_page(0,NULL,0,&page)==ESP_OK&&page.total==1);
    cerberus_s3_eapol_stop(); assert(state().capture.busy&&cerberus_s3_eapol_release()==ESP_ERR_INVALID_STATE);
    finish(); assert(state().summary.keys==3&&state().summary.context_count==1&&!state().capture.busy);
    plan=frozen; fail_alloc=true;
    assert(cerberus_s3_eapol_start_options(&request)==ESP_ERR_NO_MEM); fail_alloc=false;
    assert(state().generation==generation&&state().summary.beacons==2&&state().summary.keys==3);
    request.save_pcap=true; assert(cerberus_s3_eapol_start_options(&request)==ESP_OK);
    assert(options.save_pcap&&!state().summary.count&&!state().summary.context_count);
    finish(); assert(cerberus_s3_eapol_release()==ESP_OK&&!live);
    /* A fixed channel may deliberately include all infrastructure BSSIDs. */
    request=(cerberus_s3_eapol_options_t){.channel=6};
    assert(cerberus_s3_eapol_start_options(&request)==ESP_OK&&!options.filter_bssid&&!options.plan);
    generation=state().generation; length=m1_frame(frame,1,1); memcpy(frame+10,other_ap,6);
    observe(generation,1,100,6,frame,length);
    assert(state().summary.count==1&&!state().filter_bssid&&!state().planned);
    finish(); assert(cerberus_s3_eapol_release()==ESP_OK&&!live);
}

int main(void)
{
    cerberus_s3_eapol_status_t status;
    memset(&status, 0x5a, sizeof(status));
    assert(cerberus_s3_eapol_status(&status) == ESP_ERR_INVALID_STATE && !status.generation);
    assert(cerberus_s3_eapol_init() == ESP_ERR_NO_MEM && !allocated);
    fail_mutex = false;
    assert(cerberus_s3_eapol_init() == ESP_OK);
    assert(cerberus_s3_eapol_init() == ESP_OK && !allocated);
    assert(!state().generation && !state().capture.busy);
    const uint8_t invalid[6] = {0};
    assert(cerberus_s3_eapol_start(0U, ap, false) == ESP_ERR_INVALID_ARG);
    assert(cerberus_s3_eapol_start(6U, invalid, false) == ESP_ERR_INVALID_ARG);
    assert(cerberus_s3_eapol_start(6U, NULL, false) == ESP_ERR_INVALID_ARG);
    assert(!starts && !status_queries);
    assert(cerberus_s3_eapol_start(6U, ap, false) == ESP_ERR_NOT_SUPPORTED);
    assert(!allocated && !starts);
    firmware = 86U;
    assert(cerberus_s3_eapol_start(6U, ap, false) == ESP_ERR_NOT_SUPPORTED && !allocated);
    firmware = CERBERUS_WIFI_EAPOL_PLAN_MIN_FIRMWARE;
    peer_error = ESP_ERR_TIMEOUT;
    assert(cerberus_s3_eapol_start(6U, ap, false) == ESP_ERR_TIMEOUT && !allocated);
    peer_error = ESP_OK;
    fail_lock = true;
    assert(cerberus_s3_eapol_start(6U, ap, false) == ESP_ERR_TIMEOUT);
    fail_lock = false;
    fail_alloc = true;
    assert(cerberus_s3_eapol_start(6U, ap, false) == ESP_ERR_NO_MEM);
    fail_alloc = false;
    assert(cerberus_s3_eapol_start(6U, ap, false) == ESP_OK);
    assert(!options.save_pcap && state().capture.busy && live == 1U);
    uint32_t generation = state().generation;
    assert(generation == capture.generation && state().channel == 6U);
    assert(!memcmp(state().bssid, ap, 6U));
    assert(cerberus_s3_eapol_start(6U, ap, true) == ESP_ERR_INVALID_STATE);
    assert(cerberus_s3_eapol_release() == ESP_ERR_INVALID_STATE && live == 1U);
    send_m1(generation, 1U, 1U, 100000U, 6U);
    assert(state().summary.count == 1U && state().summary.keys == 1U);
    assert(state().summary.phase[0] == 1U);
    cerberus_s3_eapol_row_t rows[4];
    cerberus_s3_eapol_page_t page;
    assert(cerberus_s3_eapol_page(0U, rows, 2U, &page) == ESP_OK);
    assert(page.generation == generation && page.total == 1U && page.copied == 1U);
    assert(rows[0].evidence.first_us == 100000U && rows[0].last_received_age_ms == 0U);
    cerberus_s3_eapol_row_t frozen = rows[0];
    local_clock += 500000;
    send_m1(generation, 1U, 1U, 100000U, 6U); /* SPI duplicate cannot refresh. */
    send_m1(generation + 1U, 2U, 1U, 200000U, 6U);
    send_m1(generation, 2U, 1U, 200000U, 11U);
    send_m1(generation, 2U, 1U, UINT64_C(0x1000000000000), 6U);
    assert(state().summary.keys == 1U && state().rejected_provenance == 3U);
    assert(cerberus_s3_eapol_page(0U, rows, 2U, &page) == ESP_OK);
    assert(rows[0].last_received_age_ms == 500U);
    send_m1(generation, 2U, 1U, 200000U, 6U); /* New RF receive, same exchange. */
    assert(cerberus_s3_eapol_page(0U, rows, 2U, &page) == ESP_OK);
    assert(rows[0].last_received_age_ms == 0U && rows[0].first_received_age_ms == 500U);
    assert(rows[0].evidence.last_us == 200000U && frozen.evidence.last_us == 100000U);
    local_clock += 500000;
    uint8_t malformed[160];
    size_t malformed_size = m1_frame(malformed, 1U, 1U);
    malformed[35] = 96U; /* Declared body exceeds the complete received frame. */
    cerberus_s3_capture_observation_t metadata = {
        .generation = generation, .sequence = 3U, .channel = 6U,
        .rssi_dbm = -20, .observed_us = 300000U,
    };
    options.observer(&metadata, malformed, malformed_size, options.observer_context);
    assert(state().summary.malformed == 1U && state().summary.keys == 2U);
    assert(cerberus_s3_eapol_page(0U, rows, 2U, &page) == ESP_OK);
    assert(rows[0].last_received_age_ms == 500U && rows[0].evidence.last_us == 200000U);
    malformed[35] = 95U; malformed[33] = 0U; /* Non-Key EAPOL is counted separately. */
    metadata.sequence = 4U; metadata.observed_us = 400000U;
    options.observer(&metadata, malformed, malformed_size, options.observer_context);
    assert(state().summary.other == 1U && state().summary.count == 1U);
    send_m1(generation, 5U, 1U, 100000U, 6U);
    assert(state().summary.clock_rejected == 1U);
    local_clock = 1000;
    assert(cerberus_s3_eapol_page(0U, rows, 2U, &page) == ESP_OK);
    assert(rows[0].last_received_age_ms == UINT32_MAX);
    local_clock = 52000000;
    assert(cerberus_s3_eapol_page(1U, rows, 2U, &page) == ESP_OK && !page.copied);
    memset(rows, 0x5a, sizeof(rows)); memset(&page, 0x5a, sizeof(page));
    fail_lock = true;
    assert(cerberus_s3_eapol_page(0U, rows, 2U, &page) == ESP_ERR_TIMEOUT);
    assert(!rows[0].generation && !page.generation);
    fail_lock = false;
    cerberus_s3_eapol_stop();
    assert(stops == 1U && state().capture.busy);
    capture.error_code = ESP_ERR_TIMEOUT; /* Failed Stop still holds context. */
    assert(cerberus_s3_eapol_release() == ESP_ERR_INVALID_STATE && live == 1U);
    assert(state().capture.error_code == ESP_ERR_TIMEOUT && state().summary.keys == 2U);
    finish();
    assert(!state().capture.busy && state().summary.count == 1U);
    fail_alloc = true;
    assert(cerberus_s3_eapol_start(6U, ap, false) == ESP_ERR_NO_MEM);
    fail_alloc = false;
    start_error = ESP_ERR_INVALID_STATE;
    assert(cerberus_s3_eapol_start(6U, ap, true) == ESP_ERR_INVALID_STATE);
    start_error = ESP_OK;
    assert(state().generation == generation && state().summary.count == 1U && live == 1U);
    assert(cerberus_s3_eapol_start(36U, ap, true) == ESP_OK);
    assert(options.save_pcap && state().channel == 36U && !state().summary.count && live == 1U);
    generation = state().generation;
    send_m1(generation, 1U, 2U, 100U, 36U);
    assert(state().summary.keys == 1U);
    replace_after_status = true;
    unsigned prior_stops = stops;
    cerberus_s3_eapol_stop();
    assert(stops == prior_stops); /* Atomic exact-generation stop rejects new owner. */
    assert(!state().capture.busy && state().capture.error_code == ESP_ERR_INVALID_STATE);
    assert(state().generation == generation && capture.generation != generation);
    assert(cerberus_s3_eapol_release() == ESP_OK && !live && capture.busy);
    finish();
    /* A non-phase Key row may be evicted/reappear at the same source time.
     * The admission sequence, not timestamp/address alone, binds its age. */
    assert(cerberus_s3_eapol_start(6U, ap, false) == ESP_OK);
    generation = state().generation;
    local_clock = 60000000;
    for (unsigned i = 1U; i <= CERBERUS_WIFI_EAPOL_MAX_PEERS; ++i) {
        uint8_t frame[160]; size_t length = m1_frame(frame, (uint8_t)i, 1U);
        frame[38] = 0x82U; /* Group Key: retained, no incomplete pairwise exchange. */
        metadata = (cerberus_s3_capture_observation_t){
            .generation = generation, .sequence = i, .channel = 6U,
            .rssi_dbm = -45, .observed_us = 500U,
        };
        options.observer(&metadata, frame, length, options.observer_context);
    }
    assert(state().summary.count == CERBERUS_WIFI_EAPOL_MAX_PEERS);
    assert(cerberus_s3_eapol_page(0U, rows, 2U, &page) == ESP_OK);
    assert(rows[0].evidence.station[5] == 1U && rows[0].evidence.first_sequence == 1U);
    local_clock += 1000000;
    for (unsigned i = 0U; i < 2U; ++i) {
        uint8_t frame[160]; size_t length = m1_frame(frame, i ? 1U : 17U, 1U);
        frame[38] = 0x82U;
        metadata.sequence = CERBERUS_WIFI_EAPOL_MAX_PEERS + 1U + i;
        options.observer(&metadata, frame, length, options.observer_context);
    }
    assert(state().summary.peer_replaced == 2U);
    assert(cerberus_s3_eapol_page(0U, rows, 2U, &page) == ESP_OK);
    assert(rows[0].evidence.station[5] == 1U && rows[0].evidence.first_us == 500U);
    assert(rows[0].evidence.first_sequence == CERBERUS_WIFI_EAPOL_MAX_PEERS + 2U);
    assert(rows[0].first_received_age_ms == 0U && rows[0].last_received_age_ms == 0U);
    assert(rows[1].first_received_age_ms == 1000U);
    finish();
    assert(cerberus_s3_eapol_release() == ESP_OK && !live);
    finish_during_start = true;
    assert(cerberus_s3_eapol_start(6U, ap, false) == ESP_OK);
    assert(!state().capture.busy && state().capture.error_code == ESP_ERR_TIMEOUT);
    assert(cerberus_s3_eapol_release() == ESP_OK && !live);
    replace_before_start_return = true;
    assert(cerberus_s3_eapol_start(6U, ap, false) == ESP_OK);
    assert(state().generation != capture.generation && !state().capture.busy);
    assert(cerberus_s3_eapol_release() == ESP_OK && !live && capture.busy);
    finish();
    assert(cerberus_s3_eapol_release() == ESP_OK && !state().generation);
    planned_and_context();
    assert(allocated == freed && largest_store <= 6144U);
    printf("EAPOL session: scoped ownership, retained evidence, source/delivery clocks, cap87, errors; %zu-byte PSRAM store\n", largest_store);
    return 0;
}
