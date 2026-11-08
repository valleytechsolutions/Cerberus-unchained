#pragma once

#include <stdbool.h>
#include <stdint.h>

/* A UI-produced value copy. No AP identity, row pointer, C5 lease token or
 * radio control is exposed. Counts describe retained scan rows, not unique
 * physical devices. Generation/count can remain from a previous successful
 * bank while the current attempt is running, cancelled or failed.
 *
 * elapsed_ms is coordinator elapsed time (including collection/cleanup), not
 * RF airtime, physical touch latency or the legacy AP-readout measurement.
 * s3_job is the local scanner's current job; scan_s3_job is the job remembered
 * by this coordinator. scan_s3_owned is true only if that job still matches.
 * s3_radio_owned describes the local scanner; s3_wifi_owner names the actual
 * shared Wi-Fi owner, including other services when fallback is used. */
typedef struct {
    uint32_t generation, elapsed_ms, s3_job, scan_s3_job;
    int32_t error, ui_error, s3_error, c5_error, s3_cleanup_error;
    uint16_t s3_total, c5_total;
    uint8_t state, phase, count, s3_count, c5_count, s3_state, s3_wifi_owner;
    uint8_t rebind_source; /* 0=ordinary scan, 1=S3 row, 2=C5 row. No AP identity. */
    bool split, fallback, cleanup_pending, rebinding, rebind_ready, rebind_cached;
    bool s3_status_valid, s3_radio_owned, scan_s3_owned, c5_may_own;
} cerberus_dual_wifi_diagnostic_values_t;

typedef struct {
    cerberus_dual_wifi_diagnostic_values_t value;
    uint64_t publication_sequence, published_us;
    bool available, time_valid, sequence_saturated;
} cerberus_dual_wifi_diagnostics_t;

/* Exactly one UI task publishes. The supplied monotonic S3 time timestamps
 * this diagnostic publication, never RF reception or result freshness.
 * Negative/backward clock samples retain the prior timestamp and set
 * time_valid=false; the stale time must not be treated as a current sample.
 * A later nonnegative time at/above the high-water mark restores validity.
 * Sequence saturates instead of wrapping and reports lost increments.
 * Both functions allocate nothing and perform no clock, SPI or radio I/O. */
void cerberus_dual_wifi_diagnostics_publish(
    const cerberus_dual_wifi_diagnostic_values_t *value, int64_t now_us);
void cerberus_dual_wifi_diagnostics_get(cerberus_dual_wifi_diagnostics_t *out);
