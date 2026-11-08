#pragma once
#include "wifi_packet_count_scan.h"

/* Optional worker; never allocated by count-only operation. Source sessions
 * remain owned by the controller until drained() confirms all recorder access
 * has ended. Transport/context must outlive busy=false; they must not point at
 * disposable UI storage. S3 and C5 have separate, explicitly named PCAPNG files.
 * Source monotonic clocks are not converted to invented UTC timestamps. */
typedef struct {
    uint64_t session, bytes;
    uint32_t written, discarded, transport_errors;
    uint32_t received, acknowledged, queue_full, rejected;
    esp_err_t error;
    bool bound, draining, drained, loss_unknown, statistics_valid;
    char file_name[24];
} wifi_packet_count_record_source_t;
typedef struct {
    uint64_t job;
    wifi_packet_count_record_source_t source[WIFI_PACKET_COUNT_SOURCES];
    esp_err_t error;
    bool busy, ready, finishing;
    char directory[8]; /* /CERBERUS/PCOUNT/Pnnnnn */
} wifi_packet_count_record_status_t;

esp_err_t wifi_packet_count_record_start(const wifi_packet_count_transport_t *transport,
    uint64_t *job);
esp_err_t wifi_packet_count_record_start_monitor(const wifi_packet_count_transport_t *transport,
    uint64_t *job);
bool wifi_packet_count_record_status(uint64_t job, wifi_packet_count_record_status_t *out);
esp_err_t wifi_packet_count_record_bind(uint64_t job, unsigned source, uint64_t session);
/* Call only after a receiver is terminal with HELD cleared. Requests FIFO drain
 * on first call. Repeated calls are nonblocking; false prohibits source RELEASE.
 * A transport failure can finish with loss_unknown/error instead of claiming
 * every pending frame was saved. Does not renew the receive lease. */
bool wifi_packet_count_record_drained(uint64_t job, unsigned source, uint64_t session);
/* End an entire recording, after controller release (or before any source was
 * bound). Worker flushes/syncs/closes; status becomes !busy only after cleanup.
 * Calling while bound undrained sources exist does not bypass their fence. */
esp_err_t wifi_packet_count_record_finish(uint64_t job);
/* Retained result can be dismissed only after !busy. Stale jobs cannot affect a
 * newer worker. A later session of the same radio can bind after earlier drain,
 * appending to that radio's file; all counters remain cumulative. */
esp_err_t wifi_packet_count_record_release(uint64_t job);
