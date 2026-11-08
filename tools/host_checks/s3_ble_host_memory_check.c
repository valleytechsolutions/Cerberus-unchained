#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
typedef int esp_err_t;
enum { ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE, ESP_ERR_NO_MEM,
    ESP_ERR_TIMEOUT, ESP_ERR_NOT_SUPPORTED, ESP_FAIL };
#define CONFIG_BT_NIMBLE_ENABLED 1
/* Allocator placement is an SDK behavior; mock figures below test capture and
 * lifecycle only. They are not measured admission constants or predictions. */
enum { MALLOC_CAP_INTERNAL = 1, MALLOC_CAP_8BIT = 2, MALLOC_CAP_SPIRAM = 4 };
typedef unsigned portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0U
static unsigned lock_depth;
#define portENTER_CRITICAL(lock) do { (void)(lock); assert(!lock_depth); ++lock_depth; } while (0)
#define portEXIT_CRITICAL(lock) do { (void)(lock); assert(lock_depth == 1); --lock_depth; } while (0)
#define pdMS_TO_TICKS(value) (value)
#include "s3_wifi_owner.h"
static bool psram_ready, will_sync;
static uint32_t ext_total, ext_free, int_free;
static int64_t mock_clock;
static esp_err_t sdk_error;
static int address_error;
static unsigned sdk_calls, task_calls, delay_calls, wifi_reads, race_at_read;
static cerberus_s3_wifi_owner_t wifi_owner;
static void (*init_hook)(void), (*address_hook)(void), (*heap_hook)(void), (*delay_hook)(unsigned);
static void (*saved_host_task)(void *);
static bool task_exits_immediately;
static struct { void (*reset_cb)(int); void (*sync_cb)(void); } ble_hs_cfg;
static int ble_hs_util_ensure_addr(int ignored)
{ (void)ignored; assert(!lock_depth); if (address_hook) address_hook(); return address_error; }
#if CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL
static bool esp_psram_is_initialized(void) { assert(!lock_depth); return psram_ready; }
static size_t heap_caps_get_total_size(uint32_t caps)
{ assert(!lock_depth && caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)); return ext_total; }
#endif
static size_t heap_caps_get_free_size(uint32_t caps)
{
    assert(!lock_depth);
    if (heap_hook) heap_hook();
    return caps & MALLOC_CAP_SPIRAM ? ext_free : int_free;
}
static size_t heap_caps_get_minimum_free_size(uint32_t caps)
{ assert(!lock_depth); return (caps & MALLOC_CAP_SPIRAM ? ext_free : int_free) / 3U; }
static int64_t esp_timer_get_time(void) { assert(!lock_depth); return ++mock_clock; }
cerberus_s3_wifi_owner_t cerberus_s3_wifi_owner(void)
{
    assert(!lock_depth);
    if (++wifi_reads == race_at_read) wifi_owner = CERBERUS_S3_WIFI_SCAN;
    return wifi_owner;
}
static esp_err_t nimble_port_init(void)
{
    assert(!lock_depth); ++sdk_calls;
    if (init_hook) init_hook();
    if (sdk_error) return sdk_error;
#if CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL
    int_free -= 10000U; ext_free -= 20000U;
#else
    int_free -= 30000U;
#endif
    return ESP_OK;
}
static void nimble_port_freertos_init(void (*fn)(void *))
{
    assert(!lock_depth && fn); saved_host_task=fn; ++task_calls; int_free -= 4096U;
    if (will_sync) ble_hs_cfg.sync_cb();
    if (task_exits_immediately) fn(NULL);
}
static void nimble_port_run(void) { assert(!lock_depth); }
static void nimble_port_freertos_deinit(void) { assert(!lock_depth); }
static void vTaskDelay(unsigned ticks)
{ assert(!lock_depth && ticks == 10); ++delay_calls; if (delay_hook) delay_hook(delay_calls); }

#include "../../s3_main/main/s3_ble_host.c"

/* Older allocator/diagnostic cases retain their owner-oriented organization,
 * but all operations now use the exact lease returned by the real host. */
