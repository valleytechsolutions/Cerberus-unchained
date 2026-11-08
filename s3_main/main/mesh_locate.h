#pragma once
#include "mesh_scan.h"
#include "mesh_locate_protocol.h"

#define MESH_LOCATE_HISTORY 32U
#define MESH_LOCATE_POLL_MS 250U
#define MESH_LOCATE_RETRY_MS 700U
#define MESH_LOCATE_KEEPALIVE_MS 1000U

typedef struct {
    uint64_t callback_us;
    uint32_t sequence;
    int8_t rssi;
    uint8_t lqi, flags, reserved;
} mesh_locate_sample_t;

enum { MESH_LOCATE_IDLE, MESH_LOCATE_INFO, MESH_LOCATE_START, MESH_LOCATE_POLL,
    MESH_LOCATE_PEEK, MESH_LOCATE_READ, MESH_LOCATE_FINAL_STATUS, MESH_LOCATE_RELEASE };

/* Small caller-owned state, independent of the preserved survey inventory.
 * target/origin_session are immutable selection intent, not device identity.
 * session is the accepted acquisition identity, retained after exact release.
 * snapshot/history contain only raw-source-validated observations. Stop freezes
 * only after terminal projection + STATUS + successful RELEASE; Close abandons.
 * History is a ring; use history_get for oldest-to-newest value copies. */
typedef struct {
    mesh_observation_address_key_t target;
    uint64_t origin_session, session;
    cerberus_mesh_locate_start_t request;
    cerberus_mesh_locate_control_t remote, pinned_control;
    cerberus_mesh_locate_projection_t snapshot;
    mesh_locate_sample_t history[MESH_LOCATE_HISTORY];
    uint8_t scratch[CERBERUS_MESH_LOCATE_PROJECTION_BYTES];
    uint64_t snapshot_anchor_us, snapshot_age_us, pending_anchor_us, last_clock_us;
    uint64_t last_status_ms, next_action_ms, last_tick_ms;
    uint32_t pin, samples, coalesced_matches, last_sequence, pinned_sequence_floor;
    esp_err_t error, fault_error;
    uint8_t phase, part, wire_size, history_count, history_head;
    bool requested, stop_requested, close_requested, start_attempted;
    bool accepted, have_control, has_snapshot, faulted, final_valid, frozen;
    bool anchor_valid, pending_anchor_valid, clock_seen, clock_failed;
} mesh_locate_t;

void mesh_locate_init(mesh_locate_t *locate);
/* Explicit new intent only; requires released. Failure leaves state unchanged.
 * Successful Begin clears previous Locate evidence; survey data is separate. */
esp_err_t mesh_locate_begin(mesh_locate_t *locate,
    const mesh_observation_address_key_t *target, uint64_t origin_session);
void mesh_locate_stop(mesh_locate_t *locate);
void mesh_locate_close(mesh_locate_t *locate);
bool mesh_locate_released(const mesh_locate_t *locate);
void mesh_locate_tick(mesh_locate_t *locate, uint64_t now_ms,
    const mesh_scan_transport_t *transport);
/* Upper callback age in microseconds. No clock read; caller supplies current S3
 * monotonic time. Never treats delivery as source time. False clears output. */
bool mesh_locate_age_upper(const mesh_locate_t *locate, int64_t now_us, uint64_t *age_us);
bool mesh_locate_history_get(const mesh_locate_t *locate, uint8_t index,
    mesh_locate_sample_t *sample);
bool mesh_locate_control_valid(const cerberus_mesh_locate_control_t *control,
    uint8_t command, uint64_t requested_session);
/* Revalidates the complete released, terminal value for immutable report use. */
bool mesh_locate_frozen_valid(const mesh_locate_t *locate);
