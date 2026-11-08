#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    CERBERUS_S3_BLE_OWNER_NONE = 0,
    CERBERUS_S3_BLE_OWNER_SCANNER,
    CERBERUS_S3_BLE_OWNER_ADVERTISER,
    CERBERUS_S3_BLE_OWNER_CENTRAL,
    CERBERUS_S3_BLE_OWNER_WARDRIVE,
} cerberus_s3_ble_owner_t;

typedef struct {
    uint64_t token;
    unsigned epoch;
    cerberus_s3_ble_owner_t owner;
} cerberus_s3_ble_host_lease_t;

/* One NimBLE host serves the scanner, advertiser and central. Successful
 * admission returns a nonreused token bound to owner and host epoch. Failure
 * clears the output and retains no caller-owned lease. Validate before GAP
 * work; release this exact lease only after the procedure is quiescent.
 * Reset/task exit atomically retire the lease. Stale release cannot affect a
 * replacement with the same owner label. No owner-only release API exists.
 * lease_valid proves retained ownership, not SDK readiness: a sync failure
 * without reset does not abandon its claim. Query host_synced separately. */
esp_err_t cerberus_s3_ble_host_claim(cerberus_s3_ble_owner_t owner,
    cerberus_s3_ble_host_lease_t *lease);
esp_err_t cerberus_s3_ble_host_release(const cerberus_s3_ble_host_lease_t *lease);
bool cerberus_s3_ble_host_lease_valid(const cerberus_s3_ble_host_lease_t *lease);
/* Admission/diagnostic label, NOT ownership proof. A pending initialization
 * or sync wait keeps its reservation visible even if reset retired its lease. */
cerberus_s3_ble_owner_t cerberus_s3_ble_host_owner(void);
bool cerberus_s3_ble_host_initialized(void);
bool cerberus_s3_ble_host_synced(void);
/* Changes on reset/task exit. Exhaustion fails closed until reboot. */
unsigned cerberus_s3_ble_host_epoch(void);

typedef enum {
    CERBERUS_S3_BLE_MEMORY_BEFORE_INIT,
    CERBERUS_S3_BLE_MEMORY_AFTER_INIT,
    CERBERUS_S3_BLE_MEMORY_AFTER_SYNC,
    CERBERUS_S3_BLE_MEMORY_RELEASED,
    CERBERUS_S3_BLE_MEMORY_REFUSED,
    CERBERUS_S3_BLE_MEMORY_STAGE_COUNT,
} cerberus_s3_ble_memory_stage_t;

/* Process-wide heap observations at the named lifecycle boundary, not BLE-only
 * allocation totals. receipt_us is local monotonic receipt time, not RF time.
 * requested_owner identifies the caller, not an assertion of current ownership.
 * error is the admission/init/sync result at that phase; valid=false means no
 * sample. One fixed slot per phase; no packet data, heap allocation or SPI.
 * Largest-block walks remain in the explicit memory console command. */
typedef struct {
    int64_t receipt_us;
    uint32_t sequence;
    uint32_t internal_free, internal_minimum;
    uint32_t psram_free, psram_minimum;
    esp_err_t error;
    uint8_t requested_owner;
    bool initialized, synced, valid;
} cerberus_s3_ble_memory_sample_t;

typedef struct {
    cerberus_s3_ble_memory_sample_t samples[CERBERUS_S3_BLE_MEMORY_STAGE_COUNT];
} cerberus_s3_ble_memory_diagnostics_t;

void cerberus_s3_ble_host_memory_diagnostics(cerberus_s3_ble_memory_diagnostics_t *out);
