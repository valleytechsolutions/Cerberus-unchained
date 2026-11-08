#include "s3_ble_host.h"
#include "s3_wifi_owner.h"
#include "sdkconfig.h"
#include <string.h>

#ifdef CONFIG_BT_NIMBLE_ENABLED
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#if CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL
#include "esp_psram.h"
#endif
#include <limits.h>

static portMUX_TYPE host_lock = portMUX_INITIALIZER_UNLOCKED;
static cerberus_s3_ble_host_lease_t current_lease;
static uint64_t last_claim_token, pending_token;
static cerberus_s3_ble_owner_t pending_owner;
static bool initialized, synced, host_stopped, epoch_exhausted;
static unsigned host_epoch;
static portMUX_TYPE memory_lock = portMUX_INITIALIZER_UNLOCKED;
static cerberus_s3_ble_memory_diagnostics_t memory_diagnostics;
static uint32_t memory_sequence;

_Static_assert(sizeof(cerberus_s3_ble_memory_diagnostics_t) <= 256U,
    "BLE memory diagnostics must remain a small fixed snapshot");

static void memory_capture(cerberus_s3_ble_memory_stage_t stage,
    cerberus_s3_ble_owner_t requested_owner, esp_err_t error)
{
    const uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t external = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    cerberus_s3_ble_memory_sample_t sample = {
        .receipt_us = esp_timer_get_time(),
        .internal_free = heap_caps_get_free_size(internal),
        .internal_minimum = heap_caps_get_minimum_free_size(internal),
        .psram_free = heap_caps_get_free_size(external),
        .psram_minimum = heap_caps_get_minimum_free_size(external),
        .error = error, .requested_owner = (uint8_t)requested_owner,
        .initialized = cerberus_s3_ble_host_initialized(),
        .synced = cerberus_s3_ble_host_synced(), .valid = true,
    };
    portENTER_CRITICAL(&memory_lock);
    if (memory_sequence != UINT32_MAX) ++memory_sequence;
    sample.sequence = memory_sequence; /* Saturating diagnostic ordinal, not a job identity. */
    memory_diagnostics.samples[stage] = sample;
    portEXIT_CRITICAL(&memory_lock);
}

void cerberus_s3_ble_host_memory_diagnostics(cerberus_s3_ble_memory_diagnostics_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&memory_lock);
    *out = memory_diagnostics;
    portEXIT_CRITICAL(&memory_lock);
}

static bool owner_valid(cerberus_s3_ble_owner_t owner)
{
    return owner == CERBERUS_S3_BLE_OWNER_SCANNER ||
        owner == CERBERUS_S3_BLE_OWNER_ADVERTISER ||
        owner == CERBERUS_S3_BLE_OWNER_CENTRAL || owner == CERBERUS_S3_BLE_OWNER_WARDRIVE;
}

static bool passive_wifi_owns_radio(cerberus_s3_ble_owner_t claimant)
{
    cerberus_s3_wifi_owner_t owner=cerberus_s3_wifi_owner();
    return owner==CERBERUS_S3_WIFI_UPLOAD||owner==CERBERUS_S3_WIFI_SCAN||owner==CERBERUS_S3_WIFI_COUNT||
        owner==CERBERUS_S3_WIFI_SURVEY||
        (owner==CERBERUS_S3_WIFI_WARDRIVE&&claimant!=CERBERUS_S3_BLE_OWNER_WARDRIVE);
}

static bool lease_matches_locked(const cerberus_s3_ble_host_lease_t *lease)
{
    return lease->token != 0U && lease->token == current_lease.token &&
        lease->owner == current_lease.owner && lease->epoch == current_lease.epoch &&
        lease->epoch == host_epoch;
}

static void retire_host_locked(void)
{
    synced = false;
    memset(&current_lease, 0, sizeof(current_lease));
    if (host_epoch != UINT_MAX) ++host_epoch;
    else epoch_exhausted = true;
    /* Never clear pending_token here: an initializer/waiter can still be in
     * SDK code outside this lock. Its reservation lasts until it unwinds. */
}

