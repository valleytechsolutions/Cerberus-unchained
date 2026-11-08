#pragma once
#include "wardrive_log_storage.h"
#include "wardrive_upload_reply.h"
#include "wardrive_upload_net.h"
typedef enum { CERBERUS_LOG_INSPECT, CERBERUS_LOG_DELETE, CERBERUS_LOG_UPLOAD,
    CERBERUS_LOG_IMPORT_PROFILE, CERBERUS_LOG_RETRY_SERVICE } cerberus_log_action_t;
typedef struct {
    uint64_t session;
    uint32_t catalog_revision;
    unsigned count, processed, total, accepted[3], queued[3], skipped[3];
    uint32_t sent, bytes;
    bool active, busy, closing;
    bool profile_saved;
    cerberus_log_action_t action;
    unsigned service; /* 0 = none, otherwise service enum + 1. */
    cerberus_upload_transfer_t transfer; /* Current attempt only; never response contents. */
    esp_err_t error;
    char stage[40], name[CERBERUS_WARD_LOG_NAME_CAPACITY];
    cerberus_ward_log_summary_t summary;
    cerberus_upload_reply_t destination[3];
    /* True only after every part has a confirmed receipt for this file/service.
     * Presentation state only; persistent receipt format is unchanged. */
    bool destination_complete[3];
} cerberus_log_manager_status_t;
esp_err_t cerberus_log_manager_open(void);
void cerberus_log_manager_close(void);
void cerberus_log_manager_cancel(void);
void cerberus_log_manager_status(cerberus_log_manager_status_t *out);
bool cerberus_log_manager_row(unsigned index,cerberus_ward_log_entry_t *out);
/* index=-1 means all finalized files from the reviewed catalog, or the one
 * fixed SD profile for IMPORT_PROFILE. The caller must obtain confirmation
 * before DELETE/UPLOAD/IMPORT_PROFILE and pass the exact session and catalog
 * revision shown by that confirmation. */
esp_err_t cerberus_log_manager_request(uint64_t session,uint32_t revision,
    cerberus_log_action_t action,int index);
/* Explicitly confirmed retry for one reviewed file and uncertain service only.
 * Other services and acknowledged parts are not resent. This appends to the
 * receipt journal; it does not erase receipt history or the original CSV. */
esp_err_t cerberus_log_manager_retry_service(uint64_t session,uint32_t revision,
    int index,cerberus_upload_service_t service);
