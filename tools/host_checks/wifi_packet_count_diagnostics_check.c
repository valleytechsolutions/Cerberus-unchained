#undef NDEBUG
#include "wifi_packet_count_diagnostics.h"
#include "freertos/FreeRTOS.h"
#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

static _Thread_local bool lock_held;
static atomic_uint enters, exits;
static unsigned assertions;
#define CHECK(x) do { ++assertions; assert(x); } while (0)
void count_diag_test_enter(portMUX_TYPE *lock)
{
    assert(!lock_held);
    while (atomic_flag_test_and_set_explicit(&lock->flag, memory_order_acquire)) {}
    lock_held = true;
    atomic_fetch_add(&enters, 1);
}
void count_diag_test_exit(portMUX_TYPE *lock)
{
    assert(lock_held);
    atomic_fetch_add(&exits, 1);
    lock_held = false;
    atomic_flag_clear_explicit(&lock->flag, memory_order_release);
}
/* Actual module; direct access only permits exhaustion injection, never a
 * production reset API. Radio, transport and recorder facades are not linked. */
#include "wifi_packet_count_diagnostics.c"

static char output[32768];
static size_t used, max_format;
static void console_write(const char *text)
{
    CHECK(!lock_held);
    size_t length = strlen(text);
    CHECK(length < sizeof(output) - used);
    memcpy(output + used, text, length + 1U);
    used += length;
}
static void console_printf(const char *format, ...)
{
    CHECK(!lock_held);
    char buffer[768];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    CHECK(length >= 0 && (size_t)length < sizeof(buffer));
    if ((size_t)length > max_format) max_format = (size_t)length;
    console_write(buffer);
}
#include "count_diagnostic_console.inc"

