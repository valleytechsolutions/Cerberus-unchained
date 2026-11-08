#pragma once

#include "device_identity.h"
#include "c5_spi_link.h"
#include "battery.h"
#include "storage.h"
#include "system_health.h"

typedef struct {
    cerberus_device_identity_t s3, c5;
    cerberus_c5_status_t peer;
    cerberus_wifi_connection_status_payload_t connection;
    cerberus_system_health_t health;
    cerberus_battery_status_t battery;
    cerberus_storage_status_t storage;
    size_t psram_total, psram_free;
    uint64_t sampled_ms;
    esp_err_t s3_error, c5_error, storage_error, connection_error;
    bool cancelled;
} cerberus_device_info_t;

typedef struct cerberus_device_info_job cerberus_device_info_job_t;

/* UI owns one optional PSRAM job and its lifetime. The short-lived worker
 * reads SDK, SD and C5 metadata; it never initializes a radio, starts a scan,
 * connects, uploads, or writes a file. Back requests cancellation. Release
 * succeeds only after the worker's final publication, including cancellation.
 * Read/cancel/release are called by the same UI task. */
esp_err_t cerberus_device_info_start(cerberus_device_info_job_t **job);
bool cerberus_device_info_read(const cerberus_device_info_job_t *job, cerberus_device_info_t *out);
void cerberus_device_info_cancel(cerberus_device_info_job_t *job);
bool cerberus_device_info_release(cerberus_device_info_job_t **job);
