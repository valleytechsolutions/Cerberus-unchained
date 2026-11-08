#pragma once
#include "esp_err.h"
#include "wardrive_upload_file.h"
#include "wardrive_upload_reply.h"
typedef void (*cerberus_upload_progress_fn)(void *context, const char *stage, uint32_t sent, uint32_t total);
typedef struct {
    esp_err_t error;
    esp_err_t tls_error;
    int socket_errno, tls_code, tls_flags;
    unsigned http_status;
    uint32_t response_bytes;
    bool attempted, response_complete;
} cerberus_upload_transfer_t;
/* Only the log worker calls this module. It never persists station credentials
 * in the SDK, disables redirects, and uses the platform CA bundle/host checks. */
esp_err_t cerberus_upload_net_begin(const cerberus_upload_config_t *config,
    cerberus_upload_cancel_fn cancel, cerberus_upload_progress_fn progress, void *context);
esp_err_t cerberus_upload_net_end(void);
cerberus_upload_reply_t cerberus_upload_net_send(const cerberus_upload_config_t *config,
    cerberus_upload_service_t service, FILE *file, const cerberus_upload_file_t *plan,
    unsigned part, const char *upload_name, cerberus_upload_cancel_fn cancel,
    cerberus_upload_progress_fn progress, void *context, cerberus_upload_transfer_t *transfer);
