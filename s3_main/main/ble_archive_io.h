#pragma once

#include "ble_archive.h"
#include "airtag_json.h"
#include "wifi_archive.h"
#include "wifi_marauder.h"
#include "probe_sniff_model.h"
#include "mesh_report.h"
#include "mesh_locate_report.h"
#include "mesh_archive.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#define CERBERUS_BLE_ARCHIVE_IO_PATH_CAPACITY 512U
#define CERBERUS_BLE_ARCHIVE_IO_NAME_CAPACITY 256U

typedef enum {
    CERBERUS_BLE_ARCHIVE_IO_IDLE = 0,
    CERBERUS_BLE_ARCHIVE_IO_STARTING,
    CERBERUS_BLE_ARCHIVE_IO_SAVING,
    CERBERUS_BLE_ARCHIVE_IO_LOADING,
    CERBERUS_BLE_ARCHIVE_IO_COMPLETE,
    CERBERUS_BLE_ARCHIVE_IO_ERROR,
    CERBERUS_BLE_ARCHIVE_IO_CANCELED,
} cerberus_ble_archive_io_state_t;
typedef enum {
    CERBERUS_BLE_ARCHIVE_IO_SAVE = 0,
    CERBERUS_BLE_ARCHIVE_IO_LOAD,
    CERBERUS_BLE_ARCHIVE_IO_LOAD_AIRTAG_JSON,
    CERBERUS_BLE_ARCHIVE_IO_SAVE_AIRTAG_JSON,
    CERBERUS_BLE_ARCHIVE_IO_EXPORT_AIRTAG_JSON,
    CERBERUS_BLE_ARCHIVE_IO_SAVE_WIFI,
    CERBERUS_BLE_ARCHIVE_IO_LOAD_WIFI,
    CERBERUS_BLE_ARCHIVE_IO_LOAD_AP_JSON,
    CERBERUS_BLE_ARCHIVE_IO_SAVE_AP_JSON,
    CERBERUS_BLE_ARCHIVE_IO_EXPORT_AP_JSON,
    CERBERUS_BLE_ARCHIVE_IO_LOAD_SSID_TEXT,
    CERBERUS_BLE_ARCHIVE_IO_SAVE_SSID_TEXT,
    CERBERUS_BLE_ARCHIVE_IO_EXPORT_SSID_TEXT,
    CERBERUS_BLE_ARCHIVE_IO_SAVE_PROBE,
    CERBERUS_BLE_ARCHIVE_IO_SAVE_MESH,
    CERBERUS_BLE_ARCHIVE_IO_SAVE_MESH_LOCATE,
    CERBERUS_BLE_ARCHIVE_IO_SAVE_MESH_ARCHIVE,
    CERBERUS_BLE_ARCHIVE_IO_LOAD_MESH_ARCHIVE,
    CERBERUS_BLE_ARCHIVE_IO_EXPORT_MESH_ARCHIVE_CSV,
} cerberus_ble_archive_io_operation_t;