static wifi_packet_count_scan_t scan;
static wifi_packet_count_record_status_t record;
static wifi_packet_count_diagnostics_t read_snapshot(void)
{
    wifi_packet_count_diagnostics_t result;
    memset(&result, 0xa5, sizeof(result));
    wifi_packet_count_diagnostics_get(&result);
    return result;
}
static void reset(void)
{
    CHECK(!lock_held);
    memset(&published, 0, sizeof(published));
    memset(&scan, 0, sizeof(scan));
    memset(&record, 0, sizeof(record));
    used = 0;
    output[0] = 0;
}
static void command(const char *text)
{
    char line[80];
    CHECK(strlen(text) < sizeof(line));
    strcpy(line, text);
    used = 0;
    output[0] = 0;
    unsigned before = atomic_load(&enters);
    execute_command(line);
    if (strstr(text, "packet_count") && !strstr(text, " start"))
        CHECK(atomic_load(&enters) == before + 1U);
}
static void availability_and_unknowns(void)
{
    reset();
    wifi_packet_count_diagnostics_t zero = {0}, value = read_snapshot();
    CHECK(!memcmp(&zero, &value, sizeof(value)));
    unsigned before = atomic_load(&enters);
    wifi_packet_count_diagnostics_get(NULL);
    CHECK(atomic_load(&enters) == before);
    command("packet_count");
    CHECK(strstr(output, "packet_count_available=0\r\n"));
    CHECK(!strstr(output, "packet_count_c5_callbacks="));
    scan.sources[1].remote.flags = CERBERUS_PACKET_COUNT_RX_ACTIVE;
    scan.sources[1].remote.channel = 11;
    scan.sources[1].published.summary.callbacks = 987654;
    scan.sources[1].published.summary.valid_frames = 234561;
    scan.sources[1].published.summary.malformed_frames = 234562;
    scan.sources[1].published.summary.unsupported_frames = 234563;
    scan.sources[1].published.summary.outside_window_frames = 234564;
    scan.sources[1].published.summary.clock_dropped_frames = 234565;
    record.job = 999;
    record.ready = true;
    record.source[1].written = 234567;
    wifi_packet_count_diagnostics_publish(&scan, &record, 0, 7, true, false, ESP_OK);
    value = read_snapshot();
    CHECK(value.available && value.allocated && value.start_pending && value.time_valid);
    CHECK(!value.published_ms && value.publication_sequence == 1);
    CHECK(!value.record_valid && !value.record_ready && !value.source[1].written);
    CHECK(!value.source[1].control_valid && !value.source[1].flags && !value.source[1].channel);
    CHECK(!value.source[1].published_valid && !value.source[1].callbacks);
    CHECK(!value.source[1].valid_frames && !value.source[1].malformed_frames &&
        !value.source[1].unsupported_frames && !value.source[1].outside_window_frames &&
        !value.source[1].clock_dropped_frames);
    command("packet_count");
    CHECK(strstr(output, "packet_count_c5_control=unknown\r\n"));
    CHECK(strstr(output, "packet_count_c5_counters=unknown\r\n"));
    CHECK(strstr(output, "packet_count_record_state=unknown\r\n"));
    CHECK(!strstr(output, "987654") && !strstr(output, "234567"));
    CHECK(!strstr(output, "packet_count_c5_valid_frames=") &&
        !strstr(output, "packet_count_c5_malformed_frames=") &&
        !strstr(output, "packet_count_c5_unsupported_frames=") &&
        !strstr(output, "packet_count_c5_outside_window_frames=") &&
        !strstr(output, "packet_count_c5_clock_dropped_frames="));
    wifi_packet_count_diagnostics_clear(12);
    value = read_snapshot();
    CHECK(value.available && !value.allocated && value.publication_sequence == 2 && value.published_ms == 12);
    CHECK(!value.job && !value.start_pending && !value.source[1].requested_session);
    command("packet_count");
    CHECK(strstr(output, "packet_count_allocated=0\r\n") && !strstr(output, "packet_count_c5_phase="));
}
static void actual_intent_and_progress(void)
{
    reset();
    wifi_packet_count_scan_init(&scan);
    wifi_packet_count_selection_t target = {.target={.role=CERBERUS_WIFI_PACKET_COUNT_AP,
        .channel_hint=11, .mac={2,3,4,5,6,7}}, .label="PRIVATE_SELECTED_NAME"};
    CHECK(wifi_packet_count_scan_add(&scan, &target) == ESP_OK);
    uint64_t mask = wifi_packet_count_scan_plan(&scan, WIFI_PACKET_COUNT_SELECTED, 0);
    CHECK(wifi_packet_count_scan_begin(&scan, mask, 100, false) == ESP_OK);
    wifi_packet_count_diagnostics_publish(&scan, NULL, 100, 0, false, false, ESP_OK);
    wifi_packet_count_diagnostics_t value = read_snapshot();
    CHECK(value.requested && value.target_count == 1 && value.segments == 1);
    CHECK(!value.source[0].channel_mask && value.source[0].phase == WIFI_COUNT_IDLE);
    CHECK(value.source[1].channel_mask == mask && value.source[1].phase == WIFI_COUNT_INFO);
    wifi_packet_count_source_t *s = &scan.sources[1];
    s->phase = WIFI_COUNT_TARGET; s->attempted = s->accepted = s->have_control = true;
    s->request.session_id = s->session = UINT64_C(0x123456789abcdef1);
    s->remote = (cerberus_packet_count_control_t){.session_id=s->session,
        .state=CERBERUS_PACKET_COUNT_STAGING,.flags=CERBERUS_PACKET_COUNT_BOUND|CERBERUS_PACKET_COUNT_HELD,
        .configured=0,.count=1};
    s->last_status_ms = 100; s->next_action_ms = 800;
    scan.last_tick_ms = 125;
    wifi_packet_count_diagnostics_publish(&scan, NULL, 125, 0, false, false, ESP_OK);
    command(" PaCkEt_CoUnT \t");
    CHECK(strstr(output, "packet_count_c5_phase=3:targets\r\n"));
    CHECK(strstr(output, "packet_count_c5_configured=0000\r\n"));
    CHECK(strstr(output, "packet_count_c5_requested_session=123456789abcdef1\r\n"));
    CHECK(!strstr(output, "PRIVATE_SELECTED_NAME") && !strstr(output, "02:03:04"));
    s->phase = WIFI_COUNT_READ; s->start_attempted = true;
    s->remote.state = CERBERUS_PACKET_COUNT_RUNNING;
    s->remote.flags |= CERBERUS_PACKET_COUNT_RX_ACTIVE;
    s->remote.configured = 1; s->remote.channel = 11;
    s->pin = UINT32_C(0x80000001); s->part = 7; s->kind = CERBERUS_PACKET_COUNT_READ_SUMMARY;
    wifi_packet_count_diagnostics_publish(&scan, NULL, 200, 0, false, false, ESP_OK);
    command("packet_count");
    CHECK(strstr(output, "packet_count_c5_rx_active=1\r\n"));
    CHECK(strstr(output, "packet_count_c5_part=7\r\n"));
    CHECK(strstr(output, "packet_count_c5_counters=unknown\r\n"));
    s->published.valid = true; s->published.session = s->session;
    wifi_packet_count_diagnostics_publish(&scan, NULL, 300, 0, false, false, ESP_OK);
    command("packet_count");
    CHECK(strstr(output, "packet_count_c5_callbacks=0\r\n"));
    CHECK(strstr(output, "packet_count_c5_valid_frames=0\r\n") &&
        strstr(output, "packet_count_c5_malformed_frames=0\r\n") &&
        strstr(output, "packet_count_c5_unsupported_frames=0\r\n") &&
        strstr(output, "packet_count_c5_outside_window_frames=0\r\n") &&
        strstr(output, "packet_count_c5_clock_dropped_frames=0\r\n"));
    CHECK(!strstr(output, "packet_count_c5_counters=unknown\r\n"));
    s->published.summary.valid_frames = 101;
    s->published.summary.malformed_frames = 102;
    s->published.summary.unsupported_frames = 103;
    s->published.summary.outside_window_frames = 104;
    s->published.summary.clock_dropped_frames = 105;
    wifi_packet_count_diagnostics_publish(&scan, NULL, 350, 0, false, false, ESP_OK);
    value = read_snapshot();
    CHECK(value.source[1].valid_frames == 101 && value.source[1].malformed_frames == 102 &&
        value.source[1].unsupported_frames == 103 && value.source[1].outside_window_frames == 104 &&
        value.source[1].clock_dropped_frames == 105);
    command("packet_count");
    CHECK(strstr(output, "packet_count_c5_valid_frames=101\r\n") &&
        strstr(output, "packet_count_c5_malformed_frames=102\r\n") &&
        strstr(output, "packet_count_c5_unsupported_frames=103\r\n") &&
        strstr(output, "packet_count_c5_outside_window_frames=104\r\n") &&
        strstr(output, "packet_count_c5_clock_dropped_frames=105\r\n"));
    wifi_packet_count_scan_stop(&scan);
    wifi_packet_count_diagnostics_publish(&scan, NULL, 400, 0, false, true, ESP_OK);
    value = read_snapshot();
    CHECK(value.closing && !value.requested && value.source[1].stop);
    CHECK(value.source[1].requested_session == s->session && value.source[1].control_valid);
    CHECK(!value.source[1].next_action_ms);
}
static void limits_record_copy_and_clock(void)
{
    reset();
    scan.count=16;scan.requested_channels=UINT64_MAX;scan.last_tick_ms=UINT64_MAX;
    scan.segments=scan.interruptions=UINT32_MAX;scan.dwell_ms=UINT16_MAX;
    scan.error=scan.s3_admission_error=INT32_MIN;
    scan.requested=scan.retuning=scan.frozen=scan.fallback=scan.recording=true;
    record.job=UINT64_MAX;record.error=INT32_MIN;record.busy=record.ready=record.finishing=true;
    for (unsigned i=0;i<2;++i) {
        wifi_packet_count_source_t *s=&scan.sources[i];
        s->request.session_id=s->session=s->request.channel_mask=s->last_status_ms=s->next_action_ms=UINT64_MAX;
        s->request.target_count=16;s->upload_index=15;s->part=UINT16_MAX;s->phase=UINT8_MAX;s->kind=s->index=UINT8_MAX;
        s->pin=s->revision_floor=UINT32_MAX;s->error=INT32_MIN;
        s->attempted=s->accepted=s->start_attempted=s->have_control=s->stop=s->close=s->faulted=s->clock_invalid=s->record_bound=true;
        s->remote.state=s->remote.flags=s->remote.channel=UINT8_MAX;
        s->remote.configured=UINT16_MAX;s->remote.error_code=INT32_MIN;
        s->published.valid=s->published.frozen=true;s->published.session=UINT64_MAX;
        s->published.summary.revision=s->published.summary.callbacks=s->published.summary.matched_frames=
            s->published.summary.invalid_metadata=UINT32_MAX;
        s->published.summary.valid_frames=s->published.summary.malformed_frames=
            s->published.summary.unsupported_frames=s->published.summary.outside_window_frames=
            s->published.summary.clock_dropped_frames=UINT32_MAX;
        s->published.recording.dropped_full=s->published.recording.dropped_other=UINT32_MAX;
        s->published.source_error=INT32_MIN;
        wifi_packet_count_record_source_t *r=&record.source[i];
        r->session=UINT64_MAX;r->written=r->discarded=r->transport_errors=UINT32_MAX;r->error=INT32_MIN;
        r->bound=r->draining=r->drained=r->loss_unknown=true;
        strcpy(r->file_name,"PRIVATE_FILE_NAME");
    }
    wifi_packet_count_diagnostics_publish(&scan,&record,500,UINT64_MAX,true,true,INT32_MIN);
    wifi_packet_count_diagnostics_t copy=read_snapshot();
    CHECK(copy.record_valid && copy.source[1].raw_dropped_other==UINT32_MAX);
    memset(&scan,0,sizeof(scan));memset(&record,0,sizeof(record));
    wifi_packet_count_diagnostics_t again=read_snapshot();
    CHECK(!memcmp(&copy,&again,sizeof(copy)));
    command("packet_count");
    CHECK(strstr(output,"packet_count_c5_phase=255:unknown\r\n"));
    CHECK(strstr(output,"packet_count_c5_written=4294967295\r\n"));
    CHECK(strstr(output,"packet_count_c5_record_loss_unknown=1\r\n"));
    CHECK(strstr(output,"packet_count_c5_valid_frames=4294967295\r\n") &&
        strstr(output,"packet_count_c5_malformed_frames=4294967295\r\n") &&
        strstr(output,"packet_count_c5_unsupported_frames=4294967295\r\n") &&
        strstr(output,"packet_count_c5_outside_window_frames=4294967295\r\n") &&
        strstr(output,"packet_count_c5_clock_dropped_frames=4294967295\r\n"));
    CHECK(!strstr(output,"PRIVATE_FILE_NAME"));
    wifi_packet_count_diagnostics_clear(499);copy=read_snapshot();
    CHECK(!copy.allocated && !copy.time_valid && copy.published_ms==500 && copy.publication_sequence==2);
    wifi_packet_count_diagnostics_publish(NULL,NULL,500,1,true,true,ESP_FAIL);
    copy=read_snapshot();CHECK(copy.time_valid&&!copy.allocated&&!copy.job&&!copy.ui_error);
    published.publication_sequence=UINT64_MAX-1;
    wifi_packet_count_diagnostics_clear(UINT64_MAX);copy=read_snapshot();
    CHECK(copy.publication_sequence==UINT64_MAX&&!copy.sequence_saturated&&copy.published_ms==UINT64_MAX);
    wifi_packet_count_diagnostics_clear(UINT64_MAX);copy=read_snapshot();
    CHECK(copy.publication_sequence==UINT64_MAX&&copy.sequence_saturated);
    command("packet_count start");CHECK(strstr(output,"ERR unknown command"));
    command("help");CHECK(strstr(output,"packet_count Read copied Count setup/progress without radio access"));
}
static void heartbeat_without_progress(void)
{
    reset();
    scan.sources[1].phase=WIFI_COUNT_TARGET;
    scan.sources[1].request.session_id=901;
    scan.last_tick_ms=100;
    wifi_packet_count_diagnostics_publish(&scan,NULL,100,0,false,false,ESP_OK);
    wifi_packet_count_diagnostics_t value=read_snapshot();
    CHECK(!value.tick_gap_valid&&value.source[1].progress_time_valid&&value.source[1].last_progress_ms==100);
    scan.last_tick_ms=1300;scan.sources[1].last_status_ms=1300;
    wifi_packet_count_diagnostics_publish(&scan,NULL,1300,0,false,false,ESP_OK);
    value=read_snapshot();
    CHECK(value.tick_gap_valid&&value.tick_gap_ms==1200&&value.max_tick_gap_ms==1200);
    CHECK(value.source[1].last_status_ms==1300&&value.source[1].last_progress_ms==100);
    scan.last_tick_ms=2500;scan.sources[1].last_status_ms=2500;
    wifi_packet_count_diagnostics_publish(&scan,NULL,2500,0,false,false,ESP_OK);
    value=read_snapshot();CHECK(value.source[1].last_progress_ms==100&&value.max_tick_gap_ms==1200);
    scan.last_tick_ms=2600;scan.sources[1].upload_index=1;
    wifi_packet_count_diagnostics_publish(&scan,NULL,2600,0,false,false,ESP_OK);
    value=read_snapshot();CHECK(value.tick_gap_ms==100&&value.max_tick_gap_ms==1200&&value.source[1].last_progress_ms==2600);
    scan.last_tick_ms=2599;scan.sources[1].phase=WIFI_COUNT_START;
    wifi_packet_count_diagnostics_publish(&scan,NULL,2599,0,false,false,ESP_OK);
    value=read_snapshot();CHECK(!value.time_valid&&!value.tick_gap_valid&&!value.source[1].progress_time_valid);
    scan.last_tick_ms=2700;
    wifi_packet_count_diagnostics_publish(&scan,NULL,2700,0,false,false,ESP_OK);
    value=read_snapshot();CHECK(value.time_valid&&!value.source[1].progress_time_valid);
    scan.sources[1].phase=WIFI_COUNT_READ;scan.sources[1].part=0;
    wifi_packet_count_diagnostics_publish(&scan,NULL,2800,0,false,false,ESP_OK);
    value=read_snapshot();CHECK(value.source[1].progress_time_valid&&value.source[1].last_progress_ms==2800);
    scan.sources[1].part=1;
    wifi_packet_count_diagnostics_publish(&scan,NULL,2900,0,false,false,ESP_OK);
    CHECK(read_snapshot().source[1].last_progress_ms==2900);
    wifi_packet_count_diagnostics_clear(3000);
    scan.last_tick_ms=3100;
    wifi_packet_count_diagnostics_publish(&scan,NULL,3100,0,false,false,ESP_OK);
    value=read_snapshot();CHECK(!value.tick_gap_valid&&!value.max_tick_gap_ms&&value.source[1].last_progress_ms==3100);
}
static atomic_bool finished;
#ifdef _WIN32
static DWORD WINAPI writer(LPVOID unused)
#else
static void *writer(void *unused)
#endif
{
    (void)unused;
    wifi_packet_count_scan_t input={0};
    wifi_packet_count_record_status_t rec={0};
    for(uint32_t i=1;i<=20000;++i) {
        input.count=(uint8_t)(i%17U);input.last_tick_ms=(uint64_t)i*3U;input.segments=i;
        rec.job=i;rec.source[0].written=i+10U;rec.source[1].written=i+20U;
        for(unsigned source=0;source<2;++source) {
            input.sources[source].request.session_id=(uint64_t)i*7U+source;
            input.sources[source].published.valid=true;
            input.sources[source].published.summary.callbacks=i+source;
            input.sources[source].published.summary.valid_frames=i+source+1U;
            input.sources[source].published.summary.malformed_frames=i+source+2U;
            input.sources[source].published.summary.unsupported_frames=i+source+3U;
            input.sources[source].published.summary.outside_window_frames=i+source+4U;
            input.sources[source].published.summary.clock_dropped_frames=i+source+5U;
        }
        wifi_packet_count_diagnostics_publish(&input,&rec,(uint64_t)i*1000U,i,false,false,-(int32_t)i);
    }
    atomic_store(&finished,true);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}