static void host_sync(void)
{
    portENTER_CRITICAL(&host_lock);
    const unsigned epoch = host_epoch;
    portEXIT_CRITICAL(&host_lock);
    const bool ready = ble_hs_util_ensure_addr(0) == 0;
    portENTER_CRITICAL(&host_lock);
    if (epoch == host_epoch && initialized && !host_stopped && !epoch_exhausted)
        synced = ready;
    portEXIT_CRITICAL(&host_lock);
}

static void host_reset(int reason)
{
    (void)reason;
    portENTER_CRITICAL(&host_lock);
    retire_host_locked();
    portEXIT_CRITICAL(&host_lock);
}

static void host_task(void *unused)
{
    (void)unused;
    nimble_port_run();
    portENTER_CRITICAL(&host_lock);
    host_stopped = true;
    retire_host_locked();
    portEXIT_CRITICAL(&host_lock);
    /* The host is intentionally kept for this firmware session. A stopped
     * host cannot be silently reused by an already initialized scanner. */
    nimble_port_freertos_deinit();
}

cerberus_s3_ble_owner_t cerberus_s3_ble_host_owner(void)
{
    portENTER_CRITICAL(&host_lock);
    const cerberus_s3_ble_owner_t owner = pending_token ? pending_owner : current_lease.owner;
    portEXIT_CRITICAL(&host_lock);
    return owner;
}

bool cerberus_s3_ble_host_initialized(void)
{
    portENTER_CRITICAL(&host_lock);
    const bool value = initialized;
    portEXIT_CRITICAL(&host_lock);
    return value;
}
bool cerberus_s3_ble_host_synced(void)
{
    portENTER_CRITICAL(&host_lock);
    const bool value = synced;
    portEXIT_CRITICAL(&host_lock);
    return value;
}
unsigned cerberus_s3_ble_host_epoch(void)
{
    portENTER_CRITICAL(&host_lock);
    const unsigned value = host_epoch;
    portEXIT_CRITICAL(&host_lock);
    return value;
}

bool cerberus_s3_ble_host_lease_valid(const cerberus_s3_ble_host_lease_t *lease)
{
    if (!lease || !owner_valid(lease->owner) || !lease->token) return false;
    portENTER_CRITICAL(&host_lock);
    const bool valid = !pending_token && lease_matches_locked(lease);
    portEXIT_CRITICAL(&host_lock);
    return valid;
}

/* Called by the one pending claimant. Reset may already have retired its
 * lease; only a matching token may clear current ownership. */
static esp_err_t finish_claim(const cerberus_s3_ble_host_lease_t *candidate,
    esp_err_t error, cerberus_s3_ble_host_lease_t *out)
{
    portENTER_CRITICAL(&host_lock);
    const bool ours = lease_matches_locked(candidate) && !host_stopped && !epoch_exhausted;
    if (error == ESP_OK && (!ours || !initialized || !synced))
        error = ours ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE;
    if (error == ESP_OK) *out = *candidate;
    else if (lease_matches_locked(candidate)) memset(&current_lease, 0, sizeof(current_lease));
    if (pending_token == candidate->token) {
        pending_token = 0U;
        pending_owner = CERBERUS_S3_BLE_OWNER_NONE;
    }
    portEXIT_CRITICAL(&host_lock);
    return error;
}

esp_err_t cerberus_s3_ble_host_claim(cerberus_s3_ble_owner_t owner,
    cerberus_s3_ble_host_lease_t *lease)
{
    if (!lease) return ESP_ERR_INVALID_ARG;
    memset(lease, 0, sizeof(*lease));
    if (!owner_valid(owner)) return ESP_ERR_INVALID_ARG;
    if (passive_wifi_owns_radio(owner)) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&host_lock);
    if (pending_token || current_lease.token || host_stopped || epoch_exhausted ||
        last_claim_token == UINT64_MAX) {
        portEXIT_CRITICAL(&host_lock);
        return ESP_ERR_INVALID_STATE;
    }
    const cerberus_s3_ble_host_lease_t candidate = {
        .token = ++last_claim_token, .epoch = host_epoch, .owner = owner,
    };
    current_lease = candidate;
    pending_token = candidate.token;
    pending_owner = owner;
    const bool needs_init = !initialized;
    portEXIT_CRITICAL(&host_lock);
    /* The Wi-Fi scanner claims first and then checks BLE ownership. This
     * second check closes a claim race before any controller/GAP work starts. */
    if (passive_wifi_owns_radio(owner)) {
        return finish_claim(&candidate, ESP_ERR_INVALID_STATE, lease);
    }