typedef enum {
    CERBERUS_BLE_ARCHIVE_IO_FORMAT_BLE = 0,
    CERBERUS_BLE_ARCHIVE_IO_FORMAT_AIRTAG_JSON,
    CERBERUS_BLE_ARCHIVE_IO_FORMAT_WIFI,
    CERBERUS_BLE_ARCHIVE_IO_FORMAT_AP_JSON,
    CERBERUS_BLE_ARCHIVE_IO_FORMAT_SSID_TEXT,
    CERBERUS_BLE_ARCHIVE_IO_FORMAT_PROBE_REPORT,
    CERBERUS_BLE_ARCHIVE_IO_FORMAT_MESH_REPORT,
    CERBERUS_BLE_ARCHIVE_IO_FORMAT_MESH_LOCATE_REPORT,
    CERBERUS_BLE_ARCHIVE_IO_FORMAT_MESH_ARCHIVE,
} cerberus_ble_archive_io_format_t;
typedef struct {
    cerberus_ble_archive_io_state_t state;
    cerberus_ble_archive_io_operation_t operation;
    cerberus_ble_archive_io_format_t format;
    union { /* Only the stats belonging to format are meaningful. */
        cerberus_airtag_json_export_stats_t export_stats;
        cerberus_wifi_marauder_export_stats_t wifi_export_stats;
    };
    cerberus_wifi_marauder_result_t wifi_result; /* AP/SSID codec outcome only;
        ESP error still reports storage/allocation failures independently. */
    esp_err_t error;
    uint64_t job_id;
    bool worker_active, cancel_requested, result_ready;
    uint32_t bytes; /* Complete encoded/read bytes, not partial-write progress. */
    uint8_t count;
    char relative_path[CERBERUS_BLE_ARCHIVE_IO_PATH_CAPACITY];
    char file_name[CERBERUS_BLE_ARCHIVE_IO_NAME_CAPACITY];
} cerberus_ble_archive_io_status_t;

/* UI-facing calls do no filesystem or radio work. One job owns an immutable
 * PSRAM document and codec buffer; there is no internal-RAM fallback. Job IDs
 * never repeat within a boot. Release the previous job before starting another.
 * Save copies the complete document before returning; callers retain ownership. */
esp_err_t cerberus_ble_archive_io_save(const cerberus_ble_archive_t *document,
    uint64_t *job_id);
esp_err_t cerberus_ble_archive_io_load(const char *relative_path,
    const char *file_name, uint64_t *job_id);
/* JSON imports are a separate historical model with unknown source/type/time.
 * Export copies the native document before returning, then the same worker
 * validates/converts each eligible packet separately. No UI parsing or radio
 * lookup occurs. Conversion stats count packets, not physical devices.
 * All successful JSON operations publish only a typed JSON result. */
esp_err_t cerberus_ble_archive_io_load_airtag_json(const char *relative_path,
    const char *file_name, uint64_t *job_id);
esp_err_t cerberus_ble_archive_io_save_airtag_json(const cerberus_airtag_json_t *document,
    uint64_t *job_id);
esp_err_t cerberus_ble_archive_io_export_airtag_json(const cerberus_ble_archive_t *document,
    uint64_t *job_id);
esp_err_t cerberus_ble_archive_io_copy_airtag_json(uint64_t job_id,
    cerberus_airtag_json_t *document);
/* Native Wi-Fi observations use their own immutable model and exact-sized
 * PSRAM arm in the same serialized worker. Save writes a new .WFI file under
 * /CERBERUS/WIFI; it does not alter live scans, targets or saved trust rules. */
esp_err_t cerberus_ble_archive_io_save_wifi(const cerberus_wifi_archive_t *document,
    uint64_t *job_id);
esp_err_t cerberus_ble_archive_io_load_wifi(const char *relative_path,
    const char *file_name, uint64_t *job_id);
esp_err_t cerberus_ble_archive_io_copy_wifi(uint64_t job_id,
    cerberus_wifi_archive_t *document);
/* Explicit offline interchange: no type guessed from a filename or payload.
 * Export freezes a native WFI document before return, then converts in the
 * worker. AP JSON/SSID text never become live APs, targets or trust rules.
 * Save Copy keeps order/duplicates/presence/framing in the imported model. */
esp_err_t cerberus_ble_archive_io_load_ap_json(const char *relative_path,
    const char *file_name, uint64_t *job_id);
esp_err_t cerberus_ble_archive_io_save_ap_json(const cerberus_wifi_marauder_ap_t *document,
    uint64_t *job_id);
esp_err_t cerberus_ble_archive_io_export_ap_json(const cerberus_wifi_archive_t *document,
    uint64_t *job_id);
esp_err_t cerberus_ble_archive_io_copy_ap_json(uint64_t job_id,
    cerberus_wifi_marauder_ap_t *document);
