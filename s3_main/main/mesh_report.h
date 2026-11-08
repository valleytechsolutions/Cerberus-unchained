#pragma once
#include "mesh_scan.h"

#define MESH_REPORT_MAX_BYTES 98304U

/* Small borrowed value view. This validates historical evidence only and never
 * grants live session authority. Owners serialize changes during validation or
 * encoding. Keeping pointers here avoids a second full model on the stack. */
typedef struct {
    const mesh_observation_t *model;
    const cerberus_mesh_observation_summary_t *summary;
    uint64_t first_callback_us, last_callback_us;
    uint16_t observed_mask;
    bool callback_time_seen;
} mesh_evidence_view_t;
bool mesh_evidence_valid(const mesh_evidence_view_t *evidence);
bool mesh_evidence_report_encode(const mesh_evidence_view_t *evidence,
                                 char *output, size_t capacity, size_t *length);

/* Immutable released/frozen capture only. No decode or map-import authority. */
bool mesh_report_valid(const mesh_scan_t *scan);
/* NULL/0 measures exact CSV bytes, excluding terminal NUL. Output capacity must
 * include that NUL. Invalid input or insufficient capacity changes no output.
 * Aliases with the source/length are rejected before changing length. Other
 * failures set length to zero. No heap, global work buffer, filesystem or radio. */
bool mesh_report_encode(const mesh_scan_t *scan, char *output, size_t capacity,
                         size_t *length);
