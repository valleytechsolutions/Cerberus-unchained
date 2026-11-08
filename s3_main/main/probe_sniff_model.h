#pragma once

#include "probe_sniff_core.h"

#define CERBERUS_PROBE_SNIFF_REPORT_MAX_BYTES 8192U

/* UI-task/worker-owned immutable sampled or stopped evidence. Live rows have
 * independent copy times; only a fully fenced stopped bank is coherent. */
typedef struct {
    cerberus_probe_sniff_control_t control;
    cerberus_probe_sniff_summary_t summary;
    cerberus_probe_sniff_projection_t rows[CERBERUS_PROBE_SNIFF_CAPACITY];
    uint8_t count, requested_channel;
    bool frozen;
} cerberus_probe_sniff_snapshot_t;

typedef enum {
    CERBERUS_PROBE_SNIFF_PAIRS,
    CERBERUS_PROBE_SNIFF_SENDERS,
    CERBERUS_PROBE_SNIFF_NETWORKS,
} cerberus_probe_sniff_group_t;

typedef struct {
    uint64_t first_callback_us, last_callback_us;
    uint32_t frames, retry_frames;
    uint8_t representative, latest, pairs;
    bool saturated;
} cerberus_probe_sniff_group_view_t;

bool cerberus_probe_sniff_snapshot_valid(const cerberus_probe_sniff_snapshot_t *snapshot);
/* Stable first-appearance references, never a duplicate result bank. Invalid
 * arguments fail before index writes; buffers must not overlap the snapshot. */
uint8_t cerberus_probe_sniff_order(const cerberus_probe_sniff_snapshot_t *snapshot,
    cerberus_probe_sniff_group_t group, uint8_t indexes[CERBERUS_PROBE_SNIFF_CAPACITY]);
bool cerberus_probe_sniff_group_view(const cerberus_probe_sniff_snapshot_t *snapshot,
    cerberus_probe_sniff_group_t group, uint8_t representative,
    cerberus_probe_sniff_group_view_t *out);
/* Presentation only; exact length/bytes remain in the immutable source row. */
void cerberus_probe_sniff_name(const cerberus_probe_sniff_row_t *row,
    char *text, size_t capacity);

/* Only fully fenced stopped evidence can be saved. NULL/0 output queries its
 * exact encoded length; insufficient capacity or invalid input changes no
 * output bytes. Output and source must not overlap. Names are hex, never raw
 * CSV controls; all captured MACs are retained. No filesystem/clock/radio. */
bool cerberus_probe_sniff_report(const cerberus_probe_sniff_snapshot_t *snapshot,
    char *output, size_t capacity, size_t *length);
