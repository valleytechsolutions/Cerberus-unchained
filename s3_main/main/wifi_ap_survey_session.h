#pragma once

#include "wifi_ap_survey_protocol.h"

/* Pure dual-receiver coordinator. Source1 is native S3, source2 is C5.
 * The caller owns this workspace (PSRAM on target) and all transport state. */
enum { CERBERUS_AP_SURVEY_SESSION_IDLE, CERBERUS_AP_SURVEY_SESSION_INFO,
    CERBERUS_AP_SURVEY_SESSION_START, CERBERUS_AP_SURVEY_SESSION_ACTIVE };
enum { CERBERUS_AP_SURVEY_SESSION_INVALID = -2301,
    CERBERUS_AP_SURVEY_SESSION_TIMEOUT = -2302,
    CERBERUS_AP_SURVEY_SESSION_BUSY = -2303,
    CERBERUS_AP_SURVEY_SESSION_CLOCK = -2304 };

typedef struct {
    cerberus_ap_survey_start_t request;
    cerberus_ap_survey_control_t remote;
    cerberus_ap_survey_snapshot_t staging;
    uint64_t next_action_us, next_status_us, next_snapshot_us, next_renew_us;
    uint64_t read_started_us, request_started_us, receipt_us;
    uint32_t published;
    int32_t error, read_error;
    uint8_t phase, start_attempts;
    bool requested, stop_requested, close_requested, have_control;
    bool start_uncertain, reading, cancel_pending, final_attempted, unavailable;
} cerberus_ap_survey_radio_session_t;

typedef struct {
    cerberus_ap_survey_radio_session_t radios[2];
    uint64_t run_id, clock_us;
    uint8_t cursor;
    bool clock_valid;
} cerberus_ap_survey_session_t;

typedef struct {
    void *context;
    /* INFO has no request, START uses start_t, other controls request_t.
     * A canonical reply may prove state even when its error code is nonzero. */
    int32_t (*exchange)(void *, uint8_t source, uint8_t command,
        const void *request, size_t request_bytes, cerberus_ap_survey_control_t *);
    /* At most one bounded step; native S3 can complete its direct copy in one.
     * The C5 adapter retains its reader between calls and supplies the exact
     * BEGIN request anchor and final receipt on completion. */
    int32_t (*snapshot_step)(void *, uint8_t source, uint64_t session_id,
        cerberus_ap_survey_snapshot_t *, bool *complete,
        uint64_t *request_started_us, uint64_t *receipt_us);
    void (*snapshot_cancel)(void *, uint8_t source);
    bool (*publish)(void *, uint8_t source, uint64_t run_id,
        uint64_t request_started_us, uint64_t receipt_us,
        const cerberus_ap_survey_snapshot_t *);
    void (*unavailable)(void *, uint8_t source, bool unavailable);
} cerberus_ap_survey_session_ops_t;

void cerberus_ap_survey_session_init(cerberus_ap_survey_session_t *);
/* Requires exact previous release and a strictly greater nonzero local run.
 * A failed receiver does not prevent the other from continuing. */
bool cerberus_ap_survey_session_start(cerberus_ap_survey_session_t *, uint64_t run_id,
    uint64_t s3_channels, uint64_t c5_channels);
/* Stop drains a final copied result, then releases both backend workspaces.
 * Close (Back/sleep) skips final readout and retains the last published model. */
void cerberus_ap_survey_session_stop(cerberus_ap_survey_session_t *);
void cerberus_ap_survey_session_close(cerberus_ap_survey_session_t *);
bool cerberus_ap_survey_session_released(const cerberus_ap_survey_session_t *);
bool cerberus_ap_survey_session_running(const cerberus_ap_survey_session_t *);
/* Runs at most one backend operation. Cancellation itself does not touch RF.
 * Cleanup/renewal precede ordinary polling and incremental snapshot work. */
void cerberus_ap_survey_session_tick(cerberus_ap_survey_session_t *, uint64_t now_us,
    const cerberus_ap_survey_session_ops_t *);
