/* Include after the UI's S3 discovery and companion stores are declared.
 * The console never accesses either store. This publisher does no SPI. */
#pragma once
#include "dual_ble_diagnostics.h"

static void dual_ble_publish_diagnostics(uint64_t now, bool asleep)
{
    cerberus_dual_ble_diagnostics_t out = {0};
    cerberus_dual_ble_view_t view;
    dual_ble_ui_get_view(&view);
    out.requested_mode = (int8_t)dual_ble_ui_mode;
    out.requested_family = dual_ble_ui_mode == CERBERUS_DUAL_BLE_FAMILY ? dual_ble_ui_family : 0U;
    out.c5_requested_family = view.requested_family;
    out.c5_published_family = view.published_family;
    out.awake = !asleep;
    out.allocated = dual_ble_ui_store != NULL;
    out.enabled = view.enabled; out.attached = view.attached;
    out.receiving = view.receiving; out.reading = view.reading;
    out.stopping = view.stopping; out.released = dual_ble_ui_released();
    out.extended_configured = view.extended;
    out.c5_error = view.error; out.c5_sweeps = view.sweeps; out.c5_rows = view.count;
    /* Publication count/latency describe this discovery reader, never the
     * number, freshness or arrival time of over-the-air advertisements. */
    out.c5_snapshots = view.snapshots; out.c5_live_updates = view.live_updates;
    out.c5_first_result_ms = view.first_result_ms; out.c5_has_first_result = view.has_first_result;
    out.c5_inventory_known = view.inventory_known;
    out.c5_inventory_replacement_enabled = view.inventory_replacement_enabled;
    out.c5_inventory_replaced = view.inventory_replaced;
    out.c5_inventory_dropped = view.inventory_dropped;
    if (dual_ble_ui_store) {
        out.c5_phase = dual_ble_ui_store->phase;
        out.c5_state = dual_ble_ui_store->remote.state;
        out.c5_generation = dual_ble_ui_store->remote.generation;
        out.c5_read_index = dual_ble_ui_store->read_index;
    }
    out.s3_initialized = cerberus_s3_ble_host_initialized();
    out.s3_synced = cerberus_s3_ble_host_synced();
    out.s3_owner = (uint8_t)cerberus_s3_ble_host_owner();
    /* Reuse the UI owner's latest scanner observation. Explicit cached label
     * avoids presenting this as a new controller/RF read. */
    out.s3_scan_cached = ble_scan_checked;
    if (out.s3_scan_cached) {
        out.s3_state = ble_scan_status.state;
        out.s3_scan_error = ble_scan_error ? ble_scan_error : ble_scan_status.error_code;
    }
    bool discovery_scope = ble_discovery_ordinary_page() || ble_discovery_monitor_page() ||
        (ble_discovery_live && !cerberus_ble_discovery_session_released(&ble_discovery_live->session));
    if (discovery_scope) {
        /* A generic scanner's cached status may belong to a different owner.
         * Keep the discovery error after release without inventing a current
         * RF observation or attributing that other owner's state to this run. */
        out.s3_scan_cached = false;
        out.s3_state = 0U;
        out.s3_scan_error = ble_discovery_open_error;
    }
    if (ble_discovery_live) {
        const cerberus_ble_discovery_session_t *session = &ble_discovery_live->session;
        out.s3_discovery_enabled = session->enabled;
        out.s3_discovery_attached = !cerberus_ble_discovery_session_released(session);
        out.s3_discovery_owned = out.s3_discovery_attached && session->have_observation && session->observation.owned;
        if (discovery_scope && session->error) out.s3_scan_error = session->error;
        if (discovery_scope && out.s3_discovery_attached && session->have_observation) {
            out.s3_scan_cached = true; out.s3_state = session->observation.state;
            out.s3_scan_error = session->error ? session->error : session->observation.error;
        }
    }
    if (current_page == UI_PAGE_BLE && ble_mode == UI_BLE_MODE_ANALYZER) {
        /* Analyzer attachment and source errors outlive a generic scanner
         * observation. Never report another owner's cached scan as this run. */
        out.s3_scan_error = ble_analyzer_s3_error();
        out.s3_scan_cached = ble_analyzer_s3_receiving();
        out.s3_state = out.s3_scan_cached ? CERBERUS_BLE_SCAN_RUNNING : 0U;
    }
    out.fox_open = current_page == UI_PAGE_BLE && ble_mode == UI_BLE_MODE_FOX_HUNT;
    out.fox_pending = out.fox_open && ble_fox_hunt_start_pending;
    out.fox_stopping = out.fox_open && ble_fox_hunt_exit_pending;
    /* Keep shared startup/cleanup failures visible in the existing Fox
     * aggregate; per-receiver errors retain their separate fields. */
    out.fox_error = out.fox_open ? (ble_fox_hunt_barrier_error != ESP_OK ?
        ble_fox_hunt_barrier_error : ble_fox_hunt_cleanup_timed_out ?
        ESP_ERR_TIMEOUT : ble_fox_hunt_error) : ESP_OK;
    if (out.fox_open) {
        /* Generic scanner telemetry may already describe a different owner.
         * This is the Fox UI's scoped observation, not a fresh RF sample. */
        out.s3_scan_cached = ble_fox_hunt_s3_attached && ble_fox_hunt_s3_owned &&
            ble_fox_hunt_s3_receiving_cached && !out.fox_pending && !out.fox_stopping;
        out.s3_state = out.s3_scan_cached ? CERBERUS_BLE_SCAN_RUNNING : 0U;
        out.s3_scan_error = out.fox_error;
    }
    bool bound_target = out.fox_open && !out.fox_pending;
    out.s3_track_error = out.fox_open ? ble_fox_hunt_error : ESP_OK;
    if (bound_target && ble_fox_hunt_s3_attached) {
        cerberus_s3_ble_track_status_t track = {0};
        cerberus_s3_ble_track_state_t state = {0};
        out.s3_track_error = ble_fox_hunt_read_s3(&track, &state);
        out.s3_scan_cached = out.s3_track_error == ESP_OK && state.owned &&
            state.receiving && !state.stopping && !out.fox_stopping;
        out.s3_state = out.s3_scan_cached ? CERBERUS_BLE_SCAN_RUNNING : 0U;
        if (out.s3_track_error) out.s3_scan_error = out.s3_track_error;
        out.s3.target_matches = out.s3_track_error == ESP_OK && state.scan_epoch != 0U && track.enabled &&
            track.address_type == ble_fox_hunt_target.address_type &&
            !memcmp(track.address, ble_fox_hunt_target.address, 6U);
        if (out.s3.target_matches) {
            out.s3.seen = track.seen; out.s3.fresh = track.fresh;
            out.s3.rssi = track.rssi_dbm; out.s3.reports = track.reports;
            out.s3.age_ms = track.age_ms;
        }
    }
    const cerberus_dual_ble_scan_t *store = dual_ble_ui_store;
    out.c5.target_matches = bound_target && dual_ble_ui_mode == CERBERUS_DUAL_BLE_TRACK && store &&
        store->start.mode == CERBERUS_DUAL_BLE_TRACK &&
        store->start.address_type == ble_fox_hunt_target.address_type &&
        !memcmp(store->start.address, ble_fox_hunt_target.address, 6U);
    if (out.c5.target_matches) {
        cerberus_dual_ble_track_view_t track;
        cerberus_dual_ble_track_view(store, now, &track);
        out.c5.seen = track.seen; out.c5.fresh = track.fresh;
        out.c5.rssi = track.rssi; out.c5.reports = track.reports;
        out.c5.age_ms = track.age_upper_ms;
    }
    cerberus_dual_ble_diagnostics_publish(&out, now <= INT64_MAX ? (int64_t)now : -1);
}
