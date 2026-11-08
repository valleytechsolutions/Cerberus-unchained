#pragma once
#include "wifi_packet_monitor_session.h"
#include "wifi_ap_survey_protocol.h"

#include "wifi_channel_analyzer.h"
#include "wifi_fox_hunt.h"

#include "cerberus_protocol.h"
#include "csi_protocol.h"
#include "mac_monitor_protocol.h"
#include "esp_err.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t cerberus_wifi_scanner_init(void);
esp_err_t cerberus_wifi_ap_survey_exchange(uint8_t command,const void *request,size_t request_size,
    void *reply,size_t reply_size);
void cerberus_wifi_fox_hunt_info(cerberus_wifi_fox_hunt_status_t *out);
esp_err_t cerberus_wifi_fox_hunt_start(const cerberus_wifi_fox_hunt_start_t *request,
    cerberus_wifi_fox_hunt_status_t *out);
esp_err_t cerberus_wifi_fox_hunt_status(uint64_t session, cerberus_wifi_fox_hunt_status_t *out);
esp_err_t cerberus_wifi_fox_hunt_stop(uint64_t session, cerberus_wifi_fox_hunt_status_t *out);
esp_err_t cerberus_wifi_fox_hunt_signal(uint64_t session, cerberus_wifi_fox_hunt_signal_t *out);
void cerberus_wifi_channel_analyzer_info(cerberus_wifi_channel_analyzer_status_t *out);
esp_err_t cerberus_wifi_channel_analyzer_start(const cerberus_wifi_channel_analyzer_start_t *request,
    cerberus_wifi_channel_analyzer_status_t *out);
esp_err_t cerberus_wifi_channel_analyzer_status(uint64_t session,cerberus_wifi_channel_analyzer_status_t *out);
esp_err_t cerberus_wifi_channel_analyzer_stop(uint64_t session,cerberus_wifi_channel_analyzer_status_t *out);
esp_err_t cerberus_wifi_channel_analyzer_sample(const cerberus_wifi_channel_analyzer_read_t *request,
    cerberus_wifi_channel_analyzer_sample_t *out);
esp_err_t cerberus_wifi_channel_analyzer_hint_peek(uint64_t session,cerberus_wifi_channel_analyzer_hint_status_t *out);
esp_err_t cerberus_wifi_channel_analyzer_hint_read(const cerberus_wifi_channel_analyzer_hint_request_t *request,
    cerberus_wifi_channel_analyzer_hint_part_t *out);
