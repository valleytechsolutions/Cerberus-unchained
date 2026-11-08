#pragma once

/* Async SD journal for UI-consumed passive Recon rows. Start/stop/offer/status
 * never access SD or radios. Recording is an explicit UI action, not boot work.
 * An in-flight filesystem call cannot be canceled; Stop drains accepted rows
 * and keeps worker_active true until all file handles/leases are released. */
#include "esp_err.h"
#include "recon_journal.h"

typedef enum {
    CERBERUS_RECON_RECORDER_IDLE = 0,
    CERBERUS_RECON_RECORDER_STARTING,
    CERBERUS_RECON_RECORDER_RECORDING,
    CERBERUS_RECON_RECORDER_STOPPING,
    CERBERUS_RECON_RECORDER_COMPLETE,
    CERBERUS_RECON_RECORDER_ERROR,
    CERBERUS_RECON_RECORDER_CANCELED,
} cerberus_recon_recorder_state_t;

typedef struct {
    cerberus_recon_recorder_state_t state;
    esp_err_t error;
    bool worker_active;
    uint64_t session_id, started_us, ended_us;
    cerberus_recon_counters_t counters;
    uint16_t queued;
    char session_name[7]; /* Unique on this card; partial sessions keep their name. */
} cerberus_recon_recorder_status_t;

esp_err_t cerberus_recon_recorder_start(void);
void cerberus_recon_recorder_stop(void);
/* Offer copies a row; service assigns its boot-local session ID and sequence.
 * consumed_us must be current/past S3 uptime and cannot precede session start.
 * False while disabled/stopping is not an accepted observation. Full-queue and
 * invalid offers during recording are counted separately. No payload pointers
 * survive the call. CSV records are completed-sweep snapshots, not RF packets. */
bool cerberus_recon_recorder_offer(const cerberus_recon_record_t *record);
void cerberus_recon_recorder_get_status(cerberus_recon_recorder_status_t *status);
const char *cerberus_recon_recorder_state_name(cerberus_recon_recorder_state_t state);