#if CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL
    /* EXTERNAL is external-only. Missing optional PSRAM must not enter IDF's
     * allocator from any common host owner. This checks presence, not future
     * allocation success; IDF's existing exhaustion behavior is unchanged. */
    if (!esp_psram_is_initialized() ||
        !heap_caps_get_total_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) {
        memory_capture(CERBERUS_S3_BLE_MEMORY_REFUSED, owner, ESP_ERR_NO_MEM);
        return finish_claim(&candidate, ESP_ERR_NO_MEM, lease);
    }
#endif
    if (needs_init) {
        memory_capture(CERBERUS_S3_BLE_MEMORY_BEFORE_INIT, owner, ESP_OK);
        esp_err_t err = nimble_port_init();
        memory_capture(CERBERUS_S3_BLE_MEMORY_AFTER_INIT, owner, err);
        if (err != ESP_OK) {
            return finish_claim(&candidate, err, lease);
        }
        ble_hs_cfg.reset_cb = host_reset;
        ble_hs_cfg.sync_cb = host_sync;
        portENTER_CRITICAL(&host_lock);
        initialized = true;
        portEXIT_CRITICAL(&host_lock);
        nimble_port_freertos_init(host_task);
    }
    esp_err_t error = ESP_ERR_TIMEOUT;
    for (unsigned i = 0U; i <= 200U; ++i) {
        portENTER_CRITICAL(&host_lock);
        const bool ours = lease_matches_locked(&candidate) && !host_stopped && !epoch_exhausted;
        const bool ready = initialized && synced;
        portEXIT_CRITICAL(&host_lock);
        if (!ours) { error = ESP_ERR_INVALID_STATE; break; }
        if (ready) { error = ESP_OK; break; }
        if (i < 200U) vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (error == ESP_OK && passive_wifi_owns_radio(owner))
        error = ESP_ERR_INVALID_STATE;
    error = finish_claim(&candidate, error, lease);
    memory_capture(CERBERUS_S3_BLE_MEMORY_AFTER_SYNC, owner, error);
    return error;
}

esp_err_t cerberus_s3_ble_host_release(const cerberus_s3_ble_host_lease_t *lease)
{
    if (!lease || !owner_valid(lease->owner) || !lease->token)
        return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&host_lock);
    const bool ours = !pending_token && lease_matches_locked(lease);
    if (ours) memset(&current_lease, 0, sizeof(current_lease));
    portEXIT_CRITICAL(&host_lock);
    if (!ours) return ESP_ERR_INVALID_STATE;
    memory_capture(CERBERUS_S3_BLE_MEMORY_RELEASED, lease->owner, ESP_OK);
    return ESP_OK;
}
#else
cerberus_s3_ble_owner_t cerberus_s3_ble_host_owner(void) { return CERBERUS_S3_BLE_OWNER_NONE; }
bool cerberus_s3_ble_host_initialized(void) { return false; }
bool cerberus_s3_ble_host_synced(void) { return false; }
unsigned cerberus_s3_ble_host_epoch(void) { return 0U; }
esp_err_t cerberus_s3_ble_host_claim(cerberus_s3_ble_owner_t owner,
    cerberus_s3_ble_host_lease_t *lease)
{
    (void)owner;
    if (!lease) return ESP_ERR_INVALID_ARG;
    memset(lease, 0, sizeof(*lease));
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t cerberus_s3_ble_host_release(const cerberus_s3_ble_host_lease_t *lease)
{ (void)lease; return ESP_ERR_NOT_SUPPORTED; }
bool cerberus_s3_ble_host_lease_valid(const cerberus_s3_ble_host_lease_t *lease)
{ (void)lease; return false; }
void cerberus_s3_ble_host_memory_diagnostics(cerberus_s3_ble_memory_diagnostics_t *out)
{ if (out) memset(out, 0, sizeof(*out)); }
#endif