esp_err_t cerberus_wifi_packet_count_exchange(uint8_t command,const void *request,size_t request_size,void *reply,size_t reply_size);
void cerberus_wifi_probe_sniff_info(cerberus_probe_sniff_control_t *out);
esp_err_t cerberus_wifi_probe_sniff_start(const cerberus_probe_sniff_start_t *request,cerberus_probe_sniff_control_t *out);
esp_err_t cerberus_wifi_probe_sniff_status(uint64_t session,cerberus_probe_sniff_control_t *out);
esp_err_t cerberus_wifi_probe_sniff_stop(uint64_t session,cerberus_probe_sniff_control_t *out);
esp_err_t cerberus_wifi_probe_sniff_release(uint64_t session,cerberus_probe_sniff_control_t *out);
esp_err_t cerberus_wifi_probe_sniff_read(const cerberus_probe_sniff_read_t *request,cerberus_probe_sniff_chunk_t *out);
esp_err_t cerberus_wifi_mac_start(const cerberus_mac_start_request_t *,cerberus_mac_status_t *);
esp_err_t cerberus_wifi_mac_get_info(cerberus_mac_info_t *);
esp_err_t cerberus_wifi_mac_get_status(const cerberus_mac_binding_t *,cerberus_mac_status_t *);
esp_err_t cerberus_wifi_mac_get_counters(const cerberus_mac_binding_t *,cerberus_mac_counters_t *);
esp_err_t cerberus_wifi_mac_get_profile(const cerberus_mac_binding_t *,cerberus_mac_profile_t *);
esp_err_t cerberus_wifi_mac_stop(const cerberus_mac_binding_t *,cerberus_mac_status_t *);
esp_err_t cerberus_wifi_mac_renew(const cerberus_mac_renew_request_t *,cerberus_mac_status_t *);
esp_err_t cerberus_wifi_mac_peek(const cerberus_mac_binding_t *,cerberus_mac_peek_t *);
esp_err_t cerberus_wifi_mac_read(const cerberus_mac_read_request_t *,cerberus_mac_chunk_t *);
esp_err_t cerberus_wifi_mac_ack(const cerberus_mac_ack_request_t *,cerberus_mac_status_t *);
esp_err_t cerberus_wifi_csi_start(const cerberus_csi_start_request_t *, cerberus_csi_status_t *);
esp_err_t cerberus_wifi_csi_get_info(cerberus_csi_info_t *);
esp_err_t cerberus_wifi_csi_get_target(const cerberus_csi_binding_t *, cerberus_csi_target_payload_t *);
esp_err_t cerberus_wifi_csi_get_status(const cerberus_csi_binding_t *, cerberus_csi_status_t *);
esp_err_t cerberus_wifi_csi_get_clock(const cerberus_csi_binding_t *, cerberus_csi_clock_t *);
esp_err_t cerberus_wifi_csi_get_counters(const cerberus_csi_binding_t *, cerberus_csi_counters_t *);
esp_err_t cerberus_wifi_csi_stop(const cerberus_csi_binding_t *, cerberus_csi_status_t *);
esp_err_t cerberus_wifi_csi_renew(const cerberus_csi_renew_request_t *, cerberus_csi_status_t *);
esp_err_t cerberus_wifi_csi_peek(const cerberus_csi_binding_t *, cerberus_csi_peek_t *);
esp_err_t cerberus_wifi_csi_read(const cerberus_csi_read_request_t *, cerberus_csi_chunk_t *);
esp_err_t cerberus_wifi_csi_ack(const cerberus_csi_ack_request_t *, cerberus_csi_status_t *);
esp_err_t cerberus_wifi_capture_start(const cerberus_wifi_capture_start_request_t *request);
esp_err_t cerberus_wifi_capture_stop(uint32_t generation);
void cerberus_wifi_capture_status(cerberus_wifi_capture_status_payload_t *status);
esp_err_t cerberus_wifi_capture_plan_stage(const cerberus_wifi_capture_plan_request_t *,
    cerberus_wifi_capture_plan_status_payload_t *);
esp_err_t cerberus_wifi_capture_plan_target(const cerberus_wifi_capture_target_request_t *,
    cerberus_wifi_capture_plan_status_payload_t *);
esp_err_t cerberus_wifi_capture_hop_status(uint32_t generation,
    cerberus_wifi_capture_hop_status_payload_t *);
esp_err_t cerberus_wifi_ap_target_valid(const cerberus_wifi_ap_job_t *request);
esp_err_t cerberus_wifi_ap_target_ssid(const cerberus_wifi_ap_job_t *request,uint8_t ssid[32],uint8_t *length);
bool cerberus_wifi_station_running(void);
esp_err_t cerberus_wifi_station_start(const cerberus_wifi_station_request_t *request);
esp_err_t cerberus_wifi_station_stop(uint16_t job_id);
void cerberus_wifi_station_status(uint16_t lease_job_id,cerberus_wifi_station_status_t *status);
esp_err_t cerberus_wifi_station_result(const cerberus_wifi_station_query_t *query,cerberus_wifi_station_result_t *result);
esp_err_t cerberus_wifi_scanner_start(void);
esp_err_t cerberus_wifi_scanner_start_band(const cerberus_wifi_scan_band_request_t *request,
    cerberus_wifi_scan_band_response_t *response);
esp_err_t cerberus_wifi_scanner_band_info(cerberus_wifi_scan_band_response_t *response);
esp_err_t cerberus_wifi_scanner_band_status(const cerberus_wifi_scan_band_request_t *request,
    cerberus_wifi_scan_band_response_t *response);
