#pragma once

#include "wifi_packet_count_record.h"

/* UI-task scalar copies only. No target identities, payloads, pointers, clock
 * reads, allocations, producer calls or lease renewal. Session IDs describe
 * existing ownership; this interface cannot use them to control a source. */
typedef struct {
    uint64_t requested_session, accepted_session, channel_mask;
    uint64_t last_status_ms, next_action_ms, published_session, record_session;
    uint64_t last_progress_ms;
    uint32_t pin, revision_floor, published_revision;
    uint32_t callbacks, matched, invalid_metadata, raw_dropped_full, raw_dropped_other;
    uint32_t valid_frames, malformed_frames, unsupported_frames;
    uint32_t outside_window_frames, clock_dropped_frames;
    uint32_t written, discarded, transport_errors;
    int32_t error, remote_error, published_error, record_error;
    uint16_t configured, part;
    uint8_t phase, state, flags, channel, target_count, upload_index, kind, index;
    bool attempted, accepted, start_attempted, control_valid, stop, close, faulted;
    bool published_valid, published_frozen, clock_invalid, record_bound;
    bool recorder_bound, recorder_draining, recorder_drained, record_loss_unknown;
    bool progress_time_valid;
} wifi_packet_count_source_diagnostics_t;

typedef struct {
    wifi_packet_count_source_diagnostics_t source[WIFI_PACKET_COUNT_SOURCES];
    uint64_t publication_sequence, published_ms, last_tick_ms, job, channel_mask;
    uint64_t tick_gap_ms, max_tick_gap_ms;
    uint32_t segments, interruptions;
    int32_t ui_error, error, s3_admission_error, record_error;
    uint16_t dwell_ms;
    uint8_t target_count;
    bool available, allocated, time_valid, sequence_saturated;
    bool tick_gap_valid;
    bool start_pending, closing, requested, retuning, frozen, fallback, recording;
    bool record_valid, record_busy, record_ready, record_finishing;
} wifi_packet_count_diagnostics_t;

/* Exactly one UI task publishes. now_ms timestamps this copied diagnostic,
 * not reception. A backward sample retains the high-water timestamp and marks
 * time_valid=false; a later sample at/above it restores validity. Sequence
 * saturates rather than wrapping. A stale/missing recorder value is UNKNOWN,
 * even when its zero fields resemble a successful idle recording.
 * Source control fields require control_valid; published counters require
 * published_valid. Unavailable fields are cleared, never promoted to known 0.
 * Tick gaps compare consecutive allocated UI controller ticks. Progress time
 * changes for phase/upload/read/publication identity, not STATUS heartbeats.
 * A progress change observed with a backward clock has unknown timing until
 * another progress change is observed with a valid clock.
 * Caller inputs must remain stable during publish; get() is thread-safe. */
void wifi_packet_count_diagnostics_publish(const wifi_packet_count_scan_t *scan,
    const wifi_packet_count_record_status_t *record, uint64_t now_ms,
    uint64_t job, bool start_pending, bool closing, esp_err_t ui_error);
void wifi_packet_count_diagnostics_clear(uint64_t now_ms);
void wifi_packet_count_diagnostics_get(wifi_packet_count_diagnostics_t *out);
