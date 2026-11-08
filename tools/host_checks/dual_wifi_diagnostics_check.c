#undef NDEBUG
#include "dual_wifi_diagnostics.h"
#include "dual_wifi_scan.h"
#include "s3_wifi_scanner.h"
#include "s3_wifi_owner.h"
#include "freertos/FreeRTOS.h"
#include <assert.h>
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
void dual_diag_test_enter(portMUX_TYPE *lock)
{
    assert(!lock_held);
    while (atomic_flag_test_and_set_explicit(&lock->flag, memory_order_acquire)) {}
    lock_held=true; atomic_fetch_add(&enters,1);
}
void dual_diag_test_exit(portMUX_TYPE *lock)
{
    assert(lock_held); atomic_fetch_add(&exits,1); lock_held=false;
    atomic_flag_clear_explicit(&lock->flag,memory_order_release);
}
/* Include the actual production module so exhaustion is injectable without production
 * reset controls. Unexpected I/O fails to compile/link in this minimal target. */
#include "dual_wifi_diagnostics.c"

static cerberus_dual_wifi_status_t current;
static cerberus_s3_wifi_scan_status_t local;
static struct { uint32_t s3_job; uint8_t phase; bool s3_owned,c5_possible; } backing, *store;
static int64_t clock_us;
static esp_err_t local_status_error;
static cerberus_s3_wifi_owner_t owner;
void cerberus_dual_wifi_scan_get_status(cerberus_dual_wifi_status_t *out) { assert(!lock_held);*out=current; }
esp_err_t cerberus_s3_wifi_scanner_get_status(cerberus_s3_wifi_scan_status_t *out)
{ assert(!lock_held);*out=local;return local_status_error; }
cerberus_s3_wifi_owner_t cerberus_s3_wifi_owner(void) { assert(!lock_held);return owner; }
static int64_t esp_timer_get_time(void) { assert(!lock_held);return clock_us; }
#include "coordinator_publish.c.inc"

