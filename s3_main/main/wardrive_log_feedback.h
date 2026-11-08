#pragma once
#include "wardrive_service.h"
#include <stddef.h>

/* Explain the completed session, using its frozen counters rather than the
 * receiver's later live fix. A running scan is not evidence of a saved CSV. */
static inline const char *cerberus_wardrive_unsaved_reason(const cerberus_wardrive_status_t *s)
{
    if ((s->state != CERBERUS_WARDRIVE_STOPPED && s->state != CERBERUS_WARDRIVE_ERROR) ||
        s->files_saved || s->error_code != ESP_OK) return NULL;
    if (!s->gps_requested) return "GPS WAS OFF / SCAN ONLY";
    if (s->skipped_no_fix) return "NO VALID GPS FIX + UTC DURING CAPTURE";
    if (s->skipped_time) return "OBSERVATION TIMES COULD NOT MATCH GPS";
    if (s->skipped_format) return "NO OBSERVATIONS IN SUPPORTED CSV FORMAT";
    return "NO ELIGIBLE OBSERVATIONS RECEIVED";
}