esp_err_t cerberus_wifi_scanner_band_stop(const cerberus_wifi_scan_band_request_t *request,
    cerberus_wifi_scan_band_response_t *response);
esp_err_t cerberus_wifi_probe_get_status(uint8_t generation, cerberus_wifi_probe_status_t *out);
esp_err_t cerberus_wifi_probe_get_result_chunk(const cerberus_wifi_probe_query_t *query,
    cerberus_wifi_probe_chunk_t *out);
esp_err_t cerberus_wifi_relation_get_status(uint8_t generation, cerberus_wifi_relation_status_t *out);
esp_err_t cerberus_wifi_relation_get_result_chunk(const cerberus_wifi_relation_query_t *query,
    cerberus_wifi_relation_chunk_t *out);
esp_err_t cerberus_wifi_relation_get_source_clock(const cerberus_wifi_relation_clock_query_t *query,
    cerberus_wifi_relation_clock_t *out);
void cerberus_wifi_remote_id_start(const cerberus_remote_id_start_request_t *request,
    cerberus_remote_id_status_payload_t *status);
void cerberus_wifi_remote_id_status(uint64_t job_id, cerberus_remote_id_status_payload_t *status);
void cerberus_wifi_remote_id_stop(uint64_t job_id, cerberus_remote_id_status_payload_t *status);
void cerberus_wifi_remote_id_renew(uint64_t job_id, cerberus_remote_id_status_payload_t *status);
esp_err_t cerberus_wifi_watch_profile_begin(const cerberus_watch_profile_begin_request_t *,
    cerberus_watch_profile_begin_payload_t *);
esp_err_t cerberus_wifi_watch_profile_read(const cerberus_watch_profile_read_request_t *,
    cerberus_watch_profile_read_payload_t *);
esp_err_t cerberus_wifi_watch_get_timing(const cerberus_watch_scan_timing_request_t *request,
    cerberus_watch_scan_timing_payload_t *out);
esp_err_t cerberus_wifi_watch_get_motion(const cerberus_watch_scan_data_request_t *request,
    cerberus_watch_scan_motion_payload_t *out);
esp_err_t cerberus_wifi_watch_get_motion_timing(const cerberus_watch_scan_data_request_t *request,
    cerberus_watch_scan_timing_payload_t *out);
esp_err_t cerberus_wifi_scanner_stop(void);
esp_err_t cerberus_wifi_channel_survey_start(uint8_t band);
/* Legacy wire control cannot cancel a scoped held survey. Trusted native
 * radio arbitration continues to use preempt(). */
esp_err_t cerberus_wifi_channel_survey_legacy_stop(void);
void cerberus_wifi_channel_survey_session_info(cerberus_wifi_channel_survey_session_status_t *out);
esp_err_t cerberus_wifi_channel_survey_session_start(const cerberus_wifi_channel_survey_session_start_t *request,
    cerberus_wifi_channel_survey_session_status_t *out);
esp_err_t cerberus_wifi_channel_survey_session_stop(uint64_t session_id,
    cerberus_wifi_channel_survey_session_status_t *out);
esp_err_t cerberus_wifi_channel_survey_session_status(uint64_t session_id,
    cerberus_wifi_channel_survey_session_status_t *out);
esp_err_t cerberus_wifi_channel_survey_session_read(const cerberus_wifi_channel_survey_session_read_t *request,
    cerberus_wifi_channel_survey_session_chunk_t *out);
/* Explicit background handoff: waits at most one second for the worker, then
 * retries a retained failed disable. Failure never releases radio exclusion.
 * This trusted native arbiter is deliberately separate from scoped wire Stop. */
esp_err_t cerberus_wifi_channel_survey_preempt(void);
void cerberus_wifi_channel_survey_get_status(
    cerberus_wifi_channel_survey_status_payload_t *status);
esp_err_t cerberus_wifi_channel_survey_get_result(uint8_t generation, uint8_t index,
    cerberus_wifi_channel_survey_result_payload_t *result);