static char output[4096];
static size_t used;
static void console_printf(const char *format,...)
{
    assert(!lock_held); va_list args; va_start(args,format);
    int n=vsnprintf(output+used,sizeof(output)-used,format,args); va_end(args);
    /* Match production's per-call 768-byte buffer, including terminal NUL. */
    assert(n>=0 && n<768 && (size_t)n<sizeof(output)-used);used+=(size_t)n;
}
#include "print_wifi_dual.c.inc"
static cerberus_dual_wifi_diagnostics_t read_snapshot(void)
{ cerberus_dual_wifi_diagnostics_t out;memset(&out,0xA5,sizeof(out));cerberus_dual_wifi_diagnostics_get(&out);return out; }
static void reset(void)
{ assert(!lock_held);memset(&published,0,sizeof(published));memset(&current,0,sizeof(current));memset(&local,0,sizeof(local));store=NULL;local_status_error=ESP_OK;owner=CERBERUS_S3_WIFI_NONE;clock_us=0;used=0;output[0]=0; }
static void basic_and_clock(void)
{
    reset();cerberus_dual_wifi_diagnostics_t out=read_snapshot(),zero={0};assert(!memcmp(&out,&zero,sizeof(out)));
    unsigned before=atomic_load(&enters);cerberus_dual_wifi_diagnostics_get(NULL);cerberus_dual_wifi_diagnostics_publish(NULL,1);assert(atomic_load(&enters)==before);
    cerberus_dual_wifi_diagnostic_values_t value={.generation=7,.count=64,.split=true,.elapsed_ms=3200,.s3_job=99};
    cerberus_dual_wifi_diagnostics_publish(&value,0);out=read_snapshot();assert(out.available&&out.time_valid&&out.publication_sequence==1&&!out.published_us&&out.value.count==64);
    value.count=1;assert(read_snapshot().value.count==64); /* Copied, never aliases UI input. */
    cerberus_dual_wifi_diagnostics_publish(&value,1000);assert(read_snapshot().published_us==1000);
    cerberus_dual_wifi_diagnostics_publish(&value,999);out=read_snapshot();assert(!out.time_valid&&out.published_us==1000&&out.publication_sequence==3);
    cerberus_dual_wifi_diagnostics_publish(&value,-1);out=read_snapshot();assert(!out.time_valid&&out.published_us==1000);
    cerberus_dual_wifi_diagnostics_publish(&value,1000);assert(read_snapshot().time_valid);
    published.publication_sequence=UINT64_MAX-1;
    cerberus_dual_wifi_diagnostics_publish(&value,INT64_MAX);out=read_snapshot();assert(out.publication_sequence==UINT64_MAX&&!out.sequence_saturated&&out.published_us==INT64_MAX);
    cerberus_dual_wifi_diagnostics_publish(&value,INT64_MAX);out=read_snapshot();assert(out.publication_sequence==UINT64_MAX&&out.sequence_saturated);
}
static void adapter_and_console(void)
{
    reset();current=(cerberus_dual_wifi_status_t){.state=CERBERUS_DUAL_WIFI_ERROR,.generation=17,.count=64,
        .elapsed_ms=4100,.error=-12,.s3_error=-13,.c5_error=-14,.split=true,.cleanup_pending=true,
        .s3_count=32,.c5_count=32,.s3_total=40,.c5_total=48,.rebind_cached=true,
        .rebind_source=CERBERUS_DUAL_WIFI_SOURCE_C5};
    backing.s3_job=35;backing.phase=12;backing.s3_owned=true;backing.c5_possible=true;store=&backing;
    local=(cerberus_s3_wifi_scan_status_t){.job_id=35,.state=CERBERUS_S3_WIFI_SCAN_ERROR,.radio_owned=true,.cleanup_error=-15};
    owner=CERBERUS_S3_WIFI_SCAN;clock_us=9000;cerberus_dual_wifi_scan_publish_diagnostics(-16);
    cerberus_dual_wifi_diagnostics_t out=read_snapshot();assert(out.value.scan_s3_owned&&out.value.s3_radio_owned&&out.value.s3_status_valid&&out.value.c5_may_own);
    assert(out.value.generation==17&&out.value.count==64&&out.value.phase==12&&out.value.s3_total==40&&out.value.c5_total==48);
    unsigned before=atomic_load(&enters);print_wifi_dual();assert(atomic_load(&enters)==before+1);
    assert(strstr(output,"wifi_dual_scope=ui_published_aggregates_no_spi\r\n"));
    assert(strstr(output,"wifi_dual_scan_s3_job=35\r\n")&&strstr(output,"wifi_dual_ui_error=-16\r\n"));
    assert(strstr(output,"wifi_dual_s3_cleanup_error=-15\r\n")&&strstr(output,"wifi_dual_retained=64\r\n"));
    assert(out.value.rebind_cached&&strstr(output,"wifi_dual_rebind_cached=1\r\n"));
    assert(out.value.rebind_source==CERBERUS_DUAL_WIFI_SOURCE_C5&&
        strstr(output,"wifi_dual_rebind_source=2\r\n"));
    assert(!strstr(output,"ssid")&&!strstr(output,"address")&&!strstr(output,"token"));
    ++local.job_id;cerberus_dual_wifi_scan_publish_diagnostics(0);out=read_snapshot();assert(!out.value.scan_s3_owned&&out.value.s3_radio_owned&&out.value.scan_s3_job==35&&out.value.s3_job==36);
    local_status_error=ESP_FAIL;cerberus_dual_wifi_scan_publish_diagnostics(0);out=read_snapshot();assert(!out.value.s3_status_valid&&!out.value.scan_s3_owned&&!out.value.s3_job);
}
static atomic_bool finished;
#ifdef _WIN32
static DWORD WINAPI writer(LPVOID unused)
#else
static void *writer(void *unused)
#endif
{
    (void)unused;
    for(uint32_t i=1;i<=40000;++i) {
        cerberus_dual_wifi_diagnostic_values_t value={.generation=i,.elapsed_ms=i*3U,.s3_job=i+11U,
            .scan_s3_job=i+11U,.count=(uint8_t)(i%65U),.error=-(int32_t)i,.split=(i%2U)!=0};
        cerberus_dual_wifi_diagnostics_publish(&value,(int64_t)i*1000);
    }
    atomic_store(&finished,true);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}
static void coherent_readers(void)
{
    reset();atomic_store(&finished,false);
#ifdef _WIN32
    HANDLE worker=CreateThread(NULL,0,writer,NULL,0,NULL);assert(worker);
#else
    pthread_t worker;assert(pthread_create(&worker,NULL,writer,NULL)==0);
#endif
    uint64_t previous=0;
    do {
        cerberus_dual_wifi_diagnostics_t out=read_snapshot();if(!out.available)continue;
        uint32_t i=out.value.generation;
        assert(out.publication_sequence==i&&out.publication_sequence>=previous&&out.published_us==(uint64_t)i*1000);
        assert(out.time_valid&&out.value.elapsed_ms==i*3U&&out.value.s3_job==i+11U&&out.value.scan_s3_job==i+11U);
        assert(out.value.count==i%65U&&out.value.error==-(int32_t)i&&out.value.split==((i%2U)!=0));previous=out.publication_sequence;
    } while(!atomic_load(&finished));
#ifdef _WIN32
    assert(WaitForSingleObject(worker,INFINITE)==WAIT_OBJECT_0);CloseHandle(worker);
#else
    assert(pthread_join(worker,NULL)==0);
#endif
    assert(read_snapshot().publication_sequence==40000);
}
int main(void)
{
    basic_and_clock();adapter_and_console();coherent_readers();assert(atomic_load(&enters)==atomic_load(&exits));
    printf("Production dual Wi-Fi diagnostics: %zu-byte snapshot; monotonic clocks, saturation, coherent concurrent readers, local adapter and bounded console output passed.\n",sizeof(cerberus_dual_wifi_diagnostics_t));
    return 0;
}