static cerberus_s3_ble_host_lease_t accepted[4];
static esp_err_t claim(cerberus_s3_ble_owner_t owner)
{
    cerberus_s3_ble_host_lease_t candidate;
    esp_err_t error=cerberus_s3_ble_host_claim(owner,&candidate);
    if(error==ESP_OK) accepted[owner]=candidate;
    else assert(candidate.token==0 && candidate.epoch==0 && candidate.owner==0);
    return error;
}
static esp_err_t release(cerberus_s3_ble_owner_t owner)
{
    if(owner==CERBERUS_S3_BLE_OWNER_NONE) return cerberus_s3_ble_host_release(NULL);
    cerberus_s3_ble_host_lease_t lease=accepted[owner];
    if(!lease.token) lease=(cerberus_s3_ble_host_lease_t){UINT64_MAX,host_epoch,owner};
    return cerberus_s3_ble_host_release(&lease);
}

static void reset_test(void)
{
    assert(!lock_depth);
    memset(&current_lease,0,sizeof(current_lease)); memset(accepted,0,sizeof(accepted));
    last_claim_token=pending_token=0; pending_owner=CERBERUS_S3_BLE_OWNER_NONE;
    initialized=synced=host_stopped=epoch_exhausted=false; host_epoch=0;
    memset(&memory_diagnostics, 0, sizeof(memory_diagnostics)); memory_sequence = 0;
    memset(&ble_hs_cfg, 0, sizeof(ble_hs_cfg));
    psram_ready = will_sync = true; ext_total = 2097152U; ext_free = 1300000U; int_free = 105000U;
    mock_clock = 0; sdk_error = ESP_OK;address_error=0;
    sdk_calls = task_calls = delay_calls = wifi_reads = race_at_read = 0;
    wifi_owner = CERBERUS_S3_WIFI_NONE;
    init_hook=address_hook=heap_hook=NULL; delay_hook=NULL; saved_host_task=NULL;
    task_exits_immediately=false;
}
static void ownership_tests(void)
{
    reset_test();
    assert(claim(CERBERUS_S3_BLE_OWNER_NONE) == ESP_ERR_INVALID_ARG);
    wifi_owner = CERBERUS_S3_WIFI_SCAN;
    assert(claim(CERBERUS_S3_BLE_OWNER_SCANNER) == ESP_ERR_INVALID_STATE);
    assert(!sdk_calls && !memory_sequence);
    /* A passive survey owns the S3 radio until exact cleanup, including the
     * wardrive claimant that may otherwise share its own Wi-Fi owner. */
    for (unsigned claimant=CERBERUS_S3_BLE_OWNER_SCANNER;
         claimant<=CERBERUS_S3_BLE_OWNER_WARDRIVE; ++claimant) {
        reset_test();
        wifi_owner=CERBERUS_S3_WIFI_SURVEY;
        assert(claim((cerberus_s3_ble_owner_t)claimant)==ESP_ERR_INVALID_STATE);
        assert(!sdk_calls && !task_calls && !memory_sequence);
        assert(cerberus_s3_ble_host_owner()==CERBERUS_S3_BLE_OWNER_NONE);
    }
    reset_test(); race_at_read = 2;
    assert(claim(CERBERUS_S3_BLE_OWNER_CENTRAL) == ESP_ERR_INVALID_STATE);
    assert(cerberus_s3_ble_host_owner() == CERBERUS_S3_BLE_OWNER_NONE && !sdk_calls);
    reset_test();
    assert(claim(CERBERUS_S3_BLE_OWNER_SCANNER) == ESP_OK);
    unsigned sequence = memory_sequence;
    psram_ready = false; /* A failed foreign claim may not release this owner. */
    assert(claim(CERBERUS_S3_BLE_OWNER_CENTRAL) == ESP_ERR_INVALID_STATE);
    assert(release(CERBERUS_S3_BLE_OWNER_CENTRAL) == ESP_ERR_INVALID_STATE);
    assert(cerberus_s3_ble_host_owner() == CERBERUS_S3_BLE_OWNER_SCANNER && memory_sequence == sequence);
    assert(release(CERBERUS_S3_BLE_OWNER_SCANNER) == ESP_OK);
    assert(release(CERBERUS_S3_BLE_OWNER_NONE) == ESP_ERR_INVALID_ARG);
}
static void missing_psram_tests(void)
{
    for (unsigned missing = 0; missing < 2; ++missing) {
        reset_test();
        if (!missing) psram_ready = false;
        else ext_total = 0;
        for (unsigned owner = CERBERUS_S3_BLE_OWNER_SCANNER; owner <= CERBERUS_S3_BLE_OWNER_CENTRAL; ++owner) {
#if CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL
            assert(claim((cerberus_s3_ble_owner_t)owner) == ESP_ERR_NO_MEM);
            assert(!sdk_calls && !task_calls && !cerberus_s3_ble_host_initialized());
            assert(cerberus_s3_ble_host_owner() == CERBERUS_S3_BLE_OWNER_NONE);
            assert(memory_diagnostics.samples[CERBERUS_S3_BLE_MEMORY_REFUSED].requested_owner == owner);
#else
            assert(claim((cerberus_s3_ble_owner_t)owner) == ESP_OK);
            assert(release((cerberus_s3_ble_owner_t)owner) == ESP_OK);
#endif
        }
        psram_ready = true; ext_total = 2097152U;
        assert(claim(CERBERUS_S3_BLE_OWNER_SCANNER) == ESP_OK);
        assert(sdk_calls == 1 && cerberus_s3_ble_host_initialized());
        assert(release(CERBERUS_S3_BLE_OWNER_SCANNER) == ESP_OK);
    }
}
static void sdk_failure_and_warm_tests(void)
{
    reset_test(); sdk_error = ESP_ERR_NO_MEM; ext_free = 1U;
    assert(claim(CERBERUS_S3_BLE_OWNER_SCANNER) == ESP_ERR_NO_MEM);
    assert(sdk_calls == 1 && !task_calls && !cerberus_s3_ble_host_initialized());
    assert(cerberus_s3_ble_host_owner() == CERBERUS_S3_BLE_OWNER_NONE);
    assert(memory_diagnostics.samples[CERBERUS_S3_BLE_MEMORY_AFTER_INIT].error == ESP_ERR_NO_MEM);
    /* No invented headroom threshold: the IDF error is preserved. */
    sdk_error = ESP_OK; ext_free = 1300000U;
    assert(claim(CERBERUS_S3_BLE_OWNER_SCANNER) == ESP_OK);
    assert(release(CERBERUS_S3_BLE_OWNER_SCANNER) == ESP_OK);
    uint32_t retained_internal = int_free, retained_external = ext_free;
    uint32_t init_sequence = memory_diagnostics.samples[CERBERUS_S3_BLE_MEMORY_AFTER_INIT].sequence;
    for (unsigned i = 0; i < 12; ++i) {
        assert(claim(CERBERUS_S3_BLE_OWNER_CENTRAL) == ESP_OK);
        assert(release(CERBERUS_S3_BLE_OWNER_CENTRAL) == ESP_OK);
    }
    assert(sdk_calls == 2 && task_calls == 1 && int_free == retained_internal && ext_free == retained_external);
    assert(memory_diagnostics.samples[CERBERUS_S3_BLE_MEMORY_AFTER_INIT].sequence == init_sequence);
    psram_ready = false;
#if CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL
    assert(claim(CERBERUS_S3_BLE_OWNER_SCANNER) == ESP_ERR_NO_MEM);
    assert(cerberus_s3_ble_host_owner() == CERBERUS_S3_BLE_OWNER_NONE);
    assert(cerberus_s3_ble_host_initialized() && cerberus_s3_ble_host_synced() && sdk_calls == 2);
#else
    assert(claim(CERBERUS_S3_BLE_OWNER_SCANNER) == ESP_OK);
    assert(release(CERBERUS_S3_BLE_OWNER_SCANNER) == ESP_OK);
#endif
    psram_ready = true;
    cerberus_s3_ble_memory_diagnostics_t copied;
    memset(&copied, 0xa5, sizeof(copied));
    cerberus_s3_ble_host_memory_diagnostics(&copied);
    assert(!memcmp(&copied, &memory_diagnostics, sizeof(copied)));
    assert(copied.samples[CERBERUS_S3_BLE_MEMORY_RELEASED].internal_free == retained_internal);
    assert(copied.samples[CERBERUS_S3_BLE_MEMORY_RELEASED].psram_free == retained_external);
    assert(copied.samples[CERBERUS_S3_BLE_MEMORY_RELEASED].valid &&
        copied.samples[CERBERUS_S3_BLE_MEMORY_RELEASED].initialized);
    cerberus_s3_ble_host_memory_diagnostics(NULL);
    memory_sequence = UINT32_MAX;
    assert(claim(CERBERUS_S3_BLE_OWNER_ADVERTISER) == ESP_OK);
    assert(memory_sequence == UINT32_MAX);
    assert(release(CERBERUS_S3_BLE_OWNER_ADVERTISER) == ESP_OK);
    assert(memory_sequence == UINT32_MAX);
}
static void sync_timeout_and_reset_tests(void)
{
    reset_test(); will_sync = false;
    assert(claim(CERBERUS_S3_BLE_OWNER_SCANNER) == ESP_ERR_TIMEOUT);
    assert(sdk_calls == 1 && task_calls == 1 && delay_calls == 200);
    assert(cerberus_s3_ble_host_initialized() && !cerberus_s3_ble_host_synced());
    assert(cerberus_s3_ble_host_owner() == CERBERUS_S3_BLE_OWNER_NONE);
    assert(memory_diagnostics.samples[CERBERUS_S3_BLE_MEMORY_AFTER_SYNC].error == ESP_ERR_TIMEOUT);
    ble_hs_cfg.sync_cb();
    assert(claim(CERBERUS_S3_BLE_OWNER_CENTRAL) == ESP_OK && sdk_calls == 1);
    ble_hs_cfg.reset_cb(123);
    assert(cerberus_s3_ble_host_epoch() == 1 && !cerberus_s3_ble_host_synced());
    assert(cerberus_s3_ble_host_owner() == CERBERUS_S3_BLE_OWNER_NONE);
}