esp_err_t cerberus_wifi_packet_monitor_start(uint8_t channel);
esp_err_t cerberus_wifi_packet_monitor_stop(uint8_t generation);
bool cerberus_wifi_packet_monitor_active(void);
void cerberus_wifi_packet_monitor_get_status(
    cerberus_wifi_packet_monitor_status_payload_t *status);
esp_err_t cerberus_wifi_packet_monitor_get_sample(uint8_t generation,
    uint16_t sequence, cerberus_wifi_packet_monitor_sample_payload_t *sample);
void cerberus_wifi_packet_monitor_session_info(cerberus_wifi_packet_monitor_session_status_t *out);
esp_err_t cerberus_wifi_packet_monitor_record_start(
    const cerberus_wifi_packet_monitor_session_start_t *request,
    cerberus_wifi_packet_monitor_session_status_t *out);
esp_err_t cerberus_wifi_packet_monitor_record_exchange(uint8_t command,
    const void *request, size_t request_size, void *reply, size_t reply_size);
esp_err_t cerberus_wifi_packet_monitor_session_start(
    const cerberus_wifi_packet_monitor_session_start_t *request,
    cerberus_wifi_packet_monitor_session_status_t *out);
esp_err_t cerberus_wifi_packet_monitor_session_status(uint64_t session,
    cerberus_wifi_packet_monitor_session_status_t *out);
esp_err_t cerberus_wifi_packet_monitor_session_stop(uint64_t session,
    cerberus_wifi_packet_monitor_session_status_t *out);
esp_err_t cerberus_wifi_packet_monitor_session_sample(
    const cerberus_wifi_packet_monitor_session_read_t *request,
    cerberus_wifi_packet_monitor_session_sample_t *out);
esp_err_t cerberus_wifi_watch_get_status(uint8_t generation, cerberus_watch_scan_status_payload_t *out);
esp_err_t cerberus_wifi_watch_get_data(const cerberus_watch_scan_data_request_t *request,
    cerberus_watch_scan_data_payload_t *out);
void cerberus_wifi_scanner_get_status(cerberus_wifi_scan_status_payload_t *status);
void cerberus_wifi_scanner_get_radio_stats(cerberus_wifi_radio_stats_payload_t *stats);
esp_err_t cerberus_wifi_defense_survey_start(void);
esp_err_t cerberus_wifi_defense_survey_stop(void);
bool cerberus_wifi_defense_survey_is_running(void);
void cerberus_wifi_defense_survey_get_status(cerberus_wifi_defense_status_payload_t *status);
esp_err_t cerberus_wifi_defense_survey_get_result(
    uint8_t generation,
    uint8_t index,
    cerberus_wifi_defense_result_payload_t *result);
esp_err_t cerberus_wifi_scanner_get_result(
    uint8_t generation,
    uint8_t index,
    cerberus_wifi_scan_result_payload_t *result);
esp_err_t cerberus_wifi_scanner_get_ssid_chunk(
    uint8_t generation,
    uint8_t index,
    uint8_t chunk,
    cerberus_wifi_scan_ssid_payload_t *result);
esp_err_t cerberus_wifi_connection_begin(const cerberus_wifi_connect_begin_request_t *request);
esp_err_t cerberus_wifi_connection_password(const cerberus_wifi_connect_password_request_t *request);
esp_err_t cerberus_wifi_connection_commit(void);
esp_err_t cerberus_wifi_connection_disconnect(void);
void cerberus_wifi_connection_get_status(cerberus_wifi_connection_status_payload_t *status);
uint32_t cerberus_wifi_connection_snapshot(cerberus_wifi_connection_status_payload_t *status);
void cerberus_wifi_connection_get_dns_status(cerberus_wifi_dns_status_payload_t *status);

esp_err_t cerberus_wifi_ap_target_metadata(const cerberus_wifi_ap_job_t *request,cerberus_wifi_ap_metadata_t *out);
#ifdef __cplusplus
}
#endif

bool cerberus_wifi_wardrive_ready(void);
