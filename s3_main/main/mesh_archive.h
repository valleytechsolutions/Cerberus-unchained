#pragma once
#include "mesh_report.h"

/* Historical value-only evidence. A session identifies its original capture;
 * it is never authority to query, renew, start or stop a radio. No live control
 * or request state is serialized. CRC detects damage, not authenticity. */
typedef struct {
    mesh_observation_t model;
    cerberus_mesh_observation_summary_t final_summary;
    uint64_t first_callback_us, last_callback_us;
    uint16_t observed_mask;
    bool callback_time_seen;
} mesh_archive_t;

/* Format1 stores the current mesh_mac clear-header parser contract as parser1.
 * Unknown format/parser revisions reject; no silent reinterpretation occurs. */
#define MESH_ARCHIVE_VERSION 1U
#define MESH_ARCHIVE_PARSER_VERSION 1U
#define MESH_ARCHIVE_HEADER_BYTES 32U
#define MESH_ARCHIVE_MIN_BYTES 268U
/* Header32 + capture24 + summary128 + model84 + PAN16*37 + address64*70
 * + edge64*49 + witness64*154. Fieldwise LE; no native struct padding. */
#define MESH_ARCHIVE_MAX_BYTES 18332U

bool mesh_archive_valid(const mesh_archive_t *archive);
/* Deep-copy only an exact released/frozen capture. Failure/overlap leaves the
 * destination unchanged. No allocation, radio, clock or filesystem operations. */
bool mesh_archive_from_scan(const mesh_scan_t *scan, mesh_archive_t *out);
/* NULL/0 measures exact bytes. Invalid input/short capacity changes no output.
 * Aliases with source/length reject before changing length; other failures set
 * length zero. Binary output needs no terminal NUL. */
bool mesh_archive_encode(const mesh_archive_t *archive, uint8_t *output,
                         size_t capacity, size_t *length);
/* All three memory ranges must be disjoint. Explicit caller-owned archive scratch
 * avoids a large automatic object; failure may change scratch, never out.
 * Unknown versions, noncanonical booleans/reserved/tails, trailing data, CRC
 * mismatch and invalid evidence/references reject before publication. */
bool mesh_archive_decode(const uint8_t *bytes, size_t length,
                         mesh_archive_t *out, mesh_archive_t *workspace);
/* Same historical CSV as the live report, without constructing live control
 * state. Capacity includes the terminal NUL; NULL/0 measures. */
bool mesh_archive_report_encode(const mesh_archive_t *archive, char *output,
                                size_t capacity, size_t *length);