static void assert_empty_lease(const cerberus_s3_ble_host_lease_t *lease)
{
    const cerberus_s3_ble_host_lease_t zero={0};
    assert(memcmp(lease,&zero,sizeof(zero))==0);
}

static void lease_identity_tests(void)
{
    for(unsigned owner=1;owner<=3;++owner){
        reset_test();
        cerberus_s3_ble_host_lease_t a,b,bad;
        assert(cerberus_s3_ble_host_claim((cerberus_s3_ble_owner_t)owner,&a)==ESP_OK);
        assert(a.token && cerberus_s3_ble_host_lease_valid(&a));
        /* Readiness loss alone does not authorize abandonment of ownership. */
        address_error=1;host_sync();
        assert(!cerberus_s3_ble_host_synced() && cerberus_s3_ble_host_lease_valid(&a));
        assert(cerberus_s3_ble_host_release(&a)==ESP_OK);
        address_error=0;host_sync();
        assert(!cerberus_s3_ble_host_lease_valid(&a));
        assert(cerberus_s3_ble_host_claim((cerberus_s3_ble_owner_t)owner,&b)==ESP_OK);
        assert(b.token>a.token && b.epoch==a.epoch && b.owner==a.owner);
        assert(cerberus_s3_ble_host_release(&a)==ESP_ERR_INVALID_STATE);
        for(unsigned field=0;field<3;++field){
            bad=b;
            if(!field) --bad.token;
            else if(field==1) ++bad.epoch;
            else bad.owner=(cerberus_s3_ble_owner_t)(owner==3?1:owner+1);
            assert(!cerberus_s3_ble_host_lease_valid(&bad));
            assert(cerberus_s3_ble_host_release(&bad)==ESP_ERR_INVALID_STATE);
            assert(cerberus_s3_ble_host_lease_valid(&b));
        }
        host_reset(1);
        assert(!cerberus_s3_ble_host_lease_valid(&b));
        host_sync();
        assert(cerberus_s3_ble_host_claim((cerberus_s3_ble_owner_t)owner,&a)==ESP_OK);
        assert(a.token>b.token && a.epoch!=b.epoch);
        assert(cerberus_s3_ble_host_release(&b)==ESP_ERR_INVALID_STATE);
        assert(cerberus_s3_ble_host_lease_valid(&a));
        assert(cerberus_s3_ble_host_release(&a)==ESP_OK);
    }
    reset_test();
    cerberus_s3_ble_host_lease_t zero={0},out;
    assert(!cerberus_s3_ble_host_lease_valid(NULL) && !cerberus_s3_ble_host_lease_valid(&zero));
    assert(cerberus_s3_ble_host_release(&zero)==ESP_ERR_INVALID_ARG);
    assert(cerberus_s3_ble_host_claim(CERBERUS_S3_BLE_OWNER_SCANNER,NULL)==ESP_ERR_INVALID_ARG);
    memset(&out,0xa5,sizeof(out));
    assert(cerberus_s3_ble_host_claim((cerberus_s3_ble_owner_t)99,&out)==ESP_ERR_INVALID_ARG);
    assert_empty_lease(&out);
}

