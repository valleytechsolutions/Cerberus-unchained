#pragma once
#include "wardrive_upload_config.h"

typedef enum { CERBERUS_UPLOAD_NEW, CERBERUS_UPLOAD_SENDING, CERBERUS_UPLOAD_QUEUED,
    CERBERUS_UPLOAD_ACCEPTED, CERBERUS_UPLOAD_REJECTED, CERBERUS_UPLOAD_UNKNOWN } cerberus_upload_result_t;
typedef struct {
    cerberus_upload_result_t result;
    uint32_t imported, rejected_rows;
    bool counts_valid;
    char transaction[81];
} cerberus_upload_reply_t;
/* Full bounded JSON only. Unknown is conservative after an ambiguous POST.
 * WiGLE upload acknowledgement is QUEUED, never an import result. */
cerberus_upload_reply_t cerberus_upload_reply(cerberus_upload_service_t service,
    unsigned http_status, const char *body, size_t length, bool complete);
/* Display confirmed service receipts as UPLOADED. Keep the stored QUEUED and
 * ACCEPTED values distinct: file receipt does not establish import/credit. */
const char *cerberus_upload_result_name(cerberus_upload_result_t result);
/* Combine part outcomes without hiding an ambiguous or rejected earlier part.
 * Initialize aggregate to ACCEPTED with zero counters. False means overflow. */
bool cerberus_upload_reply_merge(cerberus_upload_reply_t *aggregate,
    const cerberus_upload_reply_t *part);
