#pragma once
#include "esp_err.h"
#include "mesh_observation.h"
#include "mesh_observation_protocol.h"

#define MESH_SCAN_TICK_CALLS 4U
#define MESH_SCAN_TICK_BUDGET_US 8000U
#define MESH_SCAN_RETRY_MS 700U
#define MESH_SCAN_KEEPALIVE_MS 1000U
#define MESH_SCAN_EMPTY_POLL_MS 250U

typedef struct {
    esp_err_t (*exchange)(void *context, uint8_t command, const void *request,
                          size_t request_size, void *reply, size_t reply_size);
    int64_t (*clock_us)(void *context);
    void *context;
} mesh_scan_transport_t;

enum {
    MESH_SCAN_IDLE, MESH_SCAN_INFO, MESH_SCAN_START, MESH_SCAN_PEEK_ROW,
    MESH_SCAN_READ_ROW, MESH_SCAN_ACK, MESH_SCAN_PEEK_SUMMARY,
    MESH_SCAN_READ_SUMMARY, MESH_SCAN_FINAL_STATUS, MESH_SCAN_RELEASE
};

/* One caller-owned optional workspace. No allocation/task/global state.
 * model/final_summary may describe the prior run while a new Start is pending;
 * model.session is authoritative. They reset only after a canonical bound Start
 * or its exact STATUS recovery. frozen is asserted only after the stopped drain,
 * summary/final control fence, and canonical successful RELEASE response. */
typedef struct {
    mesh_observation_t model;
    cerberus_mesh_observation_summary_t final_summary;
    cerberus_mesh_observation_start_t request;
    cerberus_mesh_observation_control_t remote;
    uint8_t scratch[CERBERUS_MESH_OBSERVATION_RECORD_BYTES];
    uint64_t last_status_ms, next_action_ms, last_tick_ms;
    uint64_t first_callback_us, last_callback_us;
    uint32_t sequence, acked_sequence, last_enqueued;
    uint16_t observed_mask;
    esp_err_t error, fault_error;
    uint8_t phase, part;
    bool requested, stop_requested, close_requested, start_attempted;
    bool accepted, have_control, faulted, final_valid, frozen, callback_time_seen;
} mesh_scan_t;

void mesh_scan_init(mesh_scan_t *scan);
esp_err_t mesh_scan_begin(mesh_scan_t *scan, uint8_t mode, uint16_t channel_mask,
                          uint16_t dwell_ms);
/* Pure intent. Stop drains; Close may discard unread evidence. Neither sends
 * commands or restarts RF. Explicit Begin requires released()==true. */
void mesh_scan_stop(mesh_scan_t *scan);
void mesh_scan_close(mesh_scan_t *scan);
bool mesh_scan_released(const mesh_scan_t *scan);
/* INFO/START each charge two wire calls (capability refresh); others charge one.
 * The 8ms elapsed check prevents a next command; it is not an SDK deadline. */
void mesh_scan_tick(mesh_scan_t *scan, uint64_t now_ms,
                    const mesh_scan_transport_t *transport);
bool mesh_scan_control_valid(const cerberus_mesh_observation_control_t *control,
                              uint8_t command, uint64_t requested_session);