static unsigned nested_attempts;
static cerberus_s3_ble_host_lease_t retired_during_init;
static void nested_pending_checks(void)
{
    assert(pending_token && cerberus_s3_ble_host_owner()==CERBERUS_S3_BLE_OWNER_SCANNER);
    const unsigned before_sdk=sdk_calls;
    for(unsigned owner=1;owner<=3;++owner){
        cerberus_s3_ble_host_lease_t other;
        memset(&other,0xa5,sizeof(other));
        assert(cerberus_s3_ble_host_claim((cerberus_s3_ble_owner_t)owner,&other)==ESP_ERR_INVALID_STATE);
        assert_empty_lease(&other);++nested_attempts;
    }
    assert(sdk_calls==before_sdk);
    /* Wi-Fi's existing admission check must see the pending reservation even
     * when reset has retired its exact BLE lease. No RF work is modeled here. */
    wifi_owner=CERBERUS_S3_WIFI_SCAN;
    bool wifi_can_start=cerberus_s3_ble_host_owner()==CERBERUS_S3_BLE_OWNER_NONE;
    assert(!wifi_can_start);wifi_owner=CERBERUS_S3_WIFI_NONE;
}
static void init_reset_hook(void)
{
    retired_during_init=current_lease;
    assert(!cerberus_s3_ble_host_lease_valid(&retired_during_init));
    nested_pending_checks();
    assert(cerberus_s3_ble_host_release(&retired_during_init)==ESP_ERR_INVALID_STATE);
    host_reset(42);
    assert(!current_lease.token && pending_token==retired_during_init.token);
    assert(!cerberus_s3_ble_host_lease_valid(&retired_during_init));
    nested_pending_checks();
}
static void wait_reset_hook(unsigned tick)
{
    assert(tick==1);
    init_reset_hook();
    host_sync(); /* Resync does not resurrect the old claim. */
}
static void wait_pending_hook(unsigned tick)
{
    if(tick==1 || tick==200) nested_pending_checks();
}
static void reset_in_address_hook(void)
{ address_hook=NULL;host_reset(44);nested_pending_checks(); }

