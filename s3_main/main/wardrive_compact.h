#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    uint32_t input_rows, devices, output_rows;
    uint64_t input_bytes, output_bytes;
    uint64_t index_read_bytes, index_write_bytes; /* Successful stdio transfers, not physical SD blocks. */
    uint32_t initial_refs, final_refs, merge_passes;
} cerberus_wardrive_compact_stats_t;

/* Run after RF stops. Scratch files contain counted runs of sorted extrema;
 * the source CSV is read-only throughout. The caller owns open/close/sync,
 * publication and cleanup. All files must be distinct and positioned at 0.
 * Scratch files and output must be empty, seekable binary update streams. */
size_t cerberus_wardrive_compact_workspace_size(void);
esp_err_t cerberus_wardrive_compact(FILE *source, FILE *scratch_a, FILE *scratch_b,
    FILE *output, void *workspace, size_t capacity,
    void (*yield)(void), cerberus_wardrive_compact_stats_t *stats);
