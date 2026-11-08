#pragma once
#include "wardrive_upload_config.h"
#define CERBERUS_UPLOAD_JSON_MAX_BYTES 4096U
/* Exact versioned schema, no unknown/duplicate keys. No partial profile is
 * returned on failure. Input and output must not overlap. Never logs values. */
bool cerberus_upload_config_json(const void *bytes,size_t length,cerberus_upload_config_t *out);