esp_err_t cerberus_ble_archive_io_load_ssid_text(const char *relative_path,
    const char *file_name, uint64_t *job_id);
esp_err_t cerberus_ble_archive_io_save_ssid_text(const cerberus_wifi_marauder_ssid_t *document,
    uint64_t *job_id);
esp_err_t cerberus_ble_archive_io_export_ssid_text(const cerberus_wifi_archive_t *document,
    uint64_t *job_id);
esp_err_t cerberus_ble_archive_io_copy_ssid_text(uint64_t job_id,
    cerberus_wifi_marauder_ssid_t *document);
/* Save a fully fenced stopped Probe Sniff snapshot as a new CSV report under
 * /CERBERUS/LOGS. The snapshot is validated and copied before returning, then
 * formatted and written by this same worker. No load/copy-result API or radio
 * action is implied; status reports the exact job's file outcome. */
esp_err_t cerberus_ble_archive_io_save_probe(const cerberus_probe_sniff_snapshot_t *snapshot,
    uint64_t *job_id);
/* A stopped/drained/released Mesh observation report. Copies the frozen model
 * and final C5 summary before returning, then encodes a new CSV under LOGS on
 * the same serialized worker. CSV is historical evidence, not a live scan or
 * an authoritative topology. No load or native-map reconstruction is implied. */
esp_err_t cerberus_ble_archive_io_save_mesh(const mesh_scan_t *scan, uint64_t *job_id);
/* Source-specific Locate report, using a separate schema and exact-sized PSRAM
 * arm. Saves a new Mxxxxx.CSV in LOGS through the same serialized storage worker.
 * The frozen source/history copy completes before returning to the UI. */
esp_err_t cerberus_ble_archive_io_save_mesh_locate(const mesh_locate_t *locate, uint64_t *job_id);
/* Historical native maps have no live controller or radio ownership. Save and
 * Load publish FORMAT_MESH_ARCHIVE and support the matching immutable copy.
 * Export copies the historical document before return, then writes the existing
 * Mesh CSV schema under LOGS with FORMAT_MESH_REPORT; it has no typed-map result.
 * Native Save chooses a new Mxxxxx.MSH under /CERBERUS/MESH. */
esp_err_t cerberus_ble_archive_io_save_mesh_archive(const mesh_archive_t *document, uint64_t *job_id);
esp_err_t cerberus_ble_archive_io_load_mesh_archive(const char *relative_path,
    const char *file_name, uint64_t *job_id);
esp_err_t cerberus_ble_archive_io_copy_mesh_archive(uint64_t job_id, mesh_archive_t *document);
esp_err_t cerberus_ble_archive_io_export_mesh_archive_csv(const mesh_archive_t *document, uint64_t *job_id);
void cerberus_ble_archive_io_get_status(cerberus_ble_archive_io_status_t *status);
/* Copies only a fully validated completed native BLE SAVE or LOAD. JSON jobs
 * require copy_airtag_json and Wi-Fi jobs require their matching typed copy. Getters reject
 * other formats without changing output. The immutable result is pinned and
 * copied outside the critical section; a concurrent Release prevents new readers
 * and defers disposal until accepted copies return. Historical data never becomes
 * a live scan or active target. */
esp_err_t cerberus_ble_archive_io_copy_result(uint64_t job_id,
    cerberus_ble_archive_t *document);
/* Cancel before storage skips it. An in-flight filesystem call is not
 * preempted: a SAVE reports its actual COMPLETE/ERROR result and may publish;
 * a canceled LOAD discards its result. Failed saves retain their .TMP name.
 * Release during any active phase defers disposal until worker cleanup. These
 * operations are byte-bounded, not guaranteed filesystem-latency bounds. */
void cerberus_ble_archive_io_cancel(uint64_t job_id);
void cerberus_ble_archive_io_release(uint64_t job_id);
const char *cerberus_ble_archive_io_state_name(cerberus_ble_archive_io_state_t state);