static void pending_reset_tests(void)
{
    for(unsigned fail=0;fail<2;++fail){
        reset_test();nested_attempts=0;init_hook=init_reset_hook;
        sdk_error=fail?ESP_ERR_NO_MEM:ESP_OK;
        cerberus_s3_ble_host_lease_t out;
        memset(&out,0xa5,sizeof(out));
        assert(cerberus_s3_ble_host_claim(CERBERUS_S3_BLE_OWNER_SCANNER,&out)==
            (fail?ESP_ERR_NO_MEM:ESP_ERR_INVALID_STATE));
        assert_empty_lease(&out);
        assert(nested_attempts==6 && !pending_token && !current_lease.token);
        assert(cerberus_s3_ble_host_owner()==CERBERUS_S3_BLE_OWNER_NONE);
        init_hook=NULL;sdk_error=ESP_OK;
        assert(cerberus_s3_ble_host_claim(CERBERUS_S3_BLE_OWNER_SCANNER,&out)==ESP_OK);
        assert(out.token>retired_during_init.token && out.epoch==1);
        assert(sdk_calls==(fail?2U:1U) && task_calls==1);
        assert(cerberus_s3_ble_host_release(&retired_during_init)==ESP_ERR_INVALID_STATE);
        assert(cerberus_s3_ble_host_lease_valid(&out));
        assert(cerberus_s3_ble_host_release(&out)==ESP_OK);
    }
    reset_test();will_sync=false;delay_hook=wait_reset_hook;nested_attempts=0;
    cerberus_s3_ble_host_lease_t out;
    assert(cerberus_s3_ble_host_claim(CERBERUS_S3_BLE_OWNER_SCANNER,&out)==ESP_ERR_INVALID_STATE);
    assert_empty_lease(&out);assert(delay_calls==1 && nested_attempts==6 && !pending_token);
    delay_hook=NULL;
    assert(cerberus_s3_ble_host_claim(CERBERUS_S3_BLE_OWNER_SCANNER,&out)==ESP_OK);
    assert(sdk_calls==1 && cerberus_s3_ble_host_lease_valid(&out));
    assert(cerberus_s3_ble_host_release(&out)==ESP_OK);

    reset_test();will_sync=false;delay_hook=wait_pending_hook;nested_attempts=0;
    assert(cerberus_s3_ble_host_claim(CERBERUS_S3_BLE_OWNER_SCANNER,&out)==ESP_ERR_TIMEOUT);
    assert_empty_lease(&out);assert(delay_calls==200 && nested_attempts==6);
    assert(!pending_token && !current_lease.token);
    delay_hook=NULL;host_sync();
    assert(cerberus_s3_ble_host_claim(CERBERUS_S3_BLE_OWNER_SCANNER,&out)==ESP_OK);
    assert(sdk_calls==1);assert(cerberus_s3_ble_host_release(&out)==ESP_OK);

    reset_test();address_hook=reset_in_address_hook;nested_attempts=0;
    assert(cerberus_s3_ble_host_claim(CERBERUS_S3_BLE_OWNER_SCANNER,&out)==ESP_ERR_INVALID_STATE);
    assert_empty_lease(&out);assert(!synced && !pending_token && nested_attempts==3);
    host_sync();assert(cerberus_s3_ble_host_claim(CERBERUS_S3_BLE_OWNER_SCANNER,&out)==ESP_OK);
    assert(cerberus_s3_ble_host_release(&out)==ESP_OK);
}

