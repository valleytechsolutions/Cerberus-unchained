#pragma once
#include "wardrive_log.h"
#include <stdio.h>
#define CERBERUS_UPLOAD_PART_BYTES (1024U*1024U)
#define CERBERUS_UPLOAD_PARTS 66U
typedef bool (*cerberus_upload_cancel_fn)(void *context);
typedef struct {
    cerberus_ward_log_summary_t summary;
    uint32_t offsets[CERBERUS_UPLOAD_PARTS+1];
    unsigned parts;
    size_t header_bytes;
    char headers[2048];
} cerberus_upload_file_t;
/* Full-file validation and immutable row-boundary plan. Repeated observations
 * survive byte-for-byte. Each part receives both original CSV headers. */
bool cerberus_upload_file_prepare(FILE *file, cerberus_upload_file_t *out,
    cerberus_upload_cancel_fn cancel, void *context);
