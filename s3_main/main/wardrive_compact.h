#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    uint32_t input_rows, devices, output_rows;
    uint64_t input_bytes, output_bytes;
} cerberus_wardrive_compact_stats_t;

/* Run after RF stops. Scratch files contain only sorted row references;
 * the source CSV is read-only throughout. The caller owns open/close/sync,
 * publication and cleanup. All files must be distinct and positioned at 0.
 * Scratch files and output must be empty, seekable binary update streams. */
size_t cerberus_wardrive_compact_workspace_size(void);
esp_err_t cerberus_wardrive_compact(FILE *source, FILE *scratch_a, FILE *scratch_b,
    FILE *output, void *workspace, size_t capacity,
    void (*yield)(void), cerberus_wardrive_compact_stats_t *stats);