static void exhaustion_exit_and_late_wifi_tests(void)
{
    reset_test();last_claim_token=UINT64_MAX-1U;
    cerberus_s3_ble_host_lease_t a,b;
    assert(cerberus_s3_ble_host_claim(CERBERUS_S3_BLE_OWNER_SCANNER,&a)==ESP_OK);
    assert(a.token==UINT64_MAX && cerberus_s3_ble_host_lease_valid(&a));
    assert(cerberus_s3_ble_host_release(&a)==ESP_OK);
    assert(cerberus_s3_ble_host_claim(CERBERUS_S3_BLE_OWNER_SCANNER,&b)==ESP_ERR_INVALID_STATE);
    assert_empty_lease(&b);host_reset(1);host_sync();
    assert(cerberus_s3_ble_host_claim(CERBERUS_S3_BLE_OWNER_CENTRAL,&b)==ESP_ERR_INVALID_STATE);
    assert(sdk_calls==1 && last_claim_token==UINT64_MAX);

    reset_test();host_epoch=UINT_MAX-1U;
    assert(cerberus_s3_ble_host_claim(CERBERUS_S3_BLE_OWNER_SCANNER,&a)==ESP_OK);
    host_reset(1);host_sync();
    assert(cerberus_s3_ble_host_claim(CERBERUS_S3_BLE_OWNER_SCANNER,&b)==ESP_OK && b.epoch==UINT_MAX);
    assert(cerberus_s3_ble_host_release(&a)==ESP_ERR_INVALID_STATE);
    host_reset(2);host_sync();
    assert(epoch_exhausted && host_epoch==UINT_MAX && !synced);
    assert(cerberus_s3_ble_host_release(&b)==ESP_ERR_INVALID_STATE);
    assert(cerberus_s3_ble_host_claim(CERBERUS_S3_BLE_OWNER_SCANNER,&a)==ESP_ERR_INVALID_STATE);
    assert_empty_lease(&a);

    reset_test();assert(cerberus_s3_ble_host_claim(CERBERUS_S3_BLE_OWNER_SCANNER,&a)==ESP_OK);
    assert(saved_host_task);saved_host_task(NULL);
    assert(host_stopped && !synced && !cerberus_s3_ble_host_lease_valid(&a));
    assert(cerberus_s3_ble_host_release(&a)==ESP_ERR_INVALID_STATE);
    host_sync();assert(!synced);
    assert(cerberus_s3_ble_host_claim(CERBERUS_S3_BLE_OWNER_SCANNER,&b)==ESP_ERR_INVALID_STATE);
    assert_empty_lease(&b);assert(sdk_calls==1 && task_calls==1);

    reset_test();race_at_read=3;
    assert(cerberus_s3_ble_host_claim(CERBERUS_S3_BLE_OWNER_SCANNER,&a)==ESP_ERR_INVALID_STATE);
    assert_empty_lease(&a);assert(!pending_token && !current_lease.token && initialized && synced);
    wifi_owner=CERBERUS_S3_WIFI_NONE;
    assert(cerberus_s3_ble_host_claim(CERBERUS_S3_BLE_OWNER_SCANNER,&a)==ESP_OK);
    assert(sdk_calls==1 && cerberus_s3_ble_host_lease_valid(&a));
    assert(cerberus_s3_ble_host_release(&a)==ESP_OK);

    reset_test();task_exits_immediately=true;
    assert(cerberus_s3_ble_host_claim(CERBERUS_S3_BLE_OWNER_SCANNER,&a)==ESP_ERR_INVALID_STATE);
    assert_empty_lease(&a);
    assert(host_stopped && initialized && !synced && !pending_token && !current_lease.token);
    assert(cerberus_s3_ble_host_claim(CERBERUS_S3_BLE_OWNER_SCANNER,&b)==ESP_ERR_INVALID_STATE);
    assert_empty_lease(&b);assert(sdk_calls==1 && task_calls==1);
}

