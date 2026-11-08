#pragma once

#include "cerberus_protocol.h"
#include "mesh_observation_protocol.h"
#include "mesh_locate_protocol.h"
#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t cerberus_ieee802154_scanner_init(void);
esp_err_t cerberus_ieee802154_scanner_start(void);
bool cerberus_ieee802154_scanner_is_running(void);
void cerberus_ieee802154_scanner_get_status(
    cerberus_ieee802154_energy_scan_status_payload_t *status);
esp_err_t cerberus_ieee802154_scanner_get_result(
    uint8_t generation,
    cerberus_ieee802154_energy_scan_result_payload_t *result);
esp_err_t cerberus_ieee802154_beacon_scanner_start(void);
void cerberus_ieee802154_beacon_scanner_get_status(
    cerberus_ieee802154_beacon_scan_status_payload_t *status);
esp_err_t cerberus_ieee802154_beacon_scanner_get_result(
    uint8_t generation,
    uint8_t index,
    cerberus_ieee802154_beacon_scan_result_payload_t *result);
esp_err_t cerberus_ieee802154_activity_scanner_start(void);
void cerberus_ieee802154_activity_scanner_get_status(
    cerberus_ieee802154_activity_scan_status_payload_t *status);
esp_err_t cerberus_ieee802154_activity_scanner_get_result(
    uint8_t generation,
    uint8_t result_index,
    cerberus_ieee802154_activity_scan_result_payload_t *result);

/* Commands reserve/cancel/copy only; the existing worker owns all SDK cleanup.
 * is_running includes retained mesh ownership until release/retention expiry. */
void cerberus_mesh_observation_info(cerberus_mesh_observation_control_t *out);
esp_err_t cerberus_mesh_observation_start(const cerberus_mesh_observation_start_t *request,
    cerberus_mesh_observation_control_t *out);
esp_err_t cerberus_mesh_observation_status(uint64_t session,cerberus_mesh_observation_control_t *out);
esp_err_t cerberus_mesh_observation_stop(uint64_t session,cerberus_mesh_observation_control_t *out);
esp_err_t cerberus_mesh_observation_release(uint64_t session,cerberus_mesh_observation_control_t *out);
esp_err_t cerberus_mesh_observation_peek(const cerberus_mesh_observation_peek_t *request,
    cerberus_mesh_observation_control_t *out);
esp_err_t cerberus_mesh_observation_read(const cerberus_mesh_observation_read_t *request,
    cerberus_mesh_observation_chunk_t *out);
esp_err_t cerberus_mesh_observation_ack(const cerberus_mesh_observation_ack_t *request,
    cerberus_mesh_observation_control_t *out);

/* Dedicated coalesced selected-source receive mode; the same worker owns SDK
 * operations and retained cleanup. No raw FIFO or additional radio task. */
void cerberus_mesh_locate_info(cerberus_mesh_locate_control_t *out);
esp_err_t cerberus_mesh_locate_start(const cerberus_mesh_locate_start_t *request,
    cerberus_mesh_locate_control_t *out);
esp_err_t cerberus_mesh_locate_status(uint64_t session,cerberus_mesh_locate_control_t *out);
esp_err_t cerberus_mesh_locate_stop(uint64_t session,cerberus_mesh_locate_control_t *out);
esp_err_t cerberus_mesh_locate_release(uint64_t session,cerberus_mesh_locate_control_t *out);
esp_err_t cerberus_mesh_locate_peek(uint64_t session,cerberus_mesh_locate_control_t *out);
esp_err_t cerberus_mesh_locate_read(const cerberus_mesh_locate_read_t *request,
    cerberus_mesh_locate_chunk_t *out);

#ifdef __cplusplus
}
#endif