static void concurrent_readers(void)
{
    reset();atomic_store(&finished,false);
#ifdef _WIN32
    HANDLE thread=CreateThread(NULL,0,writer,NULL,0,NULL);CHECK(thread);
#else
    pthread_t thread;CHECK(pthread_create(&thread,NULL,writer,NULL)==0);
#endif
    uint64_t previous=0;
    do {
        wifi_packet_count_diagnostics_t value=read_snapshot();
        if(!value.available)continue;
        uint32_t i=value.segments;
        CHECK(value.allocated&&value.time_valid&&value.record_valid);
        CHECK(value.publication_sequence==i&&value.publication_sequence>=previous);
        CHECK(value.published_ms==(uint64_t)i*1000U&&value.last_tick_ms==(uint64_t)i*3U);
        CHECK(value.target_count==i%17U&&value.job==i&&value.ui_error==-(int32_t)i);
        CHECK(value.source[0].requested_session==(uint64_t)i*7U&&value.source[1].requested_session==(uint64_t)i*7U+1U);
        CHECK(value.source[0].callbacks==i&&value.source[1].callbacks==i+1U);
        for(unsigned source=0;source<2;++source) {
            CHECK(value.source[source].valid_frames==i+source+1U);
            CHECK(value.source[source].malformed_frames==i+source+2U);
            CHECK(value.source[source].unsupported_frames==i+source+3U);
            CHECK(value.source[source].outside_window_frames==i+source+4U);
            CHECK(value.source[source].clock_dropped_frames==i+source+5U);
        }
        CHECK(value.source[0].written==i+10U&&value.source[1].written==i+20U);
        previous=value.publication_sequence;
    } while(!atomic_load(&finished));
#ifdef _WIN32
    CHECK(WaitForSingleObject(thread,INFINITE)==WAIT_OBJECT_0);CloseHandle(thread);
#else
    CHECK(pthread_join(thread,NULL)==0);
#endif
    CHECK(read_snapshot().publication_sequence==20000);
}
int main(void)
{
    availability_and_unknowns();actual_intent_and_progress();limits_record_copy_and_clock();
    heartbeat_without_progress();concurrent_readers();
    CHECK(atomic_load(&enters)==atomic_load(&exits));
    printf("Packet Count diagnostics PASS: %u checks, snapshot %zu bytes, largest console format %zu bytes; actual controller intent, copied progress, exact recorder identity, unknown/zero, privacy, clock/saturation, dispatch and concurrent snapshots.\n",
        assertions,sizeof(wifi_packet_count_diagnostics_t),max_format);
    return 0;
}