static cerberus_s3_ble_host_lease_t diagnostic_replacement;
static unsigned diagnostic_replacements;
static void replace_after_retirement(void)
{
    if(current_lease.token || pending_token) return;
    heap_hook=NULL;
    host_sync();
    assert(cerberus_s3_ble_host_claim(CERBERUS_S3_BLE_OWNER_SCANNER,
        &diagnostic_replacement)==ESP_OK);
    ++diagnostic_replacements;
}
static void post_retirement_diagnostic_tests(void)
{
    reset_test();diagnostic_replacements=0;
    cerberus_s3_ble_host_lease_t a;
    assert(cerberus_s3_ble_host_claim(CERBERUS_S3_BLE_OWNER_SCANNER,&a)==ESP_OK);
    heap_hook=replace_after_retirement;
    assert(cerberus_s3_ble_host_release(&a)==ESP_OK);
    assert(diagnostic_replacements==1 && diagnostic_replacement.token>a.token);
    assert(cerberus_s3_ble_host_lease_valid(&diagnostic_replacement));
    assert(cerberus_s3_ble_host_release(&a)==ESP_ERR_INVALID_STATE);
    assert(cerberus_s3_ble_host_release(&diagnostic_replacement)==ESP_OK);

    reset_test();diagnostic_replacements=0;will_sync=false;
    heap_hook=replace_after_retirement;
    assert(cerberus_s3_ble_host_claim(CERBERUS_S3_BLE_OWNER_SCANNER,&a)==ESP_ERR_TIMEOUT);
    assert_empty_lease(&a);
    assert(diagnostic_replacements==1 && diagnostic_replacement.token==2);
    assert(cerberus_s3_ble_host_lease_valid(&diagnostic_replacement));
    assert(sdk_calls==1 && task_calls==1);
    assert(cerberus_s3_ble_host_release(&diagnostic_replacement)==ESP_OK);
}
int main(void)
{
    ownership_tests(); missing_psram_tests(); sdk_failure_and_warm_tests(); sync_timeout_and_reset_tests();
    lease_identity_tests();pending_reset_tests();exhaustion_exit_and_late_wifi_tests();
    post_retirement_diagnostic_tests();
    printf("S3 BLE host %s: exact leases/ABA, pending init/reset serialization, stale cleanup, token/epoch exhaustion, task exit, WiFi admission, PSRAM/SDK faults and fixed diagnostics passed; lease=%zu diagnostics=%zu bytes.\n",
        CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL ? "EXTERNAL" : "INTERNAL", sizeof(cerberus_s3_ble_host_lease_t),sizeof(memory_diagnostics));
    return 0;
}
